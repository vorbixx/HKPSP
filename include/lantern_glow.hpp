#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds
{
struct LanternGlowSprite {unsigned x=0,y=0,width=0,height=0;float worldWidth=0,worldHeight=0;};
struct LanternGlowConfig
{
    static constexpr unsigned frameCount=362;
    float duration=0;
    std::array<LanternGlowSprite,2> sprites{};
    std::array<std::array<float,16>,frameCount> frames{};
    std::array<float,16> sample(float age) const
    {
        if(duration<=0||!std::isfinite(age))return {};
        const float frame=std::fmod(std::max(0.f,age),duration)*60.f;
        const unsigned first=std::min(unsigned(frame),frameCount-2);
        const float blend=std::clamp(frame-first,0.f,1.f);
        std::array<float,16> result{};
        for(unsigned i=0;i<16;++i)result[i]=frames[first][i]+(frames[first+1][i]-frames[first][i])*blend;
        return result;
    }
    bool load(const char* path)
    {
        duration=0;FILE* f=std::fopen(path,"rb");if(!f)return false;
        char magic[4];unsigned count=0;float seconds=0;
        bool ok=std::fread(magic,1,4,f)==4&&std::memcmp(magic,"LNG1",4)==0&&
            std::fread(&count,4,1,f)==1&&count==frameCount&&std::fread(&seconds,4,1,f)==1&&
            std::isfinite(seconds)&&std::fabs(seconds-361.f/60)<.001f;
        for(auto& s:sprites){
            ok=ok&&std::fread(&s,1,24,f)==24&&s.width>0&&s.height>0&&s.x<512&&s.y<256&&
                s.width<=512-s.x&&s.height<=256-s.y&&std::isfinite(s.worldWidth)&&
                std::isfinite(s.worldHeight)&&s.worldWidth>0&&s.worldWidth<10&&s.worldHeight>0&&s.worldHeight<10;
        }
        for(auto& frame:frames){
            ok=ok&&std::fread(frame.data(),4,16,f)==16;
            for(unsigned i=0;i<16;++i)ok=ok&&std::isfinite(frame[i])&&
                (i%4==3?(frame[i]>=0&&frame[i]<=1):std::fabs(frame[i])<10);
        }
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);if(ok)duration=seconds;return ok;
    }
};
}
