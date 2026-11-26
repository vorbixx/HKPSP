#pragma once
#include "world_quests.hpp"
#include "hero.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
struct TollSaveHooks {
    void* context=nullptr;
    bool (*opened)(void*,const char*)=nullptr;
    unsigned (*geo)(void*)=nullptr;
    // Publish Geo and the original PlayerData bool in one owner transaction.
    bool (*pay)(void*,unsigned,const char*)=nullptr;
};
struct CityTollData {
    Rect trigger{};
    float x=0,offset=0,speed=0,boxWait=0,paidWait=0;
    unsigned cost=0,owner=0;
    char flag[64]{},text[768]{},insufficient[64]{},yes[32]{},no[32]{},prompt[32]{};
    bool valid()const{
        const float values[]={trigger.left,trigger.bottom,trigger.right,trigger.top,x,offset,speed,boxWait,paidWait};
        for(float v:values)if(!std::isfinite(v)||std::abs(v)>100000)return false;
        const auto string=[](const char* p,std::size_t n){return *p&&std::memchr(p,0,n);};
        return trigger.left<trigger.right&&trigger.bottom<trigger.top&&offset>=0&&speed>0&&boxWait>=0&&paidWait>=0&&
            cost>0&&cost<=9999999&&owner&&string(flag,sizeof(flag))&&string(text,sizeof(text))&&
            string(insufficient,sizeof(insufficient))&&string(yes,sizeof(yes))&&string(no,sizeof(no))&&string(prompt,sizeof(prompt));
    }
    bool load(const char* directory){
        if(!directory)return false;
        char path[512];if(std::snprintf(path,sizeof(path),"%s/city-toll.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;CityTollData next;char magic[4];
        const auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        const bool ok=read(magic,4)&&!std::memcmp(magic,"CTF1",4)&&read(&next.trigger,sizeof(Rect))&&
            read(&next.x,4)&&read(&next.offset,4)&&read(&next.speed,4)&&read(&next.boxWait,4)&&read(&next.paidWait,4)&&
            read(&next.cost,4)&&read(&next.owner,4)&&read(next.flag,64)&&read(next.text,768)&&read(next.insufficient,64)&&
            read(next.yes,32)&&read(next.no,32)&&read(next.prompt,32)&&std::fgetc(f)==EOF&&next.valid();
        std::fclose(f);if(ok)*this=next;return ok;
    }
};
// Crossroads_49b level80 FSM819. The global Text YN FSM21856 checks
// affordability and takes Geo before YES; the machine then sets cityLift1.
class CityLiftToll {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,Approach,Opening,Reading,Closing,PaidWait,Open};
    bool bind(const CityTollData& data,TollSaveHooks save){
        if(!data.valid()||!save.opened||!save.geo||!save.pay)return false;
        CityLiftToll next;next.data_=data;next.save_=save;
        next.phase_=save.opened(save.context,data.flag)?Phase::Open:Phase::Idle;*this=next;return true;
    }
    bool reload(TollSaveHooks save){return bound()?bind(data_,save):true;}
    void clear(){*this={};}
    bool bound()const{return phase_!=Phase::Unbound;}
    bool controlsHero()const{return phase_!=Phase::Unbound&&phase_!=Phase::Idle&&phase_!=Phase::Open;}
    bool hides(unsigned owner)const{return phase_==Phase::Open&&owner==data_.owner;}
    template<class Emit> void visitAlphaOwners(Emit emit)const{if(bound())emit(data_.owner);}
    Phase phase()const{return phase_;}
    const char* heroClip(const WorldQuestDialogue& ui)const{return controlsHero()?(ui.moving()?ui.heroClip():phase_==Phase::Closing?"LookUpEnd":"LookUp"):nullptr;}
    bool update(float dt,Hero& h,Rect body,WorldQuestDialogue& ui,bool allowed,float turn){
        if(!bound()||!std::isfinite(dt)||dt<0)return false;
        ui.step(dt);ui.observePosition(h.position.x);
        if(controlsHero()&&!allowed){ui.reset();phase_=save_.opened(save_.context,data_.flag)?Phase::Open:Phase::Idle;return false;}
        const bool up=h.input.y>.5f;const auto r=data_.trigger;
        ui.prompt=allowed&&phase_==Phase::Idle&&h.controlEnabled&&h.inputEnabled&&h.contacts.ground&&!h.dashing&&!h.nail.active&&
            body.left<r.right&&body.right>r.left&&body.bottom<r.top&&body.top>r.bottom;
        if(ui.prompt)std::snprintf(ui.promptLabel,sizeof(ui.promptLabel),"%s",data_.prompt);
        if(ui.prompt&&up&&!ui.upHeld){phase_=Phase::Approach;ui.approachEither(h.position.x,data_.x,data_.offset,data_.speed,h.facingRight,turn);}
        ui.upHeld=up;
        if(phase_==Phase::Approach&&!ui.moving()){phase_=Phase::Opening;remaining_=data_.boxWait;ui.tollLayout=true;ui.open();}
        else if(phase_==Phase::Opening){remaining_-=dt;if(remaining_<=0){phase_=Phase::Reading;ui.read(data_.text);
            std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"%.18s %u Geo",data_.yes,data_.cost);
            std::snprintf(ui.noLabel,sizeof(ui.noLabel),"%s",data_.no);ui.choiceYes=save_.geo(save_.context)>=data_.cost;}}
        else if(phase_==Phase::Reading){
            const bool confirm=(h.input.pressed&(Jump|Attack|Focus|Dash))!=0;
            if(!ui.pageComplete()){if(confirm)ui.proceed();}
            else{
                const bool enough=save_.geo(save_.context)>=data_.cost;
                if(enough)std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"%.18s %u Geo",data_.yes,data_.cost);
                else std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"%.31s",data_.insufficient);
                ui.choiceVisible=true;
                if(h.input.x<-.5f&&enough)ui.choiceYes=true;
                if(h.input.x>.5f||!enough)ui.choiceYes=false;
                if(confirm){
                    ui.choiceVisible=false;
                    if(!ui.choiceYes){ui.close();phase_=Phase::Closing;}
                    else if(save_.pay(save_.context,data_.cost,data_.flag)){h.facingRight=false;ui.close();phase_=Phase::PaidWait;remaining_=data_.paidWait;}
                }
            }
        }else if(phase_==Phase::Closing&&!ui.locked()){phase_=Phase::Idle;ui.reset();}
        else if(phase_==Phase::PaidWait){remaining_-=dt;if(remaining_<=0&&!ui.locked()){phase_=Phase::Open;ui.reset();}}
        return controlsHero();
    }
private:
    CityTollData data_{};TollSaveHooks save_{};float remaining_=0;Phase phase_=Phase::Unbound;
};
static_assert(sizeof(CityLiftToll)<2048,"City toll has no texture/audio allocation");
}
