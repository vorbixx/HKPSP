#pragma once
#include "draw_list.hpp"
#include "camera_color.hpp"

namespace hkpsp {
inline unsigned char lookByte(float value){return static_cast<unsigned char>(std::clamp(std::lround(value*255),0l,255l));}
inline bool cameraGrade(Blend blend){return blend>=Blend::GradeGreen&&blend<=Blend::GradeBlue;}
// Fixed 480x272 grade geometry: UVs and positions are native screen pixels.
// The backend instantiates this once with its GU vertex layout, outside frames.
template<class Vertex> constexpr auto cameraGradeStrips(){
    constexpr unsigned width=16,count=(screenWidth+width-1)/width;
    std::array<Vertex,count*6> vertices{};
    unsigned next=0;
    for(unsigned x=0;x<screenWidth;x+=width){
        const float left=float(x),right=float(std::min(x+width,unsigned(screenWidth)));
        const float bottom=float(screenHeight);
        for(const auto& p:std::array<std::array<float,2>,6>{{
            {{left,0}},{{right,0}},{{right,bottom}},{{left,0}},{{right,bottom}},{{left,bottom}}
        }})vertices[next++]={p[0],p[1],0xffffffff,p[0],p[1],1};
    }
    return vertices;
}
// Same bounded stage order as effect_passes.hpp::cameraColor / compare_camera_color.py.
// Ambient is already authored into scene colours, not a second RGB multiplier.
inline float cameraComponent(unsigned packed,unsigned channel){return float((packed>>(channel*8))&255)/255;}
inline float cameraCurve(unsigned value,unsigned channel,unsigned seedLow,unsigned seedHigh,unsigned bend){
    const auto stage=[](float x){return lookByte(x)/255.f;};
    const float x=value/255.f;
    float curved=stage(2*(cameraComponent(seedHigh,channel)*x+cameraComponent(seedLow,channel)*(1-x)));
    curved=stage(cameraComponent(bend,channel)*x+curved*(1-x));
    curved=stage(curved*(1-x)+cameraComponent(hk3ds::cameraColor::half,channel)*x);
    return stage(2*curved*x);
}
inline bool sameCameraGrade(const hk3ds::cameraColor::Config& lhs,const hk3ds::cameraColor::Config& rhs){
    return lhs.a==rhs.a&&lhs.b==rhs.b&&lhs.c==rhs.c&&lhs.sat==rhs.sat&&lhs.twoPass==rhs.twoPass&&
        (!lhs.twoPass||(lhs.preA==rhs.preA&&lhs.preB==rhs.preB&&lhs.preC==rhs.preC));
}
// One final colour effect, decomposed into three indexed-framebuffer channel
// draws. Green seeds RGB; red and blue add their saturated curve contributions.
// The source snapshot is immutable across all three draws, so alpha/light
// composition precedes the curve, unlike a per-sprite palette grade.
inline Palette cameraPalette(Blend blend,const hk3ds::cameraColor::Config& config={}){
    Palette palette{};
    const unsigned channel=blend==Blend::GradeGreen?1:blend==Blend::GradeRed?0:2;
    for(unsigned i=0;i<256;++i){
        const unsigned input=config.twoPass?lookByte(cameraCurve(i,channel,config.preA,config.preB,config.preC)):i;
        const float value=cameraCurve(input,channel,config.a,config.b,config.c);
        if(channel==1)palette[i]={{lookByte(value*(1-cameraComponent(config.sat,0))),lookByte(value),
                                    lookByte(value*(1-cameraComponent(config.sat,2))),255}};
        else {palette[i][channel]=lookByte(value*cameraComponent(config.sat,channel));palette[i][3]=255;}
    }
    return palette;
}
inline Palette linearLightPalette(const Texture& texture,const Quad& quad){
    const auto& original=texture.paletteFor(quad.clut);
    Palette palette=original;
    for(unsigned i=0;i<256;++i){
        const float alpha=palette[i][3]/255.f*quad.rgba[3];
        for(unsigned c=0;c<3;++c)
            palette[i][c]=lookByte(.5f+(original[i][c]/255.f*quad.rgba[c]-.5f)*alpha);
    }
    return palette;
}
}
