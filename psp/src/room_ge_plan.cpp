#include "room_ge.hpp"
#include <cmath>
#include <limits>
#include <map>

namespace hkpsp {
namespace {
using Key=std::array<float,2>;
Key key(const hk3ds::RoomDraw& d){
    const Key result{d.values[11],d.values[6]};
    if(!std::isfinite(result[0])||!std::isfinite(result[1]))
        throw std::invalid_argument("Nonfinite GE sort key");
    return result;
}
RoomQuadCache cache(const hk3ds::RoomVisualBank& room,unsigned draw,bool background){
    const auto& d=room.draws.at(draw);RoomQuadCache q;
    if(!cacheRoomQuad(room.atlas.sprites.at(d.sprite),d,background,room.blurSplitDepth,q))
        throw std::invalid_argument("GE plan source is absent from this pass");
    return q;
}
}
std::size_t RoomGePlan::bytes()const{
    return buffers.bytes()+groups.capacity()*sizeof(RoomGeGroup)+pairs.capacity();
}
unsigned RoomGePlan::entry(unsigned draw)const{
    const auto& entries=buffers.ordered;
    const auto found=std::lower_bound(entries.begin(),entries.end(),draw,
        [](const auto& e,unsigned d){return e.draw<d;});
    return found==entries.end()||found->draw!=draw?~0u:static_cast<unsigned>(found-entries.begin());
}
const RoomGeGroup* RoomGePlan::group(unsigned index)const{
    const auto found=std::upper_bound(groups.begin(),groups.end(),index,
        [](unsigned i,const auto& g){return i<g.first;});
    if(found==groups.begin())return nullptr;
    const auto& g=*(found-1);return index-g.first<g.count?&g:nullptr;
}
RoomGeOrderView RoomGePlan::order(const RoomGeGroup& g)const{
    const auto bytes=(std::size_t(g.count)*(g.count?g.count-1:0)/2+3)/4;
    if(g.pairs>pairs.size()||bytes>pairs.size()-g.pairs)
        throw std::out_of_range("Invalid pooled GE order");
    return {g.count,bytes?pairs.data()+g.pairs:nullptr,bytes};
}
RoomGePlan buildRoomGePlan(const hk3ds::RoomVisualBank& room,
    std::vector<RoomGeSource> sources,bool behindHero,bool background,float focal){
    if(!std::isfinite(focal)||focal<=0)throw std::invalid_argument("Invalid GE focal length");
    RoomGePlan result;result.behindHero=behindHero;result.background=background;result.focal=focal;
    struct Group {std::vector<unsigned> ids;bool exact=false;};
    std::map<Key,Group> groups;
    unsigned previous=~0u;
    for(unsigned i=0;i<sources.size();++i){
        const auto& s=sources[i];const auto& d=room.draws.at(s.draw);
        if((previous!=~0u&&s.draw<=previous)||hk3ds::roomDrawBehindHero(d)!=behindHero)
            throw std::invalid_argument("Invalid ordered GE pass sources");
        auto& g=groups[key(d)];
        g.exact|=s.exact||(!g.ids.empty()&&g.ids.back()+1!=i);
        g.ids.push_back(i);previous=s.draw;
    }
    // A missing immutable source is a potential mutable peer. It can disappear
    // this frame, so mark its whole key before visibility or opacity filtering.
    std::size_t source=0;
    for(unsigned i=0;i<room.draws.size();++i){
        if(source<sources.size()&&sources[source].draw==i){++source;continue;}
        const auto& d=room.draws[i];
        if(hk3ds::roomDrawBehindHero(d)!=behindHero)continue;
        const auto found=groups.find(key(d));if(found==groups.end())continue;
        RoomQuadCache q;
        if(cacheRoomQuad(room.atlas.sprites.at(d.sprite),d,background,room.blurSplitDepth,q))found->second.exact=true;
    }
    for(auto& item:groups){
        auto& g=item.second;
        if(!g.exact){
            std::vector<RoomQuadCache> caches;caches.reserve(g.ids.size());
            auto camera=sources[g.ids.front()].camera;
            for(unsigned id:g.ids){
                const auto& s=sources[id];caches.push_back(cache(room,s.draw,background));
                g.exact|=classifyRoomGe(caches.back(),s.camera,focal)!=RoomGeReason::Eligible;
                camera.left=std::min(camera.left,s.camera.left);camera.bottom=std::min(camera.bottom,s.camera.bottom);
                camera.right=std::max(camera.right,s.camera.right);camera.top=std::max(camera.top,s.camera.top);
            }
            if(!g.exact){
                std::vector<const Quad*> quads;quads.reserve(caches.size());
                for(const auto& q:caches)quads.push_back(&q.world);
                const auto order=buildRoomGeOrder(quads,camera,focal);g.exact=order.needsExact;
                bool skip=false,match=false;
                for(unsigned a=0;a<order.count;++a)for(unsigned b=0;b<a;++b){
                    const auto relation=order.relation(a,b);
                    skip|=relation==RoomGeRelation::Skip;match|=relation==RoomGeRelation::Match;
                }
                // Moving past a record requires a Skip before a Match. With
                // fewer than three records, insertion also cannot change order.
                if(!g.exact&&order.count>=3&&skip&&match){
                    if(result.pairs.size()>std::numeric_limits<unsigned>::max())
                        throw std::overflow_error("GE order pool exceeds its offset range");
                    result.groups.push_back({g.ids.front(),order.count,static_cast<unsigned>(result.pairs.size())});
                    result.pairs.insert(result.pairs.end(),order.pairs.begin(),order.pairs.end());
                }
            }
        }
        if(g.exact)for(unsigned id:g.ids)sources[id].exact=true;
    }
    std::sort(result.groups.begin(),result.groups.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<RoomGeGroup>(result.groups).swap(result.groups);
    std::vector<std::uint8_t>(result.pairs).swap(result.pairs);
    result.buffers=buildRoomGeBuffers(room,sources,background,focal,1);
    if(result.buffers.ordered.size()!=sources.size())throw std::logic_error("GE plan lost singleton entries");
    return result;
}
}
