#pragma once
#include <array>
#include <cmath>

namespace hk3ds
{
// Shared by the runtime and the measured-state host replay. No native RNG or
// scheduler equivalence is implied. Call once per simulation step (dt <= .03).
inline std::array<float,3> decorMotionStep(std::array<float,3>& position,
    std::array<float,3>& base,const std::array<float,3>& module,
    const std::array<float,3>& forceImpulse,const std::array<float,3>& gravityImpulse,
    float dt,float limit,float retention)
{
    std::array<float,3> total{};
    for(unsigned a=0;a<3;++a){
        base[a]+=forceImpulse[a];
        base[a]+=gravityImpulse[a];
        total[a]=base[a]+module[a];
    }
    const float speed=std::sqrt((total[0]*total[0]+total[1]*total[1])+total[2]*total[2]);
    if(speed>limit){
        const float scale=(limit+(speed-limit)*retention)/speed;
        for(auto& v:total)v*=scale;
    }
    for(unsigned a=0;a<3;++a){
        base[a]=total[a]-module[a];
        position[a]+=total[a]*dt;
    }
    return total;
}
inline float decorRetention(float dampen,float dt)
{
    return std::pow(1.0f-dampen,30.0f*dt);
}
}
