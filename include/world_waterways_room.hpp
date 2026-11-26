#pragma once
#include "world_jiji_door.hpp"
#include "terrain.hpp"

namespace hk3ds {
// Ruins1_05b GO238/FSM2773 and GO147/FSM2763. The machine consumes one
// simple key, disables itself, waits, then broadcasts MANHOLE OPEN. The manhole
// removes collision on that event, independently of the opening animation.
struct WaterwaysRules {float boxUp=0,choiceUp=0,decline=0,boxDown=0,yes=0,activate=0;};
class WaterwaysMachine {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,BoxUp,NoKey,ChoiceUp,Choice,Decline,BoxDown,Yes,Activate,Open,Restored};
    bool bind(WaterwaysRules rules,JijiKeyDoorSaveHooks save,const char* key,const char* flag){
        if(!key||!*key||!flag||!*flag||!save.readInt||!save.readBool||!save.commitUnlock)return false;
        for(float t:{rules.boxUp,rules.choiceUp,rules.decline,rules.boxDown,rules.yes,rules.activate})if(!std::isfinite(t)||t<0)return false;
        rules_=rules;save_=save;key_=key;flag_=flag;phase_=save.readBool(save.context,flag)?Phase::Restored:Phase::Idle;return true;
    }
    Phase phase()const{return phase_;}
    bool consumed()const{return phase_==Phase::Activate||phase_==Phase::Open||phase_==Phase::Restored;}
    bool open()const{return phase_==Phase::Open||phase_==Phase::Restored;}
    void start(){if(phase_==Phase::Idle)wait(save_.readInt(save_.context,key_)>0?Phase::ChoiceUp:Phase::BoxUp,
        save_.readInt(save_.context,key_)>0?rules_.choiceUp:rules_.boxUp);}
    void finishText(){if(phase_==Phase::NoKey)wait(Phase::BoxDown,rules_.boxDown);}
    void choice(bool yes){if(phase_==Phase::Choice)wait(yes?Phase::Yes:Phase::Decline,yes?rules_.yes:rules_.decline);}
    void update(float dt){
        if(!std::isfinite(dt)||dt<0)return;
        switch(phase_){case Phase::BoxUp:case Phase::ChoiceUp:case Phase::Decline:case Phase::BoxDown:case Phase::Yes:case Phase::Activate:break;default:return;}
        remaining_-=dt;if(remaining_>0)return;
        switch(phase_){
        case Phase::BoxUp:phase_=Phase::NoKey;break;
        case Phase::ChoiceUp:phase_=Phase::Choice;break;
        case Phase::Decline:wait(Phase::BoxDown,rules_.boxDown);break;
        case Phase::BoxDown:phase_=Phase::Idle;break;
        case Phase::Yes:if(save_.commitUnlock(save_.context,key_,-1,flag_))wait(Phase::Activate,rules_.activate);break;
        case Phase::Activate:phase_=Phase::Open;break;
        default:break;
        }
    }
private:
    void wait(Phase p,float t){phase_=p;remaining_=t;}
    WaterwaysRules rules_;JijiKeyDoorSaveHooks save_;const char* key_=nullptr;const char* flag_=nullptr;
    Phase phase_=Phase::Unbound;float remaining_=0;
};
struct WaterwaysRoomData {
    WaterwaysRules rules;char key[64]{},flag[64]{};
    Rect trigger{};float x=0,walkOffset=0,walkSpeed=0;
    unsigned machineOwner=0,manholeOwner=0,machineCollider=0,manholeCollider=0;
    char noKey[768]{},keyText[768]{},yes[32]{},no[32]{},prompt[32]{};
    std::array<RoomDraw,4> draws{};std::array<unsigned,4> sprites{};
    bool valid()const{
        auto text=[](const char* s,std::size_t n){return s[0]&&std::memchr(s,0,n);};
        if(!text(key,64)||!text(flag,64)||!text(noKey,768)||!text(keyText,768)||!text(yes,32)||!text(no,32)||!text(prompt,32)||
           !machineOwner||!manholeOwner||!machineCollider||!manholeCollider||machineCollider==manholeCollider||
           !std::isfinite(x)||!std::isfinite(walkOffset)||walkOffset<0||!std::isfinite(walkSpeed)||walkSpeed<=0)return false;
        for(float t:{rules.boxUp,rules.choiceUp,rules.decline,rules.boxDown,rules.yes,rules.activate})if(!std::isfinite(t)||t<0)return false;
        for(float v:{trigger.left,trigger.bottom,trigger.right,trigger.top})if(!std::isfinite(v))return false;
        if(trigger.left>=trigger.right||trigger.bottom>=trigger.top)return false;
        for(const auto& d:draws){if(!d.owner)return false;for(float v:d.values)if(!std::isfinite(v))return false;}
        return true;
    }
    bool load(const char* directory){
        if(!directory)return false;
        char path[512];
        if(std::snprintf(path,sizeof(path),"%s/waterways-machine.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;WaterwaysRoomData next;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"WWM1",4)&&read(&next.rules,sizeof(WaterwaysRules))&&
            read(next.key,64)&&read(next.flag,64)&&read(&next.trigger,sizeof(Rect))&&read(&next.x,4)&&read(&next.walkOffset,4)&&read(&next.walkSpeed,4)&&
            read(&next.machineOwner,4)&&read(&next.manholeOwner,4)&&read(&next.machineCollider,4)&&read(&next.manholeCollider,4)&&
            read(next.noKey,768)&&read(next.keyText,768)&&read(next.yes,32)&&read(next.no,32)&&read(next.prompt,32)&&read(next.sprites.data(),16);
        for(auto& d:next.draws)ok=ok&&read(&d.owner,4)&&read(d.values.data(),64);
        ok=ok&&std::fgetc(f)==EOF&&next.valid();std::fclose(f);if(ok)*this=next;return ok;
    }
};
struct WaterwaysArtAssets {
    AnimationBank art;
    bool load(const char* directory){char path[512];return directory&&
        std::snprintf(path,sizeof(path),"%s/waterways-art.bin",directory)<int(sizeof(path))&&art.load(path);}
};
class WaterwaysRoom {
    friend struct SimulationStateHash;
public:
    bool bind(const WaterwaysRoomData& data,JijiKeyDoorSaveHooks save,TerrainBank& terrain){
        if(!data.valid())return false;
        // Source BoxCollider1807 is an interaction trigger, represented by
        // data.trigger. Only the manhole's solid BoxCollider1800 belongs in
        // TerrainBank; requiring the trigger there rejects the authored room.
        bool solid=false;for(const auto& s:terrain.shapes)solid|=s.id==data.manholeCollider;
        if(!solid)return false;
        clear();data_=data;
        if(!machine_.bind(data.rules,save,data_.key,data_.flag)){clear();return false;}
        terrain_=&terrain;terrain_->setEnabled(data_.manholeCollider,!machine_.open());return true;
    }
    bool bindArt(const WaterwaysArtAssets& assets){
        return bindArt(assets.art);
    }
    bool bindArt(const AnimationBank& art){
        if(!bound()||!art.pageCount)return false;
        for(unsigned s:data_.sprites)if(s>=art.sprites.size())return false;
        for(const char* name:{"Machine Open","Manhole Open"}){
            const auto* c=art.find(name);if(!c||!c->count||c->fps<=0||c->first>art.frames.size()||c->count>art.frames.size()-c->first)return false;
            for(unsigned i=0;i<c->count;++i)if(art.frames[c->first+i]>=art.sprites.size())return false;
        }
        art_=&art;bank_.atlas.pageCount=art_->pageCount;bank_.atlas.sprites.resize(4);
        bank_.draws.assign(data_.draws.begin(),data_.draws.end());bank_.cameraBounds.resize(4);updateArt(0);return true;
    }
    void clear(){*this={};}
    bool reload(JijiKeyDoorSaveHooks save){
        if(!bound())return true;
        const auto data=data_;auto* terrain=terrain_;const auto* art=art_;
        return bind(data,save,*terrain)&&(!art||bindArt(*art));
    }
    bool bound()const{return terrain_!=nullptr;}
    bool controlsHero()const{return interacting_;}
    const RoomVisualBank& bank()const{return bank_;}
    const char* heroClip(const WorldQuestDialogue& ui)const{return interacting_?(ui.moving()?ui.heroClip():"LookUp"):nullptr;}
    void presentDraw(RoomDraw& d)const{
        if(!bound())return;
        if((d.owner==data_.machineOwner&&machine_.consumed())||(d.owner==data_.manholeOwner&&machine_.open()))d.values[10]=0;
    }
    bool update(float dt,Hero& h,Rect body,WorldQuestDialogue& ui,bool allowed,float turn){
        if(!bound()||!std::isfinite(dt)||dt<0)return false;
        using P=WaterwaysMachine::Phase;const auto previous=machine_.phase();
        ui.step(dt);ui.observePosition(h.position.x);
        const bool up=h.input.y>.5f;const auto r=data_.trigger;
        ui.prompt=allowed&&!interacting_&&previous==P::Idle&&body.left<r.right&&body.right>r.left&&body.bottom<r.top&&body.top>r.bottom;
        if(ui.prompt)std::snprintf(ui.promptLabel,sizeof(ui.promptLabel),"%s",data_.prompt);
        if(ui.prompt&&up&&!ui.upHeld){interacting_=true;ui.approachEither(h.position.x,data_.x,data_.walkOffset,data_.walkSpeed,h.facingRight,turn);}
        ui.upHeld=up;
        if(interacting_&&!ui.moving()&&previous==P::Idle){machine_.start();ui.open();}
        if(interacting_&&!allowed){machine_.finishText();machine_.choice(false);}
        if(interacting_&&allowed&&ui.phase()==WorldQuestDialogue::Phase::Reading){
            const bool confirm=(h.input.pressed&(Jump|Attack|Focus|Dash))!=0;
            if(previous==P::NoKey&&confirm&&ui.proceed())machine_.finishText();
            if(previous==P::Choice){
                if(!ui.pageComplete()){if(confirm)ui.proceed();}
                else{ui.choiceVisible=true;if(h.input.x<-.5f)ui.choiceYes=true;if(h.input.x>.5f)ui.choiceYes=false;
                    if(confirm){ui.choiceVisible=false;machine_.choice(ui.choiceYes);}}
            }
        }
        const auto before=machine_.phase();machine_.update(dt);const auto now=machine_.phase();
        if(now!=before){
            if(now==P::NoKey)ui.read(data_.noKey);
            if(now==P::Choice){ui.read(data_.keyText);ui.choiceYes=true;
                std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"%s",data_.yes);std::snprintf(ui.noLabel,sizeof(ui.noLabel),"%s",data_.no);}
            if(now==P::BoxDown)ui.close();
            if(now==P::Idle){ui.reset();interacting_=false;}
            if(now==P::Open){ui.close();}
        }
        if(machine_.open()&&ui.phase()==WorldQuestDialogue::Phase::Idle)interacting_=false;
        apply();updateArt(dt);return interacting_;
    }
private:
    void apply(){
        if(machine_.open())terrain_->setEnabled(data_.manholeCollider,false);
    }
    void updateArt(float dt){
        if(!art_)return;
        if(machine_.consumed()&&!machineStarted_){machineStarted_=true;players_[0].play(art_->find("Machine Open"));}
        if(machine_.open()&&!manholeStarted_){manholeStarted_=true;players_[1].play(art_->find("Manhole Open"));}
        for(auto& p:players_)p.advance(dt);
        const bool restored=machine_.phase()==WaterwaysMachine::Phase::Restored;
        for(unsigned i=0;i<4;++i){auto& d=bank_.draws[i];d=data_.draws[i];d.sprite=i;
            const auto* frame=i<2?players_[i].current(*art_):nullptr;
            bank_.atlas.sprites[i]=frame?*frame:art_->sprites[data_.sprites[i]];
            const bool visible=i==0?machine_.consumed()&&!restored&&!players_[0].complete:
                i==1?machine_.open()&&!restored&&!players_[1].complete:
                i==2?machine_.consumed()&&(restored||players_[0].complete):machine_.open()&&(restored||players_[1].complete);
            if(!visible)d.values[10]=0;
            bank_.cameraBounds[i]=roomCameraBounds(bank_.atlas.sprites[i],d);
        }
    }
    WaterwaysRoomData data_;WaterwaysMachine machine_;TerrainBank* terrain_=nullptr;const AnimationBank* art_=nullptr;
    RoomVisualBank bank_;std::array<AnimationPlayer,2> players_{};
    bool interacting_=false,machineStarted_=false,manholeStarted_=false;
};
}
