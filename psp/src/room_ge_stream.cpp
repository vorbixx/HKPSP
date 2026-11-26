#include "room_ge.hpp"
#include "perf.hpp"

namespace hkpsp {
namespace {
std::array<float,2> key(const hk3ds::RoomDraw& d){return {d.values[11],d.values[6]};}
}
void appendRoomGe(RoomGeFrame& out,const RoomGePlan& plan,const hk3ds::RoomVisualBank& room,
    hk3ds::Vec2 camera,float seconds,const std::vector<RoomDrawRef>& selected,const RoomSelection& selection,
    const hk3ds::RoomSortBoundary* split,bool afterSplit,const hk3ds::RoomVisualBank* particles,
    unsigned particleSprite,unsigned firstOrder){
    if(!selection.alpha||selection.first>selection.last||selection.last>selected.size())
        throw std::invalid_argument("Invalid GE room selection");
    out.commands.clear();out.cpu.quads.clear();out.exactKeys.clear();out.cpuProjected=0;
    out.entries.resize(selection.last-selection.first);
    for(std::size_t i=selection.first;i<selection.last;++i){
        HKPSP_PROFILE(++perf::current[perf::RoomGeStreamVisits];)
        const auto& ref=selected[i];const auto e=ref.geEntry==~0u?plan.entry(ref.index):ref.geEntry;
        if(e!=~0u&&(e>=plan.buffers.ordered.size()||plan.buffers.ordered[e].draw!=ref.index))
            throw std::invalid_argument("Invalid prepared GE entry");
        out.entries[i-selection.first]=e;
        if(e==~0u||!roomGeOwnerOpaque(selection.alpha->at(ref.owner))||
           (plan.buffers.ordered[e].geometry&roomGeCpuBit))out.exactKeys.push_back(key(room.draws.at(ref.index)));
    }
    // Include even currently transparent/offscreen particles. They may have the
    // same key as immutable room geometry, and source fences are not emitted fences.
    if(particles)for(const auto& d:particles->draws)out.exactKeys.push_back(key(d));
    std::sort(out.exactKeys.begin(),out.exactKeys.end());
    out.exactKeys.erase(std::unique(out.exactKeys.begin(),out.exactKeys.end()),out.exactKeys.end());
    std::size_t ri=selection.first,pi=0,groupFirst=0,cpuFirst=0;
    std::array<float,2> groupKey{};bool haveGroup=false,cpuGroup=false;
    const auto emit=[&](const hk3ds::RoomDraw& d,unsigned entry,Quad* q){
        HKPSP_PROFILE(++perf::current[perf::RoomQuads];)
        const auto k=key(d);const bool exact=q!=nullptr;
        if(!haveGroup||k!=groupKey){
            groupKey=k;groupFirst=out.commands.size();cpuFirst=out.cpu.quads.size();cpuGroup=exact;haveGroup=true;
        }else if(cpuGroup!=exact)throw std::logic_error("GE emitted group mixed ordering rules");
        const auto order=firstOrder+static_cast<unsigned>(out.commands.size());
        if(exact){
            q->order=order;
            out.commands.push_back({roomGeCpuBit|static_cast<unsigned>(out.cpu.quads.size()),order});
            out.cpu.quads.push_back(*q);reorderRoomTail(out.cpu,cpuFirst);
        }else{
            out.commands.push_back({entry,order});
            if(cpu::clutReorder)if(const auto* group=plan.group(entry)){
                if(!reorderRoomGeTail(out.commands,groupFirst,plan.order(*group),[&](const auto& c){return c.source-group->first;}))
                    throw std::logic_error("Uncompiled GE runtime pair");
            }
        }
    };
    const auto particleCount=particles?particles->draws.size():0;
    while(ri<selection.last||pi<particleCount){
        HKPSP_PROFILE(++perf::current[perf::RoomGeStreamVisits];)
        bool particle=ri==selection.last;
        if(!particle&&pi<particleCount){const auto& v=particles->draws[pi].values;
            particle=!hk3ds::roomDrawBefore(room.draws[selected[ri].index],v[11],v[12],v[6]);}
        const auto ref=particle?RoomDrawRef{}:selected[ri];
        const auto entry=particle?~0u:out.entries[ri-selection.first];
        const auto& d=particle?particles->draws[pi++]:room.draws[ref.index];
        if(!particle)++ri;
        HKPSP_PROFILE(++perf::current[perf::Draws];++perf::current[perf::RoomTests];)
        if(entry==~0u&&hk3ds::roomDrawBehindHero(d)!=plan.behindHero){HKPSP_PROFILE(++perf::current[perf::RejectPass];)continue;}
        if(split&&(!selection.splitSelected||particle)&&
           hk3ds::roomDrawBefore(d,split->layer,split->order,split->depth)==afterSplit){HKPSP_PROFILE(++perf::current[perf::RejectSplit];)continue;}
        const float alpha=particle?1:selection.alpha->at(ref.owner);if(alpha<=0){HKPSP_PROFILE(++perf::current[perf::RejectAlpha];)continue;}
        HKPSP_PROFILE(++perf::current[perf::RoomGeKeySearches];)
        const bool exact=std::binary_search(out.exactKeys.begin(),out.exactKeys.end(),key(d));
        if(entry!=~0u&&!exact){
            const auto& e=plan.buffers.ordered[entry];
            if(e.geometry&roomGeCpuBit)throw std::logic_error("CPU entry escaped its exact key");
            const auto& run=plan.buffers.runs.at(e.geometry);
            if(run.count!=1)throw std::logic_error("Unproved multi-quad GE ordering");
            if(run.camera.contains(camera.x,camera.y))emit(d,entry,nullptr);
            else {HKPSP_PROFILE(++perf::current[perf::RejectFrustum];)}
            continue;
        }
        Quad q;
        if(entry!=~0u){
            const auto& e=plan.buffers.ordered[entry];RoomQuadCache temporary;
            const RoomQuadCache* cached=nullptr;
            if(e.geometry&roomGeCpuBit)cached=&plan.buffers.cpu.at(e.geometry&~roomGeCpuBit);
            else{
                if(!cacheRoomQuad(room.atlas.sprites.at(d.sprite),d,plan.background,room.blurSplitDepth,temporary))
                    throw std::logic_error("Immutable GE fallback changed pass");
                cached=&temporary;
            }
            ++out.cpuProjected;if(!projectRoomQuad(*cached,camera,plan.focal,q))continue;
        }else{
            const auto& sprite=room.atlas.sprites.at(d.sprite+(particle?particleSprite:0));
            hk3ds::RoomSpriteClip clip{sprite};
            if(!pspRoomClip(sprite,d,plan.background,clip,room.blurSplitDepth)){HKPSP_PROFILE(++perf::current[perf::RejectClip];)continue;}
            if(clip.homogeneousClip)throw std::runtime_error("Diagonal split-plane clipping is unsupported");
            ++out.cpuProjected;if(!roomQuad(clip.sprite,d,camera,seconds,q))continue;
            q.u0=sprite.x+clip.left*sprite.width;q.u1=sprite.x+clip.right*sprite.width;
            q.v0=sprite.y+clip.top*sprite.height;q.v1=sprite.y+clip.bottom*sprite.height;
        }
        q.rgba[3]*=alpha;emit(d,entry,&q);
    }
}
}
