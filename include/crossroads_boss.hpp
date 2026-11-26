#pragma once
#include "enemy.hpp"
#include "save.hpp"
#include <algorithm>
#include <cstring>

namespace hk3ds {
enum class CrossroadsBossKind { None, GruzMother, FalseKnight, DungDefender };
enum class BossPhase { Dormant, Intro, Idle, Aim, Charge, Bounce, Jump, Slam,
    Stagger, Recover, Rage, Dying, Brood, Collapse, Defeated, Land,
    JumpRecover, RagePause, RageJumpAntic, RageJump, RageLand, RageAntic,
    RageBegin, RageEnd, Swing, StunJump, StunLand, StunPause, StunOpen,
    JumpHit, JumpRecover2, JumpEnd,
    DolphinAntic, Dolphin, DungEvade, StunAir, StunRecover,
    DolphinRecover, DungEvadeBall, DungRageIn, DungRoar };
struct BossWaveSpurt {
    Vec2 position{};
    float elapsed=0;
    unsigned phase=0; // 0: unused, 1: anticipation, 2: damaging.
};
struct BossProjectile {
    Vec2 position{},velocity{};
    PresentationPosition presentation;
    AnimationPlayer animation;
    unsigned bounces=0;
    float life=0,accelerationX=0,waveEndPause=0;
    float barrelRecycleWait=0;
    // RandomScale.Start runs once per pooled barrel; Rotate uses world Z degrees.
    float barrelScale=1,barrelAngle=0,barrelSpin=0;
    bool barrelScaled=false;
    std::array<Vec2,4> barrelHitQuad{};
    unsigned barrelBreakUpdates=0;
    bool ground=false,burst=false,deflected=false,awaitingWaveUpdate=false,waveEnding=false;
    // At 60 control updates/s, the two .05-s waits require at most six slots.
    std::array<BossWaveSpurt,8> waveSpurts{};
    bool reusable()const{
        return life<=0&&barrelRecycleWait<=0&&std::none_of(waveSpurts.begin(),waveSpurts.end(),[](const auto& s){return s.phase!=0;});
    }
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
    std::array<Vec2,4> barrelQuad(float skin=0)const;
    void resetFlight(){const auto scale=barrelScale;const auto scaled=barrelScaled;
        *this={};barrelScale=scale;barrelScaled=scaled;}
};
struct BossMinion {
    float artScale()const{return animationFacingScale("crossroads-bosses/Gruzzer",velocity.x<0?-1.f:1.f);}
    Vec2 position{},velocity{};
    PresentationPosition presentation;
    AnimationPlayer animation;
    bool alive=false;
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
};
struct BossEffectParticle {
    Vec2 position{},velocity{};
    PresentationPosition presentation;
    float age=0,life=0,size=0,angle=0,accelerationY=0;
    unsigned emitter=0;
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
};
struct BossEffectBillboard {
    Vec2 offset{},scale{};
    float rotation=0;
};
// One bounded encounter. Arena coordinates are supplied by the scene adapter;
// the optional Tutorial fixture uses the same combat and progression path.
class CrossroadsBoss {
    friend struct SimulationStateHash;
public:
    float artScale(bool head=false,bool standalone=false)const {
        const char* id=kind==CrossroadsBossKind::GruzMother?"crossroads-bosses/Gruz":kind==CrossroadsBossKind::DungDefender?"dung-defender/Dung":
            head?(standalone?"false-knight/Head":"crossroads-bosses/Head"):(standalone?"false-knight/Knight":"crossroads-bosses/Knight");
        return animationFacingScale(id,kind==CrossroadsBossKind::GruzMother?-scaleX:scaleX,std::abs(scaleX));
    }
    static constexpr unsigned minionCapacity=8,projectileCapacity=12;
    static constexpr unsigned projectileObjectBegin=minionCapacity+1;
    static constexpr unsigned objectCapacity=projectileObjectBegin+projectileCapacity;
    bool bind(CrossroadsBossKind,Rect arena,CombatObject* objects,unsigned count,
              const AnimationBank*,std::uint32_t savedFlags=0,bool cityCrestSpent=false);
    void reset();
    void place(Vec2 p,float scale){position=p;scaleX=scale;bodyScale_=scale<0?-scale:scale;presentation_.reset(p);sync();}
    void bindArt(const AnimationBank* bank){bank_=bank;enter(phase,timer_);}
    bool loadEffects(const char* path);
    static constexpr unsigned effectCapacity=384;
    const std::vector<BossEffectParticle>& effectParticles()const{return effectParticles_;}
    float effectSize(const BossEffectParticle&)const;
    float effectOpacity(const BossEffectParticle&)const;
    BossEffectBillboard effectBillboard(const BossEffectParticle&)const;
    void activationRegion(Rect r){activationRegion_=r;authoredActivation_=true;}
    void fixedUpdate(float dt,Hero&,const World* terrain=nullptr);
    void update(float dt);
    Rect bounds()const;
    Rect headBounds()const;
    // Empty outside the source Hitter animation's active frames.
    Rect weaponBounds()const;
    // Source live Stun Opened and Death Spaz captures retain this child offset.
    Vec2 headPosition(Vec2 anchor)const{return deathHeadDetached_?deathHeadPosition_:
        Vec2{anchor.x+2.29477954f*scaleX,anchor.y-3.270931244f*bodyScale_};}
    // During Stun Open the source moves the separate head to local Y=150;
    // the opening artwork already contains the emerging maggot.
    bool headVisible()const{return kind==CrossroadsBossKind::FalseKnight&&
        (corpsePresent_||(phase==BossPhase::Stagger&&animation.clip&&
         !std::strcmp(animation.clip->name,"Knight/Opened")));}
    float titleAlpha()const{return titleAge_<0?0:std::min(1.f,titleAge_/.5f)*std::clamp(4.f-titleAge_,0.f,1.f);}
    World arenaWorld()const;
    // Source GO47: solid shell support from Roll End until recovery/death.
    TerrainShape terrainBlock()const;
    Vec2 renderPosition(PresentationTime t)const{return presentation_.sample(position,t);}
    bool active()const{return kind!=CrossroadsBossKind::None;}
    // Hits and the Dormant strike-to-start path use targetable(); rendering uses visible().
    // A defeated False Knight stays as a corpse but takes no further hits (the
    // original death is final; a hit would stand it up and pay the reward again).
    bool targetable()const{return active()&&phase!=BossPhase::Defeated&&
        !(kind==CrossroadsBossKind::DungDefender&&(phase==BossPhase::Dormant||phase==BossPhase::Charge||phase==BossPhase::Slam));}
    bool visible()const{return active()&&(phase!=BossPhase::Defeated||corpsePresent_)&&
        !(kind==CrossroadsBossKind::GruzMother&&phase==BossPhase::Brood)&&
        !(kind==CrossroadsBossKind::DungDefender&&(phase==BossPhase::Dormant||phase==BossPhase::Charge||phase==BossPhase::Slam))&&
        // Authored armour waits above the chamber until its activation fall starts.
        !(kind==CrossroadsBossKind::FalseKnight&&authoredActivation_&&phase==BossPhase::Dormant);}
    bool locked()const{return active()&&phase!=BossPhase::Defeated;}
    bool floorCollapsed()const{return kind==CrossroadsBossKind::FalseKnight&&(phase==BossPhase::Collapse||(savedFlags_&TutorialSave::falseKnightFloorFlag)!=0);}
    std::uint32_t savedFlags()const{return savedFlags_;}
    bool takeGruzGeo(Vec2& origin){if(!gruzGeoPending_)return false;origin=gruzGeoOrigin_;gruzGeoPending_=false;return true;}
    bool gruzGeoPending()const{return gruzGeoPending_;}
    const Rect& arena()const{return arena_;}
    unsigned phaseSerial=0,impactSerial=0,hitSerial=0;
    unsigned staggerCount=0,bounceCount=0,attacks=0;
    CrossroadsBossKind kind=CrossroadsBossKind::None;
    BossPhase phase=BossPhase::Dormant;
    Vec2 position{},velocity{};
    float scaleX=1,hitFlash=0;
    AnimationPlayer animation,headAnimation;
    std::array<BossMinion,minionCapacity> minions{};
    std::array<BossProjectile,projectileCapacity> projectiles{};
private:
    void updateEffects(float);
    void emitEffect(unsigned,Vec2,unsigned,float speedLo=-1,float speedHi=0,float angleLo=0,float angleHi=360);
    std::array<std::array<float,49>,8> effectConfigs_{};
    std::vector<BossEffectParticle> effectParticles_;
    std::array<float,8> effectClocks_{},effectRemainders_{};
    std::array<Vec2,8> effectOrigins_{};
    World terrainWorld_{};
    bool terrainKnown_=false;
    float deathEffectAge_=-1,steamRemainder_=0;
    bool deathHeadDetached_=false,deathHeadLanded_=false;
    Vec2 deathHeadOrigin_{},deathHeadPosition_{};
    unsigned effectImpact_=0,effectPhase_=0;
    std::uint32_t effectRandom_=0x464b4658;
    bool effectsReady_=false,effectIntroLanded_=false,effectCollapseLanded_=false;
    struct Receiver {CrossroadsBoss* boss=nullptr;unsigned index=0;};
    std::array<Receiver,objectCapacity> receivers_{};
    static void receive(void*,const NailHit&,HitOutcome);
    void hit(unsigned,HitOutcome);
    void enter(BossPhase,float duration);
    void sync();
    void health(int hp);
    void play(const char*);
    void launchWave();
    void launchRock(float x);
    void defeat();
    void brood();
    void fixedDung(float dt,Hero&);
    float dungClipTime(const char*,float fallback)const;
    void beginDolphin();
    unsigned dungRandom(unsigned bound);
    unsigned chooseDungMove();
    void dungIdle(float delay);
    void throwDung(float direction);
    bool stunPreparation()const{return phase==BossPhase::StunJump||phase==BossPhase::StunLand||
        phase==BossPhase::StunPause||phase==BossPhase::StunOpen;}
    PresentationPosition presentation_;
    CombatObject* objects_=nullptr;
    const AnimationBank* bank_=nullptr;
    Rect arena_{};
    mutable std::array<Solid,7> solids_{};
    std::uint32_t savedFlags_=0;
    Vec2 gruzGeoOrigin_{};
    bool gruzGeoPending_=false;
    unsigned completedStaggers_=0;
    unsigned dungThrows_=0,dungHits_=0,dungCombo_=0,dolphinDives_=0;
    float dungComboTime_=0;
    unsigned dungMove_=0,dungThrowLimit_=2,dungRages_=0;
    int dungLastMove_=-1;
    std::array<unsigned char,5> dungMissed_{};
    std::uint32_t dungRandomState_=0x44d06f31u;
    float dungTunnelTime_=2.5f,dungTunnelSpeed_=15,dungThrowSpeed_=6;
    float timer_=0,elapsed_=0;
    float heroX_=0;
    bool enraged_=false;
    bool dungMayEvade_=false,dungEvaded_=false;
    bool dungIntro_=false,dungFalling_=false;
    bool authoredActivation_=false;
    float titleAge_=-1;
    bool introLanded_=false,corpsePresent_=false;
    bool breakingFloor_=false,finalExposure_=false,recoverToRage_=false;
    int headHp_=40;
    float collapseGround_=0;
    bool collapseLanded_=false;
    bool jumpAttack_=false,slamAttack_=false,slamImpact_=false,smashReposition_=false;
    float smashJumpX_=0;
    float jumpHitterAge_=0;
    unsigned smashCount_=0,jumpAttackCount_=0,normalJumpCount_=0,rageSlams_=0,rocksPending_=0;
    float rageAge_=0,rockTimer_=0;
    std::uint32_t barrelRandom_=0x42415252;
    float barrelRandomUnit(){barrelRandom_^=barrelRandom_<<13;barrelRandom_^=barrelRandom_>>17;barrelRandom_^=barrelRandom_<<5;return float(barrelRandom_&0xffff)/65536.f;}
    std::uint32_t random_=0x464b;
    float randomUnit(){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return float(random_&0xffff)/65536.f;}
    float idleDuration(){return completedStaggers_?.8f+.2f*randomUnit():1.f;}
    float bodyScale_=1;
    Rect activationRegion_{};
};
}
