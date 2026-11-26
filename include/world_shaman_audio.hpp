#pragma once
#include "audio.hpp"
#include "world_transport_audio.hpp"
#include "graphics_profile.hpp"
namespace hk3ds {
constexpr std::size_t shamanPillarAudioCapBytes=755896;
inline bool shamanPillarAudioFits(std::size_t bytes,std::size_t sceneResident,std::size_t baseResident,
                                 std::size_t baseCap,std::size_t linearFree){
    return sceneResident<=shamanPillarAudioCapBytes&&bytes<=shamanPillarAudioCapBytes-sceneResident&&
        linearFree>=bytes+enemyAudioLinearReserveBytes&&
        (baseCap>=enemyAudioBankCapBytes||audioClipFits(bytes,baseResident+sceneResident,baseCap));
}
class ShamanPillarAudio {
public:
    static constexpr unsigned owner=0xC7000200u;
    void observe(const std::array<unsigned,3>& cues,bool bound,unsigned epoch,AudioVoices& voices){
        bool reset=!seen_||epoch_!=epoch||!bound;
        for(unsigned i=0;i<3;++i)reset|=cues[i]<previous_[i];
        if(reset){
            for(unsigned i=0;i<voices.voices().size();++i){const auto& v=voices.voices()[i];
                if(v.active&&v.owner>=owner&&v.owner<owner+3)voices.finish(i,v.generation);}
            previous_={};epoch_=epoch;seen_=true;
        }
        if(!bound)return;
        constexpr EnemyClip clips[]{EnemyClip::TownLeverHit,EnemyClip::ShamanPillarShake,EnemyClip::ShamanPillarFinish};
        for(unsigned i=0;i<3;++i)if(cues[i]>previous_[i])voices.playAtPitch(clips[i],owner+i,1);
        previous_=cues;
    }
private:
    std::array<unsigned,3> previous_{};unsigned epoch_=0;bool seen_=false;
};
}
