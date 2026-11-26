#include "room_visual.hpp"
#include "diagnostic_game_flags.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>
#include <bitset>
#include <string>
namespace hk3ds
{
namespace {
std::array<float,16> grassValues(const RoomDraw& draw,float k)
{
    auto v=draw.values;
    if(k==0)return v;
    for(const auto row: {std::array<unsigned,3>{0,1,2},
                         std::array<unsigned,3>{3,4,5},
                         std::array<unsigned,3>{13,14,6}}){
        v[row[1]]+=k*v[row[0]];
        v[row[2]]+=k*draw.grass.height*v[row[0]];
    }
    return v;
}
}
float RoomScaleAnimation::sample(float seconds) const
{
    if(keys.size()<2||!std::isfinite(seconds))return 1;
    const float duration=keys.back().seconds;
    float time=std::fmod(seconds,duration);if(time<0)time+=duration;
    if(time>=duration)time=0; // A tiny negative time can round up to the endpoint.
    const auto end=std::upper_bound(keys.begin(),keys.end(),time,
        [](float t,const RoomScaleKey& key){return t<key.seconds;});
    const auto& b=*end;const auto& a=*(end-1);
    const float dt=b.seconds-a.seconds,u=(time-a.seconds)/dt,u2=u*u,u3=u2*u;
    return (2*u3-3*u2+1)*a.value+(u3-2*u2+u)*dt*a.outSlope+
           (-2*u3+3*u2)*b.value+(u3-u2)*dt*b.inSlope;
}
std::array<float,16> roomDrawValues(const RoomDraw& draw,float seconds,const RoomScaleAnimation* animation)
{
    if(animation){
        auto values=draw.values;const float scale=animation->sample(seconds);
        for(unsigned i:{0u,1u,3u,4u})values[i]*=scale;
        return values;
    }
    if(draw.grass.amplitude==0&&draw.grass.push==0)return draw.values;
    return grassValues(draw,draw.grass.amplitude*std::sin(seconds*draw.grass.speed+draw.grass.phase)+draw.grass.push);
}
bool roomFlatSprite(const SpriteFrame& sprite,const std::array<float,16>& v,
                    float cameraX,float cameraY,float width,float height,float focal,
                    RoomFlatSprite& result)
{
    const float depth=38.1f+v[6];
    if(v[1]!=0||v[3]!=0||v[13]!=0||v[14]!=0||depth<10||depth>1000||!sprite.width||!sprite.height)return false;
    const float scale=focal/depth;
    // C2D uses absolute rectangle dimensions and flips only UVs for signed
    // scales. Anchor at the geometric top-left even when an axis is mirrored.
    result={width*.5f+scale*(v[2]-cameraX+v[0]*(v[0]<0?sprite.right:sprite.left)),
            height*.5f-scale*(v[5]-cameraY+v[4]*(v[4]<0?sprite.bottom:sprite.top)),
            scale*v[0]*(sprite.right-sprite.left)/sprite.width,
            scale*v[4]*(sprite.top-sprite.bottom)/sprite.height,
            width*.5f+scale*(v[2]-cameraX+v[0]*(v[0]<0?sprite.left:sprite.right)),
            height*.5f-scale*(v[5]-cameraY+v[4]*(v[4]<0?sprite.top:sprite.bottom))};
    return true;
}
bool roomRotatedSprite(const SpriteFrame& sprite,const std::array<float,16>& v,
                       float cameraX,float cameraY,float width,float height,float focal,
                       RoomRotatedSprite& result)
{
    const float depth=38.1f+v[6];
    if(v[13]!=0||v[14]!=0||depth<10||depth>1000||!sprite.width||!sprite.height)return false;
    const float xx=v[0]*v[0]+v[3]*v[3],yy=v[1]*v[1]+v[4]*v[4];
    const float dot=v[0]*v[1]+v[3]*v[4];
    // A shared C2D view supports rotation/reflection, but cannot represent shear.
    if(!(xx>0&&yy>0)||dot*dot>1e-12f*xx*yy)return false;
    const float lengthX=std::sqrt(xx);
    const float scale=focal/depth;
    const float x=(sprite.left+sprite.right)*.5f,y=(sprite.bottom+sprite.top)*.5f;
    result={width*.5f+scale*(v[2]-cameraX+v[0]*x+v[1]*y),
            height*.5f-scale*(v[5]-cameraY+v[3]*x+v[4]*y),
            scale*lengthX*(sprite.right-sprite.left),
            scale*(v[0]*v[4]-v[1]*v[3])/lengthX*(sprite.top-sprite.bottom),
            std::atan2(-v[3],v[0])};
    return true;
}
float roomFocalLength(float width,float height)
{
    const float aspect=std::fmax(1.6f,width/height);
    const float fov=24.0f*std::fmax(1.0f,(16.0f/9.0f)/aspect);
    return height/(2*std::tan(fov*3.14159265358979323846f/360));
}
bool roomEffectClipSprite(const SpriteFrame& sprite,const RoomDraw& draw,
                          bool background,RoomSpriteClip& result,float splitDepth)
{
    result={sprite};
    const auto& v=draw.values;
    if(v[13]==0&&v[14]==0)return (v[6]>=splitDepth)==background;
    float low=std::numeric_limits<float>::infinity(),high=-low;
    for(float x:{sprite.left,sprite.right})for(float y:{sprite.bottom,sprite.top}){
        const float z=v[6]+v[13]*x+v[14]*y;
        low=std::min(low,z);high=std::max(high,z);
    }
    if(low>=splitDepth)return background;
    if(high<=splitDepth)return !background;
    if(draw.grass.amplitude!=0||draw.grass.push!=0)return false;
    if(v[13]!=0&&v[14]!=0){
        if(low<=-38.1f)return false;
        // Zview=-W+scale*(Zlocal-split). After Citro2D's orthographic
        // projection, the near plane clips exactly the unwanted halfspace.
        // Bound the retained side inside the far plane at every quad corner.
        const float scale=(background?1.f:-1.f)*.5f*(38.1f+low)/
            std::max(std::fabs(low-splitDepth),std::fabs(high-splitDepth));
        result.depthClip={(scale-1)*v[13],(scale-1)*v[14],
                          scale*(v[6]-splitDepth)-(38.1f+v[6])};
        result.homogeneousClip=true;
        return true;
    }
    const bool horizontal=v[13]!=0;
    const float slope=horizontal?v[13]:v[14];
    const float cut=(splitDepth-v[6])/slope;
    const bool keepHigh=background==(slope>0);
    auto& clipped=result.sprite;
    if(horizontal){
        if(keepHigh)clipped.left=cut;else clipped.right=cut;
        result.left=(clipped.left-sprite.left)/(sprite.right-sprite.left);
        result.right=(clipped.right-sprite.left)/(sprite.right-sprite.left);
    }else{
        if(keepHigh)clipped.bottom=cut;else clipped.top=cut;
        result.top=(sprite.top-clipped.top)/(sprite.top-sprite.bottom);
        result.bottom=(sprite.top-clipped.bottom)/(sprite.top-sprite.bottom);
    }
    return clipped.left<clipped.right&&clipped.bottom<clipped.top;
}
bool roomDrawBehindHero(const RoomDraw& draw)
{return roomDrawBefore(draw,0,0,.004f);}
bool roomDrawBefore(const RoomDraw& draw,float layer,float order,float depth)
{
    const auto& v=draw.values;
    if(v[11]!=layer)return v[11]<layer;
    if(v[12]!=order)return v[12]<order;
    return v[6]>=depth;
}
RoomDrawRange roomDrawRange(const std::vector<RoomDraw>& draws,const RoomSortBoundary* split,bool after)
{
    if(!split)return {0,draws.size()};
    const auto cut=std::size_t(std::partition_point(draws.begin(),draws.end(),[&](const RoomDraw& draw){
        return roomDrawBefore(draw,split->layer,split->order,split->depth);
    })-draws.begin());
    return after?RoomDrawRange{cut,draws.size()}:RoomDrawRange{0,cut};
}
bool roomParticleCameraVisible(const SpriteFrame& s,const RoomDraw& draw,
    float x,float y,float width,float height,float focal)
{
    const auto& v=draw.values;
    const float depth=38.1f+v[6];
    if(v[13]!=0||v[14]!=0||draw.grass.amplitude!=0||draw.grass.push!=0||
       !(depth>=10&&depth<=1000))return true;
    const float ax=v[0]*s.left,bx=v[0]*s.right,ay=v[1]*s.bottom,by=v[1]*s.top;
    const float cx=v[3]*s.left,dx=v[3]*s.right,cy=v[4]*s.bottom,dy=v[4]*s.top;
    // Two pixels of slack cover reassociation versus final corner projection.
    const float halfWidth=(width*.5f+2)*depth/focal,halfHeight=(height*.5f+2)*depth/focal;
    return !(x<v[2]+std::min(ax,bx)+std::min(ay,by)-halfWidth||
             x>v[2]+std::max(ax,bx)+std::max(ay,by)+halfWidth||
             y<v[5]+std::min(cx,dx)+std::min(cy,dy)-halfHeight||
             y>v[5]+std::max(cx,dx)+std::max(cy,dy)+halfHeight);
}
RoomCameraBounds roomCameraBounds(const SpriteFrame& sprite,const RoomDraw& draw,float width,float height,const RoomScaleAnimation* animation)
{
    const float focal=roomFocalLength(width,height);
    const float inf=std::numeric_limits<float>::infinity();
    RoomCameraBounds bounds{inf,inf,-inf,-inf};
    for(float scale:{animation?animation->minimum:1.f,
                     animation?animation->maximum:1.f})
    for(float k:{draw.grass.push-std::fabs(draw.grass.amplitude),
                 draw.grass.push+std::fabs(draw.grass.amplitude)}){
    auto v=grassValues(draw,k);
    for(unsigned i:{0u,1u,3u,4u})v[i]*=scale;
    for(float x:{sprite.left,sprite.right})for(float y:{sprite.bottom,sprite.top}){
        const float depth=std::max(10.f,38.1f+v[6]+v[13]*x+v[14]*y);
        const float wx=v[0]*x+v[1]*y+v[2],wy=v[3]*x+v[4]*y+v[5];
        // One screen pixel of slack keeps the early reject conservative despite
        // reassociation/rounding relative to the renderer's projected corners.
        bounds.left=std::min(bounds.left,wx-(width*.5f+1)*depth/focal);
        bounds.right=std::max(bounds.right,wx+(width*.5f+1)*depth/focal);
        bounds.bottom=std::min(bounds.bottom,wy-(height*.5f+1)*depth/focal);
        bounds.top=std::max(bounds.top,wy+(height*.5f+1)*depth/focal);
    }
    }
    // Round outwards so near-edge float cancellation cannot discard a pixel.
    bounds.left=std::nextafter(bounds.left,-inf);bounds.bottom=std::nextafter(bounds.bottom,-inf);
    bounds.right=std::nextafter(bounds.right,inf);bounds.top=std::nextafter(bounds.top,inf);
    return bounds;
}
namespace {
bool loadScaleAnimation(const char* path,const std::vector<RoomDraw>& draws,std::shared_ptr<const RoomScaleAnimation>& result)
{
    const std::string name=std::string(path)+".scale";
    FILE* file=std::fopen(name.c_str(),"rb");if(!file)return true;
    char magic[4]{};std::uint32_t counts[2]{};
    bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"HKS1",4)==0&&
        std::fread(counts,4,2,file)==2&&counts[0]>=2&&counts[0]<=256&&counts[1]>0&&counts[1]<=16384;
    if(!ok){std::fclose(file);return false;}
    const long expected=12L+16L*counts[0]+4L*counts[1];
    ok=ok&&std::fseek(file,0,SEEK_END)==0&&std::ftell(file)==expected&&std::fseek(file,12,SEEK_SET)==0;
    if(!ok){std::fclose(file);return false;}
    auto curve=std::make_shared<RoomScaleAnimation>();curve->keys.resize(counts[0]);
    for(auto& key:curve->keys){
        ok=ok&&std::fread(&key,4,4,file)==4;
        ok=ok&&std::isfinite(key.seconds)&&std::isfinite(key.value)&&std::isfinite(key.inSlope)&&std::isfinite(key.outSlope)&&
           key.seconds>=0&&key.seconds<=3600&&key.value>0&&key.value<=16&&std::fabs(key.inSlope)<=1000&&std::fabs(key.outSlope)<=1000;
    }
    ok=ok&&curve->keys.front().seconds==0&&curve->keys.back().seconds>0;
    for(unsigned i=1;ok&&i<curve->keys.size();++i){
        const auto& a=curve->keys[i-1];const auto& b=curve->keys[i];const float dt=b.seconds-a.seconds;
        ok=dt>0;
        // Cubic Bezier control hull conservatively bounds the Hermite segment.
        for(float value:{a.value,b.value,a.value+a.outSlope*dt/3,b.value-b.inSlope*dt/3}){
            curve->minimum=std::min(curve->minimum,value);curve->maximum=std::max(curve->maximum,value);
        }
    }
    ok=ok&&curve->minimum>0&&curve->maximum<=16;
    std::vector<std::uint32_t> owners(counts[1]);
    ok=ok&&std::fread(owners.data(),4,owners.size(),file)==owners.size()&&std::fgetc(file)==EOF;
    std::fclose(file);
    std::sort(owners.begin(),owners.end());
    ok=ok&&std::adjacent_find(owners.begin(),owners.end())==owners.end();
    if(!ok)return false;
    std::vector<std::uint32_t> matched;
    for(auto& draw:draws)if(std::binary_search(owners.begin(),owners.end(),draw.owner)){
        if(draw.grass.amplitude!=0||draw.grass.push!=0||draw.values[13]!=0||draw.values[14]!=0)return false;
        matched.push_back(draw.owner);
    }
    std::sort(matched.begin(),matched.end());
    matched.erase(std::unique(matched.begin(),matched.end()),matched.end());
    if(matched!=owners)return false;
    curve->owners=std::move(owners);result=std::move(curve);
    return true;
}
}
namespace {
constexpr std::uint32_t emptySwitchSprite=0xffffffffu;
bool loadSwitchAnimations(const char* path,const std::vector<RoomDraw>& draws,std::size_t spriteCount,
                          std::shared_ptr<const std::vector<RoomSwitchAnimation>>& result)
{
    const std::string name=std::string(path)+".switches";
    FILE* file=std::fopen(name.c_str(),"rb");if(!file)return true;
    char magic[4]{};std::uint32_t count=0;
    bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"MGA1",4)==0&&
        std::fread(&count,4,1,file)==1&&count>0&&count<=64;
    auto animations=std::make_shared<std::vector<RoomSwitchAnimation>>();
    std::vector<bool> used(draws.size(),false);
    for(std::uint32_t i=0;ok&&i<count;++i){
        RoomSwitchAnimation animation;std::uint32_t frames=0;
        ok=std::fread(&animation.trigger,4,1,file)==1&&std::fread(&animation.slots,4,1,file)==1&&
            std::fread(&frames,4,1,file)==1&&std::fread(&animation.alpha,4,1,file)==1&&
            std::fread(animation.draws.data(),4,4,file)==4&&std::fread(animation.initial.data(),4,4,file)==4;
        ok=ok&&animation.slots>0&&animation.slots<=4&&frames>0&&frames<=256&&
            std::isfinite(animation.alpha)&&animation.alpha>=0&&animation.alpha<=1&&
            std::any_of(draws.begin(),draws.end(),[&](const RoomDraw& d){return d.owner==animation.trigger;});
        const auto validSprites=[&](const std::array<std::uint32_t,4>& sprites){
            bool any=false;
            for(unsigned j=0;j<4;++j){
                if(j>=animation.slots){if(sprites[j]!=emptySwitchSprite)return false;}
                else if(sprites[j]!=emptySwitchSprite){if(sprites[j]>=spriteCount)return false;any=true;}
            }
            return any;
        };
        ok=ok&&validSprites(animation.initial);
        std::uint32_t owner=0;
        for(unsigned j=0;ok&&j<4;++j){
            const auto index=animation.draws[j];
            if(j>=animation.slots){ok=index==emptySwitchSprite;continue;}
            ok=index<draws.size()&&!used[index];if(!ok)break;
            used[index]=true;const auto& draw=draws[index];
            if(j==0)owner=draw.owner;
            const auto sprite=animation.initial[j];
            ok=draw.owner==owner&&draw.grass.amplitude==0&&draw.grass.push==0&&
                (sprite==emptySwitchSprite?draw.values[10]==0:draw.sprite==sprite&&draw.values[10]==animation.alpha);
        }
        for(unsigned j=0;ok&&j<frames;++j){
            RoomSwitchFrame frame;
            ok=std::fread(&frame.seconds,4,1,file)==1&&std::fread(frame.sprites.data(),4,4,file)==4&&
                std::isfinite(frame.seconds)&&frame.seconds>=0&&frame.seconds<=60&&validSprites(frame.sprites)&&
                (animation.frames.empty()||frame.seconds>animation.frames.back().seconds);
            if(ok)animation.frames.push_back(frame);
        }
        if(ok)animations->push_back(std::move(animation));
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(ok)result=std::move(animations);
    return ok;
}
}
bool RoomVisualBank::loadSwitches(const char* path)
{
    switchAnimations.reset();
    return loadSwitchAnimations(path,draws,atlas.sprites.size(),switchAnimations);
}
void RoomVisualBank::extendSwitchBounds(float width,float height)
{
    if(!switchAnimations)return;
    for(const auto& animation:*switchAnimations)for(unsigned slot=0;slot<animation.slots;++slot){
        const auto index=animation.draws[slot];auto& bounds=cameraBounds[index];
        const auto include=[&](std::uint32_t sprite){
            if(sprite==emptySwitchSprite)return;
            const auto next=roomCameraBounds(atlas.sprites[sprite],draws[index],width,height);
            bounds.left=std::min(bounds.left,next.left);bounds.bottom=std::min(bounds.bottom,next.bottom);
            bounds.right=std::max(bounds.right,next.right);bounds.top=std::max(bounds.top,next.top);
        };
        include(animation.initial[slot]);for(const auto& frame:animation.frames)include(frame.sprites[slot]);
    }
}
void RoomVisualBank::presentSwitches(std::uint32_t trigger,float seconds,bool active)
{
    if(!switchAnimations)return;
    for(const auto& animation:*switchAnimations){
        if(animation.trigger!=trigger)continue;
        const auto* sprites=&animation.initial;
        if(active&&std::isfinite(seconds)){
            const auto frame=std::upper_bound(animation.frames.begin(),animation.frames.end(),seconds,
                [](float time,const RoomSwitchFrame& f){return time<f.seconds;});
            if(frame!=animation.frames.begin())sprites=&(frame-1)->sprites;
        }
        for(unsigned j=0;j<animation.slots;++j){
            auto& draw=draws[animation.draws[j]];const auto sprite=(*sprites)[j];
            draw.values[10]=sprite==emptySwitchSprite?0:animation.alpha;
            if(sprite!=emptySwitchSprite)draw.sprite=sprite;
        }
    }
}
bool RoomVisualBank::load(const char* path)
{
    atlas=AnimationBank{};draws.clear();cameraBounds.clear();scaleCurve.reset();switchAnimations.reset();blurSplitDepth=roomBlurSplitDepth;
    FILE* file=std::fopen(path,"rb");
    if (!file) return false;
    char magic[4]{};std::uint32_t header[4]{};
    bool ok=std::fread(magic,1,4,file)==4;
    const bool roomSplitFormat=ok&&std::memcmp(magic,"HKR4",4)==0;
    const bool grassFormat=roomSplitFormat||(ok&&std::memcmp(magic,"HKR3",4)==0);
    const bool extended=grassFormat||(ok&&std::memcmp(magic,"HKR2",4)==0);
    ok=ok && (extended||std::memcmp(magic,"HKR1",4)==0) &&
            std::fread(header,4,4,file)==4 && header[0]>0 && header[0]<=roomTextureBudget/roomTexturePageBytes &&
            header[1]>0 && header[1]<=4096 && header[2]>0 && header[2]<=16384 &&
            (grassFormat?header[3]<=header[2]:header[3]==0);
    if (!ok) {std::fclose(file);return false;}
    // Reject inconsistent counts before allocating. Worker metadata input is bounded;
    // its allocation must be bounded by the actual file too.
    const long sparseOffset=20L+36L*header[1]+(extended?72L:56L)*header[2];
    const long expected=sparseOffset+24L*header[3]+(roomSplitFormat?4L:0L);
    ok=std::fseek(file,0,SEEK_END)==0 && std::ftell(file)==expected &&
       std::fseek(file,sparseOffset,SEEK_SET)==0;
    // Sparse metadata is validated before any variable-size allocation.
    std::bitset<16384> grassSeen;
    for(unsigned i=0;ok&&i<header[3];++i){
        std::uint32_t index=0;float values[5]{};
        ok=std::fread(&index,4,1,file)==1&&std::fread(values,4,5,file)==5&&index<header[2];
        if(ok){ok=!grassSeen[index];grassSeen.set(index);}
        for(float value:values)ok=ok&&std::isfinite(value)&&std::fabs(value)<=100000;
    }
    ok=ok&&std::fseek(file,20,SEEK_SET)==0;
    if (!ok) {std::fclose(file);return false;}
    std::vector<SpriteFrame> sprites(header[1]);std::vector<RoomDraw> next(header[2]);
    for (auto& sprite:sprites)
    {
        std::uint32_t integers[5]{};float geometry[4]{};
        ok=ok && std::fread(integers,4,5,file)==5 && std::fread(geometry,4,4,file)==4;
        for (float v:geometry) ok=ok && std::isfinite(v) && std::fabs(v)<=100000;
        ok=ok && integers[0]<header[0] && integers[1]<=1024 && integers[2]<=1024 &&
            integers[3]>0 && integers[3]<=1024-integers[1] && integers[4]>0 &&
            integers[4]<=1024-integers[2] && geometry[0]<geometry[2] && geometry[1]<geometry[3];
        sprite={integers[0],integers[1],integers[2],integers[3],integers[4],geometry[0],geometry[1],geometry[2],geometry[3]};
    }
    for (auto& draw:next)
    {
        ok=ok && std::fread(&draw.sprite,4,1,file)==1 && std::fread(&draw.owner,4,1,file)==1 &&
            std::fread(draw.values.data(),4,extended?16:12,file)==(extended?16u:12u);
        // HKR1 has no material field. HKR2 stores a bounded, explicit kind in
        // the final float so a newly admitted shader cannot silently fall back
        // to an arbitrary blend state.
        ok=ok && draw.sprite<sprites.size() &&
            (!extended || (std::isfinite(draw.values[15]) && draw.values[15]>=0 &&
                           draw.values[15]<=5 && draw.values[15]==std::floor(draw.values[15])));
        for (float v:draw.values) ok=ok && std::isfinite(v) && std::fabs(v)<=100000;
        for (unsigned i=7;i<11;++i) ok=ok && draw.values[i]>=0 && draw.values[i]<=1;
    }
    for(unsigned i=0;ok&&i<header[3];++i){
        std::uint32_t index=0;float values[5]{};
        ok=std::fread(&index,4,1,file)==1&&std::fread(values,4,5,file)==5&&index<next.size();
        if(ok)next[index].grass={values[0],values[1],values[2],values[3],values[4]};
    }
    float splitDepth=roomBlurSplitDepth;
    if(roomSplitFormat)ok=ok&&std::fread(&splitDepth,4,1,file)==1&&
        std::isfinite(splitDepth)&&splitDepth>=-28.1f&&splitDepth<=961.9f;
    for(const auto& draw:next)
        ok=ok&&roomEffectSplitSupported(sprites[draw.sprite],draw,splitDepth);
    ok=ok && std::fgetc(file)==EOF;
    std::fclose(file);
    if (!ok||!loadScaleAnimation(path,next,scaleCurve)||!loadSwitchAnimations(path,next,sprites.size(),switchAnimations)) return false;
    blurSplitDepth=splitDepth;
    atlas.pageCount=header[0];atlas.sprites=std::move(sprites);draws=std::move(next);
    cameraBounds.reserve(draws.size());
    for(const auto& draw:draws)cameraBounds.push_back(roomCameraBounds(atlas.sprites[draw.sprite],draw,400,240,scaleFor(draw)));
    extendSwitchBounds(400,240);
    loadTrims(path);
    return true;
}
bool RoomVisualBank::loadTrims(const char* path)
{
    trims.clear();trimRange.clear();
    if(!gameFlagEnabled("sdmc:/3ds/HK3DS/fog-mesh.flag",gameFlagFogMesh,shippedFogMesh))return false;
    std::string name(path);
    const auto dot=name.rfind(".bin");
    if(dot==std::string::npos||dot+4!=name.size())return false;
    name.replace(dot,4,".strips");
    FILE* file=std::fopen(name.c_str(),"rb");
    if(!file)return false;
    // HKS1, sprite count (must match the bank), strip-set count, then
    // {sprite, strips, strips*{col0,col1,row0,row1}}.
    char magic[4]{};std::uint32_t header[2]{};
    bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"HKS1",4)==0&&
        std::fread(header,4,2,file)==2&&header[0]==atlas.sprites.size()&&header[1]<=4096;
    std::vector<RoomTrim> nextTrims;std::vector<std::uint32_t> nextRange(atlas.sprites.size(),0);
    for(unsigned i=0;ok&&i<header[1];++i){
        std::uint32_t set[2]{};
        ok=std::fread(set,4,2,file)==2&&set[0]<atlas.sprites.size()&&nextRange[set[0]]==0&&
            set[1]>0&&set[1]<=64&&nextTrims.size()+set[1]<(1u<<24);
        if(!ok)break;
        const auto& sprite=atlas.sprites[set[0]];
        nextRange[set[0]]=(static_cast<std::uint32_t>(nextTrims.size())<<8)|set[1];
        for(unsigned k=0;ok&&k<set[1];++k){
            RoomTrim t{};
            ok=std::fread(&t,sizeof(t),1,file)==1&&t.col0<t.col1&&t.col1<=sprite.width&&
                t.row0<t.row1&&t.row1<=sprite.height;
            if(ok)nextTrims.push_back(t);
        }
    }
    ok=ok&&std::fgetc(file)==EOF;
    std::fclose(file);
    if(!ok)return false;
    trims=std::move(nextTrims);trimRange=std::move(nextRange);
    return true;
}
}
