#include "uumuu_quirrel.hpp"
#include "uumuu_quirrel_data.hpp"
#include "uumuu_quirrel_watch_data.hpp"
#include "uumuu_data.hpp"
#include <cmath>

namespace hk3ds {
namespace {
bool finite(Vec2 p){return std::isfinite(p.x)&&std::isfinite(p.y);}
bool validClip(const Clip* c){
    return c&&c->count&&std::isfinite(c->fps)&&c->fps>0&&c->wrap==2;
}
static_assert(sizeof(UumuuQuirrelActor)<=512,"Quirrel actor exceeded fixed-state estimate");
}
bool UumuuQuirrelActor::bind(UumuuEncounter& encounter,const AnimationBank& land,
                            const AnimationBank& slash,UumuuQuirrelAdapter adapter){
    using namespace uumuu_quirrel_data;
    const auto a=land.find(landArriveBankClip.data()),l=land.find(landLeaveBankClip.data());
    const auto w=land.find(landWatchBankClip.data());
    const auto q=slash.find(slashAnticBankClip.data()),s=slash.find(slashBankClip.data());
    if(!adapter.audio||!adapter.floatRange||!adapter.intRange||
       !validClip(a)||!validClip(l)||!validClip(w)||!validClip(q)||!validClip(s))return false;
    reset();encounter_=&encounter;adapter_=adapter;arrive_=a;leave_=l;antic_=q;slashClip_=s;
    watchClip_=w;
    land_.localPosition={landLocalX,landLocalY};land_.localZ=landLocalZ;
    land_.scaleX=landScaleX;land_.visible=landVisible;
    slash_.localPosition={slashLocalX,slashLocalY};slash_.localZ=slashLocalZ;
    slash_.scaleX=slashScaleX;slash_.visible=slashVisible;
    return true;
}
void UumuuQuirrelActor::reset(){
    encounter_=nullptr;adapter_={};arrive_=leave_=antic_=slashClip_=watchClip_=nullptr;
    land_={};slash_={};phase_=Phase::Hidden;faulted_=false;
    watchPhase_=WatchPhase::Idle;watchTimer_=0;
    previousWatchPosition_=uumuu_quirrel_watch_data::watchPositions.size();
    roam_=uumuu_quirrel_watch_data::initialRoam;
    voiceIntro_=uumuu_quirrel_watch_data::initialVoiceIntro;
}
Vec2 UumuuQuirrelActor::position(const UumuuQuirrelSprite& sprite)const{
    const auto p=encounter_->motion().position();
    return {p.x+sprite.localPosition.x,p.y+sprite.localPosition.y};
}
Vec2 UumuuQuirrelActor::renderLandPosition(PresentationTime t)const{
    if(!encounter_)return {};
    const auto p=encounter_->motion().renderPosition(t);
    return {p.x+land_.localPosition.x,p.y+land_.localPosition.y};
}
Vec2 UumuuQuirrelActor::renderSlashPosition(PresentationTime t)const{
    if(!encounter_)return {};
    const auto p=encounter_->motion().renderPosition(t);
    return {p.x+slash_.localPosition.x,p.y+slash_.localPosition.y};
}
void UumuuQuirrelActor::audio(UumuuQuirrelCue cue,Vec2 at){adapter_.audio(adapter_.context,cue,at);}
bool UumuuQuirrelActor::watchPause(){
    using namespace uumuu_quirrel_watch_data;
    const auto pause=adapter_.floatRange(adapter_.context,pauseMin,pauseMax);
    if(!std::isfinite(pause)||pause<pauseMin||pause>pauseMax){faulted_=true;return false;}
    watchPhase_=WatchPhase::Pause;watchTimer_=pause;return true;
}
bool UumuuQuirrelActor::watch(){
    if(!encounter_||faulted_||watchPhase_!=WatchPhase::Idle)return false;
    return watchPause();
}
bool UumuuQuirrelActor::watchPosition(){
    using namespace uumuu_quirrel_watch_data;
    const float sum=static_cast<float>(watchChoiceNames.size());
    float sample=adapter_.floatRange(adapter_.context,0,sum);
    if(!std::isfinite(sample)||sample<0||sample>sum){faulted_=true;return false;}
    // ActionHelpers.GetRandomWeightedIndex uses strict comparisons, including
    // a valid float Range upper endpoint that emits no event at all.
    unsigned choice=0;
    while(choice<watchChoiceNames.size()&&sample>=1){sample-=1;++choice;}
    if(choice==watchChoiceNames.size()){watchPhase_=WatchPhase::NoChoice;return true;}
    auto name=watchChoiceNames[choice];
    if(previousWatchPosition_<watchPositions.size()&&watchPositions[previousWatchPosition_].name==name)
        name=watchPositions[previousWatchPosition_].next;
    unsigned index=0;
    while(index<watchPositions.size()&&watchPositions[index].name!=name)++index;
    if(index==watchPositions.size()){faulted_=true;return false;}
    // The immediate NEXT transition aborts the repeated state's later actions.
    // Store and apply only the actual destination. Z and scale Y/Z are unset.
    previousWatchPosition_=index;
    const auto& pose=watchPositions[index];
    const auto parent=encounter_->motion().position();
    land_.localPosition={pose.x-parent.x,pose.y-parent.y};land_.scaleX=pose.scaleX;
    audio(UumuuQuirrelCue::WatchVoice,position(land_));
    if(!voiceIntro_){voiceIntro_=true;audio(UumuuQuirrelCue::WatchIntro,position(land_));}
    land_.animation.play(watchClip_,0,true);land_.visible=true;
    audio(UumuuQuirrelCue::WatchArrive,position(land_));
    watchPhase_=WatchPhase::Watching;watchTimer_=animationWait;return true;
}
bool UumuuQuirrelActor::controlUpdate(float dt){
    if(!encounter_||faulted_||!std::isfinite(dt)||dt<=0)return false;
    switch(watchPhase_){
    // Idle's BoolTest runs on state entry, never every frame. A later Roam
    // assignment needs a local WATCH event until the FSM enters Idle again.
    case WatchPhase::Idle:break;
    case WatchPhase::Pause:
        watchTimer_-=dt;if(watchTimer_<=0)return watchPosition();break;
    case WatchPhase::Watching:
        watchTimer_-=dt;
        if(watchTimer_<=0){
            audio(UumuuQuirrelCue::WatchLeave,position(land_));watchPhase_=WatchPhase::Leap;
        }
        break;
    case WatchPhase::Leap:
        // Tk2dWatchAnimationEvents.OnUpdate also catches an animation that
        // completed before Leap Out SFX subscribed to its completion event.
        if(land_.animation.complete){
            land_.visible=false;watchPhase_=WatchPhase::Idle;
            if(roam_)return watchPause();
        }
        break;
    case WatchPhase::NoChoice:break;
    }
    return true;
}
bool UumuuQuirrelActor::consume(){
    if(!encounter_||faulted_)return false;
    const auto watchEvents=encounter_->takeQuirrelEvents();
    if(watchEvents.roamChanged)roam_=watchEvents.roam;
    if(watchEvents.watch&&watchPhase_==WatchPhase::Idle&&!watchPause())return false;
    const auto e=encounter_->takeArrivalEvents();
    // Missed source callback boundaries must be visible to the room adapter.
    if(unsigned(e.arrive)+unsigned(e.leave)+unsigned(e.hide)+unsigned(e.slashAntic)>1){
        faulted_=true;return false;
    }
    if(e.arrive){
        audio(UumuuQuirrelCue::CheckPosition,position(land_));
        const auto parent=encounter_->motion().position();
        land_.localPosition={e.position.x-parent.x,e.position.y-parent.y};
        land_.scaleX=e.scaleX;land_.visible=true;
        audio(UumuuQuirrelCue::Arrive,position(slash_));
        land_.animation.play(arrive_,0,true);phase_=Phase::Arrive;
    }else if(e.leave){
        audio(UumuuQuirrelCue::Leave,position(slash_));
        land_.animation.play(leave_,0,true);phase_=Phase::Leave;
    }else if(e.hide){
        land_.visible=false;phase_=Phase::Hidden;
    }else if(e.slashAntic){
        audio(UumuuQuirrelCue::SlashVoice,position(slash_));
        audio(UumuuQuirrelCue::SlashAntic,position(slash_));
        // Preserve Unity's distinct float and exclusive-upper-bound integer
        // overloads, in source action order. Never synthesize a missing draw.
        const float rotation=adapter_.floatRange(adapter_.context,
            uumuu_data::quirrelSlashRotationMin,uumuu_data::quirrelSlashRotationMax);
        const int scale=adapter_.intRange(adapter_.context,0,100);
        UumuuSlashPose pose;
        if(!encounter_->prepareSlashAntic(rotation,scale,pose)){faulted_=true;return false;}
        slash_.rotationZ=pose.localRotationZ;slash_.scaleX=pose.scaleX;slash_.visible=true;
        slash_.animation.play(antic_,0,true);phase_=Phase::Antic;
    }
    return true;
}
bool UumuuQuirrelActor::animationUpdate(float dt,Vec2 hero){
    if(!encounter_||faulted_||!std::isfinite(dt)||dt<=0||!finite(hero))return false;
    const bool battleAnimation=encounter_->shellPhase()!=UumuuShellPhase::Dead&&!encounter_->completed();
    // Capture this callback's phase: completion cannot spend its elapsed time
    // on a different clip started synchronously by that completion.
    const auto previous=phase_;
    land_.animation.advance(dt);slash_.animation.advance(dt);
    if(watchPhase_==WatchPhase::Leap&&land_.animation.complete){
        land_.visible=false;watchPhase_=WatchPhase::Idle;
        if(roam_&&!watchPause())return false;
    }
    // HP death does not destroy the original child or finish its Watch FSM.
    // The room resets this borrowed actor when its parent object is removed.
    if(!battleAnimation)return true;
    if(previous==Phase::Leave&&land_.animation.complete){
        if(!encounter_->quirrelLeaveAnimationFinished(hero))return false;
        return consume();
    }
    if(previous==Phase::Antic&&slash_.animation.complete){
        if(encounter_->arrivalPhase()!=UumuuArrivalPhase::Ready||
           encounter_->shellPhase()!=UumuuShellPhase::Guarded)return false;
        slash_.animation.play(slashClip_,0,true);
        audio(UumuuQuirrelCue::Slash,position(slash_));
        audio(UumuuQuirrelCue::Wound,encounter_->motion().position());
        if(!encounter_->beginSlash(hero)){faulted_=true;return false;}
        phase_=Phase::Slash;
    }else if(previous==Phase::Slash&&slash_.animation.complete){
        if(!encounter_->slashAnimationFinished(hero))return false;
        slash_.visible=false;phase_=Phase::Finished;
    }
    return true;
}
}
