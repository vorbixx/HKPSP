#include "terrain.hpp"
#include <cassert>
#include "hero.hpp"
#include "combat.hpp"
#include "room_visual.hpp"
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
namespace hk3ds
{
bool TerrainBank::load(const char* path)
{
    auto* f=std::fopen(path,"rb"); if (!f) return false;
    const auto read=[f](void* out,std::size_t bytes) { return std::fread(out,1,bytes,f)==bytes; };
    char magic[4]; std::uint32_t h[4]; float meta[10];
    bool ok=read(magic,4) && std::memcmp(magic,"HKT1",4)==0 && read(h,sizeof(h)) && read(meta,sizeof(meta));
    if (!ok || !h[0] || h[0]>terrainShapeCapacity || !h[1] || h[1]>terrainVertexCapacity || h[2]!=1 || h[3]!=0)
    { std::fclose(f); return false; }
    TerrainBank next;
    next.shapes.resize(h[0]); next.vertices.resize(h[1]);
    std::uint32_t offset=0;
    for (auto& s:next.shapes)
    {
        std::uint32_t a[5]; float m[2];
        if (!read(a,sizeof(a)) || !read(m,sizeof(m))) { ok=false; break; }
        s.id=a[0]; s.kind=a[1]; s.flags=a[2]; s.first=a[3]; s.count=a[4]; s.material={m[0],m[1]};
        if (!s.id || s.kind>1 || s.flags>4095 || s.first!=offset || s.count<(s.kind==0?3u:2u) || s.count>512 ||
            s.first>h[1] || s.count>h[1]-s.first || !std::isfinite(m[0]) || m[0]<0 || m[0]>100 || m[1]!=0) ok=false;
        offset+=s.count;
    }
    for (auto& p:next.vertices) if (!read(&p,sizeof(p))) { ok=false; break; }
    ok=ok && offset==h[1] && std::fgetc(f)==EOF; std::fclose(f);
    for (float x:meta) if (!std::isfinite(x) || std::abs(x)>100000) ok=false;
    for (const auto& p:next.vertices) if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x)>100000 || std::abs(p.y)>100000) ok=false;
    if (!ok || meta[2]>=meta[4] || meta[3]>=meta[5] || meta[6]<0 || meta[6]>100 || meta[8]<0 || meta[8]>100 || meta[7]!=0 || meta[9]!=0) return false;
    for (auto& s:next.shapes)
    {
        const auto* v=next.vertices.data()+s.first;
        s.bounds={v[0].x,v[0].y,v[0].x,v[0].y}; double area=0;
        for (std::uint32_t i=0;i<s.count;++i)
        {
            const auto p=v[i], q=v[(i+1)%s.count];
            s.bounds.left=std::min(s.bounds.left,p.x); s.bounds.right=std::max(s.bounds.right,p.x);
            s.bounds.bottom=std::min(s.bounds.bottom,p.y); s.bounds.top=std::max(s.bounds.top,p.y);
            if ((s.kind==0 || i+1<s.count) && p.x==q.x && p.y==q.y) return false;
            area+=static_cast<double>(p.x)*q.y-static_cast<double>(p.y)*q.x;
        }
        if (s.kind==0 && area<=0) return false;
        s.queryBoundsValid=true;
    }
    next.crawlerSpawn={meta[0],meta[1]}; next.region={meta[2],meta[3],meta[4],meta[5]};
    next.actorMaterial={meta[6],meta[7]}; next.corpseMaterial={meta[8],meta[9]};
    next.sourceContacts=true;
    const bool motion=std::any_of(next.shapes.begin(),next.shapes.end(),[](const TerrainShape& s){return s.flags&TerrainShape::kinematic;});
    if(motion){
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"terrain-motion.bin";
        auto* mf=std::fopen(filename.c_str(),"rb");if(!mf)return false;
        const auto mr=[mf](void* out,std::size_t size){return std::fread(out,1,size,mf)==size;};
        char tag[4];std::uint32_t version=0,count=0;
        bool valid=mr(tag,4)&&!std::memcmp(tag,"HKM1",4)&&mr(&version,4)&&(version>=3&&version<=6)&&mr(&count,4)&&count>0&&count<=8;
        std::vector<unsigned> assigned;
        for(unsigned i=0;valid&&i<count;++i){
            TerrainLift lift;std::uint32_t n=0;float data[9]{};
            if(version>=5){
                unsigned kind=0;valid=mr(&kind,4)&&kind<=(version>=6?2u:1u)&&mr(&lift.key,sizeof(lift.key))&&mr(&lift.acceleration,4);
                lift.falling=kind!=0;lift.saveOnLand=kind==2;
                valid=valid&&std::isfinite(lift.acceleration)&&
                    (lift.falling?(lift.key.scene&&lift.key.object&&lift.acceleration>0&&lift.acceleration<=1000):
                     (!lift.key.scene&&!lift.key.object&&lift.acceleration==0));
                if(!valid)break;
            }
            valid=mr(&n,4)&&n>0&&n<=16&&mr(data,sizeof(data));
            for(float value:data)if(!std::isfinite(value)||std::abs(value)>100000)valid=false;
            if(!valid)break;
            lift.lower=data[0];lift.upper=data[1];lift.speed=data[2];lift.initial=data[3];lift.position.y=data[4];
            lift.target=lift.position.y;lift.upperControl={data[5],data[6]};lift.lowerControl={data[7],data[8]};
            valid=lift.lower<lift.upper&&lift.speed>0&&lift.speed<=100&&lift.initial==lift.position.y;
            if(version>=4){
                unsigned stops=0;valid=valid&&mr(&stops,4)&&stops>=2&&stops<=8;
                for(unsigned j=0;valid&&j<stops;++j){
                    float value=0;valid=mr(&value,4)&&std::isfinite(value)&&std::abs(value)<=100000&&
                        (lift.stops.empty()||value<lift.stops.back());
                    if(valid)lift.stops.push_back(value);
                }
                valid=valid&&!lift.stops.empty()&&lift.stops.front()==lift.upper&&lift.stops.back()==lift.lower;
            }else lift.stops={lift.upper,lift.lower};
            valid=valid&&std::find(lift.stops.begin(),lift.stops.end(),lift.initial)!=lift.stops.end();
            lift.goingDown=lift.initial==lift.upper;
            lift.controls.resize(lift.stops.size()+1);
            for(unsigned j=0;valid&&j<n;++j){
                unsigned id=0;valid=mr(&id,4);auto found=std::find_if(next.shapes.begin(),next.shapes.end(),[id](const TerrainShape& s){return s.id==id;});
                valid=valid&&found!=next.shapes.end()&&(found->flags&TerrainShape::kinematic)&&std::find(assigned.begin(),assigned.end(),id)==assigned.end();
                if(valid){lift.shapes.push_back(static_cast<unsigned>(found-next.shapes.begin()));assigned.push_back(id);}
            }
            for(auto* ids:{&lift.owners,&lift.chains}){
                unsigned size=0;if(!valid||!mr(&size,4)||size>4096){valid=false;break;}
                for(unsigned j=0;valid&&j<size;++j){unsigned id=0;valid=mr(&id,4)&&id>0&&std::find(ids->begin(),ids->end(),id)==ids->end();if(valid)ids->push_back(id);}
            }
            for(auto& control:lift.controls){
                unsigned size=0;if(!valid||!mr(&size,4)||size<3||size>16){valid=false;break;}
                for(unsigned j=0;valid&&j<size;++j){Vec2 p{};valid=mr(&p,sizeof(p))&&std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<=100000&&std::abs(p.y)<=100000;if(valid)control.push_back(p);}
            }
            lift.presentation.reset(lift.position);if(valid)next.lifts.push_back(std::move(lift));
        }
        valid=valid&&std::fgetc(mf)==EOF;std::fclose(mf);
        for(const auto& s:next.shapes)if((s.flags&TerrainShape::kinematic)&&std::find(assigned.begin(),assigned.end(),s.id)==assigned.end())valid=false;
        if(!valid)return false;
    }
    if(std::any_of(next.shapes.begin(),next.shapes.end(),[](const TerrainShape& s){return s.flags&TerrainShape::breakableWall;})){
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"terrain-walls.bin";
        auto* wf=std::fopen(filename.c_str(),"rb");if(!wf)return false;
        const auto wr=[wf](void* out,std::size_t size){return std::fread(out,1,size,wf)==size;};
        char tag[4];unsigned version=0,count=0;
        bool valid=wr(tag,4)&&!std::memcmp(tag,"HKW1",4)&&wr(&version,4)&&(version==2||version==3||version==4||version==5||version==6)&&wr(&count,4)&&count>0&&count<=8;
        const long payload=std::ftell(wf);
        const bool headerValid=valid;
        const auto readWalls=[&](bool prefixLayout,bool suffixLayout,bool interactionLayout=true){
        valid=headerValid&&payload>=0&&std::fseek(wf,payload,SEEK_SET)==0;
        next.walls.clear();
        std::vector<unsigned> assigned;
        for(unsigned i=0;valid&&i<count;++i){
            unsigned values[7]{};valid=wr(values,sizeof(values));if(!valid)break;
            TerrainWall wall;wall.root=values[0];wall.scene=values[1];wall.object=values[2];wall.playerData=values[3];wall.hits=values[4];wall.facing=values[5];
            float origin[3]{};valid=wr(origin,sizeof(origin));if(!valid)break;
            for(float v:origin)valid=valid&&std::isfinite(v);
            if(!valid)break;
            wall.origin={origin[0],origin[1]};wall.depth=origin[2];
            if(version>=3){
                unsigned policy=0;
                valid=wr(&policy,4)&&policy<=TerrainWall::DeactivateLeftOf&&wr(&wall.entryX,4)&&
                    std::isfinite(wall.entryX)&&std::abs(wall.entryX)<=100000;
                if(!valid)break;
                wall.entryPolicy=static_cast<TerrainWall::EntryPolicy>(policy);
            }
            if(prefixLayout){
                unsigned oneWay=0;float boxes[8]{};valid=wr(&oneWay,4)&&oneWay<=1&&wr(boxes,sizeof(boxes));
                for(float value:boxes)valid=valid&&std::isfinite(value)&&std::abs(value)<=100000;
                wall.oneWay=oneWay!=0;wall.hitBounds={boxes[0],boxes[1],boxes[2],boxes[3]};
                wall.heroRange={boxes[4],boxes[5],boxes[6],boxes[7]};
                if(wall.oneWay)valid=valid&&boxes[0]<boxes[2]&&boxes[1]<boxes[3]&&boxes[4]<boxes[6]&&boxes[5]<boxes[7];
            }
            if(!valid)break;
            if(interactionLayout&&version>=6){
                unsigned kind=0;
                valid=wr(&kind,4)&&kind<=3&&wr(&wall.nailTriggerCount,4)&&wall.nailTriggerCount<=8&&
                    wr(wall.nailTrigger.data(),wall.nailTriggerCount*sizeof(Vec2))&&
                    wr(&wall.collisionDelay,4)&&wr(&wall.hideDelay,4);
                if(!valid)break;
                wall.rejectDown=(kind&2)!=0;wall.kind=static_cast<TerrainWall::Kind>(kind&1);
                for(unsigned p=0;p<wall.nailTriggerCount;++p)valid=valid&&
                    std::isfinite(wall.nailTrigger[p].x)&&std::isfinite(wall.nailTrigger[p].y)&&
                    std::abs(wall.nailTrigger[p].x)<=100000&&std::abs(wall.nailTrigger[p].y)<=100000;
                valid=valid&&std::isfinite(wall.collisionDelay)&&std::isfinite(wall.hideDelay)&&
                    wall.collisionDelay>=0&&wall.hideDelay>=wall.collisionDelay&&wall.hideDelay<=30;
                if(wall.kind==TerrainWall::MantisGate)valid=valid&&wall.collisionDelay>0&&wall.playerData==0&&
                    wall.entryPolicy==TerrainWall::Always&&wall.nailTriggerCount>=3;
                if(wall.kind==TerrainWall::MantisGate){
                    float winding=0;
                    for(unsigned p=0;p<wall.nailTriggerCount;++p){
                        const auto a=wall.nailTrigger[p],b=wall.nailTrigger[(p+1)%wall.nailTriggerCount],c=wall.nailTrigger[(p+2)%wall.nailTriggerCount];
                        const float cross=(b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
                        valid=valid&&std::isfinite(cross)&&std::abs(cross)>1e-8f&&(!p||cross*winding>0);
                        // Every vertex must share the interior half-plane of each edge.
                        // Local turn signs alone also accept self-intersecting stars.
                        for(unsigned q=0;valid&&q<wall.nailTriggerCount;++q){
                            if(q==p||q==(p+1)%wall.nailTriggerCount)continue;
                            const auto point=wall.nailTrigger[q];
                            const float side=(b.x-a.x)*(point.y-a.y)-(b.y-a.y)*(point.x-a.x);
                            valid=std::isfinite(side)&&std::abs(side)>1e-8f&&side*cross>0;
                        }
                        winding=cross;
                    }
                }
                if(!valid)break;
            }
            if(interactionLayout&&version>=5){
                unsigned triangles=0;
                valid=wr(&wall.hitWait,4)&&std::isfinite(wall.hitWait)&&wall.hitWait>0&&wall.hitWait<=1&&wr(&triangles,4)&&triangles<=8;
                for(unsigned j=0;valid&&j<triangles;++j){
                    std::array<Vec2,3> triangle{};valid=wr(triangle.data(),sizeof(triangle));
                    for(const auto p:triangle)valid=valid&&std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<=100000&&std::abs(p.y)<=100000;
                    const auto a=triangle[0],b=triangle[1],c=triangle[2];
                    valid=valid&&std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))>0;
                    if(valid)wall.hitTriangles.push_back(triangle);
                }
                if(!valid)break;
            }
            const bool prefixOneWay=wall.oneWay;
            const auto shape=std::find_if(next.shapes.begin(),next.shapes.end(),[&](const TerrainShape& s){return s.id==values[6];});
            valid=wall.root&&wall.scene&&wall.object&&wall.hits>0&&wall.hits<=16&&wall.facing<=3&&shape!=next.shapes.end()&&
                (shape->flags&TerrainShape::breakableWall)&&shape->kind==0&&shape->count==4&&
                std::find(assigned.begin(),assigned.end(),values[6])==assigned.end();
            if(!valid)break;
            wall.shape=unsigned(shape-next.shapes.begin());assigned.push_back(values[6]);
            for(auto* owners:{&wall.children,&wall.locks}){
                unsigned n=0;valid=valid&&wr(&n,4)&&n<=128;
                for(unsigned j=0;valid&&j<n;++j){unsigned id=0;valid=wr(&id,4)&&id>0&&id!=wall.root&&std::find(owners->begin(),owners->end(),id)==owners->end();if(valid)owners->push_back(id);}
            }
            if(suffixLayout){
                unsigned kind=0,n=0;
                valid=valid&&wr(&kind,4)&&kind<=(version>=5?2u:1u)&&wr(&wall.contactDelay,4)&&
                    std::isfinite(wall.contactDelay)&&wall.contactDelay>=0&&wall.contactDelay<=10&&
                    wr(&n,4)&&n<=8;
                wall.contactFloor=kind==1;wall.oneWay=wall.oneWay||kind==2;
                valid=valid&&(wall.contactFloor?(wall.contactDelay>0&&n>0&&wall.hits==1&&wall.facing==0&&
                    wall.entryPolicy==TerrainWall::Always&&wall.playerData==0):(wall.contactDelay==0&&(kind==2?(n==1&&wall.hits==3&&wall.playerData==0&&wall.entryPolicy==TerrainWall::Always&&(shape->flags&1u)):(n==0||wall.oneWay))));
                // A one-way break floor (Crossroads_09) may own extra Solid boxes; they break with it.
                for(unsigned j=0;valid&&j<n;++j){
                    unsigned id=0;valid=wr(&id,4);
                    const auto linked=std::find_if(next.shapes.begin(),next.shapes.end(),[&](const TerrainShape& s){return s.id==id;});
                    valid=valid&&linked!=next.shapes.end()&&linked!=shape&&linked->kind==0&&linked->count==4&&
                        !(linked->flags&(TerrainShape::breakableWall|TerrainShape::leverGate))&&
                        std::find(assigned.begin(),assigned.end(),id)==assigned.end();
                    if(valid){
                        const unsigned index=unsigned(linked-next.shapes.begin());
                        valid=std::find(wall.linkedShapes.begin(),wall.linkedShapes.end(),index)==wall.linkedShapes.end();
                        if(valid){wall.linkedShapes.push_back(index);assigned.push_back(id);}
                    }
                }
            }
            if(suffixLayout&&version>=5){
                Rect range{};valid=valid&&wr(&range,sizeof(range));
                const auto& r=range;
                valid=valid&&std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&std::isfinite(r.top)&&
                    (wall.oneWay&&!prefixOneWay?(r.left<r.right&&r.bottom<r.top):(r.left==0&&r.bottom==0&&r.right==0&&r.top==0));
                if(wall.oneWay&&!prefixOneWay)wall.heroRange=range;
            }
            next.walls.push_back(std::move(wall));
        }
        valid=valid&&std::fgetc(wf)==EOF;
        for(const auto& s:next.shapes)if((s.flags&TerrainShape::breakableWall)&&std::find(assigned.begin(),assigned.end(),s.id)==assigned.end())valid=false;
        return valid;
        };
        // Legacy layouts and the combined v6 layout are both accepted. Each
        // retry rewinds to the payload and validates EOF before committing.
        if(version==4){valid=readWalls(true,false);if(!valid)valid=readWalls(false,true);}
        else if(version==5){valid=readWalls(true,false);if(!valid)valid=readWalls(false,true,false);}
        else if(version==6){valid=readWalls(true,true);if(!valid)valid=readWalls(true,false);if(!valid)valid=readWalls(true,true,false);}
        else valid=readWalls(false,false);
        std::fclose(wf);
        if(!valid)return false;
    }
    const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
    const auto maskPath=geometry.substr(0,slash==std::string::npos?0:slash+1)+"room-masks.bin";
    if(!loadRoomMasks(maskPath.c_str(),next.masks))return false;
    if(std::any_of(next.shapes.begin(),next.shapes.end(),[](const TerrainShape& s){return s.flags&TerrainShape::leverGate;})){
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"terrain-levers.bin";
        auto* file=std::fopen(filename.c_str(),"rb");if(!file)return false;
        const auto readLever=[file](void* out,std::size_t bytes){return std::fread(out,1,bytes,file)==bytes;};
        char tag[4];unsigned version=0,count=0;
        bool valid=readLever(tag,4)&&!std::memcmp(tag,"HKL1",4)&&readLever(&version,4)&&(version==1||version==2)&&
                   readLever(&count,4)&&count>0&&count<=8;
        std::vector<unsigned> assigned;
        for(unsigned i=0;valid&&i<count;++i){
            unsigned ids[6];float values[9];TerrainLever lever;
            valid=readLever(ids,sizeof(ids))&&readLever(values,sizeof(values));if(!valid)break;
            lever.owner=ids[0];lever.gate=ids[1];lever.key={ids[3],ids[4]};lever.count=ids[5];
            if(version==2){
                unsigned extra[4];float fps=0;
                valid=readLever(extra,sizeof(extra))&&readLever(&fps,4)&&extra[0]<=1&&std::isfinite(fps);
                if(!valid)break;
                lever.state.pillar=extra[0];lever.switchKey={extra[1],extra[2]};lever.playerDataKey=extra[3];lever.state.shakeFps=fps;
                valid=lever.state.pillar?extra[1]&&extra[2]&&extra[3]&&fps>0&&fps<=120:
                    !extra[1]&&!extra[2]&&fps==0;
                if(!valid)break;
            }
            valid=lever.owner&&lever.gate&&lever.owner!=lever.gate&&lever.key.scene&&lever.key.object&&
                  lever.count>=3&&lever.count<=lever.polygon.size();
            for(float value:values)valid=valid&&std::isfinite(value)&&std::fabs(value)<=100000;
            lever.range={values[0],values[1],values[2],values[3]};
            valid=valid&&values[0]<values[2]&&values[1]<values[3];
            for(unsigned j=0;j<lever.state.seconds.size();++j){
                lever.state.seconds[j]=values[4+j];valid=valid&&values[4+j]>=0&&values[4+j]<30;
            }
            // Ruins1_05b's Toll Gate has one Gate Open clip and no Dust or
            // second clip phase. Artwork validation below requires a positive
            // first clip and matching duration for every admitted animation.
            valid=valid&&lever.state.seconds[0]>0&&lever.state.finishTime()>0;
            const auto shape=std::find_if(next.shapes.begin(),next.shapes.end(),[&](const TerrainShape& s){return s.id==ids[2];});
            valid=valid&&shape!=next.shapes.end()&&std::find(assigned.begin(),assigned.end(),ids[2])==assigned.end();
            if(valid)valid=(shape->flags&TerrainShape::leverGate)&&shape->kind==0&&shape->count==4;
            for(const auto& previous:next.levers)valid=valid&&previous.gate!=lever.gate;
            if(!valid)break;
            lever.shape=static_cast<unsigned>(shape-next.shapes.begin());assigned.push_back(ids[2]);
            for(unsigned j=0;valid&&j<lever.count;++j){
                float point[2];valid=readLever(point,sizeof(point))&&std::isfinite(point[0])&&std::isfinite(point[1])&&
                                      std::fabs(point[0])<=100000&&std::fabs(point[1])<=100000;
                if(valid)lever.polygon[j]={point[0],point[1]};
            }
            if(valid){
                bool positive=false,negative=false;
                for(unsigned j=0;j<lever.count;++j){
                    const auto a=lever.polygon[j],b=lever.polygon[(j+1)%lever.count],c=lever.polygon[(j+2)%lever.count];
                    const float turn=(b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
                    positive=positive||turn>0;negative=negative||turn<0;
                }
                valid=positive!=negative;
            }
            for(const auto& previous:next.levers)if(previous.owner==lever.owner){
                // A source switch may address several gates. Repeated owners
                // must describe the identical non-pillar trigger and save key.
                valid=valid&&!previous.state.pillar&&!lever.state.pillar&&
                    previous.key.scene==lever.key.scene&&previous.key.object==lever.key.object&&
                    previous.count==lever.count&&previous.range.left==lever.range.left&&
                    previous.playerDataKey==lever.playerDataKey&&
                    previous.range.bottom==lever.range.bottom&&previous.range.right==lever.range.right&&
                    previous.range.top==lever.range.top&&previous.state.seconds==lever.state.seconds;
                for(unsigned j=0;valid&&j<lever.count;++j)
                    valid=previous.polygon[j].x==lever.polygon[j].x&&previous.polygon[j].y==lever.polygon[j].y;
            }
            if(valid)next.levers.push_back(lever);
        }
        for(const auto& shape:next.shapes)if((shape.flags&TerrainShape::leverGate)&&
            std::find(assigned.begin(),assigned.end(),shape.id)==assigned.end())valid=false;
        valid=valid&&std::fgetc(file)==EOF;std::fclose(file);if(!valid)return false;
    }
    if(!next.levers.empty()){
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"room-levers.bin";
        if(auto* file=std::fopen(filename.c_str(),"rb")){
            const auto read=[file](void* out,std::size_t bytes){return std::fread(out,1,bytes,file)==bytes;};
            char magic[4]{};unsigned count=0;
            bool valid=read(magic,4)&&(!std::memcmp(magic,"HKA1",4)||!std::memcmp(magic,"HKA2",4))&&read(&count,4)&&count==next.levers.size();
            const bool extended=!std::memcmp(magic,"HKA2",4);
            for(unsigned i=0;valid&&i<count;++i){
                unsigned ids[3];valid=read(ids,sizeof(ids));if(!valid)break;
                auto lever=std::find_if(next.levers.begin(),next.levers.end(),[&](const TerrainLever& l){return l.gate==ids[0];});
                if(lever!=next.levers.end()&&lever->state.pillar){
                valid=lever!=next.levers.end()&&lever->frames.empty()&&ids[2]>=3&&
                      ids[2]<=(lever->state.pillar?130u:129u)&&
                      ids[1]>0&&ids[1]<=64;
                if(valid){const unsigned tail=lever->state.pillar?2:1;
                    valid=ids[1]+tail<ids[2]&&ids[2]-ids[1]-tail<=64&&(!lever->state.pillar||extended);}
                }else{
                valid=lever!=next.levers.end()&&lever->frames.empty()&&
                      (ids[2]>=2&&ids[2]<=129&&ids[1]>0&&ids[1]<=64&&ids[1]<=ids[2]-1&&
                       ids[2]-ids[1]-1<=64&&lever->state.seconds[3]>0&&
                       ((ids[1]==ids[2]-1)==(lever->state.seconds[4]==0)));
                }
                if(!valid)break;
                lever->firstFrames=ids[1];lever->frames.resize(ids[2]);
                valid=read(lever->frames.data(),lever->frames.size()*sizeof(unsigned));
                for(unsigned frame:lever->frames)valid=valid&&frame<65536;
                if(extended){
                    unsigned frames=0,idle=0;float fps=0,idleFps=0;
                    valid=valid&&read(&frames,4)&&read(&idle,4)&&read(&idleFps,4)&&read(&fps,4)&&
                          std::isfinite(fps)&&std::isfinite(idleFps)&&
                          (lever->state.pillar?(idle>0&&idle<=64&&frames>idle+2&&frames<=idle+66&&
                           fps>0&&fps<=120&&idleFps>0&&idleFps<=120):(frames==0&&idle==0&&fps==0&&idleFps==0));
                    if(!valid)break;
                    lever->switchFps=fps;lever->switchIdleFps=idleFps;lever->switchIdleFrames=idle;lever->switchFrames.resize(frames);
                    valid=read(lever->switchFrames.data(),frames*sizeof(unsigned));
                    for(unsigned frame:lever->switchFrames)valid=valid&&frame<65536;
                }
            }
            valid=valid&&std::fgetc(file)==EOF;std::fclose(file);if(!valid)return false;
        }else return false;
    }
    {
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"scene-shrooms.bin";
        if(auto* file=std::fopen(filename.c_str(),"rb")){
            const auto read=[file](void* out,std::size_t bytes){return std::fread(out,1,bytes,file)==bytes;};
            char magic[4];unsigned count=0;
            bool valid=read(magic,4)&&!std::memcmp(magic,"HSB1",4)&&read(&count,4)&&count<=256;
            for(unsigned i=0;valid&&i<count;++i){
                TerrainShroom shroom;float circle[3];
                valid=read(&shroom.owner,4)&&read(&shroom.collider,4)&&read(circle,sizeof(circle));
                if(!valid)break;
                valid=shroom.owner&&shroom.collider&&std::isfinite(circle[0])&&std::isfinite(circle[1])&&
                      std::isfinite(circle[2])&&circle[2]>0&&circle[2]<100;
                for(const auto& previous:next.shrooms)valid=valid&&previous.collider!=shroom.collider;
                shroom.center={circle[0],circle[1]};shroom.radius=circle[2];next.shrooms.push_back(shroom);
            }
            valid=valid&&std::fgetc(file)==EOF;std::fclose(file);if(!valid)return false;
        }
    }
    {
        const std::string geometry(path);const auto slash=geometry.find_last_of("/\\");
        const auto filename=geometry.substr(0,slash==std::string::npos?0:slash+1)+"room-shrooms.bin";
        if(auto* file=std::fopen(filename.c_str(),"rb")){
            const auto read=[file](void* out,std::size_t bytes){return std::fread(out,1,bytes,file)==bytes;};
            char magic[4];unsigned count=0;
            bool valid=read(magic,4)&&!std::memcmp(magic,"HSA1",4)&&read(&count,4)&&count<=next.shrooms.size();
            for(unsigned i=0;valid&&i<count;++i){
                unsigned owner=0,initial=0;valid=read(&owner,4)&&read(&initial,4)&&initial<65536;
                auto shroom=std::find_if(next.shrooms.begin(),next.shrooms.end(),[&](const TerrainShroom& s){return s.owner==owner;});
                if(!valid||shroom==next.shrooms.end()||!shroom->frames[0].empty()){valid=false;break;}
                shroom->initial=initial;
                for(unsigned clip=0;valid&&clip<2;++clip){
                    unsigned frames=0;float fps=0;
                    valid=read(&fps,4)&&read(&frames,4)&&std::isfinite(fps)&&fps>0&&fps<=120&&frames>0&&frames<=64;
                    if(!valid)break;
                    shroom->fps[clip]=fps;shroom->frames[clip].resize(frames);
                    valid=read(shroom->frames[clip].data(),frames*sizeof(unsigned));
                    for(unsigned frame:shroom->frames[clip])valid=valid&&frame<65536;
                }
            }
            valid=valid&&std::fgetc(file)==EOF;std::fclose(file);if(!valid)return false;
        }
    }
    *this=std::move(next); return true;
}
bool TerrainBank::setEnabled(unsigned id,bool enabled)
{
    for(auto& shape:shapes)if(shape.id==id){shape.enabled=enabled;return true;}
    return false;
}
World TerrainBank::world() const
{ World w; w.shapes=shapes.data(); w.shapeCount=shapes.size(); w.vertices=vertices.data(); w.contact.enabled=sourceContacts; return w; }
void TerrainBank::fixedLeverMotion(float seconds,const Hero& hero,const NailBank* nails)
{
    if(!std::isfinite(seconds)||seconds<=0)return;
    for(auto& lever:levers){
        if(lever.state.activated){
            lever.state.update(seconds);
            // Pillar Control persists Activated on ACTIVATE, after Switch Hit's wait.
            if(lever.state.pillar&&lever.state.elapsed>=lever.state.seconds[0]&&motionSave_&&
               !motionSave_->get(lever.key)&&motionSave_->set(lever.key))leverChanged_=true;
        }
        else if(lever.state.pillar&&!lever.switchActivated&&lever.switchIdleFps>0&&lever.switchIdleFrames)
            lever.state.elapsed=std::fmod(lever.state.elapsed+seconds,lever.switchIdleFrames/lever.switchIdleFps);
        if(!lever.state.activated&&!lever.switchActivated&&nails&&hero.nail.hitbox&&hero.nail.serial!=lever.attackSerial&&
                nailOverlapsPolygon(hero,nails->shapes[nailVariant(hero.nail)],lever.polygon.data(),lever.count)){
            lever.attackSerial=hero.nail.serial;
            if(motionSave_&&(lever.state.pillar||overlaps(bodyBounds(hero.position),lever.range))){
                auto nextSave=*motionSave_;
                SavedPlayerData nextPlayerData=motionPlayerData_?*motionPlayerData_:SavedPlayerData{};
                if(!nextSave.set(lever.key)||(lever.playerDataKey&&
                   (!motionPlayerData_||!nextPlayerData.set(lever.playerDataKey,1)))||
                   (lever.state.pillar&&!nextSave.set(lever.switchKey)))continue;
                if(lever.state.pillar){
                    // Reserve capacity for both PersistentBoolItems before starting.
                    nextSave=*motionSave_;nextSave.set(lever.switchKey);
                }
                *motionSave_=nextSave;if(motionPlayerData_)*motionPlayerData_=nextPlayerData;
                lever.switchActivated=true;lever.state.activate();leverChanged_=true;
            }
        }
    }
    applyLeverCollision();
}

void TerrainBank::fixedMotion(float seconds,Hero& hero,const NailBank* nails)
{
    for(auto& shroom:shrooms){
        shroom.elapsed+=seconds;
        if(shroom.bounceElapsed>=0){
            shroom.bounceElapsed+=seconds;
            if(shroom.bounceElapsed*shroom.fps[1]>=shroom.frames[1].size())shroom.bounceElapsed=-1;
        }
    }
    if(nails&&hero.nail.hitbox){
        const auto& nail=nails->shapes[nailVariant(hero.nail)];
        std::array<Vec2,maxNailVertices> polygon{};
        Rect bounds{};
        const bool active=(nail.layerMask&(1u<<19))&&nail.count>=3&&nail.count<=maxNailVertices;
        if(active)for(unsigned i=0;i<nail.count;++i){
            const auto point=nailPoint(hero,nail,nail.vertices[i]);polygon[i]=point;
            if(!i)bounds={point.x,point.y,point.x,point.y};
            else{bounds.left=std::min(bounds.left,point.x);bounds.right=std::max(bounds.right,point.x);
                 bounds.bottom=std::min(bounds.bottom,point.y);bounds.top=std::max(bounds.top,point.y);}
        }
        if(active)for(auto& shroom:shrooms){
            const CombatCircle circle{shroom.center,shroom.radius};
            if(shroom.attackSerial==hero.nail.serial||
               !circleOverlapsBox(circle,bounds)||!polygonOverlapsCircle(polygon.data(),nail.count,circle))continue;
            shroom.attackSerial=hero.nail.serial;
            shroom.bounceElapsed=0;
            // Layer-19 BounceShroom follows the shared CombatScene reaction:
            // downward ShroomBounce, sideways large recoil, upward recoil-down.
            hero.nailContact(true,hero.nail.direction!=Strike::Down,true);
        }
    }
    if(!std::isfinite(seconds)||seconds<=0)return;
    fixedLeverMotion(seconds,hero,nails);
    for(auto& lift:lifts){
        if(lift.scripted)continue;
        PresentationPosition::Step motionStep(lift.presentation,lift.position);
        const auto body=bodyBounds(hero.position);
        bool riding=false;
        for(unsigned index:lift.shapes){
            const auto& shape=shapes[index];
            if(shape.actorContact(true)&&body.right>shape.bounds.left&&body.left<shape.bounds.right&&
               std::abs(body.bottom-shape.bounds.top)<.12f&&hero.velocity.y<=0)riding=true;
        }
        if(nails&&hero.nail.hitbox&&hero.nail.serial!=lift.attackSerial){
            const auto& nail=nails->shapes[nailVariant(hero.nail)];
            for(unsigned i=0;i<lift.controls.size();++i){
                const auto& points=lift.controls[i];
                const float offset=i==0?lift.position.y-lift.initial:0;
                const auto point=[&](unsigned v){return Vec2{points[v].x,points[v].y+offset};};
                bool hit=false;
                for(unsigned v=1;v+1<points.size();++v)
                    hit|=nailOverlapsQuad(hero,nail,{{point(0),point(v),point(v+1),point(v+1)}});
                if(!hit)continue;
                lift.attackSerial=hero.nail.serial;
                if(lift.falling){
                    if(!lift.cut){
                        lift.cut=true;lift.moving=true;lift.target=lift.lower;
                        if(motionSave_&&!lift.saveOnLand)motionSave_->set(lift.key);
                    }
                    break;
                }
                if(!lift.moving||(i==0&&lift.wait<=0)){
                    float target=lift.position.y;
                    const bool reverse=lift.moving;
                    if(i>0)target=lift.stops[i-1];
                    else{
                        if(reverse)lift.goingDown=!lift.goingDown;
                        const float from=reverse?lift.target:lift.position.y;
                        const auto stop=std::find(lift.stops.begin(),lift.stops.end(),from);
                        const int next=int(stop-lift.stops.begin())+(lift.goingDown?1:-1);
                        // RuinsLift.KeepInBounds returns the first stop for an invalid index.
                        target=lift.stops[next>=0&&next<int(lift.stops.size())?next:0];
                    }
                    if(target!=lift.position.y||reverse){
                        if(target!=lift.position.y)lift.goingDown=target<lift.position.y;
                        // Get Pos/Swerve wait .5s. Move Start translates during its .15s wait.
                        lift.target=target;lift.wait=.5f;lift.moving=true;
                    }
                }
                break;
            }
        }
        if(!lift.moving)continue;
        if(lift.wait>0){lift.wait=std::max(0.f,lift.wait-seconds);continue;}
        const float distance=lift.target-lift.position.y;
        // AccelerateVelocity adds -2 on each original 50 Hz fixed update.
        if(lift.falling)lift.velocity=std::min(lift.speed,lift.velocity+lift.acceleration*seconds);
        const float delta=std::copysign(std::min(std::abs(distance),(lift.falling?lift.velocity:lift.speed)*seconds),distance);
        lift.position.y+=delta;
        for(unsigned index:lift.shapes){
            auto& shape=shapes[index];shape.bounds.bottom+=delta;shape.bounds.top+=delta;
            for(unsigned v=shape.first;v<shape.first+shape.count;++v)vertices[v].y+=delta;
        }
        // HeroPlatformStick translates a grounded passenger with the platform.
        // Jumping detaches before this fixed step; the shared solver then handles landing.
        if(riding)hero.platformDisplacement+=delta;
        if(lift.position.y==lift.target){
            lift.moving=false;
            if(lift.falling&&lift.saveOnLand&&motionSave_)motionSave_->set(lift.key);
            // LIFT STOP returns the lever to Idle; only termini flip its direction.
            if(!lift.falling&&(lift.position.y==lift.upper||lift.position.y==lift.lower))lift.goingDown=!lift.goingDown;
        }
    }
}
void TerrainBank::bindMotionSave(SceneBoolSave& save,SavedPlayerData* playerData)
{
    motionSave_=&save;
    motionPlayerData_=playerData;
    leverChanged_=false;
    for(auto& lever:levers){
        lever.attackSerial=0;lever.state.restore(save.get(lever.key));
        lever.switchActivated=lever.state.activated||(lever.state.pillar&&(save.get(lever.switchKey)||
            (playerData&&playerData->get(lever.playerDataKey))));
        shapes[lever.shape].enabled=lever.state.colliderEnabled();
    }
    for(auto& lift:lifts)if(lift.falling&&save.get(lift.key)&&!lift.cut){
        const float delta=lift.lower-lift.position.y;
        lift.cut=true;lift.position.y=lift.target=lift.lower;lift.moving=false;
        for(unsigned index:lift.shapes){auto& shape=shapes[index];shape.bounds.bottom+=delta;shape.bounds.top+=delta;
            for(unsigned v=shape.first;v<shape.first+shape.count;++v)vertices[v].y+=delta;}
        lift.presentation.reset(lift.position);
    }
}
void TerrainBank::bindMotionVisuals(RoomVisualBank& room)
{
    for(auto& shroom:shrooms){
        shroom.draw=~0u;if(shroom.frames[0].empty())continue;
        bool valid=true;
        for(const auto& clip:shroom.frames)for(unsigned frame:clip)valid=valid&&frame<room.atlas.sprites.size();
        for(unsigned i=0;i<room.draws.size();++i)if(room.draws[i].owner==shroom.owner){
            valid=valid&&shroom.draw==~0u&&room.draws[i].sprite==shroom.initial;shroom.draw=i;
        }
        if(!valid)shroom.draw=~0u;
    }
    for(auto& lever:levers){
        lever.draw=lever.switchDraw=~0u;if(lever.frames.empty())continue;
        bool valid=std::all_of(lever.frames.begin(),lever.frames.end(),[&](unsigned f){return f<room.atlas.sprites.size();});
        valid=valid&&std::all_of(lever.switchFrames.begin(),lever.switchFrames.end(),[&](unsigned f){return f<room.atlas.sprites.size();});
        for(unsigned i=0;i<room.draws.size();++i)if(room.draws[i].owner==lever.gate){
            valid=valid&&lever.draw==~0u&&room.draws[i].sprite==lever.frames[0];lever.draw=i;
        }
        valid=valid&&lever.draw!=~0u;
        if(lever.state.pillar){
            for(unsigned i=0;i<room.draws.size();++i)if(room.draws[i].owner==lever.owner){
                valid=valid&&lever.switchDraw==~0u&&!lever.switchFrames.empty()&&room.draws[i].sprite==lever.switchFrames[0];
                lever.switchDraw=i;
            }
            valid=valid&&lever.switchDraw!=~0u;
        }
        assert(valid&&"Invalid source gate artwork binding");
        if(!valid){lever.frames.clear();lever.switchFrames.clear();lever.draw=lever.switchDraw=~0u;}
    }
    for(auto& lift:lifts){
        lift.draws.clear();
        for(unsigned i=0;i<room.draws.size();++i){
            const auto& draw=room.draws[i];
            const bool moving=std::find(lift.owners.begin(),lift.owners.end(),draw.owner)!=lift.owners.end();
            const bool chain=std::find(lift.chains.begin(),lift.chains.end(),draw.owner)!=lift.chains.end();
            if(moving||chain){
                lift.draws.push_back({i,draw.values[5],draw.values[10],chain});
                if(room.cameraBounds.size()==room.draws.size()&&draw.sprite<room.atlas.sprites.size()){
                    auto bounds=roomCameraBounds(room.atlas.sprites[draw.sprite],draw);
                    // The renderer rejects against this cache before projecting
                    // the animated pose. Cover the entire authored lift travel.
                    bounds.bottom+=lift.lower-lift.initial;
                    bounds.top+=lift.upper-lift.initial;
                    room.cameraBounds[i]=bounds;
                }
            }
        }
    }
}
void TerrainBank::applyLeverCollision()
{
    for(const auto& lever:levers)if(!lever.state.colliderEnabled())shapes[lever.shape].enabled=false;
}
float TerrainBank::leverAlpha(unsigned owner) const
{
    // Source controller disables the renderer after Door Open 2.
    for(const auto& lever:levers)if(lever.gate==owner&&!lever.state.visible())return 0;
    return 1;
}
void TerrainBank::presentMotion(RoomVisualBank& room,PresentationTime time)
{
    const auto present=[&](unsigned index,unsigned sprite){
        auto& draw=room.draws[index];
        if(draw.sprite!=sprite){
            draw.sprite=sprite;
            if(index<room.cameraBounds.size())room.cameraBounds[index]=roomCameraBounds(room.atlas.sprites[sprite],draw);
        }
    };
    for(const auto& shroom:shrooms)if(shroom.draw<room.draws.size()){
        const unsigned clip=shroom.bounceElapsed>=0?1:0;
        const auto& frames=shroom.frames[clip];
        const float seconds=clip?shroom.bounceElapsed:shroom.elapsed;
        const unsigned frame=unsigned(seconds*shroom.fps[clip]);
        present(shroom.draw,frames[clip?std::min<unsigned>(frame,frames.size()-1):frame%frames.size()]);
    }
    for(const auto& lever:levers)if(lever.draw<room.draws.size()&&!lever.frames.empty()){
        const auto frame=lever.state.openingFrame(lever.firstFrames,lever.frames.size()-lever.firstFrames-(lever.state.pillar?2:1));
        present(lever.draw,lever.frames[frame]);
        if(lever.switchDraw<room.draws.size()&&!lever.switchFrames.empty()){
            unsigned switchFrame=lever.switchIdleFrames?
                1+static_cast<unsigned>(lever.state.elapsed*lever.switchIdleFps)%lever.switchIdleFrames:0;
            if(lever.switchActivated){
                switchFrame=lever.switchFrames.size()-1;
                if(lever.state.activated&&lever.state.elapsed<lever.state.seconds[0])
                    switchFrame=1+lever.switchIdleFrames+std::min(static_cast<unsigned>(lever.switchFrames.size()-lever.switchIdleFrames-3),
                                                               static_cast<unsigned>(lever.state.elapsed*lever.switchFps));
            }
            present(lever.switchDraw,lever.switchFrames[switchFrame]);
        }
    }
    for(auto& lift:lifts){
        const float y=lift.presentation.sample(lift.position,time).y;
        for(const auto& authored:lift.draws){
            auto& draw=room.draws[authored.index];
            draw.values[5]=authored.y+y-lift.initial;
            if(authored.chain){
                if(lift.falling){draw.values[5]=authored.y;draw.values[10]=lift.cut?0:authored.alpha;continue;}
                const auto rank=std::find(lift.chains.begin(),lift.chains.end(),draw.owner)-lift.chains.begin();
                const auto hidden=static_cast<unsigned>(std::floor((y-lift.lower)/(lift.upper-lift.lower)*lift.chains.size()));
                draw.values[10]=static_cast<unsigned>(rank)>=hidden?authored.alpha:0;
            }
        }
    }
}

}
