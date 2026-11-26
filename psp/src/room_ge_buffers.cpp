#include "room_ge.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hkpsp {
namespace {
bool sameCamera(const hk3ds::RoomCameraBounds& a,const hk3ds::RoomCameraBounds& b){
    return a.left==b.left&&a.bottom==b.bottom&&a.right==b.right&&a.top==b.top;
}
bool sameKey(const hk3ds::RoomDraw& a,const hk3ds::RoomDraw& b){
    return a.owner==b.owner&&a.values[11]==b.values[11]&&
           a.values[12]==b.values[12]&&a.values[6]==b.values[6];
}
RoomQuadCache cache(const hk3ds::RoomVisualBank& room,unsigned index,bool background){
    const auto& d=room.draws.at(index);RoomQuadCache result;
    if(!cacheRoomQuad(room.atlas.sprites.at(d.sprite),d,background,room.blurSplitDepth,result))
        throw std::invalid_argument("GE source is absent from this room pass");
    return result;
}
void appendVertices(RoomGeBuffers& out,const hk3ds::RoomVisualBank& room,unsigned index,
                    bool background,const RoomQuadCache& q){
    const auto& d=room.draws[index];const auto& s=room.atlas.sprites[d.sprite];
    hk3ds::RoomSpriteClip clip{s};
    if(!pspRoomClip(s,d,background,clip,room.blurSplitDepth))
        throw std::runtime_error("GE pass changed while filling vertices");
    const auto& c=clip.sprite;const auto& v=d.values;
    const std::array<Point,4> local{{{c.left,c.top},{c.right,c.top},{c.right,c.bottom},{c.left,c.bottom}}};
    const auto& w=q.world;
    for(unsigned i=0;i<4;++i){
        const auto p=local[i];const float z=38.1f+v[6]+v[13]*p.x+v[14]*p.y;
        out.vertices.push_back({i==0||i==3?w.u0:w.u1,i<2?w.v0:w.v1,
                                w.positions[i].x,w.positions[i].y,z});
    }
}
}
std::size_t RoomGeBuffers::bytes()const{
    return vertices.capacity()*sizeof(RoomGePosition)+runs.capacity()*sizeof(RoomGeRun)+
           cpu.capacity()*sizeof(RoomQuadCache)+ordered.capacity()*sizeof(RoomGeEntry);
}
const std::array<std::uint16_t,roomGeMaxQuads*6>& roomGeIndices(){
    alignas(16) static constexpr auto indices=[](){
        std::array<std::uint16_t,roomGeMaxQuads*6> result{};
        for(unsigned i=0;i<roomGeMaxQuads;++i){
            const unsigned base=i*4;const unsigned ids[]={0,1,2,0,2,3};
            for(unsigned j=0;j<6;++j)result[i*6+j]=static_cast<std::uint16_t>(base+ids[j]);
        }
        return result;
    }();
    return indices;
}
RoomGeBuffers buildRoomGeBuffers(const hk3ds::RoomVisualBank& room,
    const std::vector<RoomGeSource>& sources,bool background,float focal,unsigned maxRunQuads){
    if(!maxRunQuads||maxRunQuads>roomGeMaxQuads)throw std::invalid_argument("Invalid GE run limit");
    RoomGeBuffers out;RoomQuadCache previous;
    unsigned eligible=0,cpuCount=0,previousDraw=~0u;
    // This first pass retains metadata only; final payloads reserve once. The
    // caller must replace its old cache allocation, not retain both arrays.
    for(const auto& source:sources){
        if(previousDraw!=~0u&&source.draw<=previousDraw)
            throw std::invalid_argument("GE sources must retain strict authored order");
        const auto q=cache(room,source.draw,background);
        const auto reason=classifyRoomGe(q,source.camera,focal);out.counts.add(reason,&q);
        if(source.exact||reason!=RoomGeReason::Eligible){
            out.ordered.push_back({source.draw,roomGeCpuBit|cpuCount++});
        }else{
            const auto camera=roomGeVisibleCamera(q,source.camera,focal);
            bool join=false;
            if(!out.ordered.empty()&&!(out.ordered.back().geometry&roomGeCpuBit)){
                auto& last=out.runs.back();
                join=last.count<maxRunQuads&&source.draw==last.firstDraw+last.count&&
                     sameKey(room.draws[last.firstDraw],room.draws[source.draw])&&
                     sameBatchState(previous.world,q.world)&&sameCamera(last.camera,camera);
                if(join)++last.count;
            }
            if(!join){
                out.ordered.push_back({source.draw,static_cast<unsigned>(out.runs.size())});
                out.runs.push_back({camera,eligible*4,source.draw,1});
            }
            ++eligible;
        }
        previous=q;previousDraw=source.draw;
    }
    // Release geometric-growth capacity before allocating vertex payloads.
    std::vector<RoomGeRun>(out.runs).swap(out.runs);
    std::vector<RoomGeEntry>(out.ordered).swap(out.ordered);
    out.vertices.reserve(eligible*4);out.cpu.reserve(cpuCount);
    std::size_t sourceIndex=0;
    for(const auto& entry:out.ordered){
        if(entry.geometry&roomGeCpuBit){++sourceIndex;out.cpu.push_back(cache(room,entry.draw,background));continue;}
        const auto& run=out.runs.at(entry.geometry);
        if(run.firstVertex!=out.vertices.size())throw std::runtime_error("GE vertex offset changed");
        for(unsigned i=0;i<run.count;++i){
            const auto q=cache(room,run.firstDraw+i,background);
            const auto& source=sources.at(sourceIndex++);
            if(!sameCamera(run.camera,roomGeVisibleCamera(q,source.camera,focal)))
                throw std::runtime_error("GE run changed exact visible membership");
            appendVertices(out,room,run.firstDraw+i,background,q);
        }
    }
    return out;
}
RoomGeMatrix roomGeProjection(hk3ds::Vec2 camera,float focal){
    if(!std::isfinite(camera.x)||!std::isfinite(camera.y)||!std::isfinite(focal)||focal<=0)
        throw std::invalid_argument("Invalid GE camera");
    RoomGeMatrix result{};result[0]=2*focal/screenWidth;result[5]=2*focal/screenHeight;
    result[11]=1;result[12]=-result[0]*camera.x;result[13]=-result[5]*camera.y;return result;
}
RoomGeMatrix roomExactProjection(){
    RoomGeMatrix result{};result[0]=2.f/screenWidth;result[5]=-2.f/screenHeight;
    result[8]=-1;result[9]=1;result[11]=1;return result;
}
}
