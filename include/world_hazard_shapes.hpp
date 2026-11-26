#pragma once
#include "room.hpp"
#include "trigger_polygon.hpp"
#include <cmath>
#include <cstdio>
#include <utility>

namespace hk3ds {
// HKO3 appends an optional PolygonCollider2D path union for every hazard.
// Keep RoomHazard's legacy layout and commit only after all shapes validate.
inline bool readWorldHazardShapes(std::FILE* file,const std::vector<RoomHazard>& hazards,
                                 std::vector<TriggerPolygon>& target)
{
    if(hazards.size()>128)return false;
    std::vector<TriggerPolygon> shapes(hazards.size());
    unsigned total=0;
    const auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
    for(std::size_t i=0;i<hazards.size();++i){
        auto& shape=shapes[i];unsigned paths=0;
        if(!read(&paths,4)||paths>8)return false;
        for(unsigned path=0;path<paths;++path){
            unsigned count=0;
            if(!read(&count,4)||count<3||count>64||total>4096-count)return false;
            const auto first=shape.vertices.size();
            shape.vertices.resize(first+count);
            if(!read(shape.vertices.data()+first,count*sizeof(Vec2)))return false;
            const auto r=hazards[i].trigger;
            for(std::size_t p=first;p<shape.vertices.size();++p){
                const auto v=shape.vertices[p];
                if(!std::isfinite(v.x)||!std::isfinite(v.y)||v.x<r.left||v.x>r.right||v.y<r.bottom||v.y>r.top)return false;
            }
            double area=0;
            for(std::size_t p=first,j=shape.vertices.size()-1;p<shape.vertices.size();j=p++){
                const auto a=shape.vertices[j],b=shape.vertices[p];
                area+=double(a.x)*b.y-double(b.x)*a.y;
            }
            if(std::abs(area)<=1e-8||!simpleTriggerPath(shape.vertices,first,shape.vertices.size()))return false;
            total+=count;shape.ends.push_back(static_cast<unsigned>(shape.vertices.size()));
        }
    }
    target=std::move(shapes);return true;
}
}
