#pragma once
#include "draw_list.hpp"
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace hkpsp {
// Geometry admission is separate from the exact renderer and the GU adapter.
enum class RoomGeReason : unsigned { Eligible, Moving, Animated, Ambient, NearDepth, GuardBand, SplitClip, Material, Count };
inline constexpr const char* roomGeReasonNames[]={"eligible","moving","animated","ambient","near_depth","guard_band","split_clip","material"};
struct RoomGeVertex {float u,v;std::uint32_t color;float x,y,z;};
static_assert(sizeof(RoomGeVertex)==24,"GE vertex estimate must match GU layout");
struct RoomGeCounts {
    std::array<unsigned,static_cast<unsigned>(RoomGeReason::Count)> pairs{};
    unsigned flatPairs=0;
    void add(RoomGeReason reason,const RoomQuadCache* cache=nullptr);
    unsigned eligible()const{return pairs[static_cast<unsigned>(RoomGeReason::Eligible)];}
    // Payload estimates exclude run bounds/state, alignment and allocator overhead.
    std::size_t indexedBytes()const{return eligible()*(4*sizeof(RoomGeVertex)+6*sizeof(std::uint16_t));}
    std::size_t triangleBytes()const{return eligible()*6*sizeof(RoomGeVertex);}
    std::size_t spriteBytes()const{return flatPairs*2*sizeof(RoomGeVertex);}
};
// bounds is the complete camera-admission rectangle for this immutable draw,
// including offscreen slack. Affine projection extrema occur at its corners.
RoomGeReason classifyRoomGe(const RoomQuadCache&,const hk3ds::RoomCameraBounds&,float focal);
// Geometry eligibility is immutable; owner opacity is deliberately a frame gate.
inline bool roomGeOwnerOpaque(float alpha){return alpha==1.f;}

// Runs have one tint, supplied by sceGuColor. Keeping colour out of the vertex
// also permits the original material's repeated passes without rewriting RAM.
struct RoomGePosition {float u,v,x,y,z;};
static_assert(sizeof(RoomGePosition)==20,"GE world vertex stride");
inline constexpr unsigned roomGeMaxQuads=64;
inline constexpr unsigned roomGeCpuBit=0x80000000u;
struct RoomGeSource {unsigned draw;hk3ds::RoomCameraBounds camera;bool exact=false;};
struct RoomGeRun {
    // Exact CPU-visible camera interval, intersected with source admission.
    // All members have this same interval; an empty interval never draws.
    // Membership alone does not prove a multi-quad run is atomic for ordering.
    hk3ds::RoomCameraBounds camera;
    unsigned firstVertex=0,firstDraw=0,count=0;
};
static_assert(sizeof(RoomGeRun)==28,"Budget run metadata explicitly");
// Entries are authored-order barriers, referring to either a run or an exact
// cache. A run never crosses a source gap, owner, sort key or batch state.
struct RoomGeEntry {unsigned draw,geometry;};
struct RoomGeBuffers {
    std::vector<RoomGePosition> vertices;
    std::vector<RoomGeRun> runs;
    std::vector<RoomQuadCache> cpu;
    std::vector<RoomGeEntry> ordered;
    RoomGeCounts counts;
    std::size_t bytes()const;
};
// Only immutable quad/pass pairs belong in sources. Mutable records are gaps
// between their source indices and remain on the existing CPU path.
RoomGeBuffers buildRoomGeBuffers(const hk3ds::RoomVisualBank&,
    const std::vector<RoomGeSource>&,bool background,float focal,unsigned maxRunQuads=roomGeMaxQuads);
// Load-time inversion of projectRoomQuad's original float operations. The
// result includes touching edges and excludes the immediately adjacent float.
// Requires GE-eligible geometry and a finite source-admission rectangle.
hk3ds::RoomCameraBounds roomGeVisibleCamera(const RoomQuadCache&,
    const hk3ds::RoomCameraBounds& source,float focal);
// The existing CLUT rule scans the last 32 *visible* quads. Its pair decisions
// can be compiled, but its insertion cannot be applied to the unculled list.
// Exact means the compiler could not prove a camera-independent decision;
// callers must keep the affected group on the original CPU rule.
enum class RoomGeRelation : std::uint8_t { Stop, Skip, Match, Exact };
struct RoomGeOrderView {
    unsigned count=0;
    const std::uint8_t* pairs=nullptr;
    std::size_t bytes=0;
    RoomGeRelation relation(unsigned a,unsigned b)const;
};
struct RoomGeOrder {
    unsigned count=0;
    std::vector<std::uint8_t> pairs; // symmetric lower triangle, two bits/pair
    bool needsExact=false;
    RoomGeRelation relation(unsigned a,unsigned b)const;
    RoomGeOrderView view()const{return {count,pairs.data(),pairs.size()};}
};
// All inputs belong to one layer/depth group, with immutable state/geometry.
// A mutable/ambient insertion, a nonunit owner alpha or a repeated noncontiguous
// key requires the original rule for the affected emitted group. Do not fence
// groups by an intervening source record that can disappear through culling.
// camera encloses every camera at which any member can be submitted.
RoomGeOrder buildRoomGeOrder(const std::vector<const Quad*>&,
    const hk3ds::RoomCameraBounds& camera,float focal);
// ids contains visible group-local IDs, with the newly emitted ID at its tail.
// first is the start of the current emitted layer/depth group. Returns false
// without changing ids when this insertion needs the original projected rule.
// Call only when the original cpu::clutReorder switch is enabled.
bool reorderRoomGeTail(std::vector<unsigned>& ids,std::size_t first,const RoomGeOrder&);
// The same insertion rule serves standalone IDs and compact mixed commands.
template<class T,class Id> bool reorderRoomGeTail(std::vector<T>& ids,std::size_t first,
                                               RoomGeOrderView order,Id id){
    if(ids.empty()||first>=ids.size()-1)return true;
    const auto last=ids.size()-1,begin=std::max(first,last>32?last-32:std::size_t(0));
    for(auto j=last;j>begin;){
        const auto relation=order.relation(id(ids[last]),id(ids[--j]));
        if(relation==RoomGeRelation::Exact)return false;
        if(relation==RoomGeRelation::Stop)return true;
        if(relation==RoomGeRelation::Match){
            std::rotate(ids.begin()+j+1,ids.end()-1,ids.end());return true;
        }
    }
    return true;
}
// Retain descriptors only for groups that can actually move a visible record.
// first/count address singleton entries in buffers.ordered; pairs is a byte
// offset into the shared pool. No per-group allocation survives room load.
struct RoomGeGroup {unsigned first,count,pairs;};
static_assert(sizeof(RoomGeGroup)==12,"Budget pooled order descriptors");
struct RoomGePlan {
    RoomGeBuffers buffers;
    std::vector<RoomGeGroup> groups;
    std::vector<std::uint8_t> pairs;
    bool behindHero=false,background=false;
    float focal=0;
    std::size_t bytes()const;
    unsigned entry(unsigned draw)const; // ~0u for mutable/nonmember records
    const RoomGeGroup* group(unsigned entry)const;
    RoomGeOrderView order(const RoomGeGroup&)const;
};
// Unsafe geometry, uncertain/repeated keys and mutable peers keep the whole
// layer/depth key on the CPU rule. Singleton runs preserve the 32-quad window;
// batching several quads requires a separate order-atomicity proof.
RoomGePlan buildRoomGePlan(const hk3ds::RoomVisualBank&,
    std::vector<RoomGeSource>,bool behindHero,bool background,float focal);
struct RoomGeCommand {
    // GE: plan entry index. CPU: high bit plus frame.cpu.quads index. CPU slots
    // remain sequential while reorderRoomTail moves their Quad payloads.
    unsigned source=0,order=0;
};
static_assert(sizeof(RoomGeCommand)==8,"Mixed commands contain no vertex payload");
struct RoomGeFrame {
    DrawList cpu;
    std::vector<RoomGeCommand> commands;
    // Reused scratch, never projected geometry for the GE path.
    std::vector<unsigned> entries;
    std::vector<std::array<float,2>> exactKeys;
    unsigned cpuProjected=0;
};
// CPU-only gaps between these ordered spans contain actors/lights/HUD. A span's
// commands address the cumulative CPU list or its immutable plan, never copies
// of GE geometry. The scene and plans outlive every partial platform submission.
struct RoomGeSpan {
    const RoomGePlan* plan=nullptr;
    unsigned firstCpu=0,lastCpu=0,firstCommand=0,lastCommand=0;
};
struct RoomGeScene {
    RoomGeScene()=default;
    RoomGeScene(const RoomGeScene&)=delete;
    RoomGeScene& operator=(const RoomGeScene&)=delete;
    ~RoomGeScene();
    const hk3ds::RoomVisualBank* room=nullptr;
    std::vector<RoomGeSpan> spans;
    std::vector<RoomGeCommand> commands;
    RoomGeFrame scratch;
    void clear(DrawList&,const hk3ds::RoomVisualBank&);
    void append(DrawList&,const RoomGePlan&);
    std::size_t bytes()const;
};
// A borrowed, allocation-free view of the complete submission order, including
// CPU-only gaps. Indices are logical draws, not offsets into the CPU Quad array.
struct RoomGeItem {
    unsigned cpu=~0u,cpuEnd=0,order=0;
    const RoomGePlan* plan=nullptr;
    const RoomGeRun* run=nullptr;
    const hk3ds::RoomDraw* draw=nullptr;
    bool world()const{return plan!=nullptr;}
};
class RoomGeSubmission {
    const DrawList& list;
    unsigned count=0;
public:
    explicit RoomGeSubmission(const DrawList&);
    unsigned size()const{return count;}
    RoomGeItem at(unsigned)const;
    // ~0u means the camera-grade draw reads no texture-bank page.
    unsigned page(unsigned)const;
    // State only: never projects corners or writes world vertex payloads.
    Quad state(const RoomGeItem&)const;
};
// Host oracle only: reconstruct complete Quad bytes at the consumption boundary.
// PSP submission must consume the scene's references directly.
#ifndef __PSP__
void expandRoomGe(const DrawList& source,DrawList& destination);
#endif
// selected is the RoomIndex candidate stream. The alpha table is dense, just
// as in appendRoom. Mutable/ambient insertions and nonunit alpha preflight the
// entire key, so an emitted group always uses one ordering rule. Group fences
// advance only when a visible record is emitted. firstOrder includes the hero
// and other already submitted draws. This replaces, rather than appends to, out.
void appendRoomGe(RoomGeFrame& out,const RoomGePlan&,const hk3ds::RoomVisualBank&,
    hk3ds::Vec2 camera,float seconds,const std::vector<RoomDrawRef>& selected,
    const RoomSelection&,const hk3ds::RoomSortBoundary* split=nullptr,bool afterSplit=false,
    const hk3ds::RoomVisualBank* particles=nullptr,unsigned particleSprite=0,unsigned firstOrder=0);
const std::array<std::uint16_t,roomGeMaxQuads*6>& roomGeIndices();
// Column-major GU projection. World z is the original, unclamped depth, not a
// reciprocal round trip. Model and view remain identity.
using RoomGeMatrix=std::array<float,16>;
RoomGeMatrix roomGeProjection(hk3ds::Vec2 camera,float focal);
RoomGeMatrix roomExactProjection();
// PSP adapter: flush immutable payloads once after load. The caller retains the
// buffers until GU completion, selects page/CLUT/blend, and gates owner opacity.
// Model/view must be identity. Restore roomExactProjection before CPU geometry.
void prepareRoomGe(const RoomGeBuffers&);
void setRoomGeProjection(const RoomGeMatrix&);
void drawRoomGe(const RoomGeBuffers&,const RoomGeRun&,std::uint32_t tint);
}
