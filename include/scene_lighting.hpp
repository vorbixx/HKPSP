#pragma once
#include "camera_color.hpp"
#include "lighting_math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hk3ds {
struct SceneLightValues {
    int darkness=0;
    float heroAlpha=1;
    std::array<float,3> heroRgb{1,1,1},ambientRatio{1,1,1};
    cameraColor::Config color;
};
// SceneManager ambient is baked into room artwork. GradeMarker changes only
// lit owners; haze and other unlit source materials retain their authored tint.
struct SceneLighting:SceneLightValues {
    bool hasGrade=false;
    std::array<float,5> gradeRegion{}; // world xyz, maximum radius, cutoff radius
    std::array<float,4> ambientBase{},ambientTarget{},heroTarget{}; // rgb + intensity/alpha
    cameraColor::Config gradeColor;
    std::vector<std::uint32_t> litOwners;
    std::vector<DarknessTrigger> darknessTriggers;
    void configureDarkness(LightingConfig& config) const {
        config.selectSceneDarkness(darkness,darknessTriggers);
    }
    SceneLightValues sample(float x,float y,float z=0) const {
        SceneLightValues result=*this;
        if(!hasGrade||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return result;
        const float dx=x-gradeRegion[0],dy=y-gradeRegion[1],dz=z-gradeRegion[2];
        const float near=gradeRegion[3]*gradeRegion[3],far=gradeRegion[4]*gradeRegion[4];
        const float factor=std::clamp(1-(dx*dx+dy*dy+dz*dz-near)/(far-near),0.f,1.f);
        const auto mix=[&](float a,float b){return a+(b-a)*factor;};
        result.heroAlpha=mix(heroAlpha,heroTarget[3]);
        const float intensity=1+(mix(ambientBase[3],ambientTarget[3])-1)*.5f;
        const float baseIntensity=1+(ambientBase[3]-1)*.5f;
        for(unsigned i=0;i<3;++i){
            result.heroRgb[i]=mix(heroRgb[i],heroTarget[i]);
            result.ambientRatio[i]=mix(ambientBase[i],ambientTarget[i])*intensity/(ambientBase[i]*baseIntensity);
        }
        // Approximation: interpolate fitted TEV constants through the radial
        // transition, rather than rebuilding Unity's paired curve keyframes.
        const auto packed=[&](std::uint32_t a,std::uint32_t b){
            std::uint32_t value=0xff000000;
            for(unsigned shift=0;shift<24;shift+=8)
                value|=std::uint32_t(mix(float((a>>shift)&255),float((b>>shift)&255))+.5f)<<shift;
            return value;
        };
        result.color={packed(color.a,gradeColor.a),packed(color.b,gradeColor.b),
                      packed(color.c,gradeColor.c),packed(color.sat,gradeColor.sat)};
        if(color.twoPass){
            // Grade curves are single-pass fits; fade the preliminary pass to identity.
            result.color.preA=packed(color.preA,0xff404040);
            result.color.preB=packed(color.preB,0xff404040);
            result.color.preC=packed(color.preC,0xff808080);
            result.color.twoPass=true;
        }
        return result;
    }
    bool loadCurrent(const char* path) {
        auto* file=std::fopen(path,"rb");if(!file)return false;
        SceneLighting next;char magic[4]{};
        bool ok=std::fread(magic,4,1,file)==1&&(!std::memcmp(magic,"HLS1",4)||!std::memcmp(magic,"HLS2",4)||!std::memcmp(magic,"HLS3",4)||!std::memcmp(magic,"HLS4",4)||!std::memcmp(magic,"HLS5",4)||!std::memcmp(magic,"HLS6",4)||!std::memcmp(magic,"HLS7",4))&&
            std::fread(&next.darkness,4,1,file)==1&&std::fread(&next.heroAlpha,4,1,file)==1&&
            std::fread(&next.color.a,4,1,file)==1&&std::fread(&next.color.b,4,1,file)==1&&
            std::fread(&next.color.c,4,1,file)==1&&std::fread(&next.color.sat,4,1,file)==1;
        if(ok&&std::memcmp(magic,"HLS1",4))ok=std::fread(next.heroRgb.data(),4,3,file)==3;
        // Both historical HLS3 layouts exist: two-pass has exactly 52 bytes.
        const auto position=std::ftell(file);
        const bool sized=position>=0&&std::fseek(file,0,SEEK_END)==0;
        const auto length=sized?std::ftell(file):-1;
        ok=ok&&sized&&length>=0&&std::fseek(file,position,SEEK_SET)==0;
        const bool legacyTwoPass=!std::memcmp(magic,"HLS3",4)&&length==52;
        // HLS7 separates grade/prepass flags and stores authored darkness levels.
        // The two historical HLS4 writers collided; a bounded size probe retains
        // the old regional prepass without treating its colors as grade floats.
        const bool unified=!std::memcmp(magic,"HLS7",4);
        const bool hls4=!std::memcmp(magic,"HLS4",4);
        unsigned flags=0;
        const bool extended=!std::memcmp(magic,"HLS6",4);
        const bool polygons=!std::memcmp(magic,"HLS5",4)||extended||unified;
        const bool regions=hls4||polygons;
        std::uint32_t grade=!std::memcmp(magic,"HLS3",4)&&!legacyTwoPass;
        if(ok&&regions)ok=std::fread(&flags,4,1,file)==1&&flags<=(unified?3u:1u);
        bool regionalPrepass=false;
        if(ok&&hls4&&flags==1){
            const auto saved=std::ftell(file);unsigned count=7;
            if(std::fseek(file,56,SEEK_SET)==0&&std::fread(&count,4,1,file)==1)
                regionalPrepass=count<=LightingConfig{}.triggers.size()&&length==60+20*long(count);
            ok=std::fseek(file,saved,SEEK_SET)==0;
        }
        if(regions)grade=regionalPrepass?0:flags&1u;
        if(ok&&regionalPrepass){
            ok=std::fread(&next.color.preA,4,1,file)==1&&std::fread(&next.color.preB,4,1,file)==1&&
                std::fread(&next.color.preC,4,1,file)==1;
            next.color.twoPass=true;
        }
        if(ok&&grade){
            next.hasGrade=true;std::uint32_t count=0;
            ok=std::fread(next.gradeRegion.data(),4,5,file)==5&&
                std::fread(next.ambientBase.data(),4,4,file)==4&&std::fread(next.ambientTarget.data(),4,4,file)==4&&
                std::fread(next.heroTarget.data(),4,4,file)==4&&
                std::fread(&next.gradeColor.a,4,1,file)==1&&std::fread(&next.gradeColor.b,4,1,file)==1&&
                std::fread(&next.gradeColor.c,4,1,file)==1&&std::fread(&next.gradeColor.sat,4,1,file)==1&&
                std::fread(&count,4,1,file)==1&&count<=65536;
            const auto finite=[](const auto& values){return std::all_of(values.begin(),values.end(),[](float v){return std::isfinite(v);});};
            ok=ok&&finite(next.gradeRegion)&&finite(next.ambientBase)&&finite(next.ambientTarget)&&finite(next.heroTarget)&&
                next.gradeRegion[3]>=0&&next.gradeRegion[4]>next.gradeRegion[3]&&
                next.ambientBase[3]>=0&&next.ambientTarget[3]>=0&&next.heroTarget[3]>=0&&next.heroTarget[3]<=1;
            const float nearSquared=next.gradeRegion[3]*next.gradeRegion[3];
            const float farSquared=next.gradeRegion[4]*next.gradeRegion[4];
            ok=ok&&std::isfinite(farSquared)&&farSquared>nearSquared;
            const float baseIntensity=1+(next.ambientBase[3]-1)*.5f;
            const float maxIntensity=1+(std::max(next.ambientBase[3],next.ambientTarget[3])-1)*.5f;
            for(unsigned i=0;i<3;++i){
                const float base=next.ambientBase[i]*baseIntensity;
                const float ceiling=std::max(next.ambientBase[i],next.ambientTarget[i])*maxIntensity;
                ok=ok&&next.ambientBase[i]>0&&next.ambientTarget[i]>=0&&next.heroTarget[i]>=0&&
                    std::isfinite(base)&&base>0&&std::isfinite(ceiling)&&std::isfinite(ceiling/base);
            }
            if(ok){next.litOwners.resize(count);ok=std::fread(next.litOwners.data(),4,count,file)==count&&
                std::is_sorted(next.litOwners.begin(),next.litOwners.end())&&
                std::adjacent_find(next.litOwners.begin(),next.litOwners.end())==next.litOwners.end();}
        }
        if(ok&&regions){
            std::uint32_t count=0;
            ok=std::fread(&count,4,1,file)==1&&count<=LightingConfig{}.triggers.size();
            if(ok)next.darknessTriggers.resize(count);
            for(auto& trigger:next.darknessTriggers){
                std::array<float,4> b{};
                ok=ok&&std::fread(b.data(),4,4,file)==4&&std::fread(&trigger.region,4,1,file)==1;
                for(float value:b)ok=ok&&std::isfinite(value);
                ok=ok&&b[0]<b[2]&&b[1]<b[3]&&(unified||regionalPrepass?(trigger.region>=-1&&trigger.region<=2):hls4?(trigger.region>=-1&&trigger.region<=3):(trigger.region>=1&&trigger.region<=3));
                trigger.bounds={b[0],b[1],b[2],b[3]};
                if(ok&&polygons){
                    ok=std::fread(&trigger.vertexCount,4,1,file)==1&&trigger.vertexCount<=trigger.vertices.size();
                    for(unsigned i=0;ok&&i<trigger.vertexCount;++i)
                        ok=std::fread(&trigger.vertices[i].x,4,1,file)==1&&std::fread(&trigger.vertices[i].y,4,1,file)==1;
                    ok=ok&&trigger.validPolygon();
                }
            }
        }
        if(ok&&!unified&&!regionalPrepass){
            // Positive-only HLS4 is ambiguous: retain main's normalized convention.
            // Prototype Deepnest banks with only positive levels must regenerate as HLS7.
            const bool authored=hls4&&std::any_of(next.darknessTriggers.begin(),next.darknessTriggers.end(),
                [](const auto& trigger){return trigger.region<=0;});
            if(!authored)for(auto& trigger:next.darknessTriggers)trigger.region=trigger.region==1?-1:trigger.region==2?1:0;
        }
        if(ok&&(legacyTwoPass||extended||(unified&&(flags&2u)))){
            ok=std::fread(&next.color.preA,4,1,file)==1&&std::fread(&next.color.preB,4,1,file)==1&&
                std::fread(&next.color.preC,4,1,file)==1;
            next.color.twoPass=true;
        }
        for(float channel:next.heroRgb)ok=ok&&std::isfinite(channel)&&channel>=0;
        ok=ok&&next.darkness>=-1&&next.darkness<=2&&std::isfinite(next.heroAlpha)&&
            next.heroAlpha>=0&&next.heroAlpha<=1&&std::fgetc(file)==EOF;
        std::fclose(file);if(ok)*this=std::move(next);return ok;
    }
    // Historical task packages store precurves and box regions without the
    // GradeMarker prefix. A failed candidate must leave the live state intact.
    bool load(const char* path) {
        if(loadCurrent(path))return true;
        auto* file=std::fopen(path,"rb");if(!file)return false;
        SceneLighting next;char magic[4]{};
        bool ok=std::fread(magic,4,1,file)==1;
        const bool v3=!std::memcmp(magic,"HLS3",4),v4=!std::memcmp(magic,"HLS4",4),v5=!std::memcmp(magic,"HLS5",4);
        ok=ok&&(v3||v4||v5)&&std::fread(&next.darkness,4,1,file)==1&&std::fread(&next.heroAlpha,4,1,file)==1;
        for(auto* value:{&next.color.a,&next.color.b,&next.color.c,&next.color.sat})
            ok=ok&&std::fread(value,4,1,file)==1;
        ok=ok&&std::fread(next.heroRgb.data(),4,3,file)==3;
        const long start=std::ftell(file);
        ok=ok&&start>=0&&std::fseek(file,0,SEEK_END)==0;
        const long remaining=ok?std::ftell(file)-start:-1;
        ok=ok&&std::fseek(file,start,SEEK_SET)==0;
        bool pre=v5,regions=v5;
        if(v3){pre=remaining==12;regions=remaining>=4&&(remaining-4)%20==0;ok=ok&&(pre||regions);}
        if(v4){
            if(remaining>=4&&(remaining-4)%20==0)regions=true;
            else{unsigned flags=0;ok=ok&&std::fread(&flags,4,1,file)==1&&flags>0&&(flags&~3u)==0;
                pre=(flags&1)!=0;regions=(flags&2)!=0;}
        }
        if(ok&&pre){
            for(auto* value:{&next.color.preA,&next.color.preB,&next.color.preC})ok=ok&&std::fread(value,4,1,file)==1;
            next.color.twoPass=true;
        }
        if(ok&&regions){
            unsigned count=0;ok=std::fread(&count,4,1,file)==1&&count<=LightingConfig{}.triggers.size();
            if(ok)next.darknessTriggers.resize(count);
            for(auto& trigger:next.darknessTriggers){
                std::array<float,4> b{};ok=ok&&std::fread(b.data(),4,4,file)==4&&std::fread(&trigger.region,4,1,file)==1;
                for(float v:b)ok=ok&&std::isfinite(v);
                ok=ok&&b[0]<b[2]&&b[1]<b[3]&&(trigger.region==1||trigger.region==2);
                trigger.bounds={b[0],b[1],b[2],b[3]};
            }
        }
        for(float v:next.heroRgb)ok=ok&&std::isfinite(v)&&v>=0;
        ok=ok&&next.darkness>=-1&&next.darkness<=2&&std::isfinite(next.heroAlpha)&&next.heroAlpha>=0&&next.heroAlpha<=1&&std::fgetc(file)==EOF;
        std::fclose(file);if(ok)*this=std::move(next);return ok;
    }

};
}
