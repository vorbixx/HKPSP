#include "wind_audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cerrno>

namespace hk3ds {
SceneAudioId sceneAudioId(const char* scene)
{
    if(!scene)return SceneAudioId::Silent;
    if(std::strcmp(scene,"Tutorial_01")==0)return SceneAudioId::Tutorial;
    if(std::strcmp(scene,"Town")==0)return SceneAudioId::Dirtmouth;
    if(std::strcmp(scene,"Crossroads_ShamanTemple")==0)return SceneAudioId::ShamanTemple;
    if(std::strncmp(scene,"Crossroads_",11)==0)return SceneAudioId::Crossroads;
    // Admitted from these rooms' saved SceneManager musicCue/snapshot metadata.
    // Other Fungus1 rooms may have different cues; do not infer them by prefix.
    static const char* greenpath[]={"Fungus1_01","Fungus1_02","Fungus1_17","Fungus1_31",
        "Fungus1_32","Fungus1_21","Fungus1_04","Fungus1_06"};
    for(const auto* room:greenpath)if(std::strcmp(scene,room)==0)return SceneAudioId::Greenpath;
    if(std::strcmp(scene,"Fungus1_01b")==0||std::strcmp(scene,"Fungus1_03")==0)return SceneAudioId::GreenpathSub;
    return SceneAudioId::Silent;
}
bool sceneAudioKeepsPhase(SceneAudioId from,SceneAudioId to)
{
    if(from>=SceneAudioId::Count||to>=SceneAudioId::Count)return false;
    // AudioManager.ApplyMusicCue changes playback only for a different cue;
    // BeginApplyAtmosCue calls Play only when that source is not playing.
    // Cave resolves sharedassets6:1054 / resources:3852 in both rooms.
    // Greenpath Main/Sub share music sharedassets36:27 and atmos sharedassets128:174.
    if(from==to)return true;
    const auto cave=[](SceneAudioId id){return id==SceneAudioId::Tutorial||id==SceneAudioId::ShamanTemple;};
    const auto green=[](SceneAudioId id){return id==SceneAudioId::Greenpath||id==SceneAudioId::GreenpathSub;};
    return (cave(from)&&cave(to))||(green(from)&&green(to));
}
const SceneAudioProfile& sceneAudioProfile(SceneAudioId id)
{
    static const SceneAudioProfile profiles[]={
        // level6:12820 fileID 2 -> sharedassets6:1054 Cave, snapshot :3852.
        // Same source admission as Temple below; quiet rain remains omitted.
        {{{"wind-cave_wind_loop.bin","wind-cave_noises.bin",nullptr}},WindCue::TutorialMixerGain,WindCue::TutorialMask,0},
        // Surface cue s5:14; at Surface groups 3838/3843: 0/-2.2889833 dB.
        {{{"wind-dirtmouth_wind_loop_a.bin","wind-dirtmouth_wind_loop_c_gate.bin","music-dirtmouth.bin"}},{{1.f,.768335f,1.f}},7,4},
        // Crossroads cue s19:86, Normal snapshot: Main/Sub at 0 dB.
        // Approximation: omit Action/Tension stems at -60 dB and cave's
        // two surface stems at -80 dB to retain the existing three channels.
        {{{"wind-cave_wind_loop.bin","music-crossroads-bass.bin","music-crossroads-main.bin"}},{{1.f,1.f,1.f}},7,6},
        {{{nullptr,nullptr,nullptr}},{{0,0,0}},0,0},
        // MusicCue sharedassets36:27, Main/Sub -> clips 14/13. Normal snapshot
        // resources:3855 has both music groups at 0 dB. AtmosCue sharedassets128:174
        // enables source 7 -> resources:1292, native 48 kHz. at Greenpath :3864
        // sets its mixer group :3820 to 0 dB; source volume/pitch are both 1.
        {{{"wind-green_path_atmos_loop.bin","music-greenpath-bass.bin","music-greenpath-main.bin"}},{{1.f,1.f,1.f}},7,6},
        // Sub Area resources:3865, Main -59.606762 dB / Sub +0.025578512 dB.
        {{{"wind-green_path_atmos_loop.bin","music-greenpath-bass.bin","music-greenpath-main.bin"}},{{1.f,.0010463136f,1.0029492f}},7,6},
        // level76 -> sharedassets6:1054 Cave, at Cave resources:3852.
        // Sources 0/4 use cave_wind_loop/cave_noises at 0 dB, volume/pitch 1.
        // Approximation: omit rain sources 5/6 at -60.40684 dB and retained
        // music under Silent :3896 (-60 dB); no new music cue is authored here.
        {{{"wind-cave_wind_loop.bin","wind-cave_noises.bin",nullptr}},{{1.f,1.f,0.f}},3,0},
        {{{"wind-cave_wind_loop.bin","music-false-knight.bin",nullptr}},{{1.f,1.f,0.f}},3,2}
    };
    const auto index=static_cast<unsigned>(id);
    static_assert(sizeof(profiles)/sizeof(*profiles)==static_cast<unsigned>(SceneAudioId::Count),"scene audio profiles");
    return profiles[index<static_cast<unsigned>(SceneAudioId::Count)?index:static_cast<unsigned>(SceneAudioId::Silent)];
}
bool WindPcmStream::openScene(const char* directory,SceneAudioId id,unsigned slot)
{
    close();const auto& p=sceneAudioProfile(id);
    if(slot>=p.files.size()||!p.files[slot])return false;
    char path[512];const int count=std::snprintf(path,sizeof(path),"%s/%s",directory,p.files[slot]);
    // Native format belongs to the clip, including when another room reuses it.
    const bool native48=std::strcmp(p.files[slot],"wind-green_path_atmos_loop.bin")==0||
                        std::strcmp(p.files[slot],"wind-cave_noises.bin")==0;
    const unsigned expectedRate=native48?48000:44100;
    if(count>0&&static_cast<unsigned>(count)<sizeof(path)&&open(path)&&sampleRate_==expectedRate)return true;
    close();return false;
}
void WindCue::apply(unsigned mask)
{
    for(unsigned i=0;i<ChannelCount;++i){
        auto& c=channels_[i];
        const bool enabled=(mask&(1u<<i))!=0;
        if(enabled&&!c.playing){c={};c.playing=true;}
        c.start=c.gain;c.target=enabled?1.0f:0.0f;c.elapsed=0;
    }
}
void WindCue::advance(float seconds)
{
    if(!std::isfinite(seconds)||seconds<=0)return;
    for(auto& c:channels_){
        if(!c.playing)continue;
        c.elapsed=std::min(TransitionSeconds,c.elapsed+seconds);
        // Approximation: linear transition in amplitude. The backend applies the
        // serialized at Cave group gains; enabled loops always keep phase.
        c.gain=c.start+(c.target-c.start)*(c.elapsed/TransitionSeconds);
        if(c.elapsed>=TransitionSeconds&&c.target==0)c.playing=false;
    }
}
void WindCue::scene(unsigned epoch,bool active,SceneAudioId id)
{
    const auto mask=sceneAudioProfile(id).mask;
    if(!observed_||!sceneAudioKeepsPhase(id_,id)){reset();observed_=true;active_=active;apply(active?mask:0);}
    else if(active_!=active){active_=active;apply(active?mask:0);}
    epoch_=epoch;id_=id;
}
void WindCue::reset(){channels_={};observed_=active_=false;}

namespace {
unsigned le32(const unsigned char* p){return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24);}
bool retryable(int error){return error==0||error==EINTR||error==EAGAIN||error==EIO;}
}
bool WindPcmStream::open(const char* path)
{
    close();file_=opener_(path,"rb");if(!file_)return false;
    unsigned char h[16]{};
    bool ok=std::fread(h,1,sizeof(h),file_)==sizeof(h)&&std::memcmp(h,"HKAW",4)==0&&le32(h+8)==2;
    sampleRate_=le32(h+4);
    // Admit only the native rates of the authored area clips. NDSP consumes
    // each stream at its own rate; buffer sizes and worker ownership stay fixed.
    ok=ok&&(sampleRate_==44100||sampleRate_==48000);
    frames_=le32(h+12);ok=ok&&frames_>0&&frames_<0x10000000u;
    if(ok)ok=std::fseek(file_,0,SEEK_END)==0&&std::ftell(file_)==16L+static_cast<long>(frames_)*4;
    if(!ok||!rewind()){close();return false;}return true;
}
bool WindPcmStream::cachePrefix(void* storage,std::size_t bytes)
{
    if(prefix_||!storage||!bytes||bytes%4||bytes>static_cast<std::size_t>(frames_)*4)return false;
    if(!rewind()||!read(storage,bytes))return false;
    prefix_=static_cast<const unsigned char*>(storage);prefixFrames_=bytes/4;
    return rewind();
}
bool WindPcmStream::rewind()
{
    // Cached scene starts must submit without RomFS I/O. A terminal I/O fault
    // still requires a successful physical seek before recovery is admitted.
    if((!prefix_||faulted_)&&!seek(0))return false;
    position_=wraps_=0;faulted_=false;return true;
}
bool WindPcmStream::openWave(const char* path)
{
    close();file_=opener_(path,"rb");if(!file_)return false;
    unsigned char h[44]{};
    bool ok=std::fread(h,1,sizeof(h),file_)==sizeof(h)&&
        std::memcmp(h,"RIFF",4)==0&&std::memcmp(h+8,"WAVEfmt ",8)==0&&
        le32(h+16)==16&&le32(h+20)==0x00020001&&le32(h+24)==44100&&
        le32(h+28)==176400&&le32(h+32)==0x00100004&&std::memcmp(h+36,"data",4)==0;
    const auto bytes=le32(h+40);
    ok=ok&&bytes>0&&bytes<4000000&&bytes%4==0&&le32(h+4)==bytes+36;
    frames_=bytes/4;dataOffset_=44;sampleRate_=44100;
    if(ok)ok=std::fseek(file_,0,SEEK_END)==0&&std::ftell(file_)==44L+static_cast<long>(bytes);
    if(!ok||!rewind()){close();return false;}return true;
}
bool WindPcmStream::readOnce(void* destination,std::size_t capacity,unsigned& frames)
{
    frames=0;
    if(!file_||faulted_||!destination||capacity%4||!capacity)return false;
    const auto bytes=std::min(capacity,static_cast<std::size_t>(remainingFrames())*4);
    if(!read(destination,bytes))return false;
    frames=bytes/4;return true;
}
bool WindPcmStream::seek(unsigned frame)
{
    if(!file_)return false;
    for(unsigned attempt=0;attempt<IoAttempts;++attempt){
        std::clearerr(file_);errno=0;
        if(seeker_(file_,dataOffset_+static_cast<long>(frame)*4,SEEK_SET)==0){
            if(attempt)++recoveries_;
            return true;
        }
        fail(Seek,frame,0,0);if(!retryable(failure_.error))break;
    }
    faulted_=true;return false;
}
bool WindPcmStream::read(void* destination,std::size_t bytes)
{
    if(!file_||faulted_||!destination||bytes%4)return false;
    auto* out=static_cast<unsigned char*>(destination);
    while(bytes){
        if(position_==frames_){if(!prefix_&&!seek(0))return false;position_=0;++wraps_;}
        if(position_<prefixFrames_){
            const auto cached=std::min(bytes,static_cast<std::size_t>(prefixFrames_-position_)*4);
            std::memcpy(out,prefix_+position_*4,cached);
            position_+=cached/4;bytes-=cached;out+=cached;continue;
        }
        // Cached reads leave FILE at its old cursor, including after retry/wrap.
        if(prefix_&&position_==prefixFrames_&&!seek(position_))return false;
        const auto count=std::min(bytes,static_cast<std::size_t>(frames_-position_)*4);
        bool complete=false;
        for(unsigned attempt=0;attempt<IoAttempts;++attempt){
            // A short fread may have consumed even a partial stereo frame and
            // latched EOF/error. Retry the entire segment at its committed phase.
            if(attempt&&!seek(position_))return false;
            errno=0;const auto received=reader_(out,1,count,file_);
            if(received==count){if(attempt)++recoveries_;complete=true;break;}
            fail(Read,position_,count,received);if(!retryable(failure_.error))break;
        }
        if(!complete){faulted_=true;return false;}
        position_+=count/4;bytes-=count;out+=count;
    }
    return true;
}
void WindPcmStream::fail(Operation operation,unsigned frame,unsigned requested,unsigned received)
{
    ++ioFailures_;failure_={unsigned(operation),dataOffset_+frame*4,requested,received,errno,
              file_?unsigned(std::ferror(file_)!=0)|(unsigned(std::feof(file_)!=0)<<1):0};
}
void WindPcmStream::close()
{
    if(file_)std::fclose(file_);
    prefix_=nullptr;prefixFrames_=0;
    file_=nullptr;frames_=position_=wraps_=ioFailures_=recoveries_=sampleRate_=0;dataOffset_=16;failure_={};faulted_=false;
}
}
