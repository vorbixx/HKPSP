#pragma once
#include "camera_color.hpp"
#include "room_visual.hpp"
#include "lighting_math.hpp"
#include "gpu_attrib.hpp"
#include <array>
#include <initializer_list>
#include <string>
#include <memory>
#include <stdexcept>

namespace hkpsp {
inline constexpr unsigned screenWidth=480,screenHeight=272;
struct Point {float x=0,y=0;};
// ClampedAdd realizes the exported shader557; Room retains the raw material API.
enum class Blend { Room, Multiply2, MaskAlpha, MaskedLiquid, ScreenPremult, ClampedAdd, GradeGreen, GradeRed, GradeBlue, FadeBlack };
inline bool samplesTexture(Blend blend){
    return blend!=Blend::MaskAlpha&&blend!=Blend::GradeGreen&&blend!=Blend::GradeRed&&
           blend!=Blend::GradeBlue&&blend!=Blend::FadeBlack;
}
struct Quad {
    unsigned page=0;
    float u0=0,v0=0,u1=0,v1=0;
    // TL, TR, BR, BL; inverse depth preserves perspective-correct sampling.
    std::array<Point,4> positions{};
    std::array<float,4> inverseDepth{{1,1,1,1}};
    std::array<float,4> rgba{{1,1,1,1}};
    float whiteFlash=0;
    hk3ds::RoomMaterial material=hk3ds::RoomMaterial::Normal;
    Blend blend=Blend::Room;
    unsigned order=0;
    unsigned clut=0;
};
// Eligible sprite batches must retain both the texture and its palette.
inline bool sameBatchState(const Quad& a,const Quad& b){
    return a.page==b.page&&a.clut==b.clut&&a.material==b.material&&
        a.blend==b.blend&&a.rgba==b.rgba;
}
using Palette=std::array<std::array<unsigned char,4>,256>;
struct alignas(16) Clut {
    Palette raw{},premultiplied{},light{};
};
using ClutBank=std::vector<Clut>;
struct Texture {
    unsigned width=0,height=0;
    unsigned bits=8; // HPT1 T8 or T4; host indices always unpacked.
    std::array<std::array<unsigned char,4>,256> palette{};
    std::vector<unsigned char> indices; // Linear rows after HPT1 unswizzle.
    // HPT2 pages in one directory share the immutable palettes.pcl bank.
    std::shared_ptr<const ClutBank> cluts;
    bool transient=false;
    const Palette& paletteFor(unsigned id)const {
        if(cluts)return cluts->at(id).raw;
        if(id)throw std::out_of_range("Legacy texture CLUT index");
        return palette;
    }
};
struct Glyph {
    unsigned code=0,font=0,page=0,x=0,y=0,width=0,height=0;
    float advance=0,bearingX=0,bearingY=0;
};
struct FontMetrics {
    unsigned id=0,size=0,first=0,count=0;
    float ascent=0,descent=0,capitals=0,lineHeight=0;
};
struct Kern { unsigned font=0,left=0,right=0;float adjustment=0; };
struct FontBank {
    unsigned pageCount=0;
    std::vector<FontMetrics> fonts;
    std::vector<Glyph> glyphs;
    std::vector<Kern> kerning;
    const Glyph& glyph(unsigned font,unsigned code)const;
    float kern(unsigned font,unsigned left,unsigned right)const;
};
struct FrameInfo {
    unsigned frame=0,controlSteps=0;
    float heroX=0,heroY=0,cameraX=0,cameraY=0;
    float velocityX=0,velocityY=0;
    unsigned actor=0,phase=0,health=0,soul=0,geo=0;
    const char* scene="Tutorial_01";
    float simulationX=0,simulationY=0;
    unsigned abilityPhase=0;
    float transitionFade=0;
};
struct RoomGeScene;
struct DrawList {
#ifdef HKPSP_GPU_ATTRIB
    std::vector<gpu::Pass> gpuPasses;
#endif
    const std::vector<Texture>* textures=nullptr;
    // PSR1 v2 keeps logical sprite/draw IDs stable for shared room controllers.
    const std::vector<std::vector<hk3ds::SpriteFrame>>* roomTiles=nullptr;
    // Sampled once per frame; only source-authored lit owners receive ambient.
    const std::vector<std::uint32_t>* roomLitOwners=nullptr;
    std::array<float,3> roomAmbient{{1,1,1}};
    hk3ds::cameraColor::Config cameraColor;
    std::vector<Quad> quads;
    // One game-thread scene binds its cumulative list for a frame. Static
    // sidecar storage keeps the exact DrawList ABI used by baseline oracles.
    inline static const DrawList* geTarget=nullptr;
    inline static const RoomGeScene* geScene=nullptr;
    inline static unsigned geQuads=0;
    const RoomGeScene* roomGe()const{return geTarget==this?geScene:nullptr;}
    unsigned nextOrder()const{return static_cast<unsigned>(quads.size())+(roomGe()?geQuads:0);}
    FrameInfo info;
#ifdef HKPSP_PERF
    // Profiling metadata only: indices survive the room's within-pass reorder.
    struct RoomRange {std::size_t first,last;};
    std::vector<RoomRange> roomRanges;
    bool isRoomQuad(std::size_t index)const{
        for(const auto& range:roomRanges)if(index>=range.first&&index<range.last)return true;
        return false;
    }
#endif
};
// Join the appended room quad to a preceding state run within 32 quads. The
// caller's first index fences one append/pass and identical authored layer/depth.
// Crossed screen rectangles must be disjoint; all other pairs stay stable.
void reorderRoomTail(DrawList&,std::size_t first);
using RoomTiles=std::vector<std::vector<hk3ds::SpriteFrame>>;
hk3ds::RoomVisualBank readRoom(const std::string& path,RoomTiles* tiles=nullptr);
hk3ds::RoomVisualBank readLightAtlas(const std::string& path);
hk3ds::AnimationBank readAnimation(const std::string& path,std::string* pool=nullptr);
// The PSP needs GE-ready bytes only when a page is submitted. CPU light
// validation and HUD sampling request unpacked indices; the host keeps them.
#ifdef __PSP__
inline constexpr bool cpuTextureIndices=false;
#else
inline constexpr bool cpuTextureIndices=true;
#endif
Texture readTexture(const std::string& path,bool unpack=cpuTextureIndices);
class Platform;
struct AnimationAtlas {hk3ds::AnimationBank bank;unsigned page=0;};
// One registry per texture vector; pooled owners retain local sprite page IDs.
class AnimationPages {
    struct Pool {std::string name;unsigned first,count;std::vector<std::string> paths;};
    std::vector<Pool> pools;
public:
    AnimationAtlas load(Platform&,std::vector<Texture>&,const std::string& table,
                        const std::string& prefix,bool unpack=cpuTextureIndices,
                        std::initializer_list<const char*> cpuClips={});
};
FontBank readFonts(const std::string& path);
bool roomQuad(const hk3ds::SpriteFrame& sprite,const hk3ds::RoomDraw& draw,
              hk3ds::Vec2 camera,float seconds,Quad& result);
// Immutable draw/pass data. Positions are world corners until projection; the
// clipped UV fractions and all other Quad fields are baked at room load.
struct RoomQuadCache {
    Quad world;
    std::array<float,4> clip{{0,0,1,1}};
    bool depthVisible=false,homogeneousClip=false;
};
// roomEffectClipSprite without the 3DS diagonal split-plane clip (homogeneous W clip, room_visual.cpp:108):
// such a sprite is drawn whole on the side of the split holding its centre (Fungus2_23 threw; other-rooms 2026-11-21).
inline bool pspRoomClip(const hk3ds::SpriteFrame& sprite,const hk3ds::RoomDraw& draw,bool background,
                 hk3ds::RoomSpriteClip& clip,float splitDepth){
    if(!hk3ds::roomEffectClipSprite(sprite,draw,background,clip,splitDepth))return false;
    if(!clip.homogeneousClip)return true;
    const auto& v=draw.values;
    const float centre=v[6]+v[13]*.5f*(sprite.left+sprite.right)+v[14]*.5f*(sprite.bottom+sprite.top);
    clip=hk3ds::RoomSpriteClip{sprite};
    return (centre>=splitDepth)==background;
}
bool cacheRoomQuad(const hk3ds::SpriteFrame&,const hk3ds::RoomDraw&,bool background,
                   float splitDepth,RoomQuadCache&);
bool projectRoomQuad(const RoomQuadCache&,hk3ds::Vec2 camera,float focal,Quad&);
// owner is the room-load dense alpha index, not the exported raw owner ID.
struct RoomDrawRef {
    unsigned index=0;const RoomQuadCache* cached=nullptr;unsigned owner=0;
    // Precomputed plan entry; ~0u also serves legacy callers without a mapping.
    unsigned geEntry=~0u;
};
// A contiguous split of the ordered candidates. Mutable keys that no longer
// form an ordered stream retain the original per-record split/ambient merge.
struct RoomSelection {
    std::size_t first=0,last=0;
    const std::vector<float>* alpha=nullptr;
    bool splitSelected=false;
};
void appendRoom(DrawList& list,const hk3ds::RoomVisualBank& bank,hk3ds::Vec2 camera,
                float seconds,bool behindHero,bool background,
                float (*visibility)(unsigned,void*)=nullptr,void* context=nullptr,
                const hk3ds::RoomSortBoundary* split=nullptr,bool afterSplit=false,
                const hk3ds::RoomVisualBank* particles=nullptr,unsigned particleSprite=0,
                const std::vector<RoomDrawRef>* selected=nullptr,const RoomSelection* selection=nullptr);
void appendHero(DrawList& list,const hk3ds::AnimationBank& bank,
                const hk3ds::AnimationPlayer& player,hk3ds::Vec2 position,
                hk3ds::Vec2 camera,bool facingRight,unsigned firstPage,bool presentation,float flash=0);
// All moving sprites share transform, UV reflection, tint and culling.
// HK art faces left: logical facing +1 (right) supplies scale.x=-1.
struct ActorPose {
    hk3ds::Vec2 position{},scale{1,1};
    float depth=.004f,rotation=0,alpha=1,flash=0;
    std::array<float,3> tint{{1,1,1}};
    hk3ds::RoomMaterial material=hk3ds::RoomMaterial::Normal;
    Blend blend=Blend::Room;
};
void appendActor(DrawList&,const hk3ds::AnimationBank&,const hk3ds::AnimationPlayer&,
                 const ActorPose&,hk3ds::Vec2 camera,unsigned firstPage);
void appendLight(DrawList& list,const hk3ds::RoomVisualBank& atlas,const hk3ds::LightingConfig& config,
                 hk3ds::Vec2 hero,hk3ds::Vec2 camera,bool facingRight,unsigned firstPage,
                 unsigned kind,float scale,std::array<float,3> rgb={1,1,1},float alpha=1);
void appendCameraGrade(DrawList& list);
}
