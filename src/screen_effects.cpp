#include "screen_effects.hpp"
#include <memory>
#include "diagnostic_game_flags.hpp"
#include "light_scissor.hpp"
#include "render_memory.hpp"
#include "performance_sections.hpp"
#include "gpu_profile.hpp"
#include "effect_passes.hpp"
#include <cassert>
#include <cstdio>
extern const unsigned char effects_shbin[];
extern const unsigned int effects_shbin_size;
namespace hk3ds
{
#if defined(BLUR_VARIANT_CLEAR)
constexpr effects::BlurTargetInit kBlurInit = effects::BlurTargetInit::Clear;
#elif !defined(BLUR_VARIANT_OFF)
constexpr effects::BlurTargetInit kBlurInit = effects::BlurTargetInit::Overwrite;
#endif

bool ScreenEffects::load()
{
    release();
    sharedRoomLights_=gameFlagEnabled("sdmc:/3ds/HK3DS/blend-grab-shared.flag",gameFlagBlendGrabShared,shippedBlendGrabShared);
    // Default on (user 2026-11-04 after viewing mask 502 on the console: "the light is fine"; console
    // 36.27 -> 34.24 ms): keep the 400x240 grab and playfield, but rasterize light sprites into a 200x120
    // view before bilinear composition. light-target-full.flag restores the full-size light target.
    halfLight_=gameFlagEnabled("sdmc:/3ds/HK3DS/light-target-half.flag",gameFlagHalfLight,
                               shippedHalfLight&&!sdFileExists("sdmc:/3ds/HK3DS/light-target-full.flag"));
    // Default on (user 2026-10-31): console frame time -14.3% Tutorial_01, -19.7% Town, -14.7% Crossroads_07
    // (docs/tasks/perf-fog-ab.md); Tutorial fog differs by up to 10 levels, accepted by the user.
    // blur-target-full.flag restores the full-size blurred background.
    smallBackground_=gameFlagEnabled("sdmc:/3ds/HK3DS/blur-target-small.flag",gameFlagBlurTargetSmall,
                                     shippedBlurTargetSmall&&!sdFileExists("sdmc:/3ds/HK3DS/blur-target-full.flag"));
    // User decision 2026-11-04: 200x120 looks too low resolution on console.
    // Keep it opt-in via the flag or diagnostic bit 4; the default is 400x240.
    halfBackground_=gameFlagEnabled("sdmc:/3ds/HK3DS/blur-target-half.flag",gameFlagBlurTargetHalf,
                                    shippedBlurTargetHalf);
    // Three disjoint small VRAM surfaces retain both light ping-pong buffers
    // and the destination grab. Never shrink the full 600x360 background.
    compactTargets_=gameFlagEnabled("sdmc:/3ds/HK3DS/effects-target-compact.flag",gameFlagEffectsTargetCompact,shippedEffectsTargetCompact)&&
        backgroundPassWidth()<=foregroundTextureWidth&&backgroundPassHeight()<=foregroundTextureHeight;
    // Default on: Crossroads_07 saves 6.52% with pixel-identical frames; Tutorial has no loss.
    // background-cache-off.flag restores direct drawing unless the enable flag/diagnostic mask overrides it.
    const bool cacheBackground=gameFlagEnabled("sdmc:/3ds/HK3DS/background-cache.flag",gameFlagBackgroundCache,
        shippedBackgroundCache&&!sdFileExists("sdmc:/3ds/HK3DS/background-cache-off.flag"));
    // A full image cannot fit beside the 832 KiB reserve. Keep it in target 0;
    // foreground ping-pong and grab use disjoint views of target 1 instead.
    static_assert(3u*foregroundTextureWidth*foregroundTextureHeight*4u<=screenEffectsLinearTargetBytes,
                  "Borrowed cache needs disjoint foreground ping-pong and grab views");
    backgroundCacheBorrowed_=!halfBackground_&&!compactTargets_&&cacheBackground;
    const unsigned targetWidth=compactTargets_?foregroundTextureWidth:screenEffectsTargetWidth;
    const unsigned targetHeight=compactTargets_?foregroundTextureHeight:screenEffectsTargetHeight;
    // libctru balances VRAM allocations across two 3 MiB banks. Hold a room
    // page while creating the small surfaces so they cannot split its space.
    std::unique_ptr<void,decltype(&vramFree)> roomPageSlot(
        compactTargets_?vramAlloc(roomTexturePageBytes):nullptr,&vramFree);
    if(compactTargets_&&!roomPageSlot){release();return false;}
    shader_=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(effects_shbin)),effects_shbin_size);
    if(!shader_)return false;
    programReady_=R_SUCCEEDED(shaderProgramInit(&program_));
    if(!programReady_||R_FAILED(shaderProgramSetVsh(&program_,&shader_->DVLE[0]))){release();return false;}
    for(unsigned i=0;i<3;++i){
        const bool ok=i<2||compactTargets_?C3D_TexInitVRAM(&textures_[i],targetWidth,targetHeight,GPU_RGBA8):
                                         C3D_TexInit(&textures_[i],targetWidth,targetHeight,GPU_RGBA8);
        if(!ok){release();return false;}
        ++allocated_;
        C3D_TexSetFilter(&textures_[i],GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&textures_[i],GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    }
    // All owners must exist before borrowed views reference the second surface.
    for(unsigned i=0;i<3;++i){
        foregroundTextures_[i]=textures_[backgroundCacheBorrowed_&&i<2?1:i];
        assert(foregroundTextureWidth<=foregroundTextures_[i].width&&foregroundTextureHeight<=foregroundTextures_[i].height);
        foregroundTextures_[i].width=foregroundTextureWidth;foregroundTextures_[i].height=foregroundTextureHeight;
        foregroundTextures_[i].size=foregroundTextureWidth*foregroundTextureHeight*4u;
        if(backgroundCacheBorrowed_&&i<2){
            assert(3u*foregroundTextures_[i].size<=textures_[1].size);
            foregroundTextures_[i].data=static_cast<u8*>(textures_[1].data)+i*foregroundTextures_[i].size;
        }
        if(i<2){
            foregroundTargets_[i]=C3D_RenderTargetCreateFromTex(&foregroundTextures_[i],GPU_TEXFACE_2D,0,-1);
            if(!foregroundTargets_[i]){release();return false;}
            targets_[i]=C3D_RenderTargetCreateFromTex(&textures_[i],GPU_TEXFACE_2D,0,-1);
            if(!targets_[i]){release();return false;}
        }
    }
    // The foreground light layer occupies only the first 512 KiB of the
    // opposite background allocation. Its next 512 KiB holds the grab, so
    // foreground copies and sampling stay in VRAM without another allocation.
    for(unsigned i=0;i<2;++i){
        if(backgroundCacheBorrowed_){
            foregroundGrabs_[i]=foregroundTextures_[0];
            foregroundGrabs_[i].data=static_cast<u8*>(textures_[1].data)+2u*foregroundGrabs_[i].size;
            continue;
        }
        if(compactTargets_){foregroundGrabs_[i]=textures_[2];continue;}
        foregroundGrabs_[i]=foregroundTextures_[1-i];
        const unsigned bytes=foregroundGrabs_[i].size;
        assert(2u*bytes<=textures_[1-i].size);
        foregroundGrabs_[i].data=static_cast<u8*>(textures_[1-i].data)+bytes;
    }
    if(halfLight_){
        if(!C3D_TexInitVRAM(&lightTextureStorage_,halfLightTextureWidth,halfLightTextureHeight,GPU_RGBA8)){
            release();return false;
        }
        C3D_TexSetFilter(&lightTextureStorage_,GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&lightTextureStorage_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        // RTT addressing and sampling share the 256x128 Morton tile stride.
        // Only the viewport is 200x120; changing texture dimensions corrupts it.
        lightTarget_=C3D_RenderTargetCreateFromTex(&lightTextureStorage_,GPU_TEXFACE_2D,0,-1);
        if(!lightTarget_){release();return false;}
    }
    for(unsigned i=0;i<2;++i){
        if(!C3D_TexInitVRAM(&bloomTextures_[i],cameraBloom::textureWidth,cameraBloom::textureHeight,GPU_RGBA8)){
            release();return false;
        }
        ++bloomAllocated_;
        C3D_TexSetFilter(&bloomTextures_[i],GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&bloomTextures_[i],GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        bloomTargets_[i]=C3D_RenderTargetCreateFromTex(&bloomTextures_[i],GPU_TEXFACE_2D,0,-1);
        if(!bloomTargets_[i]){release();return false;}
    }
    // Full-size cache borrows an existing VRAM surface (no added texture memory).
    // Reduced targets retain separate storage for static-prefix caching. Admit
    // it without spending the map/light reserve or the held room-page budget.
    if(backgroundCacheBorrowed_){
        backgroundCacheTexture_=textures_[0];backgroundCacheTarget_=targets_[0];
    }else if(cacheBackground){
        unsigned cacheWidth=8,cacheHeight=8;
        while(cacheWidth<backgroundPassWidth())cacheWidth*=2;
        while(cacheHeight<backgroundPassHeight())cacheHeight*=2;
        backgroundCacheAllocated_=lightVramFits(vramSpaceFree(),cacheWidth*cacheHeight*4u)&&
            C3D_TexInitVRAM(&backgroundCacheTexture_,cacheWidth,cacheHeight,GPU_RGBA8);
        if(backgroundCacheAllocated_){
            C3D_TexSetFilter(&backgroundCacheTexture_,GPU_LINEAR,GPU_LINEAR);
            C3D_TexSetWrap(&backgroundCacheTexture_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
            backgroundCacheTarget_=C3D_RenderTargetCreateFromTex(&backgroundCacheTexture_,GPU_TEXFACE_2D,0,-1);
        }
    }
    return true;
}
void ScreenEffects::release()
{
    backgroundCacheInputs_.invalidate();backgroundCacheCamera_={};backgroundCachePrepared_=false;
    backgroundCachePartial_=backgroundCacheHit_=false;backgroundCachePrefix_=0;
    if(backgroundCacheTarget_&&!backgroundCacheBorrowed_)C3D_RenderTargetDelete(backgroundCacheTarget_);
    backgroundCacheTarget_=nullptr;
    if(backgroundCacheAllocated_)C3D_TexDelete(&backgroundCacheTexture_);
    backgroundCacheAllocated_=backgroundCacheBorrowed_=false;
    for(auto*& t:bloomTargets_){if(t)C3D_RenderTargetDelete(t);t=nullptr;}
    for(unsigned i=0;i<bloomAllocated_;++i)C3D_TexDelete(&bloomTextures_[i]);
    bloomAllocated_=0;
    for(auto*& t:foregroundTargets_){if(t)C3D_RenderTargetDelete(t);t=nullptr;}
    if(lightTarget_)C3D_RenderTargetDelete(lightTarget_);
    lightTarget_=nullptr;
    if(lightTextureStorage_.data)C3D_TexDelete(&lightTextureStorage_);
    lightTextureStorage_={};
    for(auto*& t:targets_){if(t)C3D_RenderTargetDelete(t);t=nullptr;}
    for(unsigned i=0;i<allocated_;++i)C3D_TexDelete(&textures_[i]);
    allocated_=0;
    if(programReady_)shaderProgramFree(&program_);
    programReady_=false;
    if(shader_)DVLB_Free(shader_);
    shader_=nullptr;
    halfLight_=false;
}
void ScreenEffects::prepareGpu()
{
    C2D_Flush();C3D_BindProgram(&program_);
    auto* attrs=C3D_GetAttrInfo();AttrInfo_Init(attrs);
    for(int i=0;i<4;++i)AttrInfo_AddLoader(attrs,i,GPU_FLOAT,4);
}
void ScreenEffects::sceneViewport()
{
    C3D_SetViewport(0,0,width_,height_);
    // Raster resolution changes without changing sprite projection or camera.
    const bool background=background_;
    C2D_SceneSize(background?width_:400,background?height_:240,false);
}
LightScissor ScreenEffects::sceneLightScissor(float left,float top,float right,float bottom) const
{
    const bool background=background_;
    return lightScissor(left,top,right,bottom,width_,height_,background?width_:400,background?height_:240);
}
LightScissor ScreenEffects::lightRasterScissor(float left,float top,float right,float bottom) const
{
    if(halfLight_)return lightScissor(left,top,right,bottom,halfLightWidth,halfLightHeight,
        background_?width_:sceneWidth,background_?height_:sceneHeight);
    return sceneLightScissor(left,top,right,bottom);
}
C3D_Tex& ScreenEffects::lightTexture()
{
    return halfLight_?lightTextureStorage_:activeTexture(1-current_);
}
C3D_RenderTarget* ScreenEffects::lightTarget()
{
    return halfLight_?lightTarget_:activeTarget(1-current_);
}
void ScreenEffects::resume2d()
{
    C2D_Prepare();C2D_SceneBegin(activeTarget(current_));
    sceneViewport();
    C2D_ViewReset();C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
}
void ScreenEffects::beginBackground()
{
    assert(ready()&&!inLight_);current_=0;background_=true;width_=backgroundPassWidth();height_=backgroundPassHeight();
    C2D_TargetClear(targets_[current_],C2D_Color32(0,0,0,255));resume2d();
    if(backgroundCachePartial_&&backgroundCacheHit_){
        prepareGpu();C3D_FrameDrawOn(targets_[current_]);C3D_SetViewport(0,0,width_,height_);
        effects::replace();C3D_TexBind(0,&backgroundCacheTexture_);
        effects::quad(width_,height_,backgroundCacheTexture_.width,backgroundCacheTexture_.height);
        resume2d();
    }
}
bool ScreenEffects::reuseBackground(const RoomVisualBank& bank,const RoomVisualBank* ambient,const RoomVisualBank* decor,
                                   float cameraX,float cameraY,float ppu,BackgroundCacheInputs::Visibility visible,void* context,
                                   const void* renderer,unsigned revision,float levelTime,bool grassVisible)
{
    backgroundCachePrepared_=backgroundCachePartial_=backgroundCacheHit_=false;
    backgroundCachePrefix_=0;
    if(!backgroundCacheTarget_){observeBackgroundCache(false,cameraX,cameraY);return false;}
    assert(ready()&&!inLight_);
    if(!performanceObservation.on(PerformanceObservation::Decor))decor=nullptr;
    if(!performanceObservation.on(PerformanceObservation::Ambient))ambient=nullptr;
    const auto plan=backgroundCachePlan(bank,ambient,decor,
        backgroundCacheCamera_.stationary(renderer,revision,cameraX,cameraY)&&!backgroundCacheBorrowed_);
    backgroundCachePartial_=plan.live;
    backgroundCachePrefix_=plan.prefix;
    if(plan.live&&plan.prefix==0){
        backgroundCacheInputs_.invalidate();observeBackgroundCache(false,cameraX,cameraY);return false;
    }
    backgroundCacheInputs_.begin(renderer,revision,performanceObservation.disabled|(plan.live?0x80000000u:0),ppu);
    backgroundCacheInputs_.append(bank,0,bank.blurSplitDepth,cameraX,cameraY,levelTime,grassVisible,visible,context,plan.live,plan.prefix);
    backgroundCachePrepared_=true;
    const bool reusable=backgroundCacheInputs_.reusable();
    backgroundCacheHit_=reusable;
    observeBackgroundCache(reusable,cameraX,cameraY);
    if(!reusable||plan.live)return false;
    PerformanceScope timing(PerformanceSection::BackgroundEffects);
    prepareGpu();
    effects::vibrancy(backgroundCacheTexture_,foregroundTargets_[1],backgroundPassWidth(),backgroundPassHeight(),sceneWidth,sceneHeight);
    gpuProfileCheckpoint(PerformanceSection::GpuVibrancy);
    current_=1;background_=false;width_=sceneWidth;height_=sceneHeight;resume2d();
    backgroundCachePrepared_=false;
    return true;
}
void ScreenEffects::captureBackgroundCache()
{
    // The borrowed full-size target already contains the completed image.
    if(backgroundCacheBorrowed_){assert(current_==0);backgroundCacheInputs_.commit();return;}
    // One-to-one RGBA8 copy; raw prefixes retain live particles and blur.
    prepareGpu();C3D_FrameDrawOn(backgroundCacheTarget_);C3D_SetViewport(0,0,width_,height_);
    effects::replace();C3D_TexBind(0,&textures_[current_]);
    effects::quad(width_,height_,textures_[current_].width,textures_[current_].height);
    backgroundCacheInputs_.commit();
}
void ScreenEffects::captureBackgroundPrefix()
{
    if(!backgroundCachePrepared_||!backgroundCachePartial_||backgroundCacheHit_)return;
    captureBackgroundCache();resume2d();
}
void ScreenEffects::compositeBackground()
{
    gpuProfileCheckpoint(PerformanceSection::GpuBackground);
    PerformanceScope timing(PerformanceSection::BackgroundEffects);
    prepareGpu();
    // Off retains the source, targets and subsequent vibrancy pass.
#if !defined(BLUR_VARIANT_OFF)
    if(performanceObservation.on(PerformanceObservation::Blur))for(unsigned pass=0;pass<4;++pass){
        const unsigned next=1-current_;
        effects::blur(textures_[current_],targets_[next],width_,height_,pass%2,
                      kBlurInit,float(width_)/backgroundWidth);current_=next;
        gpuProfileCheckpoint(PerformanceSection(unsigned(PerformanceSection::GpuBlur0)+pass));
    }
#endif
    // Foreground views all occupy target 1 in borrowed mode. Preserve target 0
    // across frames and avoid sampling the allocation vibrancy writes into.
    if(backgroundCacheBorrowed_&&current_!=0){
        C3D_FrameDrawOn(targets_[0]);C3D_SetViewport(0,0,width_,height_);
        effects::replace();C3D_TexBind(0,&textures_[current_]);
        effects::quad(width_,height_,textures_[current_].width,textures_[current_].height);
        current_=0;
    }
    if(backgroundCacheTarget_&&backgroundCachePrepared_&&!backgroundCachePartial_)captureBackgroundCache();
    backgroundCachePrepared_=false;
    const unsigned next=1-current_;
    effects::vibrancy(textures_[current_],foregroundTargets_[next],width_,height_,sceneWidth,sceneHeight);
    gpuProfileCheckpoint(PerformanceSection::GpuVibrancy);
    current_=next;background_=false;width_=sceneWidth;height_=sceneHeight;resume2d();
}
void ScreenEffects::beginLight(float left,float top,float right,float bottom)
{
    PerformanceScope timing(PerformanceSection::LightComposite);
    assert(ready()&&!inLight_);inLight_=true;
    lightScissor_=sceneLightScissor(left,top,right,bottom);
    lightRasterScissor_=lightRasterScissor(left,top,right,bottom);
    const auto grabClear=lightClear(lightScissor_,activeTexture(current_).width,activeTexture(current_).height);
    const auto rasterClear=lightClear(lightRasterScissor_,
        halfLight_?halfLightTextureWidth:lightTexture().width,
        halfLight_?halfLightTextureHeight:lightTexture().height);
    // Split/queue a GPU copy after preceding draws. Background grabs use the
    // linear allocation; foreground grabs use disjoint spare VRAM storage.
    C2D_Flush();
    gpuProfileCheckpoint(PerformanceSection::GpuScene);
    // Preserve every texel sampled by the scissor, including bilinear support.
    const auto copyStart=gpuProfileTransferBegin();
    C3D_SyncTextureCopy(static_cast<u32*>(activeTexture(current_).data)+grabClear.offset/4,0,
        static_cast<u32*>(activeTexture(2).data)+grabClear.offset/4,0,grabClear.bytes,8);
    if(copyStart)gpuProfileTransferEnd(PerformanceSection::GpuGrabCopy,copyStart);
    else gpuProfileCheckpoint(PerformanceSection::GpuGrabCopy);
    auto* data=static_cast<u32*>(lightTexture().data);
    if(halfLight_)data=static_cast<u32*>(lightTextureStorage_.data);
    const auto clearStart=gpuProfileTransferBegin();
    C3D_SyncMemoryFill(data+rasterClear.offset/4,0,data+(rasterClear.offset+rasterClear.bytes)/4,
        GX_FILL_32BIT_DEPTH|GX_FILL_TRIGGER,nullptr,0,nullptr,0);
    if(clearStart)gpuProfileTransferEnd(PerformanceSection::GpuLightClear,clearStart);
    else gpuProfileCheckpoint(PerformanceSection::GpuLightClear);
    C2D_SceneBegin(lightTarget());
    if(halfLight_){
        C3D_SetViewport(0,0,halfLightWidth,halfLightHeight);
        C2D_SceneSize(background_?width_:sceneWidth,background_?height_:sceneHeight,false);
    }else sceneViewport();
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
}
void ScreenEffects::endLight(RoomMaterial material,float left,float top,float right,float bottom)
{
    PerformanceScope timing(PerformanceSection::LightComposite);
    assert(inLight_);inLight_=false;
    prepareGpu();
    gpuProfileCheckpoint(PerformanceSection::GpuLightSprite);
    C3D_FrameDrawOn(activeTarget(current_));C3D_SetViewport(0,0,width_,height_);
    const auto r=sceneLightScissor(left,top,right,bottom);
    compositeLight(material,r);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    gpuProfileCheckpoint(PerformanceSection::GpuLightComposite);
    resume2d();
}
void ScreenEffects::compositeLight(RoomMaterial material,LightScissor r)
{
    assert(material==RoomMaterial::LinearLight||material==RoomMaterial::Screen||material==RoomMaterial::LinearDodge||material==RoomMaterial::VividLight);
    // Every sampled texel must have been cleared by beginLight.
    assert(r.left>=lightScissor_.left&&r.right<=lightScissor_.right&&
           r.bottom>=lightScissor_.bottom&&r.top<=lightScissor_.top);
    C3D_SetScissor(GPU_SCISSOR_NORMAL,r.left,r.bottom,r.right,r.top);
    auto& light=lightTexture();
    if(material==RoomMaterial::Screen)
        effects::screen(activeTexture(2),light,width_,height_,halfLight_?halfLightWidth:0,halfLight_?halfLightHeight:0);
    else if(material==RoomMaterial::LinearDodge)
        effects::linearDodge(activeTexture(2),light,width_,height_,halfLight_?halfLightWidth:0,halfLight_?halfLightHeight:0);
    else if(material==RoomMaterial::VividLight)
        effects::vivid(activeTexture(2),light,width_,height_,halfLight_?halfLightWidth:0,halfLight_?halfLightHeight:0);
    else effects::linear(activeTexture(2),light,width_,height_,halfLight_?halfLightWidth:0,halfLight_?halfLightHeight:0);
}
bool ScreenEffects::roomLightNeedsCapture(float left,float top,float right,float bottom) const
{
    const auto r=sceneLightScissor(left,top,right,bottom);
    return !inLight_||!roomLights_.accepts(r)||!lightContains(lightScissor_,r);
}
void ScreenEffects::beginRoomLight(float left,float top,float right,float bottom,float captureTop,float captureBottom)
{
    if(!sharedRoomLights_){beginLight(left,top,right,bottom);return;}
    if(roomLightNeedsCapture(left,top,right,bottom))flushRoomLights();
    // Transfers cover complete tiled rows. Bound their height to the planned
    // disjoint run; singleton runs retain the original sprite-sized transfer.
    if(!inLight_)beginLight(0,captureTop,background_?width_:400,captureBottom);
}
void ScreenEffects::endRoomLight(RoomMaterial material,float left,float top,float right,float bottom)
{
    if(!sharedRoomLights_){endLight(material,left,top,right,bottom);return;}
    assert(inLight_);
    roomLightMaterials_[roomLights_.size()]=material;
    roomLights_.append(sceneLightScissor(left,top,right,bottom));
}
void ScreenEffects::flushRoomLights()
{
    if(!roomLights_.size())return;
    PerformanceScope timing(PerformanceSection::LightComposite);
    assert(inLight_);inLight_=false;
    prepareGpu();
    gpuProfileCheckpoint(PerformanceSection::GpuLightSprite);
    C3D_FrameDrawOn(activeTarget(current_));C3D_SetViewport(0,0,width_,height_);
    for(unsigned i=0;i<roomLights_.size();++i)compositeLight(roomLightMaterials_[i],roomLights_[i]);
    roomLights_.clear();
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    gpuProfileCheckpoint(PerformanceSection::GpuLightComposite);
    resume2d();
}
void ScreenEffects::present(C3D_RenderTarget* screen,bool applyCameraColor,bool applyBloom,float healFlash,float secondHealFlash)
{
    assert(ready()&&!inLight_&&width_==sceneWidth&&height_==sceneHeight);
    for(float flash:{healFlash,secondHealFlash})if(flash>0){
        prepareGpu();const unsigned next=1-current_;
        effects::scuttlerFlash(activeTexture(current_),activeTarget(next),width_,height_,flash);current_=next;
    }
    gpuProfileCheckpoint(PerformanceSection::GpuScene);
    if(applyBloom&&performanceObservation.on(PerformanceObservation::Bloom)){
        PerformanceScope timing(PerformanceSection::Bloom);
        prepareGpu();
        effects::bloomExtract(activeTexture(current_),bloomTargets_[0],width_,height_);
        gpuProfileCheckpoint(PerformanceSection::GpuBloomExtract);
        for(unsigned iteration=0;iteration<2;++iteration){
            const float spacing=float(iteration+1)*cameraBloom::radiusScale;
            effects::bloomBlur(bloomTextures_[0],bloomTargets_[1],0,spacing);
            effects::bloomBlur(bloomTextures_[1],bloomTargets_[0],1,spacing);
            gpuProfileCheckpoint(PerformanceSection::GpuBloomBlur);
        }
        effects::bloomAdd(bloomTextures_[0],activeTarget(current_),width_,height_);
        gpuProfileCheckpoint(PerformanceSection::GpuBloomAdd);
    }
    if(applyCameraColor&&performanceObservation.on(PerformanceObservation::Color)){
        PerformanceScope timing(PerformanceSection::CameraColor);
        prepareGpu();
        if(cameraColor_.twoPass){
            cameraColor::Config pre;
            pre.a=cameraColor_.preA;pre.b=cameraColor_.preB;pre.c=cameraColor_.preC;pre.sat=0xffffffff;
            effects::cameraColor(activeTexture(current_),activeTarget(1-current_),width_,height_,pre);
            current_=1-current_;
        }
        const unsigned next=1-current_;
        effects::cameraColor(activeTexture(current_),activeTarget(next),width_,height_,cameraColor_);
        gpuProfileCheckpoint(PerformanceSection::GpuCameraColor);
        current_=next;
    }
    PerformanceScope timing(PerformanceSection::Submission);
    C2D_Flush();
    C2D_Prepare();C2D_SceneBegin(screen);C2D_ViewReset();
    Tex3DS_SubTexture sub{};sub.width=width_;sub.height=height_;
    sub.left=0;sub.right=float(width_)/activeTexture(current_).width;sub.top=float(height_)/activeTexture(current_).height;sub.bottom=0;
    effects::cameraCopyBlend();
    C2D_DrawImageAt({&activeTexture(current_),&sub},0,0,0,nullptr,400.f/width_,240.f/height_);
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
}
}
