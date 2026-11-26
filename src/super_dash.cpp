#include "hero.hpp"
#include "hero_tuning.hpp"
#include <cmath>

namespace hk3ds {
namespace {
void clearMotion(Hero& h) {
    h.stopAttack();h.stopJump();h.stopDash();
    h.doubleJumping=h.wallSliding=h.wallLocked=h.bouncing=h.mushroomBouncing=false;
    h.jumpQueued=h.doubleQueued=h.dashQueued=h.attackQueued=-1;
    h.recoilDirection=0;h.focus=FocusPhase::None;h.focusTime=0;
    h.lookTime=0;h.lookUp=h.lookDown=false;h.fallTime=0;h.willHardLand=false;
}
bool wallSupport(const Hero& h,const World& world) {
    float distance=0;
    const auto ray=[&](float y){return rayTerrain({h.position.x,h.position.y+y},
        {float(h.superDash.wall),0},super_dash::wallProbeLength,world,distance,
        TerrainShape::terrainQueryExcluded);};
    // Knight Superdash Wall Charge: authored children, self-space direction,
    // repeatInterval=1, layer 8. These are queries, not body contacts.
    const bool top=ray(super_dash::supportTopY),bottom=ray(super_dash::supportBottomY);
    return top&&bottom;
}
bool groundSupport(Hero& h,const World& world) {
    float distance=0;
    h.superDash.groundProbeHit=rayTerrain(h.position,{0,-1},super_dash::groundProbeLength,
        world,distance,TerrainShape::terrainQueryExcluded);
    h.superDash.groundProbeFrame=2;
    return h.superDash.groundProbeHit;
}
}
bool Hero::canSuperDash() const {
    return !surfaceSwim.active()&&abilities.superDash&&inputEnabled&&controlEnabled&&!superDash.active()&&
        actor!=Actor::Dead&&actor!=Actor::Damage&&actor!=Actor::HardLanding&&castTime<=0&&
        !dashing&&freezePhase==0&&recoilDirection==0&&
        (!nail.active||nail.elapsed>=tuning::attackRecovery)&&(contacts.ground||wallSliding);
}
void Hero::startSuperDash() {
    if(!canSuperDash())return;
    const int wall=contacts.ground?0:wallSide;
    clearMotion(*this);superDash={};superDash.phase=SuperDashPhase::Charging;
    superDash.wall=wall;superDash.direction=wall?-wall:(facingRight?1:-1);
    velocity={};controlEnabled=false;gravityEnabled=wall==0;
}
void Hero::resumeSuperDash(int direction) {
    if(!abilities.superDash||!direction)return;
    clearMotion(*this);superDash={};superDash.phase=SuperDashPhase::Flying;
    superDash.direction=direction>0?1:-1;facingRight=direction>0;
    velocity={superDash.direction*super_dash::speed,0};
    controlEnabled=false;gravityEnabled=false;
}
void Hero::cancelSuperDash() {
    if(!superDash.active())return;
    superDash={};controlEnabled=true;gravityEnabled=true;
}
bool Hero::updateSuperDash(float dt,const World& world) {
    if(!superDash.active()) {
        if(input.isPressed(SuperDash)&&canSuperDash()){
            startSuperDash();
            if(superDash.wall&&!wallSupport(*this,world))cancelSuperDash();
            return true;
        }
        return false;
    }
    if(!inputEnabled||vitals.health<=0||actor==Actor::Dead||actor==Actor::Damage) {
        const bool control=controlEnabled;cancelSuperDash();controlEnabled=control;return true;
    }
    superDash.elapsed+=dt;
    if(superDash.phase==SuperDashPhase::Charging||superDash.phase==SuperDashPhase::Charged) {
        const bool charging=superDash.phase==SuperDashPhase::Charging;
        // c1-23052: Wall Charged listens only for release. The two support
        // BoolTests belong to Wall Charge, not to the fully charged state.
        if(!charging&&!superDash.wall){
            if(superDash.groundProbeFrame)--superDash.groundProbeFrame;
            if(!superDash.groundProbeFrame)groundSupport(*this,world);
        }
        const bool support=superDash.wall?(!charging||wallSupport(*this,world)):
            (charging?velocity.y>=-.1f:superDash.groundProbeHit);
        // ListenForSuperdash precedes Wait in both charge states: a release
        // on the update that would complete the timer still cancels the charge.
        if(!input.isHeld(SuperDash)||!support) {
            if(superDash.phase==SuperDashPhase::Charged){
                // Direction / Direction Wall read HeroController.GetState at
                // release; wall launches invert that current facing direction.
                const int facing=facingRight?1:-1;
                resumeSuperDash(superDash.wall?-facing:facing);
            }
            else {velocity.x=0;cancelSuperDash();}
        }
        else if(charging&&superDash.elapsed>=super_dash::charge){
            superDash.phase=SuperDashPhase::Charged;
            // RayCast2d.OnEnter casts immediately; OnUpdate then decrements its
            // repeat counter. A lost floor launches on entry to Ground Charged.
            if(!superDash.wall&&!groundSupport(*this,world))resumeSuperDash(facingRight?1:-1);
        }
    } else if(superDash.flying()) {
        if(superDash.elapsed>=super_dash::cancelable&&(input.isPressed(Jump)||input.isPressed(SuperDash))) {
            superDash.phase=SuperDashPhase::Braking;superDash.elapsed=0;
        }
    } else if(superDash.elapsed>=(superDash.phase==SuperDashPhase::Braking?super_dash::braking:super_dash::wallRecovery)) {
        const int wall=superDash.phase==SuperDashPhase::WallRecovery?superDash.wall:0;
        cancelSuperDash();
        if(wall&&abilities.wallJump&&!contacts.nonSlider&&!contacts.ground) {
            wallSliding=true;wallSide=wall;facingRight=wall>0;
        }
    }
    return true;
}
void Hero::fixedSuperDash(float dt,const World& world) {
    const auto phase=superDash.phase;
    if(phase==SuperDashPhase::Charging||phase==SuperDashPhase::Charged) {
        velocity={};gravityEnabled=superDash.wall==0;
        if(gravityEnabled)velocity.y=tuning::gravity*tuning::gravityScale*dt;
    } else if(phase==SuperDashPhase::Flying) {
        gravityEnabled=false;velocity={superDash.direction*super_dash::speed,0};
    } else if(phase==SuperDashPhase::Braking) {
        gravityEnabled=false;velocity.x*=super_dash::deceleration;velocity.y*=super_dash::deceleration;
    } else {gravityEnabled=false;velocity={};}
    contacts=moveBody(position,velocity,dt,world);
    falling=false;fallTime=0;willHardLand=false;
    actor=contacts.ground?Actor::Idle:Actor::Airborne;
    if(phase!=SuperDashPhase::Flying)return;
    if((superDash.direction<0&&contacts.left)||(superDash.direction>0&&contacts.right)) {
        superDash.phase=SuperDashPhase::WallRecovery;superDash.elapsed=0;
        superDash.wall=superDash.direction;velocity={};
    } else {
        if(std::abs(velocity.x)<=.1f)superDash.stationary+=dt;
        if(superDash.stationary>super_dash::stalled) {
            superDash.phase=SuperDashPhase::Braking;superDash.elapsed=0;
        }
    }
}
}
