#pragma once
#include <algorithm>
#include <array>

namespace hk3ds {
// Source Switch Control -> Mantis/Toll Gate clocks. Clip lengths come from tk2d.
struct SceneLeverState {
    // Lever Hit wait, Open Pause wait, Dust wait, Door Open 1, Door Open 2.
    std::array<float,5> seconds{};
    float elapsed=0;
    float shakeFps=0;
    bool activated=false,pillar=false;
    float colliderTime() const {return seconds[0]+seconds[1]+seconds[2]+(pillar?0:seconds[3]);}
    float finishTime() const {return seconds[0]+seconds[1]+seconds[2]+seconds[3]+seconds[4];}
    void restore(bool saved) {activated=saved;elapsed=saved?finishTime():0;}
    void activate() {activated=true;elapsed=0;}
    void update(float dt) {if(activated)elapsed=std::min(finishTime(),elapsed+dt);}
    bool colliderEnabled() const {return !activated||elapsed<colliderTime();}
    bool visible() const {return pillar||!activated||elapsed<finishTime();}
    unsigned openingFrame(unsigned first,unsigned second) const {
        if(!first||!activated)return 0;
        if(pillar){
            if(elapsed<seconds[0])return 0;
            const float shakeEnd=seconds[0]+seconds[1];
            if(elapsed<shakeEnd)return 1+static_cast<unsigned>((elapsed-seconds[0])*shakeFps)%first;
            if(elapsed<shakeEnd+seconds[3])return 1+first+std::min(second-1,static_cast<unsigned>((elapsed-shakeEnd)/seconds[3]*second));
            return 1+first+second;
        }
        const float start=seconds[0]+seconds[1]+seconds[2];
        if(elapsed<start)return 0;
        if(seconds[3]>0&&elapsed<colliderTime())
            return 1+std::min(first-1,static_cast<unsigned>((elapsed-start)/seconds[3]*first));
        if(!second||seconds[4]<=0)return first;
        return 1+first+std::min(second-1,static_cast<unsigned>((elapsed-colliderTime())/seconds[4]*second));
    }
};
static_assert(sizeof(SceneLeverState)<=32,"Source lever state exceeded its storage estimate");
}
