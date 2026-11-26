#pragma once
#include <citro2d.h>
#include <array>
#include <cassert>
#include "room_visual.hpp"
#include "light_scissor.hpp"
#include "camera_color.hpp"
#include "background_cache.hpp"
namespace hk3ds
{
class ScreenEffects
{
public:
    static constexpr unsigned backgroundWidth=600,backgroundHeight=360;
    static constexpr unsigned sceneWidth=400,sceneHeight=240;  // Native playfield; only the blurred background is reduced.
    static constexpr unsigned halfBackgroundWidth=sceneWidth/2,halfBackgroundHeight=sceneHeight/2;
    static constexpr unsigned halfLightWidth=sceneWidth/2,halfLightHeight=sceneHeight/2;
    static constexpr unsigned halfLightTextureWidth=256,halfLightTextureHeight=128;
    static_assert(sceneWidth*240==sceneHeight*400,"Keep the native scene aspect ratio");
    static_assert(sceneWidth!=backgroundWidth,"Scene and background must select different target views");
    static constexpr float splitDepth=roomBlurSplitDepth;
    bool load();
    // Default: blurred background rendered at scene resolution (blur-target-full.flag: full size).
    unsigned backgroundPassWidth() const {return halfBackground_?halfBackgroundWidth:(smallBackground_?sceneWidth:backgroundWidth);}
    unsigned backgroundPassHeight() const {return halfBackground_?halfBackgroundHeight:(smallBackground_?sceneHeight:backgroundHeight);}
    bool smallBackground() const {return smallBackground_;}
    unsigned vramBytes() const {
        unsigned bytes=0;
        for(unsigned i=0;i<allocated_;++i)if(i<2||compactTargets_)bytes+=textures_[i].size;
        if(halfLight_&&lightTextureStorage_.data)bytes+=lightTextureStorage_.size;
        for(unsigned i=0;i<bloomAllocated_;++i)bytes+=bloomTextures_[i].size;
        if(backgroundCacheAllocated_)bytes+=backgroundCacheTexture_.size;
        return bytes;
    }
    void setCameraColor(const cameraColor::Config& config={}) {cameraColor_=config;}
    void release();
    bool ready() const {return allocated_==3&&targets_[0]&&targets_[1]&&bloomAllocated_==2&&bloomTargets_[0]&&bloomTargets_[1]&&foregroundTargets_[0]&&foregroundTargets_[1]&&(!halfLight_|| (lightTextureStorage_.data&&lightTarget_));}
    void beginBackground();
    bool reuseBackground(const RoomVisualBank& bank,const RoomVisualBank* ambient,const RoomVisualBank* decor,
                         float cameraX,float cameraY,float ppu,BackgroundCacheInputs::Visibility visible,void* context,
                         const void* renderer,unsigned revision,float levelTime,bool grassVisible);
    void compositeBackground();
    std::size_t backgroundCachePrefix() const {return backgroundCachePartial_?backgroundCachePrefix_:0;}
    bool backgroundCacheHit() const {return backgroundCacheHit_;}
    void captureBackgroundPrefix();
    void beginLight(float left=0,float top=0,float right=-1,float bottom=-1);
    void endLight(RoomMaterial material=RoomMaterial::LinearLight,
                  float left=0,float top=0,float right=-1,float bottom=-1);
    bool sharedRoomLights() const {return sharedRoomLights_;}
    bool roomLightNeedsCapture(float left,float top,float right,float bottom) const;
    void beginRoomLight(float left,float top,float right,float bottom,float captureTop,float captureBottom);
    void endRoomLight(RoomMaterial material,float left,float top,float right,float bottom);
    void flushRoomLights();
    void present(C3D_RenderTarget* screen,bool applyCameraColor=true,bool applyBloom=true,float healFlash=0,float secondHealFlash=0);
private:
    BackgroundCacheInputs backgroundCacheInputs_;
    BackgroundCacheCamera backgroundCacheCamera_;
    C3D_Tex backgroundCacheTexture_{};
    C3D_RenderTarget* backgroundCacheTarget_=nullptr;
    bool backgroundCacheAllocated_=false,backgroundCachePrepared_=false,backgroundCacheBorrowed_=false;
    bool backgroundCachePartial_=false,backgroundCacheHit_=false;
    std::size_t backgroundCachePrefix_=0;
    void captureBackgroundCache();
    cameraColor::Config cameraColor_;
    static constexpr unsigned foregroundTextureWidth=512,foregroundTextureHeight=256;
    C3D_Tex& activeTexture(unsigned i) {
        assert(i<3);
        return background_?textures_[i]:(i==2?foregroundGrabs_[current_]:foregroundTextures_[i]);
    }
    C3D_RenderTarget* activeTarget(unsigned i) {
        assert(i<2);
        return background_?targets_[i]:foregroundTargets_[i];
    }
    void prepareGpu();
    void sceneViewport();
    LightScissor sceneLightScissor(float left,float top,float right,float bottom) const;
    LightScissor lightRasterScissor(float left,float top,float right,float bottom) const;
    C3D_Tex& lightTexture();
    C3D_RenderTarget* lightTarget();
    void resume2d();
    void compositeLight(RoomMaterial material,LightScissor scissor);
    std::array<C3D_Tex,3> textures_{};
    std::array<C3D_RenderTarget*,3> targets_{};
    // Non-owning views reuse the existing allocation after the background pass.
    // Their RTTs are destroyed before the allocation-owning textures.
    std::array<C3D_Tex,3> foregroundTextures_{};
    std::array<C3D_Tex,2> foregroundGrabs_{};
    std::array<C3D_RenderTarget*,2> foregroundTargets_{};
    C3D_Tex lightTextureStorage_{};
    C3D_RenderTarget* lightTarget_=nullptr;
    std::array<C3D_Tex,2> bloomTextures_{};
    std::array<C3D_RenderTarget*,2> bloomTargets_{};
    DVLB_s* shader_=nullptr;
    shaderProgram_s program_{};
    bool programReady_=false;
    bool inLight_=false;
    LightScissor lightScissor_{};
    bool sharedRoomLights_=false;
    LightBatch roomLights_;
    std::array<RoomMaterial,LightBatch::capacity> roomLightMaterials_{};
    unsigned allocated_=0,width_=400,height_=240;
    bool background_=false,smallBackground_=false;
    bool halfBackground_=false,compactTargets_=false;
    bool halfLight_=false;
    // A two-slot selector remains bounded when release builds omit assertions.
    bool current_=false;
    unsigned bloomAllocated_=0;
    LightScissor lightRasterScissor_{};
};
}
