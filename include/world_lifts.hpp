#pragma once
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace hk3ds {
// PlayerData storage belongs to the save owner. Names and numeric rules come
// from private room data; this module allocates neither save bits nor geometry.
struct LiftPlayerBoolHooks {
    void* context=nullptr;
    bool (*read)(void*,const char*)=nullptr;
    // A refused persistent write must leave the cage closed and retryable.
    bool (*write)(void*,const char*,bool)=nullptr;
};
struct MineLiftRules {
    float bottom=0,top=0,speed=0,departureWait=0,arrivalWait=0;
};
enum class MineLiftActionKind {
    DeactivateCarry,ActivateCarry,HideClosedCage,ShowOpenCage,
    DepartureBob,StartUp,StartDown,TranslateY,Stop,SetY,ArrivalBob
};
struct MineLiftAction {MineLiftActionKind kind{};float value=0;};
struct MineLiftEvents {
    std::array<MineLiftAction,6> actions{};
    unsigned count=0;
    void add(MineLiftActionKind kind,float value=0){
        assert(count<actions.size());actions[count++]={kind,value};
    }
};
// Town Mines Exit Lift / Lift Control subset. The adapter sends authored OPEN,
// collision-enter and SEND COLLIDE events, and applies these actions in order.
// In particular, arrival detaches the hero before snapping the lift endpoint.
class MineExitLift {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Pause,Inactive,Init,Bottom,Top,Departure,
                      MovingUp,MovingDown,ArrivingTop,ArrivingBottom};
    bool bind(MineLiftRules rules,LiftPlayerBoolHooks save,const char* key,float initialY)
    {
        if(!save.read||!save.write||!key||!*key||!std::isfinite(initialY)||
           !std::isfinite(rules.bottom)||!std::isfinite(rules.top)||
           !std::isfinite(rules.speed)||!std::isfinite(rules.departureWait)||
           !std::isfinite(rules.arrivalWait)||rules.bottom>=rules.top||rules.speed<=0||
           rules.departureWait<0||rules.arrivalWait<0||!std::isfinite(rules.top-rules.bottom))return false;
        MineExitLift next;next.rules_=rules;next.save_=save;next.key_=key;
        next.y_=initialY;next.phase_=Phase::Pause;*this=next;return true;
    }
    void clear(){*this={};}
    Phase phase()const{return phase_;}
    float y()const{return y_;}
    MineLiftEvents heroPositioned(float heroY)
    {
        MineLiftEvents events;
        if(phase_!=Phase::Pause||!std::isfinite(heroY))return events;
        events.add(MineLiftActionKind::DeactivateCarry);
        if(save_.read(save_.context,key_)){
            events.add(MineLiftActionKind::HideClosedCage);initialize(heroY,events);
        }else phase_=Phase::Inactive;
        return events;
    }
    MineLiftEvents open(float heroY)
    {
        MineLiftEvents events;
        if(phase_!=Phase::Inactive||!std::isfinite(heroY))return events;
        if(!save_.write(save_.context,key_,true))return events;
        events.add(MineLiftActionKind::HideClosedCage);
        events.add(MineLiftActionKind::ShowOpenCage);
        initialize(heroY,events);return events;
    }
    // dt is scaled frame time. New departure/arrival waits start on the next
    // frame; Update translation is separate from the hero physics adapter.
    MineLiftEvents update(float dt,float heroY,bool heroCollisionEnter=false,bool sendCollide=false)
    {
        MineLiftEvents events;
        if(!std::isfinite(dt)||dt<0||!std::isfinite(heroY))return events;
        if(phase_==Phase::Bottom||phase_==Phase::Top){
            idle(heroY,heroCollisionEnter,events);
        }else if(phase_==Phase::Departure){
            remaining_-=dt;
            if(remaining_<=0)startMoving(fromBottom_,events);
        }else if(phase_==Phase::ArrivingTop||phase_==Phase::ArrivingBottom){
            remaining_-=dt;
            if(remaining_<=0){
                phase_=phase_==Phase::ArrivingTop?Phase::Top:Phase::Bottom;
                idle(heroY,false,events);
            }
        }else if(phase_==Phase::MovingDown&&sendCollide){
            // Direct FSM transition: no bob, wait, snap or carry detachment.
            startMoving(true,events);
        }else if(phase_==Phase::MovingUp||phase_==Phase::MovingDown){
            const bool up=phase_==Phase::MovingUp;
            const float delta=(up?rules_.speed:-rules_.speed)*dt;
            if(!std::isfinite(delta)||!std::isfinite(y_+delta))return events;
            y_+=delta;events.add(MineLiftActionKind::TranslateY,delta);
            checkArrival(up,events);
        }
        return events;
    }
private:
    float midpoint()const{return rules_.bottom+(rules_.top-rules_.bottom)*.5f;}
    void idle(float heroY,bool collision,MineLiftEvents& events)
    {
        const bool bottom=phase_==Phase::Bottom;
        if(collision||(bottom?heroY>midpoint():heroY<midpoint())){
            fromBottom_=bottom;phase_=Phase::Departure;remaining_=rules_.departureWait;
            events.add(MineLiftActionKind::DepartureBob);
        }
    }
    void initialize(float heroY,MineLiftEvents& events)
    {
        phase_=Phase::Init;
        // FloatCompare tolerance is zero. Its equality event is unconnected;
        // Init samples hero Y once, so subsequent motion cannot resolve it.
        if(heroY<midpoint()){phase_=Phase::Bottom;y_=rules_.bottom;}
        else if(heroY>midpoint()){phase_=Phase::Top;y_=rules_.top;}
        else return;
        events.add(MineLiftActionKind::SetY,y_);
    }
    void startMoving(bool up,MineLiftEvents& events)
    {
        phase_=up?Phase::MovingUp:Phase::MovingDown;
        events.add(MineLiftActionKind::ActivateCarry);
        events.add(up?MineLiftActionKind::StartUp:MineLiftActionKind::StartDown);
        checkArrival(up,events); // GetPosition/FloatCompare also run on entry.
    }
    void checkArrival(bool up,MineLiftEvents& events)
    {
        if(up?y_<rules_.top:y_>rules_.bottom)return;
        events.add(MineLiftActionKind::DeactivateCarry);
        events.add(MineLiftActionKind::Stop);
        y_=up?rules_.top:rules_.bottom;
        events.add(MineLiftActionKind::SetY,y_);
        events.add(MineLiftActionKind::ArrivalBob);
        remaining_=rules_.arrivalWait;
        phase_=up?Phase::ArrivingTop:Phase::ArrivingBottom;
    }
    MineLiftRules rules_{};
    LiftPlayerBoolHooks save_{};
    const char* key_=nullptr; // Borrowed; must outlive the binding.
    float y_=0,remaining_=0;
    Phase phase_=Phase::Unbound;
    bool fromBottom_=false;
};
static_assert(sizeof(MineExitLift)<=128,"mine lift controller budget");
}
