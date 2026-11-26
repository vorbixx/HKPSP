#pragma once
#include "room.hpp"
#include "world_switches.hpp"
#include "world_switch_terrain.hpp"
#include "world_hazard_shapes.hpp"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hk3ds {
// Optional companion to scene.bin; modes follow its gate order. Commit only
// after validating every record, so a bad room never replaces live rules.
inline bool loadWorldGateInputs(const char* path,RoomConfig& rules)
{
    auto* file=std::fopen(path,"rb");if(!file)return errno==ENOENT;
    char magic[4];unsigned count=0;std::array<unsigned,32> modes{};
    bool ok=std::fread(magic,1,4,file)==4&&!std::memcmp(magic,"HKG1",4)&&
        std::fread(&count,4,1,file)==1&&count==rules.gates.size()&&count<=modes.size();
    if(ok)ok=std::fread(modes.data(),4,count,file)==count;
    if(ok)for(unsigned i=0;i<count;++i)ok=ok&&(modes[i]==1||modes[i]==2);
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(!ok)return false;
    for(unsigned i=0;i<count;++i)rules.gates[i].entryKind=modes[i];
    return true;
}
// HeroController coroutine constants and hero geometry are shared across rooms.
// Scene-authored hazards, checkpoints and entry placement remain room-local.
inline void inheritHeroHazardRules(RoomConfig& room,const RoomConfig& heroRules)
{
    room.hazardFadeWait=heroRules.hazardFadeWait;
    room.hazardWakeWait=heroRules.hazardWakeWait;
    room.hazardInvulnerability=heroRules.hazardInvulnerability;
    room.groundProbe=heroRules.groundProbe;room.heroHurtbox=heroRules.heroHurtbox;
}
// Optional for old room packages. A present but malformed bank fails admission.
inline bool loadWorldObstacles(const char* path,RoomConfig& rules)
{
    auto* file=std::fopen(path,"rb");if(!file)return errno==ENOENT;
    char magic[4];unsigned hazards=0,checkpoints=0;
    auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
    auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=100000;};
    auto rect=[&](Rect r){return finite(r.left)&&finite(r.right)&&finite(r.bottom)&&finite(r.top)&&r.left<r.right&&r.bottom<r.top;};
    bool ok=read(magic,4);
    const bool polygons=ok&&!std::memcmp(magic,"HKO3",4);
    const bool shadeFlags=polygons||(ok&&!std::memcmp(magic,"HKO2",4));
    ok=ok&&(shadeFlags||!std::memcmp(magic,"HKO1",4))&&read(&hazards,4)&&read(&checkpoints,4)&&hazards<=128&&checkpoints<=128;
    std::vector<RoomHazard> nextHazards(ok?hazards:0);
    std::vector<RoomCheckpoint> nextCheckpoints(ok?checkpoints:0);
    for(auto& h:nextHazards)ok=ok&&read(&h.trigger,sizeof(Rect))&&read(&h.damage,4)&&read(&h.type,4)&&rect(h.trigger)&&h.damage>0&&h.damage<=100&&(h.hazardType()==2||h.hazardType()==3)&&(shadeFlags||h.type==h.hazardType());
    for(auto& c:nextCheckpoints)ok=ok&&read(&c.trigger,sizeof(Rect))&&read(&c.marker,sizeof(Vec2))&&read(&c.flags,4)&&rect(c.trigger)&&finite(c.marker.x)&&finite(c.marker.y)&&c.flags<=3;
    std::vector<TriggerPolygon> nextShapes;
    if(ok&&polygons)ok=readWorldHazardShapes(file,nextHazards,nextShapes);
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(!ok)return false;
    rules.hazardShapes=std::move(nextShapes);
    rules.hazards=std::move(nextHazards);rules.checkpoints=std::move(nextCheckpoints);return true;
}
}
