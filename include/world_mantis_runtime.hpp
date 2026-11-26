#pragma once
#include "world_mantis_gate.hpp"
#include "world_switch_terrain.hpp"
#include "room_visual.hpp"
#include "trigger_polygon.hpp"
#include "game_clock.hpp"
#include "combat.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
struct MantisGateData {
    std::array<float,3> waits{};
    std::array<unsigned,2> colliders{};
    std::array<std::array<RoomDraw,2>,2> draws{};
    std::array<std::array<Vec2,5>,2> points{};
    bool valid()const {
        if(waits!=std::array<float,3>{.75f,.5f,.7f}||colliders!=std::array<unsigned,2>{3595,3642})return false;
        constexpr unsigned owners[2][2]={{143,149},{215,1157}};
        const auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=100000;};
        for(unsigned i=0;i<2;++i){
            for(unsigned a=0;a<2;++a){const auto& d=draws[i][a];
                if(d.owner!=owners[i][a]||d.sprite>=64||d.values[15]!=0)return false;
                for(float v:d.values)if(!finite(v))return false;
                for(unsigned c=7;c<=10;++c)if(d.values[c]<0||d.values[c]>1)return false;
            }
            for(auto p:points[i])if(!finite(p.x)||!finite(p.y))return false;
        }
        return true;
    }
    bool load(const char* path){
        auto* f=std::fopen(path,"rb");if(!f)return false;
        MantisGateData next;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"MTG1",4)&&read(next.waits.data(),12);
        for(unsigned i=0;i<2;++i){
            ok=ok&&read(&next.colliders[i],4);
            for(auto& d:next.draws[i])ok=ok&&read(&d.owner,4)&&read(&d.sprite,4)&&read(d.values.data(),64);
            ok=ok&&read(next.points[i].data(),40);
        }
        ok=ok&&std::fgetc(f)==EOF&&next.valid();std::fclose(f);
        if(ok)*this=next;
        return ok;
    }
};

// Owns only these four actors and the enabled bits of the two authored solids.
// Hero attack geometry and the existing scene-bool save table are supplied by
// their owners. Nothing in this adapter changes movement or save schemas.
class MantisGates {
    friend struct SimulationStateHash;
public:
    static constexpr const char* scene="Fungus2_04";
    static constexpr unsigned drawCapacity=4;
    bool load(const char* directory,TerrainBank& terrain,SceneBoolHooks save){
        if(!directory||!*directory)return false;
        char path[512];MantisGateData data;AnimationBank art;
        std::snprintf(path,sizeof(path),"%s/mantis-gate-room.bin",directory);if(!data.load(path))return false;
        std::snprintf(path,sizeof(path),"%s/mantis-gate.bin",directory);if(!art.load(path))return false;
        return bind(data,std::move(art),terrain,save);
    }
    bool bind(const MantisGateData& data,AnimationBank art,TerrainBank& terrain,SceneBoolHooks save){
        if(!data.valid()||!save.read||!save.write||art.pageCount!=1||art.sprites.empty()||art.sprites.size()>64)return false;
        for(const char* name:{"Lever Idle","Lever Hit","Lever Activated","Door Closed","Door Open 1","Door Open 2"}){
            const auto* c=art.find(name);
            if(!c||!c->count||!std::isfinite(c->fps)||c->fps<=0||c->first>art.frames.size()||c->count>art.frames.size()-c->first)return false;
            if((!std::strcmp(name,"Lever Hit")||!std::strcmp(name,"Door Open 1")||!std::strcmp(name,"Door Open 2"))&&c->wrap!=2)return false;
            for(unsigned i=0;i<c->count;++i)if(art.frames[c->first+i]>=art.sprites.size())return false;
        }
        for(unsigned i=0;i<2;++i){
            for(const auto& d:data.draws[i])if(d.sprite>=art.sprites.size())return false;
            const std::vector<Vec2> polygon(data.points[i].begin(),data.points[i].end());
            if(!simpleTriggerPath(polygon,0,5))return false;
        }
        TollSwitchTerrain collision;if(!collision.bind(terrain,data.colliders))return false;
        clear();data_=data;save_=save;collision_=collision;
        bank_.atlas=std::move(art);bank_.draws.reserve(drawCapacity);bound_=true;
        return reload();
    }
    bool reload(){
        if(!bound())return true;
        if(!collision_.resetCollision())return false;
        players_={};shown_={true,true};hits_={};
        for(unsigned i=0;i<2;++i){
            play(i*2,"Lever Idle");play(i*2+1,"Door Closed");
            if(!controls_[i].bind({data_.waits[0],data_.waits[1],data_.waits[2]},save_,scene,identity(i))||
               !apply(i,controls_[i].takeEvents(),nullptr))return false;
        }
        present();return true;
    }
    void clear(){*this={};}
    bool bound()const{return bound_;}
    bool hides(unsigned owner)const{
        if(!bound())return false;
        for(const auto& pair:data_.draws)for(const auto& d:pair)if(owner==d.owner)return true;
        return false;
    }
    const RoomVisualBank& bank()const{return bank_;}
    template<class Emit> void visitAlphaOwners(Emit emit)const{
        if(bound())for(const auto& pair:data_.draws)for(const auto& d:pair)emit(d.owner);
    }
    MantisGate::Phase phase(unsigned i)const{return i<2?controls_[i].phase():MantisGate::Phase::Unbound;}
    const std::array<unsigned,2>& hits()const{return hits_;}
    bool update(float dt,const Hero& hero,const NailBank& nails,GameClock& clock){
        if(!bound()||!std::isfinite(dt)||dt<=0)return false;
        if(!collision_.apply({})){clear();return false;}
        for(auto& p:players_)p.advance(dt);
        bool hit=false;
        for(unsigned i=0;i<2;++i){
            controls_[i].advance(dt,players_[i*2+1].complete);
            if(!apply(i,controls_[i].takeEvents(),&clock)){clear();return false;}
            if(hero.nail.active&&hero.nail.hitbox&&
               nailOverlapsPolygon(hero,nails.shapes[nailVariant(hero.nail)],data_.points[i].data(),5)&&controls_[i].nailHit()){
                hit=true;if(!apply(i,controls_[i].takeEvents(),&clock)){clear();return false;}
            }
        }
        present();return hit;
    }
    void present(){
        if(!bound())return;
        bank_.draws.resize(drawCapacity);
        for(unsigned i=0;i<2;++i)for(unsigned a=0;a<2;++a){
            auto& d=bank_.draws[i*2+a];d=data_.draws[i][a];
            if(const auto* f=players_[i*2+a].current(bank_.atlas))d.sprite=static_cast<unsigned>(f-bank_.atlas.sprites.data());
            if(a&&!shown_[i])d.values[10]=0;
        }
    }
private:
    static const char* identity(unsigned i){return i?"Mantis Lever (1)":"Mantis Lever";}
    void play(unsigned actor,const char* name){players_[actor].play(bank_.atlas.find(name),0,true);}
    bool apply(unsigned i,const MantisGateEvents& e,GameClock* clock){
        TollSwitchEvents collision;
        if(e.removeCollider)collision.removeGates=1u<<i;
        if(!collision_.apply(collision))return false;
        if(e.switchHit){play(i*2,"Lever Hit");++hits_[i];if(clock)clock->enemyKill();}
        if(e.activatedPose)play(i*2,"Lever Activated");
        if(e.firstClip)play(i*2+1,"Door Open 1");
        if(e.secondClip)play(i*2+1,"Door Open 2");
        if(e.hideGate)shown_[i]=false;
        return true;
    }
    MantisGateData data_{};SceneBoolHooks save_{};TollSwitchTerrain collision_{};
    RoomVisualBank bank_{};
    std::array<MantisGate,2> controls_{};std::array<AnimationPlayer,4> players_{};
    std::array<bool,2> shown_{};std::array<unsigned,2> hits_{};bool bound_=false;
};
}
