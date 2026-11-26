#include "audio_mixer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
namespace {
unsigned u32(const unsigned char* p){return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24);}
}
void AudioClip::load(const std::string& filename,bool streaming){
    const auto slash=filename.find_last_of("/\\");
    const auto name=filename.substr(slash==std::string::npos?0:slash+1);
    const auto pack=filename.substr(0,slash==std::string::npos?0:slash+1)+"audio.pak";
    FILE* f=std::fopen(pack.c_str(),"rb");
    unsigned base=0,size=0;bool ok=true;
    if(f){
        // HPK1 v1: bounded stack directory scan at admission only. Every HPA1
        // blob starts on a 2 KiB boundary; streaming retains no directory heap.
        path=pack;unsigned char header[16]{};
        ok=std::fread(header,1,16,f)==16&&std::memcmp(header,"HPK1",4)==0&&u32(header+4)==1;
        const unsigned count=u32(header+8),total=u32(header+12);
        ok=ok&&count>0&&count<=audioClipCount&&total<0x80000000u&&
            std::fseek(f,0,SEEK_END)==0&&std::ftell(f)==long(total)&&std::fseek(f,16,SEEK_SET)==0;
        unsigned end=(16+count*64+2047)/2048*2048;
        for(unsigned i=0;ok&&i<count;++i){
            unsigned char record[64]{};ok=std::fread(record,1,64,f)==64;
            const unsigned offset=u32(record+56),bytes=u32(record+60);
            ok=ok&&record[0]&&std::memchr(record,0,56)&&offset==end&&
                offset<=total&&bytes>=audioPcmOffset&&bytes%audioSectorBytes==0&&bytes<=total-offset;
            if(ok&&name==reinterpret_cast<const char*>(record)){
                if(size)ok=false; // Duplicate logical clip is never ambiguous.
                base=offset;size=bytes;
            }
            if(ok)end=(offset+bytes+2047)/2048*2048;
        }
        ok=ok&&end==total&&size&&std::fseek(f,long(base),SEEK_SET)==0;
    }else{
        // Loose HPA1 remains supported for standalone tools and synthetic banks.
        path=filename;f=std::fopen(filename.c_str(),"rb");
        if(!f)throw std::runtime_error("Audio open: "+filename);
        ok=std::fseek(f,0,SEEK_END)==0;
        const long length=std::ftell(f);
        ok=ok&&length>=0&&length==long(unsigned(length)&0x7fffffffu);size=ok?unsigned(length):0;
        ok=ok&&std::fseek(f,0,SEEK_SET)==0;
    }
    pcmOffset=base+audioPcmOffset;
    unsigned char h[32]{};
    ok=ok&&std::fread(h,1,32,f)==32;
    channels=u32(h+12);frames=u32(h+16);stream=streaming;
    std::memcpy(&gain,h+24,4);std::memcpy(&pitch,h+28,4);
    ok=ok&&std::memcmp(h,"HPA1",4)==0&&u32(h+4)==2&&u32(h+8)==audioRate&&
        // Original Crossroads music is 153.6 s; stream rings stay fixed-size.
        (channels==1||channels==2)&&frames>0&&frames<=audioRate*(streaming?180:120)&&u32(h+20)==unsigned(streaming)&&
        (!streaming||channels==2)&&std::isfinite(gain)&&gain>=0&&gain<=4&&std::isfinite(pitch)&&pitch>0&&pitch<=4;
    if(ok){
        const long end=long(audioPcmOffset)+long(frames)*long(channels)*2;
        ok=size==unsigned((end+audioSectorBytes-1)/audioSectorBytes*audioSectorBytes)&&
            std::fseek(f,long(pcmOffset),SEEK_SET)==0;
    }
    if(ok&&!streaming){
        const unsigned resident=frames>audioLongFrames?audioHeadFrames+1:frames;
        pcm.resize(resident*channels);ok=std::fread(pcm.data(),2,pcm.size(),f)==pcm.size();
    }
    std::fclose(f);
    if(!ok){pcm.clear();throw std::runtime_error("Invalid HPA1: "+filename);}
}
void AudioMixer::apply(const AudioState& state){
    for(unsigned i=0;i<audioVoiceCount;++i){
        if(state[i].generation!=state_[i].generation||state[i].clip!=state_[i].clip){
            position_[i]=0;streamRestart_[i]=true;
        }
    }
    state_=state;
}
void AudioMixer::render(std::int16_t* stereo,unsigned frames,AudioStream* const* streams,AudioTail* const* tails){
    if(frames>2048)throw std::runtime_error("Audio block too large");
    // Bound each tail-cache working set at the maximum admitted 8x pitch.
    if(frames>512){
        for(unsigned offset=0;offset<frames;offset+=512)
            render(stereo+offset*2,std::min(512u,frames-offset),streams,tails);
        return;
    }
    std::array<std::int64_t,4096> sum;std::fill_n(sum.data(),frames*2,0);
    std::array<std::int16_t,4096> streamPcm;
    for(unsigned v=0;v<audioVoiceCount;++v){const auto& s=state_[v];
        if(s.clip>=audioClipCount)continue;
        const auto& c=clips_[s.clip];if(!c.frames)continue;
        if(c.stream){
            if(!streams||!streams[s.clip])throw std::runtime_error("Missing stream reader");
            if(streamRestart_[v]){streams[s.clip]->restart();streamRestart_[v]=false;}
            const auto n=streams[s.clip]->read(streamPcm.data(),frames);
            for(unsigned i=0;i<n;++i){sum[i*2]+=std::int64_t(streamPcm[i*2])*s.left;sum[i*2+1]+=std::int64_t(streamPcm[i*2+1])*s.right;}
            continue;
        }
        const std::uint64_t end=std::uint64_t(c.frames)<<16;
        if(s.loop)position_[v]%=end;
        AudioTail* tail=nullptr;
        if(c.tail()){
            if(!tails||!tails[v])throw std::runtime_error("Missing voice tail reader");
            tail=tails[v];tail->prepare(s.clip,unsigned(position_[v]>>16));
        }
        // Spatially silent loops still keep phase, without interpolating PCM.
        if(!s.left&&!s.right){position_[v]+=std::uint64_t(frames)*s.step;
            if(s.loop)position_[v]%=end;
            continue;
        }
        // Most authored loops use unity pitch. Integer positions need no
        // interpolation or 64-bit position arithmetic in their sample loop.
        if(!tail&&s.step==65536&&!(position_[v]&65535)){
            unsigned frame=unsigned(position_[v]>>16);
            for(unsigned i=0;i<frames;++i){
                if(frame>=c.frames){if(!s.loop)break;frame=0;}
                sum[i*2]+=std::int64_t(c.pcm[frame*c.channels])*s.left;
                sum[i*2+1]+=std::int64_t(c.pcm[frame*c.channels+c.channels-1])*s.right;
                ++frame;
            }
            position_[v]=std::uint64_t(frame)<<16;continue;
        }
        std::array<const std::int16_t*,audioTailPages> pages{};
        std::array<unsigned,audioTailPages> tags;tags.fill(~0u);
        const auto point=[&](unsigned frame)->const std::int16_t*{
            if(!tail||frame<=audioHeadFrames)return c.pcm.data()+frame*c.channels;
            const unsigned page=(frame-audioHeadFrames)/audioTailPageFrames,slot=page%audioTailPages;
            if(tags[slot]!=page){tags[slot]=page;pages[slot]=tail->page(page);}
            return pages[slot]?pages[slot]+((frame-audioHeadFrames)%audioTailPageFrames)*c.channels:nullptr;
        };
        if(tail&&s.step==65536&&!(position_[v]&65535)){
            unsigned frame=unsigned(position_[v]>>16);
            for(unsigned i=0;i<frames;++i){
                if(frame>=c.frames){if(!s.loop)break;frame=0;}
                if(const auto p=point(frame)){
                    sum[i*2]+=std::int64_t(p[0])*s.left;
                    sum[i*2+1]+=std::int64_t(p[c.channels-1])*s.right;
                }
                ++frame;
            }
            position_[v]=std::uint64_t(frame)<<16;continue;
        }
        for(unsigned i=0;i<frames;++i){auto& pos=position_[v];
            if(pos>=end){if(!s.loop)break;pos%=end;}
            const unsigned a=unsigned(pos>>16),fraction=unsigned(pos&65535),b=a+1<c.frames?a+1:s.loop?0:a;
            const auto x=point(a),y=fraction?point(b):x;
            const auto sample=[&](unsigned channel){
                const int first=x?x[channel]:0,second=y?y[channel]:0;
                return std::int64_t(first)+(std::int64_t(second-first)*fraction)/65536;
            };
            sum[i*2]+=sample(0)*s.left;sum[i*2+1]+=sample(c.channels-1)*s.right;pos+=s.step;
        }
    }
    for(unsigned i=0;i<frames*2;++i){const auto value=sum[i]/65536;
        if(value>32767||value< -32768)++clipped;
        const int out=int(std::max<std::int64_t>(-32768,std::min<std::int64_t>(32767,value)));
        peak=std::max(peak,unsigned(out<0?-out:out));stereo[i]=std::int16_t(out);
    }
}
}
