#pragma once
#include "world_lift_motion.hpp"
#include "room_visual.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
struct CityLiftArtAssets {
    AnimationBank art;std::array<RoomDraw,3> draws{};unsigned idle=0,leverIdle=0;
    bool load(const char* directory){
        if(!directory)return false;
        char path[512];
        if(std::snprintf(path,sizeof(path),"%s/city-lift-art.bin",directory)>=int(sizeof(path))||!art.load(path))return false;
        return loadRoom(directory);
    }
    // Room table only (city-lift-art-room.bin). The PSP binds its own PSA1 art, which keeps this sprite order.
    bool loadRoom(const char* directory){
        if(!directory)return false;
        char path[512];
        if(std::snprintf(path,sizeof(path),"%s/city-lift-art-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;char magic[4];
        bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CLA2",4)&&std::fread(&idle,4,1,f)==1&&idle<art.sprites.size()&&
            std::fread(&leverIdle,4,1,f)==1&&leverIdle<art.sprites.size();
        for(auto& d:draws){ok=ok&&std::fread(&d.sprite,4,1,f)==1&&std::fread(&d.owner,4,1,f)==1&&std::fread(d.values.data(),4,16,f)==16&&d.owner;
            for(float v:d.values)ok=ok&&std::isfinite(v);}
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);return ok;
    }
};
struct CityLiftData {
    float initial=0,bottom=0,delay=0,duration=0,landWait=0;
    std::array<unsigned,16> colliders{};
    std::array<unsigned,96> owners{};
    std::array<unsigned,3> open{},closed{};
    unsigned colliderCount=0,ownerCount=0,inactive=0;
    bool valid()const{
        for(float v:{initial,bottom,delay,duration,landWait})if(!std::isfinite(v)||std::abs(v)>100000)return false;
        if(initial<bottom||delay<0||duration<=0||landWait<0||!inactive||
           !colliderCount||colliderCount>colliders.size()||!ownerCount||ownerCount>owners.size())return false;
        const auto contains=[&](unsigned id){return id&&std::find(colliders.begin(),colliders.begin()+colliderCount,id)!=colliders.begin()+colliderCount;};
        if(!contains(inactive))return false;
        for(auto id:open)if(!contains(id))return false;
        for(auto id:closed)if(!contains(id)||std::find(open.begin(),open.end(),id)!=open.end())return false;
        for(unsigned i=0;i<colliderCount;++i){if(!colliders[i])return false;for(unsigned j=0;j<i;++j)if(colliders[i]==colliders[j])return false;}
        for(unsigned i=0;i<ownerCount;++i){if(!owners[i])return false;for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return false;}
        return true;
    }
    bool load(const char* directory){
        if(!directory)return false;
        char path[512];if(std::snprintf(path,sizeof(path),"%s/city-lift.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;CityLiftData next;char magic[4];
        const auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"CLD1",4)&&read(&next.initial,4)&&read(&next.bottom,4)&&
            read(&next.delay,4)&&read(&next.duration,4)&&read(&next.landWait,4)&&read(&next.colliderCount,4)&&
            read(&next.ownerCount,4)&&read(&next.inactive,4)&&next.colliderCount<=next.colliders.size()&&next.ownerCount<=next.owners.size();
        if(ok)ok=read(next.open.data(),12)&&read(next.closed.data(),12)&&read(next.colliders.data(),next.colliderCount*4)&&
            read(next.owners.data(),next.ownerCount*4)&&std::fgetc(f)==EOF&&next.valid();
        std::fclose(f);if(ok)*this=next;return ok;
    }
};
// Crossroads_49b FSM845: TOLL LIFT PAY -> delayed easeOutCubic lower ->
// Land wait -> DOORS OPEN. FSM851 restores the paid endpoint on scene entry.
class CityLiftLowering {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Up,Delay,Moving,Landing,Open};
    bool bind(const CityLiftData& data,TerrainBank& terrain,bool paid,Hero& hero){
        if(!data.valid())return false;
        clear();data_=data;terrain_=&terrain;
        if(!motion_.bind(terrain,data.colliders.data(),data.colliderCount,data.initial)){clear();return false;}
        phase_=paid?Phase::Open:Phase::Up;
        if(paid){MineLiftEvents e;e.add(MineLiftActionKind::SetY,data.bottom);if(!motion_.apply(e,hero)){clear();return false;}}
        applyCollision();return true;
    }
    bool reload(bool paid,Hero& hero){
        if(!bound())return true;
        const auto data=data_;auto* terrain=terrain_;const auto* art=art_;
        MineLiftEvents reset;reset.add(MineLiftActionKind::SetY,data.initial);
        if(!motion_.apply(reset,hero))return false;
        return bind(data,*terrain,paid,hero)&&(!art||bindArt(*art));
    }
    bool bindArt(const CityLiftArtAssets& art){
        if(!bound()||!art.art.pageCount||art.idle>=art.art.sprites.size())return false;
        const auto* open=art.art.find("Lift Door Open");const auto* close=art.art.find("Lift Door Close");
        if(!open||!close||open->fps<=0||!open->count||!close->count)return false;
        art_=&art;bank_.atlas.pageCount=art.art.pageCount;bank_.atlas.sprites.resize(3);bank_.draws.resize(3);bank_.cameraBounds.resize(3);
        door_.play(phase_==Phase::Open?open:close);door_.advance(float((phase_==Phase::Open?open:close)->count)/20.f+1);
        lever_.play(art.art.find("Call Switch Idle"));
        updateArt(0);return true;
    }
    const RoomVisualBank& bank()const{return bank_;}
    void clear(){*this={};}
    bool bound()const{return terrain_!=nullptr;}
    Phase phase()const{return phase_;}
    float y()const{return motion_.y();}
    bool travelY(float target,Hero& hero,bool carry){
        if(!bound()||!std::isfinite(target))return false;
        const float delta=target-motion_.y();MineLiftEvents e;e.add(MineLiftActionKind::TranslateY,delta);
        if(!motion_.apply(e,hero))return false;
        if(carry)hero.platformDisplacement+=delta;
        return true;
    }
    void travelDoors(bool closed){
        travelClosed_=closed;applyCollision();
        if(art_)door_.play(art_->art.find(closed?"Lift Door Close":"Lift Door Open"),0,true);
    }
    void strikeLever(bool right){if(art_)lever_.play(art_->art.find(right?"Call Switch Right":"Call Switch Left"),0,true);}
    bool controlsHero()const{return phase_==Phase::Delay||phase_==Phase::Moving||phase_==Phase::Landing;}
    bool takeControlRelease(){const bool value=release_;release_=false;return value;}
    bool lower(){if(phase_!=Phase::Up)return false;phase_=Phase::Delay;elapsed_=0;return true;}
    void applyCollision(){
        if(!bound())return;
        terrain_->setEnabled(data_.inactive,phase_!=Phase::Open);
        for(auto id:data_.closed)terrain_->setEnabled(id,travelClosed_);
        for(auto id:data_.open)terrain_->setEnabled(id,phase_==Phase::Open&&!travelClosed_);
    }
    void fixedUpdate(float dt,Hero& hero){
        if(!bound()||!std::isfinite(dt)||dt<0)return;
        MineLiftEvents e;
        if(controlsHero())elapsed_+=dt;
        if(phase_==Phase::Delay&&elapsed_>=data_.delay){phase_=Phase::Moving;elapsed_=0;}
        else if(phase_==Phase::Moving){
            const float u=std::min(1.f,elapsed_/data_.duration),v=1-u;
            e.add(MineLiftActionKind::SetY,data_.initial+(data_.bottom-data_.initial)*(1-v*v*v));
            if(u>=1){phase_=Phase::Landing;elapsed_=0;}
        }else if(phase_==Phase::Landing&&elapsed_>=data_.landWait){phase_=Phase::Open;elapsed_=0;release_=true;
            if(art_)door_.play(art_->art.find("Lift Door Open"));}
        // Translation retains PresentationPosition interpolation; only reloads
        // and initial scene placement snap to their saved endpoint.
        for(unsigned i=0;i<e.count;++i){e.actions[i].kind=MineLiftActionKind::TranslateY;e.actions[i].value-=motion_.y();}
        if(!motion_.apply(e,hero)){clear();return;}
        applyCollision();updateArt(dt);
    }
    bool moves(unsigned owner)const{return bound()&&std::find(data_.owners.begin(),data_.owners.begin()+data_.ownerCount,owner)!=data_.owners.begin()+data_.ownerCount;}
    void presentDraw(RoomDraw& draw,PresentationTime time)const{
        if(moves(draw.owner))draw.values[5]+=motion_.presentedY(time)-data_.initial;
    }
private:
    void updateArt(float dt){
        if(!art_)return;
        door_.advance(dt);lever_.advance(dt);
        if(lever_.complete)lever_.play(art_->art.find("Call Switch Idle"));
        for(unsigned i=0;i<3;++i){
            bank_.draws[i]=art_->draws[i];bank_.draws[i].sprite=i;
            const auto* frame=i<2?door_.current(art_->art):lever_.current(art_->art);
            bank_.atlas.sprites[i]=frame?*frame:art_->art.sprites[i<2?art_->idle:art_->leverIdle];
            // The common actor capture applies the interpolated lift transform.
            bank_.cameraBounds[i]=roomCameraBounds(bank_.atlas.sprites[i],bank_.draws[i]);
        }
    }
    CityLiftData data_{};MineLiftMotion motion_{};TerrainBank* terrain_=nullptr;
    const CityLiftArtAssets* art_=nullptr;RoomVisualBank bank_;AnimationPlayer door_,lever_;
    float elapsed_=0;Phase phase_=Phase::Unbound;bool release_=false,travelClosed_=false;
};
static_assert(sizeof(CityLiftLowering)<2048,"City lift reuses scene art and collision storage");
}
