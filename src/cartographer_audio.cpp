#include "cartographer_audio.hpp"
#include "city_gate.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hk3ds {
void CartographerAudio::stop(){
    LightLock_Lock(&lock_);
    if(active_){ndspChnReset(channel);++generation_;}
    active_=0;starting_=false;buffers_={};
    LightLock_Unlock(&lock_);
}
void CartographerAudio::closeFile(){
    if(file_)close_(file_);
    file_=nullptr;remaining_=length_=0;
}
void CartographerAudio::release(){
    stop();quit_=true;
    if(thread_){threadJoin(thread_,U64_MAX);threadFree(thread_);thread_=nullptr;}
    closeFile();if(pcm_)linearFree(pcm_);pcm_=nullptr;
}
void CartographerAudio::pause(bool paused){
    LightLock_Lock(&lock_);
    paused_=paused;if(active_)ndspChnSetPaused(channel,paused||starting_);
    LightLock_Unlock(&lock_);
}
bool CartographerAudio::request(unsigned kind){
    if(!thread_){
        quit_=false;s32 priority=0;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
        thread_=threadCreate(entry,this,8192,priority-1,-2,false);
        if(!thread_)return false;
    }
    LightLock_Lock(&lock_);
    ndspChnReset(channel);buffers_={};active_=kind;++generation_;starting_=true;
    ndspChnSetPaused(channel,true);
    LightLock_Unlock(&lock_);
    return true;
}
bool CartographerAudio::openFile(const char* path){
    closeFile();
    file_=open_(path,"rb");
    if(!file_)return false;
    char riff[12];
    if(read_(riff,1,12,file_)!=12||std::memcmp(riff,"RIFF",4)||std::memcmp(riff+8,"WAVE",4)){closeFile();return false;}
    bool format=false;
    for(unsigned chunk=0;chunk<32;++chunk){
        char name[4];std::uint32_t size=0;
        if(read_(name,1,4,file_)!=4||read_(&size,4,1,file_)!=1||size>16*1024*1024){closeFile();return false;}
        if(!std::memcmp(name,"fmt ",4)){
            std::array<unsigned char,16> values{};
            if(size<16||read_(values.data(),1,16,file_)!=16){closeFile();return false;}
            format=values[0]==1&&values[1]==0&&values[2]==1&&values[3]==0&&values[14]==16&&values[15]==0;
            std::memcpy(&rate_,values.data()+4,4);
            seek_(file_,long(size-16+(size&1)),SEEK_CUR);
        }else if(!std::memcmp(name,"data",4)&&format){
            length_=remaining_=size/2;start_=std::ftell(file_);break;
        }else seek_(file_,long(size+(size&1)),SEEK_CUR);
    }
    if(!remaining_||rate_<8000||rate_>48000){closeFile();return false;}
    if(!pcm_)pcm_=static_cast<std::int16_t*>(linearAlloc(samples*2*2));
    if(!pcm_){closeFile();return false;}
    return true;
}
void CartographerAudio::update(const Cartographer& npc,Vec2 listener,unsigned volume){
    if(!npc.present){stop();serial_=npc.voiceSerial;return;}
    if(serial_!=npc.voiceSerial){serial_=npc.voiceSerial;request(2);}
    else if(npc.humming()&&active_!=1)request(1);
    else if(npc.locked()&&active_==1)stop();
    if(!active_)return;
    const float distance=std::hypot(listener.x-npc.position.x,listener.y-npc.position.y);
    // Approximation: bounded linear falloff until the source AudioSource curve is exported.
    const float gain=(active_==2?.8f:std::clamp(1-distance/35.f,0.f,1.f)*.65f)*std::min(volume,10u)*.1f;
    mix(gain);
}
void CartographerAudio::updateGate(const CityGate& gate,unsigned volume){
    if(!gate.active){stop();serial_=0;return;}
    if(serial_!=gate.soundSerial){
        serial_=gate.soundSerial;request(10+gate.sound);
    }
    if(active_)mix(std::min(volume,10u)*.1f);
}
void CartographerAudio::mix(float gain){
    float mix[12]{};mix[0]=mix[1]=gain;
    LightLock_Lock(&lock_);ndspChnSetMix(channel,mix);LightLock_Unlock(&lock_);
}
void CartographerAudio::service(){
    LightLock_Lock(&lock_);
    const auto generation=generation_,kind=active_;
    LightLock_Unlock(&lock_);
    if(serviced_!=generation){
        serviced_=generation;next_=0;
        if(!kind){closeFile();return;}
        char path[128];
        if(kind<10)std::snprintf(path,sizeof(path),"romfs:/cornifer-%s.wav",kind==1?"hum":"voice");
        else std::snprintf(path,sizeof(path),"romfs:/scenes/Fungus2_21/city-gate-%u.wav",kind-10);
        // RomFS can wait behind an SD save. Keep file I/O outside the gameplay lock.
        if(!openFile(path))return;
        LightLock_Lock(&lock_);
        if(generation_==generation){
            ndspChnSetInterp(channel,NDSP_INTERP_POLYPHASE);ndspChnSetFormat(channel,NDSP_FORMAT_MONO_PCM16);
            ndspChnSetRate(channel,float(rate_));
        }
        LightLock_Unlock(&lock_);
    }
    if(!kind||!file_)return;
    for(unsigned filled=0;filled<buffers_.size();++filled){
        LightLock_Lock(&lock_);
        const auto status=buffers_[next_].status;
        const bool refill=generation_==generation&&status!=NDSP_WBUF_QUEUED&&status!=NDSP_WBUF_PLAYING;
        LightLock_Unlock(&lock_);
        if(!refill)break;
        if(!remaining_&&kind==1){seek_(file_,start_,SEEK_SET);remaining_=length_;}
        if(!remaining_)break;
        const unsigned count=std::min(samples,remaining_);
        const unsigned read=read_(staging_.data(),2,count,file_);
        remaining_-=count;if(!read){remaining_=0;break;}
        LightLock_Lock(&lock_);
        if(generation_!=generation){LightLock_Unlock(&lock_);return;}
        auto& buffer=buffers_[next_];buffer={};buffer.data_vaddr=pcm_+next_*samples;buffer.nsamples=read;
        std::memcpy(pcm_+next_*samples,staging_.data(),read*2);
        DSP_FlushDataCache(buffer.data_vaddr,read*2);ndspChnWaveBufAdd(channel,&buffer);
        // Match the old streamer's first-buffer start: a slow second read must
        // not hold already queued audio paused (and need not touch pause at idle).
        if(starting_){starting_=false;ndspChnSetPaused(channel,paused_);}
        next_=(next_+1)%buffers_.size();
        LightLock_Unlock(&lock_);
    }
}
void CartographerAudio::run(){
    while(!quit_){service();svcSleepThread(1000000);}
    closeFile();
}
}
