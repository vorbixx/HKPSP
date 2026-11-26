#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
// Fungus2_21 level186 FSM5567 and FSM5613. Room data supplies every duration
// and displacement; save ownership includes the source death-respawn change.
struct CityCrestRules {
    float boxUp=0,choiceUp=0,decline=0,boxDown=0,yes=0,activate=0;
    float firstDelta=0,firstTime=0,firstWait=0,secondDelta=0,secondTime=0;
    float bumpDelta=0,bumpDown=0,bumpUp=0,dropDelta=0,dropTime=0;
    float restoredY=0,slamY=0,slamSpeed=0;
    bool load(const char* path){
        if(!path)return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;
        CityCrestRules next;char magic[4];
        const bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CCR1",4)&&
            std::fread(&next,sizeof(next),1,f)==1&&std::fgetc(f)==EOF&&next.valid();
        std::fclose(f);if(ok)*this=next;return ok;
    }
    bool valid()const {
        const float waits[]={boxUp,choiceUp,decline,boxDown,yes,activate};
        for(float x:waits)if(!std::isfinite(x)||x<0)return false;
        const float times[]={firstTime,firstWait,secondTime,bumpDown,bumpUp,dropTime,slamSpeed};
        for(float x:times)if(!std::isfinite(x)||x<=0)return false;
        const float positions[]={firstDelta,secondDelta,bumpDelta,dropDelta,restoredY,slamY};
        for(float x:positions)if(!std::isfinite(x))return false;
        return firstDelta>0&&secondDelta>0&&bumpDelta<0&&dropDelta<0&&
            firstWait>=firstTime&&std::abs(restoredY-firstDelta-secondDelta)<.0001f;
    }
};
static_assert(sizeof(CityCrestRules)==19*4,"CCR1 contains exactly nineteen source floats");
struct CityCrestSaveHooks {
    void* context=nullptr;
    bool (*hasCrest)(void*)=nullptr;
    bool (*opened)(void*)=nullptr;
    bool (*closed)(void*)=nullptr;
    // Consume hasCityKey and set openedCityGate together, or change nothing.
    bool (*insert)(void*)=nullptr;
    // Set cityGateClosed AND the authored death respawn together. The adapter
    // also applies SetMapZoneToSpecific(CITY); the controller owns no save bits.
    bool (*slam)(void*)=nullptr;
};
enum class CityCrestAction {BoxUp,ChoiceUp,NoKeyText,KeyText,ChoiceDown,BoxDown,
    ConversationEnd,Activated,GateOpen};
struct CityCrestEvents {
    std::array<CityCrestAction,3> values{};unsigned count=0;
    void add(CityCrestAction a){assert(count<values.size());values[count++]=a;}
};
class CityCrestSocket {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,BoxUp,ChoiceUp,NoKey,Choice,Decline,Yes,Activate,BoxDown,Inactive};
    bool bind(CityCrestRules rules,CityCrestSaveHooks save){
        if(!rules.valid()||!save.hasCrest||!save.opened||!save.closed||!save.insert||!save.slam)return false;
        clear();rules_=rules;save_=save;phase_=save.opened(save.context)?Phase::Inactive:Phase::Idle;return true;
    }
    void clear(){*this={};}
    Phase phase()const{return phase_;}
    bool locked()const{return phase_!=Phase::Unbound&&phase_!=Phase::Idle&&phase_!=Phase::Inactive;}
    CityCrestEvents begin(){
        CityCrestEvents e;if(phase_!=Phase::Idle)return e;
        const bool key=save_.hasCrest(save_.context);
        wait(key?Phase::ChoiceUp:Phase::BoxUp,key?rules_.choiceUp:rules_.boxUp);
        e.add(key?CityCrestAction::ChoiceUp:CityCrestAction::BoxUp);return e;
    }
    CityCrestEvents choose(bool yes){
        CityCrestEvents e;if(phase_!=Phase::Choice)return e;
        e.add(CityCrestAction::ChoiceDown);wait(yes?Phase::Yes:Phase::Decline,yes?rules_.yes:rules_.decline);return e;
    }
    CityCrestEvents finish(){
        CityCrestEvents e;if(phase_==Phase::NoKey)close(e);return e;
    }
    CityCrestEvents update(float dt){
        CityCrestEvents e;if(!std::isfinite(dt)||dt<0)return e;
        if(phase_!=Phase::BoxUp&&phase_!=Phase::ChoiceUp&&phase_!=Phase::Yes&&phase_!=Phase::Decline&&
           phase_!=Phase::Activate&&phase_!=Phase::BoxDown)return e;
        remaining_-=dt;if(remaining_>0)return e;
        switch(phase_){
        case Phase::BoxUp:phase_=Phase::NoKey;e.add(CityCrestAction::NoKeyText);break;
        case Phase::ChoiceUp:phase_=Phase::Choice;e.add(CityCrestAction::KeyText);break;
        case Phase::Decline:close(e);break;
        case Phase::Yes:
            // Failed admission remains retryable and emits no opening event.
            if(save_.insert(save_.context)){wait(Phase::Activate,rules_.activate);e.add(CityCrestAction::Activated);}
            break;
        case Phase::Activate:e.add(CityCrestAction::GateOpen);close(e);break;
        case Phase::BoxDown:phase_=save_.opened(save_.context)?Phase::Inactive:Phase::Idle;
            e.add(CityCrestAction::ConversationEnd);break;
        default:break;
        }return e;
    }
private:
    void wait(Phase phase,float seconds){phase_=phase;remaining_=seconds;}
    void close(CityCrestEvents& e){wait(Phase::BoxDown,rules_.boxDown);e.add(CityCrestAction::BoxDown);}
    CityCrestRules rules_{};CityCrestSaveHooks save_{};Phase phase_=Phase::Unbound;float remaining_=0;
};
struct CityCrestSounds {
    unsigned open=0,bump=0,drop=0,slam=0;
    std::array<float,4> y{};
    bool moving=false;
};
class CityCrestDoor {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Closed,First,Second,BumpDown,BumpUp,Open,Drop,Slam,Slammed};
    bool bind(CityCrestRules rules,CityCrestSaveHooks save){
        if(!rules.valid()||!save.opened||!save.closed||!save.slam)return false;
        clear();rules_=rules;save_=save;
        // FSM5613 Check tests cityGateClosed before openedCityGate.
        const bool opened=!save.closed(save.context)&&save.opened(save.context);
        phase_=opened?Phase::Open:Phase::Closed;y_=opened?rules.restoredY:0;return true;
    }
    void clear(){*this={};}
    Phase phase()const{return phase_;}
    float y()const{return y_;}
    CityCrestSounds sounds()const{auto s=sounds_;s.moving=phase_==Phase::First||phase_==Phase::Second;return s;}
    bool open(){if(phase_!=Phase::Closed)return false;enter(Phase::First);return true;}
    bool drop(){if(phase_!=Phase::Open)return false;enter(Phase::Drop);return true;}
    bool slam(){
        if(phase_==Phase::Unbound||phase_==Phase::Closed||phase_==Phase::Slam||phase_==Phase::Slammed)return false;
        if(!save_.slam(save_.context))return false;
        enter(Phase::Slam);return true;
    }
    void update(float dt){
        if(!std::isfinite(dt)||dt<0)return;
        age_+=dt;float delta=0,time=0,wait=0;Phase next=phase_;bool inward=false;
        switch(phase_){
        case Phase::First:delta=rules_.firstDelta;time=rules_.firstTime;wait=rules_.firstWait;next=Phase::Second;break;
        case Phase::Second:delta=rules_.secondDelta;time=wait=rules_.secondTime;inward=true;next=Phase::BumpDown;break;
        case Phase::BumpDown:delta=rules_.bumpDelta;time=wait=rules_.bumpDown;next=Phase::BumpUp;break;
        case Phase::BumpUp:delta=-rules_.bumpDelta;time=wait=rules_.bumpUp;inward=true;next=Phase::Open;break;
        case Phase::Drop:delta=rules_.dropDelta;time=rules_.dropTime;break;
        case Phase::Slam:{
            const float travel=rules_.slamSpeed*dt;
            y_=y_>rules_.slamY?std::max(rules_.slamY,y_-travel):std::min(rules_.slamY,y_+travel);
            if(y_==rules_.slamY)enter(Phase::Slammed);
            return;
        }
        default:return;
        }
        const float p=std::min(1.f,age_/time),angle=p*1.5707963267948966f;
        y_=from_+delta*(inward?1-std::cos(angle):std::sin(angle));
        // PlayMaker changes states on a frame boundary; do not spend overflow
        // in a second state or complete a fresh tween in the same update.
        if(wait>0&&age_>=wait)enter(next);
    }
private:
    void enter(Phase phase){
        phase_=phase;age_=0;from_=y_;
        if(phase==Phase::First){++sounds_.open;sounds_.y[0]=y_;}
        if(phase==Phase::BumpDown){++sounds_.bump;sounds_.y[1]=y_;}
        if(phase==Phase::Drop){++sounds_.drop;sounds_.y[2]=y_;}
        if(phase==Phase::Slammed){++sounds_.slam;sounds_.y[3]=y_;}
    }
    CityCrestSounds sounds_{};
    CityCrestRules rules_{};CityCrestSaveHooks save_{};Phase phase_=Phase::Unbound;float y_=0,from_=0,age_=0;
};
}
