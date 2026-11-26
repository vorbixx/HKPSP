#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace hk3ds
{
struct FrameClock
{
    double remainder=0;
    float advance(float unscaled,float scale)
    {
        const float scaled=unscaled*scale;
        remainder+=scaled;
        return scaled;
    }
    bool nextFixed(double step)
    {
        if(remainder<step)return false;
        remainder-=step;return true;
    }
};
// The measured GameManager freeze subset, resumed after Update. A concurrent
// writer is clamped against the shared value while more than one owner lives.
// This intentionally differs from taking the minimum of independent ramps.
class GameClock
{
    friend struct SimulationStateHash;
public:
    struct Freeze
    {
        float down=0, wait=0, up=0, target=0, origin=1, timer=0;
        unsigned phase=0;
        std::uint64_t born=0, id=0;
    };
    float scale=1;
    std::uint64_t frame=0;
    unsigned count=0;
    std::uint64_t overflows=0, invalidSteps=0;
    std::uint64_t start(float down,float wait,float up,float target)
    {
        if (count==freezes_.size()) {++overflows;return 0;}
        auto& f=freezes_[count++];
        f={down,wait,up,target,scale,0,1,frame,++serial_};
        write(f.origin);
        return f.id;
    }
    std::uint64_t heroDamage() { return start(0.001f,0.25f,0.05f,0.0001f); }
    std::uint64_t heroDeath() { return start(0.01f,0.35f,0.1f,0); }
    std::uint64_t enemyKill() { return start(0.04f,0.03f,0.04f,0); }
    bool active(std::uint64_t id) const
    { for(unsigned i=0;i<count;++i)if(freezes_[i].id==id)return true;return false; }
    void beginFrame() { ++frame; }
    void afterUpdate(float unscaled)
    {
        if(!std::isfinite(unscaled)||unscaled<0){++invalidSteps;return;}
        for(unsigned i=0;i<count;)
        {
            auto& f=freezes_[i];
            if(f.born==frame){++i;continue;}
            f.timer+=unscaled;
            if(f.phase==1)
            {
                if(f.timer<f.down)write(f.origin+(f.target-f.origin)*(f.timer/f.down));
                else {write(f.target);f.phase=2;f.timer=0;}
            }
            else if(f.phase==2)
            {
                if(f.timer>=f.wait){f.phase=3;f.timer=0;f.origin=scale;write(f.origin);}
            }
            else
            {
                if(f.timer<f.up)write(f.origin+(1-f.origin)*(f.timer/f.up));
                else
                {
                    write(1);
                    for(unsigned j=i+1;j<count;++j)freezes_[j-1]=freezes_[j];
                    --count;continue;
                }
            }
            ++i;
        }
    }
private:
    std::array<Freeze,16> freezes_{};
    std::uint64_t serial_=0;
    void write(float value)
    {
        if(count>1)value=std::min(value,scale);
        scale=value>0.01f?value:0;
    }
};
}
