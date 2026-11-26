#pragma once
#include "audio_mixer.hpp"
#include <memory>

namespace hkpsp {
// Owned by runGame. Asset loading is startup-only; frame commands never read files.
class AudioDevice {
public:
    virtual ~AudioDevice()=default;
    virtual void load(unsigned clip,const std::string& path,bool stream=false)=0;
    virtual void musicClip(unsigned){}
    virtual void volumes(unsigned,unsigned,unsigned=10){}
    virtual const AudioClip& clip(unsigned id)const=0;
    // Both commands take source gain/pitch; Device applies clip metadata once.
    virtual void play(unsigned voice,unsigned clip,bool loop,float gain=1,float pitch=1,float pan=0)=0;
    virtual void stop(unsigned voice)=0;
    virtual void volumePan(unsigned voice,float gain,float pan,float pitch=1)=0;
    virtual bool active(unsigned voice)const=0;
    // Before start: reserve tail caches only for voices that can play long
    // non-streaming clips. Generic devices keep every voice enabled by default.
    virtual void tailVoices(std::uint32_t){}
    virtual void start()=0;
    virtual void frame(unsigned frame)=0;
    virtual void finish()=0;
};
std::unique_ptr<AudioDevice> makeAudio(const std::string& output,bool diagnostic);
}
