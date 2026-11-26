// Kept empty in the PC build, which compiles the common PSP source directory.
#ifdef __PSP__
#include "../include/test_mode.hpp"
#include "../include/platform.hpp"
#include "draw_list.hpp"
#include "room_ge.hpp"
#include "room_ge_trace.hpp"
#include "asset_path.hpp"
#include "look_grade.hpp"
#include "diag.hpp"
#include "perf.hpp"
#include "page_residency.hpp"
#include "chunked_scratch.hpp"
#include "../include/memory_report.hpp"
#pragma GCC diagnostic push
// SDK SceCtrlData has an anonymous struct, a C extension; keep -pedantic on our code.
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspctrl.h>
#pragma GCC diagnostic pop
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace hkpsp {
#ifdef HKPSP_PERF
namespace perf {unsigned now(){return sceKernelGetSystemTimeLow();}}
#endif
namespace {
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
// GE timeline markers, without splitting the command list or adding CPU syncs.
// WAIT ensures the timestamp is published before the GE proceeds past a marker.
volatile unsigned geStamp[4]{},geSeen=0;
unsigned cpuStamp[4]{};
void geMarker(int id){if(id>=0&&id<4){geStamp[id]=perf::now();geSeen|=1u<<id;}}
#endif
#ifdef HKPSP_PERF
constexpr const char* out=perf::directory;
#elif defined(HKPSP_DIAG)
constexpr const char* out="ms0:/PSP/GAME/HKPSPDIAG/out/";
#else
constexpr const char* out="ms0:/PSP/GAME/HKPSP/out/";
#endif
constexpr unsigned stride=512,bufferBytes=stride*screenHeight*4;
// Same PSL1 neutral-byte contract as the host; alpha is already folded into P.
constexpr unsigned char lightDelta(unsigned p){return p>128?2*p-256:0;}
static_assert(lightDelta(128)==0&&lightDelta(129)==2&&lightDelta(255)==254,
              "Folded light must preserve black at neutral and add on dark scenes");
// Unsigned subtraction keeps the interval valid when the display counter wraps.
constexpr bool beforeFlip(unsigned last,unsigned now){return now-last<2u;}
static_assert(beforeFlip(7,7)&&beforeFlip(7,8)&&!beforeFlip(7,9)&&!beforeFlip(7,12),
              "Short frames wait only to last flip + 2; overdue frames need one next vblank");
static_assert(beforeFlip(~0u,0)&&!beforeFlip(~0u,1),"Vcount wrap must preserve the two-vblank interval");
alignas(16) unsigned commands[262144];
// Keep room for GU allocation alignment, draw commands and list termination.
// Check before sceGuGetMemory advances the cursor or a caller writes vertices.
void requireGuCapacity(unsigned bytes){
    constexpr unsigned tail=256;
    const unsigned used=static_cast<unsigned>(sceGuCheckList());
    if(used>sizeof(commands)-tail||bytes>sizeof(commands)-tail-used)
        throw std::runtime_error("GU command capacity exceeded");
}
void* guMemory(unsigned bytes){
    requireGuCapacity(bytes);
    return sceGuGetMemory(bytes);
}
struct Vertex {float u,v;unsigned color;float x,y,z;};
// Counters attribute shared draws once if they contain any room geometry.
// Vertex bytes count final GU payload writes, including material repeat passes.
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
bool emittingRoom=false,directRoom=false,pendingRoom=false;
#endif
bool roomEmission(){
#ifdef HKPSP_GPU_ATTRIB
    return false;
#else
    return cpu::roomEmission;
#endif
}
// Cache actual GE state, including changes made by mask/grade/scratch passes.
// A list restart invalidates every entry before pages or render targets change.
template<class Emit,class... T> void stateCommand(Emit emit,T... args){
    HKPSP_PROFILE(const unsigned before=sceGuCheckList();)
    emit(args...);
    HKPSP_PROFILE(perf::current[perf::GuStateCommands]+=(sceGuCheckList()-before)/4;)
}
template<class... T> struct GuState {
    bool valid=false;
    std::tuple<T...> value{};
    template<class Emit> void set(Emit emit,T... args){
        const auto next=std::make_tuple(args...);
        if(roomEmission()&&valid&&value==next)return;
        stateCommand(emit,args...);
        valid=true;value=next;
    }
};
GuState<int,int,int,int> modeState;
GuState<unsigned,unsigned,unsigned,unsigned> clutState;
GuState<int,int,int,int,const void*> imageState;
GuState<float,float> scaleState;
GuState<int,int,int,unsigned,unsigned> blendState;
GuState<int,int> functionState,filterState;
GuState<unsigned> environmentState;
void resetState(){
    modeState.valid=clutState.valid=imageState.valid=scaleState.valid=false;
    blendState.valid=functionState.valid=filterState.valid=environmentState.valid=false;
}
void texMode(int psm,int levels,int unknown,int swizzle){modeState.set(sceGuTexMode,psm,levels,unknown,swizzle);}
void clutMode(unsigned psm,unsigned shift,unsigned mask,unsigned start){clutState.set(sceGuClutMode,psm,shift,mask,start);}
void texImage(int level,int width,int height,int stride,const void* pixels){
    imageState.set([](int l,int w,int h,int s,const void* p){
        sceGuTexImage(l,w,h,s,p);
        HKPSP_PROFILE(++perf::current[perf::TextureFlushes];)
    },level,width,height,stride,pixels);
}
void texScale(float u,float v){scaleState.set(sceGuTexScale,u,v);}
void blendFunc(int op,int src,int dst,unsigned s,unsigned d){
    blendState.set([](int o,int a,int b,unsigned x,unsigned y){
        sceGuBlendFunc(o,a,b,x,y);
    },op,src,dst,s,d);
}
void texFunc(int function,int components){functionState.set(sceGuTexFunc,function,components);}
void texFilter(int min,int mag){filterState.set(sceGuTexFilter,min,mag);}
void texEnvironment(unsigned color){environmentState.set(sceGuTexEnvColor,color);}
void texFlush(){
    sceGuTexFlush();
    HKPSP_PROFILE(++perf::current[perf::GuStateCommands];++perf::current[perf::TextureFlushes];)
}
alignas(64) constexpr auto gradeStrips=cameraGradeStrips<Vertex>();
static_assert(sizeof(gradeStrips)==4320,"Grade strip budget changed");
void drawGradeStrips(){
    HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,
        gradeStrips.size(),nullptr,gradeStrips.data());
    HKPSP_PROFILE(perf::current[perf::Triangles]+=gradeStrips.size()/3;++cpu::drawCalls;)
}
unsigned color(const Quad& q,bool premultiply){
    unsigned packed=0;
    for(unsigned c=0;c<4;++c){
        const float value=q.rgba[c]*(premultiply&&c<3?q.rgba[3]:1.f)*(c<3&&q.whiteFlash>0?q.whiteFlash:1.f);
        packed|=static_cast<unsigned>(std::clamp(std::lround(value*255),0l,255l))<<(8*c);
    }
    return packed;
}
#ifndef HKPSP_GPU_ATTRIB
bool sameEmissionState(const Quad& a,const Quad& b){
    const bool tintState=a.material==hk3ds::RoomMaterial::LinearLight||a.whiteFlash>0;
    return a.page==b.page&&a.clut==b.clut&&a.material==b.material&&a.blend==b.blend&&
        a.whiteFlash==b.whiteFlash&&(!tintState||a.rgba==b.rgba);
}
#endif
struct ClipVertex {float x,y,iz,uz,vz;};
#ifndef HKPSP_GPU_ATTRIB
struct Geometry {
    const Vertex* vertices=nullptr;unsigned count=0,split=0;
    bool direct()const{return split==~0u;}
};
struct EmissionRun {unsigned end,vertices;};
using GeometryScratch=ChunkedScratch<Geometry,32>;
using ClippedVertices=std::array<Vertex,30>;
class ClippedGeometryScratch {
    ChunkedScratch<ClippedVertices,8> storage_;
    std::size_t used_=0;
public:
    void beginBatch(){used_=0;}
    std::size_t highWater()const{return storage_.size();}
    ClippedVertices& next(){
        storage_.grow(used_+1);
        return storage_[used_++];
    }
};
EmissionRun emissionRun(const std::vector<Quad>& quads,const GeometryScratch& prepared,unsigned first,unsigned limit){
    EmissionRun run{first+1,prepared[first].count};
    // GE PRIM carries a 16-bit vertex count. Keep a whole quad on one side
    // of that boundary, and preserve the authored sequence of state changes.
    while(run.end<limit&&sameEmissionState(quads[first],quads[run.end])&&
          prepared[run.end].count<=65535-run.vertices)run.vertices+=prepared[run.end++].count;
    return run;
}
GeometryScratch geometry;
ClippedGeometryScratch clippedGeometry;
const Geometry* currentGeometry=nullptr;
std::vector<Vertex> pending;
Vertex* directVertices=nullptr;
unsigned directCount=0,directCapacity=0;
void beginTriangles(unsigned count){
    if(directVertices)throw std::runtime_error("Unsubmitted GU vertex run");
    if(!count)return;
    directVertices=static_cast<Vertex*>(guMemory(count*sizeof(Vertex)));
    directCount=0;directCapacity=count;
    HKPSP_PROFILE(directRoom=false;)
}
void flushTriangles(){
    if(directVertices){
        if(directCount!=directCapacity)throw std::runtime_error("Incomplete GU vertex run");
        HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,
            directCount,nullptr,directVertices);HKPSP_PROFILE(++cpu::drawCalls;)
        HKPSP_PROFILE(if(directRoom)++perf::current[perf::RoomRunsDrawn];directRoom=false;)
        directVertices=nullptr;directCount=directCapacity=0;
    }
    if(pending.empty())return;
    auto* target=static_cast<Vertex*>(guMemory(pending.size()*sizeof(Vertex)));
    std::memcpy(target,pending.data(),pending.size()*sizeof(Vertex));
    HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,
        pending.size(),nullptr,target);HKPSP_PROFILE(++cpu::drawCalls;)pending.clear();
    HKPSP_PROFILE(if(pendingRoom)++perf::current[perf::RoomRunsDrawn];pendingRoom=false;)
}
void prepareTriangle(const Quad& q,const std::array<unsigned,3>& ids,ClippedVertices& vertices,unsigned& vertexCount){
    std::array<ClipVertex,10> polygon{},scratch{};unsigned count=3;
    bool clipped=false;
    // Clip relative to the sprite, so moving it within an atlas cannot change
    // interpolation rounding before the integer origin is restored.
    const float originU=std::min(q.u0,q.u1),originV=std::min(q.v0,q.v1);
    bool safe=cpu::config().c2;
    for(unsigned j=0;j<3;++j){const auto k=ids[j];const float iz=q.inverseDepth[k];
        if(iz<=0)throw std::runtime_error("Invalid quad inverse depth");
        polygon[j]={q.positions[k].x,q.positions[k].y,iz,
            ((k==1||k==2?q.u1:q.u0)-originU)*iz,((k>=2?q.v1:q.v0)-originV)*iz};
        const auto& p=q.positions[k];
        // GE raster coordinates include the 2048-centred viewport offset.
        // Keep one pixel inside 0..4095 to cover subpixel rounding.
        safe=safe&&p.x>=-1807&&p.x<=2286&&p.y>=-1911&&p.y<=2182;
    }
    for(unsigned edge=0;!safe&&edge<4&&count;++edge){
        const auto distance=[&](const ClipVertex& v){return edge==0?v.x:edge==1?screenWidth-v.x:edge==2?v.y:screenHeight-v.y;};
        unsigned next=0;auto a=polygon[count-1];float da=distance(a);
        for(unsigned j=0;j<count;++j){const auto b=polygon[j];const float db=distance(b);
            if((da>=0)!=(db>=0)){
                clipped=true;
                const float t=da/(da-db);
                scratch.at(next++)={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.iz+(b.iz-a.iz)*t,
                    a.uz+(b.uz-a.uz)*t,a.vz+(b.vz-a.vz)*t};
            }
            if(db>=0)scratch.at(next++)=b;
            a=b;da=db;
        }
        polygon=scratch;count=next;
    }
    for(unsigned j=1;j+1<count;++j)for(auto k:{0u,j,j+1}){
        const auto& v=polygon[k];const float z=1/v.iz;
        // Untouched vertices already describe exact texel edges. A redundant
        // multiply/divide round trip changes them depending on atlas position.
        const auto id=clipped?0u:ids[k];
        const float u=clipped?originU+v.uz*z:(id==1||id==2?q.u1:q.u0);
        const float t=clipped?originV+v.vz*z:(id>=2?q.v1:q.v0);
        vertices.at(vertexCount++)={u,t,0,v.x*z,v.y*z,z};
    }
}
void prepareGeometry(const Quad& q,Geometry& result,ClippedGeometryScratch& scratch){
    result={};
    bool direct=roomEmission()&&cpu::config().c2;
    for(unsigned k=0;direct&&k<4;++k){
        if(q.inverseDepth[k]<=0)throw std::runtime_error("Invalid quad inverse depth");
        const auto& p=q.positions[k];
        direct=p.x>=-1807&&p.x<=2286&&p.y>=-1911&&p.y<=2182;
    }
    if(direct){result.count=6;result.split=~0u;return;}
    auto& vertices=scratch.next();result.vertices=vertices.data();
    prepareTriangle(q,{{0,1,2}},vertices,result.count);result.split=result.count;
    prepareTriangle(q,{{0,2,3}},vertices,result.count);
}
void triangle(const Quad& q,const std::array<unsigned,3>& ids,unsigned tint){
    const unsigned split=currentGeometry->direct()?3:currentGeometry->split;
    const unsigned first=ids[1]==1?0:split;
    const unsigned end=ids[1]==1?split:currentGeometry->count;
    if(first==end)return;
    HKPSP_PROFILE(perf::current[perf::Triangles]+=(end-first)/3;)
    HKPSP_PROFILE(if(emittingRoom)perf::current[perf::RoomVertexBytes]+=(end-first)*sizeof(Vertex);)
    if(roomEmission()){
        if(!directVertices)beginTriangles(currentGeometry->count);
        HKPSP_PROFILE(directRoom=directRoom||emittingRoom;)
        if(end-first>directCapacity-directCount)throw std::runtime_error("GU vertex run capacity exceeded");
        if(currentGeometry->direct()){
            // Identical expressions to the untouched-vertex branch of the
            // clipper, evaluated straight into the final GU allocation.
            for(auto k:ids){
                const float z=1/q.inverseDepth[k];
                directVertices[directCount++]={k==1||k==2?q.u1:q.u0,k>=2?q.v1:q.v0,tint,
                    q.positions[k].x*z,q.positions[k].y*z,z};
            }
        }else for(unsigned i=first;i<end;++i){auto& v=directVertices[directCount++];v=currentGeometry->vertices[i];v.color=tint;}
        return;
    }
    if(pending.size()+end-first>120)flushTriangles();
    HKPSP_PROFILE(pendingRoom=pendingRoom||emittingRoom;)
    for(unsigned i=first;i<end;++i){auto v=currentGeometry->vertices[i];v.color=tint;pending.push_back(v);}
    if(!cpu::config().c3)flushTriangles();
}
#else
void triangle(const Quad& q,const std::array<unsigned,3>& ids,unsigned tint){
    HKPSP_PROFILE(const auto clipStart=perf::now();)
    std::array<ClipVertex,10> polygon{},scratch{};unsigned count=3;
    bool clipped=false;
    const float originU=std::min(q.u0,q.u1),originV=std::min(q.v0,q.v1);
    for(unsigned j=0;j<3;++j){const auto k=ids[j];const float iz=q.inverseDepth[k];
        if(iz<=0)throw std::runtime_error("Invalid quad inverse depth");
        polygon[j]={q.positions[k].x,q.positions[k].y,iz,
            ((k==1||k==2?q.u1:q.u0)-originU)*iz,((k>=2?q.v1:q.v0)-originV)*iz};
    }
    // The GE can reject an entire primitive if any screen coordinate exceeds
    // its 4096-unit guard band, even with GU_CLIP_PLANES. Entry-frame foreground
    // sprites extend thousands of pixels below the viewport. Clip before GE
    // submission, interpolating reciprocal depth and UV/depth in screen space.
    for(unsigned edge=0;edge<4&&count;++edge){
        const auto distance=[&](const ClipVertex& v){
            return edge==0?v.x:edge==1?screenWidth-v.x:edge==2?v.y:screenHeight-v.y;
        };
        unsigned next=0;auto a=polygon[count-1];float da=distance(a);
        for(unsigned j=0;j<count;++j){const auto b=polygon[j];const float db=distance(b);
            if((da>=0)!=(db>=0)){
                clipped=true;
                const float t=da/(da-db);
                scratch.at(next++)={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.iz+(b.iz-a.iz)*t,
                                    a.uz+(b.uz-a.uz)*t,a.vz+(b.vz-a.vz)*t};
            }
            if(db>=0)scratch.at(next++)=b;
            a=b;da=db;
        }
        polygon=scratch;count=next;
    }
    HKPSP_PROFILE(const auto clipTime=perf::now()-clipStart;
        perf::current[perf::Clipping]+=clipTime;perf::current[perf::DrawList]+=clipTime;)
    if(count<3)return;
    const unsigned verticesCount=(count-2)*3;
    HKPSP_PROFILE(perf::current[perf::Triangles]+=count-2;)
    auto* vertices=static_cast<Vertex*>(guMemory(verticesCount*sizeof(Vertex)));
    unsigned written=0;
    for(unsigned j=1;j+1<count;++j)for(auto k:{0u,j,j+1}){
        const auto& v=polygon[k];const float z=1/v.iz;
        const auto id=clipped?0u:ids[k];
        const float u=clipped?originU+v.uz*z:(id==1||id==2?q.u1:q.u0);
        const float t=clipped?originV+v.vz*z:(id>=2?q.v1:q.v0);
        vertices[written++]={u,t,tint,v.x*z,v.y*z,z};
    }
    HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,
                   verticesCount,nullptr,vertices);
}
#endif
// Sample a framebuffer at screen coordinates over the original clipped shape.
// Sprite UVs and inverse depth do not apply to a framebuffer resolve.
void screenTriangle(const Quad& q,const std::array<unsigned,3>& ids){
    std::array<Point,10> polygon{},scratch{};unsigned count=3;
    for(unsigned i=0;i<3;++i)polygon[i]=q.positions[ids[i]];
    for(unsigned edge=0;edge<4&&count;++edge){
        const auto distance=[&](Point p){return edge==0?p.x:edge==1?screenWidth-p.x:edge==2?p.y:screenHeight-p.y;};
        unsigned next=0;auto a=polygon[count-1];float da=distance(a);
        for(unsigned i=0;i<count;++i){const auto b=polygon[i];const float db=distance(b);
            if((da>=0)!=(db>=0)){
                const float t=da/(da-db);scratch.at(next++)={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};
            }
            if(db>=0)scratch.at(next++)=b;
            a=b;da=db;
        }
        polygon=scratch;count=next;
    }
    if(count<3)return;
    auto* vertices=static_cast<Vertex*>(guMemory((count-2)*3*sizeof(Vertex)));
    unsigned written=0;
    for(unsigned j=1;j+1<count;++j)for(auto k:{0u,j,j+1}){
        const auto p=polygon[k];vertices[written++]={p.x,p.y,0xffffffff,p.x,p.y,1};
    }
    HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,written,nullptr,vertices);
    HKPSP_PROFILE(perf::current[perf::Triangles]+=count-2;)
#ifndef HKPSP_GPU_ATTRIB
    HKPSP_PROFILE(++cpu::drawCalls;)
    HKPSP_PROFILE(if(emittingRoom){++perf::current[perf::RoomRunsDrawn];perf::current[perf::RoomVertexBytes]+=written*sizeof(Vertex);})
#endif
}
// Screen-space triangle with sprite UVs, clipped exactly like screenTriangle. For a constant-depth
// quad, affine UVs equal the perspective-correct ones. 2026-11-17 P1 stripes [M]: ClampedAdd drew
// its passes from (x*z,y*z,z) vertices and its composite from (x,y,1) vertices; the PSP-3000 GE
// rounds the two differently, so on some glow-pulse phases the composite covered one more row or
// column than the passes and added the copied destination twice (a 1-px line at 2x the
// background for one frame: DIAG climb, 40 transient lines in 860 frames, no page loads).
void spriteScreenTriangle(const Quad& q,const std::array<unsigned,3>& ids,unsigned tint){
    struct Corner {float x,y,u,v;};
    std::array<Corner,10> polygon{},scratch{};unsigned count=3;
    for(unsigned i=0;i<3;++i){const auto k=ids[i];
        polygon[i]={q.positions[k].x,q.positions[k].y,k==1||k==2?q.u1:q.u0,k>=2?q.v1:q.v0};}
    for(unsigned edge=0;edge<4&&count;++edge){
        const auto distance=[&](const Corner& c){return edge==0?c.x:edge==1?screenWidth-c.x:edge==2?c.y:screenHeight-c.y;};
        unsigned next=0;auto a=polygon[count-1];float da=distance(a);
        for(unsigned i=0;i<count;++i){const auto b=polygon[i];const float db=distance(b);
            if((da>=0)!=(db>=0)){
                const float t=da/(da-db);
                scratch.at(next++)={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t};
            }
            if(db>=0)scratch.at(next++)=b;
            a=b;da=db;
        }
        polygon=scratch;count=next;
    }
    if(count<3)return;
    auto* vertices=static_cast<Vertex*>(guMemory((count-2)*3*sizeof(Vertex)));
    unsigned written=0;
    for(unsigned j=1;j+1<count;++j)for(auto k:{0u,j,j+1}){
        const auto& c=polygon[k];vertices[written++]={c.u,c.v,tint,c.x,c.y,1};
    }
    sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,written,nullptr,vertices);
    HKPSP_PROFILE(perf::current[perf::Triangles]+=count-2;)
#ifndef HKPSP_GPU_ATTRIB
    HKPSP_PROFILE(++cpu::drawCalls;)
#endif
}
FILE* output(const char* name){
    const auto path=std::string(out)+name;
    auto* f=std::fopen(path.c_str(),"wb");
    // console-capture (docs/psp/next-tasks.md): frame-0000.ppm failed on the PSP-3000 while a 1 MiB probe passed.
    // Report errno so the next console run names the cause (EMFILE, ENOSPC, EIO...).
    if(!f){const int e=errno;throw std::runtime_error("Cannot write "+path+" (errno "+std::to_string(e)+": "+std::strerror(e)+")");}
    return f;
}
class Psp final:public Platform {
    PageResidency residency;
    std::vector<Page> pages;
    Buffer readback;
    Buffer gradePalettes=aligned(3072),linearPalettes=aligned(16384);
    hk3ds::cameraColor::Config paletteGrade;
#ifdef HKPSP_GPU_ATTRIB
    Buffer gradeSource=aligned(bufferBytes);
#endif
    struct LinearKey {unsigned page=~0u,clut=0;std::array<float,4> tint{};};
    std::array<LinearKey,16> linearKeys{};
    unsigned nextLinear=0;
    unsigned usedLinear=0;
    bool fixedPose=false,gradeReady=false;
    std::unique_ptr<FILE,decltype(&std::fclose)> monomonStream{nullptr,&std::fclose};
    Buffer monomonPixels;
    unsigned monomonFrame=0;
    bool stressPageUploads=false;
    bool useRoomGe=false;
    RenderPose pose;
    FILE* metrics=nullptr;
    FILE* pageMetrics=nullptr;
    FILE* roomTraceFile=nullptr;
    RoomGeTrace roomTrace;
    unsigned frame=0,frames,every,peakHeap=0,peakArena=0;
#ifdef HKPSP_PERF
    unsigned gamePeakHeap=0,configHeapStart=0;
#endif
    unsigned submittedQuads=0,peakDisplayList=0;
    unsigned pageSlotLimit=~0u;
    PageCounters framePages;
    struct PinnedPage {unsigned id,evictions;};
    std::vector<PinnedPage> pinnedPages;
    FrameInfo frameInfo{};
    HKPSP_PROFILE(unsigned guUs=0,gePart=0;)
    unsigned lastFlipVcount=0;
    unsigned long long exitHeldSince=0;
    void* drawOffset=nullptr;
    void* gradeVram(){
        static_assert(bufferBytes==557056,"Grade snapshot budget changed");
        const auto capacity=static_cast<unsigned>(sceGeEdramGetSize());
        if(capacity<3*bufferBytes)throw std::runtime_error("Grade target exceeds VRAM");
        return static_cast<unsigned char*>(sceGeEdramGetAddr())+2*bufferBytes;
    }
    bool clampedAdd(const Quad& q,unsigned tint){
        if(gradeReady)throw std::runtime_error("Source LinearDodge must precede camera grade");
        float left=screenWidth,right=0,top=screenHeight,bottom=0;
        for(const auto p:q.positions){left=std::min(left,p.x);right=std::max(right,p.x);top=std::min(top,p.y);bottom=std::max(bottom,p.y);}
        const unsigned x=unsigned(std::clamp(std::floor(left),0.f,float(screenWidth)))&~3u;
        const unsigned y=unsigned(std::clamp(std::floor(top),0.f,float(screenHeight)));
        const unsigned r=std::min(screenWidth,(unsigned(std::clamp(std::ceil(right),0.f,float(screenWidth)))+3u)&~3u);
        const unsigned b=unsigned(std::clamp(std::ceil(bottom),0.f,float(screenHeight)));
        if(r<=x||b<=y)return false;
        void* scene=reinterpret_cast<void*>(2*bufferBytes);
#ifdef HKPSP_GPU_ATTRIB
        if(gpu::config().grade!=gpu::Grade::Target)scene=drawOffset;
#endif
        void* spare=scene==drawOffset?reinterpret_cast<void*>(2*bufferBytes):drawOffset;
#ifdef HKPSP_GPU_ATTRIB
        // Residency starts after the two display buffers. Keep its first-page
        // contract, and reserve the scratch target at the TOP of VRAM instead.
        if(scene==drawOffset)spare=reinterpret_cast<void*>(sceGeEdramGetSize()-bufferBytes);
#endif
        (void)gradeVram();
        auto* vram=static_cast<unsigned char*>(sceGeEdramGetAddr());
        auto* source=vram+reinterpret_cast<std::uintptr_t>(scene);
        auto* target=vram+reinterpret_cast<std::uintptr_t>(spare);
        sceGuCopyImage(GU_PSM_8888,x,y,r-x,b-y,stride,source,x,y,stride,target);stateCommand(sceGuTexSync);
        // Constant-depth quads: passes and composite share one screen-space clipped polygon, so
        // they cover the same pixels on the GE (see spriteScreenTriangle).
        const bool flat=q.inverseDepth[0]==q.inverseDepth[1]&&q.inverseDepth[0]==q.inverseDepth[2]&&
                        q.inverseDepth[0]==q.inverseDepth[3];
        const auto draw=[&](){
#ifndef HKPSP_GPU_ATTRIB
            flushTriangles();
#endif
            if(flat){spriteScreenTriangle(q,{{0,1,2}},tint);spriteScreenTriangle(q,{{0,2,3}},tint);return;}
            triangle(q,{{0,1,2}},tint);triangle(q,{{0,2,3}},tint);
#ifndef HKPSP_GPU_ATTRIB
            flushTriangles();
#endif
        };
        // Scratch = A*clamp(D+S); scene = (1-A)*D. Neither pass uses stencil.
        stateCommand(sceGuDrawBufferList,GU_PSM_8888,spare,stride);
        blendFunc(GU_ADD,GU_FIX,GU_FIX,0xffffff,0xffffff);draw();
        blendFunc(GU_ADD,GU_FIX,GU_SRC_ALPHA,0,0);draw();
        stateCommand(sceGuDrawBufferList,GU_PSM_8888,scene,stride);
        blendFunc(GU_ADD,GU_FIX,GU_ONE_MINUS_SRC_ALPHA,0,0);draw();
        stateCommand(sceGuTexSync);texMode(GU_PSM_8888,0,0,0);
        texImage(0,512,512,stride,target);texScale(1.f/512,1.f/512);
        texFilter(GU_NEAREST,GU_NEAREST);texFlush();texFunc(GU_TFX_REPLACE,GU_TCC_RGBA);
        blendFunc(GU_ADD,GU_FIX,GU_FIX,0xffffff,0xffffff);
        screenTriangle(q,{{0,1,2}});screenTriangle(q,{{0,2,3}});
        texFunc(GU_TFX_MODULATE,GU_TCC_RGBA);
#ifdef HKPSP_GPU_ATTRIB
        texFilter(gpu::config().nearest?GU_NEAREST:GU_LINEAR,gpu::config().nearest?GU_NEAREST:GU_LINEAR);
        activeClut=nullptr;
#else
        texFilter(GU_LINEAR,GU_LINEAR);
#endif
        HKPSP_PROFILE(perf::current[perf::Quads]+=3;++perf::current[perf::TextureBinds];perf::current[perf::BlendChanges]+=3;)
        return true;
    }
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
    void* cpuCapturedOffset=nullptr;
#endif
    std::uint16_t previous=0;
    bool interactiveMenus=false;
    UiInputRouter uiRouter;
    const void* activeClut=nullptr;
    unsigned activeClutBlocks=0;
#ifndef HKPSP_GPU_ATTRIB
    void loadClut(unsigned blocks,const void* palette){
        if(activeClut==palette&&activeClutBlocks==blocks)return;
        stateCommand(sceGuClutLoad,blocks,palette);HKPSP_PROFILE(++perf::current[perf::ClutLoads];)
        activeClut=palette;activeClutBlocks=blocks;
    }
#endif
#ifdef HKPSP_GPU_ATTRIB
    gpu::Pass activePass=gpu::Pass::Far;
    Buffer staleNext;
    void* capturedOffset=nullptr;
    unsigned vramNext=2*bufferBytes;
    Buffer fillIndices,fillPalette;
    void sweep(const Quad& q){
        const auto& c=gpu::config();
        if(c.sweep==gpu::Sweep::Cached16){
            drawGradeStrips();
        }else if(c.sweep==gpu::Sweep::Sprites){
            auto* v=static_cast<Vertex*>(guMemory(2*sizeof(Vertex)));
            v[0]={0,0,0xffffffff,0,0,0};
            v[1]={screenWidth,screenHeight,0xffffffff,screenWidth,screenHeight,0};
            HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_SPRITES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D,2,nullptr,v);
            perf::current[perf::Triangles]+=2;
        }else if(c.strip){
            for(unsigned x=0;x<screenWidth;x+=c.strip){
                const auto right=std::min(x+c.strip,unsigned(screenWidth));Quad strip=q;
                strip.u0=float(x);strip.u1=float(right);
                strip.positions[0].x=strip.positions[3].x=float(x);
                strip.positions[1].x=strip.positions[2].x=float(right);
                triangle(strip,{{0,1,2}},0xffffffff);triangle(strip,{{0,2,3}},0xffffffff);
            }
        }else {triangle(q,{{0,1,2}},0xffffffff);triangle(q,{{0,2,3}},0xffffffff);}
    }
    void finishSegment(){
        const auto started=perf::now();
        finishList();sceGuSync(0,0);
        const auto elapsed=perf::now()-started;
        gpu::current.ge+=elapsed;++gpu::current.segments;
        if(gpu::config().passSync)gpu::current.passUs[unsigned(activePass)]+=elapsed;
    }
    void loadClut(unsigned blocks,const void* palette){
        if(gpu::config().clutCache&&activeClut==palette&&activeClutBlocks==blocks){
            ++gpu::current.clutSkips;return;
        }
        stateCommand(sceGuClutLoad,blocks,palette);++gpu::current.clutLoads;
        HKPSP_PROFILE(++perf::current[perf::ClutLoads];)
        activeClut=palette;activeClutBlocks=blocks;
    }
    void placeVisiblePages(const DrawList& list,unsigned first,unsigned end){
        if(!gpu::config().vram)return;
        const auto total=static_cast<unsigned>(sceGeEdramGetSize());
        if(total<3*bufferBytes)throw std::runtime_error("Composition targets exceed VRAM");
        const auto capacity=total-bufferBytes; // Topmost target is transient composition scratch.
        gpu::vramCapacity=capacity-2*bufferBytes;
        for(unsigned index=first;index<end;++index){
            const auto& q=list.quads[index];
            if(!samplesTexture(q.blend)||!gpu::enabled(list.gpuPasses.at(index))||list.textures->at(q.page).transient)continue;
            auto& p=pages.at(q.page);if(p.visited)continue;p.visited=true;
            const unsigned bytes=p.width*p.height*p.bits/8;
            const bool fits=bytes<=capacity-vramNext;
            const unsigned offset=fits?vramNext:0;
            if(fits){
                auto* address=static_cast<unsigned char*>(sceGeEdramGetAddr())+offset;
                auto* uncached=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(address)|0x40000000u);
                std::memcpy(uncached,p.indices.get(),bytes);
                p.vram=address;vramNext+=bytes;gpu::vramUsed+=bytes;
            }
            gpu::residency.push_back({q.page,p.width,p.height,p.bits,bytes,list.info.frame,offset,fits});
        }
    }
#endif
    void measureHeap(){
        const auto info=mallinfo();
        peakHeap=std::max(peakHeap,static_cast<unsigned>(info.uordblks));
        peakArena=std::max(peakArena,static_cast<unsigned>(info.arena));
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
        if(!readback)gamePeakHeap=std::max(gamePeakHeap,unsigned(info.uordblks));
#endif
    }
    void writeMemory(){
        measureHeap();
        // GPU configs own separate backends; report the entire probe's high water.
        static unsigned runHeap=0,runArena=0,runDisplayList=0;
        peakHeap=runHeap=std::max(runHeap,peakHeap);
        peakArena=runArena=std::max(runArena,peakArena);
        peakDisplayList=runDisplayList=std::max(runDisplayList,peakDisplayList);
        FILE* memory=output("memory.txt");
        unsigned textureBytes=0,loads=0,evictions=0;
        for(const auto& p:pages){
            if(p.indices)textureBytes+=p.bytes();
            loads+=p.loads;evictions+=p.evictions;
        }
        std::fprintf(memory,"peak_sampled_heap_bytes=%u\npeak_sampled_heap_arena_bytes=%u\ntexture_page_bytes=%u\ntexture_page_loads=%u\ntexture_page_evictions=%u\npeak_display_list_bytes=%u\n",
                     peakHeap,peakArena,textureBytes,loads,evictions,peakDisplayList);
        if(stressPageUploads)for(unsigned i=0;i<pages.size();++i){
            const auto* name=std::strrchr(memory_report::texturePath(i),'/');
            if(name&&!std::strncmp(name,"/page-",6))
                std::fprintf(memory,"%s_loads=%u\n%s_evictions=%u\n",name+1,pages[i].loads,name+1,pages[i].evictions);
        }
        if(std::fclose(memory))throw std::runtime_error("Memory report write failed");
    }
    void checkPinnedPages()const{
        for(const auto& pin:pinnedPages){
            const auto& p=pages.at(pin.id);
            if(!p.indices||p.evictions!=pin.evictions)
                throw std::runtime_error("Submitted texture page replaced before GE sync");
        }
    }
    void capture(){
#ifdef HKPSP_GPU_ATTRIB
        char name[40];std::snprintf(name,sizeof(name),"%s.ppm",gpu::config().name);
#elif defined(HKPSP_PERF)
        char name[40];std::snprintf(name,sizeof(name),"cpu-%s.ppm",cpu::config().name);
#else
        char name[40];std::snprintf(name,sizeof(name),"frame-%04u.ppm",frame);
#endif
        FILE* f=output(name);std::fprintf(f,"P6\n%u %u\n255\n",screenWidth,screenHeight);
        const auto address=reinterpret_cast<std::uintptr_t>(readback.get());
        const auto* pixels=reinterpret_cast<const volatile unsigned char*>(address|0x40000000u);
        unsigned char row[screenWidth*3];bool ok=true;
        for(unsigned y=0;y<screenHeight;++y){
            for(unsigned x=0;x<screenWidth;++x)for(unsigned c=0;c<3;++c)row[x*3+c]=pixels[(y*stride+x)*4+c];
            if(std::fwrite(row,1,sizeof(row),f)!=sizeof(row)){ok=false;break;}
        }
        const int closed=std::fclose(f);if(!ok||closed)throw std::runtime_error("Frame capture write failed");
    }
public:
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
    void resetCpu(){
        // runGameConfig owns all room banks, indices, plans and scratch by value;
        // returning from it destroys them before the next mode loads any data.
        if(DrawList::geScene)throw std::runtime_error("Room GE scene survived config teardown");
        pages.clear();readback.reset();linearKeys={};nextLinear=usedLinear=0;pending.clear();
#ifndef HKPSP_PERF_SWEEP
        if(cpu::roomGeAB){
            // Both modes use the same texture bank. Retain its contiguous pool,
            // as the sweep does: reallocating it after the next room loads can
            // fail on a fragmented 16 MiB heap. Drop file handles and all CPU
            // scene/scratch ownership before loading that room.
            residency.closeFiles();std::vector<Page>().swap(pages);
            geometry=GeometryScratch{};clippedGeometry=ClippedGeometryScratch{};std::vector<Vertex>().swap(pending);
            std::vector<PinnedPage>().swap(pinnedPages);currentGeometry=nullptr;
            if(geometry.size()||pending.capacity()||pages.capacity())
                throw std::runtime_error("Room backend storage survived config teardown");
        }
#endif
        configHeapStart=gamePeakHeap=unsigned(mallinfo().uordblks);
    }
    void captureCpu(){
        measureHeap();
#ifndef HKPSP_PERF_SWEEP
        if(cpu::roomGeAB){
            if(DrawList::geScene)throw std::runtime_error("Room GE scene survived config teardown");
            const auto heapAfterRelease=unsigned(mallinfo().uordblks);
            const auto path=std::string(perf::directory)+"room-ge-lifetime.csv";
            FILE* report=std::fopen(path.c_str(),cpu::configIndex?"ab":"wb");
            if(!report)throw std::runtime_error("Cannot write room GE lifetime");
            if(!cpu::configIndex)std::fprintf(report,"config,heap_before_room_load_bytes,peak_sampled_game_heap_bytes,heap_after_room_release_bytes,room_scene_alive\n");
            std::fprintf(report,"%s,%u,%u,%u,0\n",cpu::config().name,configHeapStart,gamePeakHeap,heapAfterRelease);perf::close(report);
        }
#endif
        if(cpu::configIndex==0){
            // Post-run raw Memory Stick probe; never included in frame timers.
            auto bytes=aligned(256*1024);FILE* report=output("ms-raw.csv");
            std::fprintf(report,"requests,bytes,open_us,read_us,checksum\n");
            for(unsigned requests:{1u,4u}){
                const auto start=perf::now();const int fd=sceIoOpen(memory_report::texturePath(0),PSP_O_RDONLY,0);
                const auto opened=perf::now();
                if(fd<0)throw std::runtime_error("Raw MS probe open failed");
                for(unsigned i=0;i<requests;++i)if(sceIoRead(fd,bytes.get()+i*(256*1024/requests),256*1024/requests)!=int(256*1024/requests)){
                    sceIoClose(fd);throw std::runtime_error("Raw MS probe short read");}
                const auto elapsed=perf::now()-opened;sceIoClose(fd);
                unsigned hash=2166136261u;for(unsigned i=0;i<256*1024;++i)hash=(hash^bytes.get()[i])*16777619u;
                std::fprintf(report,"%u,262144,%u,%u,%u\n",requests,opened-start,elapsed,hash);
            }
            perf::close(report);
        }
        readback=aligned(bufferBytes);measureHeap();sceKernelDcacheWritebackInvalidateRange(readback.get(),bufferBytes);
        auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+reinterpret_cast<std::uintptr_t>(cpuCapturedOffset);
        sceGuStart(GU_DIRECT,commands);resetState();
        sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,source,0,0,stride,readback.get());
        finishList();sceGuSync(0,0);capture();writeMemory();
    }
#endif
#ifdef HKPSP_GPU_ATTRIB
    void captureGpu(){
        // Called after runGame/perf::end: no readback or file I/O in timed frames.
        measureHeap();readback=aligned(bufferBytes);measureHeap();
        sceKernelDcacheWritebackInvalidateRange(readback.get(),bufferBytes);
        auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+reinterpret_cast<std::uintptr_t>(capturedOffset);
        sceGuStart(GU_DIRECT,commands);resetState();
        sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,source,0,0,stride,readback.get());
        finishList();sceGuSync(0,0);capture();writeMemory();
    }
#endif
    Psp(unsigned count,unsigned interval,bool menus):frames(count),every(interval),interactiveMenus(menus){
        if(!uiReplay().empty())interactiveMenus=true;
#if !defined(HKPSP_GPU_ATTRIB) && !defined(HKPSP_ROOM_GE_OFF)
        // Stage-2 opt-in; read once before RoomIndex builds either geometry mode.
        SceIoStat ge{};
        useRoomGe=sceIoGetstat("ms0:/PSP/GAME/HKPSP/data/room-ge.cfg",&ge)>=0;
#if defined(HKPSP_PERF) && !defined(HKPSP_PERF_SWEEP)
        cpu::roomGeAB=sceIoGetstat("ms0:/PSP/GAME/HKPSP/data/room-ge-ab.cfg",&ge)>=0;
#endif
#endif
        // Explicit diagnostic run only: exercise the real draw-list hero pages
        // on every frame, after the previous GE list has completed.
        SceIoStat stress{};
        stressPageUploads=frames&&sceIoGetstat("ms0:/PSP/GAME/HKPSP/data/page-upload-stress.cfg",&stress)>=0;
        // Capture-only pressure injection: compare the same frame with fewer
        // slots. Ordinary play always uses the unchanged production pool.
        if(frames)if(auto* f=std::fopen("ms0:/PSP/GAME/HKPSP/data/page-slot-limit.cfg","rb")){
            char extra[2]{};
            const auto fields=std::fscanf(f,"%u %1s",&pageSlotLimit,extra);std::fclose(f);
            if(fields!=1||!pageSlotLimit)throw std::runtime_error("Invalid page-slot-limit.cfg");
        }
#ifndef HKPSP_PERF
        // Capture-only counterpart of host --pose; absent in ordinary play.
        if(frames)if(auto* f=std::fopen("ms0:/PSP/GAME/HKPSP/data/look-pose.cfg","rb")){
            char extra[2]{};
            const int fields=std::fscanf(f,"hero=%f,%f camera=%f,%f %1s",&pose.heroX,&pose.heroY,&pose.cameraX,&pose.cameraY,extra);
            std::fclose(f);
            if(fields!=4||!std::isfinite(pose.heroX)||!std::isfinite(pose.heroY)||
               !std::isfinite(pose.cameraX)||!std::isfinite(pose.cameraY))throw std::runtime_error("Invalid look-pose.cfg");
            fixedPose=true;
        }
#endif
        updateGradePalettes(paletteGrade);
        sceKernelDcacheWritebackRange(gradeStrips.data(),sizeof(gradeStrips));
#ifdef HKPSP_GPU_ATTRIB
        sceKernelDcacheWritebackInvalidateRange(gradeSource.get(),bufferBytes);
#endif
#ifdef HKPSP_GPU_ATTRIB
        if(gpu::config().grade==gpu::Grade::Stale){
            staleNext=aligned(bufferBytes);sceKernelDcacheWritebackInvalidateRange(staleNext.get(),bufferBytes);
        }
        if(gpu::config().sweep==gpu::Sweep::Clut8){
            fillIndices=aligned(512*512);fillPalette=aligned(1024);
            for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x){
                const unsigned offset=((y/8)*32+x/16)*128+(y%8)*16+x%16;
                fillIndices.get()[offset]=static_cast<unsigned char>(x^y);
            }
            for(unsigned i=0;i<256;++i)for(unsigned c=0;c<4;++c)
                fillPalette.get()[i*4+c]=c==3?255:i;
            sceKernelDcacheWritebackRange(fillIndices.get(),512*512);
            sceKernelDcacheWritebackRange(fillPalette.get(),1024);
        }
#endif
#ifdef HKPSP_PERF
        // Replay selection belongs to the loop; disable captures, heap probes and
        // trajectory I/O in this backend so they cannot contaminate the timing.
        frames=0;
#endif
        if(frames){
            readback=aligned(bufferBytes);
            metrics=output("metrics.csv");
            std::fprintf(metrics,"frame,hero_x,hero_y,draws,quads,camera_x,camera_y,control_steps,velocity_x,velocity_y,actor,phase,health,soul,geo,scene,simulation_x,simulation_y,ability_phase,transition_fade\n");
            pageMetrics=output("page-residency.csv");
            SceIoStat trace{};
            if(sceIoGetstat("ms0:/PSP/GAME/HKPSP/data/room-ge-parity.cfg",&trace)>=0){
                roomTraceFile=output("room-ge-parity.csv");
                std::fprintf(roomTraceFile,"frame,draws,state_hash,cpu,ge,scene_bytes,ge_enabled\n");
            }
            std::fprintf(pageMetrics,"frame,page_loads,page_open_us,page_read_us,page_verify_us,page_evictions,page_resident_bytes,page_blocking_miss,page_loaded_ids,page_overflow_frames,page_ge_waits,page_ge_wait_us,page_vram_bytes,page_vram_free_bytes,page_pool_slots,page_fixed_pages,page_pass_evictions\n");
        }
        sceCtrlSetSamplingCycle(0);sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        sceGuInit();
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
        sceGuSetCallback(GU_CALLBACK_SIGNAL,geMarker);
#endif
        sceGuStart(GU_DIRECT,commands);resetState();
        stateCommand(sceGuDrawBuffer,GU_PSM_8888,nullptr,stride);
        stateCommand(sceGuDispBuffer,screenWidth,screenHeight,reinterpret_cast<void*>(bufferBytes),stride);
        stateCommand(sceGuOffset,2048-screenWidth/2,2048-screenHeight/2);
        stateCommand(sceGuViewport,2048,2048,screenWidth,screenHeight);
        stateCommand(sceGuScissor,0,0,screenWidth,screenHeight);stateCommand(sceGuEnable,GU_SCISSOR_TEST);
        stateCommand(sceGuEnable,GU_CLIP_PLANES);
        stateCommand(sceGuDisable,GU_DEPTH_TEST);stateCommand(sceGuDisable,GU_CULL_FACE);stateCommand(sceGuDisable,GU_LIGHTING);
        stateCommand(sceGuDisable,GU_ALPHA_TEST);stateCommand(sceGuDisable,GU_DITHER);
        stateCommand(sceGuEnable,GU_TEXTURE_2D);stateCommand(sceGuEnable,GU_BLEND);stateCommand(sceGuShadeModel,GU_FLAT);
        texMode(GU_PSM_T8,0,0,1);clutMode(GU_PSM_8888,0,255,0);
        texFunc(GU_TFX_MODULATE,GU_TCC_RGBA);
        // All room/hero UVs describe texel edges; centres are n+.5. Keep UVs
        // unchanged, matching host taps at floor(u-.5),floor(v-.5). Atlas sprites
        // have replicated 1px borders, so GE linear taps cannot bleed neighbours.
        texFilter(GU_LINEAR,GU_LINEAR);stateCommand(sceGuTexWrap,GU_CLAMP,GU_CLAMP);
        stateCommand(sceGuTexMapMode,GU_TEXTURE_COORDS,0,0);stateCommand(sceGuTexOffset,0,0);
        ScePspFMatrix4 identity{};identity.x.x=identity.y.y=identity.z.z=identity.w.w=1;
        stateCommand(sceGuSetMatrix,GU_MODEL,&identity);stateCommand(sceGuSetMatrix,GU_VIEW,&identity);
        // Vertices store (screenX*z,screenY*z,z); clip.w=z restores the
        // draw-list's inverseDepth and perspective-correct UV interpolation.
        ScePspFMatrix4 projection{};
        projection.x.x=2.f/screenWidth;projection.y.y=-2.f/screenHeight;
        projection.z.x=-1;projection.z.y=1;projection.z.w=1;
        stateCommand(sceGuSetMatrix,GU_PROJECTION,&projection);
        sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuDisplay(GU_TRUE);
        lastFlipVcount=static_cast<unsigned>(sceDisplayGetVcount());
    }
    ~Psp()override{if(metrics)std::fclose(metrics);if(pageMetrics)std::fclose(pageMetrics);if(roomTraceFile)std::fclose(roomTraceFile);sceGuTerm();}
    bool roomGeEnabled()const override{
#if defined(HKPSP_PERF) && !defined(HKPSP_PERF_SWEEP) && !defined(HKPSP_GPU_ATTRIB) && !defined(HKPSP_ROOM_GE_OFF)
        if(cpu::roomGeAB)return cpu::configIndex==1;
#endif
        return useRoomGe;
    }
    std::string audioOutput()const override{return out;}
    SceCtrlData readPad(){
        SceCtrlData pad{};sceCtrlPeekBufferPositive(&pad,1);
        if(!frames){
            const auto now=sceKernelGetSystemTimeWide();
            if((pad.Buttons&(PSP_CTRL_START|PSP_CTRL_SELECT))==(PSP_CTRL_START|PSP_CTRL_SELECT)){
                if(!exitHeldSince)exitHeldSince=now;
                if(now-exitHeldSince>=1000000)sceKernelExitGame();
            }else exitHeldSince=0;
        }
        return pad;
    }
    bool menusEnabled()const override{return interactiveMenus&&!fixedPose;}
    std::string uiReplay()const override{
#if !defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB) && !defined(HKPSP_DIAG)
        const char* tape="ms0:/PSP/GAME/HKPSP/data/menu-replay.hkir";
        if(auto* file=std::fopen(tape,"rb")){std::fclose(file);return tape;}
#endif
        return {};
    }
    UiInput pollUi()override{
        const auto pad=readPad();unsigned held=0;
        constexpr unsigned masks[]={PSP_CTRL_UP,PSP_CTRL_DOWN,PSP_CTRL_LEFT,PSP_CTRL_RIGHT,
            PSP_CTRL_CIRCLE,PSP_CTRL_CROSS,PSP_CTRL_START,PSP_CTRL_SELECT,PSP_CTRL_TRIANGLE};
        constexpr unsigned buttons[]={UiUp,UiDown,UiLeft,UiRight,UiCircle,UiCross,UiStart,UiSelect,UiTriangle};
        for(unsigned i=0;i<9;++i)if(pad.Buttons&masks[i])held|=buttons[i];
        return uiRouter.sample(held,1./30.);
    }
    hk3ds::Input poll()override{
        SceCtrlData pad=readPad();hk3ds::Input result;
        HKPSP_DIAG_ONLY(diag::controller(pad.Buttons,pad.Lx,pad.Ly);)
        const auto axis=[](int value){return std::abs(value)<=32?0.f:float(value>0?value-32:value+32)/96.f;};
        result.x=axis(int(pad.Lx)-128);result.y=-axis(int(pad.Ly)-128);
        if(pad.Buttons&PSP_CTRL_LEFT)result.x=-1;
        if(pad.Buttons&PSP_CTRL_RIGHT)result.x=1;
        if(pad.Buttons&PSP_CTRL_UP)result.y=1;
        if(pad.Buttons&PSP_CTRL_DOWN)result.y=-1;
        constexpr unsigned masks[]={PSP_CTRL_CROSS,PSP_CTRL_SQUARE,PSP_CTRL_CIRCLE,PSP_CTRL_TRIANGLE,PSP_CTRL_RTRIGGER,PSP_CTRL_LTRIGGER};
        for(unsigned i=0;i<6;++i)if(pad.Buttons&masks[i])result.held|=buttonBindings[i].simulation;
        if(result.y<-.5f)result.held|=hk3ds::Down;
        result.pressed=result.held&~previous;result.released=previous&~result.held;previous=result.held;
        return result;
    }
    std::string path(const std::string& name)const override{
        if(!validAssetName(name))
            throw std::runtime_error("Invalid logical asset name");
        // stage-data preserves logical names, including the four regenerated
        // room sidecars selected with their PSR1 table by the host asset trace.
        return "ms0:/PSP/GAME/HKPSP/data/"+name;
    }
    std::string sceneRoot()const override{return "ms0:/PSP/GAME/HKPSP/data";}
    bool audioDiagnostics()const override{return every!=0;}
    std::string savePath()const override{
#if !defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB) && !defined(HKPSP_DIAG)
        if(!fixedPose&&(interactiveMenus||!frames||!every||captureRestoresSave))
            return "ms0:/PSP/GAME/HKPSP/tutorial.sav";
#endif
        return {};
    }
    std::string inputReplay()const override{
        if(interactiveMenus)return {};
#ifndef HKPSP_PERF
        const char* tape="ms0:/PSP/GAME/HKPSP/data/room-replay.hkir";
        if(auto* file=std::fopen(tape,"rb")){std::fclose(file);return tape;}
#endif
        return {};
    }
    void releaseRoomAssets(unsigned retainedPages)override{
        // The previous endFrame synchronized the GE. No Page may retain a
        // palette/texel pointer or VRAM address belonging to the replaced room.
        residency.closeFiles();pages.clear();residency.reset(retainedPages!=0);
        linearKeys={};nextLinear=usedLinear=0;
#ifdef HKPSP_GPU_ATTRIB
        activeClut=nullptr;activeClutBlocks=0;vramNext=2*bufferBytes;
#endif
    }
    void releaseTexture(unsigned id)override{
        residency.closeFiles();
        if(id<pages.size()&&pages[id].indices){
            HKPSP_DIAG_ONLY(diag::page(id,true,pages[id].indices.get());)
            pages[id].evict();
        }
        for(auto& key:linearKeys)if(key.page==id)key={};
    }
    void initializeResidency(const std::vector<Texture>& textures){
        const unsigned vramSize=sceGeEdramGetSize();
        if(vramSize<3*bufferBytes)throw std::runtime_error("Composition targets exceed VRAM");
        auto* pageVram=static_cast<unsigned char*>(sceGeEdramGetAddr())+3*bufferBytes;
        pageVram=reinterpret_cast<unsigned char*>(reinterpret_cast<std::uintptr_t>(pageVram)|0x40000000u);
#ifdef HKPSP_GPU_ATTRIB
        if(gpu::config().vram)residency.initialize(textures,pageSlotLimit);
        else
#endif
        residency.initialize(textures,pageSlotLimit,pageVram,vramSize-3*bufferBytes);
    }
    void preloadRoomPages(const std::vector<Texture>& textures,const char* scene,bool startup)override{
        // Page-demand is disabled for the shipped package. Ignore manifests that
        // may remain on a memory stick after copying over an older installation.
        (void)textures;(void)scene;(void)startup;
        return;
#if 0
        if(stressPageUploads||pageSlotLimit!=~0u)return;
#ifdef HKPSP_GPU_ATTRIB
        if(gpu::config().vram)return;
#endif
        const auto manifest=path(startup?"room-pages-startup.txt":std::string("rooms/")+scene+"/room-pages.txt");
        std::unique_ptr<FILE,decltype(&std::fclose)> input(std::fopen(manifest.c_str(),"rb"),&std::fclose);
        if(!input){
            if(errno==ENOENT)return;
            throw std::runtime_error("Cannot open page manifest: "+manifest);
        }
        std::vector<unsigned> ids;
        char line[192];
        while(std::fgets(line,sizeof line,input.get())){
            const auto length=std::strlen(line);
            if(!length||line[length-1]!='\n')throw std::runtime_error("Invalid page manifest: "+manifest);
            line[length-1]=0;
            const bool optional=line[0]=='?';
            const char* relative=line+optional;
            if(!*relative||(optional&&std::strncmp(relative,"rooms/",6)!=0))
                throw std::runtime_error("Invalid page manifest: "+manifest);
            const auto pagePath=path(relative);
            unsigned id=0;
            for(;id<textures.size();++id)
                if(textures[id].width&&pagePath==memory_report::texturePath(id))break;
            if(optional&&id==textures.size())continue;
            if(id==textures.size()||std::find(ids.begin(),ids.end(),id)!=ids.end())
                throw std::runtime_error("Invalid preload page: "+pagePath);
            ids.push_back(id);
        }
        if(ids.empty()||std::ferror(input.get()))throw std::runtime_error("Invalid page manifest: "+manifest);
        input.reset();
        initializeResidency(textures);
        residency.describePages(pages,textures);
        unsigned pooled=0;
        for(unsigned id:ids)if(!residency.fixed(pages[id]))++pooled;
        if(pooled>residency.slots()){
            if(every)std::printf("Page preload skipped %s: %u/%u pooled\n",scene,pooled,residency.slots());
            return;
        }
        for(unsigned id:ids)if(!pages[id].indices)residency.load(pages[id],textures[id],id,pages);
        residency.closeFiles();
        if(every)std::printf("Page preload %s: %u pages (%u pooled)\n",scene,unsigned(ids.size()),pooled);
#endif
    }
    double seconds()const override{return double(sceKernelGetSystemTimeWide())/1000000.;}
    const RenderPose* renderPose()const override{return fixedPose?&pose:nullptr;}
    void finishList(){
        requireGuCapacity(0);
        const auto bytes=static_cast<unsigned>(sceGuFinish());
        if(bytes>sizeof(commands))throw std::runtime_error("GU command capacity exceeded");
        peakDisplayList=std::max(peakDisplayList,bytes);
    }
    void beginFrame()override{
        if(!monomonStream)monomonPixels.reset();
        HKPSP_DIAG_ONLY(diag::begin();)
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
        geSeen=0;
#endif
        HKPSP_PROFILE(const auto started=perf::now();guUs=gePart=0;)
        HKPSP_PROFILE(cpu::change(cpu::Upload);)
        submittedQuads=0;pinnedPages.clear();
        roomTrace={};
        framePages.loads=framePages.openUs=framePages.readUs=framePages.verifyUs=0;
        framePages.evictions=framePages.residentBytes=framePages.blockingMiss=0;
        framePages.overflowFrames=framePages.geWaits=framePages.geWaitUs=0;
        framePages.passEvictions=0;
        framePages.loadedIds.clear();
#ifdef HKPSP_GPU_ATTRIB
        gpu::current={};activePass=gpu::Pass::Far;
        perf::current[perf::Quads]=0;
#endif
        activeClut=nullptr;activeClutBlocks=0;
        sceGuStart(GU_DIRECT,commands);resetState();
#ifdef HKPSP_GPU_ATTRIB
        if(gpu::config().grade==gpu::Grade::Target){
#else
        {
#endif
            (void)gradeVram();stateCommand(sceGuDrawBufferList,GU_PSM_8888,reinterpret_cast<void*>(2*bufferBytes),stride);
        }
        stateCommand(sceGuClearColor,0xff000000);sceGuClear(GU_COLOR_BUFFER_BIT);
        gradeReady=false;
        usedLinear=0;
        HKPSP_PROFILE(cpu::change(-1);guUs+=perf::now()-started;)
    }
    void updateGradePalettes(const hk3ds::cameraColor::Config& grade){
        for(unsigned i=0;i<3;++i){
            const auto palette=cameraPalette(static_cast<Blend>(unsigned(Blend::GradeGreen)+i),grade);
            std::memcpy(gradePalettes.get()+i*1024,palette.data(),1024);
        }
        sceKernelDcacheWritebackRange(gradePalettes.get(),3072);
        paletteGrade=grade;
    }
    void submit(const DrawList& list)override{
        HKPSP_PROFILE(const auto started=perf::now();const auto clipBefore=perf::current[perf::Clipping];
            unsigned pageWaitUs=0;cpu::change(cpu::Upload);)
#ifdef HKPSP_GPU_ATTRIB
        const auto geBefore=gpu::current.ge;
#endif
        const RoomGeSubmission submission(list);
        const PageSequence pageSequence{&submission,submission.size(),[](const void* context,unsigned i){
            return static_cast<const RoomGeSubmission*>(context)->page(i);
        }};
        if(submission.size()<submittedQuads)throw std::runtime_error("Submitted draw-list prefix was removed");
        frameInfo=list.info;
#ifndef HKPSP_GPU_ATTRIB
        HKPSP_PROFILE(perf::current[perf::Quads]=submission.size();)
#endif
        if(!list.textures)throw std::runtime_error("Draw list has no texture bank");
        if(stressPageUploads&&!submittedQuads)for(unsigned i=0;i<pages.size();++i){
            const auto* name=std::strrchr(memory_report::texturePath(i),'/');
            // Room and actor pages: legacy page-N and, since the repack, shared-page-N and
            // static-room-page-N (2026-11-17: the old prefix matched none, so stress was a no-op).
            const bool room=name&&(!std::strncmp(name,"/page-",6)||!std::strncmp(name,"/shared-page-",13)||
                                   !std::strncmp(name,"/static-room-page-",18));
            if(room&&pages[i].indices)pages[i].evict();
        }
        initializeResidency(*list.textures);
        residency.beginPass(pages,list.info.frame);
        do {
        HKPSP_PROFILE(++perf::current[perf::SubmitBatches];const auto pagesStarted=perf::now();)
        // Keep earlier GE references pinned while preparing a fitting suffix.
        checkPinnedPages();
        // Validate the entire suffix before changing pins. Every resident page
        // belongs to the bounded pool established by preparePages, so their
        // union fits without eviction. Frame start / pressure sync clear pins.
        bool reuse=submittedQuads!=0;
        if(reuse)for(unsigned i=submittedQuads;i<submission.size();++i){
            const unsigned id=submission.page(i);
            if(id!=~0u&&!pages.at(id).indices){reuse=false;break;}
        }
        if(reuse){
            for(unsigned i=submittedQuads;i<submission.size();++i){
                const unsigned id=submission.page(i);if(id==~0u)continue;
                auto& p=pages.at(id);
                if(!p.required){p.required=true;pinnedPages.push_back({id,p.evictions});}
                p.lastUse=list.info.frame;residency.used(p);
            }
            unsigned bytes=0,count=0;for(const auto& pin:pinnedPages)if(!residency.fixed(pages[pin.id])){
                bytes+=pages[pin.id].bytes();++count;
            }
            if(count>residency.slots()||bytes>residency.poolBytes())
                throw std::runtime_error("Resident page union exceeds pool");
        }
        const auto batchEnd=reuse?submission.size():
            preparePages(residency,pages,*list.textures,pageSequence,list.info.frame,pagePoolBudget,submittedQuads);
        HKPSP_PROFILE(if(!reuse)++perf::current[perf::PagePrepareBatches];)
        HKPSP_PROFILE(perf::current[perf::UploadPagesUs]+=perf::now()-pagesStarted;
            const auto bookkeepingStarted=perf::now();)
        if(!reuse){
        checkPinnedPages();
        // The loader counts prepare calls; expose one blocking-miss flag per frame.
        if(framePages.blockingMiss&&pageFrame.blockingMiss)--pageTotals.blockingMiss;
        pinnedPages.clear();
        for(unsigned i=0;i<pages.size();++i)if(pages[i].required)
            pinnedPages.push_back({i,pages[i].evictions});
        framePages.loads+=pageFrame.loads;framePages.openUs+=pageFrame.openUs;
        framePages.readUs+=pageFrame.readUs;framePages.verifyUs+=pageFrame.verifyUs;
        framePages.evictions+=pageFrame.evictions;
        framePages.passEvictions+=pageFrame.passEvictions;
        residency.describe(framePages);
        framePages.residentBytes=std::max(framePages.residentBytes,pageFrame.residentBytes);
        framePages.blockingMiss|=pageFrame.blockingMiss;
        framePages.loadedIds.insert(framePages.loadedIds.end(),pageFrame.loadedIds.begin(),pageFrame.loadedIds.end());
        HKPSP_PROFILE(
            perf::current[perf::PageLoads]+=pageFrame.loads;
            perf::current[perf::PageOpen]+=pageFrame.openUs;
            perf::current[perf::PageRead]+=pageFrame.readUs;
            perf::current[perf::PageVerify]+=pageFrame.verifyUs;
            perf::current[perf::PageEvictions]+=pageFrame.evictions;
            perf::current[perf::PageLoad]+=pageFrame.openUs+pageFrame.readUs+pageFrame.verifyUs;
            perf::current[perf::ResidentBytes]=std::max(perf::current[perf::ResidentBytes],pageFrame.residentBytes);
            perf::current[perf::BlockingMiss]|=pageFrame.blockingMiss;
            perf::current[perf::PageVramBytes]=pageFrame.vramBytes;
            perf::current[perf::PageVramFreeBytes]=pageFrame.vramFreeBytes;
            perf::current[perf::PagePoolSlots]=pageFrame.poolSlots;
            perf::current[perf::PageFixedPages]=pageFrame.fixedPages;
            perf::current[perf::PagePassEvictions]+=pageFrame.passEvictions;
            for(const auto id:pageFrame.loadedIds){
                if(!perf::current.pageLoadedIds.empty())perf::current.pageLoadedIds+=';';
                perf::current.pageLoadedIds+=std::to_string(id);
            })
        }

#ifdef HKPSP_GPU_ATTRIB
        placeVisiblePages(list,submittedQuads,batchEnd);
        texFilter(gpu::config().nearest?GU_NEAREST:GU_LINEAR,gpu::config().nearest?GU_NEAREST:GU_LINEAR);
#endif
        HKPSP_PROFILE(perf::current[perf::UploadBookkeepingUs]+=perf::now()-bookkeepingStarted;)
        // Keep allocation-event samples, including intermediate pressure chunks,
        // without walking the heap again for every allocation-free pass.
        if(!reuse&&pageFrame.loads&&frames){
            HKPSP_PROFILE(const auto heapStarted=perf::now();)
            measureHeap();
            HKPSP_PROFILE(perf::current[perf::UploadHeapUs]+=perf::now()-heapStarted;)
        }
        frame=list.info.frame;
#ifndef HKPSP_GPU_ATTRIB
        HKPSP_PROFILE(cpu::change(-1);)
        {HKPSP_PROFILE(cpu::Scope scope(cpu::Clip);)
            HKPSP_PROFILE(const auto storageStarted=perf::now();)
            // Every consumed entry resets count/split below. Keep initialized
            // storage across passes and frames; capacity already had this peak.
            HKPSP_PROFILE(if(list.quads.size()>geometry.size())perf::current[perf::ClipInitializedBytes]+=
                (list.quads.size()-geometry.size())*sizeof(Geometry);)
            geometry.grow(list.quads.size());
            clippedGeometry.beginBatch();
            HKPSP_PROFILE(perf::current[perf::ClipStorageUs]+=perf::now()-storageStarted;
                const auto trianglesStarted=perf::now();const auto clippedBefore=clippedGeometry.highWater();)
            for(unsigned i=submittedQuads;i<batchEnd;++i){
                const auto item=submission.at(i);if(item.world())continue;
                auto& g=geometry[item.cpu];g={};
                if(cameraGrade(list.quads[item.cpu].blend))continue;
                prepareGeometry(list.quads[item.cpu],g,clippedGeometry);
            }
            HKPSP_PROFILE(perf::current[perf::ClipTrianglesUs]+=perf::now()-trianglesStarted;
                perf::current[perf::ClipInitializedBytes]+=(clippedGeometry.highWater()-clippedBefore)*sizeof(ClippedVertices);)
        }
        HKPSP_PROFILE(perf::current[perf::Clipping]=cpu::times[cpu::Clip];
        cpu::change(cpu::Commands);const unsigned roomCommandsBefore=sceGuCheckList();)
        const Quad* previousState=nullptr;
        bool worldProjection=false;
        const auto worldMatrix=roomGeProjection({list.info.cameraX,list.info.cameraY},
                                               hk3ds::roomFocalLength(screenWidth,screenHeight));
        HKPSP_PROFILE(if(!submittedQuads){
            cpuStamp[0]=perf::runClock(perf::gameThread);sceGuSignal(GU_SIGNAL_WAIT,0);})
        const auto simple=[](const Quad& q){return samplesTexture(q.blend)&&q.blend!=Blend::ClampedAdd&&q.whiteFlash<=0&&
            q.material!=hk3ds::RoomMaterial::LinearLight&&q.material!=hk3ds::RoomMaterial::AmbientAdditive;};
#endif
        Quad worldState;
        for(unsigned index=submittedQuads;index<batchEnd;++index){
            const auto item=submission.at(index);
            if(item.world())worldState=submission.state(item);
            const auto& q=item.world()?worldState:list.quads[item.cpu];
#ifndef HKPSP_GPU_ATTRIB
            HKPSP_PROFILE(const unsigned part=cameraGrade(q.blend)?1u:(gePart?2u:0u);
                if(part!=gePart){
                    if(part!=gePart+1)throw std::runtime_error("Noncontiguous GE grade pass");
                    flushTriangles();previousState=nullptr;cpuStamp[part]=perf::runClock(perf::gameThread);
                    sceGuSignal(GU_SIGNAL_WAIT,int(part));gePart=part;
                })
            if(item.world()!=worldProjection){
                flushTriangles();previousState=nullptr;
                setRoomGeProjection(item.world()?worldMatrix:roomExactProjection());
                worldProjection=item.world();
            }
            currentGeometry=item.world()?nullptr:&geometry[item.cpu];
            HKPSP_PROFILE(emittingRoom=item.world()||list.isRoomQuad(item.cpu);)
            if(!item.world()&&!roomEmission()&&cpu::config().c3){
                if(previousState&&simple(q)&&sameBatchState(q,*previousState)){
                    const auto tint=color(q,(q.material==hk3ds::RoomMaterial::Screen&&q.blend!=Blend::ScreenPremult)||
                        q.material==hk3ds::RoomMaterial::DarknessCutout);
                    triangle(q,{{0,1,2}},tint);triangle(q,{{0,2,3}},tint);continue;
                }
                flushTriangles();previousState=nullptr;
            }
#endif
#ifdef HKPSP_GPU_ATTRIB
            const auto group=list.gpuPasses.at(&q-list.quads.data());
            if(!gpu::enabled(group))continue;
            if(gpu::config().passSync&&group!=activePass){
                if(group==gpu::Pass::Grade&&gpu::config().grade==gpu::Grade::Stale){
                    // Maintain the next snapshot in the preceding scene segment.
                    // The isolated grade segment reads the previous frame only.
                    auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+reinterpret_cast<std::uintptr_t>(drawOffset);
                    sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,source,0,0,stride,staleNext.get());stateCommand(sceGuTexSync);
                }
                finishSegment();sceGuStart(GU_DIRECT,commands);resetState();activePass=group;
                if(gpu::config().grade==gpu::Grade::Target&&group!=gpu::Pass::Grade&&group!=gpu::Pass::Hud){
                    // sceGuStart re-emits the SDK's double-buffer draw pointer.
                    // Every segmented scene list must restore the third target.
                    stateCommand(sceGuDrawBufferList,GU_PSM_8888,reinterpret_cast<void*>(2*bufferBytes),stride);
                }
            }
            ++perf::current[perf::Quads];++gpu::current.passQuads[unsigned(group)];
#endif
            // Clipping a triangle to a rectangle produces at most seven vertices:
            // <= 30 emitted vertices/quad (720 bytes), plus state and draw commands.
            if(unsigned(sceGuCheckList())+4096>sizeof(commands))throw std::runtime_error("GU command capacity exceeded");
            if(q.blend==Blend::FadeBlack){
                sceGuDisable(GU_TEXTURE_2D);
                sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
                triangle(q,{{0,1,2}},color(q,false));triangle(q,{{0,2,3}},color(q,false));
#ifndef HKPSP_GPU_ATTRIB
                // Close the untextured run before any later quad: the opening blanker is
                // followed by the HUD (2026-11-18 "Unsubmitted GU vertex run" on frame 0),
                // unlike the room fades, which were always last.
                flushTriangles();
#endif
                sceGuEnable(GU_TEXTURE_2D);
                HKPSP_PROFILE(++perf::current[perf::BlendChanges];)
                continue;
            }
            if(q.blend==Blend::MaskAlpha){
                if(cpu::hudLiquid){
                    // Clear mode writes vertex alpha directly to stencil. Normal
                    // GU_REPLACE uses a constant reference, forcing one draw/span.
                    // Integer, screen-space rectangles have the same coverage as
                    // the old two triangles; colour/depth clear bits stay off.
                    unsigned end=index;
                    const auto limit=std::min(batchEnd,index+item.cpuEnd-item.cpu);
                    while(end<limit&&list.quads[item.cpu+end-index].blend==Blend::MaskAlpha&&end-index<16384)++end;
                    struct MaskVertex {unsigned color;short x,y,z;};
                    static_assert(sizeof(MaskVertex)==12,"GE mask vertex stride");
                    auto* vertices=static_cast<MaskVertex*>(guMemory((end-index)*2*sizeof(MaskVertex)));
                    for(unsigned i=index;i<end;++i){
                        const auto& mask=list.quads[item.cpu+i-index];const auto a=mask.positions[0],b=mask.positions[2];
                        if(a.x!=std::floor(a.x)||a.y!=std::floor(a.y)||b.x!=std::floor(b.x)||b.y!=std::floor(b.y)||
                           a.x<0||a.y<0||b.x>screenWidth||b.y>screenHeight||a.x>=b.x||a.y>=b.y||
                           mask.positions[1].x!=b.x||mask.positions[1].y!=a.y||
                           mask.positions[3].x!=a.x||mask.positions[3].y!=b.y)
                            throw std::runtime_error("Invalid batched alpha span");
                        const unsigned alpha=color(mask,false)&0xff000000;
                        vertices[(i-index)*2]={alpha,short(a.x),short(a.y),0};
                        vertices[(i-index)*2+1]={alpha,short(b.x),short(b.y),0};
                    }
                    // GE command CLEARMODE (0xd3), exactly the stencil-only mode
                    // used by sceGuClear, with our own per-rectangle colours.
                    stateCommand(sceGuSendCommandi,0xd3,1|(GU_STENCIL_BUFFER_BIT<<8));
                    HKPSP_DIAG_ONLY(diag::draw();)sceGuDrawArray(GU_SPRITES,GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D,
                        (end-index)*2,nullptr,vertices);
                    stateCommand(sceGuSendCommandi,0xd3,0);
                    HKPSP_PROFILE(++cpu::drawCalls;perf::current[perf::Triangles]+=(end-index)*2;)
#ifdef HKPSP_GPU_ATTRIB
                    perf::current[perf::Quads]+=end-index-1;gpu::current.passQuads[unsigned(group)]+=end-index-1;
#endif
                    index=end-1;
                    continue;
                }
                // GE alpha is stencil, never sampled fragment alpha. Filtered
                // constant-alpha spans preserve RGB, including at alpha zero.
                const auto& maskPage=pages.at(q.page);
                texImage(0,maskPage.width,maskPage.height,maskPage.width,maskPage.indices.get());
                stateCommand(sceGuDisable,GU_TEXTURE_2D);stateCommand(sceGuDisable,GU_BLEND);
                stateCommand(sceGuEnable,GU_STENCIL_TEST);stateCommand(sceGuStencilFunc,GU_ALWAYS,color(q,false)>>24,255);
                stateCommand(sceGuStencilOp,GU_KEEP,GU_KEEP,GU_REPLACE);stateCommand(sceGuPixelMask,0x00ffffff);
                triangle(q,{{0,1,2}},0xffffffff);triangle(q,{{0,2,3}},0xffffffff);
#ifndef HKPSP_GPU_ATTRIB
                flushTriangles();
#endif
                stateCommand(sceGuPixelMask,0xff000000);stateCommand(sceGuDisable,GU_STENCIL_TEST);
                stateCommand(sceGuEnable,GU_TEXTURE_2D);stateCommand(sceGuEnable,GU_BLEND);
                HKPSP_PROFILE(++perf::current[perf::TextureBinds];++perf::current[perf::BlendChanges];)
                continue;
            }
            if(cameraGrade(q.blend)){
                if(q.blend==Blend::GradeGreen&&!sameCameraGrade(list.cameraColor,paletteGrade))
                    updateGradePalettes(list.cameraColor);
#ifdef HKPSP_GPU_ATTRIB
                const auto mode=gpu::config().grade;
                void* snapshot=(mode==gpu::Grade::Vram||mode==gpu::Grade::Target)?gradeVram():gradeSource.get();
                if(mode==gpu::Grade::Stale&&frame==0)snapshot=staleNext.get();
                if(q.blend==Blend::GradeGreen){
                    if(mode==gpu::Grade::Target){stateCommand(sceGuDrawBufferList,GU_PSM_8888,drawOffset,stride);stateCommand(sceGuTexSync);}
                    else if(mode!=gpu::Grade::Stale){
                        auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+reinterpret_cast<std::uintptr_t>(drawOffset);
                        sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,source,0,0,stride,snapshot);stateCommand(sceGuTexSync);
                    }
                    gradeReady=true;
                }
                if(mode==gpu::Grade::CopyOnly||((mode==gpu::Grade::OneSweep||gpu::config().sweep==gpu::Sweep::Clut8)&&q.blend!=Blend::GradeGreen)){
                    --perf::current[perf::Quads];--gpu::current.passQuads[unsigned(gpu::Pass::Grade)];continue;
                }
#else
                if(q.blend==Blend::GradeGreen){
                    stateCommand(sceGuDrawBufferList,GU_PSM_8888,drawOffset,stride);
                    stateCommand(sceGuTexSync);gradeReady=true;
                }
#endif
                if(!gradeReady)throw std::runtime_error("Camera grade requires green first");
                const unsigned index=unsigned(q.blend)-unsigned(Blend::GradeGreen);
                const unsigned channel=q.blend==Blend::GradeGreen?1:q.blend==Blend::GradeRed?0:2;
#ifdef HKPSP_GPU_ATTRIB
                if(mode==gpu::Grade::Direct32)texMode(GU_PSM_8888,0,0,0);
                else {
#endif
                texMode(GU_PSM_T32,0,0,0);clutMode(GU_PSM_8888,channel*8,255,0);
                loadClut(32,gradePalettes.get()+index*1024);
#ifdef HKPSP_GPU_ATTRIB
                }
#endif
#ifdef HKPSP_GPU_ATTRIB
                texImage(0,512,512,stride,snapshot);texScale(1.f/512,1.f/512);
#else
                texImage(0,512,512,stride,gradeVram());texScale(1.f/512,1.f/512);
#endif
                texFilter(GU_NEAREST,GU_NEAREST);texFlush();
                blendFunc(GU_ADD,GU_FIX,GU_FIX,0xffffff,index?0xffffff:0);
                HKPSP_PROFILE(++perf::current[perf::TextureBinds];++perf::current[perf::BlendChanges];)
#ifdef HKPSP_GPU_ATTRIB
                const auto kind=gpu::config().sweep;
                if(kind==gpu::Sweep::Clut8){
                    texMode(GU_PSM_T8,0,0,1);clutMode(GU_PSM_8888,0,255,0);
                    loadClut(32,fillPalette.get());
                    texImage(0,512,512,512,fillIndices.get());texFlush();
                }
                if(kind==gpu::Sweep::Fill3)stateCommand(sceGuDisable,GU_TEXTURE_2D);
                if(kind==gpu::Sweep::NoBlend0&&index==0)stateCommand(sceGuDisable,GU_BLEND);
                sweep(q);
                if(kind==gpu::Sweep::NoBlend0&&index==0)stateCommand(sceGuEnable,GU_BLEND);
                if(kind==gpu::Sweep::Fill3)stateCommand(sceGuEnable,GU_TEXTURE_2D);
#else
                drawGradeStrips();
#endif
                texMode(GU_PSM_T8,0,0,1);clutMode(GU_PSM_8888,0,255,0);
#ifdef HKPSP_GPU_ATTRIB
                texFilter(gpu::config().nearest?GU_NEAREST:GU_LINEAR,gpu::config().nearest?GU_NEAREST:GU_LINEAR);
#else
                texFilter(GU_LINEAR,GU_LINEAR);
#endif
                continue;
            }
            const auto& page=pages.at(q.page);const bool screen=q.material==hk3ds::RoomMaterial::Screen;
            const bool premultiplyScreen=screen&&q.blend!=Blend::ScreenPremult;
            texMode(page.bits==4?GU_PSM_T4:GU_PSM_T8,0,0,1);
            clutMode(GU_PSM_8888,0,page.bits==4?15:255,0);
            const bool cutout=q.material==hk3ds::RoomMaterial::DarknessCutout;
            const bool linear=q.material==hk3ds::RoomMaterial::LinearLight;
            const bool foldedLight=linear&&q.blend==Blend::Multiply2;
            const bool ambient=q.material==hk3ds::RoomMaterial::AmbientAdditive;
#ifdef HKPSP_GPU_ATTRIB
            texImage(0,page.width,page.height,page.width,page.texels());
            if(page.vram)++gpu::current.vramBinds;
#else
            texImage(0,page.width,page.height,page.width,page.indices.get());
#endif
            texScale(1.f/page.width,1.f/page.height);
            if(linear&&!foldedLight){
                unsigned slot=0;
                for(;slot<linearKeys.size();++slot)if(linearKeys[slot].page==q.page&&linearKeys[slot].clut==q.clut&&linearKeys[slot].tint==q.rgba)break;
                if(slot==linearKeys.size()){
                    // Never overwrite a CLUT still referenced by this in-flight
                    // command list. Cross-frame replacement is safe after Sync.
                    if(usedLinear==0xffffu)throw std::runtime_error("Too many LinearLight palettes in one frame");
                    do {slot=nextLinear++%linearKeys.size();}while(usedLinear&(1u<<slot));
                    linearKeys[slot]={q.page,q.clut,q.rgba};
                    const auto palette=linearLightPalette(list.textures->at(q.page),q);
                    std::memcpy(linearPalettes.get()+slot*1024,palette.data(),1024);
                    sceKernelDcacheWritebackRange(linearPalettes.get()+slot*1024,1024);
                }
                usedLinear|=1u<<slot;
                loadClut(32,linearPalettes.get()+slot*1024);
                stateCommand(sceGuEnable,GU_ALPHA_TEST);stateCommand(sceGuAlphaFunc,GU_GREATER,0,255);
            }else {
                const auto& texture=list.textures->at(q.page);
                const unsigned variant=foldedLight?2:(premultiplyScreen||cutout)?1:0;
                const void* palette=page.palettes.get()+variant*1024;
                if(texture.cluts){
                    const auto& clut=texture.cluts->at(q.clut);
                    palette=variant==2?clut.light.data():variant==1?clut.premultiplied.data():clut.raw.data();
                }else if(q.clut)throw std::runtime_error("Legacy texture CLUT index");
                loadClut(page.bits==4?2:32,palette);
            }
            HKPSP_PROFILE(++perf::current[perf::TextureBinds];)
            // Host Normal: S*A+D*(1-A); GU ADD, SRC_ALPHA, ONE_MINUS_SRC_ALPHA.
            // Host Additive: D+S*A; GU ADD, SRC_ALPHA, FIX(1).
            // Host Screen: D+S*A*(1-D); premultiplied CLUT+vertex RGB supplies
            // S*A, then GU ADD, ONE_MINUS_DST_COLOR, FIX(1).
            // Cutout: premultiplied S*A + D*(1-S*A), FIX(1)/ONE_MINUS_SRC_COLOR.
            // LinearDodge: D+S*A, plan approximation; original clamps D+S
            // BEFORE alpha blending. LinearLight: neutral-alpha CLUT + 2x multiply;
            // original lerp(D,clamp(D+2*S-1),A) is not a final-blend clamp.
            // Host Multiply2: 2*S*D; alpha already folded into neutral .5 by
            // transcode_lighting.py. GU ADD, DST_COLOR, SRC_COLOR is identical.
            // Folded HeroLight's positive signed intercept is already alpha
            // weighted in its CLUT. One additive draw replaces the old multiply.
            if(q.blend==Blend::MaskedLiquid)blendFunc(GU_ADD,GU_DST_ALPHA,GU_ONE_MINUS_DST_ALPHA,0,0);
            else if(foldedLight)blendFunc(GU_ADD,GU_FIX,GU_FIX,0xffffff,0xffffff);
            else if(q.blend==Blend::Multiply2||linear)blendFunc(GU_ADD,GU_DST_COLOR,GU_SRC_COLOR,0,0);
            else if(cutout)blendFunc(GU_ADD,GU_FIX,GU_ONE_MINUS_SRC_COLOR,0xffffff,0);
            // GE doubles modulated RGB; DOUBLE_SRC_ALPHA doubles the modulated
            // alpha at the blend, with the same saturation as ambientSprite.
            else if(ambient)blendFunc(GU_ADD,GU_DOUBLE_SRC_ALPHA,GU_FIX,0,0xffffff);
            else blendFunc(GU_ADD,screen?GU_ONE_MINUS_DST_COLOR:GU_SRC_ALPHA,
                screen||q.material==hk3ds::RoomMaterial::Additive||q.material==hk3ds::RoomMaterial::LinearDodge?GU_FIX:GU_ONE_MINUS_SRC_ALPHA,0,0xffffff);
            HKPSP_PROFILE(++perf::current[perf::BlendChanges];)
            const auto tint=linear&&!foldedLight?0xffffffff:color(q,premultiplyScreen||cutout);
            // GE BLEND: Cs*(1-Ct)+Cc*Ct. Cs=tint*flash, Cc=tint
            // gives tint*(Ct*(1-flash)+flash), retaining texture alpha.
            if(q.whiteFlash>0){
                unsigned environment=0;
                for(unsigned c=0;c<3;++c)environment|=unsigned(std::clamp(std::lround(q.rgba[c]*255),0l,255l))<<(c*8);
                texEnvironment(environment);texFunc(GU_TFX_BLEND,GU_TCC_RGBA);
            }
            if(q.blend==Blend::ClampedAdd){
#ifdef HKPSP_GPU_ATTRIB
                if(clampedAdd(q,tint))gpu::current.passQuads[unsigned(group)]+=3;
#else
                clampedAdd(q,tint);
#endif
                continue;
            }
            if(ambient)stateCommand(sceGuEnable,GU_FRAGMENT_2X);
#ifndef HKPSP_GPU_ATTRIB
            if(item.world()){
                flushTriangles();previousState=nullptr;
                drawRoomGe(item.plan->buffers,*item.run,tint);
            }else if(roomEmission()){
                // Vertex colour may vary without changing GE state. LinearLight
                // palettes and flash environment colour also depend on the tint.
                const auto limit=std::min(item.cpuEnd,item.cpu+batchEnd-index);
                const auto run=emissionRun(list.quads,geometry,item.cpu,limit);
                // Preserve the existing logical per-quad counters. Actual GE
                // emission is measured by GuStateCommands and TextureFlushes.
                HKPSP_PROFILE(perf::current[perf::TextureBinds]+=run.end-item.cpu-1;
                    perf::current[perf::BlendChanges]+=run.end-item.cpu-1;)
                beginTriangles(run.vertices);
                for(unsigned i=item.cpu;i<run.end;++i){
                    const auto& sprite=list.quads[i];currentGeometry=&geometry[i];
                    HKPSP_PROFILE(emittingRoom=list.isRoomQuad(i);)
                    const auto packed=i==item.cpu?tint:linear?0xffffffff:color(sprite,premultiplyScreen||cutout);
                    triangle(sprite,{{0,1,2}},packed);triangle(sprite,{{0,2,3}},packed);
                }
                flushTriangles();index+=run.end-item.cpu-1;
            }else {
#endif
            triangle(q,{{0,1,2}},tint);triangle(q,{{0,2,3}},tint);
#ifndef HKPSP_GPU_ATTRIB
            if(cpu::config().c3&&simple(q))previousState=&q;
            else flushTriangles();
            }
#endif
            if(q.whiteFlash>0)texFunc(GU_TFX_MODULATE,GU_TCC_RGBA);
            if(ambient)stateCommand(sceGuDisable,GU_FRAGMENT_2X);
            if(linear)stateCommand(sceGuDisable,GU_ALPHA_TEST);
        }
#ifndef HKPSP_GPU_ATTRIB
        flushTriangles();
        if(worldProjection)setRoomGeProjection(roomExactProjection());
        // Command emission only, excluding uploads/list setup/finish and readback.
        HKPSP_PROFILE(perf::current[perf::GuListBytes]+=sceGuCheckList()-roomCommandsBefore;)
        HKPSP_PROFILE(cpu::change(-1);)
#endif
        if(roomTraceFile)roomTrace.append(list,submittedQuads,batchEnd);
        submittedQuads=batchEnd;
        if(batchEnd==submission.size())break;
        // All commands referencing these slots must finish before CPU reuse.
        // Preserve framebuffer contents, stencil, grade snapshot and draw order.
        checkPinnedPages();finishList();
        HKPSP_PROFILE(cpu::change(-1);)
        const auto waitStart=sceKernelGetSystemTimeLow();
        sceGuSync(0,0);
        const auto waitUs=sceKernelGetSystemTimeLow()-waitStart;
        if(!framePages.overflowFrames){
            framePages.overflowFrames=1;++pageTotals.overflowFrames;
        }
        ++framePages.geWaits;framePages.geWaitUs+=waitUs;
        ++pageTotals.geWaits;pageTotals.geWaitUs+=waitUs;
        HKPSP_PROFILE(perf::current[perf::PageOverflowFrames]=1;
            ++perf::current[perf::PageGeWaits];perf::current[perf::PageGeWaitUs]+=waitUs;
            perf::current[perf::Sync]+=waitUs;pageWaitUs+=waitUs;cpu::change(cpu::Upload);)
#ifdef HKPSP_GPU_ATTRIB
        gpu::current.ge+=waitUs;++gpu::current.segments;
        if(gpu::config().passSync)gpu::current.passUs[unsigned(activePass)]+=waitUs;
#endif
        activeClut=nullptr;activeClutBlocks=0;
        pinnedPages.clear();for(auto& p:pages)p.required=false;
        usedLinear=0;
        sceGuStart(GU_DIRECT,commands);resetState();
        // sceGuStart emits the SDK draw pointer: restore the active scene/grade target.
#ifdef HKPSP_GPU_ATTRIB
        const bool sceneTarget=gpu::config().grade==gpu::Grade::Target&&!gradeReady;
#else
        const bool sceneTarget=!gradeReady;
#endif
        stateCommand(sceGuDrawBufferList,GU_PSM_8888,sceneTarget?reinterpret_cast<void*>(2*bufferBytes):drawOffset,stride);
        } while(true);
        HKPSP_PROFILE(cpu::change(-1);
            guUs+=perf::now()-started-(perf::current[perf::Clipping]-clipBefore)-pageWaitUs;)
#ifdef HKPSP_GPU_ATTRIB
        guUs-=gpu::current.ge-geBefore-pageWaitUs;
#endif
    }
    bool presentMonomonMovie()override{
        constexpr unsigned width=200,height=112,pitch=208,textureHeight=128,frameCount=348;
        constexpr unsigned textureBytes=pitch*textureHeight*sizeof(std::uint16_t);
        if(!monomonStream){
            constexpr unsigned char expected[]={ 'H','K','M','1',200,0,0,0,112,0,0,0,92,1,0,0,30,0,0,0 };
            unsigned char header[sizeof expected];
            monomonStream.reset(std::fopen(path("rooms/Dream_Guardian_Monomon/mask-shatter.bin").c_str(),"rb"));
            if(!monomonStream||std::fread(header,1,sizeof header,monomonStream.get())!=sizeof header||
               std::memcmp(header,expected,sizeof header))throw std::runtime_error("Invalid Monomon film header");
            monomonPixels=aligned(textureBytes);
            std::memset(monomonPixels.get(),0,textureBytes);
            monomonFrame=0;
        }
        auto* pixels=reinterpret_cast<std::uint16_t*>(monomonPixels.get());
        if(std::fread(pixels,sizeof *pixels,width*height,monomonStream.get())!=width*height)
            throw std::runtime_error("Truncated Monomon film frame");
        for(unsigned row=height;row-- > 0;){
            for(unsigned col=width;col-- > 0;){
                const auto rgb=pixels[row*width+col];
                pixels[row*pitch+col]=std::uint16_t((rgb&0x07e0)|((rgb&0xf800)>>11)|((rgb&0x001f)<<11));
            }
        }
        sceKernelDcacheWritebackRange(pixels,textureBytes);
        stateCommand(sceGuDrawBufferList,GU_PSM_8888,drawOffset,stride);
        gradeReady=true;
        stateCommand(sceGuClearColor,0xff000000);sceGuClear(GU_COLOR_BUFFER_BIT);
        stateCommand(sceGuEnable,GU_TEXTURE_2D);
        stateCommand(sceGuDisable,GU_BLEND);
        texMode(GU_PSM_5650,0,0,0);texImage(0,256,textureHeight,pitch,pixels);texScale(1,1);
        texFilter(GU_NEAREST,GU_NEAREST);texFlush();texFunc(GU_TFX_REPLACE,GU_TCC_RGB);
        constexpr float top=(screenHeight-screenWidth*float(height)/width)/2;
        auto* vertices=static_cast<Vertex*>(guMemory(2*sizeof(Vertex)));
        vertices[0]={0,0,0xffffffff,0,top,0};
        vertices[1]={width,height,0xffffffff,screenWidth,screenHeight-top,0};
        sceGuDrawArray(GU_SPRITES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D,
                       2,nullptr,vertices);
        stateCommand(sceGuEnable,GU_BLEND);
#ifdef HKPSP_GPU_ATTRIB
        texFilter(gpu::config().nearest?GU_NEAREST:GU_LINEAR,gpu::config().nearest?GU_NEAREST:GU_LINEAR);
#else
        texFilter(GU_LINEAR,GU_LINEAR);
#endif
        texFunc(GU_TFX_MODULATE,GU_TCC_RGBA);
        if(++monomonFrame!=frameCount)return false;
        monomonStream.reset();
        return true;
    }
    void endFrame()override{
        // Sample once after all passes. Perf already samples at the flip below.
#ifndef HKPSP_PERF
        if(frames)measureHeap();
#endif
        HKPSP_PROFILE(const auto started=perf::now();)
        // A list without camera grade still renders into the scene target.
        // Publish it to the display/readback buffer, as the green grade pass does.
#ifdef HKPSP_GPU_ATTRIB
        if(!gradeReady&&gpu::config().grade==gpu::Grade::Target){
#else
        if(!gradeReady){
#endif
            auto* base=static_cast<unsigned char*>(sceGeEdramGetAddr());
            sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,base+2*bufferBytes,
                           0,0,stride,base+reinterpret_cast<std::uintptr_t>(drawOffset));
            sceGuTexSync();
        }
        const auto& i=frameInfo;
        if(metrics&&std::fprintf(metrics,"%u,%.9g,%.9g,%u,%u,%.9g,%.9g,%u,%.9g,%.9g,%u,%u,%u,%u,%u,%s,%.9g,%.9g,%u,%.9g\n",i.frame,double(i.heroX),double(i.heroY),
            submittedQuads,submittedQuads,double(i.cameraX),double(i.cameraY),i.controlSteps,
            double(i.velocityX),double(i.velocityY),i.actor,i.phase,i.health,i.soul,i.geo,i.scene,double(i.simulationX),double(i.simulationY),
            i.abilityPhase,double(i.transitionFade))<0)
            throw std::runtime_error("Metrics write failed");
        const bool save=frames&&(frame%every==0||frame+1==frames);
        HKPSP_DIAG_ONLY(diag::readback(drawOffset);)
        if(save){
            // A GE transfer to RAM is also an explicit GPU readback in PPSSPP.
            // Reading VRAM directly can observe its stale CPU shadow on Vulkan.
            sceKernelDcacheWritebackInvalidateRange(readback.get(),bufferBytes);
            auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+
                         reinterpret_cast<std::uintptr_t>(drawOffset);
            sceGuCopyImage(GU_PSM_8888,0,0,screenWidth,screenHeight,stride,source,0,0,stride,readback.get());
        }
#ifdef HKPSP_GPU_ATTRIB
        guUs+=perf::now()-started;
        finishSegment();
        if(staleNext)std::swap(gradeSource,staleNext);
        perf::current[perf::Sync]=gpu::current.ge;
        perf::current[perf::Gu]=guUs;
        gpu::records.at(gpu::configIndex).at(frame)=gpu::current;
#else
        HKPSP_PROFILE(cpu::change(cpu::Finish);)
#ifdef HKPSP_PERF
        cpuStamp[3]=perf::runClock(perf::gameThread);
        sceGuSignal(GU_SIGNAL_WAIT,3);
#endif
        finishList();
        HKPSP_PROFILE(cpu::change(-1);)
        HKPSP_PROFILE(perf::current[perf::Gu]=guUs+perf::now()-started;
            const auto syncStart=perf::now();)
        sceGuSync(0,0);
        HKPSP_PROFILE(perf::current[perf::Sync]+=perf::now()-syncStart;)
        HKPSP_PROFILE(if(geSeen!=15)throw std::runtime_error("Missing GE timeline marker");
            perf::current[perf::GeScene]=geStamp[1]-geStamp[0];
            perf::current[perf::GeGrade]=geStamp[2]-geStamp[1];
            perf::current[perf::GeHud]=geStamp[3]-geStamp[2];
            for(unsigned part=0;part<3;++part)perf::current[perf::CpuScene+part]=cpuStamp[part+1]-cpuStamp[part];)
#endif
        if(save)capture();
        HKPSP_DIAG_ONLY(diag::finishWork();)
        HKPSP_PROFILE(const auto vblankStart=perf::now();)
        // Always flip on a new vblank. Work (including the previous frame's
        // simulation) consumes this interval; an overrun adds only the next one.
        do {sceDisplayWaitVblankStart();}
        while(beforeFlip(lastFlipVcount,static_cast<unsigned>(sceDisplayGetVcount())));
        HKPSP_PROFILE(perf::current[perf::Vblank]=perf::now()-vblankStart;const auto swapStart=perf::now();)
#ifdef HKPSP_GPU_ATTRIB
        capturedOffset=drawOffset;
#elif defined(HKPSP_PERF)
        cpuCapturedOffset=drawOffset;
#endif
        drawOffset=sceGuSwapBuffers();
        HKPSP_DIAG_ONLY(diag::displayed(frameInfo,framePages,submittedQuads);)
        HKPSP_PROFILE(const auto precedingFlip=lastFlipVcount;)
        lastFlipVcount=static_cast<unsigned>(sceDisplayGetVcount());
        HKPSP_PROFILE(perf::current[perf::Swap]=perf::now()-swapStart;const auto bookkeepingStart=perf::now();)
        HKPSP_PROFILE(perf::current[perf::VblankDelta]=lastFlipVcount-precedingFlip;
            const auto heap=mallinfo();perf::current[perf::ArenaBytes]=unsigned(heap.arena);
            perf::current[perf::GameHeapBytes]=unsigned(heap.uordblks);
            perf::current[perf::RoomGeMode]=unsigned(roomGeEnabled());
            perf::current[perf::RoomSceneBytes]=unsigned(DrawList::geScene?DrawList::geScene->bytes():0);
            gamePeakHeap=std::max(gamePeakHeap,unsigned(heap.uordblks));
            peakHeap=std::max(peakHeap,unsigned(heap.uordblks));peakArena=std::max(peakArena,unsigned(heap.arena));
            perf::current[perf::LargestFree]=sceKernelMaxFreeMemSize();)
        HKPSP_PROFILE(perf::current[perf::Bookkeeping]+=perf::now()-bookkeepingStart;)
        if(pageMetrics)framePages.writeCsv(pageMetrics,frameInfo.frame);
        if(roomTraceFile&&std::fprintf(roomTraceFile,"%u,%u,%016llx,%u,%u,%u,%u\n",
            frameInfo.frame,roomTrace.draws,static_cast<unsigned long long>(roomTrace.hash),
            roomTrace.draws-roomTrace.world,roomTrace.world,
            unsigned(DrawList::geScene?DrawList::geScene->bytes():0),unsigned(roomGeEnabled()))<0)
            throw std::runtime_error("Room parity trace write failed");
        if(frames&&frame+1==frames){
            writeMemory();
            FILE* closing=metrics;metrics=nullptr;if(std::fclose(closing))throw std::runtime_error("Metrics close failed");
            closing=pageMetrics;pageMetrics=nullptr;if(std::fclose(closing))throw std::runtime_error("Page counters close failed");
            if(roomTraceFile){closing=roomTraceFile;roomTraceFile=nullptr;
                if(std::fclose(closing))throw std::runtime_error("Room parity trace close failed");}
        }
    }
};
}
std::unique_ptr<Platform> makePsp(unsigned frames,unsigned every,bool menus){
    // DIAG owns its readback partition; keep ordinary test captures off its game heap.
    HKPSP_DIAG_ONLY(frames=0;)
    return std::make_unique<Psp>(frames,every,menus);
}
std::unique_ptr<Platform> makePsp(unsigned frames,unsigned every){
    return makePsp(frames,every,false);
}
#ifdef HKPSP_GPU_ATTRIB
void captureGpu(Platform& platform){static_cast<Psp&>(platform).captureGpu();}
#elif defined(HKPSP_PERF)
void resetCpu(Platform& platform){static_cast<Psp&>(memory_report::unwrap(platform)).resetCpu();}
void captureCpu(Platform& platform){static_cast<Psp&>(memory_report::unwrap(platform)).captureCpu();}
#endif
}
#endif
