#pragma once
#include "world_city_toll.hpp"
#include "resting.hpp"
#include "room_visual.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <utility>

namespace hk3ds {
// Crossroads_47 FSM1077.  The station bell owns the toll conversation; the
// existing Resting menu remains responsible for travel after the grate opens.
class StagStationToll {
    friend struct SimulationStateHash;
public:
    enum class Phase { Unbound, Idle, Approach, Opening, Reading, Closing, PaidWait, Open };
    static constexpr unsigned station=1;
    static constexpr unsigned cost=50;
    // Enabled source BoxCollider2D741, projected through Transform537/503.
    static constexpr Rect trigger={18.325626f,4.94517901f,21.325626f,5.22260859f};

    bool bind(TollSaveHooks save,RestSave* resting=nullptr) {
        if(!save.opened||!save.geo||!save.pay)return false;
        save_=save;resting_=resting;phase_=save.opened(save.context,"openedCrossroads")?Phase::Open:Phase::Idle;
        if(phase_==Phase::Open)unlockRoute();
        remaining_=0;upHeld_=false;prompt_=false;unlocked_=false;paidPose_=phase_==Phase::Open;
        resetArtPose();return true;
    }
    bool reload(TollSaveHooks save,RestSave* resting=nullptr) { return bind(save,resting); }
    void clear() {*this={};}
    bool bound()const{return phase_!=Phase::Unbound;}
    bool controlsHero()const{return phase_!=Phase::Unbound&&phase_!=Phase::Idle&&phase_!=Phase::Open&&phase_!=Phase::PaidWait;}
    bool blocksResting()const{return prompt_||(phase_!=Phase::Unbound&&phase_!=Phase::Idle&&phase_!=Phase::Open);}
    bool prompt()const{return prompt_;}
    Phase phase()const{return phase_;}
    bool hasArt()const{return art_.pageCount&&bank_.draws.size()==4;}
    const RoomVisualBank& bank()const{return bank_;}
    // The source converter supplies HKA1 animation data and four source
    // projected actor records.  Keep the copy local so retry/reload never
    // aliases a transient loader buffer.
    bool bindArt(const AnimationBank& art,const RoomVisualBank& room){
        if(!art.pageCount||art.sprites.empty()||room.draws.size()!=4)return false;
        for(const char* name:{"Box_idle","Box_disappear","Bell_idle","Bell_stand_appear",
                              "Bell_stand","Grate_idle","Grate_disappear","Front"})
            if(!art.find(name))return false;
        AnimationBank nextArt=art;RoomVisualBank next=room;next.atlas=nextArt;
        if(!next.cameraBounds.empty()&&next.cameraBounds.size()!=next.draws.size())return false;
        constexpr std::array<unsigned,4> owners={206,209,205,17};
        for(unsigned i=0;i<next.draws.size();++i){
            const auto& draw=next.draws[i];
            if(draw.sprite>=next.atlas.sprites.size()||draw.owner!=owners[i])return false;
            for(float value:draw.values)if(!std::isfinite(value)||std::fabs(value)>100000.f)return false;
            if(draw.values[7]<0||draw.values[7]>1||draw.values[8]<0||draw.values[8]>1||
               draw.values[9]<0||draw.values[9]>1||draw.values[10]<0||draw.values[10]>1||
               draw.values[15]<0||draw.values[15]>5||draw.values[15]!=std::floor(draw.values[15]))return false;
        }
        for(const auto& bounds:next.cameraBounds)
            if(!std::isfinite(bounds.left)||!std::isfinite(bounds.bottom)||!std::isfinite(bounds.right)||
               !std::isfinite(bounds.top)||bounds.left>=bounds.right||bounds.bottom>=bounds.top)return false;
        art_=std::move(nextArt);bank_=std::move(next);resetArtPose();return true;
    }
    bool loadArt(const char* directory){
        if(!directory||!bound())return false;
        char path[512];
        if(std::snprintf(path,sizeof(path),"%s/stag-station-art.bin",directory)>=int(sizeof(path))||
           !art_.load(path))return false;
        if(std::snprintf(path,sizeof(path),"%s/stag-station-art-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;char magic[4]{};std::uint32_t count=0;
        RoomVisualBank room;bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"SSA1",4)&&
            std::fread(&count,4,1,f)==1&&count==4;
        if(ok)room.draws.resize(count);
        for(auto& draw:room.draws)ok=ok&&std::fread(&draw.sprite,4,1,f)==1&&std::fread(&draw.owner,4,1,f)==1&&
            std::fread(draw.values.data(),4,16,f)==16;
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);if(!ok){art_={};return false;}
        if(!bindArt(art_,room)){art_={};bank_={};return false;}return true;
    }
    bool takeUnlocked(){const bool result=unlocked_;unlocked_=false;return result;}

    bool update(float dt,Hero& hero,WorldQuestDialogue& ui,Rect range,float stationX,bool allowed,float turnSeconds) {
        if(!bound()||!std::isfinite(dt)||dt<0)return false;
        ui.step(dt);ui.observePosition(hero.position.x);prompt_=false;
        if(controlsHero()&&!allowed){ui.reset();phase_=save_.opened(save_.context,"openedCrossroads")?Phase::Open:Phase::Idle;
            paidPose_=phase_==Phase::Open;resetArtPose();return false;}
        const bool up=hero.input.y>.5f;
        const Rect body=bodyBounds(hero.position);
        if(phase_==Phase::Idle){
            prompt_=allowed&&hero.controlEnabled&&hero.inputEnabled&&hero.contacts.ground&&!hero.dashing&&!hero.nail.active&&
                body.left<range.right&&body.right>range.left&&body.bottom<range.top&&body.top>range.bottom;
            if(prompt_){std::snprintf(ui.promptLabel,sizeof(ui.promptLabel),"Inspect");ui.prompt=true;}
            if(prompt_&&up&&!upHeld_){
                phase_=Phase::Approach;ui.approachEither(hero.position.x,stationX,1.15f,6.f,hero.facingRight,turnSeconds);
            }
        } else if(phase_==Phase::Approach&&!ui.moving()){
            phase_=Phase::Opening;remaining_=.25f;ui.open();
        } else if(phase_==Phase::Opening){
            remaining_-=dt;
            if(remaining_<=0){
                phase_=Phase::Reading;ui.tollLayout=true;ui.read("@Prompts/TOLLBOOTH_STAG");
                ui.choiceVisible=true;ui.choiceYes=save_.geo(save_.context)>=cost;
                std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"YES %u Geo",cost);
                std::snprintf(ui.noLabel,sizeof(ui.noLabel),"NO");
            }
        } else if(phase_==Phase::Reading){
            const bool enough=save_.geo(save_.context)>=cost;
            ui.choiceVisible=true;ui.choiceYes=ui.choiceYes&&enough;
            if(hero.input.x<-.5f&&enough)ui.choiceYes=true;
            if(hero.input.x>.5f||!enough)ui.choiceYes=false;
            if((hero.input.pressed&(Jump|Attack|Focus|Dash))!=0&&!ui.pageComplete())ui.proceed();
            else if((hero.input.pressed&(Jump|Attack|Focus|Dash))!=0){
                ui.choiceVisible=false;
                if(!ui.choiceYes){ui.close();phase_=Phase::Closing;}
                else if(save_.pay(save_.context,cost,"openedCrossroads")){
                    // Snapshotting during the paid animation must already retain
                    // the route; reload also repairs older interrupted payments.
                    unlockRoute();startPaidArt();
                    ui.close();phase_=Phase::PaidWait;remaining_=.5f;
                }
            }
        } else if(phase_==Phase::Closing&&!ui.locked()){
            ui.reset();phase_=Phase::Idle;
        } else if(phase_==Phase::PaidWait){
            remaining_-=dt;
            if(remaining_<=0&&!ui.locked()){
                ui.reset();phase_=Phase::Open;unlocked_=true;
            }
        }
        upHeld_=up;
        updateArt(dt);
        return controlsHero();
    }
private:
    void unlockRoute(){if(resting_)resting_->stations|=1u<<station;}
    unsigned spriteIndex(const SpriteFrame* frame)const{return frame?static_cast<unsigned>(frame-art_.sprites.data()):0;}
    enum class ArtPhase { Idle, BoxDelay, BoxDisappear, GrateDisappear, BellPause, BellAppear, Open };
    void resetArtPose(){
        if(!hasArt())return;
        artPhase_=paidPose_?ArtPhase::Open:ArtPhase::Idle;artRemaining_=0;
        if(paidPose_){
            box_.play(art_.find("Box_disappear"),0,true);grate_.play(art_.find("Grate_disappear"),0,true);
            bell_.play(art_.find("Bell_stand_appear"),0,true);
            box_.advance(10);grate_.advance(10);bell_.advance(10);bell_.play(art_.find("Bell_stand"),0,true);
        } else {
            box_.play(art_.find("Box_idle"),0,true);grate_.play(art_.find("Grate_idle"),0,true);bell_.play(art_.find("Bell_idle"),0,true);
        }
        updateArt(0);
    }
    void startPaidArt(){
        if(!hasArt())return;
        paidPose_=true;artPhase_=ArtPhase::BoxDelay;artRemaining_=.5f;
        box_.play(art_.find("Box_idle"),0,true);grate_.play(art_.find("Grate_idle"),0,true);bell_.play(art_.find("Bell_idle"),0,true);
        updateArt(0);
    }
    void updateArt(float dt){
        if(!hasArt()||!std::isfinite(dt)||dt<0)return;
        float left=dt;
        while(left>0&&artPhase_!=ArtPhase::Idle&&artPhase_!=ArtPhase::Open){
            if(artPhase_==ArtPhase::BoxDelay||artPhase_==ArtPhase::BellPause){
                const float used=std::min(left,artRemaining_);artRemaining_-=used;left-=used;
                if(artRemaining_>0)break;
                if(artPhase_==ArtPhase::BoxDelay){artPhase_=ArtPhase::BoxDisappear;box_.play(art_.find("Box_disappear"),0,true);}
                else {artPhase_=ArtPhase::BellAppear;bell_.play(art_.find("Bell_stand_appear"),0,true);}
                continue;
            }
            if(artPhase_==ArtPhase::BoxDisappear){
                box_.advance(left);left=0;
                if(box_.complete){artPhase_=ArtPhase::GrateDisappear;grate_.play(art_.find("Grate_disappear"),0,true);}
                continue;
            }
            if(artPhase_==ArtPhase::GrateDisappear){
                grate_.advance(left);left=0;
                if(grate_.complete){artPhase_=ArtPhase::BellPause;artRemaining_=2.3f;}
                continue;
            }
            if(artPhase_==ArtPhase::BellAppear){
                bell_.advance(left);left=0;
                if(bell_.complete){bell_.play(art_.find("Bell_stand"),0,true);artPhase_=ArtPhase::Open;}
            }
        }
        if(artPhase_==ArtPhase::Idle){box_.advance(dt);grate_.advance(dt);bell_.advance(dt);}
        const auto set=[&](unsigned slot,const AnimationPlayer& player,const char* fallback){
            const auto* frame=player.current(art_);const auto* clip=frame?frame:art_.sprites.empty()?nullptr:&art_.sprites[0];
            if(!clip)return;
            bank_.draws[slot].sprite=spriteIndex(clip);
            if(bank_.cameraBounds.size()==bank_.draws.size())
                bank_.cameraBounds[slot]=roomCameraBounds(art_.sprites[bank_.draws[slot].sprite],bank_.draws[slot]);
            (void)fallback;};
        set(0,box_,"Box_idle");set(1,bell_,"Bell_idle");set(3,grate_,"Grate_idle");
    }
    RestSave* resting_=nullptr;
    TollSaveHooks save_{};
    float remaining_=0;
    Phase phase_=Phase::Unbound;
    bool upHeld_=false,prompt_=false,unlocked_=false,paidPose_=false;
    ArtPhase artPhase_=ArtPhase::Idle;float artRemaining_=0;
    AnimationBank art_;
    RoomVisualBank bank_;
    AnimationPlayer box_,bell_,grate_;
};
static_assert(sizeof(StagStationToll)<512,"station toll controller state must remain bounded");
}
