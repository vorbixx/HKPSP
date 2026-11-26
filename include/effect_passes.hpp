#pragma once
#include <citro3d.h>
#include <algorithm>
#include <array>
#include <cmath>
#include "camera_color.hpp"
#include "camera_bloom.hpp"
#include "light_sampling.hpp"
namespace hk3ds::effects
{
// Source shader contract; offsets are SOURCE texels, including its asymmetry.
constexpr float offsets[]={0,1.384615f,-.615385f,3.230769f,-2.769231f};
constexpr float weights[]={.227027f,.316261f,.316261f,.070270f,.070270f};
// A completed camera image is copied, not composited as a translucent sprite.
// Intermediate light passes may leave alpha below one; RGB is already final.
inline void cameraCopyBlend()
{
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
}
// Straight-alpha sprites must preserve destination RGB and alpha at zero alpha.
inline void spriteBlend()
{
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
}
// For a room Screen sprite, its per-object grab G is the current destination D.
// Screen(D,S)*a + D*(1-a) = D + (S*a)*(1-D). Premultiply after C2D's tint
// stages and let the blend unit read D, avoiding a GX split/copy/clear per sprite.
// C2D uses tint stages 0..3 and fade stage 5. Restore stage 4 before ordinary sprites.
inline void screenSprite(bool enabled)
{
    auto* e=C3D_GetTexEnv(4);C3D_TexEnvInit(e);
    C3D_AlphaTest(enabled,GPU_GEQUAL,3);
    if(enabled){
        C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_PREVIOUS);
        C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE_MINUS_DST_COLOR,GPU_ONE,
                       GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);
    }else{
        spriteBlend();
    }
}
// Sprites/Default Mix Channels: Blend One OneMinusSrcColor (Unity factor 6).
// _JustAlphaLerp=0 outputs premultiplied RGB and alpha ONE. Black cutouts
// preserve scene RGB even when a preceding light pass leaves fractional alpha.
inline void darknessCutoutSprite()
{
    auto* e=C3D_GetTexEnv(4);C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_PREVIOUS);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);
    C3D_TexEnvColor(e,0xffffffff);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_CONSTANT);
    C3D_TexEnvFunc(e,C3D_Alpha,GPU_REPLACE);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ONE_MINUS_SRC_COLOR,
                   GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
}
// Legacy Particles/Additive: Blend SrcAlpha One, ColorMask RGB. Keep camera
// alpha intact for subsequent blur/bloom composition.
inline void additiveSprite()
{
    C3D_TexEnvInit(C3D_GetTexEnv(4));
    C3D_AlphaTest(true,GPU_GEQUAL,3);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE,GPU_ZERO,GPU_ONE);
}
// shader462: saturate(2 * vertex.rgba * texture.rgba) for a UNORM target.
// Scale after C2D texture/tint modulation so translucent texels keep their weight.
inline void ambientSprite()
{
    additiveSprite();
    auto* e=C3D_GetTexEnv(4);
    C3D_TexEnvSrc(e,C3D_Both,GPU_PREVIOUS);
    C3D_TexEnvFunc(e,C3D_Both,GPU_REPLACE);
    C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_2);
    C3D_TexEnvScale(e,C3D_Alpha,GPU_TEVSCALE_2);
}
// Recovered transformed banks already carry the source alpha gain in vertices.
inline void deepnestAmbientSprite()
{
    ambientSprite();C3D_TexEnvScale(C3D_GetTexEnv(4),C3D_Alpha,GPU_TEVSCALE_1);
}
// Legacy Particles/Additive (Soft): premultiplied vertex*texture, Blend One
// OneMinusSrcColor, ColorMask RGB. The equivalent Screen blend reuses stage 4.
inline void softParticleSprite()
{
    screenSprite(true);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE_MINUS_DST_COLOR,GPU_ONE,GPU_ZERO,GPU_ONE);
}
inline void reset()
{
    for(int i=0;i<6;++i)C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(0,0);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
}
inline void replace()
{
    reset();C3D_TexEnvSrc(C3D_GetTexEnv(0),C3D_Both,GPU_TEXTURE0);
}
// Independent UVs let a moving liquid sample a fixed silhouette. Mask alpha
// multiplies source alpha before blending; it never paints the world black.
inline void alphaMaskedSprite(C3D_Tex& source,C3D_Tex& mask,float tone)
{
    reset();spriteBlend();
    // No C3D_TexBind(2,nullptr): citro3d 1.7.1 reads tex->fmt for units 1-2 and null crashed the console
    // (data abort, FAR 0xC). The env below never samples unit 2, so a stale binding there is harmless.
    C3D_TexBind(0,&source);C3D_TexBind(1,&mask);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE0,GPU_TEXTURE1);
    C3D_TexEnvFunc(e,C3D_Both,GPU_MODULATE);
    const auto value=static_cast<u32>(std::clamp(tone,0.f,1.f)*255);
    C3D_TexEnvColor(e,0xff000000u|value|(value<<8)|(value<<16));
}
// Split at every sampler clamp transition. Clamping only the outside vertices
// stretches the interior; texture wrap alone samples power-of-two padding.
inline void quad(unsigned w,unsigned h,unsigned tw,unsigned th,int axis=-1,unsigned first=0,
                 const float* sampleOffsets=offsets,float spacing=1,bool twoBlurSamplers=false)
{
    const float extent=axis==0?w:h;
    std::array<float,8> cuts{};unsigned count=2;cuts[1]=extent;
    if(axis>=0)for(unsigned k=first;k<std::min(first+3,5u);++k){
        const float d=(axis==0?sampleOffsets[k]:-sampleOffsets[k])*spacing;
        for(float boundary:{.5f,extent-.5f}){
            const float p=boundary-d;if(p>0&&p<extent)cuts[count++]=p;
        }
    }
    for(unsigned i=1;i<count;++i){
        const float value=cuts[i];unsigned j=i;
        while(j&&cuts[j-1]>value){cuts[j]=cuts[j-1];--j;}
        cuts[j]=value;
    }
    static constexpr unsigned corner[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    // ImmDrawBegin uploads cached bindings. The second blur draw references
    // only units 0/1; restore the three bound units before returning to C3D.
    if(twoBlurSamplers)GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG,1,3);
    for(unsigned strip=1;strip<count;++strip)for(const auto& c:corner){
        const float x=axis==0?cuts[strip-1+c[0]]:c[0]*w;
        const float y=axis==1?cuts[strip-1+c[1]]:c[1]*h;
        C3D_ImmSendAttrib(2*x/w-1,2*y/h-1,-.5f,1);
        for(unsigned k=0;k<3;++k){
            const float d=axis>=0&&first+k<5?sampleOffsets[first+k]*spacing:0;
            C3D_ImmSendAttrib((axis==0?std::clamp(x+d,.5f,w-.5f):x)/tw,
                              (axis==1?std::clamp(y-d,.5f,h-.5f):y)/th,0,1);
        }
    }
    C3D_ImmDrawEnd();
    if(twoBlurSamplers)GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG,1,7);
}
// Composition quad for a full-resolution grab and a separately rasterized
// light source. The light coordinates are scaled in scene space, then clamped
// to its valid texels so bilinear filtering never reaches power-of-two padding.
inline void quadDual(unsigned w,unsigned h,unsigned grabW,unsigned grabH,
                     unsigned lightW,unsigned lightH,unsigned lightTexW,unsigned lightTexH)
{
    static constexpr unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    const auto xs=lightSampleAxis(w,lightW,lightTexW),ys=lightSampleAxis(h,lightH,lightTexH);
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)
    for(const auto& c:corners){
        const unsigned ix=col+c[0],iy=row+c[1];
        const float x=xs.scene[ix],y=ys.scene[iy],u=xs.uv[ix],v=ys.uv[iy];
        C3D_ImmSendAttrib(2*x/w-1,2*y/h-1,-.5f,1);
        C3D_ImmSendAttrib(x/grabW,y/grabH,0,1);
        C3D_ImmSendAttrib(u,v,0,1);
        C3D_ImmSendAttrib(u,v,0,1);
    }
    C3D_ImmDrawEnd();
}
inline void bloomExtract(C3D_Tex& source,C3D_RenderTarget* target,unsigned w,unsigned h)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,cameraBloom::width,cameraBloom::height);
    reset();C3D_TexBind(0,&source);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);C3D_TexEnvColor(e,cameraBloom::threshold);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_CONSTANT);
    e=C3D_GetTexEnv(1);C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);C3D_TexEnvColor(e,cameraBloom::intensity);
    // Approximation: one bilinear downsample replaces the original four-tap
    // average. Threshold remains after sampling, never before downsampling.
    quad(w,h,source.width,source.height);
}
inline void bloomBlur(C3D_Tex& source,C3D_RenderTarget* target,unsigned axis,float spacing)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,cameraBloom::width,cameraBloom::height);
    for(int unit=0;unit<3;++unit)C3D_TexBind(unit,&source);
    for(unsigned first:{0u,3u}){
        reset();
        for(unsigned k=0;k<(first==0?3u:2u);++k){
            auto* e=C3D_GetTexEnv(k);
            C3D_TexEnvSrc(e,C3D_RGB,static_cast<GPU_TEVSRC>(GPU_TEXTURE0+k),GPU_CONSTANT,GPU_PREVIOUS);
            C3D_TexEnvFunc(e,C3D_RGB,k?GPU_MULTIPLY_ADD:GPU_MODULATE);
            const unsigned q=cameraBloom::weights[first+k];
            C3D_TexEnvColor(e,0xff000000u|q|(q<<8)|(q<<16));
            C3D_TexEnvSrc(e,C3D_Alpha,GPU_CONSTANT);
        }
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,first?GPU_ONE:GPU_ZERO,GPU_ONE,GPU_ZERO);
        quad(cameraBloom::width,cameraBloom::height,source.width,source.height,int(axis),first,
             cameraBloom::offsets,spacing);
    }
}
inline void bloomAdd(C3D_Tex& bloom,C3D_RenderTarget* target,unsigned w,unsigned h)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,w,h);replace();C3D_TexBind(0,&bloom);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ONE,GPU_ZERO,GPU_ONE);
    // Clamp the active image, including its border strips, rather than sampling
    // uninitialized power-of-two padding when magnifying the quarter-size image.
    const float xs[]={0,.5f,cameraBloom::width-.5f,float(cameraBloom::width)};
    const float ys[]={0,.5f,cameraBloom::height-.5f,float(cameraBloom::height)};
    static constexpr unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)for(const auto& c:corners){
        const float x=xs[col+c[0]],y=ys[row+c[1]];
        C3D_ImmSendAttrib(2*x/cameraBloom::width-1,2*y/cameraBloom::height-1,-.5f,1);
        const float u=std::clamp(x,.5f,cameraBloom::width-.5f)/bloom.width;
        const float v=std::clamp(y,.5f,cameraBloom::height-.5f)/bloom.height;
        for(int unit=0;unit<3;++unit)C3D_ImmSendAttrib(u,v,0,1);
    }
    C3D_ImmDrawEnd();
}
enum class BlurTargetInit { Clear, Overwrite };
// Overwrite passed the device A/B with poisoned targets (see docs/progress.md).
// Both paths cover the entire active viewport; padding is never a valid blur
// input. Keep Clear as the reference default for the original probe.
inline void blur(C3D_Tex& source,C3D_RenderTarget* target,unsigned w,unsigned h,unsigned axis,
                 BlurTargetInit init=BlurTargetInit::Clear,float spacing=1)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,w,h);
    if(init==BlurTargetInit::Clear)C3D_RenderTargetClear(target,C3D_CLEAR_COLOR,0,0);
    for(int unit=0;unit<3;++unit)C3D_TexBind(unit,&source);
    for(unsigned first:{0u,3u}){
        reset();
        for(unsigned k=0;k<(first==0?3u:2u);++k){
            auto* e=C3D_GetTexEnv(k);
            C3D_TexEnvSrc(e,C3D_RGB,static_cast<GPU_TEVSRC>(GPU_TEXTURE0+k),GPU_CONSTANT,GPU_PREVIOUS);
            C3D_TexEnvFunc(e,C3D_RGB,k?GPU_MULTIPLY_ADD:GPU_MODULATE);
            const unsigned q=std::lround(weights[first+k]*255);
            C3D_TexEnvColor(e,0xff000000u|q|(q<<8)|(q<<16));
            C3D_TexEnvSrc(e,C3D_Alpha,GPU_CONSTANT);
        }
        const auto destination=init==BlurTargetInit::Overwrite&&first==0?GPU_ZERO:GPU_ONE;
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,destination,GPU_ONE,GPU_ZERO);
        quad(w,h,source.width,source.height,int(axis),first,offsets,spacing,first==3);
    }
}
inline void vibrancy(C3D_Tex& source,C3D_RenderTarget* target,unsigned w,unsigned h,unsigned outW,unsigned outH)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,outW,outH);replace();
    C3D_TexBind(0,&source);auto* e=C3D_GetTexEnv(0);
    // One multiply-add avoids rounding 0.6*color before doubling it. The
    // 64x64 PICA fixture covers all byte codes and guards exact 1.2*color.
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_TEXTURE0);
    C3D_TexEnvColor(e,0xff333333);C3D_TexEnvFunc(e,C3D_RGB,GPU_MULTIPLY_ADD);
    quad(w,h,source.width,source.height);
}
// Apply after all room/light/decor composition. Baking this into individual
// sprites would change alpha blends and omit procedural effects.
// Screen Flash460 uses UI/BlendModes/Font/Overlay, tinted (0,.7,1).
// H=max(2D-1,0), M=min(D,1-D): Overlay(D,S)=H+2*M*S.
// This preserves white highlights instead of covering the room with cyan.
inline void scuttlerFlash(C3D_Tex& source,C3D_RenderTarget* target,unsigned w,unsigned h,float alpha)
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,w,h);reset();C3D_TexBind(0,&source);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_TEXTURE0);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE0);
    C3D_TexEnvBufUpdate(C3D_RGB,1u<<0); // Retain stage 0's H for stage 3.
    e=C3D_GetTexEnv(1);C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_PREVIOUS);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);
    e=C3D_GetTexEnv(2);C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT);
    C3D_TexEnvColor(e,0xffffb200u);C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);
    C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_2);
    e=C3D_GetTexEnv(3);C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_ADD);
    e=C3D_GetTexEnv(4);C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_TEXTURE0,GPU_CONSTANT);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvColor(e,0x00ffffffu|(u32(std::clamp(alpha,0.f,1.f)*255)<<24));C3D_TexEnvFunc(e,C3D_RGB,GPU_INTERPOLATE);
    quad(w,h,source.width,source.height);
}
inline void cameraColor(C3D_Tex& source,C3D_RenderTarget* target,unsigned w,unsigned h,
                        const hk3ds::cameraColor::Config& config=hk3ds::cameraColor::Config())
{
    C3D_FrameDrawOn(target);C3D_SetViewport(0,0,w,h);reset();
    C3D_TexBind(0,&source);
    auto* e=C3D_GetTexEnv(0);
    // Seed PREVIOUS explicitly: the combiner buffer is not available to the
    // first stage. Losing curveB here removes mostly green from the scene.
    C3D_TexEnvSrc(e,C3D_RGB,GPU_CONSTANT);
    C3D_TexEnvColor(e,config.b);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE0);
    e=C3D_GetTexEnv(1);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT,GPU_TEXTURE0);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_INTERPOLATE);
    C3D_TexEnvColor(e,config.a);
    C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_2);
    e=C3D_GetTexEnv(2);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_CONSTANT,GPU_PREVIOUS,GPU_TEXTURE0);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_INTERPOLATE);
    C3D_TexEnvColor(e,config.c);
    e=C3D_GetTexEnv(3);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT,GPU_TEXTURE0);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_INTERPOLATE);
    C3D_TexEnvColor(e,hk3ds::cameraColor::half);
    e=C3D_GetTexEnv(4);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_TEXTURE0);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);
    C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_2);
    e=C3D_GetTexEnv(5);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS,GPU_PREVIOUS,GPU_CONSTANT);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_G,GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_INTERPOLATE);
    C3D_TexEnvColor(e,config.sat);
    // Approximation: green stands in for luminance at saturation .92, keeping
    // the curve and desaturation in one pass. On the saved original lit pixels,
    // the complete approximation averages <=1.04 byte/channel error.
    quad(w,h,source.width,source.height);
}
// Configure the signed source and original alpha cutoff. The isolated precision
// probe also uses this state before testing a different composition step.
inline void linearSource(C3D_Tex& grab,C3D_Tex& light)
{
    reset();C3D_TexBind(0,&grab);C3D_TexBind(1,&light);
    // Positive and negative parts cannot both be nonzero. Subtracting the
    // negative part before adding the positive part preserves final saturation
    // without the half-range scheme's loss of one bit of the grab color.
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE1,GPU_TEXTURE1);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE1);
    e=C3D_GetTexEnv(1);C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_PREVIOUS);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);
    C3D_TexEnvBufUpdate(C3D_RGB,1u<<1);
    e=C3D_GetTexEnv(2);C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE1,GPU_TEXTURE1);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_SUBTRACT);
    e=C3D_GetTexEnv(3);C3D_TexEnvSrc(e,C3D_RGB,GPU_PREVIOUS_BUFFER,GPU_PREVIOUS);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_ADD);
    C3D_AlphaTest(true,GPU_GEQUAL,3);
}
// Hero donut's VividLight upper branch. Converter stores gain/4 and original
// alpha; PICA clamps G*gain before source-alpha blending, as the UNORM source.
inline void vivid(C3D_Tex& grab,C3D_Tex& gain,unsigned w,unsigned h,unsigned lightW=0,unsigned lightH=0)
{
    reset();C3D_TexBind(0,&grab);C3D_TexBind(1,&gain);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_TEXTURE1);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_MODULATE);
    C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_4);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE1);
    C3D_AlphaTest(true,GPU_GEQUAL,3);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);
    if(lightW)quadDual(w,h,grab.width,grab.height,lightW,lightH,gain.width,gain.height);
    else quad(w,h,grab.width,grab.height);
}
inline void linear(C3D_Tex& grab,C3D_Tex& light,unsigned w,unsigned h,unsigned lightW=0,unsigned lightH=0)
{
    linearSource(grab,light);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);
    if(lightW)quadDual(w,h,grab.width,grab.height,lightW,lightH,light.width,light.height);
    else quad(w,h,grab.width,grab.height);
}
// UI/BlendModes/LinearDodge (shader557): the UNORM source clamps G + S
// BEFORE SrcAlpha/OneMinusSrcAlpha blending. Particles/Additive differs at
// translucent bright texels, which then feed the camera bloom threshold.
inline void linearDodge(C3D_Tex& grab,C3D_Tex& source,unsigned w,unsigned h,unsigned lightW=0,unsigned lightH=0)
{
    reset();C3D_TexBind(0,&grab);C3D_TexBind(1,&source);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE0,GPU_TEXTURE1);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_ADD);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE1);
    C3D_AlphaTest(true,GPU_GEQUAL,3);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);
    if(lightW)quadDual(w,h,grab.width,grab.height,lightW,lightH,source.width,source.height);
    else quad(w,h,grab.width,grab.height);
}
// UI/BlendModes/Screen: screen(G,S), then the original alpha blend onto D.
// Reference for independent G/D; room sprites use screenSprite with G == D.
inline void screen(C3D_Tex& grab,C3D_Tex& source,unsigned w,unsigned h,unsigned lightW=0,unsigned lightH=0)
{
    reset();C3D_TexBind(0,&grab);C3D_TexBind(1,&source);
    auto* e=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e,C3D_RGB,GPU_TEXTURE1,GPU_TEXTURE0,GPU_TEXTURE0);
    C3D_TexEnvOpRgb(e,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR,
                   GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e,C3D_RGB,GPU_MULTIPLY_ADD);
    C3D_TexEnvSrc(e,C3D_Alpha,GPU_TEXTURE1);
    C3D_AlphaTest(true,GPU_GEQUAL,3);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);
    if(lightW)quadDual(w,h,grab.width,grab.height,lightW,lightH,source.width,source.height);
    else quad(w,h,grab.width,grab.height);
}
}
