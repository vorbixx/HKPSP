#pragma once
#include "world_switches.hpp"
#include "room_visual.hpp"
#include "terrain.hpp"
#include "combat.hpp"
#include "game_clock.hpp"
#include "world_shaman_effects.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// Stage both source ledgers before the caller publishes either one.
template<class Scenes,class Player>
bool stageShamanPillarSave(Scenes& scenes,Player& player,unsigned which){
    if(which>1)return false;
    auto next=scenes;
    if(!next.set(Scenes::key("Crossroads_06",which?"Raising Pillar":"Gate Switch")))return false;
    if(!which){
        auto target=next;
        if(!target.set(Scenes::key("Crossroads_06","Raising Pillar")))return false;
        auto data=player;
        if(!data.set("shamanPillar",1))return false;
        player=data;
    }
    scenes=next;return true;
}
struct ShamanPillarHooks {
    void* context=nullptr;
    bool (*read)(void*,unsigned)=nullptr; // switch/global, pillar, infected
    bool (*write)(void*,unsigned)=nullptr; // atomic switch/global, then pillar
};
struct ShamanPillarData {
    std::array<float,3> waits{};
    Rect trigger{};
    std::array<unsigned,3> ids{512,120,2245};
    std::array<RoomDraw,2> draws{};
    bool valid()const{
        for(float v:waits)if(!std::isfinite(v)||v<=0||v>30)return false;
        if(!std::isfinite(trigger.left)||!std::isfinite(trigger.bottom)||!std::isfinite(trigger.right)||!std::isfinite(trigger.top)||
           trigger.left>=trigger.right||trigger.bottom>=trigger.top||ids!=std::array<unsigned,3>{512,120,2245})return false;
        for(unsigned i=0;i<2;++i){if(draws[i].owner!=ids[i])return false;for(float v:draws[i].values)if(!std::isfinite(v))return false;}
        return true;
    }
    bool load(const char* path){
        auto* f=std::fopen(path,"rb");if(!f)return false;char magic[4];ShamanPillarData next;
        bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"SHP1",4)&&
            std::fread(next.waits.data(),4,3,f)==3&&std::fread(&next.trigger,4,4,f)==4&&std::fread(next.ids.data(),4,3,f)==3;
        for(auto& d:next.draws)ok=ok&&std::fread(&d.owner,4,1,f)==1&&std::fread(&d.sprite,4,1,f)==1&&std::fread(d.values.data(),4,16,f)==16;
        ok=ok&&std::fgetc(f)==EOF&&next.valid();std::fclose(f);if(ok)*this=next;return ok;
    }
};
// Switch Control3406 sends ACTIVATE to Pillar Control3351. No moving hero geometry.
class ShamanPillar {
    friend struct SimulationStateHash;
public:
    static constexpr const char* scene="Crossroads_06";
    enum class Phase {Idle,Shake,Open,Finishing,Opened};
    bool load(const char* directory,TerrainBank& terrain,ShamanPillarHooks save){
        char path[512];ShamanPillarData data;AnimationBank art;
        std::snprintf(path,sizeof(path),"%s/shaman-pillar-room.bin",directory);if(!data.load(path))return false;
        std::snprintf(path,sizeof(path),"%s/shaman-pillar.bin",directory);if(!art.load(path)||!bind(data,std::move(art),terrain,save)||!dust_.load(directory,bank_.atlas))return false;
        dust_.admit(bank_);return true;
    }
    bool bind(const ShamanPillarData& data,AnimationBank art,TerrainBank& terrain,ShamanPillarHooks save){
        if(!data.valid()||!save.read||!save.write||!art.pageCount)return false;
        for(const char* name:{"Switch Idle","Switch Hit","Switch Activated","Pillar Shake","Pillar Open","Pillar Opened"}){
            const auto* c=art.find(name);if(!c||!c->count||!std::isfinite(c->fps)||c->fps<=0||c->first>art.frames.size()||c->count>art.frames.size()-c->first)return false;
            for(unsigned i=0;i<c->count;++i)if(art.frames[c->first+i]>=art.sprites.size())return false;
        }
        bool found=false;for(const auto& shape:terrain.shapes)found|=shape.id==data.ids[2];if(!found)return false;
        for(const auto& draw:data.draws)if(draw.sprite>=art.sprites.size())return false;
        clear();data_=data;terrain_=&terrain;save_=save;bank_.atlas=std::move(art);bank_.draws.reserve(ShamanPillarDust::drawCapacity);bank_.draws.resize(2);
        return reload(save);
    }
    bool reload(ShamanPillarHooks save){
        if(!bound())return true;
        if(!save.read||!save.write)return false;
        save_=save;sounds_={};players_={};timer_=hitTimer_=0;hit_=false;
        activated_=save_.read(save_.context,0);hidden_=save_.read(save_.context,2);
        const bool raised=save_.read(save_.context,1);
        phase_=raised?Phase::Opened:Phase::Idle;pending_=activated_&&!raised;
        terrain_->setEnabled(data_.ids[2],!raised&&!hidden_);
        play(0,activated_?"Switch Activated":"Switch Idle");
        if(raised)play(1,"Pillar Opened");
        dust_.reset();present();return true;
    }
    void clear(){*this={};}
    bool bound()const{return terrain_!=nullptr;}
    bool hides(unsigned owner)const{return bound()&&(owner==data_.ids[0]||owner==data_.ids[1]);}
    template<class Emit> void visitAlphaOwners(Emit emit)const{
        if(bound()){emit(data_.ids[0]);emit(data_.ids[1]);}
    }
    const RoomVisualBank& bank()const{return bank_;}
    Phase phase()const{return phase_;}
    bool activated()const{return activated_;}
    const std::array<unsigned,3>& sounds()const{return sounds_;}
    const ShamanPillarDust& dust()const{return dust_;}
    bool update(float dt,const Hero& hero,const NailBank& nails,GameClock& clock){
        if(!bound()||!std::isfinite(dt)||dt<=0)return false;
        const auto before=phase_;dust_.update(dt);
        for(auto& p:players_)p.advance(dt);
        if(save_.read(save_.context,2)){hidden_=true;terrain_->setEnabled(data_.ids[2],false);}
        if(before==Phase::Shake){
            timer_-=dt;if(timer_<=.000001f){phase_=Phase::Open;terrain_->setEnabled(data_.ids[2],false);play(1,"Pillar Open");dust_.start(0);dust_.start(4);}
        }else if(before==Phase::Open&&players_[1].complete){
            phase_=Phase::Finishing;timer_=data_.waits[2];play(1,"Pillar Opened");++sounds_[2];dust_.start(1);dust_.start(3);dust_.start(5);
        }else if(before==Phase::Finishing){timer_-=dt;if(timer_<=.000001f)phase_=Phase::Opened;}
        if(hit_){hitTimer_-=dt;if(hitTimer_<=.000001f){hit_=false;pending_=true;}}
        if(pending_&&!hidden_&&phase_==Phase::Idle&&save_.write(save_.context,1)){
            pending_=false;phase_=Phase::Shake;timer_=data_.waits[1];play(1,"Pillar Shake");++sounds_[1];dust_.start(2);
        }
        bool hit=false;
        if(!activated_&&hero.nail.active&&hero.nail.hitbox&&nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],data_.trigger)&&save_.write(save_.context,0)){
            activated_=hit_=hit=true;hitTimer_=data_.waits[0];play(0,"Switch Hit");++sounds_[0];clock.enemyKill();
        }
        present();return hit;
    }
    void present(PresentationTime time={}){
        if(!bound())return;
        bank_.draws.resize(2);
        for(unsigned i=0;i<2;++i){auto& d=bank_.draws[i];d=data_.draws[i];
            if(const auto* f=players_[i].current(bank_.atlas))d.sprite=static_cast<unsigned>(f-bank_.atlas.sprites.data());
            if(i==1&&hidden_)d.values[10]=0;
        }
        if(!hidden_)dust_.present(bank_,time);
    }
private:
    void play(unsigned actor,const char* name){players_[actor].play(bank_.atlas.find(name),0,true);}
    ShamanPillarDust dust_;
    ShamanPillarData data_{};ShamanPillarHooks save_{};TerrainBank* terrain_=nullptr;
    RoomVisualBank bank_{};std::array<AnimationPlayer,2> players_{};std::array<unsigned,3> sounds_{};
    Phase phase_=Phase::Idle;float timer_=0,hitTimer_=0;bool activated_=false,hit_=false,pending_=false,hidden_=false;
};
}
