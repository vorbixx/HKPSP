#pragma once
#include "world_switch_terrain.hpp"
#include "animation.hpp"
#include "combat.hpp"
#include "game_clock.hpp"

namespace hk3ds {
struct WorldSwitchBinding {
    TollSwitchRules rules{};
    Rect nailTrigger{};
    std::array<unsigned,2> colliders{};
    std::array<unsigned,3> idleSprites{}; // switch, gate 0, gate 1 in the supplied bank
    const char* scene=nullptr;
    const char* id=nullptr;
    const char* hitClip=nullptr;
    const char* idleClip=nullptr; // only when the source animator auto-plays it
    std::array<const char*,2> gateClips{};
};
// Room/save owners supply source data and lifetime-stable banks/identity.
// Missing art is not replaced; an invalid binding leaves the old state intact.
class WorldSwitchRuntime {
    friend struct SimulationStateHash;
public:
    bool bind(const WorldSwitchBinding& binding,SceneBoolHooks save,TerrainBank& terrain,const AnimationBank& bank)
    {
        const auto r=binding.nailTrigger;
        if(!std::isfinite(r.left)||!std::isfinite(r.bottom)||!std::isfinite(r.right)||
           !std::isfinite(r.top)||r.left>=r.right||r.bottom>=r.top||binding.rules.freezeMoment!=1)return false;
        WorldSwitchRuntime next;
        for(unsigned i=0;i<3;++i){
            if(binding.idleSprites[i]>=bank.sprites.size())return false;
            const char* name=i?binding.gateClips[i-1]:binding.hitClip;
            if(!name)return false;
            const auto* clip=bank.find(name);
            if(!clip||!std::isfinite(clip->fps)||clip->fps<=0||!clip->count||
               clip->first>bank.frames.size()||clip->count>bank.frames.size()-clip->first||
               (i&&clip->wrap!=2))return false;
            for(unsigned f=0;f<clip->count;++f)
                if(bank.frames[clip->first+f]>=bank.sprites.size())return false;
            next.clips_[i]=clip;
        }
        if(binding.idleClip){
            const auto* idle=bank.find(binding.idleClip);
            if(!idle||!std::isfinite(idle->fps)||idle->fps<=0||!idle->count||
               idle->first>bank.frames.size()||idle->count>bank.frames.size()-idle->first)return false;
            for(unsigned f=0;f<idle->count;++f)if(bank.frames[idle->first+f]>=bank.sprites.size())return false;
            next.players_[0].play(idle,0,true);
            next.idleClip_=idle;
        }
        if(!next.terrain_.bind(terrain,binding.colliders)||
           !next.controller_.bind(binding.rules,save,binding.scene,binding.id))return false;
        next.bank_=&bank;next.trigger_=r;next.idleSprites_=binding.idleSprites;
        if(!next.apply(next.controller_.takeEvents(),nullptr))return false;
        *this=next;return true;
    }
    void clear(){*this={};}
    bool reload(){
        if(!bound())return true;
        if(!controller_.reload()||!terrain_.resetCollision())return false;
        pending_={};players_={};
        if(idleClip_)players_[0].play(idleClip_,0,true);
        return apply(controller_.takeEvents(),nullptr);
    }
    bool bound()const{return bank_!=nullptr;}
    const AnimationBank* artwork()const{return bank_;}
    // The activation frame does not charge the newly started Wait/animations.
    bool update(float dt,const Hero& hero,const NailBank& nails,GameClock& clock)
    {
        if(!bound()||!std::isfinite(dt)||dt<=0)return false;
        if(!terrain_.apply({})){clear();return false;}
        for(unsigned i=0;i<3;++i){
            players_[i].advance(dt);
            if(i&&players_[i].complete)controller_.gateAnimationFinished(i-1);
        }
        controller_.advance(dt);
        const bool hit=controller_.phase()==TollSwitch::Phase::Idle&&hero.nail.active&&hero.nail.hitbox&&
            nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],trigger_)&&controller_.nailHit();
        if(!apply(controller_.takeEvents(),&clock))clear();
        return hit;
    }
    const SpriteFrame* switchFrame()const{return pose(0);}
    const SpriteFrame* gateFrame(unsigned gate)const {
        return gate<2&&!(controller_.removedGates()&(1u<<gate))?pose(gate+1):nullptr;
    }
    const AnimationPlayer& switchPlayer()const{return players_[0];}
    TollSwitchEvents takeEvents(){return std::exchange(pending_,{});}
private:
    const SpriteFrame* pose(unsigned index)const {
        return !bank_?nullptr:players_[index].clip?players_[index].current(*bank_):&bank_->sprites[idleSprites_[index]];
    }
    bool apply(const TollSwitchEvents& event,GameClock* clock)
    {
        if(!terrain_.apply(event))return false;
        if(event.switchHit){
            players_[0].play(clips_[0],0,true);
            // GameManager.FreezeMoment(1) matches the existing enemy-kill profile.
            if(clock)clock->enemyKill();
        }
        if(event.restorePose){
            players_[0].play(clips_[0],0,true);
            // Tk2dPlayFrame calls PlayFromFrame on the preceding Hit clip.
            // WarpClipToLocalTime samples modulo count, retaining the requested
            // clock; Once completes on the following animator update.
            players_[0].clock=static_cast<float>(event.activatedFrame)+.001f;
            players_[0].frame=event.activatedFrame%clips_[0]->count;
        }
        for(unsigned i=0;i<2;++i)if(event.openGates&(1u<<i))players_[i+1].play(clips_[i+1],0,true);
        pending_.switchHit|=event.switchHit;pending_.restorePose|=event.restorePose;
        if(event.switchHit)pending_.freezeMoment=event.freezeMoment;
        if(event.restorePose)pending_.activatedFrame=event.activatedFrame;
        pending_.openGates|=event.openGates;pending_.removeGates|=event.removeGates;
        pending_.detachChildren|=event.detachChildren;return true;
    }
    TollSwitch controller_;
    TollSwitchTerrain terrain_;
    const AnimationBank* bank_=nullptr;
    const Clip* idleClip_=nullptr;
    std::array<const Clip*,3> clips_{};
    std::array<AnimationPlayer,3> players_{};
    std::array<unsigned,3> idleSprites_{};
    Rect trigger_{};
    TollSwitchEvents pending_{};
};
static_assert(sizeof(WorldSwitchRuntime)<=384,"Bound room-switch runtime memory on host and ARM");
}
