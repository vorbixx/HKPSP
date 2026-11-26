#pragma once
#include "input.hpp"
#include "psp_audio.hpp"
#include "ui_input.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hkpsp {
struct DrawList;
struct Texture;
// Capture-only fixed pose: no route input; settle scene timers before capture.
struct RenderPose { float heroX=0,heroY=0,cameraX=0,cameraY=0; };
// Position map from docs/psp/controls.md; Select/Start are UI actions outside Input.
enum class Button { Cross, Square, Circle, Triangle, R, L, SelectTap, SelectHold, Start };
struct ButtonBinding { Button button; const char* switchButton; const char* action; std::uint16_t simulation; };
inline constexpr std::array<ButtonBinding,9> buttonBindings{{
    {Button::Cross,"B (bottom)","Jump",hk3ds::Jump}, {Button::Square,"Y (left)","Attack",hk3ds::Attack},
    {Button::Circle,"A (right)","Focus / Cast",hk3ds::Focus}, {Button::Triangle,"X (top)","Dream Nail",hk3ds::Dream},
    {Button::R,"ZR","Dash",hk3ds::Dash}, {Button::L,"ZL","Super Dash",hk3ds::SuperDash},
    {Button::SelectTap,"Minus","Inventory",0}, {Button::SelectHold,"L","Quick Map",0}, {Button::Start,"Plus","Pause",0}
}};
// Quick Cast (Switch R) has no dedicated binding; Circle tap casts through Focus.
// Movement uses nub/d-pad x/y; d-pad down also sets hk3ds::Down.
class Platform {
public:
    virtual ~Platform()=default;
    virtual hk3ds::Input poll()=0;
    // Presentation input remains available while simulation is frozen.
    // Deterministic gameplay captures retain their existing input path.
    virtual bool menusEnabled()const{return false;}
    virtual UiInput pollUi(){return {};}
    // HKIR HID records sampled at 30 Hz presentation time, including paused frames.
    virtual std::string uiReplay()const{return {};}
    virtual std::string path(const std::string& name) const=0;
    // Shared scene loaders use original relative metadata names under this root.
    virtual std::string sceneRoot()const{return {};}
    virtual std::string sceneEnemyRoot()const{return sceneRoot();}
    virtual std::string inputReplay()const{return {};}
    // Empty for deterministic capture/performance runs; ordinary play persists.
    virtual std::string savePath()const{return {};}
    // Diagnostic boundary includes transition allocations in the destination visit.
    virtual void beginSceneLoad(const char* /*scene*/){}
    // Called after the preceding frame has completed and before replacing banks.
    virtual void releaseRoomAssets(unsigned /*retainedPages*/){}
    virtual void preloadRoomPages(const std::vector<Texture>&,const char*,bool){}
    virtual void releaseTexture(unsigned /*page*/){}
    virtual double seconds() const=0;
    virtual std::string audioOutput()const{return {};}
    // Memory-only replays keep the shipped audio queues and omit log buffers.
    virtual bool audioDiagnostics()const{return true;}
    virtual const RenderPose* renderPose()const{return nullptr;}
    virtual void beginFrame()=0;
    virtual void submit(const DrawList& list)=0;
    virtual bool presentMonomonMovie(){return true;}
    virtual void endFrame()=0;
    virtual bool roomGeEnabled()const{return false;}
};
struct HostOptions {
    std::string assets,source,output,replay,data,save,uiReplay;
    unsigned frames=300,every=1;
    bool realtime=true,raster=true,capturesOnly=false;
    bool fixedPose=false;
    bool roomGe=false;
    bool menus=false;
    RenderPose pose;
};
std::unique_ptr<Platform> makeHost(const HostOptions& options);
struct GameRunResult { unsigned frames=0,controlSteps=0; };
GameRunResult runGame(Platform& platform,unsigned frames);
void checkDrawList();
}
