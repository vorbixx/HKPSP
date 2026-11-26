#pragma once
#include "world_key_door_room.hpp"
#include "world_quests.hpp"
#include "room_visual.hpp"
#include "room.hpp"

namespace hk3ds {
struct JijiDoorRoomData {
    WorldKeyDoorRoomData controller;
    char gate[32]{};unsigned owner=0;Rect trigger{};
    float x=0,walkOffset=0,walkSpeed=0;
    char noKey[768]{},key[768]{},yes[32]{},no[32]{},prompt[32]{};
    bool valid()const{
        auto text=[](const char* s,std::size_t n){return s[0]&&std::memchr(s,0,n);};
        return controller.valid()&&text(gate,sizeof(gate))&&owner&&
            std::isfinite(trigger.left)&&std::isfinite(trigger.bottom)&&std::isfinite(trigger.right)&&std::isfinite(trigger.top)&&
            trigger.left<trigger.right&&trigger.bottom<trigger.top&&std::isfinite(x)&&
            std::isfinite(walkOffset)&&walkOffset>=0&&std::isfinite(walkSpeed)&&walkSpeed>0&&
            text(noKey,sizeof(noKey))&&text(key,sizeof(key))&&text(yes,sizeof(yes))&&text(no,sizeof(no))&&text(prompt,sizeof(prompt));
    }
    bool load(const char* directory){
        if(!directory)return false;
        JijiDoorRoomData next;char path[512];
        if(std::snprintf(path,sizeof(path),"%s/jiji-controller.bin",directory)>=int(sizeof(path))||!next.controller.load(path))return false;
        if(std::snprintf(path,sizeof(path),"%s/jiji-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"JJR1",4)&&read(next.gate,32)&&read(&next.owner,4)&&
            read(&next.trigger,sizeof(Rect))&&read(&next.x,4)&&read(&next.walkOffset,4)&&read(&next.walkSpeed,4)&&
            read(next.noKey,768)&&read(next.key,768)&&read(next.yes,32)&&read(next.no,32)&&read(next.prompt,32);
        ok=ok&&std::fgetc(f)==EOF&&next.valid();std::fclose(f);if(ok)*this=next;return ok;
    }
};
// Source FSM4067 owns unlock timing; this adapter owns only input, dialogue and
// source iTween easeOutSine/easeInSine presentation. It never changes save schema.
class JijiDoorRuntime {
    friend struct SimulationStateHash;
public:
    bool bind(const JijiDoorRoomData& data,JijiKeyDoorSaveHooks save){
        if(!data.valid())return false;
        clear();data_=data;
        if(!data_.controller.bind(controller_,save)){clear();return false;}
        bound_=true;apply(controller_.update(0),nullptr);history_.reset(position_);return true;
    }
    void clear(){*this={};}
    bool reload(JijiKeyDoorSaveHooks save){
        if(!bound())return true;
        const auto data=data_;return bind(data,save);
    }
    bool bound()const{return bound_;}
    bool controlsHero()const{return interacting_;}
    JijiKeyDoor::Phase phase()const{return controller_.phase();}
    float slamEndElapsed()const{return controller_.slamEndElapsed();}
    bool matchesGate(const RoomGate& gate)const{return bound_&&!std::strcmp(gate.name,data_.gate);}
    bool gateEnabled(const RoomGate& gate)const{return !matchesGate(gate)||entrance_;}
    const char* heroClip(const WorldQuestDialogue& ui)const{return controlsHero()?ui.heroClip():nullptr;}
    void presentDraw(RoomDraw& draw,PresentationTime time)const{
        if(!bound_||draw.owner!=data_.owner)return;
        if(hidden_)draw.values[10]=0;
        draw.values[5]+=history_.sample(position_,time).y;
    }
    bool update(float dt,Hero& h,Rect body,WorldQuestDialogue& ui,bool allowed,float turnSeconds){
        if(!bound_||!std::isfinite(dt)||dt<0)return false;
        const PresentationPosition::Step capture(history_,position_);
        const bool up=h.input.y>.5f;
        const bool overlap=body.left<data_.trigger.right&&body.right>data_.trigger.left&&
            body.bottom<data_.trigger.top&&body.top>data_.trigger.bottom;
        if(interacting_&&!allowed){
            if(phase()==JijiKeyDoor::Phase::KeyDialogue)apply(controller_.choice(false),&ui);
            if(phase()==JijiKeyDoor::Phase::NoKey)apply(controller_.conversationFinished(),&ui);
            if(phase()==JijiKeyDoor::Phase::Idle){ui.reset();interacting_=false;}
        }
        if(interacting_){ui.step(dt);ui.observePosition(h.position.x);}
        ui.prompt=allowed&&overlap&&!ui.locked()&&!interacting_&&phase()==JijiKeyDoor::Phase::Idle;
        if(ui.prompt)std::snprintf(ui.promptLabel,sizeof(ui.promptLabel),"%s",data_.prompt);
        if(ui.prompt&&up&&!ui.upHeld){
            // Town npc_control4068 sets Hero Always Left, including approaches
            // from the right side of the inspection trigger.
            interacting_=true;ui.approachLeft(h.position.x,data_.x,data_.walkOffset,data_.walkSpeed,h.facingRight,turnSeconds);
        }
        if(interacting_||ui.prompt)ui.upHeld=up;
        if(interacting_&&!ui.moving()&&phase()==JijiKeyDoor::Phase::Idle)
            apply(controller_.conversationStart(),&ui);
        if(tween_){
            tweenAge_+=dt;const float p=tweenTime_>0?std::min(1.f,tweenAge_/tweenTime_):1;
            const float eased=slam_?1-std::cos(p*1.57079632679f):std::sin(p*1.57079632679f);
            position_.y=tweenFrom_+tweenDelta_*eased;
            if(p>=1){tween_=false;if(slam_)apply(controller_.slamFinished(),&ui);}
        }
        // Input is observed before stepping the FSM, so a freshly displayed
        // choice cannot consume the button which opened the dialogue.
        if(interacting_&&allowed&&ui.phase()==WorldQuestDialogue::Phase::Reading){
            const unsigned confirm=h.input.pressed&(Jump|Attack|Focus|Dash);
            if(phase()==JijiKeyDoor::Phase::NoKey&&confirm&&ui.proceed())apply(controller_.conversationFinished(),&ui);
            else if(phase()==JijiKeyDoor::Phase::KeyDialogue){
                if(!ui.pageComplete()){if(confirm)ui.proceed();}
                else{
                    ui.choiceVisible=true;
                    const int direction=h.input.x<-.5f?-1:h.input.x>.5f?1:0;
                    repeat_=std::max(0.f,repeat_-dt);
                    if(direction&&(direction!=direction_||repeat_<=0)){
                        ui.choiceYes=direction<0;repeat_=direction!=direction_?.25f:.15f;
                    }
                    direction_=direction;
                    if(confirm)apply(controller_.choice(ui.choiceYes),&ui);
                }
            }
        }
        apply(controller_.update(dt),interacting_?&ui:nullptr);
        if(interacting_&&ui.phase()==WorldQuestDialogue::Phase::Idle&&
           (phase()==JijiKeyDoor::Phase::Idle||phase()==JijiKeyDoor::Phase::Activate||phase()==JijiKeyDoor::Phase::Antic||
            phase()==JijiKeyDoor::Phase::Slam||phase()==JijiKeyDoor::Phase::SlamEnd||phase()==JijiKeyDoor::Phase::Open))interacting_=false;
        return interacting_;
    }
private:
    void apply(const JijiKeyDoorEvents& events,WorldQuestDialogue* ui){
        for(unsigned i=0;i<events.count;++i){const auto& a=events.actions[i];switch(a.kind){
        case JijiKeyDoorActionKind::DisableEntrance:entrance_=false;break;
        case JijiKeyDoorActionKind::EnableEntrance:entrance_=true;break;
        case JijiKeyDoorActionKind::HideSprite:hidden_=true;break;
        case JijiKeyDoorActionKind::BoxUp:case JijiKeyDoorActionKind::ChoiceBoxUp:if(ui)ui->open();break;
        case JijiKeyDoorActionKind::NoKeyDialogue:if(ui)ui->read(data_.noKey);break;
        case JijiKeyDoorActionKind::KeyDialogue:if(ui){ui->read(data_.key);ui->choiceYes=true;
            std::snprintf(ui->yesLabel,sizeof(ui->yesLabel),"%s",data_.yes);std::snprintf(ui->noLabel,sizeof(ui->noLabel),"%s",data_.no);}break;
        case JijiKeyDoorActionKind::ChoiceBoxDown:if(ui)ui->choiceVisible=false;break;
        case JijiKeyDoorActionKind::BoxDown:case JijiKeyDoorActionKind::ConversationEnd:if(ui)ui->close();break;
        case JijiKeyDoorActionKind::ResetConversation:if(ui)ui->reset();interacting_=false;break;
        case JijiKeyDoorActionKind::AnticTween:case JijiKeyDoorActionKind::SlamTween:
            tween_=true;slam_=a.kind==JijiKeyDoorActionKind::SlamTween;tweenFrom_=position_.y;
            tweenDelta_=a.deltaY;tweenTime_=a.duration;tweenAge_=0;break;
        default:break;
        }}
    }
    JijiDoorRoomData data_;JijiKeyDoor controller_;PresentationPosition history_;Vec2 position_{};
    float tweenAge_=0,tweenTime_=0,tweenFrom_=0,tweenDelta_=0,repeat_=0;
    int direction_=0;bool bound_=false,entrance_=true,hidden_=false,interacting_=false,tween_=false,slam_=false;
};
}
