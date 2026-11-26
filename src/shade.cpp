#include "shade.hpp"
#include "render_memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds {
static_assert(sizeof(HollowShade)<=shadePresentationStateBudgetBytes,"Shade presentation ordinary memory budget");
static_assert(sizeof(ShadeEmitter)==12*sizeof(float),"Shade emitter wire layout");
namespace {
float length(Vec2 p){return std::sqrt(p.x*p.x+p.y*p.y);}
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
}
bool ShadeConfig::load(const char* path){
    auto* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4];ShadeConfig next;
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"HKS2",4)&&
        std::fread(&next,sizeof(next),1,f)==1&&std::fgetc(f)==EOF;std::fclose(f);
    const auto valid=[](Rect r){return std::isfinite(r.left)&&std::isfinite(r.right)&&std::isfinite(r.bottom)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;};
    ok=ok&&valid(next.body)&&valid(next.alert)&&valid(next.slash);
    for(auto p:next.markers)ok=ok&&std::isfinite(p.x)&&std::isfinite(p.y);
    for(auto p:next.slashPoints)ok=ok&&std::isfinite(p.x)&&std::isfinite(p.y);
    for(const auto& e:next.emitters)ok=ok&&std::isfinite(e.rate)&&e.rate>=0&&e.rate<=3000&&
        std::isfinite(e.lifeMin)&&std::isfinite(e.lifeMax)&&e.lifeMin>0&&e.lifeMax>=e.lifeMin&&e.lifeMax<=2&&
        std::isfinite(e.speedMin)&&std::isfinite(e.speedMax)&&e.speedMin>=0&&e.speedMax>=e.speedMin&&e.speedMax<=80&&
        std::isfinite(e.forceMin)&&std::isfinite(e.forceMax)&&e.forceMin>=0&&e.forceMax>=e.forceMin&&e.forceMax<=30&&
        std::isfinite(e.x)&&std::isfinite(e.y)&&std::isfinite(e.radius)&&e.radius>0&&e.radius<=2&&std::isfinite(e.size)&&e.size>0&&e.size<=1&&
        std::isfinite(e.duration)&&e.duration>0&&e.duration<=1;
    if(ok)*this=next;
    return ok;
}
Vec2 ShadeConfig::spawnPosition(Vec2 death) const {
    float nearest=1e30f;Vec2 result=death;
    for(auto marker:markers){const float d=length({death.x-marker.x,death.y-marker.y});if(d<nearest){nearest=d;result=marker;}}
    return result;
}
void HollowShade::clear(){if(object_)object_->colliderEnabled=false;state=ShadeState::Absent;velocity={};animation={};slashAnimation={};glowAnimation={};deathSprite={};particles_={};emissionRemainder_=waveRemaining_=waveRemainder_=hitRemaining_=hitRemainder_=0;hitAge_=1;}
void HollowShade::bind(const ShadeConfig& config,const AnimationBank* bank,CombatObject& object,Vec2 spawn,int hp,EnemyRandom random){
    *this={};config_=config;bank_=bank;object_=&object;random_=random;position=origin=spawn;presentation_.reset(position);
    particleSeed_^=unsigned(std::abs(spawn.x)*997)^unsigned(std::abs(spawn.y)*991);
    object={};object.name="Hollow Shade";object.hasHealth=true;object.health.hp=hp;object.health.enemyType=3;
    object.receiver=this;object.onHit=hit;enter(ShadeState::Idle);sync();
}
void HollowShade::enter(ShadeState next){
    state=next;timer_=0;const char* clip=nullptr;duration_=0;
    switch(next){
    case ShadeState::Idle:clip="Idle";velocity={};break;
    case ShadeState::Startle:clip="Startle";++startleSerial;break;
    case ShadeState::Fly:clip="Fly";duration_=random_.range(1,2);break;
    case ShadeState::Position:clip="Fly";duration_=6;break;
    case ShadeState::SlashAntic:clip="Slash Antic";velocity={};++slashSerial;break;
    case ShadeState::Slash:clip="Slash";slashAnimation.play(bank_?bank_->require("Slash Effect"):nullptr,0,true);duration_=.083f;break;
    case ShadeState::SlashEnd:break;
    case ShadeState::SlashCooldown:clip="Slash CD";break;
    case ShadeState::RetreatStart:clip="Retreat Start";break;
    case ShadeState::Retreat:retreatPose_=retreatFrom_=position;retreatPresentation_.reset(retreatPose_);velocity={};duration_=1;break;
    case ShadeState::RetreatEnd:clip="Retreat End";break;
    case ShadeState::Recover:clip="Death Start";duration_=.5f;velocity={};++recoverySerial;
        for(auto& p:particles_)if(p.emitter<GhostOne)p={};
        emissionRemainder_=0;waveRemaining_=config_.emitters[3].rate>0?config_.emitters[3].duration:0;break;
    case ShadeState::RecoveryBlow:clip="Death";++blowSerial;glowAnimation.play(bank_?bank_->require("Death Glow"):nullptr,0,true);deathSprite.play(bank_?bank_->require("Death"):nullptr,0,true);break;
    case ShadeState::RecoveryEnd:duration_=.75f;break;
    case ShadeState::RecoveryDissipate:duration_=1;break;
    default:break;
    }
    if(clip){animation.play(bank_?bank_->require(clip):nullptr,0,true);if(!duration_)duration_=animation.clip?animation.clip->count/animation.clip->fps:.25f;}
    sync();
}
void HollowShade::sync(){if(!object_)return;object_->bounds=bounds();object_->colliderEnabled=alive()&&state!=ShadeState::RetreatStart&&state!=ShadeState::Retreat&&state!=ShadeState::RetreatEnd;}
float HollowShade::particleRandom(){particleSeed_=particleSeed_*1664525u+1013904223u;return float(particleSeed_>>8)/16777216.f;}
const char* HollowShade::Particle::hitClip()const{
    switch(emitter){case GhostOne:return "Hit Ghost 1";case GhostTwo:return "Hit Ghost 2";case HitSlash:return "Hit Slash";case HitFlash:return "Hit Flash";default:return "Shade Particle";}
}
float HollowShade::Particle::hitScaleX()const{return emitter==GhostOne||emitter==GhostTwo?2:emitter==HitSlash?mix:emitter==HitMote?size():1;}
float HollowShade::Particle::hitScaleY()const{return emitter==GhostOne||emitter==GhostTwo?2:emitter==HitMote?size():1;}
float HollowShade::Particle::hitRotation()const{return emitter==GhostOne||emitter==GhostTwo?std::atan2(velocity.y,velocity.x):emitter==HitSlash?force:0;}
float HollowShade::Particle::hitOpacity()const{return emitter==HitFlash?std::max(0.f,1-age/.5f):1;}
float HollowShade::Particle::size()const{
    const float t=std::clamp(age/lifetime,0.f,1.f),t2=t*t,t3=t2*t;
    // Native SizeModule's two Hermite curves. The minimum reaches zero at
    // .9712988; the maximum is (0,1,tangent0) -> (1,0,tangent-2).
    const float u=std::min(1.f,t/.9712988f),u2=u*u,u3=u2*u;
    const float low=std::max(0.f,(2*u3-3*u2+1)*.5675675f+(u3-u2)*.9712988f*-1.5089992f);
    const float high=std::max(0.f,2*t3-3*t2+1-2*(t3-t2));
    return low+(high-low)*mix;
}
void HollowShade::presentationFixedUpdate(float dt){
    if(!visible()||dt<=0)return;
    hitAge_=std::min(1.f,hitAge_+dt);
    for(auto& p:particles_)if(p.occupied()){
        PresentationPosition::Step step(p.history,p.position);
        p.age+=dt;
        if(p.emitter!=HitSlash&&p.emitter!=HitFlash){
            if(p.emitter==3){
                const float speed=length(p.velocity);
                if(speed>20){const float factor=(20+(speed-20)*.2f)/speed;p.velocity.x*=factor;p.velocity.y*=factor;}
            }
            p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt+.5f*p.force*dt*dt;p.velocity.y+=p.force*dt;
        }
    }
    if(hitRemaining_>0){
        // Native Hit Shade: constant rate450 for .1s, not the unused min10.
        hitRemainder_+=450*std::min(dt,hitRemaining_);
        const unsigned count=unsigned(hitRemainder_+1e-4f);hitRemainder_-=count;
        emitHitMotes(count);hitRemaining_=std::max(0.f,hitRemaining_-dt);
    }
    if(waveRemaining_>0){
        waveRemainder_+=config_.emitters[3].rate*std::min(dt,waveRemaining_);
        const unsigned count=unsigned(waveRemainder_+1e-4f);waveRemainder_-=count;
        emit(3,count);waveRemaining_=std::max(0.f,waveRemaining_-dt);
    }
    const unsigned emitter=state==ShadeState::Retreat?1:alive()?0:2;
    if(!alive()&&state!=ShadeState::Recover&&state!=ShadeState::RecoveryBlow){emissionRemainder_=0;return;}
    const auto& e=config_.emitters[emitter];
    emissionRemainder_+=e.rate*dt;
    const unsigned count=unsigned(emissionRemainder_+1e-5f);emissionRemainder_-=count;emit(emitter,count);
}
HollowShade::Particle* HollowShade::allocate(unsigned kind){
    unsigned tries=0;
    while(tries<particleCapacity&&particles_[particleCursor_].occupied()){particleCursor_=(particleCursor_+1)%particleCapacity;++tries;}
    if(tries==particleCapacity){++droppedParticles_;return nullptr;}
    auto& p=particles_[particleCursor_];particleCursor_=(particleCursor_+1)%particleCapacity;
    p={};p.emitter=kind;return &p;
}
void HollowShade::emit(unsigned emitter,unsigned count){
    const auto& e=config_.emitters[emitter];
    for(unsigned i=0;i<count;++i){
        auto* slot=allocate(emitter);if(!slot)continue;auto& p=*slot;
        const float angle=particleRandom()*6.28318530718f,radius=std::sqrt(particleRandom())*e.radius;
        const float speed=e.speedMin+(e.speedMax-e.speedMin)*particleRandom();
        p={};p.emitter=emitter;p.position={e.x+std::cos(angle)*radius,e.y+std::sin(angle)*radius};
        p.history.reset(p.position);p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
        p.lifetime=e.lifeMin+(e.lifeMax-e.lifeMin)*particleRandom();
        p.force=e.forceMin+(e.forceMax-e.forceMin)*particleRandom();p.mix=particleRandom();++emitted_[emitter];
    }
}
void HollowShade::emitHit(Strike direction){
    if(hitAge_==0)return; // EnemyHitEffectsShade.didFireThisFrame cosmetic gate.
    constexpr float rad=3.14159265358979323846f/180;
    hitAge_=0;hitOrigin_=position;hitRemaining_=.1f;hitRemainder_=0;
    hitAngle_=direction==Strike::Left?180:direction==Strike::Up?90:direction==Strike::Down?270:0;
    for(unsigned kind:{unsigned(GhostOne),unsigned(GhostTwo)}){
        const unsigned count=2+unsigned(particleRandom()*2);
        for(unsigned i=0;i<count;++i){
            const float speed=20+15*particleRandom(),angle=(hitAngle_-40+80*particleRandom())*rad;
            if(auto* p=allocate(kind)){p->position=position;p->history.reset(position);p->velocity={std::cos(angle)*speed,std::sin(angle)*speed};p->lifetime=(kind==GhostOne?7.f:6.f)/18;}
        }
    }
    for(unsigned i=0;i<3;++i){
        const float scale=(direction==Strike::Left?-1.f:1.f)*(1+.75f*particleRandom());
        const float rotation=(direction==Strike::Down?-60-60*particleRandom():direction==Strike::Up?60+60*particleRandom():-30+60*particleRandom())*rad;
        if(auto* p=allocate(HitSlash)){p->position=position;p->history.reset(position);p->lifetime=4.f/25;p->mix=scale;p->force=rotation;}
    }
    if(auto* p=allocate(HitFlash)){p->position=position;p->history.reset(position);p->lifetime=.5f;}
}
void HollowShade::emitHitMotes(unsigned count){
    constexpr float rad=3.14159265358979323846f/180;
    // Approximation: project the source 3D cone (angle25, radius.5) into
    // the gameplay plane; native RNG and depth distribution are not reproduced.
    for(unsigned i=0;i<count;++i){
        auto* p=allocate(HitMote);if(!p)continue;
        const float angle=(hitAngle_-25+50*particleRandom())*rad,speed=10+120*particleRandom();
        const float offset=particleRandom()-.5f,axis=hitAngle_*rad;
        p->position={hitOrigin_.x-std::sin(axis)*offset,hitOrigin_.y+std::cos(axis)*offset};p->history.reset(p->position);
        p->velocity={std::cos(angle)*speed,std::sin(angle)*speed};p->lifetime=.5f+.9f*particleRandom();p->force=5+15*particleRandom();p->mix=particleRandom();
    }
}
void HollowShade::update(float dt,const Hero& hero,const World& world){
    if(!visible()||dt<=0)return;
    timer_+=dt;animation.advance(dt);slashAnimation.advance(dt);
    const Vec2 delta{hero.position.x-position.x,hero.position.y-position.y};
    if(state==ShadeState::Idle){
        const auto alert=boxBounds(position,config_.alert);float distance=0;const float l=length(delta);
        if(hero.actor!=Actor::Dead&&l<config_.alert.right&&overlap(alert,bodyBounds(hero.position))&&(l==0||!rayTerrain(position,{delta.x/l,delta.y/l},l,world,distance)))enter(ShadeState::Startle);
    }else if(state==ShadeState::Fly||state==ShadeState::Position){
        const float facing=delta.x>0?-1.0f:1.0f;
        // FaceObject.DoFace resets TurnToFly only when the facing changes.
        // Its source LoopSection retains the flying tail after the turn.
        if(facing!=scaleX){scaleX=facing;animation.play(bank_?bank_->require("TurnToFly"):nullptr,0,true);}
        if(length({position.x-origin.x,position.y-origin.y})>25)enter(ShadeState::RetreatStart);
        else if(state==ShadeState::Fly&&timer_>=duration_)enter(ShadeState::Position);
        else if(state==ShadeState::Position&&length(delta)<5&&std::abs(delta.y)<=.2f)enter(ShadeState::SlashAntic);
        else if(state==ShadeState::Position&&timer_>=duration_)enter(ShadeState::Fly);
    }else if(state==ShadeState::Slash&&timer_>=.083f)enter(ShadeState::SlashEnd);
    else if(state==ShadeState::SlashEnd&&animation.complete)enter(ShadeState::SlashCooldown);
    else if(timer_>=duration_){
        switch(state){
        case ShadeState::Startle:enter(ShadeState::Fly);break;
        case ShadeState::SlashAntic:scaleX=delta.x>0?-1.0f:1.0f;enter(ShadeState::Slash);break;
        case ShadeState::SlashCooldown:enter(ShadeState::Fly);break;
        case ShadeState::RetreatStart:enter(ShadeState::Retreat);break;
        case ShadeState::Retreat:position=origin;presentation_.reset(position);enter(ShadeState::RetreatEnd);break;
        case ShadeState::RetreatEnd:enter(ShadeState::Idle);break;
        // Source Blow has no wait: animation and glow continue during End.
        case ShadeState::Recover:enter(ShadeState::RecoveryBlow);enter(ShadeState::RecoveryEnd);break;
        case ShadeState::RecoveryBlow:enter(ShadeState::RecoveryEnd);break;
        case ShadeState::RecoveryEnd:enter(ShadeState::RecoveryDissipate);break;
        case ShadeState::RecoveryDissipate:clear();break;
        default:break;
        }
    }
    // Source active children update in the same frame as Blow activates them.
    // Advancing before the transition holds the bright penultimate glow frame
    // at recovery .65s; the saved source capture already shows its faint end.
    glowAnimation.advance(dt);deathSprite.advance(dt);
    sync();
}
void HollowShade::fixedUpdate(float dt,Hero& hero,const World& world){
    PresentationPosition::Step step(presentation_,position);
    PresentationPosition::Step retreatStep(retreatPresentation_,retreatPose_);if(!visible()||dt<=0)return;
    if(state==ShadeState::Fly||state==ShadeState::Position){
        Vec2 target=hero.position;
        if(state==ShadeState::Position&&length({target.x-position.x,target.y-position.y})<=3)target.x=position.x+(position.x<hero.position.x?-1:1);
        const auto accelerate=[](float v,float delta){return std::clamp(v+(delta>0?.2f:delta<0?-.2f:0),-4.0f,4.0f);};
        velocity.x=accelerate(velocity.x,target.x-position.x);velocity.y=accelerate(velocity.y,target.y-position.y);
        if(state==ShadeState::Fly){
            const float speed=length(velocity);if(speed>4){velocity.x*=4/speed;velocity.y*=4/speed;}
            const Vec2 delta{target.x-position.x,target.y-position.y};const float divisor=std::max(1.0f,length(delta));
            velocity.x+=delta.x/divisor*8*dt;velocity.y+=delta.y/divisor*8*dt;
        }
        // Approximation: the source Terrain Buffer repulsion is represented by
        // the room's shared contact solver, with the exported solid body box.
        moveBox(position,velocity,config_.body,dt,world);
    }else if(state==ShadeState::Retreat){
        const float t=std::clamp(timer_,0.0f,1.0f);const float ease=1-(1-t)*(1-t)*(1-t);
        position={retreatFrom_.x+(origin.x-retreatFrom_.x)*ease,retreatFrom_.y+(origin.y-retreatFrom_.y)*ease};
        // iTweenMoveTo easeType 5 = easeInOutCubic. Keep the existing physical
        // route; only the collider-disabled retreat image uses the source ease.
        const float visual=t<.5f?4*t*t*t:1-4*(1-t)*(1-t)*(1-t);
        retreatPose_={retreatFrom_.x+(origin.x-retreatFrom_.x)*visual,retreatFrom_.y+(origin.y-retreatFrom_.y)*visual};
    }
    if(recoil_>0&&alive()){moveBox(position,recoilVelocity_,config_.body,std::min(dt,recoil_),world);recoil_=std::max(0.0f,recoil_-dt);}
    sync();
    if(object_&&object_->colliderEnabled&&hero.actor!=Actor::Dead){
        const auto body=bodyBounds(hero.position);
        std::array<Vec2,5> points{};for(unsigned i=0;i<points.size();++i)points[i]={position.x+config_.slashPoints[i].x*scaleX,position.y+config_.slashPoints[i].y};
        if(overlap(bounds(),body)||(state==ShadeState::Slash&&polygonOverlapsBox(points.data(),points.size(),body)))hero.takeDamage(1,hero.position.x<position.x?1:-1);
    }
}
void HollowShade::hit(void* self,const NailHit& hit,HitOutcome outcome){
    auto& s=*static_cast<HollowShade*>(self);if(!s.alive())return;
    if(outcome==HitOutcome::Killed){++s.injurySerial;s.enter(ShadeState::Recover);}
    else if(outcome==HitOutcome::Damaged){++s.injurySerial;s.recoil_=.15f;
        s.recoilVelocity_=hit.direction==Strike::Right?Vec2{15,0}:hit.direction==Strike::Left?Vec2{-15,0}:hit.direction==Strike::Up?Vec2{0,15}:Vec2{0,-15};
        s.recoilVelocity_.x*=hit.recoilMultiplier;s.recoilVelocity_.y*=hit.recoilMultiplier;
        if(s.state==ShadeState::Idle)s.enter(ShadeState::Startle);}
    if(outcome==HitOutcome::Damaged||outcome==HitOutcome::Killed)s.emitHit(hit.direction);
}
}
