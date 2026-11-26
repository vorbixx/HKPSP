#pragma once
#include "room_entities.hpp"
#include "audio.hpp"

namespace hk3ds {
enum class CrossroadsEnemyKind : unsigned { Tiktik, Gruzzer, Vengefly, AspidHatchling };
struct CrossroadsEnemyEntry {
    const char* prefab;
    const char* assetStem;
    CrossroadsEnemyKind kind;
};
// Append entries; serialized spawn data uses the original prefab name.
inline constexpr CrossroadsEnemyEntry crossroadsEnemyRegistry[]={
    {"Climber","tiktik",CrossroadsEnemyKind::Tiktik},
    {"Fly","gruzzer",CrossroadsEnemyKind::Gruzzer},
    {"Buzzer","vengefly",CrossroadsEnemyKind::Vengefly},
    {"Hatcher Baby","aspid-hatchling",CrossroadsEnemyKind::AspidHatchling}
};
const CrossroadsEnemyEntry* findCrossroadsEnemy(const char* prefab);
struct CrossroadsEnemyConfig {
    CrossroadsEnemyKind kind=CrossroadsEnemyKind::Tiktik;
    BuzzerConfig flight{};
    Rect corpse{};
    float speed=0,acceleration=0,corpseGravity=0,flingSpeed=0;
    // XEA1: kind/HP/damage/layer/Geo uint32, RME1's 25 flight floats,
    // corpse rect and speed/acceleration/corpse gravity/fling speed float32.
    bool load(const char* path);
};
class CrossroadsEnemy {
    friend struct SimulationStateHash;
public:
    bool bind(const CrossroadsEnemyConfig&,const AnimationBank&,CombatObject&,
              Vec2 spawn,const World&,EnemyRandom,float facing=1);
    void update(float dt,const Hero&,const World&);
    void fixedUpdate(float dt,const Hero&,const World&);
    void contactHero(Hero&) const;
    bool active()const{return object_!=nullptr;}
    bool alive()const{return active()&&!object_->health.dead;}
    bool visible()const{return alive()||corpseTime_>0;}
    Rect bounds()const;
    float artScale()const;
    Vec2 renderPosition(PresentationTime time)const;
    const AnimationPlayer& animation()const;
    ActorAudioState audioState(unsigned owner,Vec2 listener)const;
    Vec2 position{},velocity{};
    float scaleX=1,rotation=0;
    unsigned geoReleased=0;
    Vec2 geoPosition{};
    float geoRotation=0;
    std::uint32_t injurySerial=0,killSerial=0,corpseSplatSerial=0;
    bool killFreezeRequested=false;
private:
    static void receive(void*,const NailHit&,HitOutcome);
    void hit(const NailHit&,HitOutcome);
    void startCorpse(Strike);
    void sync();
    void surfaceUpdate(float,const World&);
    void gruzzerDirection(float angle);
    void gruzzerBounce(unsigned side);
    void gruzzerTouching(const World&,bool stay,bool deferred);
    void gruzzerContacts(const Contacts&,const World&);
    CrossroadsEnemyConfig config_{};
    const AnimationBank* bank_=nullptr;
    CombatObject* object_=nullptr;
    EnemyRandom random_{};
    Buzzer buzzer_;
    HatchlingSteering hatchling_;
    AnimationPlayer animation_;
    PresentationPosition presentation_,corpsePresentation_;
    Vec2 corpsePosition_{},corpseVelocity_{},surfaceNormal_{0,1};
    Vec2 turnFrom_{},turnTo_{},nextNormal_{};
    Vec2 previousSurfacePosition_{},previousTurnPosition_{};
    float recoil_=0,corpseTime_=0;
    float flightAngle_=0;
    Vec2 flightVelocity_{};
    // Layer-8 CheckCollisionSide flags survive Fly 2 re-entry. Collider IDs
    // distinguish Enter from Stay; multiple manifold normals are one callback.
    std::array<std::uint32_t,16> gruzzerContacts_{};
    unsigned gruzzerContactCount_=0,gruzzerStaySides_=0;
    float turnTime_=0,turnStartRotation_=0,turnDeltaRotation_=0;
    bool turning_=false;
    bool corpseLanded_=false;
};
}
