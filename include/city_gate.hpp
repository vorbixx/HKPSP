#pragma once
#include "terrain.hpp"
#include "route_tolls.hpp"
#include <cstdio>
#include <cstring>
#include <limits>

namespace hk3ds {
// Source: Fungus2_21 Conversation Control 5567 and Open 5613 (build22529139).
// Geometry/text come from scene_city_gate.py; motion uses the resident lift adapter.
struct CityGate {
    enum class Phase { Closed, Insert, Open1, Pause, Open2, BumpDown, BumpUp, Open, Drop, Dropped, Slam, Slammed };
    bool active=false,opened=false,closed=false;
    Phase phase=Phase::Closed;
    Rect inspect{},drop{},slam{};
    Vec2 respawn{};
    unsigned lift=~0u,solid=~0u,blocker=~0u,soundSerial=0,sound=0;
    float elapsed=0,offset=0;
    char noKey[256]{},withKey[256]{};
    TollMenu menu;
    bool load(const char* root,const char* scene,TerrainBank& terrain,bool wasOpened,bool wasClosed){
        *this={};
        if(std::strcmp(scene,"Fungus2_21"))return true;
        char path[384];std::snprintf(path,sizeof(path),"%s/scenes/Fungus2_21/city-gate.bin",root);
        FILE* file=std::fopen(path,"rb");if(!file)return false;
        auto read=[&](void* out,std::size_t size){return std::fread(out,1,size,file)==size;};
        char magic[4];bool ok=read(magic,4)&&!std::memcmp(magic,"CGT1",4)&&read(&inspect,sizeof(inspect))&&
            read(&drop,sizeof(drop))&&read(&slam,sizeof(slam))&&read(&respawn,sizeof(respawn));
        unsigned indices[2]{};
        for(unsigned i=0;i<2&&ok;++i){
            unsigned id=0;Rect bounds{};ok=read(&id,4)&&read(&bounds,sizeof(bounds))&&
                std::isfinite(bounds.left)&&std::isfinite(bounds.bottom)&&std::isfinite(bounds.right)&&std::isfinite(bounds.top)&&
                bounds.left<bounds.right&&bounds.bottom<bounds.top;
            if(!ok)break;
            auto found=std::find_if(terrain.shapes.begin(),terrain.shapes.end(),[&](const TerrainShape& s){return s.id==id;});
            if(found==terrain.shapes.end()){
                TerrainShape shape;shape.id=id;shape.first=unsigned(terrain.vertices.size());shape.count=4;
                shape.flags=TerrainShape::kinematic;shape.bounds=bounds;shape.queryBoundsValid=true;shape.material=terrain.actorMaterial;
                terrain.vertices.insert(terrain.vertices.end(),{{bounds.left,bounds.bottom},{bounds.right,bounds.bottom},
                    {bounds.right,bounds.top},{bounds.left,bounds.top}});
                indices[i]=unsigned(terrain.shapes.size());terrain.shapes.push_back(shape);
            }else indices[i]=unsigned(found-terrain.shapes.begin());
        }
        unsigned count=0;ok=ok&&read(&count,4)&&count>0&&count<=64;
        TerrainLift motion;motion.owners.resize(ok?count:0);
        if(ok)ok=read(motion.owners.data(),count*4)&&read(noKey,256)&&read(withKey,256)&&
            noKey[255]==0&&withKey[255]==0&&std::fgetc(file)==EOF;
        std::fclose(file);if(!ok)return false;
        active=true;opened=wasOpened;closed=wasClosed;solid=indices[0];blocker=indices[1];
        motion.scripted=true;motion.lower=0;motion.upper=9;motion.initial=0;motion.speed=30;motion.shapes.push_back(solid);
        lift=unsigned(terrain.lifts.size());terrain.lifts.push_back(std::move(motion));
        phase=closed?Phase::Slammed:opened?Phase::Open:Phase::Closed;
        pose(terrain,opened&&!closed?9.f:0.f,true);terrain.shapes[blocker].enabled=closed;
        return true;
    }
    void pose(TerrainBank& terrain,float y,bool reset=false){
        if(!active||lift>=terrain.lifts.size())return;
        auto& motion=terrain.lifts[lift];
        const float delta=y-offset;auto& shape=terrain.shapes[solid];
        shape.bounds.bottom+=delta;shape.bounds.top+=delta;
        for(unsigned i=0;i<shape.count;++i)terrain.vertices[shape.first+i].y+=delta;
        PresentationPosition::Step step(motion.presentation,motion.position);motion.position.y=y;motion.target=y;
        if(reset)motion.presentation.reset(motion.position);
        offset=y;
    }
    void cue(unsigned id){sound=id;++soundSerial;}
    void activate(){opened=true;phase=Phase::Insert;elapsed=0;}
    bool moving()const{return active&&phase!=Phase::Closed&&phase!=Phase::Open&&phase!=Phase::Dropped&&phase!=Phase::Slammed;}
    // Returns the original Slam save boundary. It is consumed once by the session.
    bool step(float seconds,TerrainBank& terrain,const Hero& hero){
        if(!active)return false;
        auto trigger=[&](Rect r){r.bottom+=offset;r.top+=offset;return boxSensorOverlap(bodyBounds(hero.position),r);};
        if(opened&&phase!=Phase::Insert&&phase!=Phase::Slam&&phase!=Phase::Slammed){
            if(trigger(slam)){
                closed=true;phase=Phase::Slam;elapsed=0;terrain.shapes[blocker].enabled=true;return true;
            }
        }
        if(phase==Phase::Open||phase==Phase::Dropped){
            if(phase==Phase::Open&&trigger(drop)){phase=Phase::Drop;elapsed=0;cue(4);}
        }
        elapsed+=seconds;
        switch(phase){
        case Phase::Insert:if(elapsed-seconds<.5f&&elapsed>=.5f)cue(1);if(elapsed>=2){phase=Phase::Open1;elapsed=0;cue(2);}break; // YES .5 + Activate 1.5.
        case Phase::Open1:pose(terrain,2*std::min(elapsed/1.5f,1.f));if(elapsed>=2){phase=Phase::Open2;elapsed=0;}break;
        case Phase::Open2:pose(terrain,2+7*std::min(elapsed/1.5f,1.f));if(elapsed>=1.5f){phase=Phase::BumpDown;elapsed=0;cue(3);}break;
        case Phase::BumpDown:pose(terrain,9-std::min(elapsed/.75f,1.f));if(elapsed>=.75f){phase=Phase::BumpUp;elapsed=0;}break;
        case Phase::BumpUp:pose(terrain,8+std::min(elapsed/1.5f,1.f));if(elapsed>=1.5f){phase=Phase::Open;elapsed=0;}break;
        case Phase::Drop:pose(terrain,9-2*std::min(elapsed,1.f));if(elapsed>=1){phase=Phase::Dropped;elapsed=0;}break;
        case Phase::Slam:pose(terrain,std::max(0.049375f,offset-30*seconds));if(offset<=.049375f){phase=Phase::Slammed;cue(6);}break;
        default:break;
        }
        return false;
    }
};
}
