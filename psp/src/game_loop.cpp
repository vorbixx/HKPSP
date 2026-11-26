#include "../include/platform.hpp"
#include "draw_list.hpp"
#include "hud_draw.hpp"
#include "text_draw.hpp"
#include "text_pack_io.hpp"
#include "credits_draw.hpp"
#include "ability_draw.hpp"
#include "../include/inventory_draw.hpp"
#include "map_draw.hpp"
#include "actors.hpp"
#include "audio_scene.hpp"
#include "tutorial.hpp"
#include "tutorial_save.hpp"
#include "input_replay.hpp"
#include "tutorial_replay.hpp"
#include "perf.hpp"
#include "room_ge.hpp"
#include "../include/memory_report.hpp"
#include "world_town_lift.hpp"
#include "world_switch_room.hpp"
#include "world_obstacles.hpp"
#include "room_lighting.hpp"
#include "room_fade.hpp"
#include "monomon_mask_data.hpp"
#include "world_actor_art.hpp"
#include "cornifer_art.hpp"
#include "world_quest_art.hpp"
#include "route_toll_art.hpp"
#include "shop_draw.hpp"
#include "boss_art.hpp"
#include "psp_uumuu_art.hpp"
#include "psp_monomon_art.hpp"
#include "diag.hpp"
#include "performance_sections.hpp"
#include "simulation_state_hash.hpp"
#include "menu_runtime.hpp"
#include <cstring>
#include <unordered_map>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>

namespace hkpsp {
namespace {
template<class T> void load(T& bank,Platform& platform,const std::string& name){
    const auto path=platform.path(name);
    if(!bank.load(path.c_str()))throw std::runtime_error("Failed asset: "+path);
}
float randomUnit(void* context){
    auto& seed=*static_cast<std::uint32_t*>(context);
    seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
    return float(seed>>8)/16777215.f;
}
float visibility(unsigned owner,void* context){
    const auto alpha=static_cast<hk3ds::TutorialSession*>(context)->visualAlpha(owner);
    HKPSP_PROFILE(++perf::current[perf::OwnerChecks];if(alpha<=0)++perf::current[perf::OwnerRejects];)
    return alpha;
}
template<class T> void releaseRoomBinding(T& controller){
    // These controllers borrow draw indices and bank storage. Reconstruct in
    // place so the PSP stack never holds a second RoomAmbient particle pool.
    controller.~T();new(&controller) T();
}
// The bounds describe CAMERA positions, including depth/parallax and grass sway,
// rather than a screen-space or hero-plane grid. Nothing here projects vertices.
class RoomIndex {
    struct Node {
        hk3ds::RoomCameraBounds bounds;
        unsigned first=0,count=0,left=0,right=0;
    };
    struct Pass {
        struct Entry {unsigned index,cache;};
        std::vector<Entry> ordered;
        std::vector<RoomQuadCache> cached;
        RoomGePlan ge;
        std::vector<unsigned> spatial;
        std::vector<Node> nodes;
        std::vector<RoomDrawRef> candidates;
        std::vector<std::uint32_t> visible;
        bool orderedKeys=true;
    };
    std::array<Pass,4> passes;
    std::vector<hk3ds::RoomCameraBounds> bounds;
    std::vector<unsigned> moving;
    std::vector<std::pair<unsigned,float>> mantisGates;
    RoomGeCounts geCounts;
    bool worldGeometry=false;
    RoomGeScene geScene;
    const RoomQuadCache* cached(const Pass& pass,unsigned entry)const{
        if(entry==~0u)return nullptr;
        if(!worldGeometry)return &pass.cached[entry];
        const auto geometry=pass.ge.buffers.ordered.at(entry).geometry;
        return geometry&roomGeCpuBit?&pass.ge.buffers.cpu.at(geometry&~roomGeCpuBit):nullptr;
    }
    // The sparse table exists only for the legacy A/B. Dense IDs are assigned
    // once at room load, including high unsigned synthetic owner IDs.
    std::unordered_map<unsigned,float> ownerAlpha;
    std::vector<unsigned> drawOwners,owners,ownerOrder;
    std::vector<float> denseAlpha;
    std::vector<hk3ds::VisualAlphaInput> alphaInputs,previousAlphaInputs;
    void invalidateAlpha(unsigned owner){
        const auto found=std::lower_bound(ownerOrder.begin(),ownerOrder.end(),owner,
            [&](unsigned id,unsigned raw){return owners[id]<raw;});
        if(found!=ownerOrder.end()&&owners[*found]==owner)denseAlpha[*found]=-1.f;
    }
    void updateAlphaInputs(const hk3ds::TutorialSession& game){
        game.captureVisualAlphaInputs(owners,alphaInputs);
        const auto less=[](const auto& a,const auto& b){
            return a.owner<b.owner||(a.owner==b.owner&&a.source<b.source);
        };
        std::sort(alphaInputs.begin(),alphaInputs.end(),less);
        alphaInputs.erase(std::unique(alphaInputs.begin(),alphaInputs.end(),[](const auto& a,const auto& b){
            return a.owner==b.owner&&a.source==b.source&&std::memcmp(&a.value,&b.value,sizeof(float))==0;
        }),alphaInputs.end());
        auto a=previousAlphaInputs.begin(),b=alphaInputs.begin();
        while(a!=previousAlphaInputs.end()||b!=alphaInputs.end()){
            if(b==alphaInputs.end()||(a!=previousAlphaInputs.end()&&less(*a,*b))){invalidateAlpha(a->owner);++a;}
            else if(a==previousAlphaInputs.end()||less(*b,*a)){invalidateAlpha(b->owner);++b;}
            else{
                if(std::memcmp(&a->value,&b->value,sizeof(float)))invalidateAlpha(b->owner);
                ++a;++b;
            }
        }
        previousAlphaInputs.swap(alphaInputs);
    }
    const hk3ds::RoomVisualBank* particles=nullptr;
    static void extend(hk3ds::RoomCameraBounds& a,const hk3ds::RoomCameraBounds& b){
        a.left=std::min(a.left,b.left);a.bottom=std::min(a.bottom,b.bottom);
        a.right=std::max(a.right,b.right);a.top=std::max(a.top,b.top);
    }
    static constexpr unsigned nodeCount(unsigned count){
        return count==0?0:count<=8?1:1+nodeCount(count/2)+nodeCount(count-count/2);
    }
    unsigned build(Pass& pass,unsigned first,unsigned count){
        Node node;node.first=first;node.count=count;
        node.bounds=bounds[pass.ordered[pass.spatial[first]].index];
        for(unsigned i=1;i<count;++i)extend(node.bounds,bounds[pass.ordered[pass.spatial[first+i]].index]);
        const unsigned id=static_cast<unsigned>(pass.nodes.size());pass.nodes.push_back(node);
        if(count>8){
            const bool x=node.bounds.right-node.bounds.left>node.bounds.top-node.bounds.bottom;
            const auto center=[&](unsigned i){const auto& b=bounds[pass.ordered[i].index];return x?b.left+b.right:b.bottom+b.top;};
            const unsigned half=count/2;
            std::nth_element(pass.spatial.begin()+first,pass.spatial.begin()+first+half,
                             pass.spatial.begin()+first+count,[&](unsigned a,unsigned b){
                                 const float ca=center(a),cb=center(b);return ca==cb?a<b:ca<cb;
                             });
            const unsigned left=build(pass,first,half),right=build(pass,first+half,count-half);
            pass.nodes[id].left=left;pass.nodes[id].right=right;pass.nodes[id].count=0;
        }
        return id;
    }
    void query(Pass& pass,unsigned id,hk3ds::Vec2 camera){
        const auto& node=pass.nodes[id];if(!node.bounds.contains(camera.x,camera.y))return;
        if(!node.count){query(pass,node.left,camera);query(pass,node.right,camera);return;}
        for(unsigned i=0;i<node.count;++i){
            HKPSP_PROFILE(++perf::current[perf::Draws];)
            const auto slot=pass.spatial[node.first+i];
            const auto& entry=pass.ordered[slot];const auto index=entry.index;
            const bool admitted=bounds[index].contains(camera.x,camera.y);
            HKPSP_PROFILE(++perf::current[perf::SpatialChecks];
                if(!admitted)++perf::current[perf::SpatialRejects];)
            if(admitted){
                if(cpu::roomCandidates)pass.visible[slot/32]|=std::uint32_t(1)<<(slot%32);
                else pass.candidates.push_back({index,cached(pass,entry.cache)});
            }
        }
    }
public:
    RoomIndex()=default;
    RoomIndex(const hk3ds::RoomVisualBank& room,const hk3ds::TerrainBank& terrain,
              const hk3ds::RoomDynamicsBank& dynamics,const hk3ds::PlantBank& plants,const hk3ds::GrassBank& grass,
              const hk3ds::RoomAmbient& ambientState,
              const hk3ds::RoomVisualBank* ambient=nullptr,
              const RoomTiles* tiles=nullptr,const WorldActorArt* actors=nullptr,bool ge=false):
              worldGeometry(ge),particles(ambient){
#ifdef HKPSP_ROOM_GE_OFF
        worldGeometry=false;
#endif
        if(worldGeometry&&!cpu::roomCandidates)throw std::invalid_argument("GE requires dense room candidates");
        static_assert(nodeCount(8)==1&&nodeCount(9)==3&&nodeCount(17)==5,
                      "Reserve exactly the existing eight-record-leaf BVH");
        std::vector<bool> dynamic(room.draws.size());
        std::vector<bool> animated(room.draws.size());
        bounds=room.cameraBounds;
        std::vector<unsigned> mask(room.draws.size());
        const auto include=[&](unsigned i,const hk3ds::RoomDraw& d,bool addBounds=true){
            const auto& sprite=room.atlas.sprites[d.sprite];
            if(addBounds)extend(bounds[i],hk3ds::roomCameraBounds(sprite,d,screenWidth,screenHeight));
            for(unsigned p=0;p<4;++p){
                if(hk3ds::roomDrawBehindHero(d)!=(p%2==0))continue;
                hk3ds::RoomSpriteClip clip{sprite};
                if(pspRoomClip(sprite,d,p<2,clip,room.blurSplitDepth))mask[i]|=1u<<p;
            }
        };
        // Ambient binding already supplies motion envelopes. Dormant particle
        // placeholders sit at (0,0) with zero alpha/scale and are not extra poses.
        for(unsigned i=0;i<room.draws.size();++i){
            include(i,room.draws[i],false);
            if(actors&&actors->dynamic(i))dynamic[i]=true;
            if(hk3ds::MantisLords::gateOwner(room.draws[i].owner)){
                dynamic[i]=true;
                mantisGates.emplace_back(i,room.draws[i].values[5]);
            }
        }
        const auto mark=[&](unsigned i){if(i<dynamic.size())dynamic[i]=true;};
        const auto owner=[&](unsigned value){for(unsigned i=0;i<room.draws.size();++i)if(room.draws[i].owner==value)mark(i);};
        // These controllers can replace sprites, change depth/pass, or move beyond
        // the authored bounds. Check their CURRENT bounds once per frame instead.
        for(const auto& rock:dynamics.rocks)owner(rock.owner);
        for(unsigned i=0;i<dynamics.crumbleCount;++i)mark(dynamics.crumble[i].draw);
        // Finite sprite animations use the union of every possible pose. Grass
        // binding already supplies its full push/cut envelope; include replacement
        // poses here also to admit every possible pass (including split planes).
        for(unsigned i=0;i<room.draws.size();++i){
            auto pose=room.draws[i];
            // Tiled sprites need one emitted quad per piece; retain their dynamic path.
            if(tiles&&pose.sprite<tiles->size()&&!(*tiles)[pose.sprite].empty())animated[i]=true;
            if(pose.grass.amplitude!=0||pose.grass.push!=0||room.scaleFor(pose)||ambientState.animatesDraw(i))animated[i]=true;
            if(room.switchAnimations)for(const auto& animation:*room.switchAnimations)
                for(unsigned slot=0;slot<animation.slots;++slot)if(animation.draws[slot]==i)animated[i]=true;
            for(const auto& spike:dynamics.spikes)if(pose.owner==spike.owner)animated[i]=true;
            for(unsigned j=0;j<plants.animationCount();++j)if(pose.owner==plants.plants[j].owner)animated[i]=true;
            for(unsigned j=0;j<grass.count;++j)if(pose.owner==grass.bushes[j].drawOwner)animated[i]=true;
            for(const auto& spike:dynamics.spikes)if(pose.owner==spike.owner)
                for(const auto& key:dynamics.keys){pose.sprite=key.sprite;include(i,pose);}
            for(unsigned j=0;j<plants.animationCount();++j)if(pose.owner==plants.plants[j].owner){
                const auto& clip=plants.clips[plants.plants[j].clip];
                for(unsigned k=0;k<clip.count;++k){pose.sprite=clip.keys[k].sprite;include(i,pose);}
            }
            for(unsigned j=0;j<grass.count;++j)if(pose.owner==grass.bushes[j].drawOwner){
                const auto& bush=grass.bushes[j];
                for(const auto sprite:bush.pushSprites){pose.sprite=sprite;include(i,pose);}
                pose.sprite=bush.idleSprite;include(i,pose);
                pose.sprite=bush.stubSprite;pose.values=bush.stubValues;
                pose.grass={bush.stubGrass[0],bush.stubGrass[1],bush.stubGrass[2],bush.stubGrass[3],bush.stubGrass[4]};include(i,pose);
            }
        }
        for(const auto& lift:terrain.lifts)for(const auto& d:lift.draws)mark(d.index);
        for(const auto& lever:terrain.levers){mark(lever.draw);mark(lever.switchDraw);}
        for(const auto& shroom:terrain.shrooms)mark(shroom.draw);
        if(cpu::roomCandidates){
            // Raw owners include sparse synthetic IDs. Assign dense IDs once;
            // no hash table or raw-ID lookup survives on the fast frame path.
            std::unordered_map<unsigned,unsigned> ids;
            ids.reserve(room.draws.size());drawOwners.reserve(room.draws.size());
            for(const auto& draw:room.draws){
                const auto entry=ids.emplace(draw.owner,static_cast<unsigned>(owners.size()));
                if(entry.second)owners.push_back(draw.owner);
                drawOwners.push_back(entry.first->second);
            }
            denseAlpha.resize(owners.size(),-1.f);
            ownerOrder.resize(owners.size());
            for(unsigned i=0;i<owners.size();++i)ownerOrder[i]=i;
            std::sort(ownerOrder.begin(),ownerOrder.end(),[&](unsigned a,unsigned b){return owners[a]<owners[b];});
        }else{
            ownerAlpha.reserve(room.draws.size());
            for(const auto& draw:room.draws)ownerAlpha.emplace(draw.owner,-1.f);
        }
        for(unsigned i=0;i<room.draws.size();++i){
            if(dynamic[i]){moving.push_back(i);continue;}
        }
        movingVisible.reserve(moving.size());
        // Animated records include every controller that can replace the tint.
        // Only immutable, constant-zero alpha records can be discarded for the room.
        unsigned zeroPairs=0;
        for(unsigned i=0;i<room.draws.size();++i)
            if(!dynamic[i]&&!animated[i]&&room.draws[i].values[10]==0){
                for(unsigned p=0;p<4;++p)if(mask[i]&(1u<<p))++zeroPairs;
                mask[i]=0;
            }
        for(unsigned p=0;p<4;++p){
            auto& pass=passes[p];unsigned count=0,staticCount=0;
            for(unsigned i=0;i<room.draws.size();++i)if((mask[i]&(1u<<p))&&(dynamic[i]||animated[i]))
                geCounts.add(ambientState.animatesDraw(i)?RoomGeReason::Ambient:dynamic[i]?RoomGeReason::Moving:RoomGeReason::Animated);
            for(unsigned i=0;i<room.draws.size();++i)if(!dynamic[i]&&(mask[i]&(1u<<p))){++count;if(!animated[i])++staticCount;}
            pass.ordered.reserve(count);if(!worldGeometry)pass.cached.reserve(staticCount);pass.spatial.reserve(count);
            std::vector<RoomGeSource> sources;if(worldGeometry)sources.reserve(staticCount);
            pass.candidates.reserve(count+moving.size());
            if(cpu::roomCandidates)pass.visible.resize((count+31)/32);
            for(unsigned i=0;i<room.draws.size();++i)if(!dynamic[i]&&(mask[i]&(1u<<p))){
                unsigned cache=~0u;
                if(!animated[i]){
                    RoomQuadCache quad;
                    if(!cacheRoomQuad(room.atlas.sprites[room.draws[i].sprite],room.draws[i],p<2,room.blurSplitDepth,quad))
                        throw std::runtime_error("Static quad/pass admission changed during room load");
                    if(worldGeometry){cache=static_cast<unsigned>(sources.size());sources.push_back({i,bounds[i]});}
                    else {cache=static_cast<unsigned>(pass.cached.size());pass.cached.push_back(quad);}
                    geCounts.add(classifyRoomGe(quad,bounds[i],hk3ds::roomFocalLength(screenWidth,screenHeight)),&quad);
                }
                pass.spatial.push_back(static_cast<unsigned>(pass.ordered.size()));pass.ordered.push_back({i,cache});
            }
            if(worldGeometry)pass.ge=buildRoomGePlan(room,std::move(sources),p%2==0,p<2,
                                                   hk3ds::roomFocalLength(screenWidth,screenHeight));
#ifdef __PSP__
            if(worldGeometry)prepareRoomGe(pass.ge.buffers);
#endif
            pass.nodes.reserve(nodeCount(unsigned(pass.spatial.size())));
            if(!pass.spatial.empty())build(pass,0,static_cast<unsigned>(pass.spatial.size()));
        }
        // The PSP has the same four-byte scalar layout; include vector capacities.
        static_assert(sizeof(RoomQuadCache)==124&&sizeof(Pass::Entry)==8,"Report PSP cache layout explicitly");
        // Retag retained cache/index allocations once at room load, so visit
        // budgets separate them from the source room banks without estimating.
        for(const auto& pass:passes){
            memory_report::tagRoomQuadCache(pass.cached.data());
            memory_report::tagRoomQuadCache(pass.ordered.data());
            memory_report::tagRoomQuadCache(pass.spatial.data());
            memory_report::tagRoomQuadCache(pass.nodes.data());
        }
        memory_report::tagRoomQuadCache(bounds.data());
        memory_report::tagRoomQuadCache(moving.data());
        memory_report::tagRoomQuadCache(movingVisible.data());
#ifndef __PSP__
        std::size_t bytes=0,pairs=0;
        for(const auto& pass:passes){bytes+=pass.cached.capacity()*sizeof(RoomQuadCache);pairs+=pass.cached.size();}
        std::cout<<"Room quad cache: pairs="<<pairs<<" bytes="<<bytes<<" moving="<<moving.size()<<'\n';
        if(worldGeometry){
            std::size_t planBytes=0,gePairs=0,cpuPairs=0;
            for(const auto& pass:passes){planBytes+=pass.ge.bytes();
                gePairs+=pass.ge.buffers.runs.size();cpuPairs+=pass.ge.buffers.cpu.size();}
            std::cout<<"Room GE plans: ge="<<gePairs<<" cpu="<<cpuPairs<<" bytes="<<planBytes
                     <<" legacy_cache="<<bytes<<'\n';
        }
        std::cout<<"Room GE classification:";
        for(unsigned i=0;i<geCounts.pairs.size();++i)std::cout<<' '<<roomGeReasonNames[i]<<'='<<geCounts.pairs[i];
        std::cout<<" flat="<<geCounts.flatPairs<<" indexed_payload="<<geCounts.indexedBytes()
                 <<" triangle_payload="<<geCounts.triangleBytes()<<" flat_sprite_payload="<<geCounts.spriteBytes()
                 <<" replaced_cache="<<geCounts.eligible()*sizeof(RoomQuadCache)<<'\n';
        std::size_t candidateSlots=0;
        for(const auto& pass:passes)candidateSlots+=pass.candidates.capacity();
        std::cout<<"Room alpha table: owners="<<(cpu::roomCandidates?owners.size():ownerAlpha.size())<<" buckets="<<ownerAlpha.bucket_count()
                 <<" candidate_slots="<<candidateSlots<<" zero_pairs_removed="<<zeroPairs<<'\n';
#endif
        (void)zeroPairs;
    }
    void beginFrame(DrawList& list,const hk3ds::RoomVisualBank& room){
        if(worldGeometry)geScene.clear(list,room);
    }
    void prepare(hk3ds::RoomVisualBank& room,hk3ds::Vec2 camera,const hk3ds::TutorialSession& game){
        for(const auto& [index,authoredY]:mantisGates)
            room.draws[index].values[5]=authoredY+game.sceneEnemies.mantis.gateOffset();
        // Mutable records are few. Recompute using the PSP viewport, without
        // changing the shared controllers' simulation/admission bounds.
        movingVisible.clear();
        for(const auto index:moving){
            HKPSP_PROFILE(++perf::current[perf::Draws];)
            HKPSP_PROFILE(++perf::current[perf::SpatialChecks];)
            const auto& d=room.draws[index];
            if(hk3ds::roomCameraBounds(room.atlas.sprites[d.sprite],d,screenWidth,screenHeight).contains(camera.x,camera.y))
                movingVisible.push_back(index);
            HKPSP_PROFILE(else ++perf::current[perf::SpatialRejects];)
        }
        for(auto& pass:passes){
            pass.candidates.clear();
            std::fill(pass.visible.begin(),pass.visible.end(),0);
            if(!pass.nodes.empty())query(pass,0,camera);
            if(cpu::roomCandidates){
                HKPSP_PHASE(Sort,
                    // BVH leaves mark room-load order slots. Enumerate only set
                    // bits and merge the short, ordered mutable stream: no sort.
                    std::size_t movingIndex=0;
                    const auto appendMoving=[&](unsigned before){
                        while(movingIndex<movingVisible.size()&&movingVisible[movingIndex]<before){
                            const auto index=movingVisible[movingIndex++];
                            pass.candidates.push_back({index,nullptr,drawOwners[index]});
                        }
                    };
                    for(unsigned word=0;word<pass.visible.size();++word){
                        auto bits=pass.visible[word];
                        while(bits){
                            const unsigned slot=word*32+__builtin_ctz(bits);bits&=bits-1;
                            const auto& entry=pass.ordered[slot];appendMoving(entry.index);
                            pass.candidates.push_back({entry.index,cached(pass,entry.cache),drawOwners[entry.index],
                                                       worldGeometry?entry.cache:~0u});
                        }
                    }
                    appendMoving(~0u);
                    // Mutable controllers can change sort keys. Preserve the old
                    // merge/filter sequence if they break authored monotonicity.
                    pass.orderedKeys=std::is_sorted(pass.candidates.begin(),pass.candidates.end(),[&](const auto& a,const auto& b){
                        const auto& v=room.draws[a.index].values;const auto& w=room.draws[b.index].values;
                        if(v[11]!=w[11])return v[11]<w[11];
                        if(v[12]!=w[12])return v[12]<w[12];
                        return v[6]>w[6];
                    });
                );
            }else for(const auto index:movingVisible)pass.candidates.push_back({index,nullptr});
        }
        HKPSP_PHASE(OwnerTable,
            if(cpu::roomCandidates){
                updateAlphaInputs(game);
                for(const auto& pass:passes)for(const auto& ref:pass.candidates){
                    auto& value=denseAlpha[ref.owner];
                    if(value>=0)continue;
                    value=game.visualAlpha(owners[ref.owner]);
                    // Pair evaluation counts with their results; cached draws do no owner check.
                    HKPSP_PROFILE(++perf::current[perf::OwnerChecks];if(value<=0)++perf::current[perf::OwnerRejects];)
                }
            }else{
                for(auto& entry:ownerAlpha)entry.second=-1.f;
                for(const auto& pass:passes)for(const auto& ref:pass.candidates){
                    const auto owner=room.draws[ref.index].owner;
                    auto& value=ownerAlpha.at(owner);
                    if(value>=0)continue;
                    value=game.visualAlpha(owner);
                    HKPSP_PROFILE(++perf::current[perf::OwnerChecks];)
                }
            }
        );
    }
    void append(DrawList& list,const hk3ds::RoomVisualBank& room,hk3ds::Vec2 camera,float seconds,unsigned p,
                float (*alpha)(unsigned,void*),void* context,const hk3ds::RoomSortBoundary* split=nullptr,bool afterSplit=false){
        (void)alpha;(void)context; // The direct callback is retained by the host oracle.
        auto& candidates=passes[p].candidates;
        RoomSelection selection{0,candidates.size(),&denseAlpha,false};
        if(cpu::roomCandidates&&split&&passes[p].orderedKeys){
            HKPSP_PHASE(Index,
                const auto cut=std::size_t(std::partition_point(candidates.begin(),candidates.end(),[&](const auto& ref){
                    return hk3ds::roomDrawBefore(room.draws[ref.index],split->layer,split->order,split->depth);
                })-candidates.begin());
                if(afterSplit)selection.first=cut;else selection.last=cut;
                selection.splitSelected=true;
            );
        }
        HKPSP_PROFILE(perf::current[perf::RoomCandidates]+=selection.last-selection.first;)
        if(!cpu::roomCandidates)HKPSP_PHASE(Sort,
            std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.index<b.index;});
        );
#ifndef __PSP__
        // This guard runs in the actual host replay, with the same mutable room,
        // visibility callback and prefix (including the hero). It compares every
        // bit of every quad, so ordering and float reassociation cannot slip by.
        DrawList expected;expandRoomGe(list,expected);
        appendRoom(expected,room,camera,seconds,p%2==0,p<2,alpha,context,split,afterSplit,particles);
#endif
        if(worldGeometry){
            appendRoomGe(geScene.scratch,passes[p].ge,room,camera,seconds,candidates,selection,
                         split,afterSplit,particles,0,list.nextOrder());
            geScene.append(list,passes[p].ge);
#ifndef __PSP__
            DrawList actual;expandRoomGe(list,actual);
            if(actual.quads.size()!=expected.quads.size()||(!actual.quads.empty()&&
               std::memcmp(actual.quads.data(),expected.quads.data(),actual.quads.size()*sizeof(Quad))))
                throw std::runtime_error("Production mixed/full-scan quad mismatch at frame "+
                    std::to_string(list.info.frame)+" pass "+std::to_string(p));
#endif
            return;
        }
        appendRoom(list,room,camera,seconds,p%2==0,p<2,[](unsigned owner,void* table){
            const float value=static_cast<const std::unordered_map<unsigned,float>*>(table)->at(owner);
            HKPSP_PROFILE(if(value<=0)++perf::current[perf::OwnerRejects];)
            return value;
        },&ownerAlpha,split,afterSplit,particles,0,&candidates,cpu::roomCandidates?&selection:nullptr);
#ifndef __PSP__
        static_assert(sizeof(Quad)==sizeof(unsigned)*5+sizeof(float)*21,"Quad equality requires no padding");
        if(list.quads.size()!=expected.quads.size()||
           (!list.quads.empty()&&std::memcmp(list.quads.data(),expected.quads.data(),list.quads.size()*sizeof(Quad))))
            throw std::runtime_error("Indexed/full-scan quad mismatch at frame "+std::to_string(list.info.frame)+" pass "+std::to_string(p));
#endif
    }
private:
    std::vector<unsigned> movingVisible;
};
}
#if (defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)) || defined(HKPSP_CPU_ATTRIB)
static GameRunResult runGameConfig(Platform& platform,unsigned frames,MenuRuntime* menus=nullptr) {
#else
static GameRunResult runGameSession(Platform& platform,unsigned frames,MenuRuntime* menus=nullptr) {
#endif
    HKPSP_PROFILE(perf::configStarted=perf::now();)
    GameRunResult result;
    loadGameText(platform);  // Checks the data format before any other data file.
    // main.cpp:1323-1345,1353-1381: resident hero/enemy, physics, rules and entities.
    RoomTiles roomTiles;
    auto room=readRoom(platform.path("static-room-visual.bin"),&roomTiles);
    std::vector<Texture> textures;
    AnimationPages animationPages;
    const auto loadPage=[&](const std::string& name,bool unpack=cpuTextureIndices){
        auto page=readTexture(platform.path(name),unpack);
        if(page.width!=512||page.height!=512)throw std::runtime_error("Atlas/page dimensions disagree: "+name);
        textures.push_back(std::move(page));
    };
    for(unsigned i=0;i<room.atlas.pageCount;++i)loadPage("static-room-page-"+std::to_string(i)+".hpt");
    auto heroAtlas=animationPages.load(platform,textures,"hero.bin","");
    auto& hero=heroAtlas.bank;const unsigned heroPage=heroAtlas.page;
    hk3ds::LightingConfig lighting;load(lighting,platform,"lighting.bin");
    RoomLighting roomLighting(lighting);
    const auto lights=readLightAtlas(platform.path("lighting-atlas.bin"));
    const unsigned lightPage=static_cast<unsigned>(textures.size());
    for(unsigned i=0;i<lights.atlas.pageCount;++i)loadPage("light-page-"+std::to_string(i)+".hpt",true);
    hk3ds::LanternGlowConfig lanternGlow;load(lanternGlow,platform,"lantern-glow.bin");
    // Soul-liquid alpha spans sample this atlas on the CPU every frame.
    auto hudAtlas=animationPages.load(platform,textures,"hud-visual.bin","hud",true,
                                    {"Soul Mask","Liquid Idle","Liquid Shrink"});
    const unsigned hudPage=hudAtlas.page;
    auto fonts=readFonts(platform.path("fonts.bin"));
    const unsigned fontPage=static_cast<unsigned>(textures.size());
    for(unsigned i=0;i<fonts.pageCount;++i){
        auto page=readTexture(platform.path("font-page-"+std::to_string(i)+".hpt"),true);
        if(page.width!=256||page.height!=256||page.bits!=4)throw std::runtime_error("PFT1 page contract failed");
        textures.push_back(std::move(page));
    }
    HudArt hudArt(std::move(hudAtlas.bank),std::move(fonts),hudPage,fontPage);
    auto inventoryAtlas=animationPages.load(platform,textures,"inventory.bin","inventory");
    InventoryArt inventoryArt(std::move(inventoryAtlas.bank),inventoryAtlas.page);
    const auto abilityAtlas=animationPages.load(platform,textures,"ability-panel.bin","ability");
    const auto& abilityArt=abilityAtlas.bank;const unsigned abilityPage=abilityAtlas.page;
    // Cast Vengeful Spirit clips (Ball, Ball End, Ball Lv2), resident once like the other banks.
    const auto spiritAtlas=animationPages.load(platform,textures,"spirit.bin","spirit");
    const auto& spiritArt=spiritAtlas.bank;const unsigned spiritPage=spiritAtlas.page;
    // The positive signed-CLUT path is valid only for this asset contract:
    // an opaque folded light with every used channel at or above neutral128.
    // Fail loudly if a regenerated/different room light needs signed subtraction.
    const auto& light=lights.atlas.sprites[0];const auto& lightTexture=textures.at(lightPage+light.page);
    for(unsigned y=light.y;y<light.y+light.height;++y)for(unsigned x=light.x;x<light.x+light.width;++x){
        const auto& p=lightTexture.paletteFor(light.clut)[lightTexture.indices[y*lightTexture.width+x]];
        if(p[3]!=255||p[0]<128||p[1]<128||p[2]<128)
            throw std::runtime_error("PSL1 HeroLight requires opaque positive folded operands");
    }
#ifdef __PSP__
    for(unsigned i=0;i<lights.atlas.pageCount;++i)
        std::vector<unsigned char>().swap(textures[lightPage+i].indices);
#endif
    auto actorArt=std::make_unique<ActorArt>();actorArt->load(platform,textures,animationPages);
    SceneEnemyArt sceneEnemyArt;
    GreenpathArt greenpathArt;
    BossArt bossArt;
    UumuuArt uumuuArt;
    MonomonArt monomonArt;
    const auto& crawler=actorArt->crawler.bank;const auto& buzzer=actorArt->buzzer.bank;const auto& shade=actorArt->shade.bank;
    hk3ds::CrawlerConfig crawlerConfig;hk3ds::ShadeConfig shadeConfig;
    hk3ds::NailBank nails;hk3ds::RoomConfig rules;hk3ds::RoomEntityBank entities;
    hk3ds::TerrainBank terrain;hk3ds::GreatDoorConfig door;hk3ds::GeoConfig geo;
    hk3ds::DecorBank decor;hk3ds::PlantBank plants;
    load(crawlerConfig,platform,"crawler.bin");load(shadeConfig,platform,"shade.bin");
    load(nails,platform,"nail.bin");load(rules,platform,"room-rules.bin");load(entities,platform,"room-entities.bin");
    load(terrain,platform,"tutorial-room.bin");load(door,platform,"great-door.bin");load(geo,platform,"geo.bin");
    load(decor,platform,"decor.bin");load(plants,platform,"plants.bin");
    // main.cpp:1400,1413-1421: seeded room binding, with PSP viewport aspect.
    crawlerConfig.bodyMaterial=terrain.actorMaterial;crawlerConfig.corpseMaterial=terrain.corpseMaterial;
    std::uint32_t seed=1;
    auto session=std::make_unique<hk3ds::TutorialSession>();auto& game=*session;
    actorArt->bindTerrain(platform,game,terrain);
    // ConditionalEvents opens these siblings itself; include them in the host
    // dependency trace so the same binding also succeeds in staged PSP data.
    platform.path("events-fx.bin");platform.path("events-fx-extra.bin");
    if(!game.conditionalEvents.bind(&actorArt->events.bank,platform.path("events-text.bin").c_str()))
        throw std::runtime_error("Conditional events binding failed");
    if(!game.bind(rules,terrain,hero,crawler,crawlerConfig,nails,entities,{&seed,randomUnit},
                  &door,&geo,&buzzer,&decor,float(screenWidth)/screenHeight,false,&plants,&shadeConfig,&shade))
        throw std::runtime_error("Tutorial_01 bind failed");
    game.bindLighting(&lighting);
    // main.cpp:1462-1467,1513-1517,1530: static package indices address the PSR1 bank.
    hk3ds::RoomDynamicsBank dynamics;hk3ds::GrassBank grass;
    load(plants,platform,"static-plants.bin");load(dynamics,platform,"static-room-dynamics.bin");
    load(grass,platform,"static-grass.bin");
    if(!game.roomDynamics.bind(dynamics,room))throw std::runtime_error("Static room-dynamics binding failed");
    if(!game.plants.bind(plants,room,{&seed,randomUnit}))throw std::runtime_error("Static plants binding failed");
    if(!game.grass.bind(grass,room))throw std::runtime_error("Static grass binding failed");
    actorArt->bindEffects(platform,game,hero,grass);
    // Keep the complete source particle pool. Only its duplicate presentation
    // vector grows with emitted records instead of reserving every pool slot.
    game.decorEffects.visuals.draws.shrink_to_fit();
    if(!game.roomAmbient.bind(room,platform.path("static-room-ambient.bin").c_str()))
        throw std::runtime_error("Static room-ambient binding failed");
    auto moteArt=animationPages.load(platform,textures,"motes.bin","mote");
    hk3ds::RoomVisualBank moteAtlas;moteAtlas.atlas=std::move(moteArt.bank);
    const unsigned motePage=moteArt.page;
    for(auto& sprite:moteAtlas.atlas.sprites)sprite.page+=motePage;
    unsigned particleSprite=static_cast<unsigned>(room.atlas.sprites.size());
    const unsigned decorSprites=unsigned(actorArt->decor.bank.sprites.size());
    for(auto sprite:actorArt->decor.bank.sprites){sprite.page+=actorArt->decor.page;room.atlas.sprites.push_back(sprite);}
    room.atlas.sprites.insert(room.atlas.sprites.end(),moteAtlas.atlas.sprites.begin(),moteAtlas.atlas.sprites.end());
    if(!game.ambientParticles.load(platform.path("ambient-particles.bin").c_str(),0)||
       !game.ambientParticles.bindVisuals(moteAtlas))throw std::runtime_error("Tutorial motes binding failed");
    game.ambientParticles.prewarm(game.flow.camera);
    hk3ds::RoomVisualBank particles;
    std::optional<RoomIndex> roomIndex(std::in_place,room,terrain,dynamics,plants,grass,game.roomAmbient,&particles,&roomTiles,nullptr,platform.roomGeEnabled());
    const unsigned menuPage=unsigned(textures.size());
    // Always resident: the selection wings of the Yes/No boxes live on this page in test mode too.
    textures.push_back(readTexture(platform.path("menu-fill.hpt")));
    MapAtlas mapAtlas(platform,textures);
    MapAtlas regionalMapAtlas(platform,textures,"map-regions.hpt");
    MapOverlay map;
    const unsigned retainedPages=unsigned(textures.size());
    unsigned lanternGlowPage=0;
    Texture glowTexture;
    const auto appendGlowPage=[&]{
        const auto path=platform.path("lantern-glow.hpt");
        if(!glowTexture.width){
            glowTexture=readTexture(path,cpuTextureIndices);
            if(glowTexture.width!=512||glowTexture.height!=256||glowTexture.bits!=8)
                throw std::runtime_error("Lantern glow page contract failed");
        }
        lanternGlowPage=unsigned(textures.size());
        textures.push_back(std::move(glowTexture));
    };
    appendGlowPage();
    AnimationAtlas regionTitleArt;
    hk3ds::SceneBank activeScene;
    hk3ds::TownLiftRoomAssets townLiftAssets;
    hk3ds::JijiDoorRoomData jijiDoorData;
    WorldActorArt worldActorArt;
    bool worldActorsAdmitted=false;
    CorniferArt corniferArt;
    WorldQuestArt worldQuestArt;
    WorldQuestArt tisoArt;
    RouteTollArt routeTollArt;
    SlyShopArt slyShopArt;
    hk3ds::WorldSwitchRoomAssets switchAssets;
    // The waterways bank keeps pointers to its data and PSP art (WaterwaysRoom::bindArt): both live here.
    hk3ds::WaterwaysRoomData waterwaysData;
    hk3ds::AnimationBank waterwaysArt;
    // Stag bank (rooms/<scene>/stag-visual.bin, PSP form of stag-anim.bin): bound to the resting state per scene.
    hk3ds::AnimationBank stagArt;
    const auto savePath=menus?menuSlotPath(platform.savePath(),menus->state.title.selectedSlot):platform.savePath();
    auto saveWriter=savePath.empty()?nullptr:std::make_unique<TutorialSaveWriter>(savePath);
    std::vector<hk3ds::RoomGate> roomSurvey;
    // Diagnostic survey walk per frame: "<scene> walk=<l|r><ticks> wait=<ticks>" (ordinary horizontal input),
    // or "<scene> at=<x> wait=<ticks>" / "<scene> at=<x>,<y> wait=<ticks>" (survey-only placement: hero x set, y kept
    // or set when given, velocity zeroed, then settle on the floor), or "<scene> pose=<x>,<y>"
    // (match a frozen source reference: exact camera, hidden hero, three-second trigger settle).
    struct SurveyWalk{int side=0;unsigned ticks=0,wait=0;bool place=false;float x=0;bool placeY=false;float y=0;bool pinned=false;};
    std::vector<SurveyWalk> surveyWalks;
    if(frames&&savePath.empty()&&!platform.audioOutput().empty()&&!platform.renderPose()){
        const auto path=platform.sceneRoot()+"/room-survey.txt";
        std::unique_ptr<FILE,decltype(&std::fclose)> input(std::fopen(path.c_str(),"rb"),&std::fclose);
        if(input){
            char line[64];
            while(std::fgets(line,sizeof line,input.get())){
                const auto length=std::strcspn(line,"\r\n");
                if(length>=sizeof line-1||line[length]=='\0'||roomSurvey.size()>=frames)
                    throw std::runtime_error("Invalid room survey length");
                line[length]='\0';
                SurveyWalk walk{};
                if(auto* space=std::strchr(line,' ')){
                    *space='\0';
                    char side=0,extra=0;unsigned ticks=0,wait=0;float x=0,y=0;
                    if(std::sscanf(space+1,"walk=%c%u wait=%u",&side,&ticks,&wait)==3&&(side=='l'||side=='r')&&ticks)
                        walk={side=='r'?1:-1,ticks,wait};
                    else if(std::sscanf(space+1,"at=%f,%f wait=%u",&x,&y,&wait)==3&&std::isfinite(x)&&std::isfinite(y))
                        walk={0,0,wait,true,x,true,y};
                    else if(std::sscanf(space+1,"at=%f wait=%u",&x,&wait)==2&&std::isfinite(x))
                        walk={0,0,wait,true,x};
                    else if(std::sscanf(space+1,"pose=%f,%f %c",&x,&y,&extra)==2&&std::isfinite(x)&&std::isfinite(y)&&!roomSurvey.empty())
                        walk={0,0,0,false,x,false,y,true};
                    else throw std::runtime_error("Invalid room survey walk");
                }
                if(std::strlen(line)>=sizeof(hk3ds::RoomGate::scene)||!hk3ds::validSceneIdentifier(line))
                    throw std::runtime_error("Invalid room survey scene");
                hk3ds::RoomGate gate{};
                std::strcpy(gate.scene,line);
                roomSurvey.push_back(gate);
                surveyWalks.push_back(walk);
            }
            if(std::ferror(input.get())||roomSurvey.size()!=frames||std::strcmp(roomSurvey.front().scene,"Tutorial_01"))
                throw std::runtime_error("Room survey requires one scene per frame, starting with Tutorial_01");
        }
    }
    const bool surveying=!roomSurvey.empty();
    std::unique_ptr<FILE,decltype(&std::fclose)> surveyLog(nullptr,&std::fclose);
    std::unique_ptr<FILE,decltype(&std::fclose)> enemySurvey(nullptr,&std::fclose);
    if(surveying){
        surveyLog.reset(std::fopen((platform.audioOutput()+"/room-survey.csv").c_str(),"wb"));
        if(!surveyLog||std::fprintf(surveyLog.get(),"frame,scene,animation_missing_plays,entry_ticks,hero_x,hero_y,camera_x,camera_y,boss_phase,boss_x,boss_y\n")<0)
            throw std::runtime_error("Cannot open room survey report");
        enemySurvey.reset(std::fopen((platform.audioOutput()+"/enemy-survey.csv").c_str(),"wb"));
        if(!enemySurvey||std::fputs("scene,owner,bank,bound,quads,x,y,path\n",enemySurvey.get())<0)
            throw std::runtime_error("Cannot open enemy survey report");
    }
    unsigned surveyMissing=0;
    bool savesReady=false;
    // Snapshot scratch stays out of the physics/room-loader stack frame.
    const auto writeSave=[&](bool flush) __attribute__((noinline)) {
        if(!savesReady||!saveWriter||(!flush&&!saveWriter->needsSnapshot(game.saveRevision)))return;
        const auto snapshot=game.saved();
        if(flush){
            if(!saveWriter->flush(snapshot,game.saveRevision))throw std::runtime_error("Tutorial save flush failed");
        }else saveWriter->update(snapshot,game.saveRevision);
    };
    std::unique_ptr<hk3ds::DreamDialogueBank> dreamDialogue;
    const auto loadScene=[&](hk3ds::RoomGate gate,bool survey=false){
        if(!hk3ds::validSceneIdentifier(gate.scene)||(!survey&&!hk3ds::validGateIdentifier(gate.entry)))
            throw std::runtime_error("Invalid scene transition");
        const bool returning=std::strcmp(gate.scene,"Tutorial_01")==0;
        const std::string prefix=returning?"":"scenes/"+std::string(gate.scene)+"/";
        const std::string art="rooms/"+std::string(gate.scene)+"/";
        const auto root=platform.sceneRoot();
        const std::string directory=returning?root:root+"/scenes/"+gate.scene;
        // Never overlap slot I/O with synchronous room-bank I/O.
        writeSave(true);
        platform.beginSceneLoad(gate.scene);
        // Save/entry state must be captured before any borrowed room data changes.
        auto saved=std::make_unique<hk3ds::TutorialSave>(game.saved());
        const hk3ds::SceneHeroState entryState(game.simulation.hero);
        game.decorEffects.reset();game.ambientParticles.clear();
        game.sceneEnemies.clear();game.roomBreakables.clear();
        game.worldSwitch.clear();game.townLift.clear();
        corniferArt.clear(game.cartographer);
        worldQuestArt.clear(game.worldQuests);
        tisoArt.clear(game.tiso);game.tiso.bind({});
        routeTollArt.clear(game.routeTolls);
        slyShopArt.clear(game);
        bossArt.clear(game);
        uumuuArt={};
        monomonArt={};
        releaseRoomBinding(game.roomDynamics);releaseRoomBinding(game.plants);
        releaseRoomBinding(game.grass);releaseRoomBinding(game.roomAmbient);
        roomIndex.reset();particles={};
        greenpathArt.clear(game.greenpath);
        platform.releaseRoomAssets(retainedPages);
        glowTexture=std::move(textures.at(lanternGlowPage));
        textures.resize(retainedPages);
        regionTitleArt={};
        sceneEnemyArt.clear();
        // Initial room pages precede the stable shared banks. Vacate those slots
        // once; following rooms use the replaceable tail after the shared banks.
        for(unsigned i=0;i<heroPage;++i)textures[i]=Texture{};
        room={};roomTiles.clear();terrain={};activeScene={};
        dynamics={};plants={};grass={};
        worldActorArt.clear();worldActorsAdmitted=false;townLiftAssets={};jijiDoorData={};switchAssets={};waterwaysData={};waterwaysArt={};
        stagArt={};game.resting.bindStag(nullptr);
        std::optional<hk3ds::AnimationBank> worldActorRemap;
        const auto sceneLight=returning?std::string{}:platform.path(prefix+"scene-lighting.bin");
        const int previousDarkness=lighting.sceneDarkness;
        const bool lantern=game.hasLantern();
        if(!roomLighting.load(returning?nullptr:sceneLight.c_str(),lighting,lantern))
            throw std::runtime_error("Scene lighting failed: "+sceneLight);
        const float lanternReveal=hk3ds::sceneLanternRevealStart(lighting.sceneDarkness,lantern,previousDarkness);
        if(!returning){
            load(activeScene,platform,prefix+"scene.bin");
            if(survey){
                if(activeScene.rules.gates.empty())throw std::runtime_error("Room survey scene has no authored gate");
                const char* entry=activeScene.rules.gates.front().name;
                constexpr const char* surveyEntries[][2]={{"Crossroads_21","left1"},
                    {"Waterways_02","bot2"},{"Ruins2_01","left2"},
                    {"Deepnest_39","right1"},{"Deepnest_41","right1"}};
                for(const auto& item:surveyEntries)
                    if(!std::strcmp(gate.scene,item[0])){entry=item[1];break;}
                std::strcpy(gate.entry,entry);
            }
            const auto& checkpoint=saved->resting;
            const hk3ds::Vec2 position{checkpoint.benchX,checkpoint.benchY};
            if(!activeScene.enter(gate.entry,!std::strcmp(checkpoint.benchScene,gate.scene)?&position:nullptr))
                throw std::runtime_error("Scene entry missing: "+std::string(gate.scene)+"/"+gate.entry);
            hk3ds::inheritHeroHazardRules(activeScene.rules,rules);
        }
        load(terrain,platform,prefix+"tutorial-room.bin");
        game.abilityPickup.dreamText=nullptr;
        if(!std::strcmp(gate.scene,"RestingGrounds_04")||!std::strcmp(gate.scene,"Dream_Nailcollection")){
            if(!dreamDialogue)dreamDialogue=std::make_unique<hk3ds::DreamDialogueBank>();
            load(*dreamDialogue,platform,"dream-dialogue.bin");
            game.abilityPickup.dreamText=dreamDialogue.get();
        }else dreamDialogue.reset();
        if(!returning){
            // User 2026-11-21 (PSP-3000 photo): an exit into a room the package does not stage (Crossroads_30) threw
            // "Scene lighting failed". As on the 3DS (src/main.cpp:1716-1719), such a gate is closed and becomes a
            // wall; the PSP package has scenes/<room>/scene.bin for every staged room.
            for(auto& exit:activeScene.rules.gates){
                if(!exit.scene[0]||!std::strcmp(exit.scene,"Tutorial_01"))continue;
                std::unique_ptr<FILE,decltype(&std::fclose)> staged(
                    std::fopen((root+"/scenes/"+exit.scene+"/scene.bin").c_str(),"rb"),&std::fclose);
                if(!staged){exit.scene[0]=0;exit.unavailable=true;}
            }
            hk3ds::addUnavailableGateBarriers(activeScene.rules,terrain);
        }
        room=readRoom(platform.path(art+"static-room-visual.bin"),&roomTiles);
        const unsigned firstPage=unsigned(textures.size());
        for(unsigned i=0;i<room.atlas.pageCount;++i)loadPage(art+"static-room-page-"+std::to_string(i)+".hpt");
        for(auto& sprite:room.atlas.sprites)sprite.page+=firstPage;
        for(auto& pieces:roomTiles)for(auto& piece:pieces)piece.page+=firstPage;
        terrain.bindMotionVisuals(room);
        if(returning){
            actorArt->bindTerrain(platform,game,terrain);
            load(plants,platform,"static-plants.bin");load(dynamics,platform,"static-room-dynamics.bin");load(grass,platform,"static-grass.bin");
            if(!game.roomDynamics.bind(dynamics,room)||!game.plants.bind(plants,room,{&seed,randomUnit})||
               !game.grass.bind(grass,room)||!game.roomAmbient.bind(room,platform.path("static-room-ambient.bin").c_str()))
                throw std::runtime_error("Tutorial return visuals failed");
            auto life=std::move(game.enemyLife);
            if(!game.bind(rules,terrain,hero,crawler,crawlerConfig,nails,entities,{&seed,randomUnit},
                          &door,&geo,&buzzer,&decor,float(screenWidth)/screenHeight,false,&plants,&shadeConfig,&shade)||
               !game.restoreSceneEntry(*saved,entryState,gate.entry))throw std::runtime_error("Tutorial return failed");
            game.enemyLife=std::move(life);
            for(const auto& entry:rules.gates)if(!std::strcmp(entry.name,gate.entry)){
                auto& h=game.simulation.hero;h.position={(entry.trigger.left+entry.trigger.right)*.5f-2,(entry.trigger.bottom+entry.trigger.top)*.5f};
                h.inputEnabled=h.controlEnabled=h.gravityEnabled=true;h.velocity={};game.flow.phase=hk3ds::RoomPhase::Playing;
                game.flow.camera=h.position;game.flow.enteredThrough(gate.entry);game.rememberTutorialGate(gate.entry);
            }
            game.ambientParticles.load(platform.path("ambient-particles.bin").c_str(),0);
            if(!game.ambientParticles.bindVisuals(moteAtlas))throw std::runtime_error("Tutorial return motes failed");
            game.setSceneAmbient(true);
        }else{
            game.worldQuests.loadScene(root.c_str(),gate.scene);
            if(!std::strcmp(gate.scene,"Town")&&!game.tiso.load(platform.path("tiso.bin").c_str()))
                throw std::runtime_error("Town Tiso table missing or invalid");
            if(hk3ds::Cartographer::hasPlacement(gate.scene))corniferArt.load(platform,textures,game.cartographer);
            game.enterScene(activeScene.rules,terrain,gate.scene,gate.entry);
            if(game.areaTitle.originalRegionLayout())
                regionTitleArt=animationPages.load(platform,textures,"region-title.bin","region-title");
            // main.cpp:1865-1875: the quest art is bound only while the source room holds the NPC. Load it after
            // the scene entry (WorldQuests::enter sets presence); a present quest without a bound bank throws.
            worldQuestArt.load(platform,textures,game.worldQuests);worldQuestArt.require(game.worldQuests);
            tisoArt.load(platform,textures,game.tiso);tisoArt.require(game.tiso);
            if(!std::strcmp(gate.scene,"Room_shop"))slyShopArt.load(platform,textures,game);
            if(!std::strcmp(gate.scene,"Room_mapper"))slyShopArt.loadOrnaments(platform,textures);
            game.sceneWalls.bindVisuals(room);
            game.setSceneAmbient(false);
            if(activeScene.defaultAmbient){
                const bool sharedMotes=activeScene.ambientZone==5;
                const auto config=sharedMotes?"ambient-particles.bin":
                    "deepnest-ambient-"+std::to_string(activeScene.ambientZone)+".bin";
                if(!game.ambientParticles.load(platform.path(config).c_str(),sharedMotes?0:~0u)||
                   !game.ambientParticles.bindVisuals(sharedMotes?moteAtlas:game.decorEffects.visuals))
                    throw std::runtime_error("Scene ambient binding failed: "+config);
                game.setSceneAmbient(true,activeScene.ambientZone);
            }else{
                const bool archive=!std::strcmp(gate.scene,"Fungus3_archive_02");
                const auto path=directory+(archive?"/archive-particles.bin":"/room-particles.bin");
                if(auto* file=std::fopen(path.c_str(),"rb")){
                    std::fclose(file);
                    if(!game.ambientParticles.load(path.c_str())||(game.ambientParticles.ready()&&!game.ambientParticles.bindVisuals(room,true)))
                        throw std::runtime_error("Room particles failed");
                }else if(archive)throw std::runtime_error("Missing asset: "+path);
                game.setSceneAmbient(game.ambientParticles.ready());
            }
            std::unique_ptr<hk3ds::SavedPlayerData> dreamArrival;
            if(survey&&!std::strcmp(gate.scene,"Cutscene_Boss_Door")){
                dreamArrival=std::make_unique<hk3ds::SavedPlayerData>();
                if(!dreamArrival->set("maskToBreak",2)||!dreamArrival->set("dreamReturnScene",1))
                    throw std::runtime_error("Room survey could not set Monomon's cutscene arrival");
            }
            const bool sourceCityGate=!std::strcmp(gate.scene,"Fungus2_21");
            if(!game.bindSceneEnemies(activeScene,platform.sceneEnemyRoot().c_str(),dreamArrival.get(),!sourceCityGate))
                throw std::runtime_error("Scene enemies failed: "+std::string(gate.scene));
            // main.cpp:1864-1876: bindSceneEnemies loads the toll definition (RouteTolls::load), and only then is
            // the toll art bound. Loading earlier found no definition and left the machines without a bank.
            routeTollArt.load(platform,textures,animationPages,game.routeTolls,gate.scene);
            if(game.routeTolls.active()&&!routeTollArt.bound())throw std::runtime_error("Route toll art unbound: "+std::string(gate.scene));
            // 2026-11-21 (boss-uumuu Monomon review): door_dreamReturn is a customFade door, so RoomFlow keeps the
            // blanker black for its source owner (src/room.cpp:447). No owner releases it on the PSP, and the
            // Archives stayed black after Monomon's dream (and on a save restored at that gate). Fade in here.
            if(dreamArrival||!std::strcmp(gate.entry,"door_dreamReturn"))game.flow.fadeIn();
            sceneEnemyArt.load(platform,textures,animationPages,game.sceneEnemies);
            // main.cpp:1926-1934: the boss bank (False Knight, Gruz Mother) is bound once its encounter is entered.
            bossArt.load(platform,textures,animationPages,game,gate.scene);
            uumuuArt.load(platform,textures,animationPages,game.uumuu);
            monomonArt.load(platform,textures,animationPages,game.monomon);
            if(!std::strcmp(gate.scene,"Town")){
                if(!townLiftAssets.data.load(directory.c_str()))throw std::runtime_error("Town lift assets failed");
                townLiftAssets.art=loadWorldActorArt(platform,textures,art+"town-lift");
                if(!game.bindSavedTownLift(townLiftAssets.data,townLiftAssets.art)||
                   !jijiDoorData.load(directory.c_str())||!game.bindSavedJijiDoor(jijiDoorData))throw std::runtime_error("Town mechanisms failed");
                worldActorsAdmitted=true;
            }
            // main.cpp:1827-1839 (stag bank) and main.cpp:1907-1908 (station): Crossroads_47 binds its station on the
            // shared world-actor path, with the stag bank bound to the resting state. Load after the scene entry (the
            // station needs terrain and rules); a missing bank or station throws instead of drawing nothing.
            // Room_Town_Stag_Station (Dirtmouth, station 0) is the second ride station: it binds the bank only, its stand
            // and bell are drawn from the bank (appendStagStation).
            if(!std::strcmp(gate.scene,"Crossroads_47")||!std::strcmp(gate.scene,"Room_Town_Stag_Station")){
                stagArt=loadWorldActorArt(platform,textures,art+"stag");
                if(stagArt.pageCount!=5)throw std::runtime_error("Stag bank missing or not 5 pages: "+std::string(gate.scene));
                if(!std::strcmp(gate.scene,"Crossroads_47")){
                    if(!game.bindSavedStagStation(directory.c_str()))throw std::runtime_error("Stag station failed");
                    worldActorRemap=loadWorldActorArt(platform,textures,art+"stag-station-art");
                    worldActorsAdmitted=true;
                }
                game.resting.bindStag(&stagArt);
            }
            if(!std::strcmp(gate.scene,"Crossroads_03")){
                if(!switchAssets.load(directory.c_str()))throw std::runtime_error("World switch failed");
                // PSP pages replace the HK3DS 1024 RGBA4 atlas. Sprite order and frame IDs stay HK3DS; page IDs
                // are global texture indices, so the bank's page bound becomes the global texture count.
                switchAssets.art=loadWorldActorArt(platform,textures,art+"world-switch");
                switchAssets.art.pageCount=unsigned(textures.size());
                if(!game.bindSavedWorldSwitch(switchAssets.data,switchAssets.art))throw std::runtime_error("World switch failed");
                worldActorsAdmitted=true;
            }
            if(!std::strcmp(gate.scene,hk3ds::ShamanPillar::scene)){
                if(!game.bindSavedShamanPillar(directory.c_str()))throw std::runtime_error("Shaman pillar failed");
                worldActorRemap=loadWorldActorArt(platform,textures,art+"shaman-pillar");
                worldActorsAdmitted=true;
            }
            if(!std::strcmp(gate.scene,hk3ds::MantisGates::scene)){
                if(!game.bindSavedMantisGates(directory.c_str()))throw std::runtime_error("Mantis gates failed");
                worldActorRemap=loadWorldActorArt(platform,textures,art+"mantis-gate");
                worldActorsAdmitted=true;
            }
            if(sourceCityGate){
                hk3ds::CityCrestRoomData crest;
                if(!crest.load(directory.c_str())||!game.bindSavedCityCrest(crest))
                    throw std::runtime_error("City Crest gate failed");
                if(!game.cityCrestDust.load(directory.c_str(),true))
                    throw std::runtime_error("City Crest dust data failed");
                auto crestArt=loadWorldActorArt(platform,textures,art+"city-crest-fx");
                if(crestArt.pageCount!=1||crestArt.sprites.size()!=game.cityCrestDust.visuals.atlas.sprites.size())
                    throw std::runtime_error("City Crest dust art failed");
                game.cityCrestDust.visuals.atlas=std::move(crestArt);
                worldActorsAdmitted=true;
            }
            // main.cpp:1915-1916 bindSavedWaterways + loadActors("waterways-art-"): the machine data is the
            // 3DS WWM1 file, the art is the PSP PSA1 table of the same sprite and clip order.
            if(!std::strcmp(gate.scene,"Ruins1_05b")){
                if(!waterwaysData.load(directory.c_str()))throw std::runtime_error("Waterways machine failed");
                waterwaysArt=loadWorldActorArt(platform,textures,art+"waterways-art");
                if(!game.bindSavedWaterways(waterwaysData)||!game.waterways.bindArt(waterwaysArt))
                    throw std::runtime_error("Waterways failed");
                worldActorsAdmitted=true;
            }
            // main.cpp:1917-1918 bindSavedCityToll + loadActors("city-lift-art-"): the lift art is the PSP PSA1 table
            // (same sprite order as HK3DS, PSP pages); its room table and machine data are the scene sidecars.
            if(!std::strcmp(gate.scene,"Crossroads_49b")||!std::strcmp(gate.scene,"Crossroads_49")){
                hk3ds::CityLiftArtAssets liftArt;
                liftArt.art=loadWorldActorArt(platform,textures,art+"city-lift-art");
                if(!liftArt.loadRoom(directory.c_str())||!game.bindSavedCityToll(directory.c_str(),&liftArt))
                    throw std::runtime_error("City lift failed: "+std::string(gate.scene));
                worldActorsAdmitted=true;
            }
            greenpathArt.load(platform,textures,animationPages,game.greenpath,nails,
                              directory+"/greenpath-enemies.bin",saved->flags);
        }
        appendGlowPage();
        game.collectibles.bind({});game.collectibles.load((directory+"/collectibles.bin").c_str());
        game.bindLighting(&lighting);
        if(lanternReveal>0)game.darkness.beginLanternReveal(lanternReveal,true);
        particleSprite=unsigned(room.atlas.sprites.size());
        for(auto sprite:actorArt->decor.bank.sprites){sprite.page+=actorArt->decor.page;room.atlas.sprites.push_back(sprite);}
        room.atlas.sprites.insert(room.atlas.sprites.end(),moteAtlas.atlas.sprites.begin(),moteAtlas.atlas.sprites.end());
        if(worldActorsAdmitted)worldActorArt.bind(room,game.worldActorBank(),[&](unsigned owner){
            return game.townLift.moves(owner)||owner==townLiftAssets.data.closedOwner||
                (game.jijiDoor.bound()&&owner==jijiDoorData.owner)||game.cityLift.moves(owner)||
                (game.cityCrest.bound()&&owner==game.cityCrest.data().owner);
        },worldActorRemap?&*worldActorRemap:nullptr);
        if(!survey||std::strcmp(gate.scene,"Cutscene_Boss_Door"))
            roomIndex.emplace(room,terrain,dynamics,plants,grass,game.roomAmbient,&particles,&roomTiles,&worldActorArt,platform.roomGeEnabled());
        platform.preloadRoomPages(textures,gate.scene,false);
        std::cout<<"Entered "<<gate.scene<<" / "<<gate.entry<<"; room pages="<<room.atlas.pageCount<<std::endl;
    };
    bool restoredSave=false;
    bool enteredSavedRoom=false;
    // A replay may carry a prepared save beside its tape. Load it read-only:
    // captures exercise ordinary input without ever changing the fixture.
    std::string initialSave=savePath;
    const auto initialReplay=platform.inputReplay();
    if(initialSave.empty()&&!platform.renderPose()&&!initialReplay.empty()){
        const auto slash=initialReplay.find_last_of("/\\");
        initialSave=initialReplay.substr(0,slash==std::string::npos?0:slash+1)+"replay-save.bin";
    }
    if(!initialSave.empty()){
        auto saved=std::make_unique<hk3ds::TutorialSave>();
        if(saved->load(initialSave.c_str())){
            restoredSave=true;
            if(!game.restore(*saved))throw std::runtime_error("Tutorial save restore failed");
            // Match the shared startup policy: the last bench takes precedence
            // over the scene/gate recorded while travelling.
            if(saved->resting.benchScene[0]){
                std::snprintf(saved->scene,sizeof(saved->scene),"%s",saved->resting.benchScene);
                std::strcpy(saved->gate,"bench");
            }
            if(std::strcmp(saved->scene,"Tutorial_01")||saved->gate[0]){
                hk3ds::RoomGate gate{};
                std::snprintf(gate.scene,sizeof(gate.scene),"%s",saved->scene);
                std::snprintf(gate.entry,sizeof(gate.entry),"%s",saved->gate);
                loadScene(gate);
                enteredSavedRoom=true;
            }
            std::cout<<"Restored tutorial save: "<<saved->scene<<" / "<<saved->gate
                     <<"; broken="<<saved->broken<<"; flags="<<saved->flags
                     <<"; geo="<<saved->geo<<"; shade="<<saved->shadePresent<<std::endl;
        }
    }
    if(menus&&!game.setPlayerData("backerCredits",menus->state.pause.settings.backerCredits))
        throw std::runtime_error("Cannot apply Backer Credits setting");
    savesReady=true;
    // Bounded save-enabled acceptance only: no per-frame file I/O or snapshots.
    unsigned acceptanceDeaths=0,acceptanceRetries=0,acceptanceTownExits=0;
    const auto recordSaveState=[&](const char* name){
        if(!frames||!saveWriter||platform.audioOutput().empty())return;
        const auto saved=std::make_unique<hk3ds::TutorialSave>(game.saved());
        const auto path=platform.audioOutput()+"/"+name;
        FILE* file=std::fopen(path.c_str(),"w");
        if(!file)throw std::runtime_error("Tutorial state open failed");
        const int result=std::fprintf(file,
            "{\"restored\":%s,\"scene\":\"%s\",\"gate\":\"%s\",\"broken\":%u,\"flags\":%u,\"geo\":%u,"
            "\"shade_present\":%u,\"shade_geo\":%u,\"health\":%u,\"deaths\":%u,\"retries\":%u,\"town_exits\":%u,"
            "\"save_writes\":%u,\"worker_writes\":%u,\"syncs\":%u}\n",
            restoredSave?"true":"false",saved->scene,saved->gate,unsigned(saved->broken),unsigned(saved->flags),unsigned(saved->geo),
            unsigned(saved->shadePresent),unsigned(saved->shadeGeo),unsigned(game.simulation.hero.vitals.health),
            acceptanceDeaths,acceptanceRetries,acceptanceTownExits,
            saveWriter->writes(),saveWriter->workerWrites(),saveWriter->syncs());
        const int closed=std::fclose(file);
        if(result<0||closed)throw std::runtime_error("Tutorial state write failed");
    };
    recordSaveState("tutorial-start.json");
    // main.cpp:1630-1661: replay is consumed by the simulation's InputLatch source,
    // once per control tick, including entry/transition phases (never per draw).
    hk3ds::InputReplay replay;
    // A zero frame limit selects interactive play; the host/test path keeps its tape.
    const auto* fixedPose=platform.renderPose();
    hk3ds::AnimationPlayer captureIdle;
    if(fixedPose){
        const auto* idle=hero.find("Idle");if(!idle)throw std::runtime_error("Capture Idle clip missing");
        captureIdle.play(idle);
        // stable-v1 provenance: first render after a three-second scene settle,
        // HeroController disabled, Rigidbody2D frozen but simulated. Keep the
        // source player-triggered remask/reveal callbacks during the settle.
        game.setFrameCaptureActive(true,true);game.testFreezeEnemies=true;game.testInvulnerable=true;
        game.testPinnedHeroTriggers=true;
        game.flow.phase=hk3ds::RoomPhase::Playing;
        game.flow.camera={fixedPose->cameraX,fixedPose->cameraY};
        game.simulation.hero.position={fixedPose->heroX,fixedPose->heroY};
        game.simulation.hero.velocity={};game.simulation.hero.actor=hk3ds::Actor::Idle;
        // stable-v1/stable-room-state.json: right; HeroLight scale is (-3,3,1).
        game.simulation.hero.facingRight=true;
        hk3ds::InputLatch neutral;
        const hk3ds::Vec2 emitterCamera{fixedPose->cameraX,fixedPose->cameraY};
        game.ambientParticles.prewarm(emitterCamera);
        for(unsigned i=0;i<180;++i)game.advance(hk3ds::tuning::controlStep,neutral,&emitterCamera);
    }
#ifndef HKPSP_DIAG
    const auto replayPath=platform.inputReplay();
    if(!surveying&&!fixedPose&&!replayPath.empty()){
        if(!replay.load(replayPath.c_str()))throw std::runtime_error("Input replay invalid: "+replayPath);
    }else if(frames&&!menus&&!surveying&&!fixedPose&&!replay.loadBytes(hk3ds::tutorialReplay,sizeof(hk3ds::tutorialReplay)))throw std::runtime_error("Built-in replay invalid");
    if(frames&&!menus&&!surveying&&!fixedPose&&replayPath.empty()){
#if (defined(HKPSP_PERF) || defined(HKPSP_CPU_ATTRIB)) && !defined(HKPSP_GPU_ATTRIB)
    // Optional ordinary-input tape. Invalid supplied data must never fall back.
    const auto tapePath=platform.path("perf-tape.hkir");
#else
    // Test-mode captures replay the same combat input without PERF disabling I/O.
    const auto tapePath=platform.path("residency-tape.hkir");
#endif
        if(auto* tape=std::fopen(tapePath.c_str(),"rb")){
            std::fclose(tape);if(!replay.load(tapePath.c_str()))throw std::runtime_error("Perf tape invalid");
            // Ordinary neutral settling matches the tutorial boot before measured combat.
            hk3ds::InputLatch neutral;neutral.sample(0,0,0);
            for(unsigned tick=0;tick<780;++tick)game.advance(hk3ds::tuning::controlStep,neutral);
        }
    }
#endif
    HKPSP_DIAG_ONLY(diag::start(game);)
    struct Source {hk3ds::InputReplay* replay;Platform* platform;hk3ds::TutorialSession* game;
        GameplayMenuGuard* guard;bool neutral;unsigned steps=0;};
    Source source{&replay,&platform,&game,menus?&menus->guard:nullptr,menus&&menus->replay.active()};hk3ds::InputLatch input;
    input.bindSource(&source,[](void* context,hk3ds::InputLatch& latch){
        HKPSP_PROFILE(const auto inputRunStart=perf::runClock(perf::gameThread);const auto inputStart=perf::now();)
        auto& s=*static_cast<Source*>(context);++s.steps;
        if(s.replay->active())s.replay->sample(latch,s.game->shopMenuOpen());
        else {const auto i=s.neutral?hk3ds::Input{}:s.platform->poll();latch.sample(i.x,i.y,s.guard?s.guard->filter(i.held):i.held);}
        HKPSP_PROFILE(const auto elapsed=perf::now()-inputStart;
            perf::current[perf::Input]+=elapsed;perf::inputRun+=perf::runClock(perf::gameThread)-inputRunStart;
            // Input is subtracted from Simulation; exclude it from parent probes too.
            if(hk3ds::performanceObservation.row)hk3ds::performanceObservation.simulationAccounted+=elapsed;)
    });
    DrawList list;list.textures=&textures;list.roomTiles=&roomTiles;
    TextCanvas textCanvas(list,hudArt);
    textCanvas.setRegionTitleArt(regionTitleArt);
    textCanvas.setMenuArrowPage(menuPage);
    if(!game.bindLore(platform.path("lore-tablets.bin").c_str()))
        throw std::runtime_error("Native lore binding failed");
    textCanvas.prepareLore(game.loreBank);
    textCanvas.setShopIcons(slyShopArt.icons);
#ifdef __PSP__
    // Canvas keeps only the fill texel coordinates; GE uses the packed payload.
    for(unsigned i=0;i<hudArt.fonts.pageCount;++i)
        std::vector<unsigned char>().swap(textures[fontPage+i].indices);
#endif
    if(!enteredSavedRoom)platform.preloadRoomPages(textures,"Tutorial_01",true);
#ifdef HKPSP_SIM_TRACE
    // Host work counts only: a zero clock deliberately supplies no timing data.
    std::unique_ptr<FILE,decltype(&std::fclose)> simulationCounts(
        std::fopen((platform.audioOutput()+"/simulation-counts.csv").c_str(),"wb"),&std::fclose);
    if(!simulationCounts)throw std::runtime_error("Cannot open simulation counts");
    std::fprintf(simulationCounts.get(),"frame,bodies,collider_visits,combat_tests,decor_tests,decor_particles,ambient_slots,decor_emission_slots,state_hash_lo,state_hash_hi,state_leaves,decor_terrain_tests,decor_terrain_queries\n");
#endif
#ifdef HKPSP_PERF
    // Opt-in functional evidence. Hash after perf::end(), outside all measured
    // phases; never enable this marker for PSP-3000 timing acceptance.
    auto* stateMarker=std::fopen(platform.path("simulation-state.cfg").c_str(),"rb");
    const bool stateHashRequested=stateMarker!=nullptr;
    if(stateMarker)std::fclose(stateMarker);
#endif
    // Opt-in parity evidence only; ordinary captures and hardware timings do no
    // hashing or trace I/O. Compare logical draws before backend grade variants.
    std::unique_ptr<FILE,decltype(&std::fclose)> parity(nullptr,&std::fclose);
    if(frames)if(auto* marker=std::fopen(platform.path("gpu-parity.cfg").c_str(),"rb")){
        std::fclose(marker);
#ifdef HKPSP_GPU_ATTRIB
        const auto name=std::string(gpu::config().name)+"-parity.csv";
#else
        const std::string name="simulation-parity.csv";
#endif
        parity.reset(std::fopen((platform.audioOutput()+"/"+name).c_str(),"wb"));
        if(!parity)throw std::runtime_error("Cannot open simulation parity trace");
        std::fprintf(parity.get(),"frame,control_steps,seed,seconds,hero_x,hero_y,camera_x,camera_y,quads,draw_hash\n");
    }
    // main.cpp:1611-1625,2349-2359: committed vitals/Geo, retry epoch,
    // shared HUD clocks advanced only by elapsed scaled simulation time.
    const auto hudSnapshot=[&](){const auto& h=game.simulation.hero;
        return hk3ds::HudSnapshot::from(h.vitals,game.geo,h.focus==hk3ds::FocusPhase::Draining,game.shade.recovering());};
    hk3ds::HudPresentation hud;hud.reset(hudSnapshot());
    HudTextCache hudText;
    unsigned hudEpoch=game.audioEpoch;float hudSeconds=game.levelSeconds(),hudClock=0;
    std::unique_ptr<AudioScene> audio;
    // Fixed-pose captures are a silent visual fixture with no gameplay input.
    if(!fixedPose&&!surveying&&!platform.audioOutput().empty())audio=std::make_unique<AudioScene>(platform,frames!=0&&platform.audioDiagnostics());
    const auto applyMenuVolumes=[&]{
        if(audio&&menus){
            const auto& settings=menus->state.pause.settings;
            audio->volumes(settings.musicVolume,settings.soundVolume,settings.masterVolume);
        }
    };
    applyMenuVolumes();
    for(unsigned frame=0;(!frames||frame<frames)&&(!menus||menus->available(frames));++frame){
        if(surveying&&frame)loadScene(roomSurvey[frame],true);
        const auto* surveyPose=surveying&&surveyWalks[frame].pinned?&surveyWalks[frame]:nullptr;
        unsigned surveyEntryTicks=0;
        if(surveying){
            namespace mask=hk3ds::monomon_mask_data;
            constexpr unsigned maxEntryTicks=600;
            constexpr float step=float(hk3ds::tuning::controlStep);
            constexpr auto maskDisappear=mask::bodySprite[std::size(mask::bodySprite)-1].time;
            constexpr auto maskCaptureTick=unsigned((maskDisappear-step*.5f)/step);
            const bool maskCinematic=!std::strcmp(game.sceneName(),"Cutscene_Boss_Door");
            hk3ds::InputLatch neutral;
            hk3ds::SavedPlayerData isolatedMaskProgress;
            while(game.credits.blankerAlpha()>0||game.flow.transitionFade()>0||game.flow.deathFade()>0||
                  (!frame&&game.flow.phase!=hk3ds::RoomPhase::Playing)||
                  (maskCinematic&&surveyEntryTicks<maskCaptureTick)){
                if(surveyEntryTicks==maxEntryTicks||game.flow.transition)
                    throw std::runtime_error("Room survey entry did not clear: "+std::string(game.sceneName()));
                if(!frame)game.advance(step,neutral);
                else if(game.flow.phase==hk3ds::RoomPhase::DoorArrival&&!game.monomon.locked())
                    game.flow.update(step,game.simulation,terrain.world(),&terrain,step,false,&hero);
                else game.flow.advanceArrivalFade(step);
                if(maskCinematic){
                    game.monomon.update(step,game.simulation.hero,neutral.consume(),isolatedMaskProgress,false);
                    if(game.monomon.transition())throw std::runtime_error("Room survey missed the visible mask cinematic");
                }
                ++surveyEntryTicks;
            }
            if(maskCinematic){
                game.flow.camera={mask::rootX[0].d,mask::rootY[0].d};
                bool hasMaskBody=false;
                for(unsigned index=0;index<room.draws.size();++index){
                    auto& draw=room.draws[index];
                    game.monomon.present(draw);
                    if(draw.owner>=mask::ownerBase&&draw.owner<mask::ownerBase+24&&draw.values[10]>0)
                        hasMaskBody=true;
                    room.cameraBounds[index]=hk3ds::roomCameraBounds(room.atlas.sprites[draw.sprite],draw,screenWidth,screenHeight);
                }
                if(!hasMaskBody)throw std::runtime_error("Room survey mask body is missing");
                roomIndex.emplace(room,terrain,dynamics,plants,grass,game.roomAmbient,&particles,&roomTiles,&worldActorArt,platform.roomGeEnabled());
            }
            // Diagnostic walk or placement (survey only): same control step as the entry loop; normal play never reaches here.
            if(surveyPose){
                const hk3ds::Vec2 position{surveyPose->x,surveyPose->y};
                game.setFrameCaptureActive(true,true);
                game.testPinnedHeroTriggers=true;
                game.flow.phase=hk3ds::RoomPhase::Playing;
                game.flow.camera=position;
                game.simulation.hero.position=position;
                game.simulation.hero.velocity={};
                game.simulation.hero.facingRight=true;
                for(unsigned tick=0;tick<180;++tick){
                    game.advance(step,neutral,&position);
                    if(game.flow.transition)throw std::runtime_error("Room survey pose left the scene: "+std::string(game.sceneName()));
                }
                game.setFrameCaptureActive(false);
                game.testPinnedHeroTriggers=false;
            }else if(frame&&(surveyWalks[frame].side||surveyWalks[frame].place)){
                const auto& walk=surveyWalks[frame];
                hk3ds::InputLatch walkInput;
                if(walk.place){
                    // Settle 30 control ticks (0.5 s) on the floor after placement with neutral input.
                    game.simulation.hero.position.x=walk.x;
                    if(walk.placeY)game.simulation.hero.position.y=walk.y;
                    game.simulation.hero.velocity={};
                    for(unsigned tick=0;tick<30;++tick){
                        walkInput.sample(0.f,0,0);
                        game.advance(step,walkInput);
                        if(game.flow.transition)throw std::runtime_error("Room survey placement left the scene: "+std::string(game.sceneName()));
                    }
                }
                for(unsigned tick=0;tick<walk.ticks+walk.wait;++tick){
                    walkInput.sample(tick<walk.ticks?float(walk.side):0.f,0,0);
                    game.advance(step,walkInput);
                    if(game.flow.transition)throw std::runtime_error("Room survey walk left the scene: "+std::string(game.sceneName()));
                }
            }
        }
        bool quitToTitle=false;
        std::size_t mapRoom=hk3ds::savedMapRoomCount;
        if(menus){
            const auto ui=menus->pollUi(platform);
            const bool wasPaused=map.pauses();
            auto menuUi=ui;
            // main.cpp:2151: START does not open the pause menu while Jiji's door owns the hero.
            if(game.jijiDoor.controlsHero()||game.cityToll.controlsHero()||game.cityLift.controlsHero()||game.cityTransport.locked())
                menuUi.pressed&=~UiStart;
            const auto action=menus->state.update(menuUi);
            if(action==MenuState::Action::SettingsChanged){
                menus->saveSettings();applyMenuVolumes();
            }
            if(ui.selectHoldPressed||map.showsMap()||wasPaused)mapRoom=hk3ds::savedMapRoomIndex(game.sceneName());
            map.update(ui,menus->state.simulate()&&!game.flow.transition&&!game.shopMenuOpen()&&
                !game.cartographer.locked()&&!game.worldQuestDialogue.locked(),
                game.cartographer.progress.ownsMap(),mapRoom<hk3ds::savedMapRoomCount&&
                game.cartographer.progress.ownsArea(hk3ds::savedMapRoom(mapRoom).area));
            // Charms page: d-pad/nub move the grid (CharmsPage, as the 3DS Charms tab); Cross is Jump
            // and Circle (menu confirm, docs/psp/controls.md) toggles the selected charm. toggleCharm refuses
            // unless the player rests at a bench.
            if(map.view()==MapView::Charms){
                hk3ds::Input charmInput;
                charmInput.x=float(int(bool(ui.pressed&UiRight))-int(bool(ui.pressed&UiLeft)));
                charmInput.y=float(int(bool(ui.pressed&UiUp))-int(bool(ui.pressed&UiDown)));
                game.charmsPage.navigate(charmInput);
                if(ui.pressed&UiCircle)game.toggleCharm();
            }
            mapAtlas.setVisible(platform,textures,map.showsMap());
            regionalMapAtlas.setVisible(platform,textures,map.showsMap()&&
                (game.cartographer.progress.flags&hk3ds::CartographySave::regionalMaps)&&
                (map.view()!=MapView::Quick||mapRoom>=hk3ds::mapdata::roomCount));
            if(!menus->state.simulate()||map.pauses()||wasPaused){
                menus->guard.block(platform.poll().held);input.sample(0,0,0);
            }
            quitToTitle=action==MenuState::Action::QuitToTitle;
        }
        HKPSP_PROFILE(perf::begin();cpu::times={};cpu::runTimes={};cpu::drawCalls=0;)
#ifdef __PSP__
        platform.beginFrame();
#endif
        // Share phase boundaries with the coarse timer: a preemption between
        // separate clock reads must not become unattributed draw-list time.
        HKPSP_PROFILE(const auto drawStart=cpu::change(cpu::Prepare);)
#ifdef HKPSP_RENDER_PROBE
        cpu::focal=hk3ds::roomFocalLength(screenWidth,screenHeight);
#endif
        // main.cpp:2095,2270-2301,2477-2483: present the committed simulation.
        // Frame zero deliberately records startup before consuming the first tick.
        HKPSP_PHASE(Plants,if(!game.sceneActive())game.presentPlants());
        HKPSP_PHASE(Ambient,if(!game.sceneActive())game.roomAmbient.present(game.presentationTime(),game.levelSeconds());game.presentAmbient());
        HKPSP_PHASE(Decor,appendDecor(game));
        HKPSP_PROFILE(cpu::change(cpu::Particles);)
        particles.draws=game.decorEffects.visuals.draws;
        for(auto& draw:particles.draws)draw.sprite+=particleSprite;
        for(auto draw:game.ambientParticles.visuals.draws){
            if(game.ambientParticles.visuals.usesRoomTextures){
                if(draw.sprite>=particleSprite)throw std::runtime_error("Room particle sprite outside room atlas");
            }else{
                draw.sprite+=particleSprite;
                if(!activeScene.defaultAmbient||activeScene.ambientZone==5)draw.sprite+=decorSprites;
            }
            particles.draws.push_back(draw);
        }
        HKPSP_PROFILE(cpu::change(cpu::Prepare);)
        HKPSP_PHASE(Motion,terrain.presentMotion(room,game.presentationTime());
            if(room.switchAnimations)game.sceneWalls.presentVisuals(room,game.presentationTime());
            if(worldActorsAdmitted){
                const auto time=game.presentationTime();
                game.shamanPillar.present(time);
                game.cityCrestDust.present(time);
                worldActorArt.present(room,particles,game.worldActorBank(),[&](auto& draw){
                    game.townLift.presentDraw(draw,time);
                    game.jijiDoor.presentDraw(draw,time);
                    game.cityLift.presentDraw(draw,time);
                    game.cityCrest.presentDraw(draw,time);
                });
            });
        HKPSP_PROFILE(cpu::change(cpu::Particles);)
        for(unsigned i=0;i<particles.draws.size();++i)particles.draws[i].owner=i;
        // room_renderer.cpp:390-408: stable bank rank (decor, ambient, actors),
        // then original index for identical authored layer/order/depth.
        std::sort(particles.draws.begin(),particles.draws.end(),[](const auto& a,const auto& b){
            const auto& av=a.values;const auto& bv=b.values;
            if(av[11]!=bv[11])return av[11]<bv[11];
            if(av[12]!=bv[12])return av[12]<bv[12];
            if(av[6]!=bv[6])return av[6]>bv[6];
            return a.owner<b.owner;
        });
        HKPSP_PROFILE(cpu::change(cpu::Prepare);)
        const auto pose=surveyPose?hk3ds::Vec2{surveyPose->x,surveyPose->y}:
            (fixedPose?hk3ds::Vec2{fixedPose->heroX,fixedPose->heroY}:game.heroRenderPosition());
        auto camera=fixedPose?hk3ds::Vec2{fixedPose->cameraX,fixedPose->cameraY}:game.flow.camera;
        if(!fixedPose)camera=slyShopCamera(camera,game.slyShop.open);
        const auto shake=game.decorEffects.shake(),chestShake=game.furyChest.shake();
        const float shakeScale=menus?menus->state.pause.settings.cameraShakeMultiplier():1.f;
        if(surveyPose)camera=pose;
        else {camera.x+=(shake.x+chestShake.x)*shakeScale;camera.y+=(shake.y+chestShake.y)*shakeScale;}
        const auto& h=game.simulation.hero;
        roomLighting.present(list,h.position);
        list.info={menus?menus->presented:frame,source.steps+(menus?menus->controlSteps:0),pose.x,pose.y,camera.x,camera.y,h.velocity.x,h.velocity.y,
                   unsigned(h.actor),unsigned(game.flow.phase),unsigned(h.vitals.health),unsigned(h.vitals.soul),game.geo,game.sceneName(),
                   h.position.x,h.position.y,unsigned(game.abilityPickup.phase),game.flow.transitionFade()};list.quads.clear();
        roomIndex->beginFrame(list,room);
        HKPSP_PROFILE(list.roomRanges.clear();)
        HKPSP_PROFILE(cpu::change(cpu::Other);)
        HKPSP_PHASE(Index,roomIndex->prepare(room,camera,game));
        // Keep the cumulative prefix alive as the texture residency fence.
        // The PSP emits only the appended tail; host parity still submits once.
        const auto submitPass=[&]{
#ifdef __PSP__
            HKPSP_PROFILE(cpu::change(-1);)
            platform.submit(list);
            HKPSP_PROFILE(cpu::change(cpu::Other);)
#endif
        };
        // main.cpp:401-415: both far/background sort groups are composited before
        // near behind-hero draws. Preserve that order even without the blur pass.
        HKPSP_GPU_PASS(list,Far,roomIndex->append(list,room,camera,game.levelSeconds(),0,visibility,&game));
        HKPSP_GPU_PASS(list,Far,roomIndex->append(list,room,camera,game.levelSeconds(),1,visibility,&game));
        submitPass();
        const hk3ds::RoomSortBoundary lightSplit{0,0,lighting.transform[2]};
        HKPSP_GPU_PASS(list,Near,roomIndex->append(list,room,camera,game.levelSeconds(),2,visibility,&game,&lightSplit,false));
        submitPass();
        const auto heroLighting=roomLighting.scene().sample(h.position.x,h.position.y);
        HKPSP_GPU_PASS(list,Lights,appendLight(list,lights,lighting,pose,camera,game.simulation.hero.facingRight,lightPage,0,game.heroLight.scale(),heroLighting.heroRgb,heroLighting.heroAlpha));
        submitPass();
        HKPSP_GPU_PASS(list,Near,roomIndex->append(list,room,camera,game.levelSeconds(),2,visibility,&game,&lightSplit,true));
        submitPass();
        HKPSP_GPU_PASS(list,Lights,(appendLanternGlow(list,lanternGlow,game.darkness,pose,camera,lanternGlowPage),
            appendLight(list,lights,lighting,pose,camera,game.simulation.hero.facingRight,lightPage,2,1)));
        submitPass();
        hk3ds::MonomonRoomDraw monomonDraw;
        game.monomon.capture(monomonDraw);
        // main.cpp:462-628,724-729: actors in the original pass positions.
        HKPSP_GPU_PASS(list,Actors,(appendDoor(list,*actorArt,game,camera),appendGeo(list,*actorArt,game,camera),
            corniferArt.append(list,game.cartographer,camera),worldQuestArt.append(list,game.worldQuests,camera),
            tisoArt.append(list,game.tiso,camera),routeTollArt.append(list,game.routeTolls,camera),
            slyShopArt.append(list,game,camera),
            appendFury(list,*actorArt,game,camera,false),appendCrawlers(list,*actorArt,game,camera,enemySurvey.get()),
            appendShade(list,*actorArt,game,camera),appendBuzzers(list,*actorArt,game,camera,enemySurvey.get()),
            appendGreenpath(list,greenpathArt,game,camera,enemySurvey.get()),appendSceneEnemies(list,sceneEnemyArt,actorArt->crawler,game,camera,enemySurvey.get()),bossArt.append(list,game,camera),
            monomonArt.append(list,monomonDraw,camera),uumuuArt.append(list,textCanvas,game.uumuu,game.presentationTime(),camera),
            appendConditional(list,*actorArt,game,camera),appendStagStation(list,stagArt,game,game.presentationTime(),camera)));
        const auto phase=game.flow.phase;
        if(!surveyPose&&!monomonDraw.maskScene&&game.simulation.hero.actor!=hk3ds::Actor::Dead&&phase!=hk3ds::RoomPhase::Dead&&phase!=hk3ds::RoomPhase::HazardDeath){
            const bool presentation=!fixedPose&&game.flow.presentation.clip!=nullptr;
            HKPSP_GPU_PASS(list,Actors,appendHero(list,hero,fixedPose?captureIdle:(presentation?game.flow.presentation:game.simulation.animator.player),
                       pose,camera,game.simulation.hero.facingRight,heroPage,presentation,game.focusEffects.healFlash()));
        }
        appendFocus(list,hero,heroPage,game,camera,hk3ds::FocusDrawPass::BeforeForeground);
        appendHeroEffects(list,hero,heroPage,game,camera);
        appendSpirits(list,spiritArt,spiritPage,game,camera);
        appendAbilityFlash(list,game.abilityPickup,abilityArt,abilityPage,camera);
        if(game.simulation.hero.actor!=hk3ds::Actor::Dead&&phase!=hk3ds::RoomPhase::Dead&&phase!=hk3ds::RoomPhase::HazardDeath)
            appendSlash(list,hero,heroPage,game,nails,pose,camera);
        // main.cpp:798-807,898-902: hero above near background, below near foreground.
        submitPass();
        const hk3ds::RoomSortBoundary vignetteSplit{13,0,lighting.transform[5]};
        HKPSP_GPU_PASS(list,Near,roomIndex->append(list,room,camera,game.levelSeconds(),3,visibility,&game,&vignetteSplit,false));
        submitPass();
        HKPSP_GPU_PASS(list,Actors,(appendFocus(list,hero,heroPage,game,camera,hk3ds::FocusDrawPass::AfterForeground),
            appendFury(list,*actorArt,game,camera,true)));
        submitPass();
        HKPSP_GPU_PASS(list,Lights,appendLight(list,lights,lighting,pose,camera,game.simulation.hero.facingRight,lightPage,1,game.darkness.scale()));
        submitPass();
        HKPSP_GPU_PASS(list,Near,roomIndex->append(list,room,camera,game.levelSeconds(),3,visibility,&game,&vignetteSplit,true));
        submitPass();
        HKPSP_GPU_PASS(list,Grade,appendCameraGrade(list));
        submitPass();
        HKPSP_PROFILE(cpu::change(cpu::Hud);)
        const float nextHudSeconds=game.levelSeconds();
        if(hudEpoch!=game.audioEpoch){hudEpoch=game.audioEpoch;hud.reset(hudSnapshot());}
        else {const float dt=std::max(0.f,nextHudSeconds-hudSeconds);hud.update(hudSnapshot(),dt);hudClock+=dt;}
        hudSeconds=nextHudSeconds;
        // main.cpp:1015-1018: opening credits before the blanker (blanker covers them).
        HKPSP_GPU_PASS(list,Hud,appendCredits(textCanvas,game.credits,game.crossroadsBoss.active()));
        // main.cpp:1017-1020: opening blanker over the world, under the HUD.
        HKPSP_GPU_PASS(list,Hud,appendOpeningBlanker(list,game.credits.blankerAlpha(),game.crossroadsBoss.active()));
        // Original HudCamera overlays the completed world and final colour effect.
        if(menus&&quitToTitle)appendMenuDim(list);
        // Original FSM: the tablet's "Box Up" sends OUT to HUD Canvas and "Box Down" sends IN. The lore fade
        // covers Opening..Down; the tablet alpha also covers the interrupt path, where inspection jumps to
        // Cooldown (alpha 0) while the tablet is still fading out for .15 s.
        const bool loreUp=game.inspection.alpha()>0||game.prompts.tabletAlpha()>0;
        const bool hudVisible=!monomonDraw.maskScene&&!map.pauses()&&!loreUp&&!dialogueBoxOpen(game)&&(!menus||!menus->state.pause.pspHudHidden());
        const bool largeHud=menus&&menus->state.pause.settings.hudAppearance==hk3ds::SystemSettings::HudAppearance::Large;
#ifndef __PSP__
        DrawList expectedHud;expandRoomGe(list,expectedHud);
        if(hudVisible)appendHud(expectedHud,hudArt,hud,hudClock,nullptr,largeHud);
#endif
        if(hudVisible)HKPSP_GPU_PASS(list,Hud,appendHud(list,hudArt,hud,hudClock,&hudText,largeHud));
#ifndef __PSP__
        DrawList actualHud;expandRoomGe(list,actualHud);
        if(actualHud.quads.size()!=expectedHud.quads.size()||
           (!actualHud.quads.empty()&&std::memcmp(actualHud.quads.data(),expectedHud.quads.data(),actualHud.quads.size()*sizeof(Quad))))
            throw std::runtime_error("Cached/uncached HUD mismatch at frame "+std::to_string(frame));
#endif
        HKPSP_GPU_PASS(list,Hud,appendAbility(list,game.abilityPickup,abilityArt,abilityPage,hudArt,&inventoryArt.bank(),inventoryAtlas.page));
        if(map.view()==MapView::Closed)
            HKPSP_GPU_PASS(list,Hud,appendText(list,textCanvas,*actorArt,game,camera));
        HKPSP_GPU_PASS(list,Hud,appendRoomFades(list,game.flow));
        HKPSP_GPU_PASS(list,Hud,monomonArt.overlay(textCanvas,monomonDraw));
        if(map.showsMap()){
            const bool compass=game.charmInventory.wears(2);
            const auto hero=compass&&mapRoom<hk3ds::savedMapRoomCount?
                hk3ds::mapHeroPosition(hk3ds::savedMapRoom(mapRoom),hk3ds::collisionMapBounds(terrain.world()),
                    game.simulation.hero.position):hk3ds::Vec2{};
            HKPSP_GPU_PASS(list,Hud,appendMap(list,textCanvas.canvas(),mapAtlas.page(),map.view(),
                game.cartographer.progress,mapRoom,hero,compass,map.pan(),regionalMapAtlas.page()));
        }else if(map.view()==MapView::Items||map.view()==MapView::Charms||inventoryPreview!=InventoryPreview::Off){
            const auto& hero=game.simulation.hero;
            const hk3ds::InventorySnapshot inventory{hero.vitals,hero.abilities,game.geo,
                game.playerData().get("hasCityKey")!=0,hero.abilities.spiritLevel()};
            if(map.view()==MapView::Items||map.view()==MapView::Charms){
                auto canvas=textCanvas.canvas();
                const InventoryLabels labels{&canvas,[](void* context,const char* text,inventorylayout::Rect bounds,bool heading){
                    hk3ds::ui::wrappedText(*static_cast<hk3ds::ui::Canvas*>(context),text,
                        {bounds.x,bounds.y,bounds.w,bounds.h},heading?hk3ds::ui::Size::Heading:hk3ds::ui::Size::Body);
                }};
                const auto preview=inventoryPreview;
                const bool charms=map.view()==MapView::Charms;
                inventoryPreview=charms?InventoryPreview::Charms:InventoryPreview::Items;
                HKPSP_GPU_PASS(list,Hud,inventoryArt.append(list,inventory,game.charmInventory,
                    hk3ds::InventoryItem::Nail,charms?game.charmsPage.selected:0u,labels));
                inventoryPreview=preview;
                if(!charms&&game.cartographer.progress.ownsCrossroads())
                    HKPSP_GPU_PASS(list,Hud,canvas.text(canvas.user,"< Map",16,243,hk3ds::ui::Size::Body,hk3ds::ui::white));
            }else inventoryArt.append(list,inventory,game.charmInventory);
        }
        // The pause dim is drawn after the HUD so the HUD is dimmed too, as in the original.
        if(menus&&menus->state.screen==MenuState::Screen::Pause&&!quitToTitle){
            appendMenuDim(list,pauseDimAlpha);
            appendPauseMenu(list,menus->state.pause,menuPage,textCanvas.canvas());
        }
        submitPass();
#ifdef __PSP__
        HKPSP_PROFILE(cpu::change(-1);(void)drawStart;
            for(unsigned i=0;i<=cpu::Clip;++i)perf::current[perf::DrawList]+=cpu::times[i];)
#else
        HKPSP_PROFILE(perf::current[perf::DrawList]=cpu::change(-1)-drawStart;)
#endif
        HKPSP_PROFILE(const auto audioSubmitStart=perf::now();)
        if(audio)audio->update(game,frame);
        HKPSP_PROFILE(perf::current[perf::AudioSubmit]=perf::now()-audioSubmitStart;)
#ifndef __PSP__
        platform.beginFrame();platform.submit(list);
#endif
        if(parity){
#ifndef __PSP__
            DrawList parityExpanded;
            const auto& paritySource=list;
            if(paritySource.roomGe())expandRoomGe(paritySource,parityExpanded);
            const auto& list=paritySource.roomGe()?parityExpanded:paritySource;
#endif
            std::uint64_t hash=14695981039346656037ull;
            const auto add=[&](const auto& value){
                const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
                for(unsigned i=0;i<sizeof(value);++i){hash^=bytes[i];hash*=1099511628211ull;}
            };
            for(const auto& q:list.quads){
                add(q.page);add(q.u0);add(q.v0);add(q.u1);add(q.v1);
                for(const auto& p:q.positions){add(p.x);add(p.y);}
                for(auto v:q.inverseDepth)add(v);
                for(auto v:q.rgba)add(v);
                add(q.whiteFlash);add(q.material);add(q.blend);add(q.order);
            }
            const auto& i=list.info;
            if(std::fprintf(parity.get(),"%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%016llx\n",
                frame,source.steps,unsigned(seed),double(game.levelSeconds()),double(i.heroX),double(i.heroY),
                double(i.cameraX),double(i.cameraY),unsigned(list.quads.size()),
                static_cast<unsigned long long>(hash))<0)throw std::runtime_error("Parity trace write failed");
        }
        const bool filmEnded=game.monomon.cinematicRequested()&&platform.presentMonomonMovie();
        platform.endFrame();
        if(filmEnded)game.monomon.cinematicEnded();
        if(surveying){
            const unsigned missing=hk3ds::animationMissingPlayCount();
            if(std::strcmp(game.sceneName(),roomSurvey[frame].scene)||
               std::fprintf(surveyLog.get(),"%u,%s,%u,%u,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g\n",frame,game.sceneName(),
                   missing-surveyMissing,surveyEntryTicks,double(pose.x),double(pose.y),
                   double(camera.x),double(camera.y),unsigned(game.crossroadsBoss.phase),
                   double(game.crossroadsBoss.position.x),double(game.crossroadsBoss.position.y))<0||
               std::fflush(surveyLog.get())||std::fflush(enemySurvey.get()))throw std::runtime_error("Room survey report failed");
            surveyMissing=missing;
        }
        ++result.frames;
        if(menus)++menus->presented;
        if(quitToTitle)break;
        // main.cpp:2270-2301 / TutorialSession::advance; tuning::controlStep=1/60.
        // Exactly two control updates per locked 30 Hz presentation. Physics retains
        // its own shared fixed-step accumulator; no host wall-clock enters the rules.
        HKPSP_PROFILE(const auto simulationRun=perf::runClock(perf::gameThread);const auto simulationStart=perf::now();)
#ifdef HKPSP_SIM_TRACE
        hk3ds::PerformanceObservation::Ticks simulationTimes{};
        hk3ds::PerformanceObservation::Work simulationWork{};
        hk3ds::performanceObservation.row=&simulationTimes;
        hk3ds::performanceObservation.clock=+[]()->std::uint64_t{return 0;};
        hk3ds::performanceObservation.simulationAccounted=0;
        hk3ds::performanceObservation.simulationWork=&simulationWork;
#endif
        HKPSP_PROFILE(hk3ds::PerformanceObservation::Ticks simulationTimes{};
            hk3ds::PerformanceObservation::Work simulationWork{};
            hk3ds::performanceObservation.row=&simulationTimes;
            hk3ds::performanceObservation.clock=+[]()->std::uint64_t{return perf::now();};
            hk3ds::performanceObservation.simulationAccounted=0;
            hk3ds::performanceObservation.simulationWork=&simulationWork;
            hk3ds::SimulationScope observeTiming(hk3ds::PerformanceSection::SimObserve);)
        HKPSP_PROFILE(unsigned hitsBefore=0,cutsBefore=0;
            for(const auto& o:game.objects)if(o.hasHealth)hitsBefore+=o.health.hits;
            cutsBefore=game.grass.cuts();
            for(const auto& d:game.decor)cutsBefore+=d.broken();
            for(const auto& p:game.plantBreaks)cutsBefore+=p.broken();
            const auto healthBefore=game.simulation.hero.vitals.health+game.simulation.hero.vitals.blueHealth;
            const auto geoBefore=game.geo;)
        if(!fixedPose&&!surveying&&!map.pauses()&&(!menus||menus->state.simulate()))for(unsigned tick=0;tick<2;++tick){
            const auto deathsBefore=game.flow.progress.deaths,retriesBefore=game.flow.progress.retries;
            game.advance(hk3ds::tuning::controlStep,input);
            acceptanceDeaths+=game.flow.progress.deaths-deathsBefore;
            acceptanceRetries+=game.flow.progress.retries-retriesBefore;
            // The host oracle commits a transition on its exact control tick.
            // An odd-tick gate must load before the second tick samples input.
            if(game.flow.transitionReady()){
                if(!std::strcmp(game.flow.transition->scene,"Town"))++acceptanceTownExits;
                const auto gate=*game.flow.transition;loadScene(gate);
            }
        }
        if(!fixedPose&&hk3ds::saveMayStart(game.flow.transition!=nullptr))writeSave(false);
        HKPSP_PROFILE(unsigned hitsAfter=0,cutsAfter=0;
            for(const auto& o:game.objects)if(o.hasHealth)hitsAfter+=o.health.hits;
            cutsAfter=game.grass.cuts();
            for(const auto& d:game.decor)cutsAfter+=d.broken();
            for(const auto& p:game.plantBreaks)cutsAfter+=p.broken();
            perf::current[perf::NailHits]=hitsAfter>=hitsBefore?hitsAfter-hitsBefore:0;
            perf::current[perf::DecorCuts]=cutsAfter>=cutsBefore?cutsAfter-cutsBefore:0;
            const auto healthAfter=game.simulation.hero.vitals.health+game.simulation.hero.vitals.blueHealth;
            perf::current[perf::DamageTaken]=unsigned(std::max(0,healthBefore-healthAfter));
            perf::current[perf::Pickups]=game.geo>=geoBefore?game.geo-geoBefore:0;)
        HKPSP_PROFILE(observeTiming.finish();
            hk3ds::performanceObservation.row=nullptr;hk3ds::performanceObservation.simulationWork=nullptr;
            constexpr hk3ds::PerformanceSection sections[]={hk3ds::PerformanceSection::SimHero,hk3ds::PerformanceSection::SimEnemies,
                hk3ds::PerformanceSection::SimFlow,hk3ds::PerformanceSection::DecorUpdate,hk3ds::PerformanceSection::AmbientUpdate,
                hk3ds::PerformanceSection::SimParticles};
            for(unsigned i=0;i<6;++i)perf::current[perf::SimHero+i]=simulationTimes[unsigned(sections[i])];
            for(unsigned i=0;i<=unsigned(hk3ds::PerformanceSection::SimObserve)-unsigned(hk3ds::PerformanceSection::SimSchedule);++i)
                perf::current[perf::SimSchedule+i]=simulationTimes[unsigned(hk3ds::PerformanceSection::SimSchedule)+i];
            for(unsigned i=0;i<=unsigned(hk3ds::SimulationWork::DecorEmissionSlots);++i)perf::current[perf::SimBodies+i]=simulationWork[i];
            perf::current[perf::SimDecorTerrainTests]=simulationWork[unsigned(hk3ds::SimulationWork::DecorTerrainTests)];
            perf::current[perf::SimDecorTerrainQueries]=simulationWork[unsigned(hk3ds::SimulationWork::DecorTerrainQueries)];)
        HKPSP_PROFILE(perf::current[perf::Simulation]=perf::now()-simulationStart-perf::current[perf::Input];
            perf::current[perf::SimulationRun]=perf::runClock(perf::gameThread)-simulationRun-perf::inputRun;
            const auto bookkeepingStart=perf::now();
            for(unsigned i=0;i<=cpu::Clip;++i)perf::current[perf::DrawListRun]+=cpu::runTimes[i];
            for(unsigned i=cpu::Upload;i<cpu::Phases;++i)perf::current[perf::GuRun]+=cpu::runTimes[i];
            perf::current[perf::Bookkeeping]+=perf::now()-bookkeepingStart;perf::end();)
#ifdef HKPSP_SIM_TRACE
        hk3ds::performanceObservation.row=nullptr;hk3ds::performanceObservation.simulationWork=nullptr;
        const auto stateHash=hk3ds::simulationStateHash(game,terrain,seed);
        if(std::fprintf(simulationCounts.get(),"%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",frame,simulationWork[0],simulationWork[1],
            simulationWork[2],simulationWork[3],simulationWork[4],simulationWork[5],simulationWork[6],
            unsigned(stateHash.value),unsigned(stateHash.value>>32),stateHash.leaves,simulationWork[7],simulationWork[8])<0)
            throw std::runtime_error("Simulation counts write failed");
#endif
#ifdef HKPSP_PERF
        if(stateHashRequested){
            const auto stateHash=hk3ds::simulationStateHash(game,terrain,seed);
            perf::rows[frame][perf::SimHashLo]=unsigned(stateHash.value);
            perf::rows[frame][perf::SimHashHi]=unsigned(stateHash.value>>32);
            perf::rows[frame][perf::SimStateLeaves]=stateHash.leaves;
        }
#endif
#if defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)
        cpu::records[cpu::configIndex][frame].phases=cpu::times;
        cpu::records[cpu::configIndex][frame].calls=cpu::drawCalls;
#endif
    }
    writeSave(true);
    recordSaveState("tutorial-end.json");
    if(audio)audio->finish();
    if(menus)platform.releaseRoomAssets(0);
#ifndef __PSP__
    std::cout<<"Indexed/full-scan bit-identical quad lists: "<<frames<<" frames, "<<frames*4<<" passes\n";
#endif
    std::cout<<"Rendered "<<frames<<" frames; control steps="<<source.steps<<"; pages="<<textures.size()<<"\n";
    result.controlSteps=source.steps;
    if(menus)menus->controlSteps+=source.steps;
    return result;
}
#if (defined(HKPSP_PERF) && !defined(HKPSP_GPU_ATTRIB)) || defined(HKPSP_CPU_ATTRIB)
#ifdef __PSP__
void resetCpu(Platform&);
void captureCpu(Platform&);
#endif
GameRunResult runGame(Platform& platform,unsigned frames){
    GameRunResult result;
    for(unsigned c=0;c<cpu::configCount();++c){
        cpu::configIndex=c;
#ifdef __PSP__
        resetCpu(platform);
#endif
        const auto run=runGameConfig(platform,frames);if(!c)result=run;
#ifdef __PSP__
        captureCpu(platform);cpu::finishConfig();
#endif
    }
#ifdef __PSP__
    // Keep the original baseline report/marker contract for existing tools.
    for(unsigned f=0;f<perf::frames;++f)perf::rows[f]=cpu::records[0][f].row;
    perf::recorded=perf::frames;cpu::write();
#endif
    cpu::configIndex=0;
    return result;
}
#else
GameRunResult runGame(Platform& platform,unsigned frames){
    if(!platform.menusEnabled())return runGameSession(platform,frames);
    MenuRuntime menus;
    menus.loadInput(platform.uiReplay());
    menus.loadSettings(platform.savePath());
    while(menus.available(frames)&&presentTitle(platform,menus,frames)){
        if(menus.available(frames))runGameSession(platform,frames,&menus);
    }
    return {menus.presented,menus.controlSteps};
}
#endif
}
