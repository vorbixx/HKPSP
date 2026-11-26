#include "hero.hpp"
#include "hero_tuning.hpp"

namespace hk3ds {
void Hero::enterSurfaceWater() {
    if(surfaceSwim.active()||vitals.health<=0||!inputEnabled||
       (!controlEnabled&&!superDash.active())||actor==Actor::Damage||actor==Actor::Dead)return;
    // Knight Surface Water FSM c1-23952: Take Control / Enter.
    cancelSuperDash();stopAttack();stopJump();stopDash();
    doubleJumping=wallSliding=wallLocked=bouncing=mushroomBouncing=false;
    airDashUsed=doubleJumpUsed=false;
    ledgeSteps=headSteps=0;
    jumpQueued=doubleQueued=dashQueued=attackQueued=-1;
    recoilDirection=0;focus=FocusPhase::None;focusTime=0;castTime=0;
    lookTime=0;lookUp=lookDown=false;fallTime=0;willHardLand=falling=false;
    contacts={};velocity={};actor=Actor::Airborne;
    // This adapter keeps controlEnabled for the room/transition owner. The
    // dedicated state locks ordinary hero actions while accepting swim input.
    surfaceSwim={SurfaceSwimPhase::Enter,true};
}
void Hero::cancelSurfaceWater() {
    if(surfaceSwim.active()&&(!controlEnabled||!inputEnabled)) {
        velocity={};input={};
    }
    surfaceSwim={};
}
bool Hero::updateSurfaceWater(float dt) {
    if(!surfaceSwim.active())return false;
    if(!controlEnabled||!inputEnabled||actor==Actor::Damage||actor==Actor::Dead) {
        cancelSurfaceWater();return false;
    }
    if(surfaceSwim.phase==SurfaceSwimPhase::JumpFrame) {
        cancelSurfaceWater();
        // Regain Control calls SetStartWithJump -> HeroJumpNoEffect.
        jumping=true;jumpStep=5;jumpedSteps=0;jumpQueued=0;doubleQueued=-1;
        return true;
    }
    if(surfaceSwim.phase==SurfaceSwimPhase::Jump)return true;
    if(surfaceSwim.phase==SurfaceSwimPhase::Enter) {
        // Tk2dPlayAnimationWithEvents waits for Surface In (7 frames at 12fps).
        surfaceSwim.entrySeconds+=dt;
        if(surfaceSwim.entrySeconds+1e-6f<7.f/12)return true;
    }
    if(input.isPressed(Jump)) {
        surfaceSwim.phase=SurfaceSwimPhase::Jump;velocity={0,10};return true;
    }
    const auto next=input.x==0?SurfaceSwimPhase::Idle:SurfaceSwimPhase::Moving;
    if(surfaceSwim.phase!=SurfaceSwimPhase::Enter&&surfaceSwim.phase!=next)
        surfaceSwim.introductory=false;
    surfaceSwim.phase=next;
    if(input.x!=0)facingRight=input.x>0;
    velocity={input.x*5,0};
    return true;
}
void Hero::fixedSurfaceWater(float dt,const World& world) {
    if(surfaceSwim.phase==SurfaceSwimPhase::Jump) {
        const auto body=bodyBounds(position);
        float distance=0;
        // Source RC 1/RC 2 offsets are absent from the selected exports.
        // Approximation: use the two collider top corners for its 1.1-unit rays.
        if(!rayTerrain({body.left,body.top},{0,1},1.1f,world,distance,TerrainShape::terrainQueryExcluded)&&
           !rayTerrain({body.right,body.top},{0,1},1.1f,world,distance,TerrainShape::terrainQueryExcluded))position.y+=1;
        // Jump Out uses non-waiting Tk2dPlayAnimation (looping Airborne),
        // then Translate? and Frame. Velocity was set once by the jump action.
        contacts=moveBody(position,velocity,dt,world);
        surfaceSwim.phase=SurfaceSwimPhase::JumpFrame;
        return;
    }
    if(surfaceSwim.phase==SurfaceSwimPhase::JumpFrame) {
        contacts=moveBody(position,velocity,dt,world);
        return;
    }
    velocity={surfaceSwim.phase==SurfaceSwimPhase::Enter?0.f:input.x*5,0};
    contacts=moveBody(position,velocity,dt,world);
    falling=false;fallTime=0;willHardLand=false;
}
const char* Hero::surfaceWaterClip() const {
    switch(surfaceSwim.phase) {
    case SurfaceSwimPhase::Enter:return "Surface In";
    case SurfaceSwimPhase::Idle:return surfaceSwim.introductory?"Surface InToIdle":"Surface Idle";
    case SurfaceSwimPhase::Moving:return surfaceSwim.introductory?"Surface InToSwim":"Surface Swim";
    case SurfaceSwimPhase::Jump:
    case SurfaceSwimPhase::JumpFrame:return "Airborne";
    default:return nullptr;
    }
}
}
