#pragma once
#include "audio.hpp"
#include "world_stag_lever.hpp"
#include "world_transport_audio.hpp"

namespace hk3ds {
constexpr std::size_t stagFeedbackAudioCapBytes=100000;
inline bool stagFeedbackAudioFits(std::size_t bytes,std::size_t resident,std::size_t cap,std::size_t free){
    return bytes<=stagFeedbackAudioCapBytes&&free>=bytes+enemyAudioLinearReserveBytes&&
        (cap>=enemyAudioBankCapBytes||audioClipFits(bytes,resident,cap));
}
class StagLeverAudio {
public:
    static constexpr unsigned owner=0xC5000000u|164u;
    void observe(StagLeverSounds now,bool bound,unsigned epoch,AudioVoices& voices){
        if(!seen_||epoch_!=epoch||!bound||now.hit<previous_.hit||now.bell<previous_.bell){
            for(unsigned i=0;i<voices.voices().size();++i){const auto& v=voices.voices()[i];
                if(v.active&&(v.owner==owner||v.owner==owner+1))voices.finish(i,v.generation);}
            previous_={};seen_=true;epoch_=epoch;
        }
        if(!bound)return;
        if(now.hit>previous_.hit)voices.playAtPitch(EnemyClip::TownLeverHit,owner,1);
        if(now.bell>previous_.bell)voices.playAtPitch(EnemyClip::StagBellRise,owner+1,1);
        previous_=now;
    }
    void mix(AudioVoices& voices,const StagStationLever& lever,Vec2 camera)const{
        for(unsigned i=0;i<2;++i){const auto p=lever.soundPosition(i);
            // FSM1693/1842 use the same resources4126 prefab/13920 source as Town.
            voices.updateOwnerMix(owner+i,townLeverAudioMix(p[0]-camera.x,p[1]-camera.y,p[2]+38.1f));}
    }
private:
    StagLeverSounds previous_{};unsigned epoch_=0;bool seen_=false;
};
}
