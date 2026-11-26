#pragma once
#include "animation.hpp"
#include "render_memory.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <memory>
namespace hk3ds
{
struct GrassSway
{
    float speed=0,amplitude=0,phase=0,push=0,height=0;
};
struct RoomScaleKey {float seconds,value,inSlope,outSlope;};
struct RoomScaleAnimation {
    std::vector<RoomScaleKey> keys;
    std::vector<std::uint32_t> owners;
    float minimum=1,maximum=1;
    float sample(float seconds) const;
};
struct RoomDraw
{
    std::uint32_t sprite=0, owner=0;
    // a,b,tx,c,d,ty,z,r,g,b,a,layer,order,zx,zy,material kind.
    std::array<float,16> values{};
    GrassSway grass{};
};
inline bool roomDrawHasOpacity(const RoomDraw& draw){return draw.values[10]>0;}
// x/y and right/bottom bound the geometry, independently of UV reflection.
struct RoomFlatSprite {float x,y,scaleX,scaleY,right,bottom;};
bool roomFlatSprite(const SpriteFrame& sprite,const std::array<float,16>& values,
                    float cameraX,float cameraY,float width,float height,float focal,
                    RoomFlatSprite& result);
struct RoomRotatedSprite {
    float x,y,width,height,angle;
    // Citro2D takes absolute rectangle dimensions and reflects the UVs separately.
    float centerX() const {return std::fabs(width)*.5f;}
    float centerY() const {return std::fabs(height)*.5f;}
};
bool roomRotatedSprite(const SpriteFrame& sprite,const std::array<float,16>& values,
                       float cameraX,float cameraY,float width,float height,float focal,
                       RoomRotatedSprite& result);
std::array<float,16> roomDrawValues(const RoomDraw& draw,float seconds,const RoomScaleAnimation* scale=nullptr);
enum class RoomMaterial : unsigned { Normal=0, Screen=1, LinearLight=2, DarknessCutout=3, Additive=4, LinearDodge=5, VividLight=6, AmbientAdditive=7 };
// Legacy HKR1-3 banks use the measured Tutorial_01 boundary. HKR4 stores each room.
constexpr float roomBlurSplitDepth=13.71f;
// Static quads tilted along one local axis can be clipped as rectangles.
struct RoomSpriteClip {
    SpriteFrame sprite;
    float left=0,top=0,right=1,bottom=1; // UV fractions within the source sprite.
    // Optional homogeneous view-Z row in local XY coordinates. Citro2D projects
    // Z to -.5*(Z+W), so Z=-W is its canonical near clip boundary.
    std::array<float,3> depthClip{};
    bool homogeneousClip=false;
};
bool roomEffectClipSprite(const SpriteFrame& sprite,const RoomDraw& draw,
                          bool background,RoomSpriteClip& result,float splitDepth=roomBlurSplitDepth);
// Static diagonal crossings use the GPU homogeneous near clip plane.
inline bool roomEffectSplitSupported(const SpriteFrame& sprite,const RoomDraw& draw,float splitDepth=roomBlurSplitDepth)
{
    float low=draw.values[6],high=low;
    for(float k:{draw.grass.push-std::fabs(draw.grass.amplitude),
                 draw.grass.push+std::fabs(draw.grass.amplitude)}){
    float zy=draw.values[14],z0=draw.values[6];
    if(k!=0){zy+=k*draw.values[13];z0+=k*draw.grass.height*draw.values[13];}
    for(float x:{sprite.left,sprite.right})for(float y:{sprite.bottom,sprite.top}){
        const float z=z0+draw.values[13]*x+zy*y;
        low=std::min(low,z);high=std::max(high,z);
    }
    }
    return high<splitDepth||low>=splitDepth||
        (draw.grass.amplitude==0&&draw.grass.push==0&&
         (((draw.values[13]==0)!=(draw.values[14]==0))||low>-38.1f));
}
inline RoomMaterial roomMaterial(const RoomDraw& draw)
{
    return static_cast<RoomMaterial>(static_cast<unsigned>(draw.values[15]));
}
// Source Tutorial camera: perspective, 24 degrees, z=-38.1, near=10.
// ForceCameraAspect increases vertical FOV below 16:9 (clamped to 16:10).
float roomFocalLength(float width=400,float height=240);
// Cache only the lens scalar; dimensions invalidate it before either camera pass.
// Keep the original runtime calculation and rounding, with no texture allocation.
class RoomFocalCache {
    float width_=0,height_=0,value_=0;
    bool valid_=false;
public:
    float get(float width,float height) {
        if(!valid_||width!=width_||height!=height_){
            value_=roomFocalLength(width,height);
            width_=width;height_=height;valid_=true;
        }
        return value_;
    }
};
bool roomDrawBehindHero(const RoomDraw& draw);
bool roomDrawBefore(const RoomDraw& draw,float layer,float order,float depth);
struct RoomSortBoundary {float layer,order,depth;};
struct RoomDrawRange {std::size_t first,last;};
// Room draws and camera-filtered snapshots retain authored painter order.
RoomDrawRange roomDrawRange(const std::vector<RoomDraw>& draws,const RoomSortBoundary* split,bool after);
struct RoomCameraBounds {
    float left=0,bottom=0,right=0,top=0;
    bool contains(float x,float y) const {return x>=left&&x<=right&&y>=bottom&&y<=top;}
};
RoomCameraBounds roomCameraBounds(const SpriteFrame& sprite,const RoomDraw& draw,
                                  float width=400,float height=240,const RoomScaleAnimation* scale=nullptr);
// Dynamic planar particles have no cached bounds. Tilted/animated draws are admitted.
bool roomParticleCameraVisible(const SpriteFrame& sprite,const RoomDraw& draw,
    float x,float y,float width,float height,float focal);
// Platform admission limit, not a room/route-specific exception. Leave space for
// transient GPU allocations; all texture allocations happen before gameplay.
constexpr std::size_t roomTexturePageBytes=2*1024*1024;
constexpr std::size_t decorTexturePageBytes=512*512*2;
constexpr std::size_t roomLinearReserve=2*1024*1024;
inline bool roomTextureBytesFit(std::size_t bytes,std::size_t freeBytes,
                                std::size_t budget=roomTextureBudget) {
    return bytes>0&&bytes<=budget&&freeBytes>=roomLinearReserve&&
        bytes<=freeBytes-roomLinearReserve;
}
inline bool roomTexturesFit(unsigned pages,std::size_t freeBytes,std::size_t pageBytes=roomTexturePageBytes,
                            std::size_t budget=roomTextureBudget) {
    return pages>0&&pages<=budget/roomTexturePageBytes&&
        (pageBytes==roomTexturePageBytes||pageBytes==decorTexturePageBytes)&&
        freeBytes>=roomLinearReserve&&pages*pageBytes<=freeBytes-roomLinearReserve;
}
// Opaque-support rectangles of one sprite in texels (row 0 is the top row).
// Exact: each strip set covers every texel that can have non-zero bilinear alpha.
struct RoomTrim {std::uint16_t col0,col1,row0,row1;};
// Optional source timelines shared by snapshots; RoomDraw stays compact.
struct RoomSwitchFrame {
    float seconds=0;
    std::array<std::uint32_t,4> sprites{};
};
struct RoomSwitchAnimation {
    std::uint32_t trigger=0,slots=0;
    float alpha=1;
    std::array<std::uint32_t,4> draws{},initial{};
    std::vector<RoomSwitchFrame> frames;
};
class RoomVisualBank
{
public:
    bool load(const char* path);
    bool loadTrims(const char* path);
    // fog-mesh.flag only: per-sprite strips from <bank>.strips; empty otherwise.
    std::vector<RoomTrim> trims;
    std::vector<std::uint32_t> trimRange; // per sprite: (first strip<<8)|count, 0 = none
    AnimationBank atlas;
    // Dynamic scene particles share the resident room atlas.
    bool usesRoomTextures=false;
    // Recovered transformed particles already include the shader's alpha gain.
    bool ambientAlphaGainApplied=false;
    float blurSplitDepth=roomBlurSplitDepth;
    std::vector<RoomDraw> draws;
    // Curves belong to the bank, keeping every ordinary/particle draw compact.
    std::shared_ptr<const RoomScaleAnimation> scaleCurve;
    std::shared_ptr<const std::vector<RoomSwitchAnimation>> switchAnimations;
    bool loadSwitches(const char* path);
    void extendSwitchBounds(float width,float height);
    void presentSwitches(std::uint32_t trigger,float seconds,bool active);
    const RoomScaleAnimation* scaleFor(const RoomDraw& draw) const {
        return scaleCurve&&std::binary_search(scaleCurve->owners.begin(),scaleCurve->owners.end(),draw.owner)?scaleCurve.get():nullptr;
    }
    // Conservative camera-space visibility, computed once for the top screen.
    std::vector<RoomCameraBounds> cameraBounds;
    // Missing bounds must admit the draw, just like the final submission guard.
    bool cameraVisible(std::size_t index,float x,float y) const {
        return cameraBounds.size()!=draws.size()||index>=cameraBounds.size()||
            cameraBounds[index].contains(x,y);
    }
    bool particleCameraVisible(std::size_t index,float x,float y,float width,float height,float focal) const {
        if(cameraBounds.size()==draws.size())return cameraVisible(index,x,y);
        if(index>=draws.size()||draws[index].sprite>=atlas.sprites.size())return true;
        return roomParticleCameraVisible(atlas.sprites[draws[index].sprite],draws[index],x,y,width,height,focal);
    }

};
}
