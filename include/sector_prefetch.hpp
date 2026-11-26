#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace hk3ds {
struct SectorTarget { int x=-1,y=-1; };
using SectorTargets=std::array<SectorTarget,4>;
// Scheduling heuristic only: moving axes first, then nearest adjacent cells,
// including diagonals needed just after crossing a corner. At most four banks.
inline SectorTargets sectorPrefetch(int x,int y,float cameraX,float cameraY,float dx,float dy){
    SectorTargets targets{};
    const auto infinity=std::numeric_limits<float>::infinity();
    float tx=infinity,ty=infinity;
    SectorTarget horizontal{std::clamp(x+(dx>0?1:-1),0,16),y};
    SectorTarget vertical{x,std::clamp(y+(dy>0?1:-1),0,12)};
    if(std::abs(dx)>.0001f&&horizontal.x!=x)
        tx=std::max(0.f,dx>0?(x+1)*12-cameraX:cameraX-x*12)/std::abs(dx);
    if(std::abs(dy)>.0001f&&vertical.y!=y)
        ty=std::max(0.f,dy>0?(y+1)*8-cameraY:cameraY-y*8)/std::abs(dy);
    if(tx!=infinity||ty!=infinity){
        if(tx<=ty){targets[0]=horizontal;if(ty!=infinity)targets[1]=vertical;}
        else {targets[0]=vertical;if(tx!=infinity)targets[1]=horizontal;}
    }
    struct Neighbor {SectorTarget target;float distance;};
    std::array<Neighbor,8> neighbors{};unsigned n=0;
    for(int ox=-1;ox<=1;++ox)for(int oy=-1;oy<=1;++oy){
        if(!ox&&!oy)continue;
        const int nx=x+ox,ny=y+oy;
        const float distanceX=std::max({nx*12-cameraX,0.f,cameraX-(nx+1)*12})/12;
        const float distanceY=std::max({ny*8-cameraY,0.f,cameraY-(ny+1)*8})/8;
        neighbors[n++]={{nx,ny},distanceX*distanceX+distanceY*distanceY};
    }
    for(unsigned i=1;i<neighbors.size();++i){
        const auto next=neighbors[i];unsigned j=i;
        while(j&&next.distance<neighbors[j-1].distance){neighbors[j]=neighbors[j-1];--j;}
        neighbors[j]=next;
    }
    unsigned count=targets[0].x<0?0:targets[1].x<0?1:2;
    for(const auto& neighbor:neighbors){
        const auto cell=neighbor.target;
        if(cell.x<0||cell.x>16||cell.y<0||cell.y>12)continue;
        bool present=false;
        for(unsigned i=0;i<count;++i)present|=targets[i].x==cell.x&&targets[i].y==cell.y;
        if(!present)targets[count++]=cell;
        if(count==targets.size())break;
    }
    return targets;
}
}
