#pragma once
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace hk3ds {
// Borrowed identities and authored numbers belong to room data. The save owner
// commits the integer addition and unlock together; this controller owns no save fields or colliders.
struct JijiKeyDoorRules {
    const char* keyName=nullptr;
    const char* unlockName=nullptr;
    float boxUpWait=0,choiceUpWait=0,declineWait=0,boxDownWait=0;
    float yesWait=0,activateWait=0,anticWait=0,slamEndWait=0;
    float anticTweenTime=0,anticDeltaY=0,slamTweenTime=0,slamDeltaY=0;
    int decrement=0;
};
struct JijiKeyDoorSaveHooks {
    void* context=nullptr;
    int (*readInt)(void*,const char*)=nullptr;
    bool (*readBool)(void*,const char*)=nullptr;
    // Apply key delta then set unlock, or refuse without changing either value.
    bool (*commitUnlock)(void*,const char*,int,const char*)=nullptr;
};
enum class JijiKeyDoorActionKind {
    DisableEntrance,HideSprite,DisableInteractionTrigger,BoxUp,ChoiceBoxUp,
    NoKeyDialogue,KeyDialogue,ChoiceBoxDown,TalkFinish,BoxDown,NpcTitleDown,
    ConversationEnd,ResetConversation,AnticTween,SlamTween,EnableEntrance
};
struct JijiKeyDoorAction {
    JijiKeyDoorActionKind kind{};
    float deltaY=0,duration=0;
};
struct JijiKeyDoorEvents {
    std::array<JijiKeyDoorAction,4> actions{};
    unsigned count=0;
    void add(JijiKeyDoorActionKind kind,float deltaY=0,float duration=0){
        assert(count<actions.size());actions[count++]={kind,deltaY,duration};
    }
};
// Town Jiji door interaction subset. The adapter supplies CONVO START,
// CONVO_FINISH, YES/NO and the Slam tween's authored FINISHED event. Dialogue,
// tween rendering and effects remain adapter responsibilities.
class JijiKeyDoor {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Pause,Init,Inactive,Idle,BoxUp,NoKey,ChoiceBoxUp,
                      KeyDialogue,DeclinePause,BoxDown,Yes,Activate,Antic,Slam,
                      SlamEnd,Open};
    bool bind(JijiKeyDoorRules rules,JijiKeyDoorSaveHooks save)
    {
        if(!rules.keyName||!*rules.keyName||!rules.unlockName||!*rules.unlockName||
           !save.readInt||!save.readBool||!save.commitUnlock)return false;
        const float waits[]={rules.boxUpWait,rules.choiceUpWait,rules.declineWait,
            rules.boxDownWait,rules.yesWait,rules.activateWait,rules.anticWait,
            rules.slamEndWait,rules.anticTweenTime,rules.slamTweenTime};
        for(float wait:waits)if(!std::isfinite(wait)||wait<0)return false;
        if(!std::isfinite(rules.anticDeltaY)||!std::isfinite(rules.slamDeltaY))return false;
        JijiKeyDoor next;next.rules_=rules;next.save_=save;
        next.phase_=Phase::Pause;*this=next;return true;
    }
    void clear(){*this={};}
    Phase phase()const{return phase_;}
    float slamEndElapsed()const{return phase_==Phase::SlamEnd?rules_.slamEndWait-remaining_:0;}
    JijiKeyDoorEvents conversationStart()
    {
        JijiKeyDoorEvents events;
        if(phase_!=Phase::Idle)return events;
        if(save_.readInt(save_.context,rules_.keyName)<=0){
            wait(Phase::BoxUp,rules_.boxUpWait);
            events.add(JijiKeyDoorActionKind::BoxUp);
        }else{
            wait(Phase::ChoiceBoxUp,rules_.choiceUpWait);
            events.add(JijiKeyDoorActionKind::ChoiceBoxUp);
        }
        return events;
    }
    JijiKeyDoorEvents conversationFinished()
    {
        JijiKeyDoorEvents events;
        if(phase_==Phase::NoKey){
            events.add(JijiKeyDoorActionKind::TalkFinish);boxDown(events);
        }
        return events;
    }
    JijiKeyDoorEvents choice(bool yes)
    {
        JijiKeyDoorEvents events;
        if(phase_==Phase::KeyDialogue){
            events.add(JijiKeyDoorActionKind::ChoiceBoxDown);
            wait(yes?Phase::Yes:Phase::DeclinePause,yes?rules_.yesWait:rules_.declineWait);
        }
        return events;
    }
    JijiKeyDoorEvents slamFinished()
    {
        JijiKeyDoorEvents events;
        if(phase_==Phase::Slam)wait(Phase::SlamEnd,rules_.slamEndWait);
        return events;
    }
    // One call is one frame; Pause's NextFrameEvent runs Init even at dt=0.
    // Timed states use scaled dt and never spend excess time in the next state.
    // Slam has no clock transition: only its external FINISHED may advance it.
    JijiKeyDoorEvents update(float dt)
    {
        JijiKeyDoorEvents events;
        if(!std::isfinite(dt)||dt<0)return events;
        if(phase_==Phase::Pause){
            phase_=Phase::Init;
            if(save_.readBool(save_.context,rules_.unlockName)){
                phase_=Phase::Inactive;
                events.add(JijiKeyDoorActionKind::HideSprite);
                events.add(JijiKeyDoorActionKind::DisableInteractionTrigger);
                // Inactive does not touch the authored entrance collider.
            }else{
                phase_=Phase::Idle;events.add(JijiKeyDoorActionKind::DisableEntrance);
            }
            return events;
        }
        switch(phase_){
        case Phase::BoxUp:case Phase::ChoiceBoxUp:case Phase::DeclinePause:
        case Phase::BoxDown:case Phase::Yes:case Phase::Activate:
        case Phase::Antic:case Phase::SlamEnd:break;
        default:return events;
        }
        remaining_-=dt;if(remaining_>0)return events;
        switch(phase_){
        case Phase::BoxUp:
            phase_=Phase::NoKey;events.add(JijiKeyDoorActionKind::NoKeyDialogue);break;
        case Phase::ChoiceBoxUp:
            phase_=Phase::KeyDialogue;events.add(JijiKeyDoorActionKind::KeyDialogue);break;
        case Phase::DeclinePause:
            events.add(JijiKeyDoorActionKind::TalkFinish);boxDown(events);break;
        case Phase::BoxDown:
            phase_=Phase::Idle;
            events.add(JijiKeyDoorActionKind::ConversationEnd);
            events.add(JijiKeyDoorActionKind::ResetConversation);break;
        case Phase::Yes:
            if(!save_.commitUnlock(save_.context,rules_.keyName,rules_.decrement,rules_.unlockName))break;
            wait(Phase::Activate,rules_.activateWait);
            events.add(JijiKeyDoorActionKind::DisableInteractionTrigger);
            events.add(JijiKeyDoorActionKind::ConversationEnd);break;
        case Phase::Activate:
            wait(Phase::Antic,rules_.anticWait);
            events.add(JijiKeyDoorActionKind::AnticTween,rules_.anticDeltaY,rules_.anticTweenTime);break;
        case Phase::Antic:
            phase_=Phase::Slam;
            events.add(JijiKeyDoorActionKind::SlamTween,rules_.slamDeltaY,rules_.slamTweenTime);break;
        case Phase::SlamEnd:
            phase_=Phase::Open;events.add(JijiKeyDoorActionKind::EnableEntrance);break;
        default:break;
        }
        return events;
    }
private:
    void wait(Phase phase,float seconds){phase_=phase;remaining_=seconds;}
    void boxDown(JijiKeyDoorEvents& events){
        wait(Phase::BoxDown,rules_.boxDownWait);
        events.add(JijiKeyDoorActionKind::BoxDown);
        events.add(JijiKeyDoorActionKind::NpcTitleDown);
    }
    JijiKeyDoorRules rules_{};
    JijiKeyDoorSaveHooks save_{};
    float remaining_=0;
    Phase phase_=Phase::Unbound;
};
// Fixed state and returned event storage, no per-frame allocation or textures.
static_assert(sizeof(JijiKeyDoor)<=192,"New 3DS Jiji controller budget");
}
