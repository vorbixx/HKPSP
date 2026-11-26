#pragma once
#include "world_switches.hpp"
#include "room_visual.hpp"
#include "combat.hpp"
#include "resting.hpp"
#include "world_stag_dust.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hk3ds {
struct StagBellHooks {
    void* context=nullptr;
    bool (*opened)(void*)=nullptr;
    bool (*open)(void*)=nullptr;
};
struct StagLeverSounds {unsigned hit=0,bell=0;};
struct StagLeverRules {
    Rect range{};
    std::array<Vec2,16> polygon{};
    unsigned count=0,visible=0;
    float hitWait=0,boxPause=0,bellPause=0;
    bool valid()const {
        auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<100000;};
        if(count<3||count>polygon.size()||visible>15||!finite(range.left)||!finite(range.right)||
           !finite(range.bottom)||!finite(range.top)||range.left>=range.right||range.bottom>=range.top)return false;
        for(float v:{hitWait,boxPause,bellPause})if(!finite(v)||v<=0||v>30)return false;
        for(unsigned i=0;i<count;++i)if(!finite(polygon[i].x)||!finite(polygon[i].y))return false;
        return true;
    }
};
// RestingGrounds_09 Switch Control1693 -> Stag Bell1842. Its Grate variable
// is null: this instance reveals a station, rather than opening a toll gate.
class StagStationLever {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,Hit,BoxPause,BellPause,BellUp,Ready};
    static constexpr const char* scene="RestingGrounds_09";
    static constexpr const char* id="Ruins Lever";
    static constexpr const char* texturePrefix="stag-lever-";
    static constexpr unsigned station=3;
    bool bind(const StagLeverRules& rules,AnimationBank art,const std::array<RoomDraw,4>& draws,
              SceneBoolHooks save,StagBellHooks bell,RestSave* rest=nullptr){
        if(!rules.valid()||!save.read||!save.write||!bell.opened||!bell.open||!art.pageCount)return false;
        for(const char* name:{"Lever Idle","Lever Hit","Lever Activated","Box_idle","Bell_stand",
                              "Bell_stand_appear","Bell_idle","Front"})if(!art.find(name))return false;
        constexpr unsigned owners[]={164,446,443,444};
        for(unsigned i=0;i<draws.size();++i){
            const auto& d=draws[i];if(d.owner!=owners[i]||d.sprite>=art.sprites.size())return false;
            for(float v:d.values)if(!std::isfinite(v)||std::abs(v)>100000)return false;
            for(unsigned j=7;j<11;++j)if(d.values[j]<0||d.values[j]>1)return false;
        }
        rules_=rules;authored_=draws;bank_={};bank_.atlas=std::move(art);
        bank_.draws.assign(draws.begin(),draws.end());bank_.draws.reserve(4+StagBellDust::capacity);save_=save;bell_=bell;rest_=rest;
        return reload(save,bell,rest);
    }
    bool load(const char* directory,SceneBoolHooks save,StagBellHooks bell,RestSave* rest=nullptr){
        if(!directory)return false;
        char path[512];AnimationBank art;
        if(std::snprintf(path,sizeof(path),"%s/stag-lever.bin",directory)>=int(sizeof(path))||!art.load(path))return false;
        if(std::snprintf(path,sizeof(path),"%s/stag-lever-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        char magic[4];StagLeverRules rules;std::array<RoomDraw,4> draws{};
        bool ok=read(magic,4)&&!std::memcmp(magic,"SLR1",4)&&read(&rules.range,sizeof(Rect))&&
            read(&rules.hitWait,4)&&read(&rules.boxPause,4)&&read(&rules.bellPause,4)&&
            read(&rules.count,4)&&read(&rules.visible,4)&&rules.count>=3&&rules.count<=rules.polygon.size();
        if(ok)ok=read(rules.polygon.data(),rules.count*sizeof(Vec2));
        for(auto& d:draws)ok=ok&&read(&d.sprite,4)&&read(&d.owner,4)&&read(d.values.data(),64);
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
        if(!ok||!bind(rules,std::move(art),draws,save,bell,rest))return false;
        std::snprintf(path,sizeof(path),"%s/stag-lever-fx.bin",directory);
        if(!dust_.load(path,bank_.atlas.sprites.size()))return false;
        dust_.admitPage(bank_);return true;
    }
    bool reload(SceneBoolHooks save,StagBellHooks bell,RestSave* rest=nullptr){
        if(!bound()&&bank_.draws.empty())return true;
        if(!save.read||!save.write||!bell.opened||!bell.open)return false;
        save_=save;bell_=bell;rest_=rest;remaining_=0;routeChanged_=false;players_={};sounds_={};dust_.reset();
        // Source Activated sends ACTIVATE, whose bell transition is Opened;
        // it does not replay Yes or increment stationsOpened.
        phase_=(save_.read(save_.context,scene,id)||bell_.opened(bell_.context))?Phase::Ready:Phase::Idle;
        players_[0].play(bank_.atlas.find(ready()?"Lever Activated":"Lever Idle"),0,true);
        players_[1].play(bank_.atlas.find(ready()?"Bell_stand":"Box_idle"),0,true);
        players_[2].play(bank_.atlas.find("Bell_idle"),0,true);
        players_[3].play(bank_.atlas.find("Front"),0,true);
        if(ready())unlockRoute();
        present();return true;
    }
    void clear(){*this={};}
    bool bound()const{return !bank_.draws.empty();}
    bool ready()const{return phase_==Phase::Ready;}
    bool blocksResting()const{return bound()&&!ready();}
    Phase phase()const{return phase_;}
    const RoomVisualBank& bank()const{return bank_;}
    const StagLeverSounds& sounds()const{return sounds_;}
    std::array<float,3> soundPosition(unsigned i)const{return {authored_[i].values[2],authored_[i].values[5],authored_[i].values[6]};}
    const StagBellDust& dust()const{return dust_;}
    bool takeRouteChanged(){return std::exchange(routeChanged_,false);}
    // Advance before sampling the new nail hit; the hit frame never consumes
    // the source .75s Wait. The caller applies FreezeMoment1 on a true return.
    bool update(float seconds,const Hero& hero,const NailBank& nails){
        if(!bound()||!std::isfinite(seconds)||seconds<=0)return false;
        dust_.update(seconds);
        for(auto& player:players_)player.advance(seconds);
        if(phase_==Phase::Hit){
            remaining_-=seconds;
            if(remaining_<=0&&bell_.open(bell_.context)){phase_=Phase::BoxPause;remaining_=rules_.boxPause;}
        } else if(phase_==Phase::BoxPause){
            remaining_-=seconds;if(remaining_<=0){phase_=Phase::BellPause;remaining_=rules_.bellPause;}
        } else if(phase_==Phase::BellPause){
            remaining_-=seconds;
            if(remaining_<=0){phase_=Phase::BellUp;++sounds_.bell;dust_.start();players_[1].play(bank_.atlas.find("Bell_stand_appear"),0,true);}
        } else if(phase_==Phase::BellUp&&players_[1].complete){
            phase_=Phase::Ready;players_[1].play(bank_.atlas.find("Bell_stand"),0,true);unlockRoute();
        }
        const bool hit=phase_==Phase::Idle&&hero.nail.active&&hero.nail.hitbox&&
            overlaps(bodyBounds(hero.position),rules_.range)&&
            nailOverlapsPolygon(hero,nails.shapes[nailVariant(hero.nail)],rules_.polygon.data(),rules_.count)&&
            save_.write(save_.context,scene,id,true);
        if(hit){phase_=Phase::Hit;++sounds_.hit;remaining_=rules_.hitWait;players_[0].play(bank_.atlas.find("Lever Hit"),0,true);}
        present();return hit;
    }
    void present(PresentationTime time={}){
        if(!bound())return;
        bank_.draws.resize(authored_.size());
        for(unsigned i=0;i<authored_.size();++i){
            auto& draw=bank_.draws[i];draw=authored_[i];
            if(const auto* frame=players_[i].current(bank_.atlas))draw.sprite=static_cast<unsigned>(frame-bank_.atlas.sprites.data());
            bool visible=(rules_.visible&(1u<<i))!=0;
            if(i==1&&(phase_==Phase::BellUp||ready()))visible=true;
            if(i==2&&ready())visible=true;
            if(!visible)draw.values[10]=0;
        }
        dust_.append(bank_,time);
        std::sort(bank_.draws.begin(),bank_.draws.end(),[](const RoomDraw& a,const RoomDraw& b){
            if(a.values[11]!=b.values[11])return a.values[11]<b.values[11];
            if(a.values[12]!=b.values[12])return a.values[12]<b.values[12];
            if(a.values[6]!=b.values[6])return a.values[6]>b.values[6];
            return a.owner<b.owner;
        });
    }
private:
    void unlockRoute(){if(rest_&&!(rest_->stations&(1u<<station))){rest_->stations|=1u<<station;routeChanged_=true;}}
    Phase phase_=Phase::Unbound;
    StagLeverRules rules_{};
    std::array<RoomDraw,4> authored_{};
    std::array<AnimationPlayer,4> players_{};
    RoomVisualBank bank_{};
    SceneBoolHooks save_{};
    StagBellHooks bell_{};
    RestSave* rest_=nullptr;
    float remaining_=0;
    bool routeChanged_=false;
    StagLeverSounds sounds_{};
    StagBellDust dust_;
};
}
