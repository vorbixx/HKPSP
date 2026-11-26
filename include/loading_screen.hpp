#pragma once
#include <citro2d.h>
#include <3ds.h>
#include <cmath>
#include <cstdint>

namespace hk3ds {
// No atlas, heap allocation or gameplay-frame work; about twenty loading quads.
inline void drawLoadingScreen(C3D_RenderTarget* target)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(target,C2D_Color32(0,0,0,255));
    C2D_SceneBegin(target);
    const float phase=float(osGetTime()%1200)/1200.f*6.2831853f;
    const float x=370.f,y=214.f+std::sin(phase)*1.5f;
    const auto white=C2D_Color32(225,225,230,255),black=C2D_Color32(0,0,0,255);
    C2D_DrawEllipseSolid(x-6,y-5,0,12,11,white);
    C2D_DrawTriangle(x-5,y-2,white,x-8,y-15,white,x-2,y-7,white,0);
    C2D_DrawTriangle(x+5,y-2,white,x+8,y-15,white,x+2,y-7,white,0);
    C2D_DrawEllipseSolid(x-4,y-1,0,2.5f,4,black);
    C2D_DrawEllipseSolid(x+1.5f,y-1,0,2.5f,4,black);
    C2D_DrawTriangle(x,y+5,white,x-5,y+13,white,x+5,y+13,white,0);
    for(unsigned i=0;i<8;++i){
        const float a=phase+float(i)*.7853982f;
        const auto color=C2D_Color32(190,190,200,u8(32+i*25));
        C2D_DrawEllipseSolid(x+std::cos(a)*13-1,y+std::sin(a)*13-1,0,2,2,color);
    }
    C3D_FrameEnd(0);
    // Loading can immediately release textures referenced by the previous frame.
    C3D_FrameSync();
}
inline C3D_RenderTarget* loadingTarget=nullptr;
inline std::uint64_t loadingLastFrame=0;
inline void pulseLoadingScreen()
{
    if(!loadingTarget)return;
    const auto now=osGetTime();
    if(loadingTarget&&now-loadingLastFrame>=50){
        drawLoadingScreen(loadingTarget);loadingLastFrame=now;
    }
}
// Used only on the render thread, outside any open GPU frame. Nested room
// loads during startup restore the boot screen's registration on return.
class LoadingScreen {
    C3D_RenderTarget* previous_=nullptr;
    bool active_=false;
public:
    explicit LoadingScreen(C3D_RenderTarget* target){begin(target);}
    void begin(C3D_RenderTarget* target){
        previous_=loadingTarget;active_=true;loadingTarget=target;
        drawLoadingScreen(target);loadingLastFrame=osGetTime();
    }
    void finish(){if(active_){loadingTarget=previous_;active_=false;}}
    ~LoadingScreen(){finish();}
    LoadingScreen(const LoadingScreen&)=delete;
    LoadingScreen& operator=(const LoadingScreen&)=delete;
};
}
