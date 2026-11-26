#pragma once
#include <array>
#include <cstdint>

namespace hk3ds
{
struct CartographySave
{
    static constexpr std::uint32_t MapCrossroads=1u,EncounteredCrossroads=2u,
        LeftCrossroads=4u,AtHome=8u,MetCornifer=16u,HasQuill=32u;
    static constexpr std::uint32_t validFlags=63u;
    static constexpr unsigned roomCount=256;
    std::uint32_t flags=0;
    std::array<std::uint32_t,8> visited{},mapped{};
    bool ownsCrossroads() const{return (flags&MapCrossroads)!=0;}
    bool quill() const{return (flags&HasQuill)!=0;}
    bool visitedRoom(unsigned room) const{return room<roomCount&&(visited[room/32]&(1u<<(room%32)))!=0;}
    bool mappedRoom(unsigned room) const{return room<roomCount&&(mapped[room/32]&(1u<<(room%32)))!=0;}
    bool visitRoom(unsigned room)
    {
        if(room>=roomCount)return false;
        const auto bit=1u<<(room%32),index=room/32;
        const bool changed=(visited[index]&bit)==0;
        visited[index]|=bit;
        return changed;
    }
    bool mapVisited()
    {
        if(!ownsCrossroads()||!quill())return false;
        bool changed=false;
        for(unsigned i=0;i<visited.size();++i){changed|=(visited[i]&~mapped[i])!=0;mapped[i]|=visited[i];}
        return changed;
    }
    static constexpr unsigned cornifierPrice=30u,iseldaPrice=40u;
    bool buyCrossroads(unsigned& geo,bool iselda=false)
    {
        const unsigned price=iselda?iseldaPrice:cornifierPrice;
        if(ownsCrossroads()||geo<price||(iselda&&!(flags&LeftCrossroads)))return false;
        geo-=price;flags|=MapCrossroads;
        if(!iselda)flags|=MetCornifer;
        return true;
    }
    bool valid() const
    {
        if(flags&~validFlags)return false;
        for(unsigned i=0;i<visited.size();++i)if(mapped[i]&~visited[i])return false;
        return true;
    }
};
static_assert(sizeof(CartographySave)==68,"cartography save layout");
}
