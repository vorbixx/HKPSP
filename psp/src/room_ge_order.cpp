#include "room_ge.hpp"
#include "perf.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hkpsp {
namespace {
bool simple(const Quad& q){
    return q.blend!=Blend::MaskAlpha&&q.blend!=Blend::ClampedAdd&&
        q.blend!=Blend::GradeGreen&&q.blend!=Blend::GradeRed&&q.blend!=Blend::GradeBlue&&
        q.whiteFlash<=0&&q.material!=hk3ds::RoomMaterial::LinearLight&&
        q.material!=hk3ds::RoomMaterial::AmbientAdditive;
}
struct Bounds {float left,right,bottom,top;};
Bounds bounds(const Quad& q){
    Bounds b{q.positions[0].x,q.positions[0].x,q.positions[0].y,q.positions[0].y};
    for(const auto p:q.positions){
        b.left=std::min(b.left,p.x);b.right=std::max(b.right,p.x);
        b.bottom=std::min(b.bottom,p.y);b.top=std::max(b.top,p.y);
    }
    return b;
}
// Absolute error from the real affine expression through the four binary32
// operations in projectRoomQuad. 2*epsilon is four times the round-to-nearest
// unit error; the extra slack also covers rounding these bounds in double.
// Each step propagates the previous error and includes subnormal rounding.
double error(float world,double camera,float focal,float inverse,double centre){
    constexpr double e=2*std::numeric_limits<float>::epsilon();
    constexpr double tiny=std::numeric_limits<float>::denorm_min();
    double magnitude=std::fabs(double(world))+camera;
    double result=e*magnitude+tiny;
    const auto overflow=[&](){return magnitude+result>std::numeric_limits<float>::max();};
    if(overflow())return std::numeric_limits<double>::infinity();
    for(double scale:{double(focal),double(inverse)}){
        magnitude*=scale;result*=scale;result+=e*(magnitude+result)+tiny;
        if(overflow())return std::numeric_limits<double>::infinity();
    }
    result+=e*(centre+magnitude+result)+tiny;
    if(!std::isfinite(result)||centre+magnitude+result>std::numeric_limits<float>::max())
        return std::numeric_limits<double>::infinity();
    return result;
}
bool separated(float low,float high,double camera,float focal,float inverse,double centre){
    if(!(low<high))return false;
    const double gap=(double(high)-double(low))*double(focal)*double(inverse);
    return gap>error(low,camera,focal,inverse,centre)+error(high,camera,focal,inverse,centre);
}
RoomGeRelation compile(const Quad& a,const Quad& b,const hk3ds::RoomCameraBounds& camera,float focal){
    if(!simple(a)||!simple(b)||a.blend!=b.blend)return RoomGeRelation::Stop;
    if(sameBatchState(a,b))return RoomGeRelation::Match;
    const float inverse=a.inverseDepth[0];
    if(!(inverse>0)||!std::isfinite(inverse))return RoomGeRelation::Exact;
    for(unsigned i=0;i<4;++i){
        if(a.inverseDepth[i]!=inverse||b.inverseDepth[i]!=inverse||
           !std::isfinite(a.positions[i].x)||!std::isfinite(a.positions[i].y)||
           !std::isfinite(b.positions[i].x)||!std::isfinite(b.positions[i].y))return RoomGeRelation::Exact;
    }
    const auto x=bounds(a),y=bounds(b);
    // With identical positive reciprocal depth the projection is monotonic in
    // world coordinates. Overlapping world bounds therefore always overlap,
    // including touching edges and float plateaus; no error estimate is needed.
    if(!(x.right<y.left||y.right<x.left||x.top<y.bottom||y.top<x.bottom))return RoomGeRelation::Stop;
    const double cx=std::max(std::fabs(double(camera.left)),std::fabs(double(camera.right)));
    const double cy=std::max(std::fabs(double(camera.bottom)),std::fabs(double(camera.top)));
    if(separated(x.right,y.left,cx,focal,inverse,screenWidth*.5)||
       separated(y.right,x.left,cx,focal,inverse,screenWidth*.5)||
       separated(x.top,y.bottom,cy,focal,inverse,screenHeight*.5)||
       separated(y.top,x.bottom,cy,focal,inverse,screenHeight*.5))return RoomGeRelation::Skip;
    // A tiny world-space gap can collapse after subtraction/scale/addition.
    // Never turn that uncertain overlap barrier into an unconditional skip.
    return RoomGeRelation::Exact;
}
std::size_t pairIndex(unsigned a,unsigned b){
    if(a<b)std::swap(a,b);
    return std::size_t(a)*(a-1)/2+b;
}
}
RoomGeRelation RoomGeOrderView::relation(unsigned a,unsigned b)const{
    HKPSP_PROFILE(++perf::current[perf::RoomGeOrderTests];)
    if(a>=count||b>=count||a==b)throw std::out_of_range("Invalid GE order pair");
    const auto index=pairIndex(a,b);
    if(index/4>=bytes||!pairs)throw std::out_of_range("Invalid GE order storage");
    return static_cast<RoomGeRelation>((pairs[index/4]>>(2*(index%4)))&3);
}
RoomGeRelation RoomGeOrder::relation(unsigned a,unsigned b)const{
    return view().relation(a,b);
}
RoomGeOrder buildRoomGeOrder(const std::vector<const Quad*>& quads,
    const hk3ds::RoomCameraBounds& camera,float focal){
    if(!std::isfinite(focal)||focal<=0||!std::isfinite(camera.left)||!std::isfinite(camera.right)||
       !std::isfinite(camera.bottom)||!std::isfinite(camera.top)||camera.left>camera.right||camera.bottom>camera.top||
       quads.size()>65536)throw std::invalid_argument("Invalid GE order domain");
    RoomGeOrder result;result.count=static_cast<unsigned>(quads.size());
    const auto count=std::size_t(result.count)*(result.count?result.count-1:0)/2;
    result.pairs.resize((count+3)/4);
    for(unsigned a=0;a<result.count;++a){
        if(!quads[a])throw std::invalid_argument("Missing GE order geometry");
        for(unsigned b=0;b<a;++b){
            const auto value=compile(*quads[a],*quads[b],camera,focal);
            result.needsExact|=value==RoomGeRelation::Exact;
            const auto index=pairIndex(a,b);
            result.pairs[index/4]|=static_cast<unsigned>(value)<<(2*(index%4));
        }
    }
    return result;
}
bool reorderRoomGeTail(std::vector<unsigned>& ids,std::size_t first,const RoomGeOrder& order){
    return reorderRoomGeTail(ids,first,order.view(),[](unsigned id){return id;});
}
}
