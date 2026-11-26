#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hkpsp {
// Preserve shared voices 0..23; PSP-only area layers use the following slots.
constexpr unsigned audioTensionVoice=24,audioCaveNoisesVoice=25;
constexpr unsigned audioRainIndoorVoice=26,audioRainOutdoorVoice=27;
constexpr unsigned audioActionVoice=28;
constexpr unsigned audioSurfaceWindVoice=29,audioSurfaceGateVoice=30;
constexpr unsigned audioRate=32000,audioVoiceCount=audioSurfaceGateVoice+1,audioClipCount=256;
static_assert(audioVoiceCount<32,"AudioDevice tail voice mask capacity");
// Keep an immediate 250 ms head; longer cues use the existing per-voice tail
// cache rather than retaining up to two seconds per clip across room loads.
constexpr unsigned audioLongFrames=audioRate/2,audioHeadFrames=audioRate/4;
constexpr unsigned audioTailPageFrames=4096,audioTailPages=3;
constexpr unsigned audioPcmOffset=2048,audioSectorBytes=512;
struct AudioClip {
    unsigned channels=0,frames=0;bool stream=false;float gain=1,pitch=1;
    std::string path;std::vector<std::int16_t> pcm;
    unsigned pcmOffset=audioPcmOffset; // Absolute PCM offset in loose HPA1 or shared HPK1.
    void load(const std::string& filename,bool streaming);
    bool tail()const{return !stream&&pcm.size()<std::size_t(frames)*channels;}
};
// Q16 pitch and stereo coefficients make host/Allegrex accumulation identical.
struct AudioVoice {
    unsigned clip=~0u,generation=0,step=65536;bool loop=false;
    int left=0,right=0;
};
using AudioState=std::array<AudioVoice,audioVoiceCount>;
class AudioStream {
public:
    virtual ~AudioStream()=default;
    // Called by the mixer on Play, never on volume-only state updates.
    virtual void restart()=0;
    virtual unsigned read(std::int16_t* stereo,unsigned frames)=0;
};
// One cache per voice: simultaneous instances of a clip have independent phase.
// prepare/page run on the mixer; file reads and page publication run on I/O.
class AudioTail {
public:
    virtual ~AudioTail()=default;
    virtual void prepare(unsigned clip,unsigned frame)=0;
    virtual const std::int16_t* page(unsigned page)=0;
};
class AudioMixer {
public:
    explicit AudioMixer(const std::array<AudioClip,audioClipCount>& clips):clips_(clips){}
    void apply(const AudioState& state);
    void render(std::int16_t* stereo,unsigned frames,AudioStream* const* streams,AudioTail* const* tails=nullptr);
    unsigned clipped=0,peak=0;
private:
    const std::array<AudioClip,audioClipCount>& clips_;
    AudioState state_{};
    std::array<std::uint64_t,audioVoiceCount> position_{};
    std::array<bool,audioVoiceCount> streamRestart_{};
};
}
