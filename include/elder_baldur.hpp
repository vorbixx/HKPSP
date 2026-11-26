#pragma once
#include "enemy.hpp"
#include <array>

namespace hk3ds {
// Crossroads_11_alt level50: Blocker Control 4354, HealthManager 4585,
// body 2939, alert 2935/2119, attack range 2938/2117. Stationary by design.
class ElderBaldur {
    friend struct SimulationStateHash;
public:
    float artScale()const{return animationFacingScale("elder-baldur/",-scaleX,std::abs(scaleX));}
    enum class Phase { Dormant, Open, Idle, Close1, Close2, Closed, Hit, ShootAntic, ShootCooldown, Dead };
    bool bind(CombatObject&,const AnimationBank&,Vec2,float scaleX,EnemyRandom,CombatObject* rollerObject=nullptr);
    void fixedUpdate(float,Hero&,const World& world={});
    void update(float);
    bool alive()const{return object_&&!object_->health.dead;}
    bool bound()const{return object_!=nullptr;}
    bool takeDeath(){const bool result=death_;death_=false;return result;}
    Vec2 position{};
    float scaleX=1;
    AnimationPlayer animation;
    Phase phase=Phase::Dormant;
    static constexpr Rect body{-.828125f,-2.21875f,1.953125f,.9375f};
    struct Bullet {
        bool active=false,impact=false;
        Vec2 position{},velocity{};
        float scale=1,rotation=0;
        AnimationPlayer animation;
        PresentationPosition presentation;
    };
    std::array<Bullet,4> bullets{};
    enum class RollerPhase { Gone, Air, Start, Roll, Stop, Rest, Dead };
    struct Roller {
        float artScale()const{return animationFacingScale("elder-baldur/Roller",-scaleX,std::abs(scaleX));}
        RollerPhase phase=RollerPhase::Gone;
        Vec2 position{},velocity{};
        float scaleX=1,timer=0;
        AnimationPlayer animation;
        PresentationPosition presentation;
    } roller;
private:
    CombatObject* object_=nullptr;
    const AnimationBank* bank_=nullptr;
    EnemyRandom random_{};
    CombatObject* rollerObject_=nullptr;
    float timer_=0;
    bool death_=false,sleeping_=false,spawnRoller_=false;
    bool inRange(const Hero&,bool attack)const;
    void enter(Phase,const char*);
    static void receive(void*,const NailHit&,HitOutcome);
    static void receiveRoller(void*,const NailHit&,HitOutcome);
    void fire();
    void attacks(float,Hero&,const World&);
    void rollerEnter(RollerPhase,const char*);
};
static_assert(sizeof(ElderBaldur)<=2048,"Elder Baldur fixed state budget");
}
