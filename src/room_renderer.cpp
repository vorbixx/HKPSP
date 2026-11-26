#include "loading_screen.hpp"
#include "room_renderer.hpp"
#include "diagnostic_game_flags.hpp"
#include "load_diagnostics.hpp"
#include "ambient_particles.hpp"
#include "graphics_profile.hpp"
#include "performance_sections.hpp"
#include "gpu_profile.hpp"
#include "screen_effects.hpp"
#include "effect_passes.hpp"
#include "frame_profile.hpp"
#include "breakable.hpp"
#include "ambient_particles.hpp"
#include "sprite_layout.hpp"
#include "render_memory.hpp"
#include "room_vram.hpp"
#include "runtime_diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <3ds.h>
#ifndef HK3DS_PARTICLE_EARLY_CULL
#define HK3DS_PARTICLE_EARLY_CULL 1
#endif
namespace hk3ds
{
namespace
{
#if (HK3DS_PERF_DISABLE_MASK & 0x20000000u) != 0
using RendererCpuLap = RoomCpuLap;
#else
// Remove per-quad observation branches entirely from ordinary builds.
struct RendererCpuLap { void mark(PerformanceSection) {} };
#endif
constexpr bool earlyParticleCull = HK3DS_PARTICLE_EARLY_CULL != 0;
struct SortEntry {const RoomVisualBank* bank;unsigned index;};
bool boundedRoomView(float width,float height) {
    return (width==400&&height==240)||
        (width==ScreenEffects::backgroundWidth&&height==ScreenEffects::backgroundHeight);
}

// Foreground/background merges run sequentially on the render thread. draw()
// does not merge, so both passes can reuse this fixed scratch bank safely.
// The full particle capacity must not be allocated on the 32 KiB ARM stack.
std::array<SortEntry,DecorEffects::capacity+AmbientParticles::capacity+stalactiteSpriteCapacity> sortEntries;
}
void RoomRenderer::setLevelTime(float seconds) const
{
    levelTime_=seconds;
    if(decorRenderer_)decorRenderer_->setLevelTime(seconds);
}
void RoomRenderer::prepareVisibleDraws(const RoomVisualBank& bank,float x,float y) const
{
    PerformanceScope timing(PerformanceSection::RoomSubmit);
    RendererCpuLap cpu;
    visibleDraws_.prepare(bank,x,y,grassVisible_,graphicsProfile().maxRoomDepth);
    cpu.mark(PerformanceSection::RoomFilter); // The shared scan is filtering cost.
}
bool RoomRenderer::upload(const SectorData& sector,bool verify)
{
    ++backgroundRevision_;
    if(sector.pages.empty()||sector.pages.size()>pool_.size()||
       sector.pages.size()!=sector.visuals.atlas.pageCount){++failures_;return false;}
    for(const auto& page:sector.pages)if(page.size()!=2*1024*1024){++failures_;return false;}
    // Build and validate the entire plan before changing a submitted texture.
    std::vector<std::vector<RoomPageRange>> ranges(sector.pages.size());
    for(unsigned i=0;i<ranges.size();++i)
        if(!roomPageRanges(sector.visuals.atlas,i,ranges[i])){++failures_;return false;}
    // Reserve the maximum once; a failed allocation leaves the current image valid.
    while(allocated_<pool_.size()){
        if(!C3D_TexInit(&pool_[allocated_],1024,1024,GPU_RGBA4)){++failures_;return false;}
        C3D_TexSetFilter(&pool_[allocated_],GPU_LINEAR,GPU_LINEAR);++allocated_;
        frameProfile.memory();
    }
    {FrameProfile::Scope wait(frameProfile,FrameProfile::FrameBegin);C3D_FrameSync();}
    {FrameProfile::Scope wait(frameProfile,FrameProfile::GpuWait);
     if(!C3D_FrameBegin(0)){++failures_;return false;}}
    FrameProfile::Scope upload(frameProfile,FrameProfile::Page);
    std::size_t bytes=0;
    for(unsigned i=0;i<sector.pages.size();++i){
        {FrameProfile::UploadScope timing(frameProfile,false);
         for(const auto& range:ranges[i]){
             std::memcpy(static_cast<std::uint8_t*>(pool_[i].data)+range.offset,
                         sector.pages[i].data()+range.offset,range.size);
             bytes+=range.size;
         }}
        {FrameProfile::UploadScope timing(frameProfile,true);C3D_TexFlush(&pool_[i]);}
        if(verify)for(const auto& range:ranges[i])
            if(std::memcmp(static_cast<const std::uint8_t*>(pool_[i].data)+range.offset,
                           sector.pages[i].data()+range.offset,range.size)){
                ++failures_;return false;
            }
    }
    frameProfile.uploaded(sector.pages.size(),bytes);
    uploaded_=sector.pages.size();return true;
}
bool RoomRenderer::load(const char* directory,const RoomVisualBank& bank,const char* prefix)
{
    // Actors are admitted before room pages so their allocation counts against
    // the room reserve. A page reload must keep those resident textures alive.
    auto actors=std::move(actorRenderer_);
    ++backgroundRevision_;
    release();
    actorRenderer_=std::move(actors);
    const bool verify=diagnosticLogRequested("sdmc:/3ds/HK3DS/verify-room-pages.flag");unsigned verified=0;
    const auto report=[&](unsigned failures){
        if(verify)if(auto* f=std::fopen("sdmc:/3ds/HK3DS/room-page-verification.txt","w")){
            std::fprintf(f,"verified=%u failures=%u\n",verified,failures);std::fclose(f);
        }
    };
    char failurePath[320];std::snprintf(failurePath,sizeof(failurePath),"%s/%s (%lu pages)",directory,prefix,static_cast<unsigned long>(bank.atlas.pageCount));
    const char* failureStage="room_linear_admission";
    char failureInfo[160]{};
    const auto fail=[&]{
        // Capture the allocator state before releasing the partially loaded room.
        char detail[512];std::snprintf(detail,sizeof(detail),"%s: %s",failureInfo,failurePath);
        loadDiagnostics.error(failureStage,detail);
        if(runtimeDiagnostics.enabled())for(const auto* log:{"sdmc:/3ds/HK3DS/room-page-verification-errors.txt","sdmc:/3ds/HK3DS/room-texture-failure.txt"})
        if(auto* f=std::fopen(log,"a")){
            std::fprintf(f,"%s %s linear=%lu vram=%lu\n",failureStage,detail,
                static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
            std::fclose(f);
        }
        release();++failures_;report(1);return false;
    };
    const bool compactDecor=std::strcmp(prefix,"decor-fx-")==0;
    const unsigned pageWidth=compactDecor?512u:1024u;
    const auto pageBytes=compactDecor?decorTexturePageBytes:roomTexturePageBytes;
    std::vector<bool> wanted(bank.atlas.pageCount,true);
    if(!compactDecor&&!graphicsProfile().postprocessing){
        std::fill(wanted.begin(),wanted.end(),false);
        for(const auto& draw:bank.draws)if(std::abs(draw.values[6])<graphicsProfile().maxRoomDepth)
            wanted[bank.atlas.sprites[draw.sprite].page]=true;
    }
    const auto count=std::count(wanted.begin(),wanted.end(),true);
    if(!roomTexturesFit(count,linearSpaceFree(),pageBytes,activeRoomTextureBudget)){
        std::snprintf(failureInfo,sizeof(failureInfo),"pages=%lu page_bytes=%lu budget=%lu",
            static_cast<unsigned long>(count),static_cast<unsigned long>(pageBytes),static_cast<unsigned long>(activeRoomTextureBudget));
        return fail();
    }
    // Read each allocation before importing any page. A mixed RGBA4/RGBA8 bank
    // must fit by actual bytes, including the unchanged transient linear reserve.
    std::vector<std::size_t> expected(bank.atlas.pageCount,0);
    std::size_t totalBytes=0;
    for(unsigned i=0;i<bank.atlas.pageCount;++i){
        if(!wanted[i])continue;
        char path[320];std::snprintf(path,sizeof(path),"%s/%spage-%u.t3x",directory,prefix,i);
        std::snprintf(failurePath,sizeof(failurePath),"%s",path);failureStage="room_page_header";
        errno=0;auto* file=std::fopen(path,"rb");if(!file){
            failureStage="room_page_open";std::snprintf(failureInfo,sizeof(failureInfo),"errno=%d",errno);return fail();
        }
        unsigned char header[4]{};errno=0;const auto bytes=std::fread(header,1,sizeof(header),file);
        const int readError=errno;
        std::fclose(file);
        if(bytes!=sizeof(header)){
            failureStage="room_page_read";std::snprintf(failureInfo,sizeof(failureInfo),"bytes=%lu/4 errno=%d",static_cast<unsigned long>(bytes),readError);return fail();
        }
        if(header[0]!=1||header[1]!=0||header[2]!=(compactDecor?54:63)||
           (header[3]!=4&&(compactDecor||header[3]!=0))){
            std::snprintf(failureInfo,sizeof(failureInfo),"header=%02x %02x %02x %02x expected=01 00 %02x RGBA4/8",
                unsigned(header[0]),unsigned(header[1]),unsigned(header[2]),unsigned(header[3]),compactDecor?54u:63u);return fail();
        }
        expected[i]=pageBytes*(header[3]==0?2u:1u);totalBytes+=expected[i];
    }
    failureStage="room_byte_admission";
    if(!roomTextureBytesFit(totalBytes,linearSpaceFree(),activeRoomTextureBudget)){
        std::snprintf(failureInfo,sizeof(failureInfo),"bytes=%lu budget=%lu",static_cast<unsigned long>(totalBytes),static_cast<unsigned long>(activeRoomTextureBudget));return fail();
    }
    pages_.resize(bank.atlas.pageCount,nullptr);
    for(unsigned i=0;i<bank.atlas.pageCount;++i)
    {
        if(!wanted[i])continue;
        char path[320];std::snprintf(path,sizeof(path),"%s/%spage-%u.t3x",directory,prefix,i);
        FrameProfile::Scope timing(frameProfile,FrameProfile::Page);
        std::snprintf(failurePath,sizeof(failurePath),"%s",path);failureStage="room_page_import";
        frameProfile.memory();
        pulseLoadingScreen();
        errno=0;auto sheet=C2D_SpriteSheetLoad(path);
        if(!sheet){std::snprintf(failureInfo,sizeof(failureInfo),"Tex3DS import failed errno=%d",errno);return fail();}
        pages_[i]=sheet;
        const auto image=C2D_SpriteSheetGetImage(sheet,0);
        const auto* texture=image.tex;
        failureStage="room_page_layout";
        if(!texture||!image.subtex){std::snprintf(failureInfo,sizeof(failureInfo),"missing imported texture/image");return fail();}
        frameProfile.page(texture->size);
        const auto expectedFormat=expected[i]==pageBytes?GPU_RGBA4:GPU_RGBA8;
        if(texture->width!=pageWidth||texture->height!=pageWidth||texture->fmt!=expectedFormat||
           texture->size!=expected[i]||linearSpaceFree()<roomLinearReserve){
            std::snprintf(failureInfo,sizeof(failureInfo),"got=%ux%u fmt=%u bytes=%lu expected=%ux%u fmt=%u bytes=%lu",
                unsigned(texture->width),unsigned(texture->height),unsigned(texture->fmt),static_cast<unsigned long>(texture->size),
                pageWidth,pageWidth,unsigned(expectedFormat),static_cast<unsigned long>(expected[i]));return fail();
        }
        frameProfile.memory();
        if(verify){
            failureStage="room_page_verification";
            // Compare the SDK-owned texture against our independent decoder;
            // scratch is ordinary heap, never an additional GPU texture.
            auto* file=std::fopen(path,"rb");if(!file)return fail();
            const bool seek=std::fseek(file,0,SEEK_END)==0;
            const long length=seek?std::ftell(file):-1;
            bool ok=length>0&&length<=5*1024*1024&&std::fseek(file,0,SEEK_SET)==0;
            std::vector<std::uint8_t> encoded(ok?std::size_t(length):0),decoded;
            if(ok)ok=std::fread(encoded.data(),1,encoded.size(),file)==encoded.size();
            std::fclose(file);
            if(!ok||!decodeRoomPage(encoded,decoded,pageWidth)||decoded.size()!=texture->size||
               std::memcmp(decoded.data(),texture->data,decoded.size()))return fail();
            ++verified;
        }
        // The source room atlases use bilinear minification and magnification.
        C3D_TexSetFilter(C2D_SpriteSheetGetImage(sheet,0).tex,GPU_LINEAR,GPU_LINEAR);
    }
    // Rank estimated sampled sprite area, including each instance's transform.
    // This is an admission heuristic, not a claim of measured fragment traffic.
    if(!compactDecor&&graphicsProfile().postprocessing&&
       gameFlagEnabled("sdmc:/3ds/HK3DS/hot-room-vram.flag",gameFlagHotRoomVram,shippedHotRoomVram)){
        std::vector<double> scores(pages_.size(),0);
        for(const auto& draw:bank.draws){
            const auto& sprite=bank.atlas.sprites[draw.sprite];
            const auto& v=draw.values;
            if(roomDrawHasOpacity(draw))scores[sprite.page]+=double(sprite.width)*sprite.height*
                std::abs(double(v[0])*v[4]-double(v[1])*v[3]);
        }
        for(std::size_t n=0;n<scores.size();++n){
            const auto page=hottestRoomPage(scores);if(page==scores.size())break;
            scores[page]=0;if(!pages_[page])continue;
            auto* texture=C2D_SpriteSheetGetImage(pages_[page],0).tex;
            if(!roomVramPageFits(vramSpaceFree(),texture->size))break;
            pulseLoadingScreen();
            C3D_Tex promoted{};
            if(!C3D_TexInitVRAM(&promoted,texture->width,texture->height,texture->fmt))continue;
            if(promoted.width!=texture->width||promoted.height!=texture->height||
               promoted.fmt!=texture->fmt||promoted.size!=texture->size){C3D_TexDelete(&promoted);continue;}
            // Copy the already decoded, swizzled bytes without another SD read.
            C3D_TexFlush(texture);
            C3D_TexUpload(&promoted,texture->data);
            // The sheet continues to own its texture, now backed by VRAM.
            promoted.param=texture->param;promoted.border=texture->border;
            promoted.lodParam=texture->lodParam;
            C3D_TexDelete(texture);*texture=promoted;
            ++vramPages_;vramBytes_+=texture->size;
        }
    }
    report(0);
    return ready();
}
void RoomRenderer::release()
{
    if(actorRenderer_){actorRenderer_->release();actorRenderer_.reset();}
    ++backgroundRevision_;
    if(decorRenderer_){decorRenderer_->release();decorRenderer_.reset();}
    for(auto page:pages_)if(page)C2D_SpriteSheetFree(page);
    pages_.clear();
    for(unsigned i=0;i<allocated_;++i)C3D_TexDelete(&pool_[i]);
    allocated_=uploaded_=0;vramPages_=0;vramBytes_=0;
}
bool RoomRenderer::loadDecor(const char* directory,const RoomVisualBank& bank)
{
    ++backgroundRevision_;
    auto renderer=std::make_unique<RoomRenderer>();
    if(!renderer->load(directory,bank,"decor-fx-"))return false;
    decorRenderer_=std::move(renderer);return true;
}
bool RoomRenderer::loadActors(const char* directory,const RoomVisualBank& bank,const char* prefix)
{
    auto renderer=std::make_unique<RoomRenderer>();
    if(!renderer->load(directory,bank,prefix))return false;
    actorRenderer_=std::move(renderer);return true;
}
bool RoomRenderer::reuseBackground(ScreenEffects& effects,const RoomVisualBank& bank,const RoomVisualBank* ambient,
                                   const RoomVisualBank* decor,float cameraX,float cameraY,float ppu,Visibility visible,void* context) const
{
    if(ownerOffset_)return false;
    return effects.reuseBackground(bank,ambient,decor,cameraX,cameraY,ppu,visible,context,
                                   this,backgroundRevision_,levelTime_,grassVisible_);
}
void RoomRenderer::drawDecor(const RoomVisualBank& bank,float x,float y,float ppu,bool includeForeground) const
{
    if(!decorRenderer_||bank.draws.empty())return;
    for(bool behind:{true,false}){
        std::size_t first=0;
        while(first<bank.draws.size()){
            if(!includeForeground&&bank.draws[first].values[11]>0){++first;continue;}
            std::size_t last=first+1;
            while(last<bank.draws.size()&&(includeForeground||bank.draws[last].values[11]<=0))++last;
            decorRenderer_->draw(bank,x,y,ppu,nullptr,nullptr,400,240,behind,0,false,nullptr,false,first,last);
            first=last;
        }
    }
}
void RoomRenderer::drawForeground(const RoomVisualBank& bank,const RoomVisualBank& decor,
                                 float x,float y,float ppu,Visibility visible,void* context,ScreenEffects* effects,
                                 const RoomVisualBank* ambient,const RoomSortBoundary* split,bool afterSplit,const RoomVisualBank* actors) const
{
    // Every foreground particle joins the room's authored sorting order.
    // Fixed scratch storage; bulk room ranges retain the existing batching.
    if(!performanceObservation.on(PerformanceObservation::Ambient))ambient=nullptr;
    const auto range=roomDrawRange(bank.draws,split,afterSplit);
    const auto inPass=[&](const RoomDraw& d){
        return !split||roomDrawBefore(d,split->layer,split->order,split->depth)!=afterSplit;
    };
    if(!actorRenderer_)actors=nullptr;
    const bool drawDecor=performanceObservation.on(PerformanceObservation::Decor);
    if(((!decorRenderer_&&(!ambient||!ambient->usesRoomTextures))||
       ((!drawDecor||decor.draws.empty())&&(!ambient||ambient->draws.empty())))&&(!actors||actors->draws.empty())){
        draw(bank,x,y,ppu,visible,context,400,240,false,0,false,effects,false,range.first,range.last);
        return;
    }
    // Opt-in: cull with the final submission guard before sorting. Rejected particles
    // must not split room batches or force empty draw invocations.
    // Admission still precedes culling so the fixed-capacity overflow is unchanged.
    using Entry=SortEntry;
    auto& indices=sortEntries;
    unsigned count=0,admitted=0;
    const float particleFocal=earlyParticleCull ? focalCache_.get(400,240) : 0;
    {PerformanceScope timing(PerformanceSection::MergeSort);
    if(drawDecor&&decorRenderer_)for(unsigned i=0;i<decor.draws.size()&&admitted<indices.size();++i)
        if(roomDrawHasOpacity(decor.draws[i])&&!roomDrawBehindHero(decor.draws[i])&&inPass(decor.draws[i])){
            ++admitted;if(!earlyParticleCull||decor.particleCameraVisible(i,x,y,400,240,particleFocal))indices[count++]={&decor,i};
        }
    if(ambient)for(unsigned i=0;i<ambient->draws.size()&&admitted<indices.size();++i)
        if(roomDrawHasOpacity(ambient->draws[i])&&!roomDrawBehindHero(ambient->draws[i])&&inPass(ambient->draws[i])){
            ++admitted;if(!earlyParticleCull||ambient->particleCameraVisible(i,x,y,400,240,particleFocal))indices[count++]={ambient,i};
        }
    if(actors)for(unsigned i=0;i<actors->draws.size()&&admitted<indices.size();++i)
        if(roomDrawHasOpacity(actors->draws[i])&&!roomDrawBehindHero(actors->draws[i])&&inPass(actors->draws[i])){
            ++admitted;if(!earlyParticleCull||actors->particleCameraVisible(i,x,y,400,240,particleFocal))indices[count++]={actors,i};
        }
    std::sort(indices.begin(),indices.begin()+count,[&](Entry a,Entry b){
        const auto& av=a.bank->draws[a.index].values;const auto& bv=b.bank->draws[b.index].values;
        if(av[11]!=bv[11])return av[11]<bv[11];
        if(av[12]!=bv[12])return av[12]<bv[12];
        if(av[6]!=bv[6])return av[6]>bv[6];
        if(a.bank!=b.bank){
            const auto rank=[&](const RoomVisualBank* p){return p==&decor?0:(p==ambient?1:2);};
            return rank(a.bank)<rank(b.bank);
        }
        return a.index<b.index;
    });
    }
    std::size_t first=range.first;
    for(unsigned i=0;i<count;++i){
        const auto entry=indices[i];
        const auto& v=entry.bank->draws[entry.index].values;
        std::size_t last=first;
        while(last<range.last&&roomDrawBefore(bank.draws[last],v[11],v[12],v[6]))++last;
        if(last>first)draw(bank,x,y,ppu,visible,context,400,240,false,0,false,effects,false,first,last);
        // Consecutive particles can share one C2D batch until a room draw
        // or another particle bank intervenes in the authored painter order.
        unsigned end=i+1;
        while(end<count&&indices[end].bank==entry.bank&&indices[end].index==indices[end-1].index+1){
            const auto& next=entry.bank->draws[indices[end].index].values;
            if(last<range.last&&roomDrawBefore(bank.draws[last],next[11],next[12],next[6]))break;
            ++end;
        }
        const auto* renderer=entry.bank==actors?actorRenderer_.get():
            (entry.bank->usesRoomTextures?this:decorRenderer_.get());
        if(renderer)renderer->draw(*entry.bank,x,y,ppu,entry.bank->usesRoomTextures?visible:nullptr,
                       entry.bank->usesRoomTextures?context:nullptr,400,240,false,0,false,
                       entry.bank==actors?effects:nullptr,false,entry.index,indices[end-1].index+1,entry.bank==ambient);
        i=end-1;
        first=last;
    }
    if(first<range.last)draw(bank,x,y,ppu,visible,context,400,240,false,0,false,effects,false,first,range.last);
}
void RoomRenderer::drawBackground(const RoomVisualBank& bank,const RoomVisualBank* ambient,
                                  float x,float y,float ppu,Visibility visible,void* context,
                                  float width,float height,float lightDepth,bool afterLight,
                                  ScreenEffects* effects,bool background,const RoomVisualBank* decor,
                                  const RoomVisualBank* actors) const
{
    if(!performanceObservation.on(PerformanceObservation::Ambient))ambient=nullptr;
    if(!performanceObservation.on(PerformanceObservation::Decor))decor=nullptr;
    if(!actorRenderer_)actors=nullptr;
    // Opt-in: cull with the final submission guard before sorting. Rejected particles
    // must not split room batches or force empty draw invocations.
    // Admission still precedes culling so the fixed-capacity overflow is unchanged.
    using Entry=SortEntry;
    auto& indices=sortEntries;unsigned count=0,admitted=0;
    const float particleFocal=earlyParticleCull ? focalCache_.get(width,height) : 0;
    {PerformanceScope timing(PerformanceSection::MergeSort);
    for(const auto* particles:{decor,ambient})if(particles&&(particles->usesRoomTextures||decorRenderer_))
        for(unsigned i=0;i<particles->draws.size()&&admitted<indices.size();++i)
            // Foreground ambient joins drawForeground at its authored layer.
            // Admit only draws this behind-hero pass can actually submit.
            if(roomDrawHasOpacity(particles->draws[i])&&roomDrawBehindHero(particles->draws[i])){
                ++admitted;
                if(!earlyParticleCull||!boundedRoomView(width,height)||particles->particleCameraVisible(i,x,y,width,height,particleFocal))indices[count++]={particles,i};
            }
    if(actors)for(unsigned i=0;i<actors->draws.size()&&admitted<indices.size();++i)
        if(roomDrawHasOpacity(actors->draws[i])&&roomDrawBehindHero(actors->draws[i])){
            ++admitted;
            if(!earlyParticleCull||!boundedRoomView(width,height)||actors->particleCameraVisible(i,x,y,width,height,particleFocal))indices[count++]={actors,i};
        }
    std::sort(indices.begin(),indices.begin()+count,[&](Entry a,Entry b){
        const auto& av=a.bank->draws[a.index].values;const auto& bv=b.bank->draws[b.index].values;
        if(av[11]!=bv[11])return av[11]<bv[11];
        if(av[12]!=bv[12])return av[12]<bv[12];
        if(av[6]!=bv[6])return av[6]>bv[6];
        if(a.bank!=b.bank){
            const auto rank=[&](const RoomVisualBank* p){return p==decor?0:(p==ambient?1:2);};
            return rank(a.bank)<rank(b.bank);
        }
        return a.index<b.index;
    });
    }
    std::size_t first=0;
    if(background&&effects&&effects->backgroundCachePrefix()){
        first=effects->backgroundCachePrefix();
        if(!effects->backgroundCacheHit()){
            draw(bank,x,y,ppu,visible,context,width,height,true,lightDepth,afterLight,effects,true,0,first);
            effects->captureBackgroundPrefix();
        }
    }
    for(unsigned i=0;i<count;++i){
        const auto entry=indices[i];const auto& v=entry.bank->draws[entry.index].values;
        std::size_t last=first;
        while(last<bank.draws.size()&&roomDrawBefore(bank.draws[last],v[11],v[12],v[6]))++last;
        if(last>first)draw(bank,x,y,ppu,visible,context,width,height,true,lightDepth,afterLight,effects,background,first,last);
        // Consecutive particles can share one C2D batch until a room draw
        // or another particle bank intervenes in the authored painter order.
        unsigned end=i+1;
        while(end<count&&indices[end].bank==entry.bank&&indices[end].index==indices[end-1].index+1){
            const auto& next=entry.bank->draws[indices[end].index].values;
            if(last<bank.draws.size()&&roomDrawBefore(bank.draws[last],next[11],next[12],next[6]))break;
            ++end;
        }
        const auto* renderer=entry.bank==actors?actorRenderer_.get():
            (entry.bank->usesRoomTextures?this:decorRenderer_.get());
        if(renderer)renderer->draw(*entry.bank,x,y,ppu,entry.bank->usesRoomTextures?visible:nullptr,
                       entry.bank->usesRoomTextures?context:nullptr,width,height,true,lightDepth,afterLight,
                       effects,background,entry.index,indices[end-1].index+1,entry.bank==ambient,&bank);
        i=end-1;
        first=last;
    }
    if(first<bank.draws.size())draw(bank,x,y,ppu,visible,context,width,height,true,lightDepth,afterLight,effects,background,first,bank.draws.size());
}
void RoomRenderer::draw(const RoomVisualBank& bank,float cameraX,float cameraY,float ppu,
                       Visibility visible,void* context,float width,float height,bool behindHero,float lightDepth,bool afterLight,
                       ScreenEffects* effects,bool background,std::size_t first,std::size_t last,bool ambient,const RoomVisualBank* blurRoom) const
{
    PerformanceScope timing(PerformanceSection::RoomSubmit);
    // Diagnostic ceiling only: remove submission while preserving simulation.
    if(!performanceObservation.on(PerformanceObservation::RoomSubmission))return;
    constexpr float trimMinimumArea=4096;
    C2D_SetTintMode(C2D_TintMult);
    RoomMaterial activeMaterial=RoomMaterial::Normal;
    bool blendChanged=false;
    const auto selectMaterial=[&](RoomMaterial material){
        if(material==activeMaterial)return;
        // Citro2d batches commands. Flush before changing the PICA blend
        // state, otherwise a queued ordinary sprite can inherit a light blend.
        C2D_Flush();
        if(material==RoomMaterial::Additive){
            effects::additiveSprite();
        }
        else if(material==RoomMaterial::AmbientAdditive){
            // Recovered Deepnest banks use material 7 for Legacy Particles/
            // Additive. Its shader applies the authored 2x RGB gain; routing
            // it through plain additive blending leaves the motes visibly dim.
            if(ambient){
                if(bank.ambientAlphaGainApplied)effects::deepnestAmbientSprite();
                else effects::ambientSprite();
            }
            else effects::additiveSprite();
        }
        else if(material==RoomMaterial::Screen&&ambient)effects::softParticleSprite();
        else if(material==RoomMaterial::DarknessCutout)effects::darknessCutoutSprite();
        else effects::screenSprite(material==RoomMaterial::Screen);
        activeMaterial=material;blendChanged=true;
    };
    C3D_Mtx saved;C2D_ViewSave(&saved);
    C3D_Mtx identity;Mtx_Identity(&identity);
    const bool plainView=std::memcmp(&saved,&identity,sizeof(saved))==0;
    bool transformedView=false;
    const float focal=focalCache_.get(width,height);
    const bool shared=boundedRoomView(width,height)&&
        visibleDraws_.matches(bank,cameraX,cameraY,grassVisible_,graphicsProfile().maxRoomDepth);
    const auto end=shared?visibleDraws_.lowerBound(std::min(last,bank.draws.size())):std::min(last,bank.draws.size());
    for (std::size_t cursor=shared?visibleDraws_.lowerBound(first):first;cursor<end;++cursor)
    {
        RendererCpuLap cpu;
        const std::size_t index=shared?visibleDraws_.indices()[cursor]:cursor;
        // Both passes have the same frustum; the cached 400x240 pixel of slack
        // also covers the background pass at the same camera aspect.
        if(!shared&&boundedRoomView(width,height)&&!bank.cameraVisible(index,cameraX,cameraY))continue;
        const auto& draw=bank.draws[index];
        if(!shared){
            if(!roomDrawHasOpacity(draw)||std::abs(draw.values[6])>=graphicsProfile().maxRoomDepth)continue;
            if(!grassVisible_&&(draw.grass.amplitude!=0||draw.grass.push!=0))continue;
        }
        const float alpha=visible?visible(draw.owner,context):1;
        if (roomDrawBehindHero(draw)!=behindHero || alpha<=0) continue;
        if(lightDepth!=0&&roomDrawBefore(draw,0,0,lightDepth)==afterLight)continue;
        const auto& original=bank.atlas.sprites[draw.sprite];
        RoomSpriteClip clip{original};
        if(effects&&!roomEffectClipSprite(original,draw,background,clip,blurRoom?blurRoom->blurSplitDepth:bank.blurSplitDepth))continue;
        const auto& sprite=clip.sprite;
        if (sprite.page>=(uploaded_?uploaded_:pages_.size())) continue;
        if(!uploaded_&&!pages_[sprite.page])continue;
        // Animated draws stay within one pass; static axis tilts are clipped above.
        // Evaluate after pass/sort rejection, once per submitted grass quad.
        cpu.mark(PerformanceSection::RoomProjection);
        std::array<float,16> animated;
        const auto* scaleAnimation=bank.scaleFor(draw);
        const auto offset=ownerOffset_?ownerOffset_(draw.owner,ownerOffsetContext_):Vec2{};
        const bool shifted=offset.x!=0||offset.y!=0;
        const auto& v=(scaleAnimation||draw.grass.amplitude!=0||draw.grass.push!=0||shifted)?
            (animated=roomDrawValues(draw,levelTime_,scaleAnimation),animated[2]+=offset.x,animated[5]+=offset.y,animated):draw.values;
        const float depth=38.1f+v[6];
        RoomFlatSprite flat;
        const bool flatSprite=plainView&&roomFlatSprite(sprite,v,cameraX,cameraY,width,height,focal,flat);
        float left=1e9f,right=-1e9f,top=1e9f,bottom=-1e9f,nearest=1e9f,farthest=-1e9f;
        // Reuse the axis-aligned projection for culling and coverage as well
        // as drawing. Tilted/sheared sprites retain the four-corner path.
        if(flatSprite){left=flat.x;top=flat.y;right=flat.right;bottom=flat.bottom;nearest=farthest=depth;}
        else for(float lx:{sprite.left,sprite.right})for(float ly:{sprite.bottom,sprite.top})
        {
            const float z=depth+v[13]*lx+v[14]*ly;
            nearest=std::min(nearest,z);farthest=std::max(farthest,z);
            const float x=width*.5f+focal*(v[0]*lx+v[1]*ly+v[2]-cameraX)/std::max(10.0f,z);
            const float y=height*.5f-focal*(v[3]*lx+v[4]*ly+v[5]-cameraY)/std::max(10.0f,z);
            left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
        }
        if(farthest<10 || nearest>1000 || right<0 || left>width || bottom<0 || top>height)continue;
        cpu.mark(PerformanceSection::RoomState);
        const auto material=roomMaterial(draw);
        // Signed/clamped source shaders need the destination. Disjoint padded
        // rectangles may share it without changing clamp or alpha semantics.
        const bool grab=effects&&(material==RoomMaterial::LinearLight||
            material==RoomMaterial::LinearDodge||material==RoomMaterial::VividLight);
        if(grab&&!performanceObservation.on(PerformanceObservation::Lights))continue;
        const auto extent=[](float lo,float hi,float limit){
            return std::max(0.f,std::min(limit,std::ceil(hi))-std::max(0.f,std::floor(lo)));
        };
        // Renderer identity survives alternating presentation snapshot banks.
        // Ambient and decor share a renderer, so distinguish them in the pass key.
        gpuProfileRoomCategory(draw.owner,behindHero);
        const GpuOwnerScope ownerTiming({this,draw.owner,std::uint32_t(index),
            diagnosticsEnabled&&performanceObservation.serializedGpu?
                std::uint32_t(extent(left,right,width)*extent(top,bottom,height)):0u,
            unsigned(material),unsigned(background)*2+unsigned(behindHero)+unsigned(ambient)*4,grab});
        if(effects&&!grab&&effects->sharedRoomLights()){
            if(transformedView){C2D_ViewRestore(&saved);transformedView=false;}
            effects->flushRoomLights();
        }
        selectMaterial(grab?RoomMaterial::Normal:material);
        if(grab){
            if(transformedView){C2D_ViewRestore(&saved);transformedView=false;}
            float captureTop=top,captureBottom=bottom;
            if(plainView&&flatSprite&&effects->sharedRoomLights()&&
               effects->roomLightNeedsCapture(left,top,right,bottom)){
                LightBatch planned;
                const auto scissor=[&](float l,float t,float r,float b){
                    return lightScissor(l,t,r,b,unsigned(width),unsigned(height));
                };
                planned.append(scissor(left,top,right,bottom));
                // Only look ahead through consecutive static, flat grab draws.
                // Full sprite bounds conservatively include pass-clipped art.
                for(std::size_t next=index+1;next<std::min(last,bank.draws.size())&&
                    planned.size()<LightBatch::capacity;++next){
                    const auto& candidate=bank.draws[next];
                    if(ownerOffset_){const auto shift=ownerOffset_(candidate.owner,ownerOffsetContext_);
                        if(shift.x!=0||shift.y!=0)break;}
                    const auto candidateMaterial=roomMaterial(candidate);
                    if(candidateMaterial!=RoomMaterial::LinearLight&&
                       candidateMaterial!=RoomMaterial::LinearDodge&&
                       candidateMaterial!=RoomMaterial::VividLight)break;
                    if(bank.scaleFor(candidate)||candidate.grass.amplitude!=0||candidate.grass.push!=0)break;
                    RoomFlatSprite projected;
                    if(!roomFlatSprite(bank.atlas.sprites[candidate.sprite],candidate.values,
                                       cameraX,cameraY,width,height,focal,projected))break;
                    const auto r=scissor(projected.x,projected.y,projected.right,projected.bottom);
                    if(!planned.accepts(r))break;
                    planned.append(r);
                    captureTop=std::min(captureTop,projected.y);
                    captureBottom=std::max(captureBottom,projected.bottom);
                }
            }
            effects->beginRoomLight(left,top,right,bottom,captureTop,captureBottom);
            C2D_SetTintMode(C2D_TintMult);
        }
        const auto finishLight=[&]{
            if(!grab)return;
            if(transformedView){C2D_ViewRestore(&saved);transformedView=false;}
            effects->endRoomLight(material,left,top,right,bottom);
            // Projected bounds do not include an external view transform.
            if(!plainView)effects->flushRoomLights();
            C2D_ViewRestore(&saved);
            activeMaterial=RoomMaterial::Normal;
            C2D_SetTintMode(C2D_TintMult);
        };
        const C2D_Image page=uploaded_?C2D_Image{const_cast<C3D_Tex*>(&pool_[sprite.page]),nullptr}:
            C2D_SpriteSheetGetImage(pages_[sprite.page],0);
        Tex3DS_SubTexture sub{};sub.width=static_cast<u16>(sprite.width);sub.height=static_cast<u16>(sprite.height);
        sub.left=(sprite.x+sprite.width*clip.left)/page.tex->width;
        sub.right=(sprite.x+sprite.width*clip.right)/page.tex->width;
        sub.top=1-(sprite.y+sprite.height*clip.top)/page.tex->height;
        sub.bottom=1-(sprite.y+sprite.height*clip.bottom)/page.tex->height;
        auto tintRgb=std::array<float,3>{v[7],v[8],v[9]};
        if(litOwners_&&std::binary_search(litOwners_->begin(),litOwners_->end(),draw.owner))
            for(unsigned i=0;i<3;++i)tintRgb[i]=std::clamp(tintRgb[i]*ambientRatio_[i],0.f,1.f);
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(tintRgb[0],tintRgb[1],tintRgb[2],v[10]*alpha),1);
        if(diagnosticsEnabled&&performanceObservation.row)++performanceObservation.submittedQuads;
        observeRoomCoverage(left,top,right,bottom,width,height);
        // fog-mesh.flag: large soft sprites draw only the strips that cover every
        // texel with non-zero filtered alpha. The rest of the quad blends nothing.
        const RoomTrim* trim=nullptr;unsigned trimCount=0;
        if(!bank.trimRange.empty()&&!grab&&!scaleAnimation&&draw.grass.amplitude==0&&draw.grass.push==0&&
           clip.left==0&&clip.top==0&&clip.right==1&&clip.bottom==1&&
           (material==RoomMaterial::Normal||material==RoomMaterial::Screen||material==RoomMaterial::Additive)&&
           (right-left)*(bottom-top)>=trimMinimumArea&&draw.sprite<bank.trimRange.size()&&bank.trimRange[draw.sprite]){
            trim=&bank.trims[bank.trimRange[draw.sprite]>>8];trimCount=bank.trimRange[draw.sprite]&255u;
        }
        cpu.mark(PerformanceSection::RoomC2D);
        const auto drawTrim=[&](float x,float y,float scaleX,float scaleY){
            bool ok=true;
            for(unsigned k=0;k<trimCount;++k){
                const auto& t=trim[k];
                Tex3DS_SubTexture part{};part.width=static_cast<u16>(t.col1-t.col0);part.height=static_cast<u16>(t.row1-t.row0);
                part.left=(sprite.x+float(t.col0))/page.tex->width;part.right=(sprite.x+float(t.col1))/page.tex->width;
                part.top=1-(sprite.y+float(t.row0))/page.tex->height;part.bottom=1-(sprite.y+float(t.row1))/page.tex->height;
                ok&=C2D_DrawImageAt({page.tex,&part},x+t.col0*scaleX,y+t.row0*scaleY,0,&tint,scaleX,scaleY);
            }
            ++trimDraws_;trimQuads_+=trimCount;
            return ok;
        };
        if(flatSprite){
            // Axis-aligned, constant-depth quads share the original view and
            // batch together. Reflections retain their signed sprite scales.
            if(transformedView){C2D_ViewRestore(&saved);transformedView=false;}
            if(diagnosticsEnabled&&performanceObservation.row)++performanceObservation.flatQuads;
            ++draws_;
            if(trim&&flat.scaleX>0&&flat.scaleY>0){
                if(!drawTrim(flat.x,flat.y,flat.scaleX,flat.scaleY))++drawFailures_;
            }
            else if(!C2D_DrawImageAt({page.tex,&sub},flat.x,flat.y,0,&tint,flat.scaleX,flat.scaleY))++drawFailures_;
            finishLight();
            continue;
        }
        RoomRotatedSprite rotated;
        if(plainView&&roomRotatedSprite(sprite,v,cameraX,cameraY,width,height,focal,rotated)){
            if(transformedView){C2D_ViewRestore(&saved);transformedView=false;}
            // Keep atlas UVs canonical: top < bottom denotes packed rotation,
            // not a vertical mirror. C2D mirrors signed rectangle dimensions.
            const auto quad=centeredSpriteQuad(rotated.width,rotated.height);
            const C2D_DrawParams params{{rotated.x,rotated.y,quad.width,quad.height},
                                       {quad.centerX,quad.centerY},0,rotated.angle};
            ++draws_;
            if(!C2D_DrawImage({page.tex,&sub},&params,&tint))++drawFailures_;
            finishLight();
            continue;
        }
        // Homogeneous projection retains shear and depth slopes on other quads.
        // C2D's orthographic screen matrix follows this view matrix; GPU division
        // by W therefore also gives perspective-correct texture interpolation.
        C3D_Mtx affine,view;Mtx_Identity(&affine);
        affine.r[0]=FVec4_New((focal*v[0]+width*.5f*v[13])/ppu,
            -(focal*v[1]+width*.5f*v[14])/ppu,0,focal*(v[2]-cameraX)+width*.5f*depth);
        affine.r[1]=FVec4_New((-focal*v[3]+height*.5f*v[13])/ppu,
            (focal*v[4]-height*.5f*v[14])/ppu,0,-focal*(v[5]-cameraY)+height*.5f*depth);
        affine.r[3]=FVec4_New(v[13]/ppu,-v[14]/ppu,0,depth);
        if(clip.homogeneousClip)
            affine.r[2]=FVec4_New(clip.depthClip[0]/ppu,-clip.depthClip[1]/ppu,0,clip.depthClip[2]);
        // An identity saved view leaves the finite affine matrix unchanged.
        // Preserve the general multiplication for transformed views.
        if(plainView)C2D_ViewRestore(&affine);
        else {Mtx_Multiply(&view,&saved,&affine);C2D_ViewRestore(&view);}
        transformedView=true;
        ++draws_;
        const float matrixScaleX=(sprite.right-sprite.left)*ppu/sprite.width,
                    matrixScaleY=(sprite.top-sprite.bottom)*ppu/sprite.height;
        if(trim){
            if(!drawTrim(sprite.left*ppu,-sprite.top*ppu,matrixScaleX,matrixScaleY))++drawFailures_;
        }
        else if(!C2D_DrawImageAt({page.tex,&sub},sprite.left*ppu,-sprite.top*ppu,0,&tint,
                       matrixScaleX,matrixScaleY))++drawFailures_;
        finishLight();
    }
    if(transformedView)C2D_ViewRestore(&saved);
    if(effects)effects->flushRoomLights();
    if(blendChanged){
        C2D_Flush();
        effects::screenSprite(false);
    }
    C2D_SetTintMode(C2D_TintSolid);
}
}
