#pragma once
#include "audio.hpp"
#include "world_city_crest.hpp"
#include "world_transport_audio.hpp"
#include "graphics_profile.hpp"

namespace hk3ds {
constexpr std::size_t cityCrestAudioCapBytes=1200000;
inline bool cityCrestAudioFits(std::size_t bytes,std::size_t sceneResident,std::size_t baseResident,
                              std::size_t baseCap,std::size_t linearFree){
    // New's base Tutorial bank and temporary room banks have separate caps,
    // as with Uumuu. Old retains its total bounded PCM admission policy.
    return sceneResident<=cityCrestAudioCapBytes&&bytes<=cityCrestAudioCapBytes-sceneResident&&
        linearFree>=bytes+enemyAudioLinearReserveBytes&&
        (baseCap>=enemyAudioBankCapBytes||audioClipFits(bytes,baseResident+sceneResident,baseCap));
}
// Presentation counters retain transitions between render/audio observations.
// FSM5613: Open 1 starts both sources; Bump/Slam stop the movement loop.
class CityCrestAudio {
public:
    // Reserve a domain range for the loop and its four spawned source actors.
    static constexpr unsigned owner=0xC4000000u|1466u;
    static bool owns(unsigned value){return value>=owner&&value<owner+5;}
    void observe(CityCrestSounds now,bool bound,unsigned epoch,AudioVoices& voices){
        if(!seen_||epoch_!=epoch||!bound||now.open<previous_.open||now.bump<previous_.bump||
           now.drop<previous_.drop||now.slam<previous_.slam){
            voices.stopOwner(owner);
            for(unsigned i=0;i<voices.voices().size();++i){
                const auto& v=voices.voices()[i];
                if(v.active&&owns(v.owner))voices.finish(i,v.generation);
            }
            previous_={};epoch_=epoch;seen_=true;
        }
        if(!bound)return;
        if(now.open>previous_.open)voices.playAtPitch(EnemyClip::CityCrestOpen,owner+1,1);
        if(now.moving&&!previous_.moving)voices.startLoop(EnemyClip::CityCrestMove,owner,0);
        if(!now.moving&&previous_.moving)voices.stopOwner(owner);
        if(now.bump>previous_.bump)voices.playAtPitch(EnemyClip::CityCrestBump,owner+2,1);
        // Both are already present from the same original AudioClip assets:
        // sharedassets6:161 stalactite_break and :171 false_knight_land_1st_time.
        if(now.drop>previous_.drop)voices.playAtPitch(EnemyClip::StalactiteStart,owner+3,1);
        if(now.slam>previous_.slam)voices.playAtPitch(EnemyClip::InitialLand,owner+4,1);
        previous_=now;
    }
    void mix(AudioVoices& voices,float dx,float dy,float dz,float movingY)const{
        voices.updateOwnerMix(owner,townLeverAudioMix(dx,dy+movingY,dz));
        // AudioPlayerOneShotSingle spawns an independent actor at its event's
        // world position. Later gate motion must not drag that actor with it.
        for(unsigned i=0;i<4;++i)voices.updateOwnerMix(owner+1+i,townLeverAudioMix(dx,dy+previous_.y[i],dz));
    }
private:
    CityCrestSounds previous_{};unsigned epoch_=0;bool seen_=false;
};
}
