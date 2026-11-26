#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace hk3ds {
// The room loader selects an area independently of room/retry epochs.
enum class SceneAudioId : unsigned { Tutorial, Dirtmouth, Crossroads, Silent, Greenpath, GreenpathSub, ShamanTemple, FalseKnight, Count };
SceneAudioId sceneAudioId(const char* scene);
// Only admitted profiles resolving to the same original cues retain phase.
bool sceneAudioKeepsPhase(SceneAudioId from,SceneAudioId to);
// Reserve enough ID bits for every admitted profile; epochs must never alias IDs.
constexpr unsigned sceneAudioIdBits=3, sceneAudioEpochShift=sceneAudioIdBits+1;
constexpr unsigned sceneAudioIdMask=(1u<<sceneAudioIdBits)-1;
static_assert(static_cast<unsigned>(SceneAudioId::Count)<=sceneAudioIdMask+1,"scene audio command capacity");
constexpr unsigned sceneAudioCommand(unsigned epoch,bool active,SceneAudioId id)
{
    const auto value=static_cast<unsigned>(id)<static_cast<unsigned>(SceneAudioId::Count)?id:SceneAudioId::Silent;
    return (epoch<<sceneAudioEpochShift)|(static_cast<unsigned>(value)<<1)|unsigned(active);
}
constexpr SceneAudioId sceneAudioCommandId(unsigned command)
{return static_cast<SceneAudioId>((command>>1)&sceneAudioIdMask);}
constexpr unsigned sceneAudioCommandEpoch(unsigned command){return command>>sceneAudioEpochShift;}
struct SceneAudioProfile {
    std::array<const char*,3> files;
    std::array<float,3> gains;
    unsigned mask;
    unsigned musicMask;
};
const SceneAudioProfile& sceneAudioProfile(SceneAudioId id);
// Tutorial's dependency-resolved Cave cue also serves Shaman Temple.
class WindCue {
public:
    static constexpr unsigned ChannelCount=3, TutorialMask=3;
    // SceneManager level6:12820 -> at Cave resources.assets:3852.
    // Cue sources 0/4 -> groups 3841/3835 at 0 dB; third queue stays idle.
    static constexpr std::array<float,ChannelCount> TutorialMixerGain{{1.f,1.f,0.f}};
    static constexpr float TransitionSeconds=0.5f;
    struct Channel { bool playing=false; float gain=0,start=0,target=0,elapsed=0; };
    void apply(unsigned mask);
    void advance(float realSeconds);
    void scene(unsigned epoch,bool active,SceneAudioId id=SceneAudioId::Tutorial);
    void reset();
    const std::array<Channel,ChannelCount>& channels()const{return channels_;}
private:
    std::array<Channel,ChannelCount> channels_{};
    unsigned epoch_=0;
    bool observed_=false,active_=false;
    SceneAudioId id_=SceneAudioId::Tutorial;
};

// HKAW: little-endian magic/rate/channels/frame count, followed by PCM16.
// No resident clip or hot-path allocations; reads span EOF without padding.
// Commit phase only after a complete segment. Recoverable I/O gets at most
// three attempts; a terminal fault requires an explicit successful rewind.
class WindPcmStream {
public:
    enum Operation { None, Seek, Read };
    static constexpr unsigned IoAttempts=3;
    using Reader=std::size_t(*)(void*,std::size_t,std::size_t,FILE*);
    using Seeker=int(*)(FILE*,long,int);
    using Opener=FILE*(*)(const char*,const char*);
    struct Failure {
        unsigned operation=0,position=0,requested=0,received=0;
        int error=0;
        unsigned flags=0;
    };
    ~WindPcmStream(){close();}
    WindPcmStream()=default;
    WindPcmStream(Reader reader,Seeker seeker,Opener opener=std::fopen):reader_(reader),seeker_(seeker),opener_(opener){}
    WindPcmStream(const WindPcmStream&)=delete;
    WindPcmStream& operator=(const WindPcmStream&)=delete;
    bool open(const char* path);
    bool openScene(const char* directory,SceneAudioId id,unsigned slot);
    // Canonical PCM16 stereo WAV emitted by convert_enemy_audio.py.
    bool openWave(const char* path);
    bool read(void* destination,std::size_t bytes);
    bool readOnce(void* destination,std::size_t capacity,unsigned& frames);
    unsigned remainingFrames()const{return frames_-position_;}
    // Caller owns the immutable prefix until close(); priming happens at load.
    bool cachePrefix(void* storage,std::size_t bytes);
    bool rewind();
    void close();
    unsigned wraps()const{return wraps_;}
    unsigned frames()const{return frames_;}
    unsigned sampleRate()const{return sampleRate_;}
    const Failure& failure()const{return failure_;}
    unsigned ioFailures()const{return ioFailures_;}
    unsigned recoveries()const{return recoveries_;}
private:
    FILE* file_=nullptr;
    unsigned frames_=0,position_=0,wraps_=0;
    unsigned sampleRate_=0;
    unsigned dataOffset_=16;
    const unsigned char* prefix_=nullptr;
    unsigned prefixFrames_=0;
    Failure failure_{};
    Reader reader_=std::fread;
    Seeker seeker_=std::fseek;
    Opener opener_=std::fopen;
    unsigned ioFailures_=0,recoveries_=0;
    bool faulted_=false;
    bool seek(unsigned frame);
    void fail(Operation operation,unsigned frame,unsigned requested,unsigned received);
};
}
