#pragma once
#include "audio.hpp"
#include <array>
#include <cstddef>

namespace hk3ds {
// NDSP channel IDs are 0..23 (libctru ndsp/channel.h).
// Keep all 12 enemy voices. Slash variants share two slots: native tails are
// shorter than two Quick Slash cooldowns, so at most two variants overlap.
constexpr unsigned ndspChannelCount=24;
constexpr unsigned footstepChannel=4,tabletHumChannel=1;
constexpr unsigned slashFirstChannel=5,slashChannelCount=2;
constexpr unsigned windFirstChannel=7,windChannelCount=3;
constexpr unsigned enemyFirstChannel=12;
static_assert(slashFirstChannel+slashChannelCount==windFirstChannel,"slash/wind separation");
static_assert(windFirstChannel+windChannelCount==10,"decor/plant channels 10/11 remain dedicated");

constexpr unsigned audioVoiceChannel(unsigned voice)
{
    return voice==AudioVoices::TabletHumVoice ? tabletHumChannel :
        enemyFirstChannel+voice-(voice>AudioVoices::TabletHumVoice ? 1u : 0u);
}
constexpr bool audioChannelLayoutValid()
{
    std::array<bool,ndspChannelCount> used{};
    // Soft landing and running footsteps share 4; slash variants share 5/6.
    for(unsigned channel:{0u,2u,3u,footstepChannel,5u,6u,7u,8u,9u,10u,11u})used[channel]=true;
    for(unsigned i=0;i<AudioVoices::VoiceCount;++i){
        const auto channel=audioVoiceChannel(i);
        if(channel>=used.size()||used[channel])return false;
        used[channel]=true;
    }
    return true;
}
static_assert(audioChannelLayoutValid(),"every playback group must have disjoint real NDSP channels");

class FootstepAudioSlot {
public:
    static constexpr unsigned Invalid=~0u;
    bool select(unsigned clip,bool busy)
    {
        if(busy&&clip_!=clip)return false;
        clip_=clip;return true;
    }
    unsigned clip()const{return clip_;}
    bool shouldStop(bool running,bool landed)const{return clip_==4&&(!running||landed);}
    // Source lifetime is independent of delayed DSP buffer completion feedback.
    bool playing(double now)const{return clip_!=Invalid&&now<end_;}
    void started(double now,double duration){end_=now+duration;}
    void reset(){clip_=Invalid;end_=0;}
private:
    unsigned clip_=Invalid;
    double end_=0;
};

class SlashAudioSlots {
public:
    static constexpr unsigned Invalid=~0u;
    unsigned select(unsigned clip,const std::array<bool,slashChannelCount>& busy)
    {
        // Repeated variants retain the old one-buffer-per-clip restart behavior.
        for(unsigned i=0;i<slashChannelCount;++i)if(clips_[i]==clip)return i;
        for(unsigned i=0;i<slashChannelCount;++i)if(!busy[i]){clips_[i]=clip;return i;}
        return Invalid;
    }
    unsigned clip(unsigned slot)const{return clips_[slot];}
    void reset(){clips_={Invalid,Invalid};}
private:
    std::array<unsigned,slashChannelCount> clips_{{Invalid,Invalid}};
};
}
