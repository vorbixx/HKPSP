#include "diagnostic_overlay.hpp"
#include "build_config.hpp"
#include "runtime_diagnostics.hpp"
#include "text_capture.hpp"
#include "ui.hpp"
#include <cstdarg>
#include <cctype>
namespace hk3ds {
DiagnosticOverlay consoleOverlay;
namespace {
C2D_TextBuf textBuffer=nullptr;
void text(const char* value,float x,float y) {
    ui::diagnosticText(textBuffer,value,x,y);
}
}
void loadDiagnosticSettings() {
    runtimeDiagnostics.load();
    consoleOverlay.enabled=runtimeDiagnostics.enabled();
}
void toggleDiagnosticLogging(double now) {
    consoleOverlay.persistenceFailed=!runtimeDiagnostics.set(consoleOverlay.enabled);
    consoleOverlay.enabled=runtimeDiagnostics.enabled();
    consoleOverlay.confirmationUntil=now+3;
    if(consoleOverlay.enabled)diagnosticLog("Diagnostics ON\n");
}
void drawDiagnosticConfirmation(C3D_RenderTarget* top,double now) {
    if(!consoleOverlay.confirmationUntil)return;
    if(now>=consoleOverlay.confirmationUntil){consoleOverlay.confirmationUntil=0;return;}
    if(!textBuffer)textBuffer=C2D_TextBufNew(1600);
    C2D_TextBufClear(textBuffer);
    C2D_SceneBegin(top);C2D_ViewReset();textcapture::select(0,true);
    C2D_DrawRectSolid(3,214,0,394,23,C2D_Color32(0,0,0,255));
    const char* message=consoleOverlay.enabled?"Diagnostics ON":"Diagnostics OFF";
    char line[64];std::snprintf(line,sizeof(line),"%s%s",message,
        consoleOverlay.persistenceFailed?" (SD write failed)":"");
    ui::diagnosticText(textBuffer,line,7,217,.55f);
}
void diagnosticLog(const char* format,...) {
    char line[256];va_list args;va_start(args,format);
    std::vsnprintf(line,sizeof(line),format,args);va_end(args);
    for(char* cursor=line;*cursor;){
        char* end=std::strchr(cursor,'\n');if(end)*end=0;
        char lower[256];unsigned i=0;
        for(;cursor[i]&&i<sizeof(lower)-1;++i)lower[i]=std::tolower(static_cast<unsigned char>(cursor[i]));
        lower[i]=0;
        if(std::strstr(lower,"fail")||std::strstr(lower,"missing")||std::strstr(lower,"invalid")||
           std::strstr(lower,"warning")||std::strstr(lower,"error"))consoleOverlay.warning(cursor);
        if(!end)break;
        *end='\n';cursor=end+1;
    }
    if(diagnosticsEnabled)std::fputs(line,stdout);
    if(runtimeDiagnostics.enabled())if(auto* file=std::fopen("sdmc:/3ds/HK3DS/diagnostics.txt","a")){
        std::fputs(line,file);std::fclose(file);
    }
}
void drawDiagnosticOverlay(C3D_RenderTarget* top,C3D_RenderTarget* bottom,
                           const World& world,Rect hero,float cameraX,float cameraY,float ppu,
                           const Rect* actors,std::size_t actorCount) {
    if(!textBuffer)textBuffer=C2D_TextBufNew(1600);
    if(!textBuffer)return;
    C2D_TextBufClear(textBuffer);
    C2D_SceneBegin(top);C2D_ViewReset();textcapture::select(0);
    const auto sx=[&](float x){return 200+(x-cameraX)*ppu;};
    const auto sy=[&](float y){return 120-(y-cameraY)*ppu;};
    const auto box=[&](Rect b,u32 color){
        if(sx(b.right)<0||sx(b.left)>400||sy(b.top)>240||sy(b.bottom)<0)return;
        const float l=sx(b.left),r=sx(b.right),t=sy(b.top),d=sy(b.bottom);
        C2D_DrawLine(l,t,color,r,t,color,1,0);C2D_DrawLine(r,t,color,r,d,color,1,0);
        C2D_DrawLine(r,d,color,l,d,color,1,0);C2D_DrawLine(l,d,color,l,t,color,1,0);
    };
    for(std::size_t i=0;i<world.count;++i)box(world.solids[i].bounds,C2D_Color32(0,255,255,255));
    for(std::size_t i=0;i<world.shapeCount;++i)if(world.shapes[i].actorContact(true))
        box(world.shapes[i].bounds,C2D_Color32(0,255,255,255));
    box(hero,C2D_Color32(255,255,255,255));
    for(std::size_t i=0;i<actorCount;++i)box(actors[i],C2D_Color32(255,130,100,255));
    box(consoleOverlay.cameraBounds,C2D_Color32(255,80,255,255));
    // Viewport edge remains visible when authored camera limits are off screen.
    C2D_DrawRectSolid(0,0,0,400,1,C2D_Color32(255,80,255,255));
    C2D_DrawRectSolid(0,239,0,400,1,C2D_Color32(255,80,255,255));
    C2D_DrawRectSolid(0,0,0,1,240,C2D_Color32(255,80,255,255));
    C2D_DrawRectSolid(399,0,0,1,240,C2D_Color32(255,80,255,255));
    if(consoleOverlay.rules)for(const auto& hazard:consoleOverlay.rules->hazards)
        box(hazard.trigger,C2D_Color32(255,80,80,255));
    if(consoleOverlay.rules)for(const auto& gate:consoleOverlay.rules->gates){
        box(gate.trigger,C2D_Color32(255,255,0,255));
        const auto& b=gate.trigger;
        if(sx(b.right)<0||sx(b.left)>400||sy(b.top)>240||sy(b.bottom)<0)continue;
        const float x=std::clamp(sx(b.left),0.f,220.f),y=std::clamp(sy(b.top),14.f,226.f);
        C2D_DrawRectSolid(x,y,0,180,12,C2D_Color32(0,0,0,255));text(gate.scene,x+1,y);
    }
    C2D_DrawRectSolid(3,3,0,44,24,C2D_Color32(0,0,0,255));
    ui::diagnosticText(textBuffer,"DIAG",6,6,.6f);
    C2D_SceneBegin(bottom);C2D_ViewReset();textcapture::select(1,true);
    C2D_DrawRectSolid(0,0,0,320,240,C2D_Color32(0,0,0,255));
    for(unsigned i=0;i<consoleOverlay.lines.size();++i)text(consoleOverlay.lines[i].data(),3,1+i*11.8f);
}
void releaseDiagnosticOverlay(){if(textBuffer)C2D_TextBufDelete(textBuffer);textBuffer=nullptr;}
}
// Linker wrappers observe all sprite-sheet owners, including failures while hidden.
extern "C" C2D_SpriteSheet __real_C2D_SpriteSheetLoad(const char*);
extern "C" void __real_C2D_SpriteSheetFree(C2D_SpriteSheet);
extern "C" C2D_SpriteSheet __wrap_C2D_SpriteSheetLoad(const char* path){
    const auto sheet=__real_C2D_SpriteSheetLoad(path);
    if(sheet)++hk3ds::consoleOverlay.loadedAssets;else hk3ds::consoleOverlay.assetFailure(path);
    return sheet;
}
extern "C" void __wrap_C2D_SpriteSheetFree(C2D_SpriteSheet sheet){
    if(sheet&&hk3ds::consoleOverlay.loadedAssets)--hk3ds::consoleOverlay.loadedAssets;
    __real_C2D_SpriteSheetFree(sheet);
}
