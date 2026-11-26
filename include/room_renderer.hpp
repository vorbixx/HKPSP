#pragma once
#include "room_visible_draws.hpp"
#include "room_visual.hpp"
#include "sector_cache.hpp"
#include <citro2d.h>
#include <vector>
#include <array>
#include <memory>
namespace hk3ds
{
class ScreenEffects;
class RoomRenderer
{
public:
    using Visibility=float(*)(std::uint32_t owner,void* context);
    using OwnerOffset=Vec2(*)(std::uint32_t owner,const void* context);
    void setOwnerOffset(OwnerOffset offset,const void* context)const{
        if(ownerOffset_!=offset)++backgroundRevision_;
        ownerOffset_=offset;ownerOffsetContext_=context;
    }
    bool load(const char* directory,const RoomVisualBank& bank,const char* prefix="room-");
    bool upload(const SectorData& sector,bool verify=false);
    bool loadDecor(const char* directory,const RoomVisualBank& bank);
    bool loadActors(const char* directory,const RoomVisualBank& bank,const char* prefix);
    bool reuseBackground(ScreenEffects& effects,const RoomVisualBank& bank,const RoomVisualBank* ambient,
                         const RoomVisualBank* decor,float cameraX,float cameraY,float ppu,Visibility visible,void* context) const;
    void drawDecor(const RoomVisualBank& bank,float cameraX,float cameraY,float ppu,bool includeForeground=true) const;
    void drawBackground(const RoomVisualBank& bank,const RoomVisualBank* ambient,
                        float cameraX,float cameraY,float ppu,Visibility visible,void* context,
                        float width,float height,float lightDepth,bool afterLight,
                        ScreenEffects* effects,bool background=false,const RoomVisualBank* decor=nullptr,
                        const RoomVisualBank* actors=nullptr) const;
    void drawForeground(const RoomVisualBank& bank,const RoomVisualBank& decor,
                        float cameraX,float cameraY,float ppu,Visibility visible,void* context,
                        ScreenEffects* effects,const RoomVisualBank* ambient=nullptr,
                        const RoomSortBoundary* split=nullptr,bool afterSplit=false,const RoomVisualBank* actors=nullptr) const;
    void release();
    // Camera is world center; screen dimensions are top-screen pixels.
    // Layer/order/depth split inserts the hero into the source sorting order.
    void draw(const RoomVisualBank& bank,float cameraX,float cameraY,float ppu,
              Visibility visible=nullptr,void* context=nullptr,
              float width=400,float height=240,bool behindHero=true,
              float lightDepth=0,bool afterLight=false,ScreenEffects* effects=nullptr,
              bool background=false,std::size_t first=0,std::size_t last=~std::size_t{0},
              bool ambient=false,const RoomVisualBank* blurRoom=nullptr) const;
    bool ready() const {return !pages_.empty()||uploaded_!=0;}
    bool actorsReady() const {return actorRenderer_&&actorRenderer_->ready();}
    // fog-mesh.flag: draws submitted as strips and the strip quads they used.
    unsigned trimDraws() const {return trimDraws_;}
    unsigned trimQuads() const {return trimQuads_;}
    unsigned loadFailures() const {return failures_;}
    unsigned vramPages() const {return vramPages_;}
    std::size_t vramBytes() const {return vramBytes_;}
    void setLevelTime(float seconds) const;
    void setSceneAmbient(const std::array<float,3>& ratio,const std::vector<std::uint32_t>* owners) const {
        ambientRatio_=ratio;litOwners_=owners;
    }
    // Test capture isolates all other layers at two grass shader times.
    void setGrassVisible(bool visible) const {grassVisible_=visible;}
    void resetDrawStats() const {visibleDraws_.invalidate();draws_=drawFailures_=trimDraws_=trimQuads_=0;if(decorRenderer_)decorRenderer_->resetDrawStats();if(actorRenderer_)actorRenderer_->resetDrawStats();}
    // One camera/opacity/depth scan per frame, shared by every pass over this bank.
    // The list is valid only for the frame that prepared it: release before FrameEnd.
    void prepareVisibleDraws(const RoomVisualBank& bank,float cameraX,float cameraY) const;
    void releaseVisibleDraws() const {visibleDraws_.invalidate();}
    unsigned drawCount() const {return draws_+(decorRenderer_?decorRenderer_->drawCount():0)+(actorRenderer_?actorRenderer_->drawCount():0);}
    unsigned drawFailures() const {return drawFailures_+(decorRenderer_?decorRenderer_->drawFailures():0)+(actorRenderer_?actorRenderer_->drawFailures():0);}
private:
    mutable RoomVisibleDraws visibleDraws_;
    mutable OwnerOffset ownerOffset_=nullptr;
    mutable const void* ownerOffsetContext_=nullptr;
    mutable std::array<float,3> ambientRatio_{1,1,1};
    mutable const std::vector<std::uint32_t>* litOwners_=nullptr;
    mutable unsigned backgroundRevision_=0;
    mutable unsigned draws_=0,drawFailures_=0,trimDraws_=0,trimQuads_=0;
    mutable float levelTime_=0;
    mutable RoomFocalCache focalCache_;
    mutable bool grassVisible_=true;
    std::vector<C2D_SpriteSheet> pages_;
    std::array<C3D_Tex,6> pool_{};
    unsigned allocated_=0,uploaded_=0;
    unsigned failures_=0;
    unsigned vramPages_=0;
    std::size_t vramBytes_=0;
    std::unique_ptr<RoomRenderer> decorRenderer_;
    std::unique_ptr<RoomRenderer> actorRenderer_;
};
}
