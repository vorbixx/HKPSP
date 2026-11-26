#pragma once
#include "room.hpp"
#include <array>
#include <cmath>

namespace hk3ds {
// Initial Fall Impact FSM: parallel Land/Flock actions; delayed bird is .75 s.
class InitialFallPresentation {
    friend struct SimulationStateHash;
public:
    void reset(){cues_={};previous_=RoomPhase::Playing;flockSeconds_=0;}
    void update(RoomPhase phase,float scaledSeconds)
    {
        if(!std::isfinite(scaledSeconds)||scaledSeconds<0)return;
        if(phase!=previous_){
            if(phase==RoomPhase::InitialLand)++cues_[0];
            if(phase==RoomPhase::InitialFlock){++cues_[1];++cues_[3];flockSeconds_=0;}
        }else if(phase==RoomPhase::InitialFlock){
            const float before=flockSeconds_;flockSeconds_+=scaledSeconds;
            if(before<.75f&&flockSeconds_+1e-6f>=.75f){++cues_[2];flockSeconds_=std::max(flockSeconds_,.75f);}
        }
        previous_=phase;
    }
    const std::array<unsigned,4>& cues() const{return cues_;}
private:
    std::array<unsigned,4> cues_{};
    RoomPhase previous_=RoomPhase::Playing;
    float flockSeconds_=0;
};
}
