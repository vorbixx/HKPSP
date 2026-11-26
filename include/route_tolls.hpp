#pragma once
#include "text_pack.hpp"
#include "terrain.hpp"
#include "hero.hpp"
#include "combat.hpp"
#include "animation.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
namespace hk3ds {
struct TollMenu {bool open=false,prompt=false,yes=true;unsigned price=0;int heldX=0,heldY=0;const char* message="";const char* question=nullptr;bool crest=false,refusal=false;};
// Geometry and owner identities are loaded from the private original scene conversion.
struct TollArtState {
    struct Pose {Vec2 position{},scale{1,1};};
    std::array<Pose,2> poses{};
    unsigned count=0;
    std::array<bool,2> hidden{};
    AnimationPlayer animation;
    bool visible=false;
};
struct RouteTolls {
    float artScale(unsigned i) const {
        const char* id=kind==1?"route-tolls/city-toll-actor":kind==3?"greenpath-tolls/greenpath-toll-actor":"route-tolls/mines-toll-actor";
        return animationFacingScale(id,art.poses[i].scale.x,std::abs(art.poses[i].scale.x));
    }
    unsigned kind=0,price=0,sourceId=0,liftIndex=std::numeric_limits<unsigned>::max();
    float lower=0,upper=0,opening=0;bool paid=false,opened=false;
    std::vector<Rect> regions;
    std::vector<unsigned> hidePaid,hideUnpaid,disablePaid,disableUnpaid,movingOwners,movingShapes;
    std::array<unsigned,2> machineSources{};
    std::array<bool,2> machinePaid{};
    unsigned selectedMachine=0;
    unsigned purchaseSerial=0;
    TollMenu menu;
    TollArtState art;
    const AnimationBank* artBank=nullptr;
    Rect entrance{},lever{},heroCheck{};
    bool active()const{return kind!=0;}
    bool awaitingPayment()const{return kind==3?selectedMachine<2&&!machinePaid[selectedMachine]:!paid;}
    unsigned paymentSource()const{return kind==3?machineSources[selectedMachine]:sourceId;}
    void selectMachine(Rect body){if(kind==3&&!menu.open)for(unsigned i=0;i<regions.size();++i)
        if(boxSensorOverlap(body,regions[i])){selectedMachine=i;break;}}
    bool departureHit(const Hero& hero,const NailBank& nails)const {
        return kind==1&&paid&&opened&&hero.nail.hitbox&&
            boxSensorOverlap(bodyBounds(hero.position),heroCheck)&&
            nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],lever);
    }
    static bool contains(Rect r,Vec2 p){return p.x>=r.left&&p.x<=r.right&&p.y>=r.bottom&&p.y<=r.top;}
    bool inRange(Rect body)const {for(auto r:regions)if(body.left<r.right&&body.right>r.left&&body.bottom<r.top&&body.top>r.bottom)return true;return false;}
    bool hides(unsigned owner)const {const auto& ids=opened?hidePaid:hideUnpaid;return std::find(ids.begin(),ids.end(),owner)!=ids.end();}
    void branches(TerrainBank& terrain,bool open){
        opened=open;updateArt();for(unsigned id:disablePaid)terrain.setEnabled(id,!open);
        for(unsigned id:disableUnpaid)terrain.setEnabled(id,open);
    }
    bool load(const char* root,const char* scene,TerrainBank& terrain,bool wasPaid,bool visualOnly=false,bool otherPaid=false){
        *this={};if(std::strcmp(scene,"Crossroads_49b")&&std::strcmp(scene,"Mines_33")&&std::strcmp(scene,"Fungus1_31"))return true;
        char path[384];std::snprintf(path,sizeof(path),"%s/route-tolls/%s.bin",root,scene);
        auto* f=std::fopen(path,"rb");if(!f)return false;
        const auto read=[&](void* p,unsigned size){return std::fread(p,1,size,f)==size;};
        char magic[4];unsigned count=0;
        std::vector<TerrainShape> newShapes;
        std::vector<Vec2> newVertices;
        bool ok=read(magic,4)&&!std::memcmp(magic,"RTS4",4)&&read(&kind,4)&&read(&price,4)&&read(&sourceId,4)&&read(&count,4)&&read(&lower,4)&&read(&upper,4)&&read(&entrance,sizeof(entrance))&&read(&lever,sizeof(lever))&&read(&heroCheck,sizeof(heroCheck));
        ok=ok&&kind>=1&&kind<=3&&count>=1&&count<=2&&(kind!=3||count==2)&&price>0&&price<=10000&&std::isfinite(lower)&&std::isfinite(upper);
        if(kind==1)for(const auto r:{entrance,lever,heroCheck})
            ok=ok&&std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;
        if(ok){regions.resize(count);for(auto& r:regions){ok=ok&&read(&r,sizeof(r));ok=ok&&std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;}}
        for(auto* ids:{&hidePaid,&hideUnpaid,&disablePaid,&disableUnpaid,&movingOwners,&movingShapes}){
            count=0;ok=ok&&read(&count,4)&&count<=4096;
            if(ok){ids->resize(count);for(auto& id:*ids)ok=ok&&read(&id,4)&&id>0;}
        }
        count=0;ok=ok&&read(&count,4)&&count<=64;
        for(unsigned i=0;ok&&i<count;++i){
            unsigned identity=0,owner=0,n=0;ok=read(&identity,4)&&read(&owner,4)&&read(&n,4)&&n>=3&&n<=64;
            std::vector<Vec2> points;if(ok)points.resize(n);
            for(auto& p:points)ok=ok&&read(&p,sizeof(p))&&std::isfinite(p.x)&&std::isfinite(p.y);
            if(ok&&std::none_of(terrain.shapes.begin(),terrain.shapes.end(),[&](const TerrainShape& s){return s.id==identity;})){
                TerrainShape s;s.id=identity;s.first=unsigned(newVertices.size());s.count=n;s.flags=TerrainShape::kinematic;s.material=terrain.actorMaterial;
                s.bounds={points[0].x,points[0].y,points[0].x,points[0].y};
                for(auto p:points){s.bounds.left=std::min(s.bounds.left,p.x);s.bounds.right=std::max(s.bounds.right,p.x);s.bounds.bottom=std::min(s.bounds.bottom,p.y);s.bounds.top=std::max(s.bounds.top,p.y);newVertices.push_back(p);}
                s.queryBoundsValid=true;newShapes.push_back(s);
            }
        }
        count=0;ok=ok&&read(&count,4)&&count==(kind==1?1u:2u);
        if(ok){art.count=count;for(unsigned i=0;i<count;++i){
            auto& pose=art.poses[i];ok=ok&&read(&pose,sizeof(pose));
            ok=ok&&std::isfinite(pose.position.x)&&std::isfinite(pose.position.y)&&
                std::isfinite(pose.scale.x)&&std::isfinite(pose.scale.y)&&pose.scale.x!=0&&pose.scale.y!=0;
        }}
        if(ok&&kind==3){ok=count==2&&read(machineSources.data(),8)&&machineSources[0]>0&&machineSources[1]>0&&machineSources[0]!=machineSources[1];}
        ok=ok&&std::fgetc(f)==EOF&&(kind!=1||upper>lower)&&(kind!=3||(upper>0&&upper<=30));
        std::fclose(f);if(!ok){*this={};return false;}
        if(visualOnly){paid=opened=wasPaid;menu.price=price;menu.question=hk3ds::text::resolve(kind==1?"@Prompts/TOLLBOOTH_LIFT":kind==3?"@Prompts/TOLLBOOTH_BENCH":"@Prompts/TOLLBOOTH_GATE");updateArt();return true;}
        for(auto& shape:newShapes)shape.first+=unsigned(terrain.vertices.size());
        terrain.vertices.insert(terrain.vertices.end(),newVertices.begin(),newVertices.end());
        terrain.shapes.insert(terrain.shapes.end(),newShapes.begin(),newShapes.end());
        machinePaid={wasPaid,otherPaid};paid=wasPaid||(kind==3&&otherPaid);menu.price=price;menu.question=hk3ds::text::resolve(kind==1?"@Prompts/TOLLBOOTH_LIFT":kind==3?"@Prompts/TOLLBOOTH_BENCH":"@Prompts/TOLLBOOTH_GATE");
        if(kind==1){
            if(!(upper>lower))return false;
            TerrainLift lift;lift.owners=movingOwners;lift.lower=lower;lift.upper=upper;lift.initial=upper;lift.position.y=wasPaid?lower:upper;lift.target=lift.position.y;lift.speed=(upper-lower)/3;lift.stops={upper,lower};
            for(unsigned i=0;i<terrain.shapes.size();++i)if(std::find(movingShapes.begin(),movingShapes.end(),terrain.shapes[i].id)!=movingShapes.end()){
                lift.shapes.push_back(i);auto& s=terrain.shapes[i];s.flags|=TerrainShape::kinematic;
                const float delta=lift.position.y-upper;s.bounds.bottom+=delta;s.bounds.top+=delta;
                for(unsigned v=s.first;v<s.first+s.count;++v)terrain.vertices[v].y+=delta;
            }
            lift.presentation.reset(lift.position);liftIndex=unsigned(terrain.lifts.size());terrain.lifts.push_back(std::move(lift));
        }
        branches(terrain,paid);return true;
    }
    void bindArt(const AnimationBank* bank){artBank=bank;updateArt();}
    void updateArt(){
        art.hidden={};
        if(kind==3){art.hidden=machinePaid;art.visible=!machinePaid[0]||!machinePaid[1];}
        else art.visible=active()&&!opened;
        if(artBank)art.animation.play(artBank->find(paid&&kind!=3?"Box_disappear":"Box_idle"));
    }
    void activate(TerrainBank& terrain){
        ++purchaseSerial;
        if(kind==3)machinePaid[selectedMachine]=true;
        paid=true;updateArt();
        if(kind==1&&liftIndex<terrain.lifts.size()){
            auto& lift=terrain.lifts[liftIndex];lift.target=lower;lift.moving=true;opening=4;
        }else if(kind==3&&!opened)opening=upper;
        else branches(terrain,true);
    }
    void step(float seconds,TerrainBank& terrain){if(art.visible){art.animation.advance(seconds);if(kind!=3&&paid&&art.animation.complete)art.visible=false;}if(opening>0){opening=std::max(0.f,opening-seconds);if(opening==0)branches(terrain,true);}}
};
}
