#pragma once
#include "room.hpp"
#ifdef __3DS__
#include <citro2d.h>
#endif
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
// Fixed storage survives scene changes and remains available in Release.
struct DiagnosticOverlay {
    bool enabled=false, armed=false, latched=false;
    double confirmationUntil=0;
    bool persistenceFailed=false;
    double holdStart=0, previousFrame=0, windowSeconds=0;
    unsigned frames=0;
    float sumMs=0, peakMs=0, averageMs=0, worstMs=0, fps=0;
    unsigned loadedAssets=0, failedAssets=0;
    std::array<std::array<char,56>,3> missing{};
    std::array<std::array<char,56>,4> warnings{};
    std::array<std::array<char,64>,20> lines{};
    char lastFrom[32]="-", lastGate[32]="-", lastTo[32]="-", lastEntry[32]="-";
    unsigned slot=0, flags=0;
    Rect cameraBounds{};
    const RoomConfig* rules=nullptr;
    static void copy(char* out,std::size_t capacity,const char* value) {
        if(!capacity)return;
        const char* source=value&&*value?value:"-";
        const auto count=std::min(std::strlen(source),capacity-1);
        std::memcpy(out,source,count);out[count]=0;
    }
    // Monotonic wall time: a long hold toggles once, release is required to rearm.
    bool chord(bool held,double now) {
        if(!held){armed=latched=false;return false;}
        if(!armed){armed=true;holdStart=now;}
        if(latched||now-holdStart<1.0)return false;
        latched=true;enabled=!enabled;previousFrame=0;
        windowSeconds=0;sumMs=peakMs=0;frames=0;
        return true;
    }
    void frame(double now) {
        if(previousFrame){
            const float ms=static_cast<float>((now-previousFrame)*1000);
            sumMs+=ms;peakMs=std::max(peakMs,ms);++frames;windowSeconds+=now-previousFrame;
            if(windowSeconds>=1){averageMs=sumMs/frames;worstMs=peakMs;fps=1000/averageMs;
                frames=0;sumMs=peakMs=0;windowSeconds=0;}
        }
        previousFrame=now;
    }
    void assetFailure(const char* name) {
        ++failedAssets;missing[0]=missing[1];missing[1]=missing[2];
        copy(missing[2].data(),missing[2].size(),name);
    }
    void warning(const char* line) {
        warnings[0]=warnings[1];warnings[1]=warnings[2];warnings[2]=warnings[3];
        copy(warnings[3].data(),warnings[3].size(),line);
        for(char& c:warnings[3])if(c=='\n'||c=='\r')c=' ';
    }
    void transition(const char* from,const char* gate,const char* to,const char* entry) {
        copy(lastFrom,sizeof(lastFrom),from);copy(lastGate,sizeof(lastGate),gate);
        copy(lastTo,sizeof(lastTo),to);copy(lastEntry,sizeof(lastEntry),entry);
    }
    void fill(const char* build,const char* date,const char* scene,Vec2 position,
              const RoomConfig& config,unsigned saveSlot,unsigned progression,
              std::size_t linear,std::size_t vram,std::size_t heap,unsigned abilities) {
        slot=saveSlot;flags=progression;rules=&config;cameraBounds=config.cameraLimits;
        auto line=[&](unsigned i,const char* fmt,auto... args){
            std::snprintf(lines[i].data(),lines[i].size(),fmt,args...);
        };
        line(0,"TEST %s %s",build,date);
        line(1,"SCENE %s",scene);
        line(2,"XY %.2f %.2f TILE %d %d",position.x,position.y,
             static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)));
        const RoomGate* nearest=nullptr;float distance=INFINITY;unsigned index=0,nearestIndex=0;
        for(const auto& gate:config.gates){
            const auto& b=gate.trigger;
            const float dx=std::max({b.left-position.x,0.f,position.x-b.right});
            const float dy=std::max({b.bottom-position.y,0.f,position.y-b.top});
            if(dx*dx+dy*dy<distance){distance=dx*dx+dy*dy;nearest=&gate;nearestIndex=index;}
            ++index;
        }
        if(nearest){line(3,"GATE #%u %s",nearestIndex,nearest->name);
            line(4,"TO %.28s:%.28s",nearest->scene,nearest->entry);}
        else {line(3,"GATE none");line(4,"TO none");}
        line(5,"LAST %.28s:%.28s",lastFrom,lastGate);
        if(!std::strcmp(lastTo,"-"))line(6,"TO none");
        else line(6,"TO %.28s:%.28s",lastTo,lastEntry);
        line(7,"MS avg %.1f max %.1f FPS %.1f",averageMs,worstMs,fps);
        line(8,"FREE KiB L%u V%u H%u",unsigned(linear/1024),unsigned(vram/1024),unsigned(heap/1024));
        line(9,"ASSET sheets %u FAIL %u",loadedAssets,failedAssets);
        for(unsigned i=0;i<3;++i)line(10+i,"F%u %.52s",i+1,missing[i][0]?missing[i].data():"-");
        for(unsigned i=0;i<4;++i)line(13+i,"W%u %.52s",i+1,warnings[i][0]?warnings[i].data():"-");
        line(17,"SAVE slot %u FLAGS %08X",slot+1,flags);
        line(18,"ABILITY D/W/J/S/SD/V/A/SS %02X",abilities);
        line(19,"SPELL %u CLOAK %u GRUZ %u FK %u",unsigned(bool(flags&131072)),
             unsigned(bool(flags&262144)),unsigned(bool(flags&8192)),unsigned(bool(flags&16384)));
    }
};
// Every input loop (title, gameplay, menus, inventory) samples the chord through this one
// function, so no screen can silently ignore it. Returns true once per completed hold.
inline bool diagnosticChordPoll(DiagnosticOverlay& overlay,bool held,double now) {
    return (held||overlay.armed)&&overlay.chord(held,now);
}
#ifdef __3DS__
extern DiagnosticOverlay consoleOverlay;
void diagnosticLog(const char* format,...);
void drawDiagnosticOverlay(C3D_RenderTarget* top,C3D_RenderTarget* bottom,
                           const World&,Rect hero,float cameraX,float cameraY,float ppu,
                           const Rect* actors,std::size_t actorCount);
void releaseDiagnosticOverlay();
void loadDiagnosticSettings();
void toggleDiagnosticLogging(double now);
void drawDiagnosticConfirmation(C3D_RenderTarget* top,double now);
#endif
}
