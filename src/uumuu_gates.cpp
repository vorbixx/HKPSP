#include "uumuu_gates.hpp"

namespace hk3ds {
namespace {
using Phase=UumuuGatePhase;
using Event=UumuuGateEvent;
using GateClip=UumuuGateClip;
using Kind=UumuuGateAction::Kind;
}
void UumuuGates::reset(){states_={};adapter_={};}
void UumuuGates::bind(UumuuGateAdapter adapter){
    reset();adapter_=adapter;
    for(unsigned i=0;i<capacity;++i)
        enter(i,uumuu_gates_data::gates[i].startClosed?Phase::QuickClose:Phase::Init);
}
void UumuuGates::action(unsigned i,UumuuGateAction a){
    auto& s=states_[i];
    if(a.kind==Kind::Renderer)s.visible=a.enabled;
    else if(a.kind==Kind::Collider)s.collider=a.enabled;
    else if(a.kind==Kind::Animation)s.clip=a.clip;
    else if(a.kind==Kind::DestroySelf)s.destroyPending=true;
    if(adapter_.action)adapter_.action(adapter_.context,i,a);
}
void UumuuGates::enter(unsigned i,Phase phase){
    states_[i].phase=phase;
    const auto renderer=[&](bool enabled){action(i,{Kind::Renderer,GateClip::None,enabled});};
    const auto collider=[&](bool enabled){action(i,{Kind::Collider,GateClip::None,enabled});};
    const auto animation=[&](GateClip clip){action(i,{Kind::Animation,clip});};
    switch(phase){
    case Phase::Init:collider(false);renderer(false);break;
    case Phase::Close:renderer(true);animation(GateClip::Close);collider(true);break;
    // Open's disabled SetCollider action must not release the passage. Only
    // the real Open clip's FINISHED enters Opened and disables collision.
    case Phase::Open:animation(GateClip::Open);break;
    case Phase::QuickClose:collider(true);animation(GateClip::Closed);renderer(true);break;
    case Phase::Destroy:action(i,{Kind::DestroySelf});break;
    case Phase::QuickOpen:animation(GateClip::Opened);collider(false);break;
    case Phase::Opened:collider(false);break;
    case Phase::Absent:break;
    }
}
bool UumuuGates::send(unsigned i,Event event){
    if(i>=capacity||states_[i].phase==Phase::Absent)return false;
    if(event==Event::Destroy){enter(i,Phase::Destroy);return true;}
    if(event==Event::QuickOpen){enter(i,Phase::QuickOpen);return true;}
    const auto p=states_[i].phase;
    if(p==Phase::Init||p==Phase::Open||p==Phase::QuickOpen||p==Phase::Opened){
        if(event==Event::Close){enter(i,Phase::Close);return true;}
        if(event==Event::QuickClose){enter(i,Phase::QuickClose);return true;}
    }
    if((p==Phase::Close||p==Phase::QuickClose)&&event==Event::Open){enter(i,Phase::Open);return true;}
    return false;
}
void UumuuGates::broadcast(Event event){for(unsigned i=0;i<capacity;++i)send(i,event);}
void UumuuGates::battleEvents(const UumuuBattleEvents& e){
    if(e.quickOpenGates)broadcast(Event::QuickOpen);
    if(e.closeGates)broadcast(Event::Close);
    if(e.openGates)broadcast(Event::Open);
}
bool UumuuGates::animationFinished(unsigned i){
    if(i>=capacity||states_[i].phase!=Phase::Open)return false;
    enter(i,Phase::Opened);return true;
}
bool UumuuGates::destroyed(unsigned i){
    if(i>=capacity||!states_[i].destroyPending)return false;
    states_[i]={};return true;
}
const char* UumuuGates::clipName(GateClip clip){
    using namespace uumuu_gates_data;
    switch(clip){
    case GateClip::Close:return closeClip;
    case GateClip::Closed:return closedClip;
    case GateClip::Open:return openClip;
    case GateClip::Opened:return openedClip;
    case GateClip::None:return "";
    }
    return "";
}
}
