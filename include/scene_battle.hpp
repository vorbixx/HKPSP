#pragma once
#include "physics.hpp"
#include "scene_bool_save.hpp"
#include <cmath>
#include <cstring>
#include <vector>

namespace hk3ds {
struct SceneBattleData {
    struct Gate {unsigned collider=0;std::vector<unsigned> owners;};
    struct Spawn {unsigned owner=0;Vec2 position{};bool reposition=false;};
    struct Wave {
        std::vector<Spawn> spawns;
        float delay=0,timeout=0;
        unsigned healthOwner=0;
        int healthBelow=0;
    };
    struct Encounter {
        SceneBoolSave::Entry key{};
        Rect trigger{};
        std::vector<unsigned> enemies;
        float endDelay=0;
        std::vector<Wave> waves;
    };
    std::vector<Gate> gates;
    std::vector<Encounter> encounters;
};
// Source BG events own both the collision and renderer state. A cleared
// PersistentBoolItem must never close its room again after a save/reload.
class SceneBattle {
    friend struct SimulationStateHash;
public:
    void bind(const SceneBattleData& data,SceneBoolSave* save=nullptr) {
        data_=data;save_=save;states_.assign(data.encounters.size(),{});closed_=false;
        for(unsigned i=0;i<states_.size();++i)
            states_[i].done=save_&&save_->get(data_.encounters[i].key);
    }
    bool empty()const{return data_.gates.empty();}
    void restart(){bind(data_,save_);}
    bool closed()const{return closed_;}
    bool staged(unsigned owner)const {
        for(const auto& e:data_.encounters)for(const auto& w:e.waves)
            for(const auto& spawn:w.spawns)if(spawn.owner==owner)return true;
        return false;
    }
    bool owns(unsigned collider)const {
        for(const auto& g:data_.gates)if(g.collider==collider)return true;
        return false;
    }
    bool hides(unsigned owner)const {
        if(closed_)return false;
        for(const auto& g:data_.gates)for(auto id:g.owners)if(id==owner)return true;
        return false;
    }
    bool event(const char* name,unsigned encounter=0) {
        if(!std::strcmp(name,"BG CLOSE")||!std::strcmp(name,"BG QUICK CLOSE")){
            if(encounter<states_.size()&&states_[encounter].done)return false;
            closed_=true;return true;
        }
        if(!std::strcmp(name,"BG OPEN")||!std::strcmp(name,"BG QUICK OPEN")||!std::strcmp(name,"BG DESTROY")){
            closed_=false;
            if(encounter<states_.size()){
                auto& s=states_[encounter];s.done=true;s.running=false;
                const auto key=data_.encounters[encounter].key;
                if(save_&&key.scene&&key.object)save_->set(key);
            }
            return true;
        }
        return false;
    }
    template<class Alive> void step(float dt,Vec2 hero,Alive alive) {
        step(dt,hero,alive,[](const SceneBattleData::Spawn&){return true;});
    }
    template<class Health,class Wake> void step(float dt,Vec2 hero,Health health,Wake wake) {
        if(!std::isfinite(dt)||dt<=0)return;
        for(unsigned i=0;i<states_.size();++i){
            auto& s=states_[i];const auto& e=data_.encounters[i];if(s.done)continue;
            unsigned remaining=0;bool supported=true;
            for(auto id:e.enemies){const int status=health(id);supported&=status>=0;if(status>0)++remaining;}
            if(!s.running){
                // Unsupported/omitted actors cannot create an unwinnable seal.
                if(!supported||!remaining||hero.x<e.trigger.left||hero.x>e.trigger.right||
                   hero.y<e.trigger.bottom||hero.y>e.trigger.top)continue;
                if(!e.waves.empty()&&!startWave(e.waves.front(),wake)){s.done=true;continue;}
                s.running=true;s.wait=e.endDelay;event("BG CLOSE",i);
                // A newly entered source state starts its timers on this tick.
                if(!e.waves.empty())continue;
            }
            if(!e.waves.empty()&&s.wave+1<e.waves.size()){
                const auto& wave=e.waves[s.wave];s.age+=dt;
                bool cleared=true;
                for(unsigned w=0;w<=s.wave;++w)for(const auto& spawn:e.waves[w].spawns)
                    if(health(spawn.owner)>0)cleared=false;
                const int waveHealth=wave.healthOwner?health(wave.healthOwner):-1;
                const bool hp=waveHealth>=0&&waveHealth<wave.healthBelow;
                if(!s.advancing&&(cleared||hp||(wave.timeout>0&&s.age>=wave.timeout))){
                    s.advancing=true;s.pause=e.waves[s.wave+1].delay;
                    if(s.pause>0)continue;
                }
                if(s.advancing){
                    s.pause-=dt;
                    if(s.pause<=0){
                        // A failed allocation/activation opens the exit but must
                        // never persist a victory the player did not earn.
                        if(!startWave(e.waves[s.wave+1],wake)){closed_=false;s.done=true;s.running=false;continue;}
                        ++s.wave;s.age=0;s.advancing=false;
                    }
                }
                continue;
            }
            if(!remaining){s.wait-=dt;if(s.wait<=0)event("BG OPEN",i);}
        }
    }
private:
    template<class Wake> static bool startWave(const SceneBattleData::Wave& wave,Wake wake){
        for(const auto& spawn:wave.spawns)if(!wake(spawn))return false;
        return true;
    }
    struct State {bool running=false,done=false,advancing=false;float wait=0,age=0,pause=0;unsigned wave=0;};
    SceneBattleData data_;
    std::vector<State> states_;
    SceneBoolSave* save_=nullptr;
    bool closed_=false;
};
}
