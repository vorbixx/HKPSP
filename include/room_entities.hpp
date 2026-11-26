#pragma once
#include "enemy.hpp"
#include <cstdint>
namespace hk3ds {
struct BuzzerConfig {
 Vec2 spawn{}; Rect body{}, heroHurtbox{}; unsigned hp=0, damage=0, layer=0, smallGeo=0;
 float alertRadius=0, alertOffsetY=0, idleMin=0, idleMax=0, idleSpeed=0, idleAcceleration=0, roamingRange=0;
 float chaseSpeed=0, chaseAcceleration=0, attentionSpan=0, startleDuration=0, recoilSpeed=0, recoilDuration=0, stopDeceleration=0, stopDuration=0;
};
struct StalactiteConfig {
 Vec2 spawn{}; Rect trigger{}, heroHurtbox{}; unsigned layer=0, vertexCount=0;
 std::array<Vec2,16> vertices{}; float fallOffset=0, fallDelay=0, hitSpeed=0, gravity=0;
 unsigned enemyDamage=0, embeddedLayer=0, embeddedVertexCount=0;
 Vec2 embeddedOffset{}; std::array<Vec2,16> embeddedVertices{};
 // Optional native trigger landing. A zero step keeps the terrain sweep pose.
 float verticalRestY=0, landingStep=0;
};
// RME1: magic/counts; Buzzer 25 float32 + 4 uint32; stalactite
// 16 float32 + 5 uint32 + falling/embedded vertices. All little endian.
// Optional SLP1 suffix: count, then (actor index, rest Y, native fixed step).
// Counts and floating values are validated before replacing the loaded bank.
struct RoomEntityBank {
 static constexpr unsigned maxBuzzers=16, maxStalactites=32;
 std::array<BuzzerConfig,maxBuzzers> buzzers{};
 std::array<StalactiteConfig,maxStalactites> stalactites{};
 unsigned buzzerCount=0, stalactiteCount=0;
 bool load(const char* path);
};
enum class BuzzerState { Idle, Startle, ChaseInSight, ChaseOutOfSight, Stop, Dead };
class Buzzer {
    friend struct SimulationStateHash;
public:
 float artScale()const;
 bool bind(const BuzzerConfig&, CombatObject&, EnemyRandom, const AnimationBank* = nullptr);
 void update(float dt, const Hero&, const World&);
 void fixedUpdate(float dt,const Hero&,const World&);
 void contactHero(Hero&) const;
 Rect bounds() const;
 bool alive() const { return object_ && !object_->health.dead; }
 Vec2 renderPosition(PresentationTime time) const { return presentation_.sample(position,time); }
 Vec2 position{},velocity{}; BuzzerState state=BuzzerState::Idle;
 AnimationPlayer animation; float scaleX=1;
 bool killFreezeRequested=false; unsigned geoReleased=0;
 std::uint32_t injurySerial=0, killSerial=0, startleSerial=0, corpseSplatSerial=0;
private:
 static void receive(void*,const NailHit&,HitOutcome);
 void hit(const NailHit&,HitOutcome); void enter(BuzzerState);
 void buzz(float); void chase(const Hero&); void sync();
 void face(float dt,const Hero&);
 const AnimationBank* animations_=nullptr;
 std::array<float,3> turnPause_{}; bool faceHero_=false;
 PresentationPosition presentation_;
 BuzzerConfig config_{}; CombatObject* object_=nullptr; EnemyRandom random_{};
 bool soundCorpseActive_=false; Vec2 soundCorpsePosition_{},soundCorpseVelocity_{};
 Vec2 idleOrigin_{},acceleration_{},recoilDirection_{};
 float timer_=0,wait_=0,recoil_=0,recoilMagnitude_=1; bool sees_=false,recoilSweeping_=false;
};
enum class StalactiteState { Hanging, Warning, Falling, Embedded, Broken };
// Cosmetic serials preserve transitions even when several physics steps precede a draw.
enum class StalactiteCue : unsigned { Start, Fall, Land, Hit, Break, UpBreak, Count };
class Stalactite {
    friend struct SimulationStateHash;
public:
 bool bind(const StalactiteConfig&,CombatObject&);
 void update(float dt,const Hero&,const World&);
 void fixedUpdate(float dt,const World&);
 // Bounds are broadphase only: hero/enemy contacts use the rotated polygon.
 void contactHero(Hero&) const;
 // Callback adapter may send a layer-8 trigger enter independently of sweep.
 void terrainEnter();
 bool active() const { return object_ && state!=StalactiteState::Broken; }
 Rect bounds() const; bool overlapsBody(Rect) const;
 Vec2 renderPosition(PresentationTime time) const { return presentation_.sample(position,time); }
 Vec2 position{},velocity{}; float rotation=0;
 StalactiteState state=StalactiteState::Hanging;
 bool damagesHero=false,damagesEnemies=false;
 std::array<unsigned,static_cast<unsigned>(StalactiteCue::Count)> cues{};
 Strike lastStrike=Strike::Down;
 unsigned enemyDamage() const { return config_.enemyDamage; }
private:
 static void receive(void*,const NailHit&,HitOutcome);
 void nail(const NailHit&); void fall(float); void sync();
 std::size_t polygon(Vec2*) const;
 PresentationPosition presentation_;
 StalactiteConfig config_{}; CombatObject* object_=nullptr;
 float timer_=0; bool gravity_=true,heroDamageRemoved_=false,triggerInside_=false;
};
}
