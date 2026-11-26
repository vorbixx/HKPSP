#pragma once
#include "hero.hpp"
#include <array>
#include <cstdint>

namespace hk3ds {
// Modal item UI owns the shared modal space until it closes. A fading
// Focus panel likewise owns that space until its alpha reaches zero.
enum class HudOverlay { None, Reminder, Focus, Inspect, Item };
constexpr HudOverlay hudOverlay(bool item,bool inspect,float focusAlpha,float reminderAlpha)
{
    return item?HudOverlay::Item:inspect?HudOverlay::Inspect:
        focusAlpha>0?HudOverlay::Focus:reminderAlpha>0?HudOverlay::Reminder:HudOverlay::None;
}
constexpr bool hudEventVisible(HudOverlay overlay) { return overlay!=HudOverlay::Item; }
// Idle lore labels yield to the active instruction. During inspection, lore
// replaces reminders and shares its reserved body area with the Focus panel.
constexpr bool hudLoreVisible(HudOverlay overlay,bool inspecting)
{
    return overlay==HudOverlay::None||
        (inspecting&&overlay!=HudOverlay::Item&&overlay!=HudOverlay::Inspect);
}
// Reading sources (user 2026-11-12): Hornet, lore, Listen and Cornifer/pickup text are top-screen boards.
// The lower screen keeps the map unless a choice (Cornifer purchase, ability Yes/No; choice rule) or the
// Focus tablet owns it while no text is open.
struct ReadingSources {
    bool hornet=false,inspect=false,listen=false;   // top-screen text
    bool npcText=false,npcChoice=false;             // Cornifer/Card: question on top, purchase choice on the lower screen
    bool abilityText=false,abilityChoice=false;     // pickup text on top; Yes/No confirm on the lower screen
    bool focus=false;                               // Focus tablet
};
struct ReadingRoute { bool top,bottom; };
constexpr ReadingRoute readingRoute(const ReadingSources& s) {
    const bool text=s.hornet||s.inspect||s.listen||s.npcText||s.abilityText;
    return {text,s.npcChoice||s.abilityChoice||(s.focus&&!text)};
}// Presentation only: callers provide observations after simulation, and reset
// explicitly on scene/retry boundaries. No gameplay state is retained by reference.
struct HudSnapshot {
    int health=5,maxHealth=5,blueHealth=0,soul=0,maxSoul=99;
    std::uint32_t geo=0;
    bool soulDrain=false;
    // Capacity is restored at the recovery hit; the source HUD DOWN event is later.
    bool soulRecovery=false;
    bool soulLimited() const { return maxSoul<99||soulRecovery; }
    static HudSnapshot from(const Vitals& vitals,std::uint32_t geo,bool soulDrain=false,bool soulRecovery=false);
};
enum class HudMaskKind { Hidden, Empty, White, Blue };
enum class HudMaskAnimation { None, Break, Refill };
enum class HudLiquidAnimation { Idle, Shrink };
struct HudMask {
    HudMaskKind kind=HudMaskKind::Hidden;
    HudMaskKind previous=HudMaskKind::Hidden;
    HudMaskAnimation animation=HudMaskAnimation::None;
    float elapsed=0;
};
struct HudTiming {
    // Source HUD library 20665 and Soul Orb/GeoCounter presentation timings.
    float maskBreakSeconds=.3f,maskRefillSeconds=.3f;
    float blueBreakSeconds=.25f,blueRefillSeconds=.45f;
    float soulTweenSeconds=.5f;
    float geoDelaySeconds=1.5f,geoStepSeconds=.025f;
};
struct HudRefillParticle {
    float age=0,life=0,x=0,y=0,speed=0,limit=0,size=0,curveMix=0,force=0;
    unsigned mask=0,emitter=0;
    bool visible() const { return life>0&&age>=0&&age<life; }
    float positionY() const;
    float currentSize() const;
};
class HudPresentation {
public:
    static constexpr unsigned maskCapacity=32;
    static constexpr unsigned refillCapacity=120;
    static constexpr unsigned limiterCapacity=38;
    struct LimiterFragment {
        float age=0,life=0,x=0,y=0,vx=0,vy=0,vz=0,size=0,rotation=0,force=0;
        unsigned variant=0;
        bool visible() const { return life>0&&age>=0&&age<life; }
        float currentSize() const;
        float alpha() const;
    };
    explicit HudPresentation(HudTiming timing={}):timing_(timing) {}
    void reset(const HudSnapshot& snapshot);
    bool update(const HudSnapshot& snapshot,float seconds);
    const std::array<HudMask,maskCapacity>& masks() const { return masks_; }
    const HudSnapshot& snapshot() const { return snapshot_; }
    unsigned maximumMasks() const { return static_cast<unsigned>(snapshot_.maxHealth); }
    float soulFraction() const;
    float displayedSoul() const { return soul_; }
    HudLiquidAnimation liquidAnimation() const { return liquidAnimation_; }
    float liquidSeconds() const { return liquidClock_; }
    float liquidTone() const { return liquidTone_; }
    float soulEyesAlpha() const { return eyesAlpha_; }
    float soulEyesScale() const { return eyesScale_; }
    float soulFlashAlpha() const;
    bool soulFull() const { return snapshot_.maxSoul>=99&&snapshot_.soul>=snapshot_.maxSoul; }
    float soulRimAlpha() const;
    float soulBurstSeconds() const { return soulBurstClock_; }
    float limiterBurstSeconds() const { return limiterBurstClock_; }
    const std::array<LimiterFragment,limiterCapacity>& limiterFragments() const { return limiter_; }
    const std::array<HudRefillParticle,refillCapacity>& refillParticles() const { return refill_; }
    unsigned refillDrops() const { return refillDrops_; }
    bool geoVisible() const { return geoVisible_; }
    float geoRevealSeconds() const { return geoRevealClock_; }
    float geoTextAlpha() const;
    float geoCountingSeconds() const { return geoCounting_?geoRollClock_:-1.f; }
    std::uint32_t pendingGeo() const { return geoPendingValue_; }
    float pendingGeoAlpha() const { return geoPendingAlpha_; }
    std::uint32_t displayedGeo() const { return geo_; }
    bool animating() const;
private:
    void emitFeedback(unsigned mask,unsigned first,unsigned end);
    float feedbackRandom();
    HudTiming timing_;
    HudSnapshot snapshot_{};
    std::array<HudMask,maskCapacity> masks_{};
    std::array<HudRefillParticle,refillCapacity> refill_{};
    std::array<LimiterFragment,limiterCapacity> limiter_{};
    float limiterBurstClock_=1;
    float soulFlashClock_=1,soulFullClock_=0,soulBurstClock_=1;
    unsigned refillDrops_=0;
    bool canFocus_=false;
    std::uint32_t feedbackSeed_=1;
    float soul_=0,soulStart_=0,soulClock_=0,geoClock_=0;
    float liquidClock_=0,liquidTone_=1,liquidToneStart_=1,liquidToneClock_=.2f;
    HudLiquidAnimation liquidAnimation_=HudLiquidAnimation::Idle;
    float eyesClock_=1,eyesAlpha_=0,eyesScale_=.4f,eyesStartAlpha_=0,eyesStartScale_=.4f;
    bool eyesOver_=false;
    bool geoVisible_=false;
    float geoRevealClock_=1;
    float geoDelayClock_=0,geoRollClock_=0,geoPendingClock_=.25f,geoPendingAlpha_=0,geoPendingStartAlpha_=0;
    bool geoCounting_=false,geoPendingUp_=false;
    std::uint32_t geoPendingValue_=0;
    std::uint32_t geo_=0,geoStep_=1;
    bool initialized_=false;
};
}
