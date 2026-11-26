#include "broken_vessel.hpp"
#include "broken_vessel_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace hk3ds {
namespace {
using namespace broken_vessel_data;
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
Rect scaled(Rect r,float scale){const float a=r.left*scale,b=r.right*scale;return {std::min(a,b),r.bottom,std::max(a,b),r.top};}
}
bool BrokenVesselEncounter::bind(CombatObject* object,const AnimationBank* art,EnemyRandom random,BrokenVesselProgress progress,unsigned objectCapacity){
    if(!object)return false;
    reset();object_=object;art_=art;random_=random;progress_=progress;objectCapacity_=std::min(objectCapacity,4u);
    if(objectCapacity_==0){reset();return false;}
    for(unsigned i=1;i<objectCapacity_;++i){object_[i]={};object_[i].colliderEnabled=false;}
    position=spawn;presentation_.reset(position);
    *object_={};object_->name="Infected Knight";object_->hasHealth=true;object_->layer=11;
    object_->health.hp=int(hp);object_->health.invincible=true;object_->receiver=this;object_->onHit=receive;
    if(progress_.killed){battleEnded_=true;enter(BrokenVesselPhase::Complete);}else enter(BrokenVesselPhase::Sleep);
    sync();return true;
}
Rect BrokenVesselEncounter::bounds()const{return boxBounds(position,scaled(body,scaleX));}
const Rect& BrokenVesselEncounter::gate(){return broken_vessel_data::gate;}
void BrokenVesselEncounter::sync(){object_->bounds=bounds();object_->colliderEnabled=contact_&&!progress_.killed;}
void BrokenVesselEncounter::receive(void* context,const NailHit&,HitOutcome outcome){
    auto& e=*static_cast<BrokenVesselEncounter*>(context);
    if(outcome!=HitOutcome::Damaged&&outcome!=HitOutcome::Killed)return;
    e.hitFlash=.1f;
    if(outcome==HitOutcome::Killed){
        // EnemyDeathEffects records the journal before emitting the corpse.
        e.progress_.killed=true;e.progress_.corniferLeft=true;e.contact_=false;e.corpseVisible_=true;
        e.position.y+=.5f;
        e.velocity={0,15};e.enter(BrokenVesselPhase::CorpseAir);e.sync();return;
    }
    if(e.phase==BrokenVesselPhase::ShakeStart||e.phase==BrokenVesselPhase::Shaking||e.phase==BrokenVesselPhase::ShakeEnd)return;
    ++e.hits_;++e.combo_;e.comboTimer_=comboTime;
    if(e.stuns_<unsigned(stunsMax)&&(e.hits_>=unsigned(stunHits)+e.stuns_||e.combo_>=unsigned(stunCombo)+e.stuns_)){e.hits_=e.combo_=0;++e.stuns_;e.velocity={e.scaleX*10,20};e.enter(BrokenVesselPhase::StunAir);}
}
void BrokenVesselEncounter::enter(BrokenVesselPhase next){
    phase=next;timer_=0;const char* clip=nullptr;
    using P=BrokenVesselPhase;
    switch(next){
    case P::Sleep:clip="Sleep";break;
    case P::StartPause:timer_=startPause;progress_.encountered=true;break;
    case P::Rumble:timer_=rumbleTime;break;
    case P::Spawning:timer_=wake1Time;break;
    case P::Wake1:clip="Wake 1";timer_=wake2Time;break;
    case P::Wake2:clip="Wake 2";timer_=wake3Time;break;
    case P::Wake3:clip="Wake 3";break;
    case P::Rewake:clip="Fall";position={rewakeX,rewakeY};presentation_.reset(position);break;
    case P::RoarStart:clip="Roar Start";break;
    case P::Roar:clip="Roar Loop";timer_=roarTime;break;
    case P::RoarEnd:clip="Roar End";contact_=true;object_->health.invincible=false;object_->health.spellVulnerable=true;break;
    case P::Idle:clip="Idle";timer_=random_.range(.5f,.75f);velocity.x=random_.range(.5f,1.25f)*(random_.range(0,1)<.5f?-1:1);willEvade_=random_.range(0,100)>55;willOverhead_=random_.range(0,100)>75;break;
    case P::JumpAntic:clip="Jump Antic";velocity.x=0;break;
    case P::Jump:clip="Jump";velocity={jumpX_,jumpSpeed};break;
    case P::Land:clip="Land";velocity.x=0;break;
    case P::DashAntic1:clip="Dash Antic 1";velocity={};break;
    case P::DashAntic2:clip="Dash Antic 2";timer_=.65f;velocity.x=0;break;
    case P::DashAntic3:clip="Dash Antic 3";break;
    case P::Dash1:clip="Dash Attack 1";velocity={scaleX*dashSpeed,0};break;
    case P::Dash2:clip="Dash Attack 2";break;
    case P::Dash3:clip="Dash Attack 3";break;
    case P::DashRecover:clip="Dash Recover";velocity.x=0;break;
    case P::DownstabAntic:clip="Downstab Antic";velocity={};break;
    case P::Downstab:clip="Downstab";velocity={0,-40};break;
    case P::DownstabLand:clip="Downstab Land";velocity={};for(float x:{-19.f,-10.f,10.f,19.f})spawnProjectile(position,{x,0});break;
    case P::DownstabRecover:timer_=downstabRecover;break;
    case P::EvadeAntic:clip="Evade Antic";velocity={};break;
    case P::Evade:clip="Evade";velocity={-scaleX*evadeSpeed,0};timer_=.19f;break;
    case P::EvadeRecover:clip="Evade Recover";velocity={};break;
    case P::OverheadAntic:clip="Overhead Antic";timer_=.65f;velocity={};willOverhead_=false;break;
    case P::Overhead:clip="Overhead Slashing";velocity={};break;
    case P::OverheadRecover:clip="Overhead Recover";break;
    case P::ShakeJumpAntic:clip="Jump Antic";velocity={};break;
    case P::ShakeJump:clip="Jump";velocity={jumpX_,50};break;
    case P::ShakeLand:clip="Land";velocity={};break;
    case P::ShakeAntic:clip="Shake Antic";velocity={};break;
    case P::ShakeStart:clip="Shake Loop";timer_=.5f;break;
    case P::Shaking:timer_=4;shakeSideTimer_=shakeUpTimer_=0;break;
    case P::ShakeEnd:clip="Shake End";timer_=1.4f;break;
    case P::StunAir:clip="Stun Air";break;
    case P::Stun:clip="Stun";timer_=stunTime;velocity={};break;
    case P::StunRecover:clip="Stun Recover";break;
    case P::CorpseAir:clip="Death Air";break;
    case P::CorpseLand:clip="Death Land";timer_=corpsePause;velocity={};break;
    case P::CorpseSteam:clip="Death";timer_=corpseSteam;break;
    case P::CorpseReady:timer_=corpseReady;break;
    case P::CorpseBlow:clip="Death 2";timer_=corpseBlow;battleEnded_=true;break;
    case P::DoorRumble:timer_=1;break; // infected_door Open Rumble, after IK GATE OPEN.
    case P::Complete:contact_=false;velocity={};break;
    }
    if(clip){if(timer_==0)timer_=clipTime(clip);if(art_){char name[48];std::snprintf(name,sizeof(name),"BrokenVessel/%s",clip);animation.play(art_->require(name),0,true);}}
}
void BrokenVesselEncounter::choose(const Hero& hero){
    // Source SendRandomEventV3 weights, consecutive limits and missed limits.
    constexpr float weights[]={1,.5f,1};constexpr unsigned repeatMax[]={2,1,2},missMax[]={3,5,3};
    unsigned choice=3;
    for(unsigned i=0;i<3;++i)if(missed_[i]>=missMax[i]){choice=i;break;}
    if(choice==3){float sum=0;for(unsigned i=0;i<3;++i)if(repeats_[i]<repeatMax[i])sum+=weights[i];
        float sample=random_.range(0,sum);for(unsigned i=0;i<3;++i)if(repeats_[i]<repeatMax[i]){sample-=weights[i];if(sample<=0){choice=i;break;}}}
    if(choice==3)choice=2;
    for(unsigned i=0;i<3;++i){if(i==choice){repeats_[i]=lastAttack_==i?repeats_[i]+1:1;missed_[i]=0;}else{repeats_[i]=0;++missed_[i];}}
    lastAttack_=choice;scaleX=hero.position.x<position.x?-1:1;airDash_=false;
    if(choice<2){downstab_=choice==0;jumpX_=downstab_?(hero.position.x-position.x)*2.5f:(random_.range(leftX,rightX)-position.x)*1.25f;enter(BrokenVesselPhase::JumpAntic);}
    else{
        // Original Air Dash? chooses ground dash (70%) or an airborne dash jump.
        // Its Dash Range BoolTest is disabled in the source instance.
        downstab_=false;
        if(random_.range(0,1)<.7f)enter(BrokenVesselPhase::DashAntic1);
        else{airDash_=true;jumpX_=(random_.range(leftX,rightX)-position.x)*1.25f;enter(BrokenVesselPhase::JumpAntic);}
    }
}
void BrokenVesselEncounter::spawnProjectile(Vec2 origin,Vec2 speed){
    for(auto& p:projectiles)if(!p.active()){
        p={};p.position=origin;p.velocity=speed;p.life=8.4f;p.scale=2;p.presentation.reset(origin);
        if(art_)p.animation.play(art_->require("Projectile/Idle"));
        return;
    }
}
void BrokenVesselEncounter::receiveBalloon(void* context,const NailHit&,HitOutcome outcome){
    if(outcome!=HitOutcome::Killed)return;
    auto& b=*static_cast<BrokenVesselHazard*>(context);b.state=2;b.timer=0;b.velocity={};
}
// Source: level343 Scuttler Spawner/Spawn and Orange Scuttler IK/Orange Scuttler.
// Visual-only actors: the original does not expose these to nail combat.
void BrokenVesselEncounter::updateScuttlers(float dt,const World& world){
    using P=BrokenVesselPhase;
    const bool emitting=(phase==P::Spawning||phase==P::Wake1||phase==P::Wake2)&&timer_>0;
    const auto play=[&](BrokenVesselHazard& a,const char* clip){a.animation.play(art_?art_->require(clip):nullptr,0,true);};
    if(emitting){
        scuttlerTimer_-=dt;
        if(scuttlerTimer_<=0){
            scuttlerTimer_=random_.range(.05f,.2f);
            for(auto& a:scuttlers)if(!a.active()){
                a={};a.position={random_.range(15.3f,36.4f),27.7f};a.velocity={0,random_.range(2,8)};
                // Orange Scuttler Init samples Target X / Blob Y once, then adds one to Y.
                a.target={position.x,position.y+1};
                a.scale=random_.range(1,1.5f);a.speed=random_.range(4,5.5f);
                // Original Init samples Accel=1..1.5; Run adds it per 60 Hz Update.
                a.acceleration=random_.range(60,90);a.life=20;
                // Unity resolves the slight initial floor overlap at large random scales.
                // Our swept contacts require a nonpenetrating initial body.
                float groundDistance=0;
                if(rayTerrain({a.position.x,a.position.y+1},{0,-1},2,world,groundDistance))
                    a.position.y=std::max(a.position.y,a.position.y+1-groundDistance+.50501f*a.scale+.001f);
                a.presentation.reset(a.position);play(a,"Scuttler/Spawn");break;
            }
        }
    }
    for(auto& a:scuttlers)if(a.active()){
        PresentationPosition::Step history(a.presentation,a.position);a.life-=dt;a.timer-=dt;
        if(a.state==5){if(a.timer<=0)a.life=0;continue;}
        a.velocity.y-=gravity*.6f*dt;
        const auto contacts=moveBox(a.position,a.velocity,Rect{-.239556f*a.scale,-.50501f*a.scale,.385002f*a.scale,.12055f*a.scale},dt,world);
        if(a.state==0&&contacts.ground){a.state=1;a.timer=.25f;play(a,"Scuttler/Land");}
        else if(a.state==1&&a.timer<=0){a.state=2;play(a,"Scuttler/Run");}
        else if(a.state==2){
            const float direction=a.target.x<a.position.x?-1.f:1.f;
            a.facing=-direction;
            // Preserve each scuttler's sampled acceleration in the 60 Hz port.
            a.velocity.x=std::clamp(a.velocity.x+direction*a.acceleration*dt,-a.speed,a.speed);
            if(std::abs(a.target.x-a.position.x)<3){a.state=3;a.timer=random_.range(0,.25f);a.velocity.x=0;}
        }else if(a.state==3&&a.timer<=0){
            a.state=4;a.timer=2;a.velocity={a.target.x-a.position.x,random_.range(12,17)};play(a,"Scuttler/Spawn");
        }else if(a.state==4&&((a.velocity.y<0&&a.position.y<a.target.y)||a.timer<=0)){
            a.state=5;a.timer=splashTime;a.velocity={};
            a.rotation=random_.range(splashAngleMin,splashAngleMax);
            const float c=std::cos(a.rotation),s=std::sin(a.rotation);
            a.position.x+=(c*splashOffsetX*a.facing-s*splashOffsetY)*a.scale;
            a.position.y+=(s*splashOffsetX*a.facing+c*splashOffsetY)*a.scale;
            play(a,"ScuttlerSplash/Splat");
        }
    }
}
void BrokenVesselEncounter::updateHazards(float dt,Hero& hero,const World& world){
    for(auto& p:projectiles)if(p.active()){
        PresentationPosition::Step step(p.presentation,p.position);
        p.life-=dt;
        if(p.life>.4f){
            p.velocity.y+=22*dt;p.velocity.x/=1+dt;p.velocity.y/=1+dt;
            p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
            if(overlap(boxBounds(p.position,{-.8f,-.8f,.8f,.7f}),bodyBounds(hero.position)))hero.takeDamage(1,p.position.x<hero.position.x?-1:1);
        }
    }
    const auto play=[this](BrokenVesselHazard& b,const char* name){
        const auto* clip=art_?art_->require(name):nullptr;b.animation.play(clip,0,true);
        return clip&&clip->fps>0?float(clip->count)/clip->fps:1.f;
    };
    for(unsigned i=0;i<balloons.size();++i){
        auto& b=balloons[i];if(!b.active())continue;
        PresentationPosition::Step step(b.presentation,b.position);
        auto& object=object_[i+1];
        if(progress_.killed&&b.state<3){b.state=3;b.timer=play(b,"Balloon/Leave");}
        if(b.state==2&&b.timer==0)b.timer=1.2f+play(b,"Balloon/Death");
        if(b.state>=2){object.colliderEnabled=false;b.timer-=dt;if(b.timer<=0)b.life=0;continue;}
        b.timer-=dt;
        if(b.state==0){if(b.timer<=0){b.state=1;play(b,"Balloon/Chase");}else continue;}
        const Vec2 delta{hero.position.x-b.position.x,hero.position.y-b.position.y};
        if(delta.x!=0)b.facing=delta.x<0?-1.f:1.f; // Original Chase FaceObject targets the hero.
        const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);
        if(length>1e-5f){b.velocity.x+=10*dt*delta.x/length;b.velocity.y+=10*dt*delta.y/length;}
        const float speed=std::sqrt(b.velocity.x*b.velocity.x+b.velocity.y*b.velocity.y);
        if(speed>b.speed){b.velocity.x*=b.speed/speed;b.velocity.y*=b.speed/speed;}
        const float radius=.72f*b.scale;
        moveBox(b.position,b.velocity,{-radius,-radius,radius,radius},dt,world);
        object.bounds=boxBounds(b.position,{-radius,-radius,radius,radius});object.colliderEnabled=true;
        if(overlap(object.bounds,bodyBounds(hero.position)))hero.takeDamage(1,b.position.x<hero.position.x?-1:1);
    }
    if(!contact_||progress_.killed||phase==BrokenVesselPhase::ShakeStart||phase==BrokenVesselPhase::Shaking||phase==BrokenVesselPhase::ShakeEnd)return;
    balloonTimer_-=dt;if(balloonTimer_>0)return;
    balloonTimer_=random_.range(4,5);if(object_->health.hp>420)return;
    for(unsigned i=0;i+1<objectCapacity_;++i)if(!balloons[i].active()){
        auto& b=balloons[i];b={};b.life=1;b.position={random_.range(17.36f,36.46f),random_.range(32.16f,37.42f)};
        b.scale=random_.range(1,1.25f);b.speed=random_.range(3.5f,6.5f);b.timer=.1f+play(b,"Balloon/Spawn");b.presentation.reset(b.position);
        auto& object=object_[i+1];object={};object.name="Parasite Balloon";object.hasHealth=true;object.health.hp=1;object.colliderEnabled=false;
        object.receiver=&b;object.onHit=receiveBalloon;break;
    }
}
void BrokenVesselEncounter::fixedUpdate(float dt,Hero& hero,const World& world){
    if(!active()||!std::isfinite(dt)||dt<=0)return;
    PresentationPosition::Step step(presentation_,position);
    using P=BrokenVesselPhase;
    updateHazards(dt,hero,world);
    updateScuttlers(dt,world);
    if(phase==P::Complete)return;
    if(phase==P::Sleep){if(overlap(trigger,bodyBounds(hero.position))){started_=true;enter(progress_.encountered?P::Rewake:P::StartPause);}return;}
    comboTimer_-=dt;if(comboTimer_<=0)combo_=0;
    timer_-=dt;hitFlash=std::max(0.f,hitFlash-dt);
    const bool dash=phase==P::Dash1||phase==P::Dash2||phase==P::Dash3;
    const bool floating=dash||phase==P::DownstabAntic||(airDash_&&(phase==P::DashAntic1||phase==P::DashAntic2||phase==P::DashAntic3));
    if(!floating)velocity.y-=gravityScale*gravity*dt;
    const auto contacts=moveBox(position,velocity,scaled(body,scaleX),dt,world);
    // The terrain manifold can retain the support touched at takeoff.
    const bool grounded=contacts.ground&&velocity.y<=0;
    switch(phase){
    case P::StartPause:if(timer_<=0)enter(P::Rumble);break;
    case P::Rumble:if(timer_<=0)enter(P::Spawning);break;
    case P::Spawning:if(timer_<=0)enter(P::Wake1);break;
    case P::Wake1:if(timer_<=0)enter(P::Wake2);break;
    case P::Wake2:if(timer_<=0)enter(P::Wake3);break;
    case P::Wake3:if(timer_<=0&&std::none_of(scuttlers.begin(),scuttlers.end(),[](const auto& a){return a.active();}))enter(P::RoarStart);break;
    case P::Rewake:if(grounded)enter(P::RoarStart);break;
    case P::RoarStart:if(timer_<=0)enter(P::Roar);break;
    case P::Roar:if(timer_<=0)enter(P::RoarEnd);break;
    case P::RoarEnd:if(timer_<=0)enter(P::Idle);break;
    case P::Idle:{
        scaleX=hero.position.x<position.x?-1:1;
        constexpr int thresholds[]={370,220,110};
        if(shakeIndex_<3&&object_->health.hp<=thresholds[shakeIndex_]){
            ++shakeIndex_;jumpX_=(random_.range(25.5f,27.5f)-position.x)*1.25f;enter(P::ShakeJumpAntic);
        }else if(willEvade_&&overlap(boxBounds(position,scaled(evadeRange,scaleX)),bodyBounds(hero.position))&&
                 !overlap(boxBounds(position,scaled(evadeCheck,scaleX)),bodyBounds(hero.position)))enter(P::EvadeAntic);
        else if(willOverhead_&&overlap(boxBounds(position,scaled(overheadRange,scaleX)),bodyBounds(hero.position)))enter(P::OverheadAntic);
        else if(timer_<=0)choose(hero);
        break;}

    case P::JumpAntic:if(timer_<=0)enter(P::Jump);break;
    case P::Jump:if(airDash_&&position.y>=airDashHeight){position.y=airDashHeight;velocity={};enter(P::DashAntic1);}else if(downstab_&&position.y>=downstabHeight)enter(P::DownstabAntic);else if(grounded)enter(P::Land);break;
    case P::Land:if(timer_<=0)enter(P::Idle);break;
    case P::DashAntic1:if(timer_<=0)enter(P::DashAntic2);break;
    case P::DashAntic2:if(timer_<=0)enter(P::DashAntic3);break;
    case P::DashAntic3:if(timer_<=0)enter(P::Dash1);break;
    case P::Dash1:if(timer_<=0)enter(P::Dash2);break;
    case P::Dash2:if(timer_<=0)enter(P::Dash3);break;
    case P::Dash3:if(timer_<=0)enter(P::DashRecover);break;
    case P::DashRecover:if(timer_<=0){airDash_=false;enter(P::Idle);}break;
    case P::DownstabAntic:if(timer_<=0)enter(P::Downstab);break;
    case P::Downstab:if(grounded)enter(P::DownstabLand);break;
    case P::DownstabLand:if(timer_<=0)enter(P::DownstabRecover);break;
    case P::DownstabRecover:if(timer_<=0)enter(P::Idle);break;
    case P::EvadeAntic:if(timer_<=0)enter(P::Evade);break;
    case P::Evade:if(timer_<=0)enter(P::EvadeRecover);break;
    case P::EvadeRecover:if(timer_<=0)enter(P::Idle);break;
    case P::OverheadAntic:if(timer_<=0)enter(P::Overhead);break;
    case P::Overhead:if(timer_<=0)enter(P::OverheadRecover);break;
    case P::OverheadRecover:if(timer_<=0)enter(P::Idle);break;
    case P::ShakeJumpAntic:if(timer_<=0)enter(P::ShakeJump);break;
    case P::ShakeJump:if(grounded)enter(P::ShakeLand);break;
    case P::ShakeLand:if(timer_<=0)enter(P::ShakeAntic);break;
    case P::ShakeAntic:if(timer_<=0)enter(P::ShakeStart);break;
    case P::ShakeStart:if(timer_<=0)enter(P::Shaking);break;
    case P::Shaking:
        shakeSideTimer_-=dt;shakeUpTimer_-=dt;
        if(shakeSideTimer_<=0){shakeSideTimer_+=.4f;for(float side:{-1.f,1.f})spawnProjectile(position,{side*(5+3*unsigned(random_.range(0,4.999f))),0});}
        if(shakeUpTimer_<=0){shakeUpTimer_+=.7f;shakeDirection_=-shakeDirection_;spawnProjectile(position,{float(shakeDirection_),0});}
        if(timer_<=0)enter(P::ShakeEnd);
        break;
    case P::ShakeEnd:if(timer_<=0)enter(P::Idle);break;
    case P::StunAir:if(grounded)enter(P::Stun);break;
    case P::Stun:if(timer_<=0)enter(P::StunRecover);break;
    case P::StunRecover:if(timer_<=0)enter(P::Idle);break;
    case P::CorpseAir:if(grounded)enter(P::CorpseLand);break;
    case P::CorpseLand:if(timer_<=0)enter(P::CorpseSteam);break;
    case P::CorpseSteam:if(timer_<=0)enter(P::CorpseReady);break;
    case P::CorpseReady:if(timer_<=0)enter(P::CorpseBlow);break;
    case P::CorpseBlow:if(timer_<=0)enter(P::DoorRumble);break;
    case P::DoorRumble:if(timer_<=0)enter(P::Complete);break;
    default:break;
    }
    bool slash=false;
    if(dash){Vec2 points[sizeof(dashHit)/sizeof(*dashHit)];for(unsigned i=0;i<sizeof(points)/sizeof(*points);++i)points[i]={position.x+dashHit[i].x*scaleX,position.y+dashHit[i].y};slash=polygonOverlapsBox(points,sizeof(points)/sizeof(*points),bodyBounds(hero.position));}
    if(phase==P::Overhead)slash|=overlap(boxBounds(position,scaled(overheadHit,scaleX)),bodyBounds(hero.position));
    if(contact_&&(slash||overlap(bounds(),bodyBounds(hero.position))))hero.takeDamage(1,position.x<hero.position.x?-1:1);
    sync();
}
void BrokenVesselEncounter::update(float dt){
    if(!active()||!std::isfinite(dt)||dt<=0)return;
    animation.advance(dt);for(auto& p:projectiles)if(p.active())p.animation.advance(dt);
    for(auto& b:balloons)if(b.active())b.animation.advance(dt);
    for(auto& a:scuttlers)if(a.active())a.animation.advance(dt);
}
}
