#include "sprite_layout.hpp"
#include "text_capture.hpp"
#include "text_pack.hpp"
#include <memory>
#include <fstream>
#include <iterator>
#include "build_config.hpp"
#include "crash_breadcrumb.hpp"
#include "loading_screen.hpp"
#include "simulation.hpp"
#include "uumuu_graphics.hpp"
#include "monomon_movie.hpp"
#include "enemy_b_arena.hpp"
#include "renderer.hpp"
#include "door_prompt_text.hpp"
#include "top_screen_ui_layout.hpp"
#include "terrain.hpp"
#include "breakable.hpp"
#include "diagnostics.hpp"
#include "tutorial.hpp"
#include "test_command.hpp"
#include "test_frame_capture.hpp"
#include "room_renderer.hpp"
#include "scene.hpp"
#include "world_obstacles.hpp"
#include "audio_player.hpp"
#include "enemy_audio_probe.hpp"
#include "input_replay.hpp"
#include "tutorial_replay.hpp"
#include "console_metrics.hpp"
#include "console_test.hpp"
#include "frame_profile.hpp"
#include "diagnostic_game_flags.hpp"
#include "static_decor.hpp"
#include "gpu_profile.hpp"
#include "gpu_kick.hpp"
#include "save_writer.hpp"
#include "menu.hpp"
#include "menu_renderer.hpp"
#include "lighting.hpp"
#include "screen_effects.hpp"
#include "hud.hpp"
#include "hud_renderer.hpp"
#include "inventory_renderer.hpp"
#include "inventory_layout.hpp"
#include "pause_menu.hpp"
#include "map_renderer.hpp"
#include "cornifer_data.hpp"
#include "cartographer_audio.hpp"
#include "platform.hpp"
#include "frame_pipeline.hpp"
#include "tutorial_render_state.hpp"
#include "crossroads_enemy_arena.hpp"
#include <3ds.h>
// Room loading and contact scratch require extra stack; diagnostic replay also
// runs worker simulation on main and needs the worker's larger budget.
extern "C" { u32 __stacksize__ = hk3ds::diagnosticsEnabled ? hk3ds::CpuWorker::stackBytes : 64 * 1024; }
#include <citro2d.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <new>

#include "diagnostic_overlay.hpp"
#include "runtime_diagnostics.hpp"
#if HK3DS_DIAGNOSTICS
#define diagnosticPrint std::printf
#else
template<class... T> static void diagnosticPrint(const char*,T&&...) {}
#endif
#ifndef HK3DS_BUILD_COMMIT
#define HK3DS_BUILD_COMMIT "unknown"
#endif
static constexpr u32 diagnosticChord=hk3ds::diagnosticCombo;
static_assert(diagnosticChord==(KEY_L|KEY_R|KEY_SELECT));
static bool diagnosticChordHeld(){return hk3ds::diagnosticComboHeld(hidKeysHeld());}
static hk3ds::CrossroadsEnemyArena crossroadsEnemyArena;
static hk3ds::AnimationBank greenpathArt;
static hk3ds::HeroRenderer greenpathRenderer;
#include <sys/stat.h>
#include "render_memory.hpp"

#if HK3DS_DIAGNOSTICS
extern "C" { volatile hk3ds::DiagnosticSnapshot hk3ds_diagnostics{}; }
extern "C" { volatile std::uint32_t hk3ds_test_shutdown=0; }
extern "C" { volatile std::uint32_t hk3ds_test_video_ticks=0; }
extern "C" { volatile std::uint32_t hk3ds_runtime_phase=0; }
extern "C" { volatile hk3ds::TestCommand hk3ds_test_command{}; }
extern "C" { volatile hk3ds::TestFrameCapture hk3ds_test_frames{}; }
// Read only while profiling; bit 31 cycles presentation-only ablations.
extern "C" { volatile std::uint32_t hk3ds_perf_disable_mask=HK3DS_PERF_DISABLE_MASK; }
#else
extern hk3ds::TestCommand hk3ds_test_command;
extern hk3ds::TestFrameCapture hk3ds_test_frames;
static std::uint32_t hk3ds_perf_disable_mask=0;
#endif
#if HK3DS_DIAGNOSTICS
static bool testFrameCaptureActive=false;
#else
static constexpr bool testFrameCaptureActive=false;
#endif
#if HK3DS_DIAGNOSTICS
extern "C" { volatile std::uint32_t hk3ds_replay_result=0; }
#endif
// New keeps libctru's ordinary remainder; the Old allocator caps both heaps.
extern "C" { u32 __ctru_linear_heap_size=hk3ds::gameLinearHeapBytes; }

#include "load_diagnostics.hpp"
#include "heap_guard.hpp"

static hk3ds::CrashBreadcrumb breadcrumb;
static constexpr const char* breadcrumbPath="sdmc:/3ds/HK3DS/last-run.txt";
static bool sessionFailed=false;
static bool loadFailureAcknowledged=false;
static bool loadPanelCaptureFixture=false;

// Heap-guard report: appended to the crash file and reflected as a heap_corrupt breadcrumb.
static void heapGuardSink(const char* line)
{
    if(auto* file=std::fopen("sdmc:/3ds/HK3DS/crash.txt","a")){
        std::fputs(line,file);std::fputc('\n',file);std::fclose(file);
    }
    breadcrumb.set(breadcrumb.reason,sizeof(breadcrumb.reason),"heap_corrupt");
    breadcrumb.write(breadcrumbPath);
}

static void recordBreadcrumb(const char* stage,const char* reason="")
{
    if(hk3ds::heapguard::check(stage))reason="heap_corrupt";
    breadcrumb.set(breadcrumb.stage,sizeof(breadcrumb.stage),stage);
    breadcrumb.set(breadcrumb.reason,sizeof(breadcrumb.reason),reason);
    breadcrumb.linearFree=linearSpaceFree();
    const auto allocated=mallinfo().uordblks;
    const auto heapSize=envGetHeapSize();
    breadcrumb.heapFree=allocated<heapSize?heapSize-allocated:0;
    breadcrumb.write(breadcrumbPath);
    if(reason&&reason[0]&&std::strcmp(reason,"normal_exit"))hk3ds::loadDiagnostics.error(stage,reason);
    else hk3ds::loadDiagnostics.stage(stage);
    if(hk3ds::runtimeDiagnostics.enabled())if(auto* file=std::fopen("sdmc:/3ds/HK3DS/scene-memory.txt","a")){
        std::fprintf(file,"scene=%s gate=%s stage=%s reason=%s linear_free=%lu heap_free=%lu vram_free=%lu\n",
            breadcrumb.scene,breadcrumb.gate,breadcrumb.stage,breadcrumb.reason,
            static_cast<unsigned long>(breadcrumb.linearFree),static_cast<unsigned long>(breadcrumb.heapFree),
            static_cast<unsigned long>(vramSpaceFree()));
        std::fclose(file);
    }
}

__attribute__((noreturn,target("arm"))) static void crashHandler(ERRF_ExceptionInfo* exception,CpuRegisters* registers)
{
    if(auto* file=std::fopen("sdmc:/3ds/HK3DS/crash.txt","a")){
        std::fprintf(file,"type=%lu pc=%08lx lr=%08lx fault_address=%08lx\n",
            static_cast<unsigned long>(exception->type),static_cast<unsigned long>(registers->pc),
            static_cast<unsigned long>(registers->lr),static_cast<unsigned long>(exception->far));
        breadcrumb.writeFields(file);
        // Keep the kernel's raw PC: ARM/Thumb state and fault status are needed
        // to interpret its exception return offset against the matching ELF.
        std::fprintf(file,"fsr=%08lx cpsr=%08lx sp=%08lx\n",
            static_cast<unsigned long>(exception->fsr),static_cast<unsigned long>(registers->cpsr),
            static_cast<unsigned long>(registers->sp));
        for(unsigned i=0;i<13;++i)std::fprintf(file,"r%u=%08lx%c",i,
            static_cast<unsigned long>(registers->r[i]),i==12?'\n':' ');
        std::fputc('\n',file);
        std::fclose(file);
    }
    ERRF_ExceptionHandler(exception,registers);
}

static void showFailure(const char* reason)
{
    if(reason&&!std::strcmp(reason,"old_profile_unavailable"))
        reason="This area is unavailable on Old 3DS.\nIts graphics package is missing or exceeds\nthe Old 3DS memory budget.";
    gfxInitDefault();
    consoleInit(GFX_BOTTOM,nullptr);
    const auto print=[&](){
        consoleClear();
        if(const auto* error=hk3ds::loadDiagnostics.current()){
            std::printf("HK3DS could not continue.\n\n%s\n%s -> %s\nGate: %s\n%s\nLoad: %lu ms\nFree KiB L%lu V%lu H%lu\n",
                error->stage,error->from,error->to,error->gate,error->detail,
                static_cast<unsigned long>(error->elapsedMs),static_cast<unsigned long>(error->memory.linear/1024),
                static_cast<unsigned long>(error->memory.vram/1024),static_cast<unsigned long>(error->memory.heap/1024));
        }else std::printf("HK3DS could not continue.\n\n%s\n",reason?reason:"");
        std::printf("\nSee /3ds/HK3DS/last-run.txt\n\nA: Close");
        gfxFlushBuffers();gfxSwapBuffers();
    };
    print();
    // Early graphics/font initialization failures still need an acknowledgement.
    hidScanInput();bool released=!(hidKeysHeld()&KEY_A);
    while(aptMainLoop()){
        hidScanInput();if(!(hidKeysHeld()&KEY_A))released=true;
        if(released&&(hidKeysDown()&KEY_A)){
            hk3ds::loadDiagnostics.input(true);
            if(!hk3ds::loadDiagnostics.visible())break;
            print();
        }
        gspWaitForVBlank();
    }
    gfxExit();
}

[[noreturn]] static void allocationFailed()
{
    breadcrumb.set(breadcrumb.reason,sizeof(breadcrumb.reason),"allocation_failed");
    breadcrumb.write(breadcrumbPath);
    gfxExit();
    showFailure(hk3ds::graphicsProfile().postprocessing?"Not enough memory for this area.\nUse Graphics: Old 3DS in Options.":"Not enough memory for this area.\nThe Old 3DS memory budget was exceeded.");
    std::_Exit(1);
}

namespace
{
static hk3ds::UumuuGraphics archiveGraphics;
static hk3ds::HeroRenderer monomonRenderer;
static const hk3ds::AnimationBank* monomonArtwork=nullptr;
static hk3ds::MonomonMovie monomonMovie;
static double monomonMovieStep=0;
static bool monomonMovieFailed=false;

struct SceneEnemyGraphics {
    std::array<hk3ds::HeroRenderer,hk3ds::SceneEnemies::bankCount-1> owned;
    std::array<const hk3ds::HeroRenderer*,hk3ds::SceneEnemies::bankCount> renderers{};
    std::array<const hk3ds::AnimationBank*,hk3ds::SceneEnemies::bankCount> banks{};
    void release(){for(auto& r:owned)r.release();renderers.fill(nullptr);banks.fill(nullptr);}
    bool load(const hk3ds::SceneEnemies& enemies,const hk3ds::HeroRenderer& crawler){
        release();
        for(unsigned i=0;i<banks.size();++i)if(enemies.usesBank(i)){
            banks[i]=&enemies.bank(i);
            if(i==0){renderers[i]=&crawler;continue;}
            if(i>7&&!hk3ds::roomTexturesFit(banks[i]->pageCount,linearSpaceFree())){release();return false;}
            char prefix[64];
            const char* stem=i<5?hk3ds::crossroadsEnemyRegistry[i-1].assetStem:i==5?"enemy-b":i==6?"enemy-c":i==7?enemies.encounterArtStem():i==8?"elder-baldur":i==9?"zombie-guard":i==10?"spitter":i==11?"fungus-flyer":i==12?"jellyfish":i==13?"fungoon-baby":"fung-crawler";
            std::snprintf(prefix,sizeof(prefix),"%s-page-",stem);
            if(!(i==7?owned[i-1].loadBudgeted("romfs:",*banks[i],prefix):
                      owned[i-1].load("romfs:",*banks[i],prefix)))return false;
            // Original Mantis atlas and Elder Baldur outlines use filtered minification.
            if(i==8||(i==7&&std::strcmp(stem,"mantis")==0))
                for(unsigned page=0;page<banks[i]->pageCount;++page)
                    if(!owned[i-1].setLinearFilter(page))return false;
            renderers[i]=&owned[i-1];
        }return true;
    }
};
// Own diagnostic room, not an extracted Hollow Knight scene.
constexpr hk3ds::Solid room[] = {
    {{-16,-2,32,0}}, {{-16,0,-15,15}}, {{25,0,26,15}},
    {{-6,2,-2,2.6f}}, {{3,4,7,4.6f}}, {{11,0,12,7}}, {{17,5,22,5.6f}}
};
constexpr hk3ds::World fixtureWorld{room, sizeof(room)/sizeof(room[0])};
constexpr float pixelsPerUnit = 16.0f;
hk3ds::AnimationBank abilityArt;
hk3ds::HeroRenderer abilityRenderer;
using Targets = std::array<hk3ds::CombatObject,5>;
void resetTargets(Targets& targets)
{
    targets={};
    // Own stationary test targets, not enemy prefabs or enemy AI.
    targets[0].bounds={5,0,6,2}; targets[0].name="Nail test target";
    targets[1].bounds={14,0,16,1}; targets[1].name="Pogo test target";
    for (int i=0; i<2; ++i) { targets[i].hasHealth=true; targets[i].health.hp=40; }
    targets[2].colliderEnabled=false;
    targets[3].name="_Enemies"; targets[3].colliderEnabled=false;
    targets[4].colliderEnabled=false;
}
float randomUnit(void* context)
{
    auto& state=*static_cast<std::uint32_t*>(context);
    // Local diagnostic RNG; exact Unity RNG sequence is not reproduced.
    state^=state<<13; state^=state>>17; state^=state<<5;
    return static_cast<float>(state>>8)/16777215.0f;
}
void reset(hk3ds::Simulation& simulation, const hk3ds::TerrainBank* terrain=nullptr)
{
    simulation = {};
    simulation.hero.position = {0, -hk3ds::tuning::bodyOffsetY + hk3ds::tuning::bodyHeight/2};
    simulation.hero.contacts.ground = true;
    if (terrain)
    {
        // Research placement beside the source actor; not the room entrance.
        auto p=terrain->crawlerSpawn; p.x-=3; float distance;
        if (hk3ds::rayTerrain(p,{0,-1},10,terrain->world(),distance)) p.y-=distance;
        simulation.hero.position={p.x,p.y-hk3ds::bodyBounds({}).bottom};
    }
    simulation.hero.actor = hk3ds::Actor::Idle;
    // Explicit test fixture: unlocked abilities, damaged, enough soul for focus.
    simulation.hero.abilities = {true,true,true,true};
    simulation.hero.vitals.health = 3;
    simulation.hero.vitals.soul = 99;
}
void sample(hk3ds::InputLatch& latch,u32 suppressed=0,bool shopButtons=false)
{
    const u32 keys = hidKeysHeld()&~suppressed;
    circlePosition circle{}; hidCircleRead(&circle);
    static hk3ds::HidUiState context;
    hk3ds::sampleHid(latch,keys,circle.dx,circle.dy,shopButtons,&context);
}
void draw(C3D_RenderTarget* target, const hk3ds::Simulation& simulation, const hk3ds::AnimationBank& bank,
          const hk3ds::HeroRenderer& renderer, const hk3ds::NailBank& nails, const Targets& targets, bool showHitboxes,
          const hk3ds::CrawlerRenderState& crawler, const hk3ds::AnimationBank& enemyBank, const hk3ds::HeroRenderer& enemyRenderer,
          const hk3ds::AnimationBank& buzzerBank,const hk3ds::HeroRenderer& buzzerRenderer,const hk3ds::World& world, bool sourceTerrain,
          const hk3ds::TutorialRenderState* tutorial=nullptr,const hk3ds::RoomVisualBank* visuals=nullptr,const hk3ds::RoomRenderer* roomRenderer=nullptr,
          hk3ds::LightingRenderer* lighting=nullptr,hk3ds::ScreenEffects* effects=nullptr,bool frameStarted=false,
          const hk3ds::AnimationBank* geoArtwork=nullptr,const hk3ds::HeroRenderer* geoRenderer=nullptr,
          const hk3ds::AnimationBank* doorArtwork=nullptr,const hk3ds::HeroRenderer* doorRenderer=nullptr,
          hk3ds::HudRenderer* hudRenderer=nullptr,const hk3ds::HudPresentation* hud=nullptr,
          const hk3ds::AnimationBank* shadeArtwork=nullptr,const hk3ds::HeroRenderer* shadeRenderer=nullptr,
          const hk3ds::AnimationBank* furyArtwork=nullptr,const hk3ds::HeroRenderer* furyRenderer=nullptr,const hk3ds::AnimationBank* eventArt=nullptr,const hk3ds::HeroRenderer* eventRenderer=nullptr,
          const hk3ds::AnimationBank* questArt=nullptr,const hk3ds::HeroRenderer* questRenderer=nullptr,
          const hk3ds::AnimationBank* enemyCArt=nullptr,const hk3ds::HeroRenderer* enemyCRenderer=nullptr,
          const hk3ds::AnimationBank* bossArtwork=nullptr,const hk3ds::HeroRenderer* bossRenderer=nullptr,
          hk3ds::MapRenderer* mapRenderer=nullptr,const hk3ds::SimpleMap* map=nullptr,
          hk3ds::InventoryRenderer* inventoryRenderer=nullptr,hk3ds::InventoryScreen* inventory=nullptr,
          const hk3ds::InventorySnapshot* inventorySnapshot=nullptr,const hk3ds::PauseMenu* menu=nullptr,
          const SceneEnemyGraphics* sceneArt=nullptr,
          C3D_RenderTarget* bottom=nullptr,const hk3ds::HudRenderer* bottomUi=nullptr,
          const hk3ds::AnimationBank* corniferArt=nullptr,const hk3ds::HeroRenderer* corniferRenderer=nullptr,
          const hk3ds::AnimationBank* stagArt=nullptr,const hk3ds::HeroRenderer* stagRenderer=nullptr)
{
    // Both screens read hero state from the supplied render snapshot.
    const auto& hero = simulation.hero;
    showHitboxes=hk3ds::diagnosticsEnabled&&showHitboxes;
    // The room and diagnostic outlines share the same presented camera/hero.
    const auto heroPose=tutorial?tutorial->heroRenderPosition():simulation.heroRenderPosition();
    auto decorShake=tutorial?tutorial->decorShake:hk3ds::Vec2{};
    if(tutorial){const auto chestShake=tutorial->furyChest.shake();decorShake.x+=chestShake.x;decorShake.y+=chestShake.y;}
    const bool fixedCamera=testFrameCaptureActive&&!hk3ds_test_frames.followCamera;
    const float cameraX = fixedCamera?hk3ds_test_frames.cameraX:(tutorial?tutorial->flow.camera.x:sourceTerrain ? heroPose.x : std::clamp(heroPose.x, -4.0f, 13.0f))+decorShake.x;
    const float worldPpu=tutorial?hk3ds::roomFocalLength()/38.104f:pixelsPerUnit;
    // The menu never moves the presented camera (user 2026-11-12: the pause lift was a bug).
    const float cameraY = (fixedCamera?hk3ds_test_frames.cameraY:(tutorial?tutorial->flow.camera.y:std::max(5.0f, heroPose.y - 1.0f))+decorShake.y);
    if(tutorial&&((tutorial->boss.active()&&!tutorial->sceneActive)||tutorial->greenpath.fixture)){
        // The arena releases room textures before loading its boss atlas.
        visuals=nullptr;roomRenderer=nullptr;effects=nullptr;lighting=nullptr;
    }
    if(!frameStarted){
    {hk3ds::FrameProfile::Scope timing(hk3ds::frameProfile,hk3ds::FrameProfile::FrameBegin);
    hk3ds::runtimePhase(hk3ds::RuntimePhase::GpuSync);
    C3D_FrameSync();
    for(unsigned i=1;i<hk3ds::graphicsProfile().frameDivisor;++i)gspWaitForVBlank();}
    {hk3ds::FrameProfile::Scope timing(hk3ds::frameProfile,hk3ds::FrameProfile::GpuWait);
    hk3ds::runtimePhase(hk3ds::RuntimePhase::GpuBegin);
    if (!C3D_FrameBegin(0)) return;}
    hk3ds::gpuProfilePreviousFrame();}
    hk3ds::DrawCpuLap drawCpu;
    // Pass boundaries where the GPU may start on the frame so far while the CPU
    // emits the rest. Serialized GPU profiling owns the queue instead.
    const auto kickGpu=[&](hk3ds::GpuKickPoint point){
        if(!visuals||!roomRenderer||!hk3ds::gpuKickEnabled(point)||
           hk3ds::performanceObservation.serializedGpu)return;
        if(hk3ds::kickGpuFrame()&&hk3ds::performanceObservation.coverage)
            hk3ds::performanceObservation.coverage->gpuKicks|=unsigned(point);
        drawCpu.mark(hk3ds::PerformanceSection::CpuKick);
    };
    {hk3ds::FrameProfile::Scope timing(hk3ds::frameProfile,hk3ds::FrameProfile::Draw);
    hk3ds::runtimePhase(hk3ds::RuntimePhase::World);
    // Bottom metadata persists when the map reuses its retained framebuffer.
    hk3ds::textcapture::select(0,true);
    C2D_TargetClear(target, visuals?C2D_Color32(0,0,0,255):C2D_Color32(24,28,40,255)); C2D_SceneBegin(target);
    // Painter order already handles overlapping translucent sprites. A depth
    // buffer would reject later layers after their homogeneous projection.
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    if(!hk3ds::drawEnemyBArena()) {
    const auto presentationTime=tutorial?tutorial->presentationTime():simulation.presentationTime();
    if(roomRenderer){
        roomRenderer->resetDrawStats();
        const bool grassOverride=testFrameCaptureActive&&hk3ds_test_frames.grassSeconds>=0;
        roomRenderer->setLevelTime(grassOverride?hk3ds_test_frames.grassSeconds:(tutorial?tutorial->levelSeconds():0));
        roomRenderer->setGrassVisible(!testFrameCaptureActive||!hk3ds_test_frames.excludeGrass);
        roomRenderer->setOwnerOffset(tutorial&&tutorial->sceneEnemies.mantisGateOffset!=0?
            +[](std::uint32_t owner,const void* context){
                const auto& state=*static_cast<const hk3ds::SceneEnemyRenderState*>(context);
                return hk3ds::MantisLords::gateOwner(owner)?hk3ds::Vec2{0,state.mantisGateOffset}:hk3ds::Vec2{};
            }:nullptr,tutorial?&tutorial->sceneEnemies:nullptr);
    }
    if(lighting&&lighting->scene().hasGrade){
        const auto values=lighting->sampleScene(hero.position.x,hero.position.y);
        if(effects)effects->setCameraColor(values.color);
        if(roomRenderer)roomRenderer->setSceneAmbient(values.ambientRatio,&lighting->scene().litOwners);
    }else if(roomRenderer)roomRenderer->setSceneAmbient({1,1,1},nullptr);
    if(testFrameCaptureActive){hk3ds_test_frames.observedCameraX=cameraX;hk3ds_test_frames.observedCameraY=cameraY;}
    if(testFrameCaptureActive&&tutorial&&tutorial->testIsolateDecor&&roomRenderer)
        roomRenderer->drawDecor(tutorial->decorVisuals,cameraX,cameraY,worldPpu);
    else {
    const auto screenX = [cameraX,worldPpu](float x){ return 200 + (x-cameraX)*worldPpu; };
    const auto screenY = [cameraY,worldPpu](float y){ return 120 - (y-cameraY)*worldPpu; };
    for (std::size_t i=0;i<world.count;++i)
    {
        const auto b = world.solids[i].bounds;
        if (screenX(b.right) < 0 || screenX(b.left) > 400 || screenY(b.bottom) < 0 || screenY(b.top) > 240) continue;
        C2D_DrawRectSolid(screenX(b.left),screenY(b.top),0,(b.right-b.left)*worldPpu,(b.top-b.bottom)*worldPpu,C2D_Color32(62,79,94,255));
    }
    const auto visible=[](std::uint32_t owner,void* context){
        if(testFrameCaptureActive&&hk3ds::frameCaptureExcludesOwner(hk3ds_test_frames,testFrameCaptureActive,owner))return 0.f;
        return static_cast<const hk3ds::TutorialRenderState*>(context)->visualAlpha(owner);
    };
    const float lightDepth=lighting&&lighting->ready()?lighting->config().transform[2]:0;
    auto* mutableTutorial=const_cast<hk3ds::TutorialRenderState*>(tutorial);
    if(visuals&&roomRenderer&&hk3ds::roomVisibleListEnabled)roomRenderer->prepareVisibleDraws(*visuals,cameraX,cameraY);
    if(effects){
        if(!roomRenderer->reuseBackground(*effects,*visuals,mutableTutorial?&mutableTutorial->ambientVisuals:nullptr,
            mutableTutorial?&mutableTutorial->decorVisuals:nullptr,cameraX,cameraY,worldPpu,visible,mutableTutorial)){
        effects->beginBackground();
        roomRenderer->drawBackground(*visuals,mutableTutorial?&mutableTutorial->ambientVisuals:nullptr,
            cameraX,cameraY,worldPpu,visible,mutableTutorial,effects->backgroundPassWidth(),effects->backgroundPassHeight(),0,false,effects,true,
            mutableTutorial?&mutableTutorial->decorVisuals:nullptr,tutorial?&tutorial->worldSwitchVisuals:nullptr);
        roomRenderer->draw(*visuals,cameraX,cameraY,worldPpu,visible,
            mutableTutorial,effects->backgroundPassWidth(),effects->backgroundPassHeight(),false,0,false,effects,true);
        effects->compositeBackground();
        }
    }
    drawCpu.mark(hk3ds::PerformanceSection::CpuBackground);
    kickGpu(hk3ds::GpuKickPoint::AfterBackground);
    if(visuals&&roomRenderer)roomRenderer->drawBackground(*visuals,mutableTutorial?&mutableTutorial->ambientVisuals:nullptr,
        cameraX,cameraY,worldPpu,visible,mutableTutorial,400,240,lightDepth,false,effects,false,
        mutableTutorial?&mutableTutorial->decorVisuals:nullptr,tutorial?&tutorial->worldSwitchVisuals:nullptr);
    if(effects&&lighting&&lighting->ready())lighting->drawHeroLight(heroPose,simulation.hero.facingRight,tutorial?tutorial->heroLightScale:lighting->config().transform[0],cameraX,cameraY,*effects);
    if(lightDepth&&visuals&&roomRenderer)roomRenderer->drawBackground(*visuals,mutableTutorial?&mutableTutorial->ambientVisuals:nullptr,
        cameraX,cameraY,worldPpu,visible,mutableTutorial,400,240,lightDepth,true,effects,false,
        mutableTutorial?&mutableTutorial->decorVisuals:nullptr,tutorial?&tutorial->worldSwitchVisuals:nullptr);
    drawCpu.mark(hk3ds::PerformanceSection::CpuBehindHero);
    kickGpu(hk3ds::GpuKickPoint::BeforeActors);
    const auto drawSceneEnemy=[&](const hk3ds::SceneEnemyDraw& a){
        if(sceneArt->banks[a.bank]&&sceneArt->renderers[a.bank]){
            const float ppu=hk3ds::roomFocalLength()/(38.1f+a.depth);
            sceneArt->renderers[a.bank]->drawAnimation(a.animation,*sceneArt->banks[a.bank],
                200+(a.position.x-cameraX)*ppu,120-(a.position.y-cameraY)*ppu,ppu,a.scaleX,a.scaleY,1,a.rotation);
        }
    };
    const float glowDepth=lighting&&lighting->ready()?lighting->config().glowTransform[4]:.004f;
    // Preserve source actors behind the glow while Mantis foreground actors draw later.
    if(tutorial&&tutorial->sceneActive&&sceneArt)
        for(unsigned i=0;i<tutorial->sceneEnemies.count;++i){const auto& a=tutorial->sceneEnemies.actors[i];
            if(!a.foreground&&a.depth>glowDepth)drawSceneEnemy(a);
        }
    if(lighting&&tutorial)lighting->drawLanternGlow(heroPose,tutorial->darkness,cameraX,cameraY);
    // The VividLight donut is at hero depth, after the HeroLight/decor split.
    if(effects&&lighting&&lighting->ready())lighting->drawHeroGlow(heroPose,simulation.hero.facingRight,cameraX,cameraY,*effects);
    hk3ds::gpuProfileCategory(hk3ds::PerformanceSection::GpuActors);
    for (std::size_t i=0;i<world.shapeCount&&(!tutorial||showHitboxes||!roomRenderer||!roomRenderer->ready());++i)
    {
        const auto& s=world.shapes[i]; if(!s.enabled)continue; const auto bounds=s.bounds;
        if (screenX(bounds.right)<0 || screenX(bounds.left)>400 || screenY(bounds.bottom)<0 || screenY(bounds.top)>240) continue;
        if ((s.flags&1) && !showHitboxes) continue;
        const u32 color=(s.flags&1)?C2D_Color32(220,170,60,255):C2D_Color32(110,150,170,255);
        const auto* points=world.vertices+s.first;
        const unsigned edges=s.kind==0?s.count:s.count-1;
        for (unsigned j=0;j<edges;++j)
        {
            const auto a=points[j],b=points[(j+1)%s.count];
            C2D_DrawLine(screenX(a.x),screenY(a.y),color,screenX(b.x),screenY(b.y),color,1,0);
        }
    }
    if (nails.ready() && !sourceTerrain) for (int i=0; i<2; ++i)
    {
        const auto& targetObject=targets[i];
        if (targetObject.health.dead) continue;
        const auto b=targetObject.bounds;
        const auto color=targetObject.health.evasion>0 ? C2D_Color32(255,220,180,255) : C2D_Color32(150,85,65,255);
        C2D_DrawRectSolid(screenX(b.left),screenY(b.top),0,(b.right-b.left)*worldPpu,(b.top-b.bottom)*worldPpu,color);
    }
    if(tutorial&&doorArtwork&&doorRenderer&&doorRenderer->ready()&&tutorial->greatDoor.visible())
    {
        // Source scene tk2dSprite at z=.61; converted geometry includes its
        // full parent transform. Final Break retains v03 until the scene fade.
        hk3ds::AnimationPlayer door;
        door.play(doorArtwork->find(tutorial->greatDoor.spriteName()));
        const float ppu=hk3ds::roomFocalLength()/(38.1f+.61f);
        const float x=200-cameraX*ppu,y=120+cameraY*ppu;
        const auto* sprite=door.current(*doorArtwork);
        if(sprite&&x+sprite->right*ppu>=0&&x+sprite->left*ppu<=400&&
           y-sprite->bottom*ppu>=0&&y-sprite->top*ppu<=240)
            doorRenderer->drawAnimation(door,*doorArtwork,x,y,ppu,1);
    }
    if(tutorial&&geoArtwork&&geoRenderer&&geoRenderer->ready())
    {
        const auto geoDraw=[&](const hk3ds::GeoVisualDraw& item,hk3ds::Vec2 position)
        {
            if(!item.visible)return;
            const float ppu=hk3ds::roomFocalLength()/(38.1f+item.depth);
            const float x=200+(position.x-cameraX)*ppu,y=120-(position.y-cameraY)*ppu;
            const auto* frame=item.player.current(*geoArtwork);
            if(!frame)return;
            float left=std::min(frame->left*item.scale.x,frame->right*item.scale.x);
            float right=std::max(frame->left*item.scale.x,frame->right*item.scale.x);
            float bottom=std::min(frame->bottom*item.scale.y,frame->top*item.scale.y);
            float top=std::max(frame->bottom*item.scale.y,frame->top*item.scale.y);
            if(item.rotation!=0) {
                // A rotating cap/projectile must not disappear while a corner is visible.
                const float extent=std::max(std::abs(left),std::abs(right))+std::max(std::abs(bottom),std::abs(top));
                left=bottom=-extent;right=top=extent;
            }
            if(x+right*ppu<0||x+left*ppu>400||
               y-bottom*ppu<0||y-top*ppu>240)return;
            geoRenderer->drawAnimation(item.player,*geoArtwork,x,y,ppu,item.scale.x,item.scale.y,1,item.rotation);
        };
        for(const auto& item:tutorial->geoDraws)geoDraw(item,item.position);
    }
    const auto furyDraw=[&](const hk3ds::GeoVisualDraw& item,float opacity=1.f,float rotation=0.f){
        if(!item.visible||!furyArtwork||!furyRenderer||!furyRenderer->ready())return;
        const float ppu=hk3ds::roomFocalLength()/(38.1f+item.depth);
        const float x=200+(item.position.x-cameraX)*ppu,y=120-(item.position.y-cameraY)*ppu;
        const auto* frame=item.player.current(*furyArtwork);if(!frame)return;
        if(x+frame->right*item.scale.x*ppu<0||x+frame->left*item.scale.x*ppu>400||
           y-frame->bottom*item.scale.y*ppu<0||y-frame->top*item.scale.y*ppu>240)return;
        furyRenderer->drawAnimation(item.player,*furyArtwork,x,y,ppu,item.scale.x,item.scale.y,opacity,rotation+item.rotation);
    };
    if(tutorial&&furyArtwork){
        const auto& chest=tutorial->furyChest;
        for(auto part:{hk3ds::FuryPart::Back,hk3ds::FuryPart::Chest,hk3ds::FuryPart::Glow,
                       hk3ds::FuryPart::Shiny,hk3ds::FuryPart::Burst,hk3ds::FuryPart::Wave})
            furyDraw(chest.draw(part,presentationTime),part==hk3ds::FuryPart::Wave?chest.waveOpacity():1);
        for(const auto& particle:chest.particles())if(particle.draw.visible)
            furyDraw(particle.draw,chest.config().emitters[particle.emitter].v[23]);
        furyDraw(chest.effects()[0]);furyDraw(chest.effects()[1],chest.flashOpacity());
        furyDraw(chest.effects()[2],1,chest.tinkRotation()*.01745329252f);
    }
    const unsigned drawCrawlerCount=tutorial?tutorial->crawlerCount:1;
    for(unsigned enemyIndex=0;enemyIndex<drawCrawlerCount;++enemyIndex)
    {
        const auto& enemy=tutorial?tutorial->crawlers[enemyIndex]:crawler;
        if(!enemy.visible())continue;
        const auto p=enemy.renderPosition(presentationTime);
        enemyRenderer.drawAnimation(enemy.animation,enemyBank,screenX(p.x),screenY(p.y),worldPpu,
                                    enemy.alive() ? enemy.scaleX : enemy.corpseScaleX);
        if (showHitboxes && enemy.alive())
        {
            const auto b=enemy.bounds(); const u32 color=C2D_Color32(255,130,100,255);
            C2D_DrawLine(screenX(b.left),screenY(b.top),color,screenX(b.right),screenY(b.top),color,1,0);
            C2D_DrawLine(screenX(b.right),screenY(b.top),color,screenX(b.right),screenY(b.bottom),color,1,0);
            C2D_DrawLine(screenX(b.right),screenY(b.bottom),color,screenX(b.left),screenY(b.bottom),color,1,0);
            C2D_DrawLine(screenX(b.left),screenY(b.bottom),color,screenX(b.left),screenY(b.top),color,1,0);
        }
    }
    if(tutorial&&tutorial->tollArt.visible&&questArt&&questRenderer){
        const auto& art=tutorial->tollArt;
        for(unsigned i=0;i<art.count;++i){if(art.hidden[i])continue;const auto& pose=art.poses[i];
            questRenderer->drawAnimation(art.animation,*questArt,screenX(pose.position.x),screenY(pose.position.y),worldPpu,pose.scale.x,pose.scale.y);
        }
    }
    if(tutorial&&tutorial->worldQuestVisible&&questArt&&questRenderer){
        const auto p=tutorial->worldQuestPosition,scale=tutorial->worldQuestScale;
        questRenderer->drawAnimation(tutorial->worldQuestAnimation,*questArt,screenX(p.x),screenY(p.y),worldPpu,scale.x,scale.y);
    }
    if(tutorial&&stagArt&&stagRenderer){
        const auto& rest=tutorial->resting;
        if(rest.station.present){
            const auto& c=rest.station;
            if(c.standScale.x!=0)stagRenderer->drawAnimation(rest.standAnimation,*stagArt,screenX(c.toll.x),screenY(c.toll.y),worldPpu,c.standScale.x,c.standScale.y);
            if(c.bellScale.x!=0&&rest.stationOpen()&&rest.stagPhase!=hk3ds::StagPhase::Opening)
                stagRenderer->drawAnimation(rest.bellAnimation,*stagArt,screenX(c.bell.x),screenY(c.bell.y),worldPpu,c.bellScale.x,c.bellScale.y);
            if(rest.smokeVisible())stagRenderer->drawAnimation(rest.smokeAnimation,*stagArt,screenX(c.dust.x),screenY(c.dust.y),worldPpu,c.scale.x,c.scale.y);
        }
        if(rest.stagVisible()){
            auto p=rest.presentedStag(simulation.presentationTime());
            if(rest.smokeVisible()||rest.stagPhase==hk3ds::StagPhase::Departing){p.x+=rest.station.arrival.x-rest.station.position.x;p.y+=rest.station.arrival.y-rest.station.position.y;}
            stagRenderer->drawAnimation(rest.stagAnimation,*stagArt,screenX(p.x),screenY(p.y),worldPpu,rest.stagArtScale(),rest.station.scale.y);
        }
    }
    if(tutorial&&corniferArt&&corniferRenderer){
        const auto& npc=tutorial->cartographer;
        if(npc.present)corniferRenderer->drawAnimation(npc.animation,*corniferArt,screenX(npc.position.x),screenY(npc.position.y),worldPpu,npc.artScale(),hk3ds::corniferdata::scaleY);
        if(npc.card){hk3ds::AnimationPlayer card;card.play(corniferArt->require("Card"));corniferRenderer->drawAnimation(card,*corniferArt,screenX(npc.position.x),screenY(npc.position.y),worldPpu,1);}
    }
    if(tutorial&&eventArt&&eventRenderer){
        const auto& e=tutorial->conditionalEvents;
        const auto drawEvent=[&](const hk3ds::AnimationPlayer& a,hk3ds::Vec2 p,float sx=1.f,float sy=1.f){eventRenderer->drawAnimation(a,*eventArt,screenX(p.x),screenY(p.y),worldPpu,sx,sy);};
        if(e.mushroomVisible())drawEvent(e.mushroomAnimation,e.mushroomRenderPosition(presentationTime),e.mushroomArtScale());
        if(e.grimmkinVisible())drawEvent(e.grimmkinAnimation,e.grimmkinRenderPosition(presentationTime),e.grimmkinArtScale());
        if(e.flameVisible())drawEvent(e.flameAnimation,{146.49000046f,8.6700008f});
        for(const auto& p:e.projectiles)if(p.active)drawEvent(p.animation,p.renderPosition(presentationTime),p.scale(),p.scale());
        for(const auto& p:e.particles)if(p.active){const auto pos=p.renderPosition(presentationTime);eventRenderer->drawAnimation(p.animation,*eventArt,screenX(pos.x),screenY(pos.y),worldPpu,p.size,p.size,p.alpha());}
        if(e.sourceActiveCount())for(const auto& p:e.sourceParticles())if(p.active){
            const auto pos=p.renderPosition(presentationTime);const float size=e.sourceParticleSize(p);
            hk3ds::AnimationPlayer frame;frame.play(p.clip);
            if(p.clip)frame.frame=std::min<unsigned>(p.clip->count-1,unsigned(p.age/p.life*p.clip->count));
            eventRenderer->drawAnimation(frame,*eventArt,screenX(pos.x),screenY(pos.y),worldPpu,size,size,e.sourceParticleOpacity(p),p.angle);
        }
        if(e.flashAnimation.clip&&!e.flashAnimation.complete)drawEvent(e.flashAnimation,{heroPose.x,heroPose.y+1.5f});
    }
    if(tutorial&&shadeArtwork&&shadeRenderer&&tutorial->shade.visible()){
        const auto& shade=tutorial->shade;const auto p=shade.renderPosition(presentationTime);
        const float facing=shade.alive()?shade.artScale():1.f;
        hk3ds::AnimationPlayer mote;mote.clip=shadeArtwork->find("Shade Particle");
        for(const auto& particle:shade.particles())if(particle.active()){
            const auto local=particle.renderPosition(presentationTime);const float size=particle.size();
            const float x=screenX(p.x+local.x*facing),y=screenY(p.y+local.y);
            if(x<-worldPpu||x>400+worldPpu||y<-worldPpu||y>240+worldPpu)continue;
            shadeRenderer->drawAnimation(mote,*shadeArtwork,x,y,worldPpu,size,size);
        }
        if(shade.beamVisible()){
            hk3ds::AnimationPlayer beam;beam.clip=shadeArtwork->find("Recovery Beam");
            // Source child9392/renderer15586: active until End at recovery .5s.
            // Approximation: source-over preserves its alpha; full Lit/bloom
            // response remains part of the room material approximation.
            shadeRenderer->drawAnimation(beam,*shadeArtwork,screenX(p.x),screenY(p.y-.03f),worldPpu,-1.2682843f,1.2682844f,.3019608f);
        }
        // Source Dissipate hides both body renderers immediately, leaving motes.
        if(shade.state!=hk3ds::ShadeState::RecoveryDissipate){
            const bool hitTint=shade.alive()&&shade.hitNeutral()<1;
            if(hitTint){
                C2D_Flush();auto* tint=C3D_GetTexEnv(4);C3D_TexEnvInit(tint);
                C3D_TexEnvSrc(tint,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT);
                C3D_TexEnvFunc(tint,C3D_RGB,GPU_MODULATE);
                const auto neutral=static_cast<u8>(shade.hitNeutral()*255);
                C3D_TexEnvColor(tint,C2D_Color32(neutral,neutral,neutral,255));
            }
            shadeRenderer->drawAnimation(shade.animation,*shadeArtwork,screenX(p.x),screenY(p.y),worldPpu,facing);
            if(hitTint){C2D_Flush();C3D_TexEnvInit(C3D_GetTexEnv(4));}
            if(shade.deathSprite.clip)shadeRenderer->drawAnimation(shade.deathSprite,*shadeArtwork,screenX(p.x),screenY(p.y),worldPpu,1);
        }
        for(const auto& quad:shade.hitQuads())if(quad.hitActive()){
            const auto at=quad.renderPosition(presentationTime);
            const float x=screenX(at.x),y=screenY(at.y);
            // Streaks can extend several world units from their pivot.
            if(x<-8*worldPpu||x>400+8*worldPpu||y<-8*worldPpu||y>240+8*worldPpu)continue;
            hk3ds::AnimationPlayer effect;effect.play(shadeArtwork->require(quad.hitClip()));effect.advance(quad.age);
            shadeRenderer->drawAnimation(effect,*shadeArtwork,x,y,worldPpu,quad.hitScaleX(),quad.hitScaleY(),quad.hitOpacity(),quad.hitRotation());
        }
        if(shade.glowAnimation.clip&&!shade.glowAnimation.complete){
            // Original Death Glow child Transform 9361 has scale (3,3,1).
            // Approximate its luminous material with additive blending so
            // dark texels do not cover the scene with an opaque oval.
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
            shadeRenderer->drawAnimation(shade.glowAnimation,*shadeArtwork,screenX(p.x),screenY(p.y),worldPpu,3,3,1);
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        }
        if(shade.state==hk3ds::ShadeState::Slash||shade.state==hk3ds::ShadeState::SlashEnd)
            shadeRenderer->drawAnimation(shade.slashAnimation,*shadeArtwork,screenX(p.x-.04862976f*shade.scaleX),screenY(p.y-.0116996765f),worldPpu,1.2700316f*shade.scaleX);
    }
    if(tutorial&&monomonArtwork&&monomonRenderer.ready()){
        const auto& m=tutorial->monomon;
        const auto sprite=[&](bool visible,const hk3ds::AnimationPlayer& animation,hk3ds::Vec2 p,float scale){
            if(visible)monomonRenderer.drawAnimation(animation,*monomonArtwork,screenX(p.x),screenY(p.y),worldPpu,scale);
        };
        sprite(m.bodyVisible,m.bodyAnimation,m.body,m.bodyScale);
        if(m.glowAlpha>0)monomonRenderer.drawAnimation(m.glowAnimation,*monomonArtwork,screenX(m.glow.x),screenY(m.glow.y),worldPpu,hk3ds::monomon_data::glowScale,hk3ds::monomon_data::glowScale,m.glowAlpha);
        sprite(m.quirrelVisible,m.quirrelAnimation,m.quirrel,m.quirrelScale);
        sprite(m.maskVisible,m.maskAnimation,m.mask,1);
    }
    if(tutorial&&tutorial->uumuu.active&&archiveGraphics.ready()){
        const auto& u=tutorial->uumuu;
        // approximation: scene-local scattered green light replaces the source
        // arena bloom missing from the sprite pass. Fit against the copied
        // original Archives frame; HUD and later actors retain their own light.
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(114,194,146,35));
        for(unsigned i=0;i<hk3ds::UumuuGates::capacity;++i)if(u.gates[i].visible){
            const auto& gate=hk3ds::uumuu_gates_data::visuals[i];
            archiveGraphics.gateRenderer().drawAnimation(u.gates[i].animation,archiveGraphics.gateBank(),screenX(gate.position.x),screenY(gate.position.y),worldPpu,gate.scale.x,gate.scale.y,1,gate.rotation);
        }
        if(u.visible){
            // The source shell scatters turquoise/white light, rather than a
            // pink disc. Bounded translucent rings approximate its soft halo.
            for(unsigned ring=12;ring>0;--ring)
                C2D_DrawCircleSolid(screenX(u.position.x),screenY(u.position.y),0,
                    (2.f+ring*.25f)*worldPpu,C2D_Color32(184,255,224,3));
            archiveGraphics.bodyRenderer().drawAnimation(u.body,archiveGraphics.bodyBank(),screenX(u.position.x),screenY(u.position.y),worldPpu,1.06f,1.06f,.14f);
            archiveGraphics.bodyRenderer().drawAnimation(u.body,archiveGraphics.bodyBank(),screenX(u.position.x),screenY(u.position.y),worldPpu,u.artScale());
        }
        const auto drawQuirrel=[&](const hk3ds::UumuuQuirrelSprite& sprite,hk3ds::Vec2 p){
            if(sprite.visible&&!u.dead)archiveGraphics.quirrelRenderer().drawAnimation(sprite.animation,archiveGraphics.quirrelBank(),screenX(p.x),screenY(p.y),worldPpu,sprite.artScale(),1,1,sprite.rotationZ);
        };
        drawQuirrel(u.land,u.landPosition);drawQuirrel(u.slash,u.slashPosition);
        for(unsigned i=0;i<u.bloodCount;++i){const auto& blood=u.blood[i];
            hk3ds::AnimationPlayer particle;particle.play(archiveGraphics.effectsBank().require("Blood"),blood.tile,true);
            archiveGraphics.effectsRenderer().drawAnimation(particle,archiveGraphics.effectsBank(),
                screenX(blood.position.x),screenY(blood.position.y),worldPpu,blood.width,blood.height,blood.alpha,blood.rotation);
        }
        for(unsigned i=0;i<u.zapCount;++i){const auto& zap=u.zaps[i];
            if(zap.animation.clip){
                archiveGraphics.effectsRenderer().drawAnimation(zap.animation,archiveGraphics.effectsBank(),screenX(zap.position.x),screenY(zap.position.y),worldPpu,zap.scaleX,1,1,zap.rotation);
                continue;
            }
            // approximation: pooled chase-zap particles still await their source sheet.
            C2D_DrawCircleSolid(screenX(zap.position.x),screenY(zap.position.y),0,(zap.active?.65f:.35f)*worldPpu,
                C2D_Color32(220,255,255,zap.active?230:100));
        }
    }
    const auto drawSceneEnemyActors=[&](bool foreground){
        if(!(tutorial&&tutorial->sceneActive&&sceneArt))return;
        for(unsigned i=0;i<tutorial->sceneEnemies.count;++i){const auto& a=tutorial->sceneEnemies.actors[i];
            if(a.foreground==foreground&&(foreground||a.depth<=glowDepth))drawSceneEnemy(a);
        }
    };
    drawSceneEnemyActors(false);
    if(tutorial&&tutorial->sceneActive&&sceneArt){
        enemyCArt=sceneArt->banks[6];enemyCRenderer=sceneArt->renderers[6];
    }
    if(tutorial&&enemyCArt&&enemyCRenderer&&enemyCRenderer->ready()){
        for(const auto& a:tutorial->enemyC.actors)if(a.visible){
            enemyCRenderer->drawAnimation(a.animation,*enemyCArt,screenX(a.position.x),screenY(a.position.y),worldPpu,a.scaleX,a.scaleY,a.alpha);
            if(a.flash>0)C2D_DrawCircleSolid(screenX(a.position.x),screenY(a.position.y),0,.3f*worldPpu,
                C2D_Color32(255,255,255,static_cast<u8>(255*std::min(1.f,a.flash/.12f))));
        }
        for(unsigned i=0;i<tutorial->enemyC.globCount;++i){const auto& g=tutorial->enemyC.globs[i];
            enemyCRenderer->drawAnimation(g.animation,*enemyCArt,screenX(g.position.x),screenY(g.position.y),worldPpu,g.scale,g.scale,1);}
        for(unsigned i=0;i<tutorial->enemyC.shotCount;++i){const auto& s=tutorial->enemyC.shots[i];
            enemyCRenderer->drawAnimation(s.animation,*enemyCArt,screenX(s.position.x),screenY(s.position.y),worldPpu,s.scale,s.scale,1);}
        for(unsigned i=0;i<tutorial->enemyC.partCount;++i){const auto& part=tutorial->enemyC.parts[i];
            enemyCRenderer->drawAnimation(part.animation,*enemyCArt,screenX(part.position.x),screenY(part.position.y),worldPpu,part.scaleX,part.scaleY,1);}
    }
    if(tutorial&&tutorial->greenpath.fixture){
        const auto r=tutorial->greenpath.arena;
        C2D_DrawRectSolid(screenX(r.left),screenY(r.bottom),0,(r.right-r.left)*worldPpu,240,C2D_Color32(39,43,54,255));
    }
    if(tutorial&&greenpathRenderer.ready()){
        if(tutorial->greenpath.sphereVisible){
            const auto p=tutorial->greenpath.spherePosition;
            hk3ds::AnimationPlayer sphere;sphere.play(greenpathArt.require("Hornet/SphereBall"));
            sphere.advance(tutorial->greenpath.sphereAge);
            // Sphere Ball / Grow starts at .8 and scales to 1.5 in .25 s.
            const float grow=.8f+.7f*std::min(1.f,tutorial->greenpath.sphereAge/.25f);
            greenpathRenderer.drawAnimation(sphere,greenpathArt,screenX(p.x),screenY(p.y),worldPpu*grow,1);
            // Flash Effect is a separate child activated by the source Sphere state.
            hk3ds::AnimationPlayer flash;flash.play(greenpathArt.require("Hornet/SphereFlash"));
            flash.advance(tutorial->greenpath.sphereAge);
            if(flash.clip&&!flash.complete)
                greenpathRenderer.drawAnimation(flash,greenpathArt,screenX(p.x),screenY(p.y-.25f),worldPpu,1.3849015f,1.05f);
        }
        for(const auto& a:tutorial->greenpath.actors)if(a.visible)
            greenpathRenderer.drawAnimation(a.animation,greenpathArt,screenX(a.position.x),screenY(a.position.y),worldPpu,a.facing,a.scaleY,a.alpha);
        if(tutorial->greenpath.needleVisible){
            hk3ds::AnimationPlayer player;player.play(greenpathArt.require("Needle/Idle"));const auto p=tutorial->greenpath.needle;
            player.advance(tutorial->greenpath.needleAge);
            greenpathRenderer.drawAnimation(player,greenpathArt,screenX(p.x),screenY(p.y),worldPpu,1);
        }
        for(unsigned i=0;i<tutorial->greenpath.ballCount;++i){
            const auto& ball=tutorial->greenpath.balls[i];
            hk3ds::AnimationPlayer player;player.play(greenpathArt.require("MossKnight/Spit"));
            greenpathRenderer.drawAnimation(player,greenpathArt,screenX(ball.position.x),screenY(ball.position.y),worldPpu,
                                            ball.scale,ball.scale,1,ball.rotation);
        }
    }
    crossroadsEnemyArena.draw(cameraX,cameraY,worldPpu);
    if(tutorial)for(unsigned i=0;i<tutorial->buzzerCount;++i)if(tutorial->buzzers[i].alive())
    {
        const auto& enemy=tutorial->buzzers[i];
        const auto p=enemy.renderPosition(presentationTime);
        if(buzzerRenderer.ready())buzzerRenderer.drawAnimation(enemy.animation,buzzerBank,
            screenX(p.x),screenY(p.y),worldPpu,enemy.scaleX);
        if(showHitboxes||!buzzerRenderer.ready())
        {
            const auto b=enemy.bounds();const u32 color=C2D_Color32(200,150,80,255);
            if(!buzzerRenderer.ready())C2D_DrawRectSolid(screenX(b.left+p.x-enemy.position.x),screenY(b.top+p.y-enemy.position.y),0,(b.right-b.left)*worldPpu,(b.top-b.bottom)*worldPpu,color);
            else
            {
                C2D_DrawLine(screenX(b.left),screenY(b.top),color,screenX(b.right),screenY(b.top),color,1,0);
                C2D_DrawLine(screenX(b.right),screenY(b.top),color,screenX(b.right),screenY(b.bottom),color,1,0);
                C2D_DrawLine(screenX(b.right),screenY(b.bottom),color,screenX(b.left),screenY(b.bottom),color,1,0);
                C2D_DrawLine(screenX(b.left),screenY(b.bottom),color,screenX(b.left),screenY(b.top),color,1,0);
            }
        }
    }
    if(tutorial&&tutorial->sceneActive&&tutorial->sceneEnemies.locked&&!tutorial->sceneEnemies.watcherBattle){
        const auto r=tutorial->sceneEnemies.arena;
        for(float x:{r.left,r.right})
            C2D_DrawRectSolid(screenX(x-.15f),screenY(r.top),0,.3f*worldPpu,(r.top-r.bottom)*worldPpu,C2D_Color32(80,75,78,255));
    }
    if(tutorial&&tutorial->boss.active()){
        const auto& boss=tutorial->boss;
        if(!tutorial->sceneActive){
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(14,19,30,255));
        // Isolated encounter backdrop; original room art belongs to the scene adapter.
        const auto floor=[&](float l,float r,float y){C2D_DrawRectSolid(screenX(l),screenY(y),0,(r-l)*worldPpu,240,C2D_Color32(39,43,54,255));};
        const float mid=(boss.arena.left+boss.arena.right)*.5f;
        if(boss.collapsed){floor(boss.arena.left,mid-3,boss.arena.bottom);floor(mid+3,boss.arena.right,boss.arena.bottom);floor(boss.arena.left,boss.arena.right,boss.arena.bottom-11);}
        else floor(boss.arena.left,boss.arena.right,boss.arena.bottom);
        }
        if(boss.locked&&!tutorial->sceneActive)for(float x:{boss.arena.left,boss.arena.right})
            C2D_DrawRectSolid(screenX(x-.15f),screenY(boss.arena.top),0,.3f*worldPpu,(boss.arena.top-boss.arena.bottom)*worldPpu,C2D_Color32(80,75,78,255));
        if(bossRenderer&&bossArtwork&&bossRenderer->ready()){
            const auto drawBoss=[&](const hk3ds::AnimationPlayer& player,hk3ds::Vec2 p,float scale=1,float flash=0){
                bossRenderer->drawAnimation(player,*bossArtwork,screenX(p.x),screenY(p.y),worldPpu,scale,std::abs(scale),1,0,flash);
                // Native-resolution frames share up to four atlas pieces. Each
                // companion clip has the same frame count and timing as its owner.
                if(player.clip)for(unsigned layer=1;layer<4;++layer){
                    char name[64];std::snprintf(name,sizeof(name),"%s/Part%u",player.clip->name,layer);
                    auto part=player;part.clip=bossArtwork->find(name);
                    if(part.clip)bossRenderer->drawAnimation(part,*bossArtwork,screenX(p.x),screenY(p.y),worldPpu,scale,std::abs(scale),1,0,flash);
                }
            };
            if(boss.visible)drawBoss(boss.animation,boss.position,boss.scaleX,boss.hitFlash>0?1.f:0.f);
            if(boss.visible&&boss.showHead)
                drawBoss(boss.headAnimation,boss.headPosition,boss.headScaleX,boss.hitFlash>0?1.f:0.f);
            for(const auto& minion:boss.minions)if(minion.alive)drawBoss(minion.animation,minion.position,minion.artScale());
            for(const auto& p:boss.effects){
                char name[16];std::snprintf(name,sizeof(name),"FX/%u",p.emitter);
                hk3ds::AnimationPlayer a;a.play(bossArtwork->require(name));
                if(!a.clip)continue;
                a.frame=std::min<unsigned>(a.clip->count-1,unsigned(p.progress*a.clip->count));
                bossRenderer->drawAnimation(a,*bossArtwork,screenX(p.position.x),screenY(p.position.y),worldPpu,p.scale.x,p.scale.y,
                    p.opacity,p.angle);
            }
            for(const auto& projectile:boss.projectiles)if(projectile.life>0){
                if(boss.kind==hk3ds::CrossroadsBossKind::DungDefender){drawBoss(projectile.animation,projectile.position);continue;}
                const auto p=projectile.position;
                if(projectile.ground){
                    C2D_DrawTriangle(screenX(p.x-.45f),screenY(p.y-.45f),C2D_Color32(205,207,215,255),
                                     screenX(p.x+.45f),screenY(p.y-.45f),C2D_Color32(205,207,215,255),
                                     screenX(p.x),screenY(p.y+.6f),C2D_Color32(235,237,244,255),0);
                }else{
                    hk3ds::AnimationPlayer barrel;barrel.play(bossArtwork->require("Barrel/Idle"));
                    bossRenderer->drawAnimation(barrel,*bossArtwork,screenX(p.x),screenY(p.y),worldPpu,
                        projectile.barrelScale,projectile.barrelScale,1,projectile.barrelAngle*.0174532925199433f);
                }
            }
        }
    }
    const bool hideHero=(testFrameCaptureActive&&hk3ds_test_frames.hideHero!=0)||hero.actor==hk3ds::Actor::Dead||(tutorial&&(tutorial->monomon.maskScene||tutorial->flow.phase==hk3ds::RoomPhase::Dead||tutorial->flow.phase==hk3ds::RoomPhase::HazardDeath));
    if (renderer.ready()&&!hideHero)
    {
        if(tutorial&&tutorial->flow.presentation.clip)
            renderer.drawAnimation(tutorial->flow.presentation,bank,screenX(heroPose.x),screenY(heroPose.y),worldPpu,hero.facingRight?-1:1);
        else if(tutorial&&sceneArt&&tutorial->sceneEnemies.mantisHeroAnimation.clip&&sceneArt->renderers[7]&&sceneArt->banks[7])
            sceneArt->renderers[7]->drawAnimation(tutorial->sceneEnemies.mantisHeroAnimation,*sceneArt->banks[7],
                screenX(heroPose.x),screenY(heroPose.y),worldPpu,hero.facingRight?-1:1);
        else renderer.draw(hero,simulation.animator,bank,screenX(heroPose.x),screenY(heroPose.y),worldPpu,tutorial?tutorial->focusFlash:0);
    }
    else if(!hideHero)
    {
        const auto b = hk3ds::bodyBounds(heroPose);
        C2D_DrawRectSolid(screenX(b.left),screenY(b.top),0,(b.right-b.left)*worldPpu,(b.top-b.bottom)*worldPpu,C2D_Color32(64,200,240,255));
    }
    const auto drawFocus=[&](hk3ds::FocusDrawPass pass){
        if(tutorial&&renderer.ready())for(const auto& draw:tutorial->focusDraws)if(draw.active&&draw.pass==pass){
            // Screen(Cs, Cd) = Cd + Cs * (1-Cd); Lines Anim RGB is premultiplied offline.
            if(draw.screenBlend){
                C2D_Flush();
                C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE_MINUS_DST_COLOR,GPU_ONE,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
            }
            renderer.drawAnimation(draw.animation,bank,screenX(draw.position.x),screenY(draw.position.y),worldPpu,
                                   draw.scale.x,draw.scale.y,draw.alpha,0);
            if(draw.screenBlend){
                C2D_Flush();
                C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
            }
        }
    };
    drawFocus(hk3ds::FocusDrawPass::BeforeForeground);
    if(tutorial&&furyArtwork)furyDraw(tutorial->abilityPickup.flashDraw(*furyArtwork),tutorial->abilityPickup.flashOpacity());
    if(tutorial&&renderer.ready())for(const auto& item:tutorial->focusParticles){
        hk3ds::AnimationPlayer player;player.clip=item.clip;
        renderer.drawAnimation(player,bank,screenX(item.position.x),screenY(item.position.y),worldPpu,item.size,item.size,item.opacity,0);
    }
    if(tutorial&&renderer.ready())for(const auto& item:tutorial->heroParticles){
        const auto p=item.position;
        const float x=screenX(p.x),y=screenY(p.y),size=item.size;
        if(x<-80||x>480||y<-80||y>320)continue;
        hk3ds::AnimationPlayer player;player.clip=item.clip;
        renderer.drawAnimation(player,bank,x,y,worldPpu,size,size,item.opacity,item.angle);
    }
    if(tutorial&&tutorial->damageFlash.active&&renderer.ready()){
        const auto& flash=tutorial->damageFlash;
        hk3ds::AnimationPlayer player;player.clip=flash.clip;
        renderer.drawAnimation(player,bank,screenX(flash.position.x),screenY(flash.position.y),worldPpu,
                               flash.scale.x,flash.scale.y,flash.alpha,flash.angle);
    }
    if(tutorial&&tutorial->death.active()&&renderer.ready()){
        const auto& death=tutorial->death;
        for(const auto& item:death.particles()){
            const auto p=item.position;
            const float x=screenX(p.x),y=screenY(p.y),size=item.size;
            if(x<-80||x>480||y<-80||y>320)continue;
            hk3ds::AnimationPlayer player;player.clip=item.clip;
            renderer.drawAnimation(player,bank,x,y,worldPpu,size,size,1,item.angle);
        }
        const float mirror=death.facingRight?-1.f:1.f;
        if(death.flashOpacity()>0){
            // Authored white Death Crack / Hit Crack sprites, offsets and scales.
            const float size=death.flashScale(),raySize=death.flashScale(true);
            renderer.drawAnimation(death.flash,bank,screenX(death.origin.x),screenY(death.origin.y-.5f),worldPpu,size*mirror,size,death.flashOpacity());
            renderer.drawAnimation(death.flashRays,bank,screenX(death.origin.x-.21f*mirror),screenY(death.origin.y-.5f),worldPpu,raySize*mirror,raySize,death.flashOpacity(true),death.flashRotation);
        }
        if(death.bodyVisible())renderer.drawAnimation(death.body,bank,screenX(death.origin.x),screenY(death.origin.y),worldPpu,mirror);
    }
    if (renderer.ready()&&!hideHero) renderer.drawSlash(hero,simulation.animator,bank,nails,screenX(heroPose.x),screenY(heroPose.y),worldPpu);
    if(tutorial&&abilityRenderer.ready()&&tutorial->abilityPickup.visible()&&tutorial->abilityPickup.hasStandaloneArt()){
        const auto& pickup=tutorial->abilityPickup;
        hk3ds::AnimationPlayer player;
        player.play(abilityArt.find(pickup.kind==hk3ds::AbilityPickup::Kind::Spirit?
            (pickup.phase==hk3ds::AbilityPickup::Phase::Dialogue?"Shaman Talk":"Shaman Idle"):"Cloak Pickup"));
        player.advance(pickup.seconds);
        abilityRenderer.drawAnimation(player,abilityArt,screenX(pickup.position.x),screenY(pickup.position.y),worldPpu,1);
    }
    for(const auto& spirit:simulation.spirits.actors)if(spirit.active()){
        const auto p=spirit.renderPosition(presentationTime);
        const float x=screenX(p.x),y=screenY(p.y),d=float(spirit.direction);
        const auto scale=spirit.renderScale();
        const auto* clip=abilityArt.find(spirit.animation());
        if(!clip)clip=abilityArt.find("Spirit"); // Older private packages contain only the base projectile.
        hk3ds::AnimationPlayer player;player.play(clip);player.advance(spirit.age);
        if(const auto* frame=player.current(abilityArt)){
            const float left=x+frame->left*d*scale.x*worldPpu,right=x+frame->right*d*scale.x*worldPpu;
            if(std::max(left,right)<0||std::min(left,right)>400||
               y-frame->bottom*scale.y*worldPpu<0||y-frame->top*scale.y*worldPpu>240)continue;
        }
        if(abilityRenderer.ready())abilityRenderer.drawAnimation(player,abilityArt,x,y,worldPpu,d*scale.x,scale.y,spirit.opacity());
    }
    if(hero.dashing&&abilityRenderer.ready()){
        hk3ds::AnimationPlayer burst;burst.play(abilityArt.find("Dash Effect"));burst.advance(hk3ds::tuning::dashDuration-hero.dashTime);
        abilityRenderer.drawAnimation(burst,abilityArt,screenX(heroPose.x),screenY(heroPose.y),worldPpu,hero.facingRight?-1:1);
    }
    // Source Knight/Vignette sorts at layer 13/order 0. Haze at order 2 and
    // foreground particles must retain their painter order around that mask.
    drawCpu.mark(hk3ds::PerformanceSection::CpuActors);
    kickGpu(hk3ds::GpuKickPoint::BeforeForeground);
    const hk3ds::RoomSortBoundary vignetteBoundary{13,0,lighting&&lighting->ready()?lighting->config().transform[5]:0};
    const auto* foregroundSplit=lighting&&lighting->ready()?&vignetteBoundary:nullptr;
    if(visuals&&roomRenderer){
        if(tutorial)roomRenderer->drawForeground(*visuals,tutorial->decorVisuals,cameraX,cameraY,worldPpu,
            visible,mutableTutorial,effects,&tutorial->ambientVisuals,foregroundSplit,false,&tutorial->worldSwitchVisuals);
        else roomRenderer->draw(*visuals,cameraX,cameraY,worldPpu,visible,nullptr,400,240,false,0,false,effects);
    }
    hk3ds::gpuProfileCategory(hk3ds::PerformanceSection::GpuActors);
    drawSceneEnemyActors(true);
    drawFocus(hk3ds::FocusDrawPass::AfterForeground);
    if(tutorial)furyDraw(tutorial->furyChest.draws()[static_cast<unsigned>(hk3ds::FuryPart::Front)]);
    if(tutorial&&furyArtwork){
        for(const auto& tink:tutorial->nailInteractions.tinks())furyDraw(tink);
        auto prompt=tutorial->nailInteractions.prompt();
        prompt.scale.x*=hk3ds::topui::panelScale;
        prompt.scale.y*=hk3ds::topui::panelScale;
        if(prompt.visible){
            auto shadow=prompt;shadow.player.play(furyArtwork->find("Inspect Shadow"),0,true);
            furyDraw(shadow,tutorial->nailInteractions.labelAlpha());
            furyDraw(prompt);
            auto label=prompt;label.player.play(furyArtwork->find("Inspect Label"),0,true);
            furyDraw(label,tutorial->nailInteractions.labelAlpha());
        }
    }
    if(lighting&&lighting->ready())lighting->drawVignette(heroPose,simulation.hero.facingRight,tutorial?tutorial->darkness.scale():5.5f,cameraX,cameraY);
    if(tutorial&&visuals&&roomRenderer&&foregroundSplit)
        roomRenderer->drawForeground(*visuals,tutorial->decorVisuals,cameraX,cameraY,worldPpu,
            visible,mutableTutorial,effects,&tutorial->ambientVisuals,foregroundSplit,true,&tutorial->worldSwitchVisuals);
    drawCpu.mark(hk3ds::PerformanceSection::CpuForeground);
    kickGpu(hk3ds::GpuKickPoint::BeforePresent);
    if (showHitboxes && nails.ready() && hero.nail.hitbox)
    {
        const auto& shape=nails.shapes[hk3ds::nailVariant(hero.nail)];
        const u32 color=C2D_Color32(255,230,140,255);
        for (std::size_t i=0; i<shape.count; ++i)
        {
            const auto a=hk3ds::nailPoint(hero,shape,shape.vertices[i]);
            const auto b=hk3ds::nailPoint(hero,shape,shape.vertices[(i+1)%shape.count]);
            C2D_DrawLine(screenX(a.x),screenY(a.y),color,screenX(b.x),screenY(b.y),color,1,0);
        }
    }
    // Debug-only active attack window, also available without nail-shape assets.
    if (showHitboxes && hero.nail.hitbox) C2D_DrawRectSolid(6,6,0,8,8,C2D_Color32(255,230,140,255));
    if(effects)effects->present(target,!testFrameCaptureActive||!hk3ds_test_frames.skipCameraColor,
                              !testFrameCaptureActive||!hk3ds_test_frames.skipCameraBloom,
                              tutorial?tutorial->healthScuttlers.screenFlashStrength():0,
                              tutorial?tutorial->healthScuttlers.screenFlashStrength(1):0);
    drawCpu.mark(hk3ds::PerformanceSection::CpuPresent);
    hk3ds::runtimePhase(hk3ds::RuntimePhase::Hud);
    hk3ds::gpuProfileCategory(hk3ds::PerformanceSection::GpuHud);
    if(tutorial&&sceneArt&&tutorial->sceneEnemies.mantisTitleAlpha>0&&
       sceneArt->banks[7]&&sceneArt->renderers[7]){
        hk3ds::AnimationPlayer title;title.play(sceneArt->banks[7]->require("Mantis Title"));
        hk3ds::textcapture::String caption("MANTIS LORDS");
        sceneArt->renderers[7]->drawAnimation(title,*sceneArt->banks[7],0,0,16,1,1,
                                             tutorial->sceneEnemies.mantisTitleAlpha);
    }
    if(tutorial&&hudRenderer){
        if(tutorial->boss.active()&&tutorial->boss.phase!=hk3ds::BossPhase::Dormant)
            hudRenderer->drawBossTitle(tutorial->boss.titleAlpha);
        else if(tutorial->greenpath.titleAlpha>0)
            hudRenderer->drawHornetTitle(tutorial->greenpath.titleAlpha);
        else if(!tutorial->inspection.locked())hudRenderer->drawAreaTitle(tutorial->areaTitle);
    }
    if(tutorial&&tutorial->worldQuestTitleAlpha>0&&questArt&&questRenderer){
        hk3ds::AnimationPlayer title;title.play(questArt->require("Sly Title"));
        questRenderer->drawAnimation(title,*questArt,hk3ds::areatitledata::x,hk3ds::areatitledata::y,
                                     16,1,1,tutorial->worldQuestTitleAlpha);
    }
    if(tutorial&&hudRenderer){
        const auto& rest=tutorial->resting;
        const auto anchor=rest.talkPrompt?rest.station.position:
            rest.bellPrompt?rest.station.bell:rest.station.toll;
        hudRenderer->drawResting(rest,200+(anchor.x-cameraX)*worldPpu,
            120-(anchor.y+3.5f-cameraY)*worldPpu);
    }
    if(tutorial){
        // Door prompts and the gameplay prompts (Listen, Rest, Shop, Inspect) share the original Arrow Prompt art.
        const auto drawArrowPrompt=[&](const hk3ds::PromptMarkerDraw& prompt,float lift){
        if(prompt.visible){
            hk3ds::GeoVisualDraw art{};art.visible=true;art.position=prompt.gate.promptPosition;art.position.y+=lift;
            art.scale={hk3ds::topui::panelScale,hk3ds::topui::panelScale};
            if(furyArtwork){
                art.player.play(furyArtwork->find("Inspect Shadow"),0,true);furyDraw(art,prompt.alpha);
                art.player.play(furyArtwork->find(prompt.down?"Inspect Down":"Inspect Up"),0,true);
                art.player.advance(prompt.age);furyDraw(art);
            }
            float textWidth=0;
            const auto* glyphs=furyArtwork?furyArtwork->find("Door Glyphs"):nullptr;
            if(furyRenderer&&furyRenderer->ready()&&glyphs&&glyphs->count==hk3ds::doorfont::count&&
               hk3ds::doorfont::measure(prompt.gate.prompt,textWidth)){
                const float ppu=hk3ds::roomFocalLength()/38.1f;
                const float scale=hk3ds::topui::panelScale*hk3ds::doorfont::enlargement;
                float x=screenX(art.position.x)-textWidth*ppu*scale*.5f;
                const float y=std::round(screenY(art.position.y));
                hk3ds::textcapture::String capture(prompt.gate.prompt);
                hk3ds::AnimationPlayer glyph;glyph.play(glyphs);
                for(const auto* character=prompt.gate.prompt;*character;++character){
                    const unsigned value=hk3ds::doorfont::uppercase(static_cast<unsigned char>(*character));
                    if(value>=hk3ds::doorfont::first){
                        glyph.frame=value-hk3ds::doorfont::first;
                        if(const auto* frame=glyph.current(*furyArtwork)){
                            const auto box=hk3ds::spriteLayout(*frame,std::round(x),y,ppu,scale,scale);
                            hk3ds::textcapture::glyph(box.x,box.y,frame->width*box.scaleX,frame->height*box.scaleY);
                        }
                        furyRenderer->drawAnimation(glyph,*furyArtwork,std::round(x),y,ppu,scale,scale,prompt.alpha);
                    }
                    x+=hk3ds::doorfont::advance[value-32]*ppu*scale;
                }
            }else if(hudRenderer)hudRenderer->drawDoorPrompt(prompt.gate.prompt,screenX(art.position.x),screenY(art.position.y),prompt.alpha);
        }
        };
        for(const auto& old:tutorial->flow.doorPromptPresentation.fading)drawArrowPrompt(old.draw,0);
        drawArrowPrompt(tutorial->flow.doorPromptPresentation,0);
        const auto& arrow=tutorial->arrowPrompt.presentation();
        drawArrowPrompt(arrow,arrow.authoredAnchor?0:52.f/worldPpu);
    }
    if(tutorial&&tutorial->flow.deathFade()>0)
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,static_cast<u8>(255*tutorial->flow.deathFade())));
    if(tutorial&&tutorial->flow.transitionFade()>0)
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,static_cast<u8>(255*tutorial->flow.transitionFade())));
    }
    if(hudRenderer&&tutorial&&!tutorial->boss.active())hudRenderer->drawCredits(tutorial->credits);
    if(tutorial&&!tutorial->boss.active()&&tutorial->credits.blankerAlpha()>0)
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32f(hk3ds::openingdata::red,hk3ds::openingdata::green,
                          hk3ds::openingdata::blue,tutorial->credits.blankerAlpha()));
    if(tutorial&&tutorial->monomon.whiteFade>0)
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32f(1,1,1,tutorial->monomon.whiteFade));
    const bool stagHidesHud=tutorial&&(tutorial->resting.travelMenu||tutorial->resting.stagPhase==hk3ds::StagPhase::Departing||tutorial->resting.stagPhase==hk3ds::StagPhase::Riding);
    // Old Iselda layout: the open shop's item text sits above the HUD on the top screen.
    if(hudRenderer&&tutorial&&tutorial->slyShop.open)hudRenderer->drawSlyShop(tutorial->slyShop,tutorial->charmGeo,false);
    if(hudRenderer&&tutorial&&tutorial->cartographer.shop)hudRenderer->drawIseldaShop(tutorial->cartographer,tutorial->charmGeo,false);
    if(hudRenderer&&hud&&!stagHidesHud&&(!tutorial||!tutorial->monomon.maskScene))hudRenderer->draw(target,*hud,tutorial?&tutorial->furyChest:nullptr,furyArtwork,furyRenderer,tutorial?&tutorial->conditionalEvents:nullptr,eventArt,eventRenderer,tutorial?&tutorial->worldQuestDialogue:nullptr,menu&&menu->screen==hk3ds::PauseMenu::Screen::Pause);
    if(hudRenderer&&tutorial)hudRenderer->drawReadingBoard(target,&tutorial->conditionalEvents,&tutorial->worldQuestDialogue,&tutorial->cartographer,&tutorial->abilityPickup,tutorial->greenpath.introText);
    if(hudRenderer&&tutorial&&tutorial->cartographer.shop&&eventArt&&eventRenderer)hudRenderer->drawCartographer(tutorial->cartographer,*eventArt,*eventRenderer);
    if(hudRenderer&&tutorial)hudRenderer->drawAbility(tutorial->abilityPickup,abilityArt,abilityRenderer);
    if(hudRenderer&&tutorial){
        const auto& m=tutorial->monomon;
        if(m.prompt)hudRenderer->drawDoorPrompt("LISTEN",200,170);
        if(m.text){
            C2D_DrawRectSolid(10,140,0,380,90,C2D_Color32(0,0,0,220));
            char line[128];const char* cursor=m.text;float y=156;
            while(*cursor&&*cursor!='\f'&&y<226){
                unsigned n=0;while(*cursor&&*cursor!='\n'&&*cursor!='\f'&&n<sizeof(line)-1)line[n++]=*cursor++;
                line[n]=0;hudRenderer->inventoryText(line,22,y);y+=17;
                if(*cursor=='\n')++cursor;
            }
        }
    }
    if(tutorial&&tutorial->monomon.film){
        if(!monomonMovieFailed){
            const bool ready=monomonMovie.active()||monomonMovie.start("romfs:/mask-shatter.bin");
            if(!ready||!monomonMovie.draw(monomonMovieStep)){
                hk3ds::diagnosticLog("Monomon cinematic stream failed; returning to gameplay\n");
                monomonMovie.release();monomonMovieFailed=true;
            }
        }
    }
    if(hudRenderer&&tutorial){
        auto page=tutorial->charmsPage;page.open=false;
        hudRenderer->drawCharms(tutorial->charmInventory,page,tutorial->charmsEditable,tutorial->charmGeo,furyArtwork,furyRenderer);
    }
    }
    }
    if(menu&&menu->active()){
        C2D_SceneBegin(target);C2D_ViewReset();
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,185));
        if(hudRenderer)hudRenderer->drawMenuDetail(*menu,tutorial?tutorial->sceneName:nullptr,hud,map,tutorial?tutorial->heroRenderPosition():simulation.hero.position);
    }
    const bool readingUi=hudRenderer&&tutorial&&hudRenderer->readingPanelVisible(&tutorial->conditionalEvents,&tutorial->worldQuestDialogue,&tutorial->cartographer,&tutorial->abilityPickup,tutorial->greenpath.introText);
    const bool restingUi=hudRenderer&&tutorial&&tutorial->resting.charmMenu;
    if(restingUi&&!readingUi&&!(menu&&menu->active())){
        C2D_SceneBegin(target);C2D_ViewReset();
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,185));
        hudRenderer->drawResting(tutorial->resting,tutorial->charmGeo,true);
    }
    if(!readingUi&&!restingUi&&inventory&&inventory->opened()&&inventorySnapshot&&hudRenderer&&inventoryRenderer&&!(menu&&menu->active())){
        C2D_SceneBegin(target);C2D_ViewReset();
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,255));
        if(inventory->tab()==hk3ds::InventoryTab::Charms&&tutorial){
            auto page=tutorial->charmsPage;page.open=true;
            hudRenderer->drawCharms(tutorial->charmInventory,page,tutorial->charmsEditable,tutorial->charmGeo,furyArtwork,furyRenderer,true,true);
        }else inventoryRenderer->drawDetail(*inventory,*inventorySnapshot,*hudRenderer,tutorial?hk3ds::saveAreaName(tutorial->sceneName):nullptr);
    }
    // Independent of top-screen cutscenes, fades and boss renderer ownership.
    if(bottom&&bottomUi){
        const auto mapHero=tutorial?tutorial->heroRenderPosition():simulation.hero.position;
        // User 2026-11-12: lore text showed on both screens; the top-screen board owns it, the lower screen keeps the map.
        const bool reading=hudRenderer&&tutorial&&hudRenderer->lowerReadingVisible(&tutorial->conditionalEvents,&tutorial->worldQuestDialogue,&tutorial->cartographer,&tutorial->abilityPickup,tutorial->greenpath.introText);
        const bool mapPage=!inventory||inventory->tab()==hk3ds::InventoryTab::Map;
        const bool routeMenu=tutorial&&(tutorial->slyShop.open||tutorial->tollMenu.open);
        const auto stagPhase=tutorial?tutorial->resting.stagPhase:hk3ds::StagPhase::Hidden;
        const bool resting=hudRenderer&&tutorial&&(tutorial->resting.travelMenu||tutorial->resting.charmMenu||
            stagPhase==hk3ds::StagPhase::Toll||stagPhase==hk3ds::StagPhase::Poor);
        // User 2026-11-12 ("2"): while sitting on a bench the lower screen keeps the map page.
        const bool iseldaShop=tutorial&&tutorial->cartographer.shop&&tutorial->cartographer.dialogue==hk3ds::Cartographer::Dialogue::Prompt;
        const bool showMap=!routeMenu&&!iseldaShop&&!reading&&!resting&&mapPage&&mapRenderer&&map&&!map->rooms().empty()&&!(menu&&menu->active());
        if(!showMap||mapRenderer->needsDraw(*map,mapHero,nullptr,simulation.hero.charms.compass)){
            hk3ds::textcapture::select(1,true);
            if(!(menu&&menu->active()))bottomUi->drawBottomPanel(bottom);
            if(menu&&menu->active())bottomUi->drawMenu(bottom,*menu);
            else if(routeMenu&&hudRenderer){
                hudRenderer->drawSlyShop(tutorial->slyShop,tutorial->charmGeo,true);
                hudRenderer->drawRouteToll(tutorial->tollMenu,tutorial->charmGeo,true);
            }
            else if(iseldaShop&&hudRenderer)hudRenderer->drawIseldaShop(tutorial->cartographer,tutorial->charmGeo,true);
            else if(reading)hudRenderer->drawReadingPanel(&tutorial->conditionalEvents,&tutorial->worldQuestDialogue,&tutorial->cartographer,&tutorial->abilityPickup,tutorial->greenpath.introText);
            else if(resting)hudRenderer->drawResting(tutorial->resting,tutorial->charmGeo);
            else if(mapPage){
                if(showMap)mapRenderer->draw(*map,mapHero,tutorial?&tutorial->inventory:nullptr,simulation.hero.charms.compass,
                                             hudRenderer?hudRenderer->uiCanvas():hk3ds::ui::Canvas{});
                else {
                    hk3ds::ui::bottomBackground(bottomUi->uiCanvas());
                    hk3ds::ui::unavailableMap(bottomUi->uiCanvas());
                }
                if(inventoryRenderer&&hudRenderer)inventoryRenderer->drawTabs(hk3ds::InventoryTab::Map,*hudRenderer);
            }
            else if(inventoryRenderer&&inventory&&inventorySnapshot&&hudRenderer){
                // Bind drawing to the published slot, never the live simulation session.
                auto screen=*inventory;
                struct CharmDrawContext {
                    decltype(tutorial) state;
                    const hk3ds::AnimationBank* art;
                    const hk3ds::HeroRenderer* renderer;
                } context{tutorial,furyArtwork,furyRenderer};
                if(tutorial){
                    screen.charms.context=&context;
                    screen.charms.draw=[](void* raw,hk3ds::HudRenderer& hud,const hk3ds::InventorySnapshot&){
                        const auto& c=*static_cast<CharmDrawContext*>(raw);
                        auto page=c.state->charmsPage;page.open=true;
                        hud.drawCharms(c.state->charmInventory,page,c.state->charmsEditable,c.state->charmGeo,c.art,c.renderer,true);
                    };
                }
                inventoryRenderer->draw(bottom,screen,*inventorySnapshot,*hudRenderer);
            }
            // Redraw once after an overlay disappears, even if map/hero did not move.
            if(mapRenderer&&!showMap)mapRenderer->invalidate();
        }else C2D_SceneBegin(bottom); // Transfer retained pixels into the next LCD buffer.
        // Pause/inventory overwrite the shared target, so resume must repaint.
        if(!showMap&&mapRenderer)mapRenderer->invalidate();
    }
    if(hk3ds::consoleOverlay.enabled&&bottom){
        hk3ds::consoleOverlay.frame(double(svcGetSystemTick())/SYSCLOCK_ARM11);
        hk3ds::drawDiagnosticOverlay(target,bottom,world,hk3ds::bodyBounds(heroPose),cameraX,cameraY,worldPpu,
            tutorial?tutorial->diagnosticBoxes.data():nullptr,tutorial?tutorial->diagnosticBoxCount:0);
        if(mapRenderer)mapRenderer->invalidate();
    }
    if constexpr(hk3ds::diagnosticsEnabled){
        static bool checkedPanelFixture=false;
        if(testFrameCaptureActive&&!checkedPanelFixture){
            checkedPanelFixture=true;
            if(loadPanelCaptureFixture){
                hk3ds::loadDiagnostics.begin("Tutorial_01","Town","right1");
                hk3ds::loadDiagnostics.stage("room_page_import");
                hk3ds::loadDiagnostics.error("scene_textures_failed","Injected visual-check diagnostic.");
            }
        }
    }
    if(bottom&&bottomUi&&hk3ds::loadDiagnostics.visible()){
        C2D_SceneBegin(bottom);C2D_ViewReset();
        hk3ds::loadDiagnostics.draw(bottomUi->uiCanvas());
        // Details take the top screen while the panel is visible (no paging).
        if(target){
            C2D_TargetClear(target,C2D_Color32(0,0,0,255));
            C2D_SceneBegin(target);C2D_ViewReset();
            hk3ds::loadDiagnostics.drawDetails(bottomUi->uiCanvas());
        }
    }
    if(hk3ds::consoleOverlay.confirmationUntil)
        hk3ds::drawDiagnosticConfirmation(target,double(svcGetSystemTick())/SYSCLOCK_ARM11);
    drawCpu.mark(hk3ds::PerformanceSection::CpuHud);
    hk3ds::FrameProfile::Scope timing(hk3ds::frameProfile,hk3ds::FrameProfile::FrameEnd);
    hk3ds::gpuProfileCheckpoint(hk3ds::PerformanceSection::GpuDisplay);
    hk3ds::runtimePhase(hk3ds::RuntimePhase::GpuEnd);
    // CPU submission overlapped the kicked prefix. Retire it before Citro3D
    // publishes/runs the tail and owns the single display completion.
    {hk3ds::PerformanceScope wait(hk3ds::PerformanceSection::GpuKickWait);
    hk3ds::finishGpuKicks();}
    if(roomRenderer)roomRenderer->releaseVisibleDraws();
    const auto displayStart=hk3ds::gpuProfileTransferBegin();
    C3D_FrameEnd(0);
    hk3ds::gpuProfileTransferEnd(hk3ds::PerformanceSection::GpuDisplay,displayStart,true);
#if HK3DS_DIAGNOSTICS
    // Capture time must advance through paused gameplay and dropped render frames.
    static u64 videoStarted=0;
    const u64 videoNow=svcGetSystemTick();
    if(!videoStarted)videoStarted=videoNow;
    hk3ds_test_video_ticks=static_cast<std::uint32_t>((videoNow-videoStarted)*60/SYSCLOCK_ARM11);
#endif
    hk3ds::runtimePhase(hk3ds::RuntimePhase::DrawComplete);
    hk3ds::textcapture::publish();
    if(testFrameCaptureActive){
        hk3ds_test_frames.commandBufferUsage=C3D_GetCmdBufUsage();
        hk3ds_test_frames.roomDraws=roomRenderer?roomRenderer->drawCount():0;
        hk3ds_test_frames.roomDrawFailures=roomRenderer?roomRenderer->drawFailures():0;
        ++hk3ds_test_frames.rendered;
    }
}
}
static int runGameSession(int argc,char** argv)
{
    // Quit/Continue reuses the process; startup must not name the old room.
    breadcrumb.beginSession();
    hk3ds::heapguard::setSink(heapGuardSink);
    recordBreadcrumb("startup");
    auto platform=hk3ds::configureNew3dsPerformance();
    hk3ds::selectGraphicsProfile(hk3ds::GraphicsMode::Auto,platform.new3ds,envGetLinearHeapSize()>hk3ds::oldLinearHeapBytes);
    gfxInitDefault(); gfxSet3D(false);
    // Launch metadata must leave stack headroom for nested startup/physics calls.
    auto consoleTestStorage=std::make_unique<hk3ds::ConsoleTestSession>(argc,argv);
    auto& consoleTest=*consoleTestStorage;
    if(consoleTest.requested&&!consoleTest.verified()){recordBreadcrumb("console_test_init","console_test_verification_failed");gfxExit();return 3;}
    hk3ds::InputReplay replay;
    if(hk3ds::diagnosticsEnabled&&!consoleTest.playing())
        replay.load(consoleTest.active()?consoleTest.replayPath():"sdmc:/3ds/HK3DS/input-replay.bin");
    const bool benchmarkReplay=replay.benchmarkRoom()>=0;
    if(benchmarkReplay)hk3ds::replayGameFlags=replay.benchmarkMask();
    bool builtinReplay=false;
    // Choose before restoring saves or initializing the fresh tutorial session.
    auto* externalReplay=(!hk3ds::diagnosticsEnabled||consoleTest.playing())?nullptr:std::fopen("sdmc:/3ds/HK3DS/input-replay.bin","rb");
    if(externalReplay)std::fclose(externalReplay);
    hidScanInput();
    builtinReplay=hk3ds::diagnosticsEnabled&&!consoleTest.requested&&!externalReplay&&(hidKeysHeld()&KEY_SELECT)&&!diagnosticChordHeld();
    bool skipMenu=consoleTest.requested||externalReplay||builtinReplay;
    for(int i=1;hk3ds::diagnosticsEnabled&&i<argc;++i)if(!std::strcmp(argv[i],"--skip-menu"))skipMenu=true;
    for(const char* flag:{"sdmc:/3ds/HK3DS/test-commands.flag","sdmc:/3ds/HK3DS/boot-game.flag"})
        if(auto* file=hk3ds::diagnosticsEnabled?std::fopen(flag,"rb"):nullptr){skipMenu=true;std::fclose(file);}
    auto consoleMetricsStorage=std::make_unique<hk3ds::ConsoleMetrics>();
    auto& consoleMetrics=*consoleMetricsStorage;
    if(hk3ds::diagnosticsEnabled&&!consoleTest.playing())consoleMetrics.begin(consoleTest.active());
    if(hk3ds::diagnosticsEnabled&&!consoleTest.playing())hk3ds::audioEvidence.begin(consoleTest.active());
    auto& profile=hk3ds::frameProfile;
    if(hk3ds::diagnosticsEnabled&&!consoleTest.playing())profile.begin(consoleTest.active()||hk3ds::audioEvidence.enabled());
    if(profile.enabled())if(auto* f=std::fopen("sdmc:/3ds/HK3DS/performance-features.bin","rb")){
#if HK3DS_DIAGNOSTICS
        profile.performanceFeaturesOverride=true;
#endif
        std::uint32_t mask=0;
        if(std::fread(&mask,sizeof(mask),1,f)==1)hk3ds_perf_disable_mask=mask;
        std::fclose(f);
    }
    static_assert(C3D_DEFAULT_CMDBUF_SIZE==256u*1024u,"Update calibrated command-buffer accounting");
    if (!C3D_Init(hk3ds::gpuCommandBufferBytes)) { recordBreadcrumb("init","c3d_init_failed");gfxExit(); return 1; }
    if (!C2D_Init(2048)) { recordBreadcrumb("init","c2d_init_failed");C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    const bool compactScreens=hk3ds::gameFlagEnabled("sdmc:/3ds/HK3DS/effects-target-compact.flag",hk3ds::gameFlagEffectsTargetCompact,hk3ds::shippedEffectsTargetCompact);
    const auto createScreenTarget=[compactScreens](gfxScreen_t screen){
        auto* target=C2D_CreateScreenTarget(screen,GFX_LEFT);
        // Citro2D requests DEPTH16 (enum value zero). Painter-order rendering
        // disables depth testing, so detach its unused allocation before use.
        if(target&&compactScreens&&target->ownsDepth){
            vramFree(target->frameBuf.depthBuf);
            C3D_FrameBufDepth(&target->frameBuf,nullptr,GPU_RB_DEPTH16);
            target->ownsDepth=false;
        }
        return target;
    };
    C3D_RenderTarget* top = createScreenTarget(GFX_TOP);
    C3D_RenderTarget* bottom = createScreenTarget(GFX_BOTTOM);
    if (!top||!bottom) { recordBreadcrumb("init","screen_target_failed");C2D_Fini(); C3D_Fini(); gfxExit(); return 1; }
    static hk3ds::HudRenderer hudRenderer;
    hk3ds::loadDiagnostics.bind([]()->std::uint64_t{return osGetTime();},[](){
        const auto used=mallinfo().uordblks;const auto heap=envGetHeapSize();
        return hk3ds::LoadMemory{linearSpaceFree(),vramSpaceFree(),used<heap?heap-used:0};
    });
    hk3ds::loadDiagnostics.begin("Title","Tutorial_01","startup",false);
    const auto loadingBottom=[&](){
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(bottom,C2D_Color32(0,0,0,255));
        C3D_FrameEnd(0);C3D_FrameSync();
    };
    loadingBottom();
    hk3ds::LoadingScreen loading(top);
    const bool romfs = R_SUCCEEDED(consoleTest.initRomfs());
    if(!romfs){
        recordBreadcrumb("init","romfs_init_failed");
        C2D_Fini();C3D_Fini();gfxExit();showFailure("romfs_init_failed");return 1;
    }
    {   // Original game text ("@key" strings) from the converted text pack.
        std::ifstream pack("romfs:/text.bin",std::ios::binary);
        if(!pack||!hk3ds::text::load({std::istreambuf_iterator<char>(pack),std::istreambuf_iterator<char>()}))
            recordBreadcrumb("text_load","text_pack_missing");
    }
    const bool hudReady=romfs&&hudRenderer.load("romfs:");
    if(romfs&&!hudReady){recordBreadcrumb("hud_load","hud_load_failed");sessionFailed=true;}
    if constexpr(hk3ds::diagnosticsEnabled)
        // This fixture flag is independent of replay-selected graphics flags.
        loadPanelCaptureFixture=hk3ds::sdFileExists("sdmc:/3ds/HK3DS/load-error-panel.flag");
    const bool automatedLoadDiagnostics=hk3ds::diagnosticsEnabled&&
        !hk3ds::loadNoticesAllowed(consoleTest.playing(),consoleTest.requested,
            builtinReplay||externalReplay,benchmarkReplay,
            hk3ds::sdFileExists("sdmc:/3ds/HK3DS/test-commands.flag")||
            hk3ds::sdFileExists("sdmc:/3ds/HK3DS/boot-game.flag"));
    const auto showLoadDiagnostics=[&](){
        if(!hudReady||loadPanelCaptureFixture)return; // Explicit capture fixture keeps the frame mailbox running.
        if(automatedLoadDiagnostics)hk3ds::loadDiagnostics.acknowledgeSlowLoads();
        hidScanInput();bool released=!(hidKeysHeld()&KEY_A);
        while(hk3ds::loadDiagnostics.visible()&&aptMainLoop()){
            if(!hk3ds::loadDiagnostics.current()->slow)loadFailureAcknowledged=true;
            hidScanInput();const auto pressed=hidKeysDown();
            {const double now=double(svcGetSystemTick())/SYSCLOCK_ARM11;
            if(hk3ds::diagnosticChordPoll(hk3ds::consoleOverlay,hk3ds::diagnosticComboHeld(hidKeysHeld()),now))hk3ds::toggleDiagnosticLogging(now);}
            if(!(hidKeysHeld()&KEY_A))released=true;
            hk3ds::loadDiagnostics.input(released&&(pressed&KEY_A),pressed&(KEY_L|KEY_DLEFT),pressed&(KEY_R|KEY_DRIGHT));
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top,C2D_Color32(0,0,0,255));
            C2D_SceneBegin(top);C2D_ViewReset();
            hk3ds::loadDiagnostics.drawDetails(hudRenderer.uiCanvas());
            C2D_SceneBegin(bottom);C2D_ViewReset();
            hk3ds::loadDiagnostics.draw(hudRenderer.uiCanvas());
            C3D_FrameEnd(0);C3D_FrameSync();
        }
    };
    static hk3ds::InventoryRenderer inventoryRenderer;
    const bool inventoryReady=hudReady&&inventoryRenderer.load("romfs:");
    if(hudReady&&!inventoryReady)hk3ds::loadDiagnostics.error("inventory_load_failed","romfs:/inventory-visual.bin");
    if(inventoryReady)hudRenderer.inventoryArtwork(&inventoryRenderer);
    loadingBottom();
    int selectedSlot=0;
    bool newGame=false;
    if(romfs&&!skipMenu){
        loading.finish();
        selectedSlot=hk3ds::runTitleMenu(top,bottom,hudRenderer,&newGame);
        loading.begin(top);
        if(selectedSlot<0){inventoryRenderer.release();hudRenderer.inventoryArtwork(nullptr);hudRenderer.release();romfsExit();C2D_Fini();C3D_Fini();gfxExit();return 0;}
    }
    hk3ds::loadDiagnostics.begin("Title","Tutorial_01","startup",false);
    hk3ds::SystemSettings graphicsSettings;
    if(!builtinReplay&&!consoleTest.active()&&!externalReplay)graphicsSettings.load("sdmc:/3ds/HK3DS/system.sav");
    hk3ds::selectGraphicsProfile(graphicsSettings.graphics,platform.new3ds,envGetLinearHeapSize()>hk3ds::oldLinearHeapBytes);
    loadingBottom();
    hk3ds::pulseLoadingScreen();
    if(hk3ds::diagnosticsEnabled&&romfs)hk3ds::loadEnemyBArena();
    hk3ds::AnimationBank bank;
    hk3ds::NailBank nails;
    hk3ds::HeroRenderer renderer;
    if(romfs&&abilityArt.load("romfs:/ability-art.bin"))abilityRenderer.load("romfs:",abilityArt,"ability-page-");
    if (romfs && bank.load("romfs:/hero.bin")) renderer.load("romfs:",bank);
    if (romfs) nails.load("romfs:/nail.bin");
    hk3ds::pulseLoadingScreen();
    hk3ds::CrawlerConfig enemyConfig;
    hk3ds::AnimationBank enemyBank;
    hk3ds::HeroRenderer enemyRenderer;
    const bool enemyReady=romfs && nails.ready() && enemyConfig.load("romfs:/crawler.bin") &&
        enemyBank.load("romfs:/crawler-anim.bin") && enemyRenderer.load("romfs:",enemyBank,"crawler-page-");
    hk3ds::AnimationBank buzzerBank;hk3ds::HeroRenderer buzzerRenderer;
    hk3ds::AnimationBank enemyCArt;hk3ds::HeroRenderer enemyCRenderer;
    hk3ds::AnimationBank shadeBank;hk3ds::HeroRenderer shadeRenderer;hk3ds::ShadeConfig shadeConfig;
    const bool shadeReady=romfs&&shadeConfig.load("romfs:/shade.bin")&&shadeBank.load("romfs:/shade-anim.bin")&&shadeRenderer.load("romfs:",shadeBank,"shade-page-");
    hk3ds::AnimationBank eventArt;hk3ds::HeroRenderer eventRenderer;
    int previousDarkness=0;
    hk3ds::AnimationBank questArt;hk3ds::HeroRenderer questRenderer;
    hk3ds::AnimationBank corniferArt;hk3ds::HeroRenderer corniferRenderer;
    const bool corniferReady=romfs&&corniferArt.load("romfs:/cornifer-anim.bin")&&corniferRenderer.load("romfs:",corniferArt,"cornifer-page-");
    hk3ds::AnimationBank stagArt;hk3ds::HeroRenderer stagRenderer;bool stagReady=false;
    static hk3ds::CartographerAudio cartographerAudio;
    const bool eventReady=romfs&&eventArt.load("romfs:/events-anim.bin")&&eventRenderer.load("romfs:",eventArt,"events-page-");
    hk3ds::AnimationBank furyArtwork;hk3ds::HeroRenderer furyRenderer;
    hk3ds::AnimationBank geoArtwork;hk3ds::HeroRenderer geoRenderer;
    hk3ds::AnimationBank doorArtwork;hk3ds::HeroRenderer doorRenderer;
    // Source chip profiles enlarge this persistent configuration; preserve the
    // startup stack reserve used by nested room/asset loaders.
    static hk3ds::GeoRockConfig geoRocks;
    const bool buzzerReady=romfs&&buzzerBank.load("romfs:/buzzer-anim.bin")&&buzzerRenderer.load("romfs:",buzzerBank,"buzzer-page-");
    // Keep startup terrain storage off the ARM session stack as room metadata grows.
    auto terrainStorage=std::make_unique<hk3ds::TerrainBank>();
    auto& terrain=*terrainStorage;
    recordBreadcrumb("startup_assets");
    const bool terrainReady=romfs && terrain.load("romfs:/tutorial.bin");
    recordBreadcrumb("startup_room_metadata");
    static hk3ds::RoomConfig roomRules;static hk3ds::RoomEntityBank roomEntities;
    static hk3ds::WorldSwitchRoomAssets worldSwitchAssets;
    static hk3ds::TownLiftRoomAssets townLiftAssets;
    static hk3ds::TerrainBank fullTerrain;const hk3ds::RoomVisualBank* roomVisuals=nullptr;
    // Accepted static-decor banks are the New default; incomplete packages fall back.
    static bool staticVisualsActive=false;
    const bool staticDecorFlag=hk3ds::staticDecorEnabled();
    const auto staticPackageAvailable=[&](const char* directory,const char* staticDirectory){
        if(!staticDecorFlag)return false;
        char original[192],baked[192];
        for(const auto* name:{"room-visual.bin","plants.bin","room-dynamics.bin","room-ambient.bin","grass.bin"}){
            std::snprintf(original,sizeof(original),"%s/%s",directory,name);
            std::snprintf(baked,sizeof(baked),"%s/static-%s",staticDirectory,name);
            if((!std::strcmp(name,"room-visual.bin")||hk3ds::sdFileExists(original))&&!hk3ds::sdFileExists(baked))return false;
        }
        return true;
    };
    const bool tutorialStaticPackage=staticPackageAvailable("romfs:","romfs:");
    static hk3ds::RoomRenderer roomRenderer;
    static hk3ds::RoomVisualBank residentVisuals;
    static hk3ds::LightingRenderer lighting;
    static hk3ds::ScreenEffects effects;
    const bool roomReady=enemyReady&&terrainReady&&roomRules.load("romfs:/room-rules.bin")&&
        roomEntities.load("romfs:/room-entities.bin")&&fullTerrain.load("romfs:/tutorial-room.bin");
    if(roomReady){hk3ds::closeUnavailableSceneGates(roomRules,"romfs:",hk3ds::graphicsProfile().roomPrefix);hk3ds::addUnavailableGateBarriers(roomRules,fullTerrain);}
    static hk3ds::TutorialSession tutorial;
    // The Stag bank belongs to this session's stack-backed asset owner. Clear
    // any pointer retained by the static TutorialSession before rebuilding it.
    tutorial.resting.bindStag(nullptr);
    // Install additional terrain before any World stores vector-backed views.
    recordBreadcrumb("startup_bind");
    const bool furyDataReady=roomReady&&furyArtwork.load("romfs:/fury-visual.bin")&&
        tutorial.furyChest.bind(furyArtwork,"romfs:/fury-chest.bin")&&tutorial.furyChest.bindTerrain(fullTerrain)&&
        tutorial.nailInteractions.load("romfs:/nail-interactions.bin",furyArtwork)&&
        tutorial.bindLore("romfs:/lore-tablets.bin");
    bool visualsReady=false;
    int sectorX=-1,sectorY=-1;
    auto world=roomReady?fullTerrain.world():terrainReady?terrain.world():fixtureWorld;
    static hk3ds::SimpleMap map;
    tutorial.cartographer.bind(corniferReady?&corniferArt:nullptr);
    map={}; // A newly chosen profile must not inherit another profile's map.
    map.visit({terrainReady?"Tutorial_01":"fixture",hk3ds::collisionMapBounds(world),world,
        roomReady?roomRules.gates.data():nullptr,roomReady?roomRules.gates.size():0});
    enemyConfig.bodyMaterial=terrain.actorMaterial; enemyConfig.corpseMaterial=terrain.corpseMaterial;
    diagnosticPrint("HK3DS character research\nBuild 22529139 / RESEARCH\nD-Pad / Circle: move\nB: jump     R: dash\nX: nail     Y: hold to focus\nL: damage   SELECT: reset\nA: collision overlay\nSTART: exit\n\nAbilities unlocked for testing.\nCamera/hero start are fixtures.\n\nSprites: %s\nNail geometry: %s\n",renderer.ready()?"original local pack":"missing (collider view)",nails.ready()?"original local pack":"missing; no combat");
    diagnosticPrint("Terrain: %s\n",terrainReady?"Tutorial_01 intact snapshot":"missing; synthetic room");
    // Projectile hit sets grew with the combined room encounters; keep them off the ARM stack.
    auto simulationStorage=std::make_unique<hk3ds::Simulation>();
    auto& simulation=*simulationStorage;reset(simulation,terrainReady?&terrain:nullptr);
    Targets targets; resetTargets(targets);
    hk3ds::Crawler crawler;
    hk3ds::DoorConfig doorConfig;hk3ds::BreakableDoor door;
    const bool doorReady=terrainReady && doorConfig.load("romfs:/door.bin");
    // Reproducible native RNG for this room; not Unity's global RNG sequence.
    std::uint32_t seed=1;
    static hk3ds::AudioPlayer audio;
    hk3ds::GreatDoorConfig greatDoorConfig;
    hk3ds::GeoConfig geoConfig;
    hk3ds::DecorBank decorBank;
    hk3ds::PlantBank plantBank;
    // Persistent banks must leave stack room for the startup asset loaders.
    static hk3ds::GrassBank grassBank;
    static hk3ds::RoomDynamicsBank dynamicsBank;
    const bool tutorialReady=roomReady&&shadeReady&&eventReady&&tutorial.conditionalEvents.bind(&eventArt,"romfs:/events-text.bin")&&greatDoorConfig.load("romfs:/great-door.bin")&&geoConfig.load("romfs:/geo.bin")&&decorBank.load("romfs:/decor.bin")&&plantBank.load("romfs:/plants.bin")&&tutorial.bind(roomRules,fullTerrain,bank,enemyBank,enemyConfig,nails,roomEntities,{&seed,randomUnit},&greatDoorConfig,&geoConfig,buzzerReady?&buzzerBank:nullptr,&decorBank,400.0f/240.0f,false,&plantBank,&shadeConfig,&shadeBank);
    if(!tutorialReady&&!hk3ds::diagnosticsEnabled){recordBreadcrumb("startup_assets","startup_assets_failed");sessionFailed=true;}
    if(!sessionFailed)recordBreadcrumb("startup_audio");
    {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Audio);
     if(tutorialReady){audio.load("romfs:");audio.sceneAudio(tutorial.sceneName());}}
    const char* savePath=consoleTest.active()?consoleTest.savePath():hk3ds::saveSlotPath(selectedSlot);
    // Persistent room bools enlarge saves; keep profile state off the ARM stack.
    auto startupSaveStorage=std::make_unique<hk3ds::TutorialSave>();
    auto& startupSave=*startupSaveStorage;
    bool startupLocation=false;
    if(!sessionFailed)recordBreadcrumb("startup_restore");
    if(tutorialReady)
    {
        mkdir("sdmc:/3ds",0777);mkdir("sdmc:/3ds/HK3DS",0777);
        tutorial.collectibles.load("romfs:/collectibles.bin");
        if(!benchmarkReplay&&!builtinReplay&&!consoleTest.active()&&tutorial.restoreContinue(savePath,newGame,startupSave)){startupLocation=std::strcmp(startupSave.scene,"Tutorial_01")!=0||startupSave.gate[0];}
    }
    // The worker's persistent save snapshot must not consume the session stack.
    auto saveWriterStorage=std::make_unique<hk3ds::SaveWriter>();
    auto& saveWriter=*saveWriterStorage;
    // Save snapshots are large; reserve their scratch space only while saving,
    // outside the session frame used by synchronous physics and asset loading.
    const auto writeSave=[&](bool flush) __attribute__((noinline)) {
        const auto snapshot=tutorial.saved();
        if(flush)saveWriter.flush(savePath,snapshot,tutorial.saveRevision);
        else saveWriter.update(savePath,snapshot,tutorial.saveRevision);
    };
    const auto savedFlags=[&]() __attribute__((noinline)) {return tutorial.saved().flags;};
    hk3ds::PauseMenu menu;
    constexpr const char* settingsPath="sdmc:/3ds/HK3DS/system.sav";
    if(!builtinReplay&&!consoleTest.active()&&!externalReplay)menu.settings.load(settingsPath);
    audio.volumes(menu.settings.musicVolume,menu.settings.soundVolume);
    if(tutorialReady)diagnosticPrint("\x1b[2J\x1b[HTutorial_01\nD-Pad / Circle: move\nB: jump X: nail Y: focus\nSTART: pause\n\nNew game abilities\nNo dash / wall jump / double jump\n");
    if(tutorialReady){
        diagnosticPrint("Loading room...\n");
        bool metadataReady=false;
        {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Metadata);
         char path[128];
         const bool packageReady=hk3ds::graphicsProfile().roomTextureBytes!=hk3ds::oldRoomTextureBudget||
             hk3ds::scenePackageAvailable("romfs:","Tutorial_01",hk3ds::graphicsProfile().roomPrefix);
         if(!packageReady){recordBreadcrumb("startup_visuals","old_profile_unavailable");sessionFailed=true;}
         staticVisualsActive=tutorialStaticPackage&&residentVisuals.load("romfs:/static-room-visual.bin");
         if(!staticVisualsActive){
             std::snprintf(path,sizeof(path),"romfs:/%svisual.bin",hk3ds::graphicsProfile().roomPrefix);
             metadataReady=packageReady&&residentVisuals.load(path);
         }else metadataReady=packageReady;
        }
        const bool effectsReady=metadataReady&&(!hk3ds::graphicsProfile().postprocessing||effects.load());
        bool geoArtReady=false,geoRocksReady=false,geoBindReady=false,geoPageReady=false;
        bool plantsReady=false,decorReady=false,ambientReady=false,doorReady=false;
        const char* ambientFailure="not-attempted";
        const auto ambientLoad=[&](bool loaded,const char* name){
            ambientFailure=loaded?"none":name;return loaded;
        };
        const bool roomPagesReady=effectsReady&&roomRenderer.load("romfs:",residentVisuals,
            staticVisualsActive?"static-room-":hk3ds::graphicsProfile().roomPrefix);
        visualsReady=roomPagesReady;
        if(visualsReady){
            geoArtReady=furyDataReady&&
                furyRenderer.load("romfs:",furyArtwork,"fury-page-")&&geoArtwork.load("romfs:/geo-visual.bin");
            // INSPECT artwork is reduced on the top LCD; sample its actual atlas pages.
            if(geoArtReady)for(const char* name:{"Inspect Label","Inspect Up","Inspect Down","Inspect Shadow"}){
                const auto* clip=furyArtwork.find(name);
                if(!clip||!clip->count){geoArtReady=false;break;}
                hk3ds::AnimationPlayer prompt;prompt.play(clip);
                for(unsigned index=0;index<clip->count;++index){
                    prompt.frame=index;
                    const auto* frame=prompt.current(furyArtwork);
                    geoArtReady=frame&&furyRenderer.setLinearFilter(frame->page)&&geoArtReady;
                }
            }
            geoRocksReady=geoArtReady&&geoRocks.load("romfs:/geo-rocks.bin");
            geoBindReady=geoRocksReady&&tutorial.geoVisuals.bind(geoArtwork,geoRocks)&&
                tutorial.healthCocoon.bind(geoArtwork,"romfs:/health-cocoon.bin");
            geoBindReady=geoBindReady&&tutorial.healthCocoonFx.bind(geoArtwork,"Spatter Blue","Spawn","Cocoon Cap")&&
                tutorial.healthScuttlers.bind(geoArtwork);
            if(geoBindReady){
                geoBindReady=tutorial.healthCocoonFx.configure(tutorial.healthCocoon.fxConfig());
                tutorial.healthCocoonBreakEffect=[](void* context,hk3ds::Vec2 origin,hk3ds::Strike direction,hk3ds::Vec2 nailPosition){
                    auto& room=*static_cast<hk3ds::TutorialSession*>(context);
                    room.healthCocoonFx.breakAt(origin,room.healthCocoon.visual().depth,nailPosition);
                    room.decorEffects.healthCocoonBreak(origin,direction);
                };
                tutorial.healthCocoonEffectContext=&tutorial;}
            // Source cocoon/tk2d atlases use bilinear sampling; page 1 has padded sprites.
            geoPageReady=geoBindReady&&geoRenderer.load("romfs:",geoArtwork,"geo-page-")&&
                geoRenderer.setLinearFilter(1);
            doorReady=geoPageReady&&doorArtwork.load("romfs:/great-door-visual.bin")&&
                doorArtwork.find("door_v01")&&doorArtwork.find("door_v02")&&doorArtwork.find("door_v03")&&
                doorRenderer.load("romfs:",doorArtwork,"great-door-page-");
            // Bind every cached index bank to the metadata that actually loaded,
            // including the normal-bank fallback for invalid opt-in metadata.
            plantsReady=doorReady&&plantBank.load(staticVisualsActive?"romfs:/static-plants.bin":"romfs:/plants.bin")&&
                dynamicsBank.load(staticVisualsActive?"romfs:/static-room-dynamics.bin":"romfs:/room-dynamics.bin")&&
                tutorial.roomDynamics.bind(dynamicsBank,residentVisuals)&&
                tutorial.plants.bind(plantBank,residentVisuals,{&seed,randomUnit})&&
                grassBank.load(staticVisualsActive?"romfs:/static-grass.bin":"romfs:/grass.bin")&&tutorial.grass.bind(grassBank,residentVisuals);
            decorReady=plantsReady&&tutorial.decorEffects.load("romfs:/decor-effects.bin","romfs:/decor-fx-visual.bin")&&
                roomRenderer.loadDecor("romfs:",tutorial.decorEffects.visuals);
            if(decorReady)tutorial.decorEffects.bindGrass(grassBank);
            if(decorReady)decorReady=tutorial.contactEffects.load("romfs:/contact-effects.bin",tutorial.decorEffects);
            ambientReady=decorReady&&ambientLoad(tutorial.heroEffects.load("romfs:/hero-effects.bin",bank),"hero-effects")&&
                ambientLoad(tutorial.focusEffects.load("romfs:/focus-effects.bin",bank),"focus-effects")&&
                ambientLoad(tutorial.decorEffects.loadStalactites("romfs:/stalactite-fx.bin"),"stalactite-fx")&&
                ambientLoad(tutorial.decorEffects.loadInitialFall("romfs:/initial-fall-fx.bin"),"initial-fall-fx")&&
                ambientLoad(tutorial.decorEffects.loadOwners("romfs:/owner-effects.bin"),"owner-effects")&&
                ambientLoad(tutorial.stalactiteVisuals.load("romfs:/stalactite-visuals.bin",tutorial.decorEffects.visuals.atlas),"stalactite-visuals")&&
                ambientLoad(tutorial.ambientParticles.load("romfs:/ambient-particles.bin"),"ambient-particles")&&
                ambientLoad(tutorial.ambientParticles.bindVisuals(tutorial.decorEffects.visuals),"ambient-bind");
            visualsReady=ambientReady&&tutorial.roomAmbient.bind(residentVisuals,staticVisualsActive?"romfs:/static-room-ambient.bin":"romfs:/room-ambient.bin");
            if(visualsReady&&hk3ds::graphicsProfile().ambientParticles)tutorial.ambientParticles.prewarm({tutorial.flow.camera.x,tutorial.flow.camera.y});
            if(visualsReady)roomVisuals=&residentVisuals;
        }
        else hk3ds::diagnosticLog("Room load failed: missing, invalid or over budget.\n");
        if(!effectsReady)hk3ds::loadDiagnostics.error("startup_effects_failed","Screen effects initialization");
        else if(roomPagesReady&&!visualsReady){
            const char* stage=!geoArtReady?"startup_geo_art_failed":!geoRocksReady?"startup_geo_rocks_failed":
                !geoBindReady?"startup_geo_binding_failed":!geoPageReady?"startup_geo_page_failed":
                !plantsReady?"startup_plants_failed":!decorReady?"startup_decor_failed":
                !ambientReady?"startup_ambient_failed":"startup_room_ambient_failed";
            hk3ds::loadDiagnostics.error(stage,"Tutorial_01 startup assets");
        }
        const bool lightingReady=hk3ds::graphicsProfile().lighting&&lighting.load("romfs:");
        if(hk3ds::graphicsProfile().lighting&&!lightingReady)hk3ds::loadDiagnostics.error("lighting_load_failed","romfs:/lighting.bin and light textures");
        if (!lightingReady) hk3ds::diagnosticLog("Lighting: missing or invalid source lighting package.\n");
        else tutorial.bindLighting(&lighting.config());
        // Enemy PCM admission follows all mandatory room/texture allocations.
        if(visualsReady)audio.loadEnemies("romfs:");
        hk3ds::pulseLoadingScreen();
        if(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled()){
        char visualLog[192];consoleTest.path(visualLog,sizeof(visualLog),"visual-load.txt");
        if(auto* f=(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled())?std::fopen(consoleTest.active()?visualLog:"sdmc:/3ds/HK3DS/visual-load.txt","w"):nullptr){
            std::fprintf(f,"metadata=%u effects=%u room=%u lighting=%u failures=%u linear_size=%lu heap_size=%lu linear_free=%lu vram_free=%lu\n",
                unsigned(metadataReady),unsigned(effectsReady),unsigned(visualsReady),unsigned(lightingReady),roomRenderer.loadFailures(),
                static_cast<unsigned long>(envGetLinearHeapSize()),static_cast<unsigned long>(envGetHeapSize()),
                static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
            std::fprintf(f,"room_vram_pages=%u room_vram_bytes=%lu\n",roomRenderer.vramPages(),
                static_cast<unsigned long>(roomRenderer.vramBytes()));
            std::fprintf(f,"effects_vram_bytes=%u\n",effects.vramBytes());
            std::fprintf(f,"geo_art=%u geo_rocks=%u geo_bind=%u geo_page=%u plants=%u decor=%u ambient=%u\n",
                unsigned(geoArtReady),unsigned(geoRocksReady),unsigned(geoBindReady),unsigned(geoPageReady),
                unsigned(plantsReady),unsigned(decorReady),unsigned(ambientReady));
            std::fprintf(f,"great_door=%u\n",unsigned(doorReady));
            std::fprintf(f,"ambient_failure=%s\n",ambientFailure);
            std::fclose(f);
        }
        }
    }
    if(tutorialReady&&!visualsReady&&!sessionFailed){recordBreadcrumb("startup_visuals","startup_visuals_failed");sessionFailed=true;}
    // The unrelated fallback fixture must not advance the room's RNG stream.
    std::uint32_t fixtureSeed=1;
    const auto spawnEnemy=[&]()
    {
        if (terrainReady) for (int i=0;i<2;++i) targets[i].colliderEnabled=false;
        if (enemyReady && terrainReady && crawler.bind(enemyConfig,enemyBank,targets[2],terrain.crawlerSpawn,world,{&fixtureSeed,randomUnit}))
            targets[2].parent=3;
        if(doorReady)door.bind(doorConfig,terrain,targets[4]);
    };
    spawnEnemy();
    diagnosticPrint("Crawler: %s\n",crawler.active()?"Tutorial_01 source transform":"missing; disabled");
    // Admit HUD art only after mandatory world textures and PCM. Missing private
    // art retains the diagnostic console used by the public fixture.
    hk3ds::InventoryScreen inventory;
    inventory.selectTab(hk3ds::InventoryTab::Map);
    inventory.charms.context=&tutorial;
    inventory.charms.input=[](void* context,const hk3ds::Input& input){
        auto& session=*static_cast<hk3ds::TutorialSession*>(context);
        session.charmsPage.navigate(input);
        if(input.isPressed(hk3ds::Jump))session.toggleCharm();
        if(input.isPressed(hk3ds::Focus))session.buyCharm();
        if(input.isPressed(hk3ds::Attack))session.buyCharmNotch();
    };
    hk3ds::InventorySnapshot inventorySnapshot;
    static hk3ds::MapRenderer mapRenderer;
    const bool mapReady=mapRenderer.load();
    if(!mapReady)hk3ds::loadDiagnostics.error("map_load_failed","Map artwork or render target");
    if(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled()){
    char hudLog[192];consoleTest.path(hudLog,sizeof(hudLog),"hud-load.txt");
    if(auto* f=(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled())?std::fopen(consoleTest.active()?hudLog:"sdmc:/3ds/HK3DS/hud-load.txt","w"):nullptr){
        std::fprintf(f,"loaded=%u atlas_bytes=%lu map_target_vram_bytes=%lu linear_free=%lu vram_free=%lu\n",
            unsigned(hudReady),static_cast<unsigned long>(hudRenderer.textureBytes()),
            static_cast<unsigned long>(hk3ds::mapTargetVramBytes),
            static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
        std::fclose(f);
    }
    }
    static hk3ds::HudPresentation hud;
    // Stable render-owned addresses preserve the revision cache as slots alternate.
    hk3ds::TutorialPrompts hudPrompts;
    hk3ds::LoreInspection hudInspection;
    const auto hudSnapshot=[&]() {
        const auto& hero=tutorialReady?tutorial.simulation.hero:simulation.hero;
        auto snapshot=hk3ds::HudSnapshot::from(hero.vitals,tutorialReady?tutorial.geo:0,
                                            hero.focus==hk3ds::FocusPhase::Draining,
                                            tutorialReady&&tutorial.shade.recovering());
#if HK3DS_DIAGNOSTICS
        hk3ds::frameCaptureHudFixture(hk3ds_test_frames,testFrameCaptureActive,snapshot.health,
                                    snapshot.maxHealth,snapshot.blueHealth,snapshot.geo);
        if(testFrameCaptureActive){hk3ds_test_frames.observedHudHealth=snapshot.health;hk3ds_test_frames.observedHudGeo=snapshot.geo;}
#endif
        return snapshot;
    };
    hud.reset(hudSnapshot());
    unsigned hudEpoch=tutorialReady?tutorial.audioEpoch:0;
    float hudSeconds=tutorialReady?tutorial.levelSeconds():0;
    hk3ds::CombatScene combat; combat.bind(targets.data(),targets.size(),&nails);
    bool showHitboxes=false;
    hk3ds::InputLatch input;
    hk3ds::TestCommandInput commandInput;
    if(hk3ds::diagnosticsEnabled&&tutorialReady&&!consoleTest.playing()&&(replay.active()|| (builtinReplay?replay.loadBytes(hk3ds::tutorialReplay,sizeof(hk3ds::tutorialReplay)):
        replay.load(consoleTest.active()?consoleTest.replayPath():"sdmc:/3ds/HK3DS/input-replay.bin"))))
    {
        diagnosticPrint("LOCAL INPUT REPLAY: %lu control steps\n",static_cast<unsigned long>(replay.total()));
    }
    auto* commandFlag=hk3ds::diagnosticsEnabled?std::fopen("sdmc:/3ds/HK3DS/test-commands.flag","rb"):nullptr;
    const bool testCommandsEnabled=commandFlag&&!builtinReplay&&!consoleTest.requested;
    static hk3ds::AnimationBank bossArtwork;static hk3ds::HeroRenderer bossRenderer;
    bool bossTexturesAttempted=false;
    if(commandFlag)std::fclose(commandFlag);
    // The acceptance runner freezes before the first input tick in both modes.
    // The fast mode reconstructs the same checkpoint without intermediate draws.
#if HK3DS_DIAGNOSTICS
    if(auto* flag=(!hk3ds::diagnosticsEnabled||consoleTest.playing())?nullptr:std::fopen("sdmc:/3ds/HK3DS/frame-capture.bin","rb")){
        std::uint32_t words[2]{};
        if(std::fread(words,sizeof(words),1,flag)==1&&std::fgetc(flag)==EOF)
            hk3ds::frameCaptureStartup(hk3ds_test_frames,testCommandsEnabled&&tutorialReady,
                                      replay.active(),words[0],words[1]);
        std::fclose(flag);
    }
#endif
    struct InputSource { hk3ds::InputReplay* replay; hk3ds::TestCommandInput* command; hk3ds::TutorialSession* game; } inputSource{&replay,&commandInput,tutorialReady?&tutorial:nullptr};
    input.bindSource(&inputSource,[](void* context,hk3ds::InputLatch& latch)
    {
        auto& source=*static_cast<InputSource*>(context);
        const bool shopButtons=source.game&&source.game->shopMenuOpen();
        if(source.command->active){
            // Advance the route clock while capture actions supply ordinary input.
            hk3ds::InputLatch ignored;
            if(source.replay->active())source.replay->sample(ignored,shopButtons);
            source.command->sample(latch);
        }else if(source.replay->active())source.replay->sample(latch,shopButtons);
    });
    u64 previous = svcGetSystemTick();
    const u64 replayStarted=previous;
    const char* replayResult=nullptr;
    int displayCounter = 0;
    std::uint32_t diagnosticFrame=0, diagnosticResets=0;
    bool startExit=false;
    bool returnToTitle=false;
    static hk3ds::SceneBank activeScene;
    if(tutorialReady)consoleTest.sceneEntered(tutorial.sceneName());
    static SceneEnemyGraphics sceneEnemyGraphics;
    // Scene particle banks use room-atlas indices; the shared decor atlas stays resident.
    const unsigned residentAmbientSprite=tutorial.ambientParticles.spriteIndex();
    // Keep loading scratch off the 32 KiB main stack during synchronous physics.
    const auto loadScene=[&](const char* scene,const char* gate,const char* sourceGate=nullptr) __attribute__((noinline)) {
        hk3ds::runtimePhase(hk3ds::RuntimePhase::SceneLoad);
        char gateRoute[72];std::snprintf(gateRoute,sizeof(gateRoute),sourceGate?"%s -> %s":"%s%s",sourceGate?sourceGate:"",gate);
        // Automated captures cannot acknowledge an interactive slow-load panel before replay starts.
        hk3ds::loadDiagnostics.begin(tutorial.sceneName(),scene,gateRoute,
                                     !(automatedLoadDiagnostics||testCommandsEnabled||testFrameCaptureActive));
        breadcrumb.set(breadcrumb.scene,sizeof(breadcrumb.scene),scene);
        breadcrumb.set(breadcrumb.gate,sizeof(breadcrumb.gate),gate);
        recordBreadcrumb("scene_begin");
        const auto fail=[&](const char* stage,const char* detail=nullptr){
            recordBreadcrumb(stage,detail?detail:stage);
            sessionFailed=true;
            hk3ds::consoleOverlay.assetFailure(stage);
            consoleTest.stop(stage);
            hk3ds::diagnosticLog("Scene load failed: %s / %s (%s)\n",scene,gate,stage);
            if(auto* log=(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled())?std::fopen("sdmc:/3ds/HK3DS/scene-route.txt","a"):nullptr){
                std::fprintf(log,"%s %s %s\n",stage,scene,gate);std::fclose(log);}
            return false;
        };
        hk3ds::LoadingScreen sceneLoading(top);
        hk3ds::pulseLoadingScreen();
        if(!hk3ds::validSceneIdentifier(scene)||!hk3ds::validGateIdentifier(gate))return fail("scene_invalid_identifier");
        loadingBottom();mapRenderer.invalidate();
        char directory[128],staticDirectory[128],path[180];
        const bool returning=std::strcmp(scene,"Tutorial_01")==0;
        std::snprintf(directory,sizeof(directory),returning?"romfs:":"romfs:/scenes/%s",scene);
        std::snprintf(staticDirectory,sizeof(staticDirectory),returning?"romfs:":"romfs:/static-scenes/%s",scene);
        hk3ds::SceneBank nextScene;hk3ds::TerrainBank nextTerrain;hk3ds::RoomVisualBank nextVisuals;
        bool nextStaticVisuals=false;
        hk3ds::SceneLighting nextLighting;
        recordBreadcrumb("scene_metadata");
        if(!returning){std::snprintf(path,sizeof(path),"%s/scene.bin",directory);
            const auto checkpointSave=tutorial.saved().resting;
            const hk3ds::Vec2 checkpoint{checkpointSave.benchX,checkpointSave.benchY};
            const hk3ds::Vec2* savedCheckpoint=!std::strcmp(scene,checkpointSave.benchScene)?&checkpoint:nullptr;
            if(!nextScene.load(path)||!nextScene.enter(gate,savedCheckpoint))return fail("scene_metadata_failed");
            hk3ds::inheritHeroHazardRules(nextScene.rules,roomRules);
            hk3ds::closeUnavailableSceneGates(nextScene.rules,"romfs:",hk3ds::graphicsProfile().roomPrefix);}
        recordBreadcrumb("scene_terrain");
        std::snprintf(path,sizeof(path),"%s/tutorial-room.bin",directory);if(!nextTerrain.load(path))return fail("scene_terrain_failed");
        hk3ds::addUnavailableGateBarriers(returning?roomRules:nextScene.rules,nextTerrain);
        recordBreadcrumb("scene_visual_metadata");
        if(hk3ds::graphicsProfile().roomTextureBytes==hk3ds::oldRoomTextureBudget&&
           !hk3ds::scenePackageAvailable("romfs:",scene,hk3ds::graphicsProfile().roomPrefix))
            return fail("old_profile_unavailable");
        if(staticPackageAvailable(directory,staticDirectory)){
            std::snprintf(path,sizeof(path),"%s/static-room-visual.bin",staticDirectory);
            nextStaticVisuals=nextVisuals.load(path);
        }
        if(!nextStaticVisuals){
            std::snprintf(path,sizeof(path),"%s/%svisual.bin",directory,hk3ds::graphicsProfile().roomPrefix);
            if(!nextVisuals.load(path))return fail("scene_visual_metadata_failed");
        }
        hk3ds::Vec2 mapOrigin{};
        if(tutorial.flow.transition&&!map.rooms().empty()){
            const auto& previousRoom=map.rooms()[map.current()];
            const auto& from=tutorial.flow.transition->trigger;
            const auto& nextRules=returning?roomRules:nextScene.rules;
            for(const auto& entry:nextRules.gates)if(!std::strcmp(entry.name,gate)){
                const auto& to=entry.trigger;
                mapOrigin={previousRoom.origin.x+(from.left+from.right-to.left-to.right)*.5f,
                    previousRoom.origin.y+(from.bottom+from.top-to.bottom-to.top)*.5f};
                break;
            }
        }
        recordBreadcrumb("scene_lighting");
        if(!returning){std::snprintf(path,sizeof(path),"%s/scene-lighting.bin",directory);
            if(!nextLighting.load(path))return fail("scene_lighting_failed");}
        recordBreadcrumb("scene_assets");
        // Construct the save directly on the heap: this loader remains on the
        // ARM stack while nested room/entity loaders use their scratch.
        const std::unique_ptr<hk3ds::TutorialSave> savedStorage(new hk3ds::TutorialSave(tutorial.saved()));
        const auto& saved=*savedStorage;
        const hk3ds::SceneHeroState entryState(tutorial.simulation.hero);
        C3D_FrameSync();tutorial.uumuu.reset();tutorial.monomon.reset();archiveGraphics.release();monomonRenderer.release();monomonArtwork=nullptr;monomonMovie.release();monomonMovieFailed=false;roomRenderer.release();sceneEnemyGraphics.release();bossRenderer.release();
        if(!hk3ds::graphicsProfile().retainOptionalActors)greenpathRenderer.release();
        if(stagReady){stagRenderer.release();stagReady=false;}
        tutorial.worldQuests.bindArt(nullptr);tutorial.slyShopAnimation={};questRenderer.release();
        // The Great Door is Tutorial-only. Keeping its atlas resident blocked
        // Town's eight-page bank at the existing 2 MiB admission reserve.
        const auto linearBeforeDoorRelease=linearSpaceFree();
        if(!returning)doorRenderer.release();
        const auto linearAfterDoorRelease=linearSpaceFree();
        if(hk3ds::diagnosticLogRequested("sdmc:/3ds/HK3DS/scene-memory.flag")){
            if(auto* log=std::fopen("sdmc:/3ds/HK3DS/scene-memory.txt","a")){
                std::fprintf(log,"%s %s pages=%u before=%lu after=%lu admittedBefore=%u admittedAfter=%u\n",scene,gate,
                    static_cast<unsigned>(nextVisuals.atlas.pageCount),
                    static_cast<unsigned long>(linearBeforeDoorRelease),static_cast<unsigned long>(linearAfterDoorRelease),
                    hk3ds::roomTexturesFit(nextVisuals.atlas.pageCount,linearBeforeDoorRelease)?1u:0u,
                    hk3ds::roomTexturesFit(nextVisuals.atlas.pageCount,linearAfterDoorRelease)?1u:0u);
                std::fclose(log);
            }
        }
        bossTexturesAttempted=false;
        // Drop particles and delayed births before their room geometry is released.
        tutorial.decorEffects.reset();
        fullTerrain=std::move(nextTerrain);residentVisuals=std::move(nextVisuals);roomVisuals=&residentVisuals;
        staticVisualsActive=nextStaticVisuals;
        fullTerrain.bindMotionVisuals(residentVisuals);
        const auto loadRoomTextures=[&]{
            if(hk3ds::diagnosticLogRequested("sdmc:/3ds/HK3DS/scene-memory.flag")){
                if(auto* log=std::fopen("sdmc:/3ds/HK3DS/scene-memory.txt","a")){
                    const auto free=linearSpaceFree();
                    std::fprintf(log,"room %s %s pages=%u free=%lu required=%lu admitted=%u\n",scene,gate,
                        static_cast<unsigned>(residentVisuals.atlas.pageCount),static_cast<unsigned long>(free),
                        static_cast<unsigned long>(residentVisuals.atlas.pageCount*hk3ds::roomTexturePageBytes+hk3ds::roomLinearReserve),
                        hk3ds::roomTexturesFit(residentVisuals.atlas.pageCount,free)?1u:0u);
                    std::fclose(log);
                }
            }
            visualsReady=roomRenderer.load(staticVisualsActive?staticDirectory:directory,residentVisuals,
                staticVisualsActive?"static-room-":hk3ds::graphicsProfile().roomPrefix);
            return visualsReady;
        };
        world=fullTerrain.world();
        const int nextDarkness=returning?0:nextLighting.darkness;
        const float lanternReveal=hk3ds::sceneLanternRevealStart(nextDarkness,tutorial.hasLantern(),previousDarkness);
        previousDarkness=nextDarkness;
        lighting.selectScene(returning?nullptr:&nextLighting,tutorial.hasLantern());
        tutorial.bindLighting(&lighting.config());
        effects.setCameraColor(returning?hk3ds::cameraColor::Config{}:nextLighting.color);
        if(returning){
            if(!loadRoomTextures())return fail("scene_textures_failed");
            // These adapters cache draw/sprite indices and authored poses in the
            // resident bank. Validate and rebuild them before restart mutates it.
            if(!plantBank.load(staticVisualsActive?"romfs:/static-plants.bin":"romfs:/plants.bin")||
               !dynamicsBank.load(staticVisualsActive?"romfs:/static-room-dynamics.bin":"romfs:/room-dynamics.bin")||
               !grassBank.load(staticVisualsActive?"romfs:/static-grass.bin":"romfs:/grass.bin")||
               !tutorial.roomDynamics.bind(dynamicsBank,residentVisuals)||
               !tutorial.plants.bind(plantBank,residentVisuals,{&seed,randomUnit})||
               !tutorial.grass.bind(grassBank,residentVisuals)||
               !tutorial.roomAmbient.bind(residentVisuals,staticVisualsActive?"romfs:/static-room-ambient.bin":"romfs:/room-ambient.bin"))return fail("scene_tutorial_visuals_failed");
            auto enemyLife=std::move(tutorial.enemyLife);
            if(!tutorial.bind(roomRules,fullTerrain,bank,enemyBank,enemyConfig,nails,roomEntities,{&seed,randomUnit},&greatDoorConfig,&geoConfig,buzzerReady?&buzzerBank:nullptr,&decorBank,400.0f/240.0f,true,&plantBank,&shadeConfig,&shadeBank)||!tutorial.restoreSceneEntry(saved,entryState,gate))return fail("scene_tutorial_bind_failed");
            tutorial.enemyLife=std::move(enemyLife);
            if(!roomRenderer.loadDecor("romfs:",tutorial.decorEffects.visuals))return fail("scene_decor_failed");
            // As at startup, load room/decor while their scratch reserve is
            // available, then restore the Tutorial-only persistent atlas.
            if(!doorRenderer.ready()&&!doorRenderer.load("romfs:",doorArtwork,"great-door-page-"))return fail("scene_door_textures_failed");
            for(const auto& g:roomRules.gates)if(!std::strcmp(g.name,gate)){
                auto& h=tutorial.simulation.hero;h.position={(g.trigger.left+g.trigger.right)*.5f-2,(g.trigger.bottom+g.trigger.top)*.5f};
                h.inputEnabled=h.controlEnabled=h.gravityEnabled=true;h.velocity={};tutorial.flow.phase=hk3ds::RoomPhase::Playing;
                tutorial.flow.camera=h.position;tutorial.flow.enteredThrough(gate);tutorial.rememberTutorialGate(gate);}
            tutorial.ambientParticles.clear();
            if(hk3ds::graphicsProfile().ambientParticles){
                if(!tutorial.ambientParticles.load("romfs:/ambient-particles.bin")||
                   !tutorial.ambientParticles.bindVisuals(tutorial.decorEffects.visuals))return fail("tutorial_particles_failed");
                tutorial.ambientParticles.prewarm({tutorial.flow.camera.x,tutorial.flow.camera.y});
            }
        }else{
            tutorial.worldQuests.loadScene("romfs:",scene);
            if(nextScene.rules.stag.present){
                if(!stagReady){
                    stagReady=stagArt.load("romfs:/stag-anim.bin");
                    // Native Stag frames occupy fifteen RGBA4 1024-square pages.
                    // Admit the complete bank with the same free-linear reserve as room art.
                    stagReady=stagReady&&stagArt.pageCount<=15&&linearSpaceFree()>=stagArt.pageCount*2u*1024u*1024u+hk3ds::roomLinearReserve;
                    stagReady=stagReady&&stagRenderer.load("romfs:",stagArt,"stag-page-");
                    if(!stagReady)return fail("scene_stag_art_failed");
                }
            }else if(stagReady){stagRenderer.release();stagReady=false;}
            tutorial.resting.bindStag(stagReady?&stagArt:nullptr);
            activeScene=std::move(nextScene);tutorial.enterScene(activeScene.rules,fullTerrain,scene,gate);
            tutorial.sceneWalls.bindVisuals(residentVisuals);
            tutorial.ambientParticles.clear();
            tutorial.setSceneAmbient(false);
            if(hk3ds::graphicsProfile().ambientParticles){
                if(activeScene.defaultAmbient){
                    // SceneManager's recovered controller banks (Deepnest zones
                    // 9/10/25 and the shared zone 5 bank) use the decor atlas.
                    // Do not let the empty per-room placeholder mask them.
                    tutorial.setSceneAmbient(true,activeScene.ambientZone);
                    if(!tutorial.ambientParticles.ready()||
                       !tutorial.ambientParticles.bindVisuals(tutorial.decorEffects.visuals))return fail("scene_particles_failed");
                }else{
                    char particlePath[128];
                    std::snprintf(particlePath,sizeof(particlePath),"romfs:/scenes/%s/room-particles.bin",scene);
                    if(FILE* particleFile=std::fopen(particlePath,"rb")){
                        std::fclose(particleFile);
                        if(!tutorial.ambientParticles.load(particlePath)||
                           (tutorial.ambientParticles.ready()&&!tutorial.ambientParticles.bindVisuals(residentVisuals,true)))return fail("scene_particles_failed");
                    }
                    // Publishing an already-loaded room bank must not reload
                    // the shared zone-5 bank; setSceneAmbient preserves it.
                    tutorial.setSceneAmbient(tutorial.ambientParticles.ready());
                }
            }
            // Toll definitions must exist before choosing and binding their artwork.
            if(!tutorial.bindSceneEnemies(activeScene,"romfs:"))return fail("scene_enemies_failed");
            if(tutorial.worldQuests.present()||!std::strcmp(scene,"Room_shop")||tutorial.routeTolls.active()){
                const bool shopActor=!std::strcmp(scene,"Room_shop");
                const auto* prefix=shopActor?"sly-shop-actor":tutorial.routeTolls.active()?
                    (tutorial.routeTolls.kind==1?"city-toll-actor":tutorial.routeTolls.kind==3?"greenpath-toll-actor":"mines-toll-actor"):tutorial.worldQuests.artPrefix();
                char artPath[96],pagePrefix[64];
                std::snprintf(artPath,sizeof(artPath),"romfs:/%s-art.bin",prefix);
                std::snprintf(pagePrefix,sizeof(pagePrefix),"%s-page-",prefix);
                if(!questArt.load(artPath)||!questRenderer.load("romfs:",questArt,pagePrefix))return fail("quest_textures_failed");
                if(shopActor)tutorial.slyShopAnimation.play(questArt.require("Sly Idle"));
                else if(tutorial.routeTolls.active())tutorial.routeTolls.bindArt(&questArt);
                else tutorial.worldQuests.bindArt(&questArt);
            }
            if(!std::strcmp(scene,"Crossroads_06")){
                if(!tutorial.bindSavedShamanPillar(directory)||
                   !roomRenderer.loadActors(directory,tutorial.worldActorBank(),"shaman-pillar-"))
                    return fail("scene_shaman_pillar_failed");
            }
            if(!std::strcmp(scene,hk3ds::MantisGates::scene)){
                if(!tutorial.bindSavedMantisGates(directory)||
                   !roomRenderer.loadActors(directory,tutorial.worldActorBank(),"mantis-gate-"))
                    return fail("scene_mantis_gates_failed");
            }
            if(!std::strcmp(scene,"Crossroads_03")&&hk3ds::WorldSwitchSavedState<hk3ds::TutorialSave>::available){
                if(!worldSwitchAssets.load(directory)||
                   !tutorial.bindSavedWorldSwitch(worldSwitchAssets.data,worldSwitchAssets.art)||
                   !roomRenderer.loadActors(directory,tutorial.worldSwitchVisuals.bank(),"world-switch-"))
                    return fail("scene_world_switch_failed");
            }
            if(!std::strcmp(scene,"Town")&&hk3ds::WorldSwitchSavedState<hk3ds::TutorialSave>::available){
                recordBreadcrumb("scene_town_lift_assets");
                if(!townLiftAssets.load(directory))
                    return fail("scene_town_lift_assets_failed","Town lift controller/room/animation metadata");
                recordBreadcrumb("scene_town_lift_bind");
                if(!tutorial.bindSavedTownLift(townLiftAssets.data,townLiftAssets.art))
                    return fail("scene_town_lift_bind_failed","Town lift terrain/save/controller binding");
                recordBreadcrumb("scene_town_lift_textures");
                if(!roomRenderer.loadActors(directory,tutorial.worldActorBank(),"town-lift-"))
                    return fail("scene_town_lift_textures_failed","Town lift texture load; see preceding room-page diagnostic");
                if(!tutorial.bindSavedJijiDoor(directory))return fail("scene_jiji_door_failed");
            }
            if(!std::strcmp(scene,"Crossroads_47")&&(!tutorial.bindSavedStagStation(directory)||
                !roomRenderer.loadActors(directory,tutorial.worldActorBank(),"stag-station-art-")))
                return fail("scene_stag_station_failed");
            if(!std::strcmp(scene,"RestingGrounds_09")&&(!tutorial.bindSavedStagLever(directory)||
                !roomRenderer.loadActors(directory,tutorial.worldActorBank(),hk3ds::StagStationLever::texturePrefix)))
                return fail("scene_stag_lever_failed");
            if(!std::strcmp(scene,"Fungus2_21")&&(!tutorial.bindSavedCityCrest(directory)||
                !roomRenderer.loadActors(directory,tutorial.cityCrestDust.visuals,"city-crest-fx-")))return fail("scene_city_crest_failed");
            if(!std::strcmp(scene,"Ruins1_05b")&&(!tutorial.bindSavedWaterways(directory)||
                !roomRenderer.loadActors(directory,tutorial.worldActorBank(),"waterways-art-")))return fail("scene_waterways_machine_failed");
            if((!std::strcmp(scene,"Crossroads_49b")||!std::strcmp(scene,"Crossroads_49"))&&(!tutorial.bindSavedCityToll(directory)||
                !roomRenderer.loadActors(directory,tutorial.worldActorBank(),"city-lift-art-")))return fail("scene_city_toll_failed");
            if(tutorial.routeTolls.active()||tutorial.cityGate.active)fullTerrain.bindMotionVisuals(residentVisuals);
            if(tutorial.uumuu.active()&&!archiveGraphics.load("romfs:"))return fail("archive_textures_failed");
            if(tutorial.monomon.hasArt()){
                monomonArtwork=&tutorial.monomon.art();
                if(!monomonRenderer.load("romfs:",*monomonArtwork,tutorial.monomon.guardian()?"monomon-guardian-page-":"monomon-archive-page-"))return fail("monomon_textures_failed");
            }
            // Admit the room after its actors: the room loader must account for
            // their resident atlases when checking the unchanged linear reserve.
            if(!sceneEnemyGraphics.load(tutorial.sceneEnemies,enemyRenderer))return fail("scene_enemy_textures_failed");
            if(tutorial.crossroadsBoss.active()){
                bossTexturesAttempted=true;
                const bool knight=tutorial.crossroadsBoss.kind==hk3ds::CrossroadsBossKind::FalseKnight;
                const bool dung=tutorial.crossroadsBoss.kind==hk3ds::CrossroadsBossKind::DungDefender;
                if(!bossArtwork.load(dung?"romfs:/dung-art.bin":knight?"romfs:/false-knight-art.bin":"romfs:/boss-art.bin"))return fail("scene_boss_art_failed");
                if(!bossRenderer.load("romfs:",bossArtwork,dung?"dung-page-":knight?"false-knight-page-":"boss-page-"))return fail("scene_boss_textures_failed");
                tutorial.bindBossArt(&bossArtwork);
                if(knight&&!tutorial.crossroadsBoss.loadEffects("romfs:/false-knight-fx.bin"))return fail("scene_boss_effects_failed");
            }
            if(!loadRoomTextures())return fail("scene_textures_failed");
            if((tutorial.worldSwitch.bound()||tutorial.townLift.bound())&&!roomRenderer.actorsReady())
                return fail("scene_world_switch_textures_lost");
            // Loading the room releases its previous decor renderer. Generic
            // scenes still use the shared atlas for wall and contact particles.
            if(!roomRenderer.loadDecor("romfs:",tutorial.decorEffects.visuals))return fail("scene_decor_failed");
        }
        recordBreadcrumb("scene_greenpath");
        tutorial.collectibles.bind({});
        std::snprintf(path,sizeof(path),"%s/collectibles.bin",directory);
        tutorial.collectibles.load(path);
        if(!returning){
            std::snprintf(path,sizeof(path),"%s/greenpath-enemies.bin",directory);
            if(!tutorial.greenpath.loadScene(path,saved.flags))return fail("scene_greenpath_enemies_failed");
            bool present=false;for(const auto& a:tutorial.greenpath.actors)present|=a.visible();
            if(present&&!greenpathRenderer.ready()){
                if(!greenpathArt.load("romfs:/greenpath-art.bin")||!greenpathRenderer.load("romfs:",greenpathArt,"greenpath-page-"))return fail("scene_greenpath_art_failed");
                tutorial.greenpath.bind(&greenpathArt,&nails);
                if(!tutorial.greenpath.loadScene(path,saved.flags))return fail("scene_greenpath_enemies_failed");
            }
        }
        if(returning||!std::strcmp(scene,"Fungus2_15")){
            tutorial.ambientParticles.clear();
            const char* config=returning?"romfs:/ambient-particles.bin":
                "romfs:/scenes/Fungus2_15/ambient-particles.bin";
            if(!tutorial.ambientParticles.load(config,residentAmbientSprite)||
               !tutorial.ambientParticles.bindVisuals(tutorial.decorEffects.visuals))return fail("scene_ambient_failed");
            tutorial.setSceneAmbient(true);
        }
        const auto& mapRules=returning?roomRules:activeScene.rules;
        map.visit({scene,hk3ds::collisionMapBounds(world),world,mapRules.gates.data(),mapRules.gates.size(),mapOrigin});
        audio.sceneAudio(tutorial.sceneName());
        consoleTest.sceneEntered(tutorial.sceneName());
        diagnosticPrint("\x1b[2J\x1b[1;1H%s\nEntry: %s\n",scene,gate);
        if(auto* log=(hk3ds::diagnosticsEnabled||hk3ds::runtimeDiagnostics.enabled())?std::fopen("sdmc:/3ds/HK3DS/scene-route.txt","a"):nullptr){
            std::fprintf(log,"%s %s\n",scene,gate);std::fclose(log);}
        if(lanternReveal>0)tutorial.darkness.beginLanternReveal(lanternReveal,true);
        recordBreadcrumb("scene_ready");
        hk3ds::loadDiagnostics.finish();
        return true;
    };
    if(startupSave.resting.benchScene[0]){
        startupLocation=true;std::snprintf(startupSave.scene,32,"%s",startupSave.resting.benchScene);std::strcpy(startupSave.gate,"bench");
    }
    if(startupLocation&&!hk3ds::startupSceneMetadataAvailable("romfs:",startupSave.scene)){
        breadcrumb.set(breadcrumb.scene,sizeof(breadcrumb.scene),startupSave.scene);
        recordBreadcrumb("startup_scene_missing");
        startupLocation=false;
    }
    if(startupLocation&&!sessionFailed&&!loadScene(startupSave.scene,startupSave.gate))diagnosticPrint("Saved scene unavailable\n");
#if HK3DS_DIAGNOSTICS
    // Console tapes contain controller input only. Select a room at build time;
    // moving fixtures retain ordinary gameplay and camera following.
    const bool performanceFixture=!benchmarkReplay&&consoleTest.active()&&hk3ds::diagnosticPerformanceRoom>=0;
    const bool performanceStationary=performanceFixture&&!hk3ds::diagnosticPerformanceMoving;
    hk3ds::Vec2 performanceCamera=hk3ds::diagnosticPerformanceScene==0?hk3ds::Vec2{78,52}:
        (hk3ds::diagnosticPerformanceScene==1?hk3ds::Vec2{112,15}:hk3ds::Vec2{25,83});
    if(performanceFixture&&hk3ds::diagnosticPerformanceScene>0){
        const char* scene=hk3ds::diagnosticPerformanceScene==1?"Town":
            (hk3ds::diagnosticPerformanceScene==2?"Crossroads_07":"Crossroads_33");
        char path[128];std::snprintf(path,sizeof(path),"romfs:/scenes/%s/scene.bin",scene);
        hk3ds::SceneBank fixtureScene;
        if(!fixtureScene.load(path)||fixtureScene.rules.gates.empty()||
           !loadScene(scene,fixtureScene.rules.gates.front().name)){
            sessionFailed=true;consoleTest.stop("performance_fixture_failed");
        }
        if(hk3ds::diagnosticPerformanceScene==3)performanceCamera=tutorial.flow.camera;
    }
#endif
    if(benchmarkReplay&&replay.benchmarkRoom()==2&&!loadScene("Crossroads_07","left1")){
        sessionFailed=true;consoleTest.stop("benchmark_start_failed");
    }
    tutorial.resting.availableStations=0;
    for(unsigned i=0;i<hk3ds::stagStationCount;++i){
        char path[128];std::snprintf(path,sizeof(path),"romfs:/scenes/%s/scene.bin",hk3ds::stagStations[i].scene);
        hk3ds::SceneBank station;if(station.load(path)&&station.enter("stag"))tutorial.resting.availableStations|=1u<<i;
    }
#if HK3DS_DIAGNOSTICS
    const std::uint32_t resourceFlags=(renderer.ready()?1u:0u)|(nails.ready()?2u:0u)|
        (terrainReady?4u:0u)|(crawler.active()?8u:0u)|(doorReady?16u:0u)|(hudReady?32u:0u);
    profile.roomVram(roomRenderer.vramPages(),roomRenderer.vramBytes(),effects.vramBytes());
#endif
    loading.finish();
    previous=svcGetSystemTick();
    profile.commit(0,0,0,sectorX,sectorY);
    struct SimulationFrame {
        hk3ds::TutorialRenderState tutorial;
        hk3ds::PerformanceObservation::Ticks sections{};
        std::array<u64,3> details{};
        u64 logicTicks=0,snapshotTicks=0;
        double elapsed=0;
        unsigned disabled=0;
        unsigned epoch=~0u;
        unsigned diagnosticFlags=0;
    };
    hk3ds::FramePipeline<SimulationFrame> pipeline;
    constexpr unsigned pipelineBudget=2*1024*1024;
    for(auto& frame:pipeline.initialize())if(tutorialReady){
        frame.tutorial.initialize(tutorial,roomVisuals);
        frame.tutorial.captureWorld(world);
        platform.simulationSnapshotBytes+=frame.tutorial.memoryBytes()+sizeof(SimulationFrame)-sizeof(hk3ds::TutorialRenderState);
    }
    if(platform.simulationSnapshotBytes+hk3ds::CpuWorker::stackBytes>pipelineBudget){
        {char why[64];std::snprintf(why,sizeof(why),"pipeline_budget snap=%lu stack=%lu",static_cast<unsigned long>(platform.simulationSnapshotBytes),static_cast<unsigned long>(hk3ds::CpuWorker::stackBytes));recordBreadcrumb("pipeline_init",why);}diagnosticPrint("Simulation pipeline exceeds memory budget\n");
        C2D_Fini();C3D_Fini();gfxExit();return 4;
    }
    platform.simulationWorkerRequested=platform.new3ds&&tutorialReady&&!testCommandsEnabled;
    platform.simulationWorkerEnabled=pipeline.start(platform.simulationWorkerRequested);
    const bool profiling=profile.enabled();
    double updateElapsed=0;
    unsigned updateMask=0,sweepFrame=0;
    bool updatePaused=false;
    u32 suppressedMenuKeys=0;
    const auto update=[&](SimulationFrame& frame){
        frame.elapsed=updateElapsed;frame.disabled=updateMask;frame.sections={};frame.details={};
        hk3ds::simulationProfileDetails=&frame.details;
        auto& observation=hk3ds::performanceObservation;
        const auto previousObservation=observation;
        observation.row=profiling?&frame.sections:nullptr;
        observation.clock=[]()->std::uint64_t{return svcGetSystemTick();};
        observation.disabled=frame.disabled;
        const auto logicStart=profile.tick();
        const double elapsed=frame.elapsed;
        if(!updatePaused&&testCommandsEnabled&&tutorialReady){
            crossroadsEnemyArena.select(commandInput.enemyArenaSerial,commandInput.enemyArena,tutorial,nails,frame.tutorial.world());
            crossroadsEnemyArena.advance(elapsed,tutorial,frame.tutorial.world());
        }
        if(!updatePaused){
        if(tutorialReady){
#if HK3DS_DIAGNOSTICS
            auto& captureVitals=tutorial.simulation.hero.vitals;
            if(performanceStationary){
                auto& h=tutorial.simulation.hero;h.position=performanceCamera;
                h.velocity={};h.damageVelocity={};h.presentation.reset(h.position);
                h.gravityEnabled=h.controlEnabled=h.inputEnabled=false;
                tutorial.flow.phase=hk3ds::RoomPhase::Playing;tutorial.flow.transition=nullptr;
            }
            const hk3ds::Vec2 captureCamera=testFrameCaptureActive?hk3ds::Vec2{hk3ds_test_frames.cameraX,hk3ds_test_frames.cameraY}:hk3ds::Vec2{};
            tutorial.advance(hk3ds::frameCaptureElapsed(hk3ds_test_frames,testFrameCaptureActive,elapsed),input,
                             performanceStationary?&performanceCamera:
                             (testFrameCaptureActive&&!hk3ds_test_frames.followCamera?&captureCamera:nullptr));
            if(performanceStationary)tutorial.flow.camera=performanceCamera;
            if(testFrameCaptureActive){
                hk3ds_test_frames.presentationAlpha=tutorial.presentationTime().alpha;
                hk3ds_test_frames.vignetteScale=tutorial.darkness.scale();
                hk3ds_test_frames.darknessRegion=tutorial.darkness.region();
                hk3ds_test_frames.heroLightScale=tutorial.heroLight.scale();
                hk3ds_test_frames.heroLightAlpha=lighting.config().transform[3];
                hk3ds_test_frames.heroLightAge=tutorial.heroLight.scaledAge();
                hk3ds_test_frames.heroLightHits=tutorial.simulation.hero.lightDamageSerial;
                hk3ds_test_frames.heroLightHealth=captureVitals.health;
            }
            if(testFrameCaptureActive){const auto p=tutorial.heroRenderPosition();
                hk3ds_test_frames.heroX=p.x;hk3ds_test_frames.heroY=p.y;
                hk3ds_test_frames.observedSoul=captureVitals.soul;hk3ds_test_frames.observedMaxSoul=captureVitals.maxSoul;}
#else
            tutorial.advance(elapsed,input);
#endif
            tutorial.decorEffects.present(tutorial.presentationTime(),testFrameCaptureActive?tutorial.testDecorLayers:7);
            if(!tutorial.sceneActive()){
            tutorial.presentPlants();tutorial.roomAmbient.present(tutorial.presentationTime(),tutorial.levelSeconds());
            tutorial.contactEffects.present(tutorial.decorEffects.visuals,tutorial.presentationTime());
            tutorial.stalactiteVisuals.present(tutorial.stalactites.data(),tutorial.stalactiteCount,tutorial.presentationTime(),tutorial.decorEffects.visuals);}}
        else simulation.advance(elapsed,input,world,bank,&combat,&crawler);
        }
        if(roomVisuals)fullTerrain.presentMotion(residentVisuals,tutorial.presentationTime());
        if(roomVisuals)tutorial.sceneWalls.presentVisuals(residentVisuals,tutorial.presentationTime());
        frame.logicTicks=profile.tick()-logicStart;
        const auto snapshotStart=profile.tick();
        if(tutorialReady){
            // Presentation generation belongs to the producer, never the GPU consumer.
            tutorial.presentAmbient();
            tutorial.cityCrestDust.present(tutorial.presentationTime());
            tutorial.shamanPillar.present(tutorial.presentationTime());
            tutorial.stagLever.present(tutorial.presentationTime());
            if(frame.epoch!=tutorial.audioEpoch){frame.tutorial.initialize(tutorial,roomVisuals);frame.epoch=tutorial.audioEpoch;}
            frame.tutorial.capture(tutorial,roomVisuals,!testFrameCaptureActive);
            frame.tutorial.captureWorld(world);
        }
        if(hk3ds::consoleOverlay.enabled){
            frame.diagnosticFlags=tutorialReady?tutorial.saved().flags:0;
            if(tutorialReady)frame.tutorial.captureDiagnosticBounds(tutorial);
        }
        frame.snapshotTicks=profile.tick()-snapshotStart;
        observation=previousObservation;
        hk3ds::simulationProfileDetails=nullptr;
    };
    const auto beginUpdate=[&](){
        if(hk3ds::loadDiagnostics.visible()){showLoadDiagnostics();mapRenderer.invalidate();}
        hk3ds::runtimePhase(hk3ds::RuntimePhase::Input);
        if(consoleTest.active()&&!replay.active())return false;
        {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Input);
        hidScanInput();
        const bool chordHeld=hk3ds::diagnosticComboHeld(replay.active()?replay.keys():hidKeysHeld());
        const double chordTime=chordHeld?double(svcGetSystemTick())/SYSCLOCK_ARM11:0;
        if(hk3ds::diagnosticChordPoll(hk3ds::consoleOverlay,chordHeld,chordTime)){
            hk3ds::toggleDiagnosticLogging(chordTime);mapRenderer.invalidate();
        }
        const u32 pressed = hidKeysDown()&~(chordHeld?diagnosticChord:0);
        if(tutorialReady&&hudReady&&hudRenderer.readingPanelVisible(&tutorial.conditionalEvents,&tutorial.worldQuestDialogue,&tutorial.cartographer,&tutorial.abilityPickup,tutorial.greenpath.introActive()?tutorial.greenpath.introText(tutorial.greenpath.introPage()):nullptr))
            hudRenderer.turnReadingPage(int(bool(pressed&KEY_R))-int(bool(pressed&KEY_L)));
        if(consoleTest.shouldStop(diagnosticFrame,replay.tick(),replay.total()))return false;
        if(builtinReplay&&(!replay.active()||replay.exhausted()||(replay.keys()&KEY_START)||
            svcGetSystemTick()-replayStarted>u64(SYSCLOCK_ARM11)*600)){
            replayResult=!replay.active()?"Invalid replay":
                replay.exhausted()||(replay.keys()&KEY_START)?"Tape finished":"Replay timeout";
            return false;
        }
        const bool canPause=tutorialReady&&hudReady&&!consoleTest.active()&&!builtinReplay&&!replay.active();
        if ((!canPause&&(pressed & KEY_START)) || (replay.keys() & KEY_START)) {
            consoleTest.stop((pressed&KEY_START)?"user_abort":"replay_start");startExit=true;return false;
        }
        if (hk3ds::diagnosticsEnabled&&hk3ds_test_shutdown==0x484b4558u) return false;
        const bool wasMenu=menu.active();
        const bool wasInventory=inventory.opened();
        if(canPause&&(pressed&KEY_START))inventory.returnToMap();
        if(canPause&&!inventory.opened()&&!tutorial.worldQuestDialogue.locked()&&!tutorial.jijiDoor.controlsHero()&&
           !tutorial.waterways.controlsHero()&&!tutorial.cityToll.controlsHero()&&!tutorial.cityLift.controlsHero()&&!tutorial.cityTransport.locked()){
            touchPosition touch{};if(pressed&KEY_TOUCH)hidTouchRead(&touch);
            const auto action=wasMenu&&(pressed&KEY_TOUCH)?menu.tap(touch.px,touch.py):menu.update(int(bool(pressed&(KEY_DOWN|KEY_CPAD_DOWN)))-int(bool(pressed&(KEY_UP|KEY_CPAD_UP))),
                int(bool(pressed&(KEY_RIGHT|KEY_CPAD_RIGHT)))-int(bool(pressed&(KEY_LEFT|KEY_CPAD_LEFT))),pressed&KEY_A,pressed&KEY_B,pressed&KEY_START);
            if(action==hk3ds::PauseMenu::Action::SettingsChanged){
                audio.volumes(menu.settings.musicVolume,menu.settings.soundVolume);
                menu.saveFailed=!menu.settings.write(settingsPath);
            }
            if(action==hk3ds::PauseMenu::Action::QuitToMenu){
                writeSave(true);
                returnToTitle=true;return false;
            }
            if(action==hk3ds::PauseMenu::Action::ExitGame)return false;
        }
        bool menuInput=wasMenu||menu.active();

        if (!tutorialReady&&(pressed & KEY_SELECT)) {reset(simulation,terrainReady?&terrain:nullptr); resetTargets(targets); spawnEnemy(); combat.bind(targets.data(),targets.size(),&nails);input = {};hud.reset(hudSnapshot());hudRenderer.invalidate();}
        if(!menuInput&&inventoryReady&&tutorialReady&&!tutorial.worldQuestDialogue.locked()&&!tutorial.jijiDoor.controlsHero()&&!tutorial.waterways.controlsHero()&&!tutorial.cityToll.controlsHero()&&!tutorial.cityLift.controlsHero()&&!tutorial.cityTransport.locked()&&!builtinReplay&&!consoleTest.active()&&!replay.active()&&!commandInput.active){
            const auto& hero=tutorial.simulation.hero;
            inventorySnapshot={hero.vitals,hero.abilities,tutorial.geo,
                (savedFlags()&hk3ds::TutorialSave::cityCrestFlag)!=0,hero.abilities.spiritLevel()};
            if(inventory.opened()&&(pressed&KEY_B))inventory.returnToMap();
            inventory.validate(inventorySnapshot);
            if(inventory.opened()){
                const int x=int(bool(pressed&(KEY_RIGHT|KEY_CPAD_RIGHT)))-int(bool(pressed&(KEY_LEFT|KEY_CPAD_LEFT)));
                const int y=int(bool(pressed&(KEY_DOWN|KEY_CPAD_DOWN)))-int(bool(pressed&(KEY_UP|KEY_CPAD_UP)));
                if(inventory.tab()==hk3ds::InventoryTab::Inventory)inventory.navigate(x+5*y,inventorySnapshot);
                else if(inventory.tab()==hk3ds::InventoryTab::Charms){
                    if(x||y)tutorial.charmsPage.move(x+7*y);
                    if(pressed&KEY_A)tutorial.toggleCharm();
                }
            }
            if(pressed&KEY_TOUCH){
                touchPosition touch{};hidTouchRead(&touch);
                if(!inventory.touchBottomTab(touch.px,touch.py)&&inventory.tab()==hk3ds::InventoryTab::Charms){
                    for(unsigned i=0;i<hk3ds::charmDefinitions.size();++i){
                        const float x=hk3ds::inventorylayout::charmX(i,true),y=hk3ds::inventorylayout::charmY(i,true);
                        if(std::abs(float(touch.px)-x)<=17&&std::abs(float(touch.py)-y)<=17){
                            if(tutorial.charmsPage.selected==i)tutorial.toggleCharm();
                            else {tutorial.charmsPage.selected=i;tutorial.charmsPage.overcharmAttempts=0;}
                            break;
                        }
                    }
                }
            }
        }
        menuInput=menuInput||wasInventory||inventory.opened();
        suppressedMenuKeys &= hidKeysHeld();
        if(menuInput){suppressedMenuKeys|=hidKeysHeld();input.clear();}
        updatePaused=menu.active()||inventory.opened();audio.pause(updatePaused);
        cartographerAudio.pause(updatePaused);
        if (hk3ds::diagnosticsEnabled&&!simulation.hero.abilities.superDash&&!consoleTest.active()&&!menuInput&&(pressed & KEY_A)) showHitboxes=!showHitboxes;
        if (!tutorialReady&&(pressed & KEY_SELECT)) ++diagnosticResets;
        if (!tutorialReady&&(pressed & KEY_L)) simulation.hero.takeDamage(1,simulation.hero.facingRight ? 1 : -1);
        if(!replay.active()&&!commandInput.active&&!menuInput)sample(input,suppressedMenuKeys,tutorialReady&&tutorial.shopMenuOpen());
        }

        const u64 now=svcGetSystemTick();
        // Fixed 30 Hz presentation, existing 60 Hz InputLatch/simulation steps.
        // Profiling still uses real hardware clocks, including GPU waits.
        updateElapsed=benchmarkReplay?1.0/30.0:static_cast<double>(now-previous)/SYSCLOCK_ARM11;previous=now;
        const unsigned requested=hk3ds_perf_disable_mask;
        const unsigned slot=(sweepFrame++/30)%8;
        updateMask=profiling?((requested&0x80000000u)?(slot?1u<<(slot-1):0):(requested&hk3ds::PerformanceObservation::allFeatures)):0;
        if(tutorialReady){
            // Arena art is loaded on demand; normal Tutorial_01 reserves no texture memory.
#if HK3DS_DIAGNOSTICS
            if(testCommandsEnabled&&hk3ds_test_command.magic==0x484b5443u&&hk3ds_test_command.op==19&&
               hk3ds_test_command.sequence!=hk3ds_test_command.ack&&!enemyCRenderer.ready()){
                if(enemyCArt.load("romfs:/enemy-c-anim.bin")&&enemyCRenderer.load("romfs:",enemyCArt,"enemy-c-page-"))
                    tutorial.enemyC.bind(&enemyCArt,&nails,{&seed,randomUnit});
            }
            if(testCommandsEnabled&&hk3ds_test_command.magic==0x484b5443u&&hk3ds_test_command.op==20&&
               hk3ds_test_command.sequence!=hk3ds_test_command.ack&&!greenpathRenderer.ready()){
                if(greenpathArt.load("romfs:/greenpath-art.bin")&&greenpathRenderer.load("romfs:",greenpathArt,"greenpath-page-"))
                    tutorial.greenpath.bind(&greenpathArt,&nails);
            }
            // Capture-only travel uses the normal loader and acknowledges after resources are ready.
            if(testCommandsEnabled&&hk3ds_test_command.magic==0x484b5443u&&hk3ds_test_command.op==22&&
               hk3ds_test_command.sequence!=hk3ds_test_command.ack){
                char scene[64]{};bool valid=false;
                for(unsigned i=0;i<sizeof(scene);++i){
                    const char c=hk3ds_test_frames.requestedScene[i];
                    if(!c){valid=i>0;break;}
                    if(i==sizeof(scene)-1||!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))break;
                    scene[i]=c;
                }
                char gate[64]{};
                for(unsigned i=0;i<sizeof(gate)-1;++i)gate[i]=hk3ds_test_frames.requestedGate[i];
                const bool validGate=hk3ds::validGateIdentifier(gate);
                pipeline.finish();
                // Captures can travel with a streamed cue still playing. Keep
                // its NDSP queue paused while synchronous resource I/O runs.
                audio.pause(true);
                hk3ds_test_command.result=valid&&validGate&&loadScene(scene,gate)?0u:2u;
                audio.pause(false);
                hk3ds_test_command.ack=hk3ds_test_command.sequence;
                previous=svcGetSystemTick();
            }
            hk3ds::pollTestCommand(hk3ds_test_command,tutorial,testCommandsEnabled,&commandInput);
#endif
            if(tutorial.crossroadsBoss.active()&&!bossTexturesAttempted){
                bossTexturesAttempted=true;
                C3D_FrameSync();roomRenderer.release();
                if(romfs&&bossArtwork.load("romfs:/boss-art.bin")&&bossRenderer.load("romfs:",bossArtwork,"boss-page-"))
                    tutorial.bindBossArt(&bossArtwork);
                else hk3ds::diagnosticLog("Boss artwork failed to load\n");
            }
#if HK3DS_DIAGNOSTICS
            testFrameCaptureActive=hk3ds::diagnosticsEnabled&&hk3ds::frameCaptureEnabled(hk3ds_test_frames,testCommandsEnabled);
            tutorial.setFrameCaptureActive(testFrameCaptureActive,hk3ds_test_frames.pinHero!=0);
            auto& vitals=tutorial.simulation.hero.vitals;
            hk3ds::frameCaptureSoulFixture(hk3ds_test_frames,testFrameCaptureActive,vitals.soul,vitals.maxSoul);
#endif
        }
        pipeline.submit(update);
        return true;
    };
    bool queued=false;
    hk3ds::loadDiagnostics.finish();
    showLoadDiagnostics();
    while (!sessionFailed&&(!tutorialReady||visualsReady)&&aptMainLoop())
    {
        profile.next(sectorX,sectorY);
        if(!queued&&!beginUpdate())break;
        const auto waitStart=profile.tick();
        hk3ds::runtimePhase(hk3ds::RuntimePhase::CpuWait);
        pipeline.finish();queued=false;
        if(monomonMovie.complete()||monomonMovieFailed)tutorial.monomon.cinematicEnded();
        const auto waitTicks=profile.tick()-waitStart;
        const auto& frame=pipeline.published();
        platform.simulationSnapshotBytes=std::max(platform.simulationSnapshotBytes,unsigned(pipeline.storageBytes(
            [](const SimulationFrame& state){return state.tutorial.memoryBytes()+sizeof(SimulationFrame)-sizeof(hk3ds::TutorialRenderState);})));
        const double elapsed=frame.elapsed;
        monomonMovieStep=elapsed;
        hk3ds::performanceObservation.disabled=frame.disabled;
        // GPU diagnostics belong to the consumer; the simulation keeps its own TLS row.
        hk3ds::performanceObservation.serializedGpu=profiling&&(hk3ds_perf_disable_mask&0x40000000u)!=0;
        hk3ds::performanceObservation.gpuStagesOnly=hk3ds::performanceObservation.serializedGpu&&(hk3ds_perf_disable_mask&0x10000000u)!=0;
        hk3ds::performanceObservation.gpuCategory=hk3ds::PerformanceSection::GpuBackground;
        hk3ds::performanceObservation.gpuFrameSelected=
            hk3ds::performanceObservation.serializedGpu&&
            (hk3ds::performanceObservation.gpuStagesOnly||(sweepFrame-1)%hk3ds::gpuProfileStride==0);
        hk3ds::performanceObservation.cpuProfileEnabled=profiling&&
            (HK3DS_PERF_DISABLE_MASK&0x20000000u)!=0&&(hk3ds_perf_disable_mask&0x20000000u)!=0;
        hk3ds::performanceObservation.cpuFrameSelected=hk3ds::performanceObservation.cpuProfileEnabled&&
            (sweepFrame-1)%hk3ds::gpuProfileStride==0;
        if(hk3ds::performanceObservation.serializedGpu)
            hk3ds::performanceObservation.owners.begin((sweepFrame-1)/hk3ds::gpuProfileStride);
        profile.simulation(frame.logicTicks,frame.snapshotTicks,waitTicks,frame.sections,frame.details,pipeline.asynchronous());
        if(tutorialReady)map.cartography(tutorial.cartographer.progress);
        const auto& current=tutorialReady?tutorial.simulation:simulation;
        if(testFrameCaptureActive&&tutorialReady){
            hk3ds_test_frames.reminderKind=unsigned(tutorial.prompts.reminder());
            hk3ds_test_frames.promptFlags=tutorial.prompts.savedFlags();
            hk3ds_test_frames.reminderAlpha=tutorial.prompts.alpha();
            hk3ds_test_frames.tabletAlpha=tutorial.prompts.tabletAlpha();
            hk3ds_test_frames.inspectionPhase=unsigned(tutorial.inspection.phase());
            hk3ds_test_frames.inspectionOwner=tutorial.inspection.owner();
            hk3ds_test_frames.inspectionPage=tutorial.inspection.page();
            hk3ds_test_frames.inspectionCharacters=tutorial.inspection.visibleCharacters();
            hk3ds_test_frames.inspectionLocked=tutorial.inspection.locked();
            hk3ds_test_frames.inspectionSerial=tutorial.inspection.serial();
            hk3ds_test_frames.inspectionAlpha=tutorial.inspection.alpha();
            hk3ds_test_frames.inspectionHum=0;
            const auto& observedBoss=frame.tutorial.boss;
            hk3ds_test_frames.bossKind=unsigned(observedBoss.kind);
            hk3ds_test_frames.bossPhase=unsigned(observedBoss.phase);
            hk3ds_test_frames.bossVisible=observedBoss.visible;
            hk3ds_test_frames.bossX=observedBoss.position.x;
            hk3ds_test_frames.bossY=observedBoss.position.y;
            hk3ds_test_frames.bossParticles=observedBoss.effects.size();
            hk3ds_test_frames.bossHeroHealth=frame.tutorial.simulation.hero.vitals.health;
            const auto* observedSprite=observedBoss.animation.current(bossArtwork);
            hk3ds_test_frames.bossSprite=observedSprite?int(observedSprite-bossArtwork.sprites.data()):-1;
            // Reuse the existing diagnostic mailbox for live Protector
            // captures; 0x100 cannot alias a Crossroads boss kind.
            for(unsigned i=0;i<tutorial.greenpath.actors.size();++i){
                const auto& actor=tutorial.greenpath.actors[i];
                if(actor.kind!=hk3ds::GreenpathKind::Hornet)continue;
                hk3ds_test_frames.bossKind=0x100;
                hk3ds_test_frames.bossPhase=unsigned(actor.phase);
                hk3ds_test_frames.bossVisible=actor.visible();
                hk3ds_test_frames.bossX=actor.position.x;
                hk3ds_test_frames.bossY=actor.position.y;
                hk3ds_test_frames.bossParticles=unsigned(std::max(0,tutorial.greenpath.objects[i].health.hp));
                const auto* sprite=actor.animation.current(greenpathArt);
                hk3ds_test_frames.bossSprite=sprite?int(sprite-greenpathArt.sprites.data()):-1;
                break;
            }
            for(const auto& tablet:tutorial.roomAmbient.tablets())if(tablet.config.owner==tutorial.inspection.owner())
                hk3ds_test_frames.inspectionHum=tablet.hum();
        }
        if(hudReady){
            hudRenderer.advanceMenu(static_cast<float>(elapsed));
            if(tutorialReady){hudPrompts=frame.tutorial.prompts;hudInspection=frame.tutorial.inspection;}
            hudRenderer.observePrompts(tutorialReady?&hudPrompts:nullptr);
            hudRenderer.observeInspection(tutorialReady?&hudInspection:nullptr);
            if(tutorialReady&&hudEpoch!=tutorial.audioEpoch){
                hudEpoch=tutorial.audioEpoch;hud.reset(hudSnapshot());
                hudSeconds=tutorial.levelSeconds();hudRenderer.invalidate();
            }else{
                const float nextSeconds=tutorialReady?tutorial.levelSeconds():0;
                const float dt=tutorialReady?std::max(0.0f,nextSeconds-hudSeconds):
                    static_cast<float>(std::clamp(elapsed,0.0,hk3ds::tuning::controlStep*5))*current.clock.scale;
                hudSeconds=nextSeconds;
                if(hud.update(hudSnapshot(),dt))hudRenderer.invalidate();
                hudRenderer.advance(dt,hud);
            }
        }
        {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Save);
        hk3ds::runtimePhase(hk3ds::RuntimePhase::Save);
        // A pending gate defers the save until the next room has loaded.
        if(tutorialReady&&!builtinReplay&&hk3ds::saveMayStart(tutorial.flow.transition!=nullptr))writeSave(false);
        }
        if(tutorialReady){
            hk3ds::audioEvidence.beforeUpdate(diagnosticFrame+1,replay.tick());
            hk3ds::audioEvidence.sampleGameplay(diagnosticFrame+1,replay.tick(),current.hero,
                tutorial.decorEffects.decorSoundSerial(),tutorial.decorEffects.soundPitch(),
                tutorial.decorEffects.plantSoundSerial(),tutorial.decorEffects.plantSoundPitch(),tutorial.audioEpoch);
            hk3ds::audioEvidence.sampleScene(diagnosticFrame+1,tutorial);
        }
        hk3ds::runtimePhase(hk3ds::RuntimePhase::Audio);
        if(!menu.active()) {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Audio);audio.update(current.hero);
         if(tutorialReady){audio.updateDecor(tutorial.decorEffects.decorSoundSerial(),tutorial.decorEffects.soundPitch());
            audio.updatePlant(tutorial.decorEffects.plantSoundSerial(),tutorial.decorEffects.plantSoundPitch());
            const auto arenaSound=crossroadsEnemyArena.audio(tutorial.flow.camera);
            // Cornifer and tablets occupy different rooms. Release their shared
            // hum channel before starting the next room's tablet audio.
            if(audio.ready()&&!updatePaused&&!tutorial.cartographer.present&&!tutorial.cityGate.active)
                cartographerAudio.update(tutorial.cartographer,current.hero.position,menu.settings.soundVolume);
            audio.updateEnemies(tutorial,&arenaSound);
            if(audio.ready()&&!updatePaused){
                if(tutorial.cityGate.active)cartographerAudio.updateGate(tutorial.cityGate,menu.settings.soundVolume);
                else if(tutorial.cartographer.present)cartographerAudio.update(tutorial.cartographer,current.hero.position,menu.settings.soundVolume);
            }}}
        if(tutorialReady)hk3ds::audioEvidence.afterUpdate();
        if(tutorialReady)
        {
            const auto camera=tutorial.flow.camera;
            // Capture coordinates only. Resource readiness is independent of
            // camera cells, direction, speed, teleports and the input replay.
            sectorX=static_cast<int>(std::floor(camera.x/12));
            sectorY=static_cast<int>(std::floor(camera.y/8));
        }
        {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Diagnostics);
        if (hk3ds::diagnosticsEnabled&&!hudReady&&++displayCounter >= 15)
        {
            displayCounter = 0;
            const auto& h = current.hero;
            diagnosticPrint("\x1b[10;1HHP %d/%d  Soul %d   \nX %6.2f Y %6.2f   \nVX %6.2f VY %6.2f \nPhysics ticks %llu   \nOverloads %llu     \nContacts %u  limit %d    \nCrawler HP %d    ",h.vitals.health,h.vitals.maxHealth,h.vitals.soul,
                        h.position.x,h.position.y,h.velocity.x,h.velocity.y,
                        static_cast<unsigned long long>(current.physicsTicks),static_cast<unsigned long long>(current.overloads),static_cast<unsigned>(crawler.terrainContacts.count),crawler.terrainContacts.limited,targets[2].health.hp);
            if(tutorialReady)diagnosticPrint("\nHits %u  kills %u\nDoors %u  floor %u\nDeaths %u retries %u\nRoom phase %u \nArtwork %s failures %u\n%s",unsigned(tutorial.flow.progress.hits),unsigned(tutorial.flow.progress.kills),unsigned(tutorial.flow.progress.broken),tutorial.floorHits,
                unsigned(tutorial.flow.progress.deaths),unsigned(tutorial.flow.progress.retries),unsigned(tutorial.flow.phase),visualsReady?"loaded":"missing",roomRenderer.loadFailures(),
                tutorial.flow.transition?"Loading next scene...\n":"                                  \n");
        }
        }
        {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Diagnostics);
        #if HK3DS_DIAGNOSTICS
        // Publish before drawing so an unfinished render remains observable.
        // Never feed diagnostics back into simulation.
        hk3ds::runtimePhase(hk3ds::RuntimePhase::Publish);
        auto& d=hk3ds_diagnostics;
        const auto sequence=++diagnosticFrame*2;
        d.sequence=sequence-1;
        d.version=5; d.frame=diagnosticFrame; d.status=1; d.resources=resourceFlags;
        d.held=replay.active()?replay.keys():hidKeysHeld(); d.resets=diagnosticResets; d.overlay=showHitboxes;
        d.replayActive=replay.active();d.replayTick=replay.tick();d.replayTotal=replay.total();
        d.controlTicks=static_cast<std::uint32_t>(current.controlTicks);
        d.physicsTicks=static_cast<std::uint32_t>(current.physicsTicks);
        d.overloads=static_cast<std::uint32_t>(current.overloads);
        const auto& observedHero=current.hero;
        d.attackSerial=observedHero.nail.serial; d.health=observedHero.vitals.health;
        // Tutorial objects begin with its crawlers; the fixture is not advanced there.
        const auto& observedCrawler=tutorialReady?tutorial.crawlers[0]:crawler;
        const auto& observedEnemy=tutorialReady?tutorial.objects[0]:targets[2];
        d.soul=observedHero.vitals.soul; d.enemyHealth=observedEnemy.health.hp;
        d.actor=static_cast<std::uint32_t>(observedHero.actor); d.dashing=observedHero.dashing;
        d.doorBroken=door.broken(); d.x=observedHero.position.x; d.y=observedHero.position.y;
        d.vx=observedHero.velocity.x; d.vy=observedHero.velocity.y;
        d.enemyX=observedCrawler.position.x; d.enemyY=observedCrawler.position.y;
        d.roomPhase=tutorialReady?static_cast<unsigned>(tutorial.flow.phase):0xffffffffu;
        d.deaths=tutorial.flow.progress.deaths;d.retries=tutorial.flow.progress.retries;
        d.hitCount=tutorial.flow.progress.hits;d.killCount=tutorial.flow.progress.kills;
        d.brokenMask=tutorial.flow.progress.broken;
        const auto* exit=tutorial.flow.transition;
        d.exitGate=exit && std::strcmp(exit->scene,"Town")==0 && std::strcmp(exit->entry,"left1")==0;
        d.abilities=observedHero.abilities.dash|observedHero.abilities.wallJump<<1|observedHero.abilities.doubleJump<<2;
        d.visualLoaded=visualsReady&&(!hk3ds::graphicsProfile().lighting||lighting.ready())&&(!hk3ds::graphicsProfile().postprocessing||effects.ready());
        d.visualFailures=roomRenderer.loadFailures()+(visualsReady&&hk3ds::graphicsProfile().lighting&&!lighting.ready()?1u:0u)+(tutorialReady&&hk3ds::graphicsProfile().postprocessing&&!effects.ready()?1u:0u);
        d.audioReady=audio.ready();d.audioPlayed=audio.played;
        d.audioError=audio.error();d.linearFree=linearSpaceFree();
        d.floorHits=tutorial.floorHits;d.greatDoorHits=tutorial.greatDoor.hits();d.collapsed=tutorial.flow.progress.collapsed;d.hazardRespawns=tutorial.flow.progress.hazardRespawns;
        d.sequenceEnd=sequence; d.sequence=sequence;
        consoleMetrics.sample(d,sectorX,sectorY);
#endif
        }
        // Everything below reads only the published slot and render-owned state.
        // Sample the next input only after diagnostics/audio/save consumed this tick.
        const auto renderOverloads=current.overloads;
        const auto renderPhase=tutorialReady?static_cast<unsigned>(tutorial.flow.phase):~0u;
        const bool renderHitboxes=showHitboxes;
        hk3ds::CrawlerRenderState fallbackCrawler;
        fallbackCrawler.capture(crawler,current.presentationTime());
        const auto renderWorld=tutorialReady?frame.tutorial.world():world;
        if(hk3ds::consoleOverlay.enabled){
            const auto& pose=tutorialReady?frame.tutorial.simulation.hero:current.hero;
            const auto& rules=tutorialReady&&frame.tutorial.sceneActive?activeScene.rules:roomRules;
            const unsigned abilityBits=unsigned(pose.abilities.dash)|(unsigned(pose.abilities.wallJump)<<1)|
                (unsigned(pose.abilities.doubleJump)<<2)|(unsigned(pose.abilities.superDash)<<3)|
                (unsigned(pose.abilities.shadeDash)<<4)|(unsigned(pose.abilities.vengefulSpirit)<<5)|
                (unsigned(pose.abilities.acidArmour)<<6)|(unsigned(pose.abilities.shadeSoul)<<7);
            const auto used=mallinfo().uordblks;
            const auto heap=envGetHeapSize();
            hk3ds::consoleOverlay.fill(HK3DS_BUILD_COMMIT,__DATE__,tutorialReady?frame.tutorial.sceneName:"fixture",
                tutorialReady?frame.tutorial.heroRenderPosition():pose.position,rules,selectedSlot,
                frame.diagnosticFlags,linearSpaceFree(),vramSpaceFree(),used<heap?heap-used:0,abilityBits);
        }
        const bool sceneTransition=tutorialReady&&tutorial.flow.transitionReady()&&!builtinReplay;
        bool lastFrame=false;
        if(pipeline.asynchronous()&&!sceneTransition){
            queued=beginUpdate();lastFrame=!queued;
        }
        if(!hk3ds::diagnosticsEnabled||!hk3ds::frameCaptureSkipRender(hk3ds_test_frames,testFrameCaptureActive))
        draw(top,tutorialReady?frame.tutorial.simulation:current,bank,renderer,nails,targets,renderHitboxes,fallbackCrawler,enemyBank,enemyRenderer,buzzerBank,buzzerRenderer,renderWorld,terrainReady,
             tutorialReady?&frame.tutorial:nullptr,tutorialReady?&frame.tutorial.roomVisuals:roomVisuals,visualsReady?&roomRenderer:nullptr,
             visualsReady&&lighting.ready()?&lighting:nullptr,visualsReady&&effects.ready()?&effects:nullptr,false,&geoArtwork,&geoRenderer,&doorArtwork,&doorRenderer,
             hudReady?&hudRenderer:nullptr,&hud,shadeReady?&shadeBank:nullptr,shadeReady?&shadeRenderer:nullptr,&furyArtwork,&furyRenderer,&eventArt,&eventRenderer,&questArt,&questRenderer,&enemyCArt,&enemyCRenderer,&bossArtwork,&bossRenderer,
             mapReady?&mapRenderer:nullptr,&map,inventoryReady?&inventoryRenderer:nullptr,&inventory,&inventorySnapshot,&menu,&sceneEnemyGraphics,bottom,&hudRenderer,corniferReady?&corniferArt:nullptr,corniferReady?&corniferRenderer:nullptr,stagReady?&stagArt:nullptr,stagReady?&stagRenderer:nullptr);
        profile.commit(diagnosticFrame,renderPhase,renderOverloads,sectorX,sectorY,replay.tick());
        if(sceneTransition){
            hk3ds::runtimePhase(hk3ds::RuntimePhase::CpuWait);
            pipeline.finish();queued=false;
            const auto gate=*tutorial.flow.transition;
            hk3ds::consoleOverlay.transition(tutorial.sceneName(),gate.name,gate.scene,gate.entry);
            if(!loadScene(gate.scene,gate.entry,gate.name)){
                hk3ds::diagnosticLog("Scene load failed: %s / %s\n",gate.scene,gate.entry);
                break;
            }
            previous=svcGetSystemTick();
        }
        if(lastFrame)break;
    }
    hk3ds::runtimePhase(hk3ds::RuntimePhase::Cleanup);
    hk3ds::releaseDiagnosticOverlay();
    pipeline.stop();showLoadDiagnostics();tutorial.uumuu.reset();tutorial.monomon.reset();archiveGraphics.release();monomonRenderer.release();monomonMovie.release();crossroadsEnemyArena.release();
    // A distinct shutdown record includes final worker joins, outside gameplay percentiles.
    profile.next(sectorX,sectorY);
    {hk3ds::FrameProfile::Scope timing(profile,hk3ds::FrameProfile::Save);
     if(tutorialReady&&visualsReady&&!sessionFailed&&!builtinReplay)writeSave(true);}
    profile.commit(diagnosticFrame+1,~0u,tutorialReady?tutorial.simulation.overloads:simulation.overloads,sectorX,sectorY,replay.tick());
    char metricsPath[192],stagesPath[192];
    if constexpr(hk3ds::diagnosticsEnabled){
    consoleTest.path(metricsPath,sizeof(metricsPath),"metrics.csv");
    consoleTest.path(stagesPath,sizeof(stagesPath),"stages.csv");
    }
    cartographerAudio.release();
    hk3ds::audioEvidence.freeze();
    const bool metricsOk=consoleMetrics.finish(platform,consoleTest.active()?metricsPath:nullptr);
#if HK3DS_DIAGNOSTICS
    profile.diagnosticGameFlags=hk3ds::effectiveDiagnosticGameFlags();
    profile.diagnosticPerformanceRoom=hk3ds::diagnosticPerformanceRoom;
    profile.fogTrimDraws=roomRenderer.trimDraws();profile.fogTrimQuads=roomRenderer.trimQuads();
    profile.backgroundWidth=effects.backgroundPassWidth();
#endif
    const bool stagesOk=profile.finish(platform,consoleTest.active()?stagesPath:nullptr,
        [](void* value){static_cast<hk3ds::AudioPlayer*>(value)->release();},&audio);
    const bool frameOk=consoleTest.active()&&consoleTest.capture(top);
    if(builtinReplay&&replayResult){
        audio.release();
        // The replay result is an explicit diagnostic mode outside gameplay.
        const auto* exit=tutorial.flow.transition;
        const bool reached=exit&&std::strcmp(exit->scene,"Town")==0&&std::strcmp(exit->entry,"left1")==0;
        diagnosticPrint("\x1b[2J\x1b[H%s\nSteps: %lu / %lu\nExit Town / left1: %s\nAnimation missing plays: %u\nNormal save unchanged.\nSTART: quit\n",replayResult,
            static_cast<unsigned long>(replay.tick()),static_cast<unsigned long>(replay.total()),reached?"reached":"NOT reached",unsigned(hk3ds::animationMissingPlayCount()));
        #if HK3DS_DIAGNOSTICS
        hk3ds_replay_result=reached?2u:1u;
#endif
        while(aptMainLoop()){
            hidScanInput();if(hidKeysDown()&KEY_START)break;
            if(hk3ds::diagnosticsEnabled&&hk3ds_test_shutdown==0x484b4558u)break;
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            hk3ds::textcapture::select(1,true);
            hudRenderer.drawBottomPanel(bottom);
            hudRenderer.inventoryText(replayResult,160,100);
            hudRenderer.inventoryText(reached?"Town reached":"Town not reached",160,130);
            hudRenderer.inventoryText("START: quit",160,170);
            C3D_FrameEnd(0);
            hk3ds::textcapture::publish();
        }
    }
    #if HK3DS_DIAGNOSTICS
    hk3ds_diagnostics.status=2;
#endif
    // The final submitted frame still owns immutable vertices and textures.
    C3D_FrameSync();
    // Tutorial's atlas must also leave linear memory when returning to the
    // title; otherwise the next session retains the same Town-entry deficit.
    tutorial.resting.bindStag(nullptr);
    // These local renderers own sprite sheets; HeroRenderer has no implicit release.
    eventRenderer.release();questRenderer.release();enemyCRenderer.release();
    stagRenderer.release();
    corniferRenderer.release();
    doorRenderer.release();
    sceneEnemyGraphics.release();
    greenpathRenderer.release();abilityRenderer.release();inventoryRenderer.release();mapRenderer.release();bossRenderer.release();hudRenderer.release();audio.release();effects.release();lighting.release();roomRenderer.release();furyRenderer.release();geoRenderer.release();shadeRenderer.release();buzzerRenderer.release();enemyRenderer.release(); renderer.release(); if (romfs) romfsExit();
    C2D_Fini(); C3D_Fini(); gfxExit();
#if HK3DS_DIAGNOSTICS
    hk3ds_diagnostics.status=3;
#endif
    consoleTest.finish(metricsOk,stagesOk,frameOk);
    // RPC can disappear before it observes teardown. Explicit replay tests retain
    // the final snapshot only after START, worker flushes and graphics teardown.
    #if HK3DS_DIAGNOSTICS
    if(!consoleTest.active()&&replay.active()&&startExit) {
        if(auto* f=std::fopen("sdmc:/3ds/HK3DS/replay-complete.bin","wb")) {
            // RPC may disappear as soon as main returns. Preserve the same final
            // audio evidence used by the observer, after all workers stop.
            const auto write=[f](const auto& value) {
                unsigned char bytes[sizeof(value)];
                const auto* source=reinterpret_cast<const volatile unsigned char*>(&value);
                for(std::size_t i=0;i<sizeof(bytes);++i)bytes[i]=source[i];
                std::fwrite(bytes,1,sizeof(bytes),f);
            };
            std::fwrite("HKEX0002",1,8,f);
            write(hk3ds_diagnostics);write(hk3ds_enemy_audio);
            write(hk3ds_wind_audio);write(hk3ds_opening_stream);
            std::fclose(f);
        }
    }
    #endif
    if(tutorialReady&&!visualsReady&&!sessionFailed){recordBreadcrumb("startup_visuals","startup_visuals_failed");sessionFailed=true;}
    return sessionFailed?2:returnToTitle?10:0;
}

int main(int argc,char** argv)
{
    mkdir("sdmc:/3ds",0777);mkdir("sdmc:/3ds/HK3DS",0777);
    hk3ds::loadDiagnosticSettings();
    breadcrumb.set(breadcrumb.build,sizeof(breadcrumb.build),__DATE__ " " __TIME__);
    breadcrumb.set(breadcrumb.revision,sizeof(breadcrumb.revision),HK3DS_BUILD_COMMIT);
    breadcrumb.diagnostics=hk3ds::diagnosticsEnabled;
    breadcrumb.set(breadcrumb.scene,sizeof(breadcrumb.scene),"Tutorial_01");
    breadcrumb.set(breadcrumb.gate,sizeof(breadcrumb.gate),"startup");
    alignas(8) static unsigned char exceptionStack[32*1024];
    threadOnException(crashHandler,exceptionStack+sizeof(exceptionStack),WRITE_DATA_TO_HANDLER_STACK);
    std::set_new_handler(allocationFailed);
    recordBreadcrumb("startup");
#if HK3DS_CRASH_TEST
    if(auto* test=std::fopen("sdmc:/3ds/HK3DS/crash-test.flag","rb")){
        const int mode=std::fgetc(test);
        std::fclose(test);
        if(mode=='m'){
            bool new3ds=false;APT_CheckNew3DS(&new3ds);
            hk3ds::selectGraphicsProfile(hk3ds::GraphicsMode::Auto,new3ds,envGetLinearHeapSize()>hk3ds::oldLinearHeapBytes);
            gfxInitDefault();
            recordBreadcrumb("test_memory");
            allocationFailed();
        }
        if(mode=='f'){
            recordBreadcrumb("test_failure","forced_failure");
            showFailure(breadcrumb.reason);
            return 1;
        }
        if(mode=='e'){
            recordBreadcrumb("test_exception","forced_exception");
            volatile std::uintptr_t address=0;
            *reinterpret_cast<volatile u32*>(address)=1;
            recordBreadcrumb("test_exception","exception_not_delivered");
            showFailure(breadcrumb.reason);
            return 1;
        }
    }
#endif
    // Tear down workers/audio/GPU before reopening the title or changing slots.
    // Iteration keeps repeated quit/continue cycles at a constant stack depth.
    int result;
    do {result=runGameSession(argc,argv);}while(result==10);
    if(result==0)recordBreadcrumb("exit","normal_exit");
    else if(!loadFailureAcknowledged)showFailure(breadcrumb.reason);
#if HK3DS_DIAGNOSTICS
    // Azahar's guest-led shutdown frees its renderer before the active dumper.
    // After ALL session destructors and evidence flushes, let the capture runner
    // stop the render window first. Normal play never creates this flag.
    if(result==0)if(auto* flag=std::fopen("sdmc:/3ds/HK3DS/capture-finalize.flag","rb")) {
        std::fclose(flag);
        hk3ds_test_shutdown=0x484b444eu; // HKDN: complete guest teardown.
        // Bound orphaned automation; the runner must acknowledge within 90 s.
        for(unsigned i=0;i<900&&hk3ds_test_shutdown==0x484b444eu;++i)svcSleepThread(100000000);
    }
#endif
    return result;
}
