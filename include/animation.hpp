#pragma once
#include "hero.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hk3ds
{
class NailBank;
struct SpriteFrame
{
    std::uint32_t page, x, y, width, height;
    float left, bottom, right, top;
    // Optional platform atlas CLUT; legacy and 3DS readers leave it at zero.
    std::uint32_t clut=0;
};
struct Clip
{
    char name[48];
    float fps;
    std::uint32_t wrap, loopStart, first, count;
};
// Direction is logical world motion; rightSign is exported source evidence.
// Magnitude belongs to the scene instance, independently of its art convention.
float animationFacingScale(int rightSign, float direction, float magnitude = 1.0f);
// Reads the converted, source-backed table embedded in both platform builds.
float animationFacingScale(const char* kind, float direction, float magnitude = 1.0f);
// Process-wide total remains observable after a room's banks are unloaded.
std::uint32_t animationMissingPlayCount();
struct ActorFacing
{
    char kind[64];
    std::int32_t rightSign;
};
class AnimationFacingConfig
{
public:
    // AFC1 exported alongside the evidence JSON. Incomplete tables fail closed.
    bool load(const char* path);
    float scale(const char* kind, float direction, float magnitude = 1.0f) const;
private:
    std::vector<ActorFacing> actors_;
};
struct AnimationMiss
{
    std::string name;
    std::uint32_t count = 0;
};
struct AnimationEvent
{
    std::uint32_t frame;
    char info[64];
    std::int32_t integer;
    float value;
};
// Banks without frame triggers allocate no event storage. Copies retain value semantics.
class AnimationEventTable
{
public:
    AnimationEventTable() = default;
    AnimationEventTable(const AnimationEventTable&);
    AnimationEventTable& operator=(const AnimationEventTable&);
    AnimationEventTable(AnimationEventTable&&) = default;
    AnimationEventTable& operator=(AnimationEventTable&&) = default;
    const std::vector<AnimationEvent>& entries() const;
    void replace(std::vector<AnimationEvent>);
private:
    std::unique_ptr<std::vector<AnimationEvent>> entries_;
};
class AnimationBank
{
public:
    bool load(const char* path);
    const Clip* find(const char* name) const;
    // Required playback lookup. Optional feature probes must continue using find.
    const Clip* require(const char* name) const;
    mutable std::uint32_t missingPlayCount = 0;
    std::uint32_t pageCount = 0;
    mutable std::vector<AnimationMiss> missingPlays;
    std::vector<SpriteFrame> sprites;
    std::vector<Clip> clips;
    std::vector<std::uint32_t> frames;
    AnimationEventTable events;
};
static_assert(sizeof(AnimationBank) <= (sizeof(void*) == 8 ? 112 : 60),
              "Optional frame events must keep bank metadata bounded");
struct AnimationPlayer
{
    const Clip* clip = nullptr;
    float clock = 0;
    std::uint32_t frame = 0;
    bool complete = false;
    using EventCallback = void (*)(void*, const Clip&, std::uint32_t, const AnimationEvent&);
    bool initialEvent = false;
    void play(const Clip* next, std::uint32_t start = 0, bool restart = false);
    void play(const AnimationBank& bank, const char* name,
              std::uint32_t start = 0, bool restart = false);
    void advance(float seconds);
    // Callback state belongs to the caller; ordinary players retain their footprint.
    void advance(float seconds, const AnimationBank&, EventCallback, void* context = nullptr);
    const SpriteFrame* current(const AnimationBank& bank) const;
};
static_assert(sizeof(AnimationPlayer) == (sizeof(void*) == 8 ? 24 : 16),
              "Frame events must preserve the animation player memory budget");
// Walker.BeginTurning retains the old scale until the authored turn finishes.
// Caller owns motion/FSM decisions and stops horizontal velocity while active.
struct AnimationTurn
{
    const Clip* resume = nullptr;
    float targetScale = 1;
    bool active = false;
    void begin(AnimationPlayer&, const AnimationBank&, const char* turn,
               const char* walk, float nextScale);
    bool finish(AnimationPlayer&, float& scale);
};
struct HeroAnimator
{
    // Presentation owner may hold an interaction clip while gameplay input is locked.
    const Clip* overrideClip=nullptr;
    AnimationPlayer player;
    AnimationPlayer slash;
    std::size_t slashVariant = 0;
    std::uint32_t lastAttack = 0;
    std::uint32_t lastSpell = 0;
    Actor previousActor = Actor::Airborne;
    FocusPhase previousFocus = FocusPhase::None;
    bool previousDash = false, previousFacing = true;
    void update(const Hero& hero, const AnimationBank& bank, float dt, const NailBank* nails = nullptr);
};
}
