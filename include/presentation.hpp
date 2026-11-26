#pragma once
#include "physics.hpp"

namespace hk3ds
{
// Shared display phase, supplied by Simulation (or TutorialSession).
struct PresentationTime { float alpha=0; };

// Three fixed poses cover the render remainder and one control interval of
// latency. This adapter never writes back to physics or extrapolates motion.
class PresentationPosition
{
    friend struct SimulationStateHash;
public:
    void reset(Vec2 position) { older_=previous_=current_=position; valid_=true; }
    Vec2 sample(Vec2 position, PresentationTime time) const
    {
        if (!matches(position)) return position; // Spawn, reuse or teleport.
        return time.alpha<0 ? interpolatePosition(older_,previous_,time.alpha+1) :
                              interpolatePosition(previous_,current_,time.alpha);
    }
    // Keep this guard alive for the whole fixed update, including early returns.
    // Capturing here also works for callers outside the room scheduler.
    class Step
    {
    public:
        Step(PresentationPosition& history, const Vec2& position):history_(history),position_(position)
        { if (!history_.matches(position)) history_.reset(position); }
        ~Step()
        { history_.older_=history_.previous_; history_.previous_=history_.current_; history_.current_=position_; }
        Step(const Step&)=delete;
        Step& operator=(const Step&)=delete;
    private:
        PresentationPosition& history_;
        const Vec2& position_;
    };
private:
    bool matches(Vec2 p) const { return valid_ && p.x==current_.x && p.y==current_.y; }
    Vec2 older_{},previous_{},current_{};
    bool valid_=false;
};
}
