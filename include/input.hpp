#pragma once
#include <cstdint>

namespace hk3ds
{
enum Button : std::uint16_t { Jump = 1, Dash = 2, Attack = 4, Focus = 8, Down = 16, Dream = 32, Spell = 64, SuperDash = 128 };
struct Input
{
    float x = 0, y = 0;
    std::uint16_t held = 0, pressed = 0, released = 0;
    bool isHeld(Button b) const { return (held & b) != 0; }
    bool isPressed(Button b) const { return (pressed & b) != 0; }
};

// Sampling and consuming are separate: render frames without a control update
// retain edges; catch-up updates do not replay an edge more than once.
class InputLatch
{
public:
    using Source = void (*)(void*, InputLatch&);
    // Optional ordinary-input adapter, sampled once per consumed control step.
    // Rendering, catch-up and game-state ownership remain outside the source.
    void bindSource(void* context, Source source) { context_=context; source_=source; }
    void clear() { pending_={};last_={}; }
    const Input& lastConsumed() const {return last_;}
    void sample(float x, float y, std::uint16_t held)
    {
        pending_.pressed |= static_cast<std::uint16_t>(held & ~pending_.held);
        pending_.released |= static_cast<std::uint16_t>(pending_.held & ~held);
        pending_.held = held;
        pending_.x = x;
        pending_.y = y;
    }
    Input consume()
    {
        if(source_)source_(context_,*this);
        const Input result = pending_;last_=result;
        pending_.pressed = pending_.released = 0;
        return result;
    }
private:
    Input pending_{};
    Input last_{};
    void* context_=nullptr;
    Source source_=nullptr;
};
inline int horizontal(float x) { return (x > 0.3f) - (x < -0.3f); }
inline int vertical(float y) { return (y > 0.5f) - (y < -0.5f); }
}
