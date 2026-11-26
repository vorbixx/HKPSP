#pragma once
#include "animation.hpp"
#include "combat.hpp"
#include "enemy.hpp"
namespace hk3ds {
struct ShadeEmitter {
    float rate=0,lifeMin=1,lifeMax=2,speedMin=0,speedMax=1,forceMin=.8f,forceMax=1.5f;
    float x=0,y=0,radius=.5f,size=1,duration=1;
};
struct ShadeConfig {
    Rect body{},alert{},slash{};
    std::array<Vec2,10> markers{};
    std::array<Vec2,5> slashPoints{};
    std::array<ShadeEmitter,4> emitters{};
    bool load(const char* path);
    Vec2 spawnPosition(Vec2 death) const;
};
enum class ShadeState { Absent,Idle,Startle,Fly,Position,SlashAntic,Slash,SlashEnd,SlashCooldown,RetreatStart,Retreat,RetreatEnd,Recover,RecoveryBlow,RecoveryEnd,RecoveryDissipate };
// One persistent recovery actor. Scene reconstruction owns its combat registration.
class HollowShade {
    friend struct SimulationStateHash;
public:
    float artScale() const {return animationFacingScale("shade/shade",-scaleX,std::abs(scaleX));}
    // Cosmetic quads share the existing 512 records with the four mote emitters.
    enum HitKind : unsigned { GhostOne=4,GhostTwo,HitSlash,HitFlash,HitMote };
    struct Particle {
        Vec2 position{},velocity{};PresentationPosition history{};
        float age=0,lifetime=0,force=0,mix=0;
        unsigned emitter=0;
        bool occupied()const{return age<lifetime;}
        bool active()const{return emitter<GhostOne&&occupied();}
        bool hitActive()const{return emitter>=GhostOne&&occupied();}
        float size()const;
        const char* hitClip()const;
        float hitScaleX()const;
        float hitScaleY()const;
        float hitRotation()const;
        float hitOpacity()const;
        Vec2 renderPosition(PresentationTime t)const{return history.sample(position,t);}
    };
    static constexpr unsigned particleCapacity=512;
    void bind(const ShadeConfig&,const AnimationBank*,CombatObject&,Vec2 spawn,int hp,EnemyRandom);
    void clear();
    void update(float dt,const Hero&,const World&);
    void fixedUpdate(float dt,Hero&,const World&);
    void presentationFixedUpdate(float dt);
    const auto& particles()const{return particles_;}
    const auto& hitQuads()const{return particles_;}
    const auto& emitted()const{return emitted_;}
    unsigned droppedParticles()const{return droppedParticles_;}
    float hitNeutral()const{return std::min(1.f,hitAge_/.25f);}
    float hitAge()const{return hitAge_;}
    float recoveryAge()const{return state==ShadeState::Recover?timer_:state==ShadeState::RecoveryBlow?.5f:state==ShadeState::RecoveryEnd?.5f+timer_:state==ShadeState::RecoveryDissipate?1.25f+timer_:0;}
    bool beamVisible()const{return state==ShadeState::Recover;}
    bool alive() const {return state!=ShadeState::Absent&&state<ShadeState::Recover;}
    bool recovering() const {return state>=ShadeState::Recover;}
    bool visible() const {return state!=ShadeState::Absent;}
    float alpha()const{return state==ShadeState::RecoveryDissipate?std::max(0.0f,1-timer_):1;}
    Vec2 renderPosition(PresentationTime t)const{return state==ShadeState::Retreat?retreatPresentation_.sample(retreatPose_,t):presentation_.sample(position,t);}
    Rect bounds()const{return boxBounds(position,config_.body,scaleX);}
    Rect slashBounds()const{return boxBounds(position,config_.slash,scaleX);}
    ShadeState state=ShadeState::Absent;
    Vec2 position{},velocity{},origin{};
    float scaleX=1;
    AnimationPlayer animation,slashAnimation,glowAnimation;
    unsigned startleSerial=0,slashSerial=0,injurySerial=0,recoverySerial=0;
    unsigned blowSerial=0;
    AnimationPlayer deathSprite;
private:
    ShadeConfig config_{};
    const AnimationBank* bank_=nullptr;
    CombatObject* object_=nullptr;
    EnemyRandom random_{};
    PresentationPosition presentation_{};
    PresentationPosition retreatPresentation_{};
    Vec2 retreatPose_{};
    std::array<Particle,particleCapacity> particles_{};
    std::uint32_t particleSeed_=0x53484144u;
    float emissionRemainder_=0;
    float waveRemaining_=0,waveRemainder_=0;
    float hitAge_=1,hitRemaining_=0,hitRemainder_=0,hitAngle_=0;
    Vec2 hitOrigin_{};
    unsigned particleCursor_=0,droppedParticles_=0;
    std::array<unsigned,4> emitted_{};
    float particleRandom();
    void emit(unsigned emitter,unsigned count);
    Particle* allocate(unsigned kind);
    void emitHit(Strike);
    void emitHitMotes(unsigned count);
    Vec2 retreatFrom_{};
    float timer_=0,duration_=0;
    float recoil_=0;
    Vec2 recoilVelocity_{};
    void enter(ShadeState);
    void sync();
    static void hit(void*,const NailHit&,HitOutcome);
};
}
