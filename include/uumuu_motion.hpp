#pragma once
#include "presentation.hpp"

namespace hk3ds {
struct UumuuAttackEvents;
struct EnemyRandom;
struct World;
enum class Strike;
enum class UumuuShellPhase;
enum class UumuuMotionMode { Coast,Idle,Chase,Dampen,Wounded };

// Authored ChaseObjectV2/DecelerateV2 actions with a bounded free-body step.
// The owning actor supplies state entries, collision callbacks/recoil and bounds AWAKE.
// Burst/iTween and Quirrel's corner placement are not inferred here.
class UumuuMotion {
    friend struct SimulationStateHash;
public:
    bool bind(Vec2 position,Vec2 velocity={});
    // Execute OnEnter even when the new mode equals the previous mode.
    bool enter(UumuuMotionMode,Vec2 heroPosition);
    bool setVelocity(Vec2); // Owning collision/recoil adapter writes body velocity.
    bool applyRecoil(Vec2 position); // Fixed physics correction; retain presentation history.
    bool place(Vec2); // Authored SetPosition: preserve inertia, reset presentation.
    bool onAttackEvents(const UumuuAttackEvents&,Vec2 heroPosition);
    void fixedUpdate(float dt,Vec2 heroPosition);
    // ObjectBounce is a separate FixedUpdate callback. The room adapter calls
    // this once per physics tick; native callback ordering remains unadmitted.
    void sampleBounce();
    void stopBounce(){bouncing_=false;}
    void startBounce(){bouncing_=true;}
    // Call only for an actual collision-enter, using GetSafeContact's normal.
    // A true result corresponds to the source OnBounce event, even at zero
    // sampled displacement. Uumuu configures no sound, animation or FSM event.
    bool collisionEnter(Vec2 normal,EnemyRandom);
    void awakeBounds();
    // Bounds is an Update FSM, separate from the fixed physics callback.
    void updateBounds();
    Vec2 position()const{return position_;}
    Vec2 velocity()const{return velocity_;}
    Vec2 renderPosition(PresentationTime t)const{return presentation_.sample(position_,t);}
    UumuuMotionMode mode()const{return mode_;}
    bool boundsAwake()const{return boundsAwake_;}
private:
    void actions(Vec2 heroPosition);
    Vec2 position_{},velocity_{},force_{};
    PresentationPosition presentation_;
    UumuuMotionMode mode_=UumuuMotionMode::Coast;
    bool boundsAwake_=false;
    Vec2 bounceLastPosition_{},bounceDisplacement_{};
    float bounceSpeed_=0;
    unsigned bounceStep_=0;
    bool bouncing_=true;
};

// Recoil165's independent FixedUpdate callback. The actor supplies the captured
// collider shape and its live interpolated Transform, as for translateRecoil.
// This component never replaces Rigidbody velocity or chooses hit acceptance.
class UumuuRecoil {
    friend struct SimulationStateHash;
public:
    UumuuRecoil();
    bool onShellPhase(UumuuShellPhase); // Wounded/Recover SetRecoilSpeed actions.
    // Notify after becoming active, before the initial zero-delta sweep. The
    // actor sends RECOIL HORIZONTAL (horizontal only), then HIT <direction>.
    bool begin(Strike,float magnitude,Rect capturedBox,Vec2& body,Vec2& transform,const World&,
               void* receiver=nullptr,void (*started)(void*,Strike)=nullptr);
    // True corresponds to OnCancelRecoil. Collision stops translation while the
    // authored timer continues; another hit is ignored until cancellation.
    bool fixedUpdate(float dt,Vec2& body,Vec2& transform,const World&);
    bool cancel(); // Also the source OnEnable callback; preserves configured speed.
    bool active()const{return active_;}
private:
    bool updatePhysics(float dt,Vec2& body,Vec2& transform,const World&);
    Rect box_{};
    Vec2 direction_{};
    float baseSpeed_=0,speed_=0,time_=0;
    bool active_=false,sweeping_=false;
};
}
