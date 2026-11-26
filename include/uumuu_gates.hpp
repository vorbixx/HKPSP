#pragma once
#include "uumuu.hpp"
#include "uumuu_gates_data.hpp"
#include <array>

namespace hk3ds {
enum class UumuuGatePhase { Absent,Init,Close,Open,QuickClose,Destroy,QuickOpen,Opened };
enum class UumuuGateEvent { Close,Open,QuickClose,QuickOpen,Destroy };
enum class UumuuGateClip { None,Close,Closed,Open,Opened };
struct UumuuGateAction {
    enum class Kind { Renderer,Animation,Collider,DestroySelf };
    Kind kind;
    UumuuGateClip clip=UumuuGateClip::None;
    bool enabled=false;
};
struct UumuuGateAdapter {
    void* context=nullptr;
    // Synchronous, ordered source actions; the receiver must not reenter this
    // controller. Animation completion and actual destruction are callbacks.
    void (*action)(void*,unsigned,const UumuuGateAction&)=nullptr;
};
// The three overlay-owned BG Control FSMs. Loading/mesh/animation ownership
// stays with the room. No animation duration or destruction delay is inferred.
class UumuuGates {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=uumuu_gates_data::gates.size();
    void bind(UumuuGateAdapter={});
    void reset();
    bool send(unsigned,UumuuGateEvent);
    void broadcast(UumuuGateEvent);
    void battleEvents(const UumuuBattleEvents&);
    bool animationFinished(unsigned);
    bool destroyed(unsigned);
    UumuuGatePhase phase(unsigned i)const{return i<capacity?states_[i].phase:UumuuGatePhase::Absent;}
    bool colliderEnabled(unsigned i)const{return i<capacity&&states_[i].collider;}
    bool visible(unsigned i)const{return i<capacity&&states_[i].visible;}
    bool destructionPending(unsigned i)const{return i<capacity&&states_[i].destroyPending;}
    UumuuGateClip clip(unsigned i)const{return i<capacity?states_[i].clip:UumuuGateClip::None;}
    static const char* clipName(UumuuGateClip);
    static const uumuu_gates_data::Gate* source(unsigned i){return i<capacity?&uumuu_gates_data::gates[i]:nullptr;}
private:
    struct State {
        UumuuGatePhase phase=UumuuGatePhase::Absent;
        UumuuGateClip clip=UumuuGateClip::None;
        bool collider=false,visible=false,destroyPending=false;
    };
    void enter(unsigned,UumuuGatePhase);
    void action(unsigned,UumuuGateAction);
    std::array<State,capacity> states_{};
    UumuuGateAdapter adapter_{};
};
static_assert(sizeof(UumuuGates)<=128,"Uumuu gate state exceeded its fixed storage estimate");
}
