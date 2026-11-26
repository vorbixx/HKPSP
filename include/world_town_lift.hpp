#pragma once
#include "world_lift_room.hpp"
#include "world_lift_motion.hpp"
#include "world_lift_lever.hpp"
#include "world_switch_runtime.hpp"
#include "room_visual.hpp"

namespace hk3ds {
// TLR1 is a port sidecar: all coordinates, identities and animation timing must
// come from the Town export. A missing sidecar fails admission, never opens it.
struct TownLiftRoomData {
    WorldLiftRoomData lift;
    char scene[32]{},id[64]{};
    unsigned cageCollider=0,leverCollider=0,closedOwner=0,openOwner=0;
    std::array<unsigned,8> colliders{};unsigned colliderCount=0;
    std::array<unsigned,64> owners{};unsigned ownerCount=0;
    Rect trigger{},headTrigger{};MineLiftLeverRules lever;
    float initialY=0,cageHideSeconds=0;
    Vec2 hitOrigin{};float hitDepth=0;
    std::array<RoomDraw,2> draws{};std::array<unsigned,2> idleSprites{};
    bool valid()const{
        auto text=[](const char* s,std::size_t n){return s[0]&&std::memchr(s,0,n);};
        auto rect=[](Rect r){return std::isfinite(r.left)&&std::isfinite(r.right)&&std::isfinite(r.top)&&
            std::isfinite(r.bottom)&&r.left<r.right&&r.bottom<r.top;};
        if(!lift.valid()||!text(scene,sizeof(scene))||!text(id,sizeof(id))||!cageCollider||!leverCollider||
           !closedOwner||!openOwner||!colliderCount||colliderCount>colliders.size()||ownerCount>owners.size()||
           !rect(trigger)||!rect(headTrigger)||!std::isfinite(initialY)||!std::isfinite(cageHideSeconds)||cageHideSeconds<0||
           !std::isfinite(lever.hitWait)||lever.hitWait<0||!std::isfinite(hitOrigin.x)||
           !std::isfinite(hitOrigin.y)||!std::isfinite(hitDepth))return false;
        for(unsigned i=0;i<colliderCount;++i){if(!colliders[i])return false;
            for(unsigned j=0;j<i;++j)if(colliders[j]==colliders[i])return false;}
        for(const auto& d:draws){if(!d.owner)return false;for(float v:d.values)if(!std::isfinite(v))return false;}
        return true;
    }
    bool load(const char* directory){
        if(!directory)return false;
        TownLiftRoomData next;char path[512];
        if(std::snprintf(path,sizeof(path),"%s/town-lift-controller.bin",directory)>=int(sizeof(path))||!next.lift.load(path))return false;
        if(std::snprintf(path,sizeof(path),"%s/town-lift-room.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};char magic[4];
        bool ok=read(magic,4)&&!std::memcmp(magic,"TLR2",4)&&read(next.scene,32)&&read(next.id,64)&&
            read(&next.cageCollider,4)&&read(&next.leverCollider,4)&&read(&next.closedOwner,4)&&read(&next.openOwner,4)&&
            read(&next.colliderCount,4)&&read(next.colliders.data(),32)&&read(&next.ownerCount,4)&&read(next.owners.data(),256)&&
            read(&next.trigger,sizeof(Rect))&&read(&next.headTrigger,sizeof(Rect))&&read(&next.lever.hitWait,4)&&
            read(&next.initialY,4)&&read(&next.cageHideSeconds,4)&&read(next.idleSprites.data(),8);
        for(auto& d:next.draws)ok=ok&&read(&d.owner,4)&&read(d.values.data(),64);
        ok=ok&&read(&next.hitOrigin,sizeof(Vec2))&&read(&next.hitDepth,4);
        ok=ok&&std::fgetc(f)==EOF&&next.valid();std::fclose(f);if(ok)*this=next;return ok;
    }
};
struct TownLiftRoomAssets {
    TownLiftRoomData data;AnimationBank art;
    bool load(const char* directory){
        TownLiftRoomAssets next;char path[512];
        if(!directory||!next.data.load(directory)||std::snprintf(path,sizeof(path),"%s/town-lift.bin",directory)>=int(sizeof(path))||!next.art.load(path))return false;
        *this=std::move(next);return true;
    }
};
class TownMineLift {
    friend struct SimulationStateHash;
public:
    bool bind(const TownLiftRoomData& data,const AnimationBank& art,TerrainBank& terrain,
              LiftPlayerBoolHooks save,SceneBoolHooks sceneSave,float heroY){
        if(!data.valid()||!art.pageCount)return false;
        for(unsigned sprite:data.idleSprites)if(sprite>=art.sprites.size())return false;
        const auto* hit=art.find("Lever Hit");const auto* cage=art.find("Cage Open");
        if(!hit||!cage||!hit->count||!cage->count||hit->fps<=0||cage->fps<=0)return false;
        for(const auto* clip:{hit,cage}){
            if(clip->first>art.frames.size()||clip->count>art.frames.size()-clip->first)return false;
            for(unsigned i=0;i<clip->count;++i)if(art.frames[clip->first+i]>=art.sprites.size())return false;
        }
        bool cageFound=false;
        for(const auto& s:terrain.shapes)cageFound|=s.id==data.cageCollider;
        // PolygonCollider2484 is the source lever's trigger, exported in
        // data.trigger. TerrainBank contains solids, so it must not be required
        // there or inserted as a physical obstacle merely for admission.
        if(!cageFound)return false;
        // Copy owned identities before binding the controllers which borrow them.
        clear();data_=data;art_=&art;terrain_=&terrain;
        if(!data_.lift.bind(lift_,save,data.initialY)||!lever_.bind(data.lever,sceneSave,data_.scene,data_.id)||
           !motion_.bind(terrain,data.colliders.data(),data.colliderCount,data.initialY)){clear();return false;}
        bank_.atlas.pageCount=art.pageCount;bank_.atlas.sprites.resize(2);bank_.draws.assign(data.draws.begin(),data.draws.end());
        bank_.cameraBounds.resize(2);
        Hero h;h.position.y=heroY;
        // Room binding runs after the session places the hero. Deliver the
        // source HERO POSITIONED event before the lever can send OPEN.
        applyLift(lift_.heroPositioned(heroY),h);applyLever(lever_.takeEvents(),h);
        updateArt(0);return true;
    }
    void clear(){*this={};}
    bool reload(LiftPlayerBoolHooks save,SceneBoolHooks sceneSave,Hero& hero){
        if(!bound())return true;
        const auto data=data_;const auto* art=art_;auto* terrain=terrain_;
        // Return the retained moving geometry to its authored coordinates
        // before bind delivers HERO POSITIONED for the respawned hero.
        MineLiftEvents reset;reset.add(MineLiftActionKind::DeactivateCarry);reset.add(MineLiftActionKind::SetY,data.initialY);
        if(!motion_.apply(reset,hero))return false;
        terrain->setEnabled(data.cageCollider,true);
        return bind(data,*art,*terrain,save,sceneSave,hero.position.y);
    }
    bool bound()const{return terrain_!=nullptr;}
    MineLiftLever::Phase leverPhase()const{return lever_.phase();}
    Vec2 hitOrigin()const{return data_.hitOrigin;}
    float hitDepth()const{return data_.hitDepth;}
    unsigned hitSerial()const{return hitSerial_;}
    Vec2 soundPosition()const{return {data_.draws[0].values[2],data_.draws[0].values[5]};}
    float soundDepth()const{return data_.draws[0].values[6];}
    const RoomVisualBank& bank()const{return bank_;}
    bool closedCageHidden()const{return hidden_;}
    bool openCageVisible()const{return cagePlaying_&&cageAge_<data_.cageHideSeconds;}
    MineLiftLeverEvents takeLeverEvents(){auto e=feedback_;feedback_={};return e;}
    bool moves(unsigned owner)const{
        if(!bound())return false;
        return std::find(data_.owners.begin(),data_.owners.begin()+data_.ownerCount,owner)!=data_.owners.begin()+data_.ownerCount;
    }
    void presentDraw(RoomDraw& draw,PresentationTime time)const{
        if(!bound())return;
        if(moves(draw.owner))draw.values[5]+=motion_.presentedY(time)-data_.initialY;
        if(draw.owner==data_.closedOwner&&hidden_)draw.values[10]=0;
    }
    void beforePhysics(float dt,Hero& h,Rect){
        if(!bound())return;
        const bool touching=motion_.touching(h);
        Rect head=data_.headTrigger;const float delta=motion_.y()-data_.initialY;head.bottom+=delta;head.top+=delta;
        const auto b=bodyBounds(h.position);
        const bool headHit=b.left<head.right&&b.right>head.left&&b.bottom<head.top&&b.top>head.bottom;
        applyLift(lift_.update(dt,h.position.y,touching&&!wasTouching_,headHit&&!wasHead_),h);
        wasTouching_=touching;wasHead_=headHit;
    }
    void afterPhysics(Hero& h){motion_.afterPhysics(h);}
    void update(float dt,Hero& h,Rect,const NailBank& nails,GameClock& clock){
        if(!bound()||!std::isfinite(dt)||dt<0)return;
        applyLever(lever_.update(dt),h);
        if(lever_.phase()==MineLiftLever::Phase::Idle&&h.nail.active&&h.nail.hitbox&&
           nailOverlaps(h,nails.shapes[nailVariant(h.nail)],data_.trigger)){
            const auto hit=lever_.hit(true,0);
            // FSM3919 Hit sends GameManager.FreezeMoment(1). It uses the same
            // .04/.03/.04 scaled freeze as enemy kills, once per saved hit.
            if(hit.count)clock.enemyKill();
            applyLever(hit,h);
        }
        if(pendingOpen_){auto e=lift_.open(h.position.y);if(e.count){pendingOpen_=false;applyLift(e,h);}}
        updateArt(dt);
    }
private:
    void applyLift(const MineLiftEvents& e,Hero& h){
        for(unsigned i=0;i<e.count;++i){
            if(e.actions[i].kind==MineLiftActionKind::HideClosedCage){hidden_=true;terrain_->setEnabled(data_.cageCollider,false);}
            if(e.actions[i].kind==MineLiftActionKind::ShowOpenCage){cagePlaying_=true;cageAge_=0;cagePlayer_.play(art_->find("Cage Open"));}
        }
        if(!motion_.apply(e,h))clear();
    }
    void applyLever(const MineLiftLeverEvents& e,Hero&){
        for(unsigned i=0;i<e.count;++i){switch(e.actions[i].kind){
        case MineLiftLeverActionKind::Hit:++hitSerial_;leverPlayer_.play(art_->find("Lever Hit"));break;
        case MineLiftLeverActionKind::RestorePose:restored_=true;break;
        case MineLiftLeverActionKind::DisableCollider:terrain_->setEnabled(data_.leverCollider,false);break;
        case MineLiftLeverActionKind::Open:pendingOpen_=true;break;
        default:break; // ACTIVATE is not an OPEN transition in the lift FSM.
        }}
        if(e.count)feedback_=e;
    }
    void updateArt(float dt){
        leverPlayer_.advance(dt);cagePlayer_.advance(dt);if(cagePlaying_)cageAge_+=dt;
        for(unsigned i=0;i<2;++i){auto& d=bank_.draws[i];d=data_.draws[i];d.sprite=i;
            const auto* frame=i?cagePlayer_.current(*art_):leverPlayer_.current(*art_);
            if(!i&&restored_){const auto* c=art_->find("Lever Activated");if(c&&c->count&&c->first<art_->frames.size())frame=&art_->sprites[art_->frames[c->first]];}
            bank_.atlas.sprites[i]=frame?*frame:art_->sprites[data_.idleSprites[i]];
            if(i&&!openCageVisible())d.values[10]=0;
            bank_.cameraBounds[i]=roomCameraBounds(bank_.atlas.sprites[i],d);
        }
    }
    TownLiftRoomData data_;const AnimationBank* art_=nullptr;TerrainBank* terrain_=nullptr;
    MineExitLift lift_;MineLiftLever lever_;MineLiftMotion motion_;RoomVisualBank bank_;
    AnimationPlayer leverPlayer_,cagePlayer_;MineLiftLeverEvents feedback_;
    unsigned hitSerial_=0;
    float cageAge_=0;bool hidden_=false,cagePlaying_=false,restored_=false,pendingOpen_=false,wasTouching_=false,wasHead_=false;
};
}
