#pragma once
#include "wind_audio.hpp"
#include "render_memory.hpp"
#include "audio_channels.hpp"
#include <3ds.h>
#include <atomic>
#include <memory>

namespace hk3ds {
// Dedicated wind channels 7..9; hero slash variants share channels 5/6.
class WindAudioPlayer {
public:
    WindAudioPlayer(){LightLock_Init(&oneShotLock_);}
    static constexpr unsigned FirstChannel=windFirstChannel, BufferCount=3;
    bool load(const char* directory);
    void scene(unsigned epoch,bool active,SceneAudioId id=SceneAudioId::Tutorial)
    {command_.store(sceneAudioCommand(epoch,active,id));}
    void release();
    void volumes(unsigned music,unsigned sound){volumes_.store((music<<8)|sound);}
    enum class OneShot { Opening, Collapse, Secret, KnightArmour, KnightHead, KnightJump, KnightSlam, KnightStagger, KnightRage, KnightDeath, KnightSwing, KnightLand, HeroDeathDetails, Count };
    static constexpr unsigned streamSlot(OneShot clip){return clip==OneShot::Secret||clip==OneShot::KnightStagger||clip==OneShot::KnightRage||clip==OneShot::KnightDeath?1:0;}
    bool startOneShot(unsigned channel,OneShot clip);
    void stopOneShot(unsigned slot);
    unsigned oneShotStatus(unsigned slot)const{return oneShots_[slot].status.load();}
    unsigned oneShotFrames(unsigned slot)const{return oneShots_[slot].frames.load();}
    bool ready()const{return thread_&&error_.load()==0;}
    int error()const{return error_.load();}
private:
    std::atomic<unsigned> volumes_{0x0a0a};
    struct Stream {
        WindPcmStream pcm;
        std::array<ndspWaveBuf,BufferCount> buffers{};
        unsigned next=0;
        unsigned reportedFailures=0,reportedRecoveries=0;
        bool active=false;
    };
    std::array<Stream,WindCue::ChannelCount> streams_{};
    char sceneDirectory_[256]{};
    std::array<WindPcmStream,static_cast<unsigned>(OneShot::Count)> oneShotPcm_{};
    std::unique_ptr<unsigned char[]> oneShotStartCache_;
    bool selectScene(SceneAudioId id);
    struct OneShotStream {
        std::array<ndspWaveBuf,BufferCount> buffers{};
        unsigned channel=ndspChannelCount,next=0,generation=0;
        OneShot clip=OneShot::Opening;
        bool starting=false;
        std::atomic<unsigned> status{NDSP_WBUF_FREE},frames{0};
    };
    std::array<OneShotStream,2> oneShots_{};
    LightLock oneShotLock_;
    bool acceptingOneShots_=false;
    WindCue cue_;
    std::unique_ptr<unsigned char[]> startCache_;
    void* memory_=nullptr;
    Thread thread_=nullptr;
    std::atomic<bool> quit_{false};
    std::atomic<unsigned> command_{0};
    std::atomic<int> error_{0};
    static void entry(void* self){static_cast<WindAudioPlayer*>(self)->run();}
    void run();
    bool fill(unsigned channel,unsigned index);
    void stop(unsigned channel);
    void publishIo(unsigned channel);
    bool fillOneShot(unsigned slot,unsigned index,unsigned generation,WindPcmStream& pcm);
    void stopOneShotLocked(unsigned slot);
    void cancelOneShots();
    void serviceOneShot();
    friend struct WindAudioPlayerTest;
};
struct WindAudioProbe {
    std::uint32_t sequence=0,version=4,ready=0;
    std::int32_t error=0;
    std::uint32_t bufferBytes=0,activeMask=0,underruns=0,resets=0;
    std::uint32_t submitted[3]{},playing[3]{},wraps[3]{};
    std::uint32_t serviceMicros=0,maxServiceMicros=0,elapsedMillis=0,stopped=0;
    std::uint32_t failureStream=0,failureOperation=0,failurePosition=0,failureRequested=0,failureReceived=0;
    std::int32_t failureErrno=0;
    std::uint32_t failureFlags=0,ioFailures=0,recoveries=0,sequenceEnd=0;
};
struct OpeningStreamProbe {
    std::uint32_t bufferBytes=0,plays=0,submittedFrames=0,underruns=0,completed=0;
    std::int32_t error=0;
};
}
extern "C" { extern volatile hk3ds::WindAudioProbe hk3ds_wind_audio; }
extern "C" { extern volatile hk3ds::OpeningStreamProbe hk3ds_opening_stream; }
