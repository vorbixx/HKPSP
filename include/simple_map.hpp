#pragma once
#include "room.hpp"
#include "cartography.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hk3ds {
// TerrainBank::region can be a broad-phase sentinel, not the room extents.
inline Rect collisionMapBounds(const World& world)
{
    Rect bounds{};bool first=true;
    const auto extend=[&](Vec2 p){
        if(first){bounds={p.x,p.y,p.x,p.y};first=false;return;}
        bounds.left=std::min(bounds.left,p.x);bounds.right=std::max(bounds.right,p.x);
        bounds.bottom=std::min(bounds.bottom,p.y);bounds.top=std::max(bounds.top,p.y);
    };
    for(std::size_t i=0;i<world.shapeCount&&world.vertices;++i){
        const auto& s=world.shapes[i];if(!s.actorContact())continue;
        for(unsigned j=0;j<s.count;++j)extend(world.vertices[s.first+j]);
    }
    for(std::size_t i=0;i<world.count;++i){const auto b=world.solids[i].bounds;extend({b.left,b.bottom});extend({b.right,b.top});}
    return bounds;
}
struct MapLine { Vec2 a{},b{}; };
struct MapGate { Vec2 position{}; std::string scene; };
// Scene adapters supply collision coordinates and an atlas placement. No live
// pointers survive visit(), so unloading a scene cannot invalidate visited rooms.
struct MapRoomSource {
    const char* id=nullptr;
    Rect bounds{};
    World collision{};
    const RoomGate* gates=nullptr;
    std::size_t gateCount=0;
    Vec2 origin{};
};
struct MapRoom {
    std::string id;
    Rect bounds{};
    Vec2 origin{};
    std::vector<MapLine> outlines;
    std::vector<MapGate> gates;
};
class SimpleMap {
public:
    static constexpr std::size_t maxRooms=32,maxLines=1024;
    bool visit(const MapRoomSource& source) {
        if(!source.id||!*source.id)return false;
        for(std::size_t i=0;i<rooms_.size();++i)if(rooms_[i].id==source.id){current_=i;return true;}
        const auto b=source.bounds;
        if(rooms_.size()==maxRooms||!std::isfinite(b.left)||!std::isfinite(b.right)||
           !std::isfinite(b.bottom)||!std::isfinite(b.top)||b.right<=b.left||b.top<=b.bottom)return false;
        MapRoom room;room.id=source.id;room.bounds=b;room.origin=source.origin;
        const float minimum=std::max((b.right-b.left)/288.f,(b.top-b.bottom)/208.f)*.6f;
        const auto add=[&](Vec2 a,Vec2 z){
            // Clip solid contours to the room, including walls extending outside it.
            float lo=0,hi=1;
            const auto clip=[&](float p,float d,float low,float high){
                if(d==0)return p>=low&&p<=high;
                float u=(low-p)/d,v=(high-p)/d;if(u>v)std::swap(u,v);
                lo=std::max(lo,u);hi=std::min(hi,v);return lo<=hi;
            };
            if(!clip(a.x,z.x-a.x,b.left,b.right)||!clip(a.y,z.y-a.y,b.bottom,b.top))return;
            const Vec2 d{z.x-a.x,z.y-a.y};
            const Vec2 first{a.x+lo*d.x,a.y+lo*d.y},last{a.x+hi*d.x,a.y+hi*d.y};
            if(room.outlines.size()<maxLines&&(first.x!=last.x||first.y!=last.y))room.outlines.push_back({first,last});
        };
        const auto& w=source.collision;
        for(std::size_t i=0;i<w.shapeCount&&room.outlines.size()<maxLines;++i){
            const auto& s=w.shapes[i];if(!s.actorContact()||s.count<2||!w.vertices)continue;
            Vec2 previous=w.vertices[s.first];
            const auto end=s.count+(s.kind==0?1u:0u);
            for(unsigned j=1;j<end;++j){
                const Vec2 point=w.vertices[s.first+j%s.count];
                if(j+1==end||std::hypot(point.x-previous.x,point.y-previous.y)>=minimum){add(previous,point);previous=point;}
            }
        }
        for(std::size_t i=0;i<w.count;++i){
            const auto r=w.solids[i].bounds;
            add({r.left,r.bottom},{r.right,r.bottom});add({r.right,r.bottom},{r.right,r.top});
            add({r.right,r.top},{r.left,r.top});add({r.left,r.top},{r.left,r.bottom});
        }
        for(std::size_t i=0;i<source.gateCount&&source.gates;++i){
            const auto& g=source.gates[i];
            room.gates.push_back({{(g.trigger.left+g.trigger.right)*.5f,(g.trigger.bottom+g.trigger.top)*.5f},g.scene});
        }
        rooms_.push_back(std::move(room));current_=rooms_.size()-1;++revision_;return true;
    }
    void cartography(const CartographySave& state) {
        if(cartography_.flags!=state.flags||cartography_.visited!=state.visited||cartography_.mapped!=state.mapped){
            cartography_=state;++revision_;
        }
    }
    const CartographySave& cartographyState() const { return cartography_; }
    const std::vector<MapRoom>& rooms() const { return rooms_; }
    std::size_t current() const { return current_; }
    unsigned revision() const { return revision_; }
    Vec2 project(std::size_t room,Vec2 point) const {
        if(room>=rooms_.size())return {160,120};
        Rect b=rooms_.front().bounds;
        b.left+=rooms_.front().origin.x;b.right+=rooms_.front().origin.x;
        b.bottom+=rooms_.front().origin.y;b.top+=rooms_.front().origin.y;
        for(const auto& r:rooms_){
            b.left=std::min(b.left,r.bounds.left+r.origin.x);b.right=std::max(b.right,r.bounds.right+r.origin.x);
            b.bottom=std::min(b.bottom,r.bounds.bottom+r.origin.y);b.top=std::max(b.top,r.bounds.top+r.origin.y);
        }
        const float scale=std::min(288.f/(b.right-b.left),208.f/(b.top-b.bottom));
        const auto o=rooms_[room].origin;
        return {160+(point.x+o.x-(b.left+b.right)*.5f)*scale,120-(point.y+o.y-(b.bottom+b.top)*.5f)*scale};
    }
private:
    std::vector<MapRoom> rooms_;
    std::size_t current_=0;
    unsigned revision_=0;
    CartographySave cartography_{};
};
}
