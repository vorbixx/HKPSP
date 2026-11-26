#pragma once
#include <algorithm>
#include "geo_visual.hpp"
#include "presentation.hpp"

namespace hk3ds {
class TerrainBank;
enum class FuryPart : unsigned { Chest, Back, Front, Burst, Wave, Shiny, Glow, Icon, Count };
struct FuryEmitterConfig {
    // Origin XYZ, transformed circle axes XYZ/XYZ, arc, radius, rate, duration,
    // lifetime/size/speed/forceY ranges, speed limit/damping, alpha, 32 size samples.
    std::array<float,56> v{};
};
struct FuryParticle {
    GeoVisualDraw draw{};
    Vec2 velocity{};
    float velocityZ=0,age=0,lifetime=0,size=0,forceY=0;
    unsigned emitter=0;
};
struct FuryChestConfig {
    std::array<GeoVisualDraw,static_cast<unsigned>(FuryPart::Count)> parts{};
    Rect nailBounds{},heroRange{},inspectRange{};
    std::array<Rect,2> openedSides{};
    std::array<FuryEmitterConfig,4> emitters{};
    bool rangeRequired=false;
    float spawnSeconds=1.f/12;
    Rect shinyBounds{-.2625f,-.2625f,.2625f,.2625f};
    float shinyGravity=-51;
    std::array<float,3> flingRight{{25,82,86}},flingLeft{{20,98,102}};
    float bounceFactor=.5f,speedThreshold=1;
    bool load(const char* path);
};
struct FuryChestInput {
    bool nailHit=false,heroRange=false,inspectRange=false,grounded=false;
    bool inspect=false,button=false,damaged=false,aligned=false;
    Vec2 heroPosition{},nailPosition{};
    Strike direction=Strike::Right;
    unsigned openedSide=0;
};
enum class FuryChestPhase { Closed, Opening, Opened };
enum class FuryPickupPhase { Hidden, Idle, Aligning, Lowering, Display, WaitButton, Raising, Collected };
enum class FuryTutorialPart : unsigned { Backboard, Title, Icon, Name, Divider, Subtitle, Text, Illustration, Prompt };
// Presentation clock starts at Display entry and continues through WaitButton.
float furyTutorialAlpha(FuryTutorialPart, float seconds);
float furyTutorialRowAlpha(unsigned row, float seconds);
// Source Chest Control 12137 and Shiny Control 12202, charm ID 6.
// Rendering/audio adapters observe this bounded state machine.
class FuryChest {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned openedFlag=64,gotCharm6Flag=128,hasCharmFlag=256;
    bool bind(const AnimationBank&,const FuryChestConfig&);
    bool bind(const AnimationBank&,const char* path);
    // Startup only: appends opened sides before World views are constructed.
    bool bindTerrain(TerrainBank&);
    void reset(bool preserve=true,unsigned seed=1);
    void restore(unsigned flags);
    unsigned flags() const;
    void update(float dt,const FuryChestInput&);
    FuryChestPhase phase() const { return phase_; }
    FuryPickupPhase pickup() const { return pickup_; }
    bool collecting() const { return pickup_==FuryPickupPhase::Aligning||pickup_==FuryPickupPhase::Lowering||pickup_==FuryPickupPhase::Display||pickup_==FuryPickupPhase::WaitButton||pickup_==FuryPickupPhase::Raising; }
    bool prompt() const { return prompt_; }
    bool gotCharm6() const { return gotCharm6_; }
    unsigned openSerial() const { return openSerial_; }
    unsigned pickupSerial() const { return pickupSerial_; }
    float waveOpacity() const { return draws_[static_cast<unsigned>(FuryPart::Wave)].visible?std::max(0.f,1-waveSeconds_):0; }
    float displaySeconds() const { return displaySeconds_; }
    const FuryChestConfig& config() const { return config_; }
    const auto& draws() const { return draws_; }
    const auto& particles() const { return particles_; }
    Vec2 shake() const { return shake_; }
    unsigned tinkSerial() const { return tinkSerial_; }
    const auto& effects() const { return effects_; } // opening strike, pickup flash, nail block
    float tinkRotation() const { return tinkRotation_; }
    float flashOpacity() const;
    bool flying() const { return flying_; }
    bool settled() const { return shinySettled_; }
    Vec2 shinyPosition() const { return shinyPosition_; }
    Vec2 shinyRenderPosition(PresentationTime time) const { return shinyHistory_.sample(shinyPosition_,time); }
    Rect inspectRange() const;
    GeoVisualDraw draw(FuryPart,PresentationTime) const;
private:
    const AnimationBank* bank_=nullptr;
    TerrainBank* terrain_=nullptr;
    void syncTerrain();
    FuryChestConfig config_{};
    std::array<GeoVisualDraw,static_cast<unsigned>(FuryPart::Count)> draws_{};
    FuryChestPhase phase_=FuryChestPhase::Closed;
    FuryPickupPhase pickup_=FuryPickupPhase::Hidden;
    bool opened_=false,gotCharm6_=false,hasCharm_=false,prompt_=false;
    unsigned openSerial_=0,pickupSerial_=0;
    float openingSeconds_=0,displaySeconds_=0,waveSeconds_=0;
    float waveAccel_=3,waveFixed_=0,ringSeconds_=-1;
    float idleWaveSeconds_=0,shakeSeconds_=0,shakeFrame_=0;
    Vec2 shake_{};
    std::array<FuryParticle,384> particles_{};
    bool particlesLive_=false;
    std::array<float,4> emission_{};
    std::array<GeoVisualDraw,3> effects_{};
    float flashSeconds_=-1,tinkDelay_=0,tinkRotation_=0;
    unsigned tinkSerial_=0;
    Vec2 flashOrigin_{};
    Vec2 shinyPosition_{},shinyVelocity_{};
    PresentationPosition shinyHistory_{};
    bool flying_=false,shinyContact_=false,shinySettled_=true;
    void fling();
    void updateShiny(float dt);
    unsigned random_=1;
    float random(float minimum,float maximum);
    void emit(unsigned emitter);
    void updateParticles(float dt);
    void beginWave();
    void start(FuryPart,const char*,bool visible=true);
};
}
