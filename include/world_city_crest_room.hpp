#pragma once
#include "world_city_crest.hpp"
#include "world_lift_motion.hpp"
#include "world_quests.hpp"
#include "room_visual.hpp"

namespace hk3ds {
struct CityCrestRoomData {
    CityCrestRules rules;
    unsigned owner=0,collider=0,blocker=0,mapZone=0;
    Rect inspect{},slam{},drop{};
    float x=0,walkOffset=0,walkSpeed=0;
    Vec2 respawn{};
    std::array<float,3> soundPosition{};
    char noKey[768]{},key[768]{},yes[32]{},no[32]{},prompt[32]{};
    bool valid()const{
        auto rect=[](Rect r){return std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&
            std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;};
        auto text=[](const char* s,unsigned n){return s[0]&&std::memchr(s,0,n);};
        return rules.valid()&&owner&&collider&&blocker&&mapZone>0&&mapZone<64&&collider!=blocker&&rect(inspect)&&rect(slam)&&rect(drop)&&
            std::isfinite(x)&&std::isfinite(walkOffset)&&walkOffset>=0&&std::isfinite(walkSpeed)&&walkSpeed>0&&
            std::isfinite(respawn.x)&&std::isfinite(respawn.y)&&text(noKey,768)&&text(key,768)&&
            text(yes,32)&&text(no,32)&&text(prompt,32);
    }
    bool load(const char* directory){
        if(!directory)return false;
        CityCrestRoomData next;char path[512];
        if(std::snprintf(path,sizeof(path),"%s/city-crest-controller.bin",directory)>=int(sizeof(path))||!next.rules.load(path))return false;
        if(std::snprintf(path,sizeof(path),"%s/city-crest-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;char magic[4];
        auto read=[&](void* p,unsigned n){return std::fread(p,1,n,f)==n;};
        const bool ok=read(magic,4)&&!std::memcmp(magic,"CRR2",4)&&read(&next.owner,4)&&read(&next.collider,4)&&
            read(&next.blocker,4)&&read(&next.mapZone,4)&&read(&next.inspect,sizeof(Rect))&&read(&next.slam,sizeof(Rect))&&read(&next.drop,sizeof(Rect))&&
            read(&next.x,4)&&read(&next.walkOffset,4)&&read(&next.walkSpeed,4)&&read(&next.respawn,sizeof(Vec2))&&
            read(next.noKey,768)&&read(next.key,768)&&read(next.yes,32)&&read(next.no,32)&&read(next.prompt,32)&&
            std::fgetc(f)==EOF&&next.valid();std::fclose(f);
        if(!ok||std::snprintf(path,sizeof(path),"%s/city-crest-audio.bin",directory)>=int(sizeof(path)))return false;
        f=std::fopen(path,"rb");if(!f)return false;
        const bool audio=read(magic,4)&&!std::memcmp(magic,"CCA1",4)&&read(next.soundPosition.data(),12)&&std::fgetc(f)==EOF&&
            std::all_of(next.soundPosition.begin(),next.soundPosition.end(),[](float v){return std::isfinite(v);});
        std::fclose(f);if(audio)*this=next;return audio;
    }
};
class CityCrestGate {
    friend struct SimulationStateHash;
public:
    bool bind(const CityCrestRoomData& data,CityCrestSaveHooks save,TerrainBank& terrain,Hero& hero){
        if(!data.valid())return false;
        const bool blocker=std::any_of(terrain.shapes.begin(),terrain.shapes.end(),[&](const TerrainShape& s){return s.id==data.blocker;});
        if(!blocker)return false;
        clear();data_=data;save_=save;terrain_=&terrain;
        if(!socket_.bind(data.rules,save)||!door_.bind(data.rules,save)||!motion_.bind(terrain,&data_.collider,1,0)){clear();return false;}
        MineLiftEvents e;e.add(MineLiftActionKind::SetY,door_.y());
        if(!motion_.apply(e,hero)){clear();return false;}
        applyCollision();return true;
    }
    void clear(){*this={};}
    bool bound()const{return terrain_!=nullptr;}
    bool controlsHero()const{return interacting_;}
    const CityCrestRoomData& data()const{return data_;}
    CityCrestDoor::Phase phase()const{return door_.phase();}
    float y()const{return door_.y();}
    CityCrestSounds sounds()const{return door_.sounds();}
    bool reload(Hero& hero){
        if(!bound())return true;
        MineLiftEvents e;e.add(MineLiftActionKind::SetY,0);
        if(!motion_.apply(e,hero))return false;
        const auto data=data_;const auto save=save_;auto* terrain=terrain_;
        return bind(data,save,*terrain,hero);
    }
    void applyCollision(){if(bound())terrain_->setEnabled(data_.blocker,door_.phase()==CityCrestDoor::Phase::Slam||door_.phase()==CityCrestDoor::Phase::Slammed);}
    void presentDraw(RoomDraw& draw,PresentationTime time)const{
        if(bound()&&draw.owner==data_.owner)draw.values[5]+=motion_.presentedY(time);
    }
    bool update(float dt,Hero& hero,WorldQuestDialogue& ui,bool allowed,float turnSeconds){
        if(!bound()||!std::isfinite(dt)||dt<0)return false;
        const Rect body=bodyBounds(hero.position);
        auto overlap=[&](Rect r,bool moving){
            if(moving){r.bottom+=door_.y();r.top+=door_.y();}
            return body.left<r.right&&body.right>r.left&&body.bottom<r.top&&body.top>r.bottom;
        };
        const bool slam=overlap(data_.slam,true),drop=overlap(data_.drop,true);
        if(slam&&!slamHeld_)pendingSlam_=true;
        if(pendingSlam_&&door_.slam())pendingSlam_=false;
        // Events sent to an Idle gate have no receiver in the original FSM.
        if(door_.phase()==CityCrestDoor::Phase::Closed)pendingSlam_=false;
        if(drop&&!dropHeld_)door_.drop();
        slamHeld_=slam;dropHeld_=drop;
        door_.update(dt);
        MineLiftEvents motion;motion.add(MineLiftActionKind::TranslateY,door_.y()-motion_.y());
        if(!motion_.apply(motion,hero))return false;
        applyCollision();
        const bool up=hero.input.y>.5f;
        if(!interacting_){
            if(!allowed||ui.locked()||socket_.phase()!=CityCrestSocket::Phase::Idle)return false;
            ui.prompt=overlap(data_.inspect,false);
            if(ui.prompt)std::snprintf(ui.promptLabel,sizeof(ui.promptLabel),"%s",data_.prompt);
            if(ui.prompt&&up&&!ui.upHeld){
                interacting_=true;started_=false;
                ui.approachLeft(hero.position.x,data_.x,data_.walkOffset,data_.walkSpeed,hero.facingRight,turnSeconds);
            }
            ui.upHeld=up;
            if(!interacting_)return false;
        }
        ui.step(dt);ui.observePosition(hero.position.x);
        if(!started_&&!ui.moving()){apply(socket_.begin(),ui);started_=true;}
        if(!allowed){
            if(socket_.phase()==CityCrestSocket::Phase::NoKey)apply(socket_.finish(),ui);
            if(socket_.phase()==CityCrestSocket::Phase::Choice)apply(socket_.choose(false),ui);
        }
        if(allowed&&ui.phase()==WorldQuestDialogue::Phase::Reading){
            const unsigned confirm=hero.input.pressed&(Jump|Attack|Focus|Dash);
            if(socket_.phase()==CityCrestSocket::Phase::NoKey&&confirm&&ui.proceed())apply(socket_.finish(),ui);
            else if(socket_.phase()==CityCrestSocket::Phase::Choice){
                if(!ui.pageComplete()){if(confirm)ui.proceed();}
                else{
                    ui.choiceVisible=true;
                    const int direction=hero.input.x<-.5f?-1:hero.input.x>.5f?1:0;
                    repeat_=std::max(0.f,repeat_-dt);
                    if(direction&&(direction!=direction_||repeat_<=0)){
                        ui.choiceYes=direction<0;repeat_=direction!=direction_?.25f:.15f;
                    }
                    direction_=direction;if(confirm)apply(socket_.choose(ui.choiceYes),ui);
                }
            }
        }
        apply(socket_.update(dt),ui);ui.upHeld=up;
        if(interacting_){hero.controlEnabled=false;hero.velocity={ui.velocity(),0};if(ui.moving())hero.facingRight=ui.facingRight();}
        return interacting_;
    }
private:
    void apply(const CityCrestEvents& events,WorldQuestDialogue& ui){
        for(unsigned i=0;i<events.count;++i)switch(events.values[i]){
        case CityCrestAction::BoxUp:case CityCrestAction::ChoiceUp:ui.open();break;
        case CityCrestAction::NoKeyText:ui.read(data_.noKey);break;
        case CityCrestAction::KeyText:ui.read(data_.key);ui.choiceYes=true;
            std::snprintf(ui.yesLabel,sizeof(ui.yesLabel),"%s",data_.yes);std::snprintf(ui.noLabel,sizeof(ui.noLabel),"%s",data_.no);break;
        case CityCrestAction::ChoiceDown:ui.choiceVisible=false;break;
        case CityCrestAction::BoxDown:ui.close();break;
        case CityCrestAction::ConversationEnd:ui.reset();interacting_=started_=false;break;
        case CityCrestAction::GateOpen:door_.open();break;
        default:break;
        }
    }
    CityCrestRoomData data_;CityCrestSocket socket_;CityCrestDoor door_;CityCrestSaveHooks save_;
    MineLiftMotion motion_;TerrainBank* terrain_=nullptr;
    bool interacting_=false,started_=false,slamHeld_=false,dropHeld_=false,pendingSlam_=false;
    int direction_=0;float repeat_=0;
};
}
