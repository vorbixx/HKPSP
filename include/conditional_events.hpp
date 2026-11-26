#pragma once
#include "animation.hpp"
#include "combat.hpp"
#include "conditional_save.hpp"
#include "presentation.hpp"
#include "hero_effects.hpp"
#include <array>
#include <algorithm>
#include <vector>

namespace hk3ds {
enum class MushroomPhase { Off,Rest,Wake,Idle,Hit,TurnBack,RocketPause,RocketAntic,Lift,Fly,TalkPause,Gone };
enum class GrimmkinPhase { Off,Waiting,Alert,TeleIn,Follow,TeleOut,Hidden,DashAntic,Dash,DashEnd,ShootAntic,ShootEnd,SpiralAntic,SpiralEnd,Death,Absorb,Ring,Collected };
enum class EventDialogue { None,Dream,Shroomish,Final,Farewell };
struct EventProjectile {
    bool active=false,spiral=false;
    Vec2 position{},velocity{};
    float age=0,angle=0,speed=0;
    PresentationPosition presentation;
    AnimationPlayer animation;
    Vec2 renderPosition(PresentationTime t) const {return presentation.sample(position,t);}
    float scale() const {return age>(spiral?3.f:5.f)?std::max(0.f,1-(age-(spiral?3.f:5.f))/.5f):1;}
};
struct EventParticle {
    Vec2 position{},velocity{};
    PresentationPosition presentation;
    AnimationPlayer animation;
    float age=0,life=0,size=0,gravity=0;
    bool active=false;
    Vec2 renderPosition(PresentationTime t) const {return presentation.sample(position,t);}
    float alpha() const {return life>0?std::clamp(1-age/life,0.f,1.f):0;}
};
struct EventEmitter {
    float rate=0,lifeMin=0,lifeMax=0,sizeMin=0,sizeMax=0,speedMin=0,speedMax=0,gravity=0,x=0,y=0;
};
struct EventSourceEmitter {
    unsigned owner=0,shape=0,tiles=0,flags=0,capacity=0;
    std::array<float,86> values{}; // Same source fields as HeroEffectConfig.
};
class ConditionalEvents {
    friend struct SimulationStateHash;
public:
    float mushroomArtScale() const {return animationFacingScale("conditional-events/mushroom",-1);}
    float grimmkinArtScale() const {return animationFacingScale("conditional-events/grimmkin",facing);}
    bool bind(const AnimationBank* bank,const char* textPath);
    void reset(const ConditionalSave& data={});
    void update(float seconds,Hero& hero,const NailBank& nails,bool freezeEnemies=false);
    const ConditionalSave& saved() const {return save_;}
    // Ownership uses the existing save flag; acquisition/seals belong to the Dreamer controller.
    void setDreamNailOwned(bool owned) {
        if(owned)save_.flags|=ConditionalSave::DreamNail;
        else save_.flags&=~ConditionalSave::DreamNail;
    }
    MushroomPhase mushroom=MushroomPhase::Off;
    GrimmkinPhase grimmkin=GrimmkinPhase::Off;
    EventDialogue dialogue=EventDialogue::None;
    Vec2 mushroomPosition{36.36f,12.27f},grimmkinPosition{147.67f,5.59f};
    AnimationPlayer mushroomAnimation,grimmkinAnimation,flameAnimation,flashAnimation;
    std::array<EventProjectile,24> projectiles{};
    std::array<EventParticle,128> particles{};
    // 50 impact + 250 blastoff + 20 lava + 25 smoke + 40 orbs <= 385.
    static constexpr unsigned sourceCapacity=400;
    const std::vector<HeroEffectParticle>& sourceParticles() const {return sourceParticles_;}
    const std::array<EventSourceEmitter,5>& sourceEmitters() const {return sourceEmitters_;}
    const std::array<unsigned,5>& sourceEmitted() const {return sourceEmitted_;}
    unsigned sourceDropped() const {return sourceDropped_;}
    unsigned sourceActiveCount() const {unsigned n=0;for(auto v:sourceLive_)n+=v;return n;}
    float sourceParticleSize(const HeroEffectParticle&) const;
    float sourceParticleOpacity(const HeroEffectParticle&) const;
    Vec2 mushroomRenderPosition(PresentationTime t) const {return mushroomPresentation_.sample(mushroomPosition,t);}
    Vec2 grimmkinRenderPosition(PresentationTime t) const {return grimmkinPresentation_.sample(grimmkinPosition,t);}
    bool mushroomVisible() const {return mushroom!=MushroomPhase::Off&&mushroom!=MushroomPhase::Gone;}
    bool grimmkinVisible() const;
    bool flameVisible() const {return grimmkin==GrimmkinPhase::Waiting||grimmkin==GrimmkinPhase::Alert;}
    bool locked() const {return dialogue!=EventDialogue::None||mushroom==MushroomPhase::RocketPause||mushroom==MushroomPhase::RocketAntic||mushroom==MushroomPhase::Lift||mushroom==MushroomPhase::Fly||mushroom==MushroomPhase::TalkPause;}
    bool listenPrompt=false;
    unsigned textPage() const {return textPage_;}
    bool hasNextPage() const {return textPage_<lastPage_;}
    const char* text() const;
    float facing=1;
    EnemyHealth health;
    unsigned mushroomHitSerial=0,rocketSerial=0,dreamSerial=0,teleSerial=0,dashSerial=0,shootSerial=0,spiralSerial=0,deathSerial=0,pickupSerial=0,dashStartSerial=0,castSerial=0,ringSerial=0,teleInSerial=0;
private:
    void mushroomState(MushroomPhase,const char* clip,float duration=0);
    void grimmkinState(GrimmkinPhase,const char* clip,float duration);
    void beginDialogue(EventDialogue,unsigned first,unsigned last);
    void finishDialogue(Hero&);
    void shoot(bool spiral);
    void attack(Hero&);
    float random(float low,float high);
    void updateMushroom(float,Hero&,const NailBank&);
    void updateGrimmkin(float,Hero&,const NailBank&,bool frozen);
    void emit(unsigned,Vec2,float);
    void updateEffects(float,Hero&);
    bool loadSourceEffects(const char*);
    void startSourceEffect(unsigned,Vec2);
    void updateSourceEffects(float);
    void spawnSourceParticle(unsigned,Vec2);
    float sourceRandom(float,float);
    struct SourceEmission {Vec2 origin{};float age=0,remainder=0;bool active=false;};
    std::array<EventSourceEmitter,5> sourceEmitters_{};
    std::array<const Clip*,5> sourceClips_{};
    // Allocate only on reset to a save that admits either conditional event.
    std::vector<HeroEffectParticle> sourceParticles_;
    std::array<SourceEmission,5> sourceEmissions_{};
    std::array<unsigned,5> sourceEmitted_{},sourceLive_{};
    unsigned sourceDropped_=0,sourceRandom_=1;
    bool sourceReady_=false;
    const AnimationBank* bank_=nullptr;
    ConditionalSave save_{};
    std::array<std::array<char,1024>,11> texts_{};
    PresentationPosition mushroomPresentation_,grimmkinPresentation_;
    Vec2 velocity_{};
    std::array<EventEmitter,8> emitters_{};
    std::array<float,8> emission_{};
    unsigned effectsRandom_=0x46584d52u;
    float mushroomTimer_=0,grimmkinTimer_=0;
    unsigned lastNailMushroom_=~0u,lastNailGrimmkin_=~0u,textPage_=0,lastPage_=0,random_=0x4752494du;
    unsigned lastAttack_=3,repeats_=0;
    std::array<unsigned,3> missed_{};
    bool upHeld_=false,buttonHeld_=false;
};
}
