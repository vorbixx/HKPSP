#include "room.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>
namespace hk3ds
{
void RoomConfig::supportWater(Hero& hero,const SceneActivationState* activation) const
{
    if(hero.vitals.health<=0||!hero.inputEnabled||(!hero.controlEnabled&&!hero.superDash.active())||
       hero.actor==Actor::Damage||hero.actor==Actor::Dead){hero.cancelSurfaceWater();return;}
    // Jump Out owns its one-frame handoff, including movement above the trigger.
    if(hero.surfaceSwim.phase==SurfaceSwimPhase::Jump||
       hero.surfaceSwim.phase==SurfaceSwimPhase::JumpFrame)return;
    const Rect body=bodyBounds(hero.position);
    const auto contains=[&](const auto& regions)->const Rect* {
        for(const auto& r:regions)
            if(overlaps(body,r))return &r;
        return nullptr;
    };
    const Rect* region=contains(water);
    if(!region&&hero.abilities.acidArmour)
        for(unsigned i=0;i<acidWater.size();++i)
            if((!activation||activation->enabled(SceneActivationTarget::AcidWater,i+1))&&overlaps(body,acidWater[i])){
                region=&acidWater[i];break;
            }
    if(hero.surfaceSwim.active()){
        if(!region||hero.contacts.ground)hero.cancelSurfaceWater();
        return;
    }
    if(!region||hero.velocity.y>0||hero.contacts.ground)return;
    // The source region sends SURFACE ENTER on the hero collider's overlap.
    // Keep the crossing height; the water FSM stops vertical velocity in place.
    hero.enterSurfaceWater();
}
static_assert(sizeof(RoomFlow)<=2048,"RoomFlow reset must fit the ARM stack; keep presentation pools in TutorialSession");
namespace
{
bool finite(float v){return std::isfinite(v)&&std::abs(v)<=100000;}
bool interactDoor(const RoomGate& gate)
{return (gate.flags&RoomGate::Interact)||gate.entryKind==2||(!gate.flags&&!gate.entryKind&&!std::strncmp(gate.name,"door",4));}
bool canEnterDoor(const Hero& hero)
{
    // Door Control / Can Enter? calls HeroController.CanInteract (3540).
    // Recoil here is damage recoil, not the horizontal recoil from hitting terrain.
    return hero.inputEnabled&&hero.controlEnabled&&hero.contacts.ground&&
        !hero.dashing&&!hero.nail.active&&hero.actor!=Actor::Damage&&
        hero.actor!=Actor::Dead&&hero.freezePhase==0;
}
void relinquishDoorControl(Hero& hero)
{
    // Door Control / Enter calls HeroController.RelinquishControl. Admission
    // above has already excluded dead heroes and relinquished control.
    hero.input.x=hero.input.y=0;
    hero.stopJump();hero.doubleJumping=false;hero.doubleStep=0;
    hero.stopDash();
    hero.bouncing=hero.mushroomBouncing=false;hero.bounceTime=0;
    hero.recoilDirection=hero.recoilStep=0;
    hero.wallSliding=false;hero.wallSide=0;hero.contacts.left=hero.contacts.right=false;
    hero.velocity={};hero.wallLocked=false;
    hero.inputEnabled=hero.controlEnabled=false;
    hero.lookUp=hero.lookDown=false;hero.lookTime=0;
    hero.stopAttack();
}
bool rect(Rect r){return finite(r.left)&&finite(r.bottom)&&finite(r.right)&&finite(r.top)&&r.left<=r.right&&r.bottom<=r.top;}
float follow(float current,float target,float dt,float damp)
{return target+(current-target)*std::exp(-dt/std::max(0.0001f,damp));}
}
bool interactiveDoorGate(const RoomGate& gate){return interactDoor(gate);}
bool roomOverlap(Rect a,Rect b)
{return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
float RoomFlow::deathFade() const
{
    // CameraFade template20521: enabled HUD Blanker Fade Time=.75, then .5 on respawn.
    return phase==RoomPhase::Dead&&deathStage_==2?std::min(1.f,timer_/.75f):respawnFade_;
}
void RoomFlow::advanceTransition(float seconds)
{
    if(!transition||!std::isfinite(seconds)||seconds<=0)return;
    transitionTime_+=seconds;
    // EaseFsmAction starts with delayTime=0. Its first positive scaled Update
    // makes that negative; the next Update starts the linear EaseColor.
    if(departureFadeDelay_)departureFadeDelay_=false;
    else departureFadeTime_+=seconds;
}
void RoomFlow::advanceTransition(float unscaledSeconds,Simulation& simulation)
{
    if(!transition||!std::isfinite(unscaledSeconds)||unscaledSeconds<=0)return;
    simulation.clock.beginFrame();
    const float scaled=unscaledSeconds*simulation.clock.scale;
    advanceTransition(scaled);
    if(interactDoor(*transition))simulation.animator.player.advance(scaled);
    simulation.clock.afterUpdate(unscaledSeconds);
    simulation.hero.scale=simulation.clock.scale;
}
void RoomFlow::advanceArrivalFade(float seconds)
{
    if(!std::isfinite(seconds)||seconds<=0||arrivalFade_<=0)return;
    // CameraFade / FadeIn sends the Blanker FADE OUT after .1 scaled seconds.
    // DelayedEvent sends strictly below zero; discard the event wait's overshoot.
    if(arrivalDelay_>=0){arrivalDelay_-=seconds;if(arrivalDelay_>=0)return;}
    // The new EaseColor first consumes its zero delay, as on departure.
    if(arrivalFadeDelay_){arrivalFadeDelay_=false;return;}
    arrivalFade_=std::max(0.f,arrivalFade_-seconds);
}
bool RoomFlow::transitionReady() const
{
    // Door Control / Enter waits .5 scaled seconds before Change Scene.
    // TransitionPoint.entryDelay belongs to the destination EnterScene coroutine.
    return transition&&(!interactDoor(*transition)||transitionTime_>=.5f);
}
float RoomFlow::transitionFade() const
{
    // CameraFade template20521 -> Blanker Control25216, linear black alpha.
    return transition&&interactDoor(*transition)?
        std::min(1.f,departureFadeTime_/((transition->flags&RoomGate::JustFade)?.25f:.33f)):
        (holdArrivalBlanker_?1.f:std::clamp(arrivalFade_/.5f,0.f,1.f));
}
bool roomMaskOverlap(Rect body,const RoomRemask& mask)
{return polygonTriggerOverlap(body,mask.trigger,mask.polygon,mask.polygonEnds);}

bool RoomConfig::load(const char* path)
{
    auto* f=std::fopen(path,"rb");if(!f)return false;
    const auto read=[f](void* p,std::size_t n){return std::fread(p,1,n,f)==n;};
    char magic[4];unsigned h[8];float v[18];RoomConfig next;
    bool ok=read(magic,4)&&!std::memcmp(magic,"HKQ1",4)&&read(h,sizeof(h))&&read(v,sizeof(v));
    if(!ok||h[0]>16||h[1]>16||h[2]>32||h[3]>8||h[4]>3||h[5]>32||h[6]>32||h[7]>11){std::fclose(f);return false;}
    for(float x:v)ok &= finite(x);
    next.initial={v[0],v[1]};next.retry={v[2],v[3]};next.cameraLimits={v[4],v[5],v[6],v[7]};
    // HKQ1 stores the sum of the two source waits for compatibility.
    next.deathWait=v[8]-next.deathFadeWait;next.wakeWait=v[9];next.lookAhead=v[10];next.dampTime=v[11];
    next.entryWait=v[12];next.entrySpeed=v[13];
    next.initialLandY=v[14];next.initialLandWait=v[15];next.initialFlockWait=v[16];next.initialGetUpWait=v[17];
    next.initialRight=h[4]&1;next.retryRight=h[4]&2;
    ok &= rect(next.cameraLimits)&&v[8]>=next.deathFadeWait&&v[9]>=0&&v[10]>=0&&v[11]>0&&v[12]>=0&&v[13]>0;
    next.crawlers.resize(h[0]);next.doors.resize(h[1]);next.locks.resize(h[2]);next.gates.resize(h[3]);
    for(auto& p:next.crawlers)ok &= read(&p.spawn,sizeof(Vec2))&&read(&p.scale,4)&&read(&p.count,4)&&finite(p.spawn.x)&&finite(p.spawn.y)&&finite(p.scale)&&(p.count==1||p.count==2);
    for(auto& d:next.doors)
    {
        unsigned a[5];ok &= read(a,sizeof(a));if(!ok)break;
        d.collision.trigger=a[0];d.collision.count=a[1];d.collision.layer=a[2];d.persistent=a[3];d.owner=a[4];
        if(!a[0]||!a[1]||a[1]>DoorConfig::capacity||a[2]>=32||a[3]>1){ok=false;break;}
        ok &= read(d.collision.disabledShapes.data(),a[1]*4);
        bool found=false;
        for(unsigned i=0;i<a[1];++i)
        {
            found |= d.collision.disabledShapes[i]==a[0];
            ok &= d.collision.disabledShapes[i]!=0;
            for(unsigned j=0;j<i;++j)ok &= d.collision.disabledShapes[i]!=d.collision.disabledShapes[j];
        }
        ok &= found;
    }
    for(auto& l:next.locks)
        ok &= read(&l.trigger,sizeof(Rect))&&read(&l.limits,sizeof(Rect))&&read(&l.flags,4)&&rect(l.trigger)&&rect(l.limits)&&l.flags<=7;
    for(auto& g:next.gates)
    {
        ok &= read(&g.trigger,sizeof(Rect))&&read(&g.offset,sizeof(Vec2))&&read(&g.delay,4)&&
              read(g.name,32)&&read(g.scene,32)&&read(g.entry,32);
        ok &= rect(g.trigger)&&finite(g.offset.x)&&finite(g.offset.y)&&finite(g.delay)&&g.delay>=0&&
              std::memchr(g.name,0,32)&&std::memchr(g.scene,0,32)&&std::memchr(g.entry,0,32)&&g.name[0];
    }
    next.hazards.resize(h[5]);next.checkpoints.resize(h[6]);
    for(auto& a:next.hazards)ok &= read(&a,sizeof(a))&&rect(a.trigger)&&a.damage<=100&&a.type<=5;
    for(auto& a:next.checkpoints)ok &= read(&a,sizeof(a))&&rect(a.trigger)&&finite(a.marker.x)&&finite(a.marker.y)&&a.flags<=3;
    if(ok&&h[7]>=1)
    {
        unsigned ext[3]{};float values[7]{};
        ok=read(ext,sizeof(ext))&&read(values,sizeof(values))&&read(&next.collapseTrigger,sizeof(Rect))&&read(&next.heroHurtbox,sizeof(Rect));
        ok &= ext[0]<=16&&(ext[1]==~0u||ext[1]<next.locks.size())&&ext[2]<=1&&rect(next.collapseTrigger)&&rect(next.heroHurtbox);
        for(float x:values)ok &= finite(x);
        ok &= values[0]>=0&&values[1]>=0&&values[2]>=0&&values[3]>0&&values[4]>=0;
        next.hazardFadeWait=values[0];next.hazardWakeWait=values[1];next.hazardInvulnerability=values[2];
        next.groundProbe=values[3];next.collapseWait=values[4];next.initialHazard={values[5],values[6]};
        next.initialHazardRight=ext[2]!=0;next.collapseCamera=ext[1];
        if(ok){next.collapseShapes.resize(ext[0]);ok &= read(next.collapseShapes.data(),ext[0]*4);}
        for(std::size_t i=0;ok&&i<next.collapseShapes.size();++i)
        {ok &= next.collapseShapes[i]!=0;for(std::size_t j=0;j<i;++j)ok &= next.collapseShapes[i]!=next.collapseShapes[j];}
    }
    if(ok&&h[7]>=2)
    {
        auto& c=next.breakFloor;
        unsigned data[3]{};
        ok=read(data,sizeof(data))&&read(&next.breakFloorRange,sizeof(Rect))&&read(&next.breakFloorWait,4);
        c.trigger=data[0];c.count=data[1];c.layer=data[2];
        ok &= c.trigger&&c.count>0&&c.count<=c.capacity&&c.layer<32&&rect(next.breakFloorRange)&&finite(next.breakFloorWait)&&next.breakFloorWait>0;
        if(ok)ok=read(c.disabledShapes.data(),c.count*4);
    }
    if(ok&&h[7]>=3)for(auto& d:next.doors)
    {
        unsigned counts[2]{};
        ok=read(counts,sizeof(counts))&&read(&d.fadeSeconds,4);
        ok &= counts[0]<=128&&counts[1]<=128&&finite(d.fadeSeconds)&&
              (counts[1]?d.fadeSeconds>0:d.fadeSeconds==0);
        if(!ok)break;
        d.wholeOwners.resize(counts[0]);d.fadeOwners.resize(counts[1]);
        ok=read(d.wholeOwners.data(),counts[0]*4)&&read(d.fadeOwners.data(),counts[1]*4);
        for(const auto* owners:{&d.wholeOwners,&d.fadeOwners})for(std::size_t i=0;i<owners->size();++i)
        {
            ok &= (*owners)[i]!=0;
            for(std::size_t j=0;j<i;++j)ok &= (*owners)[i]!=(*owners)[j];
        }
    }
    if(ok&&h[7]>=4)
    {
        unsigned count=0;ok=read(&count,4)&&count<=16;
        if(ok)next.reveals.resize(count);
        for(auto& reveal:next.reveals)
        {
            unsigned counts[2]{};
            ok=read(counts,sizeof(counts))&&read(&reveal.seconds,4)&&finite(reveal.seconds)&&reveal.seconds>0&&
               counts[0]>0&&counts[0]<=16&&counts[1]>0&&counts[1]<=128;
            if(!ok)break;
            reveal.triggers.resize(counts[0]);reveal.owners.resize(counts[1]);
            ok=read(reveal.triggers.data(),counts[0]*sizeof(Rect))&&read(reveal.owners.data(),counts[1]*4);
            for(auto trigger:reveal.triggers)ok &= rect(trigger);
            for(std::size_t i=0;i<reveal.owners.size();++i)
            {
                ok &= reveal.owners[i]!=0;
                for(std::size_t j=0;j<i;++j)ok &= reveal.owners[i]!=reveal.owners[j];
            }
            if(!ok)break;
        }
    }
    if(ok&&h[7]>=5)
    {
        unsigned count=0;ok=read(&count,4)&&count<=16;
        if(ok)next.remasks.resize(count);
        for(auto& mask:next.remasks)
        {
            unsigned owners=0;
            ok=read(&mask.trigger,sizeof(Rect))&&read(&mask.seconds,4)&&read(&owners,4)&&
               rect(mask.trigger)&&finite(mask.seconds)&&mask.seconds>0&&owners>0&&owners<=128;
            if(!ok)break;
            if(h[7]>=6)
            {
                unsigned mode=0,vertices=0;
                ok=read(&mode,4)&&mode<=1&&read(&vertices,4)&&(vertices==0||(vertices>=3&&vertices<=128));
                mask.opaqueInside=mode!=0;
                if(ok){mask.polygon.resize(vertices);ok=read(mask.polygon.data(),vertices*sizeof(Vec2));}
                for(auto p:mask.polygon)ok &= finite(p.x)&&finite(p.y)&&p.x>=mask.trigger.left&&p.x<=mask.trigger.right&&p.y>=mask.trigger.bottom&&p.y<=mask.trigger.top;
            }
            if(!ok)break;
            mask.owners.resize(owners);ok=read(mask.owners.data(),owners*4);
            if(ok&&h[7]>=9)
            {
                unsigned positioned=0;
                ok=read(&mask.startupPause,4)&&read(&mask.startupFade,4)&&read(&positioned,4)&&positioned<=1&&
                   finite(mask.startupPause)&&finite(mask.startupFade)&&mask.startupPause>=0&&mask.startupFade>=0&&
                   (mask.startupPause>0||mask.startupFade==0)&&(!positioned||mask.startupPause>0);
                mask.startupHeroPositioned=positioned!=0;
            }
            for(std::size_t i=0;i<mask.owners.size();++i)
            {
                ok &= mask.owners[i]!=0;
                for(std::size_t j=0;j<i;++j)ok &= mask.owners[i]!=mask.owners[j];
            }
            if(!ok)break;
        }
    }
    if(ok&&h[7]>=7)for(auto& p:next.crawlers)
    {
        unsigned flags=0;
        ok=read(&flags,4)&&flags<=7&&read(&p.activationTrigger,sizeof(Rect))&&rect(p.activationTrigger)&&
           read(&p.ultrawideThreshold,4)&&finite(p.ultrawideThreshold)&&p.ultrawideThreshold>=0;
        p.initialActive=flags&1;p.contactActivation=flags&2;p.disableIfVisitedDirtmouth=flags&4;
        ok &= !p.contactActivation||(p.activationTrigger.left<p.activationTrigger.right&&p.activationTrigger.bottom<p.activationTrigger.top);
        if(!ok)break;
    }
    if(ok&&h[7]>=8)
    {
        unsigned count=0;ok=read(&count,4)&&count<=128;
        if(ok)next.enemyMessages.resize(count);
        for(auto& zone:next.enemyMessages)
        {
            ok=read(&zone.trigger,sizeof(Rect))&&rect(zone.trigger)&&
               zone.trigger.left<zone.trigger.right&&zone.trigger.bottom<zone.trigger.top&&
               read(&zone.layers,4)&&read(&zone.event,4)&&zone.event<=2;
            if(!ok)break;
        }
    }
    if(ok&&h[7]>=10)
    {
        unsigned count=0,flags=0;ok=read(&count,4)&&count<=2;
        if(ok)next.secrets.resize(count);
        for(auto& secret:next.secrets)
        {
            ok=read(&secret.trigger,sizeof(Rect))&&read(&secret.owner,4)&&read(&secret.saveFlag,4)&&read(&secret.reloadSeconds,4)&&read(&secret.discoverySeconds,4)&&
               rect(secret.trigger)&&secret.trigger.left<secret.trigger.right&&secret.trigger.bottom<secret.trigger.top&&
               secret.owner>0&&(secret.saveFlag==2048||secret.saveFlag==4096)&&!(flags&secret.saveFlag)&&
               finite(secret.reloadSeconds)&&secret.reloadSeconds>0&&finite(secret.discoverySeconds)&&secret.discoverySeconds>0;
            flags|=secret.saveFlag;if(!ok)break;
        }
    }
    if(ok&&h[7]>=11)
    {
        unsigned count=0;ok=read(&count,4)&&count>0&&count<=16;
        if(ok){next.breakFloorOwners.resize(count);ok=read(next.breakFloorOwners.data(),count*4);}
        for(unsigned i=0;ok&&i<count;++i){ok=next.breakFloorOwners[i]>0;
            for(unsigned j=0;j<i;++j)ok=ok&&next.breakFloorOwners[i]!=next.breakFloorOwners[j];}
    }
    ok &= std::fgetc(f)==EOF;std::fclose(f);
    if(!ok)return false;
    // HKQ1 is the Tutorial bank. Its active-first actor order is frozen by
    // convert_room_rules.py: source GOs 200,1490,3078,2670 (not AudioSource order).
    // Original player probe: serialized Z, then last SetZ writer after .5 s.
    // Keep this source metadata in config; synthetic/other-sized banks retain Z=0.
    if(next.crawlers.size()==4){
        constexpr float depth[4][2]={{.01f,.006f},{.01f,.001f},{.009f,.006f},{.009f,.001f}};
        for(unsigned i=0;i<4;++i){next.crawlers[i].audioInitialDepth=depth[i][0];next.crawlers[i].audioMinimumDepth=depth[i][1];}
    }
    *this=std::move(next);return true;
}
void RoomFlow::groundHero(Hero& hero,const World& world) const
{
    float distance;
    if(rayTerrain(hero.position,{0,-1},config_->groundProbe,world,distance))
        hero.position.y-=distance+bodyBounds({}).bottom-0.01f;
}
void RoomFlow::begin(const RoomConfig& config,Simulation& simulation,const World& world,bool retry,bool visitedCrossroads)
{
    respawnFade_=retry&&phase==RoomPhase::Dead?1.f:0.f;
    config_=&config;entryGate_=nullptr;prompt_=nullptr;upHeld_=downHeld_=false;transitionTime_=arrivalFade_=0;deathStage_=0;wakeYield_=retry;
    departureFadeTime_=0;departureFadeDelay_=true;
    arrivalDelay_=0;arrivalFadeDelay_=false;
    holdArrivalBlanker_=false;doorArrivalTime_=doorEntryPause_=departureEntryPause_=0;
    doorArrivalStage_=DoorArrivalStage::EndOfFrame;
    if(retry)simulation.resetScene();else simulation={};
    retryRequested=false;transition=nullptr;timer_=0;look_=0;
    crumbling_=false;hardLandingPending_=false;collapseTimer_=0;hazardYield_=false;
    checkpointInside_.assign(config.checkpoints.size(),false);checkpointFired_.assign(config.checkpoints.size(),false);
    doorCancels_.assign(config.gates.size(),{});
    finishedEnteringScene_=false;visitedCrossroads_=visitedCrossroads;
    doorStartup_.assign(config.gates.size(),{});
    for(unsigned i=0;i<config.gates.size();++i)if(gateEnabled(i))startDoor(i);
    hazardMarker=retry?config.retry:config.initialHazard;hazardRight=retry?config.retryRight:config.initialHazardRight;
    auto& h=simulation.hero;h.position=retry?config.retry:config.initial;
    h.facingRight=retry?config.retryRight:config.initialRight;
    if(retry)
    {
        groundHero(h,world);
        h.controlEnabled=false;phase=RoomPhase::Waking;++progress.retries;
    }
    else
    {phase=RoomPhase::Entering;h.inputEnabled=false;h.controlEnabled=true;h.gravityEnabled=false;}
    camera={std::clamp(h.position.x,config.cameraLimits.left,config.cameraLimits.right),
            std::clamp(h.position.y,config.cameraLimits.bottom,config.cameraLimits.top)};
    cameraHistory_.reset(camera);
}
void RoomFlow::enterDoorInit(unsigned index)
{
    auto& state=doorStartup_[index];const auto& config=config_->gates[index].startup;
    state.timer=0;
    state.wait=(config.flags&RoomDoorStartup::CrossroadsAscent)?
        (visitedCrossroads_?config.visited:config.unvisited):config.normal;
    state.phase=state.wait>0?DoorStartupPhase::Wait:DoorStartupPhase::Ready;
}
void RoomFlow::startDoor(unsigned index)
{
    auto& state=doorStartup_[index];const auto& config=config_->gates[index].startup;
    state={};
    if(!(config.flags&RoomDoorStartup::Enabled))state.phase=DoorStartupPhase::Ready;
    else if(config.flags&RoomDoorStartup::WaitForScene){
        if(!finishedEnteringScene_)state.phase=DoorStartupPhase::WaitingScene;
        else if(config.pause>0)state.phase=DoorStartupPhase::Pause;
        else enterDoorInit(index);
    }else enterDoorInit(index);
}
void RoomFlow::finishedEnteringScene()
{
    if(finishedEnteringScene_)return;
    finishedEnteringScene_=true;
    for(unsigned i=0;i<doorStartup_.size();++i)
        if(doorStartup_[i].phase==DoorStartupPhase::WaitingScene){
            if(config_->gates[i].startup.pause>0)doorStartup_[i].phase=DoorStartupPhase::Pause;
            else enterDoorInit(i);
        }
}
void RoomFlow::updateDoorStartup(float scaled,Simulation& simulation)
{
    for(unsigned i=0;i<doorStartup_.size();++i){
        auto& state=doorStartup_[i];const auto& config=config_->gates[i].startup;
        if(!gateEnabled(i)){state={};continue;}
        if(state.phase==DoorStartupPhase::Disabled)startDoor(i);
        if(state.phase==DoorStartupPhase::Pause){
            state.timer+=scaled;
            if(state.timer>=config.pause)enterDoorInit(i);
            // Wait.OnEnter resets its timer; Pause overshoot is not carried.
        }else if(state.phase==DoorStartupPhase::Wait){
            state.timer+=scaled;
            if(state.timer>=state.wait)state.phase=DoorStartupPhase::Ready;
        }else continue;
        if(state.phase==DoorStartupPhase::Ready){
            // Idle only receives trigger stays from physics after it is entered.
            doorCancels_[i].phase=DoorCancelPhase::Idle;
            doorCancels_[i].physicsTick=simulation.physicsTicks;
        }
    }
}
void RoomFlow::enteredThrough(const char* name)
{
    entryGate_=nullptr;
    if(config_)for(const auto& gate:config_->gates)if(!std::strcmp(gate.name,name)){entryGate_=&gate;break;}
}
void RoomFlow::groundDoorArrival(Hero& hero,const World& world)
{
    // HeroController.EnterScene uses FindGroundPoint for GatePosition.door.
    if(entryGate_&&(entryGate_->flags&RoomGate::Door))groundArrival(hero,world);
}
bool RoomFlow::beginDoorArrival(Simulation& simulation,const AnimationBank& bank,float delayBeforeEnter)
{
    if(!entryGate_||!(entryGate_->flags&RoomGate::Door))return false;
    if(!std::isfinite(delayBeforeEnter)||delayBeforeEnter<0)return false;
    // HeroController.EnterScene's door coroutine starts after placement.
    auto& hero=simulation.hero;
    hero.inputEnabled=hero.controlEnabled=hero.gravityEnabled=false;
    hero.transitionInvincible=true;
    hero.velocity={};hero.airDashUsed=false;hero.doubleJumpUsed=false;
    hero.contacts.ground=true;hero.actor=Actor::Idle;
    if(entryGate_->flags&RoomGate::FaceRight)hero.facingRight=true;
    if(entryGate_->flags&RoomGate::FaceLeft)hero.facingRight=false;
    simulation.animator.overrideClip=bank.require("Idle");
    simulation.animator.player.play(simulation.animator.overrideClip,0,true);
    phase=RoomPhase::DoorArrival;doorArrivalStage_=DoorArrivalStage::EndOfFrame;
    doorArrivalTime_=0;doorEntryPause_=delayBeforeEnter;holdArrivalBlanker_=true;
    return true;
}
void RoomFlow::updateDoorArrival(float scaled,Simulation& simulation,const AnimationBank& bank)
{
    if(doorArrivalStage_==DoorArrivalStage::EndOfFrame){
        // A frame yield precedes all scaled waits, including at timeScale zero.
        doorArrivalStage_=doorEntryPause_>0?DoorArrivalStage::EntryPause:
            entryGate_->delay>0?DoorArrivalStage::GateDelay:DoorArrivalStage::FadeWait;
        return;
    }
    if(!std::isfinite(scaled)||scaled<=0)return;
    doorArrivalTime_+=scaled;
    const auto next=[&](DoorArrivalStage stage){doorArrivalStage_=stage;doorArrivalTime_=0;};
    switch(doorArrivalStage_){
    case DoorArrivalStage::EntryPause:
        if(doorArrivalTime_>=doorEntryPause_)next(entryGate_->delay>0?DoorArrivalStage::GateDelay:DoorArrivalStage::FadeWait);
        break;
    case DoorArrivalStage::GateDelay:
        if(doorArrivalTime_>=entryGate_->delay)next(DoorArrivalStage::FadeWait);
        break;
    case DoorArrivalStage::FadeWait:
        if(doorArrivalTime_>=.4f){
            if(!(entryGate_->flags&RoomGate::CustomFade))fadeIn();
            if(!(entryGate_->flags&RoomGate::NoWalk)){
                simulation.animator.overrideClip=bank.require("Exit Door To Idle");
                simulation.animator.player.play(simulation.animator.overrideClip,0,true);
            }
            next(DoorArrivalStage::Walk);
        }
        break;
    case DoorArrivalStage::Walk:{
        const auto* clip=(entryGate_->flags&RoomGate::NoWalk)?nullptr:bank.find("Exit Door To Idle");
        const float duration=clip&&clip->fps>0?clip->count/clip->fps:.33f;
        if(doorArrivalTime_>=duration){
            auto& hero=simulation.hero;
            hero.inputEnabled=hero.controlEnabled=hero.gravityEnabled=true;
            hero.transitionInvincible=false;
            simulation.animator.overrideClip=nullptr;
            phase=RoomPhase::Playing;finishedEnteringScene();
        }
        break;
    }
    case DoorArrivalStage::EndOfFrame:break;
    }
}
void RoomFlow::groundArrival(Hero& hero,const World& world)
{
    groundHero(hero,world);
    camera={std::clamp(hero.position.x,config_->cameraLimits.left,config_->cameraLimits.right),
            std::clamp(hero.position.y,config_->cameraLimits.bottom,config_->cameraLimits.top)};
    cameraHistory_.reset(camera);
}
void RoomFlow::update(float seconds,Simulation& simulation,const World& world,TerrainBank* terrain,float scaledSeconds,bool allowTransitions,const AnimationBank* heroBank)
{
    if(!config_||!finite(seconds)||seconds<0)return;
    PresentationPosition::Step cameraStep(cameraHistory_,camera);
    auto& h=simulation.hero;
    if(phase==RoomPhase::Transition){
        // PromptMarker.Update hides a marker whose owner becomes inactive.
        if(prompt_&&!gateEnabled(unsigned(prompt_-config_->gates.data())))prompt_=nullptr;
        return;
    }
    const float scaled=scaledSeconds>=0?scaledSeconds:seconds*std::max(0.0f,h.scale);
    advanceArrivalFade(scaled);
    const bool up=vertical(h.input.y)>0,pressedUp=up&&!upHeld_;
    const bool down=vertical(h.input.y)<0,pressedDown=down&&!downHeld_;
    upHeld_=up;downHeld_=down;prompt_=nullptr;
    updateDoorStartup(scaled,simulation);
    if(phase==RoomPhase::DoorArrival&&heroBank){updateDoorArrival(scaled,simulation,*heroBank);return;}
    for(unsigned i=0;i<doorCancels_.size();++i){
        auto& cancel=doorCancels_[i];
        if(!gateEnabled(i))cancel={};
        else if(cancel.phase==DoorCancelPhase::NextFrame){
            // NextFrameEvent.OnUpdate returns to Idle after this frame's physics.
            cancel.phase=DoorCancelPhase::Idle;cancel.physicsTick=simulation.physicsTicks;
        }else if(cancel.phase==DoorCancelPhase::Idle){
            // Idle listens for OnTriggerStay2D, not an Update overlap poll.
            if(cancel.physicsTick!=simulation.physicsTicks&&
               roomOverlap(bodyBounds(h.position),config_->gates[i].trigger))
                cancel.phase=DoorCancelPhase::None;
            cancel.physicsTick=simulation.physicsTicks;
        }
    }
    respawnFade_=std::max(0.f,respawnFade_-scaled/.5f);
    if(hardLandingPending_){hardLandingPending_=false;if(!h.contacts.ground)h.willHardLand=true;}
    if(progress.collapsed&&terrain)for(unsigned id:config_->collapseShapes)terrain->setEnabled(id,false);
    if(phase==RoomPhase::HazardDeath)
    {
        if(hazardYield_){hazardYield_=false;return;}
        timer_+=scaled;
        if(timer_<config_->hazardFadeWait)return;
        h.position=hazardMarker;groundHero(h,world);h.facingRight=hazardRight;
        h.actor=Actor::Idle;h.velocity={};h.contacts={};h.gravityEnabled=false;
        h.invulnerableTime=config_->hazardInvulnerability;h.invulnerableDelay=0;h.invulnerabilityStarted=true;
        phase=RoomPhase::HazardWake;timer_=0;++progress.hazardRespawns;return;
    }
    if(phase==RoomPhase::HazardWake)
    {
        timer_+=scaled;
        if(timer_>=config_->hazardWakeWait){h.controlEnabled=true;h.gravityEnabled=true;phase=RoomPhase::Playing;}
        return;
    }
    if(h.actor==Actor::Dead&&phase!=RoomPhase::Dead)
    {phase=RoomPhase::Dead;timer_=0;deathStage_=0;++progress.deaths;return;}
    if(phase==RoomPhase::Dead)
    {
        // Die yields once before PlayerDead starts; each WaitForSeconds
        // starts at the end of its frame and discards the overshoot.
        if(deathStage_==0){deathStage_=1;return;}
        timer_+=scaled;
        const float wait=deathStage_==1?config_->deathWait:config_->deathFadeWait;
        if(timer_>=wait)
        {
            timer_=0;
            if(deathStage_==1)deathStage_=2;
            else retryRequested=true;
        }
        return;
    }
    if(phase==RoomPhase::Waking)
    {if(wakeYield_){wakeYield_=false;return;}timer_+=scaled;if(timer_>=config_->wakeWait){h.controlEnabled=true;phase=RoomPhase::Playing;}}
    if(phase==RoomPhase::Entering)
    {
        timer_+=seconds;
        if(timer_>=config_->entryWait&&!h.gravityEnabled)
        {h.gravityEnabled=true;h.velocity.y=-config_->entrySpeed;}
        if(timer_>=config_->entryWait&&h.position.y<=config_->initialLandY)
        {phase=RoomPhase::InitialLand;timer_=0;h.controlEnabled=false;h.velocity={};h.stopJump();h.stopAttack();}
    }
    else if(phase==RoomPhase::InitialLand||phase==RoomPhase::InitialFlock||phase==RoomPhase::InitialGetUp)
    {
        timer_+=scaled;
        const float wait=phase==RoomPhase::InitialLand?config_->initialLandWait:config_->initialFlockWait;
        if(phase==RoomPhase::InitialGetUp?presentation.complete:timer_>=wait)
        {
            timer_=0;
            if(phase==RoomPhase::InitialLand)phase=RoomPhase::InitialFlock;
            else if(phase==RoomPhase::InitialFlock)phase=RoomPhase::InitialGetUp;
            else{h.inputEnabled=true;h.controlEnabled=true;phase=RoomPhase::Playing;}
        }
    }
    const auto bounds=bodyBounds(h.position);
    if(phase==RoomPhase::Playing)
    {
        for(std::size_t i=0;i<config_->checkpoints.size();++i)
        {
            const auto& c=config_->checkpoints[i];const bool inside=roomOverlap(bounds,c.trigger);
            if(inside&&!checkpointInside_[i]&&!checkpointFired_[i])
            {hazardMarker=c.marker;hazardRight=(c.flags&1)!=0;if(c.flags&2)checkpointFired_[i]=true;}
            checkpointInside_[i]=inside;
        }
        for(std::size_t i=0;i<config_->hazards.size();++i)
        {
            const auto& hazard=config_->hazards[i];
            const auto hurtbox=boxBounds(h.position,config_->heroHurtbox,h.facingRight?-1.0f:1.0f);
            if((activation_&&!activation_->enabled(SceneActivationTarget::Hazard,i+1))||!hazard.damage||!roomOverlap(hurtbox,hazard.trigger))continue;
            if(i<config_->hazardShapes.size()){
                const auto& shape=config_->hazardShapes[i];
                if(!polygonTriggerOverlap(hurtbox,hazard.trigger,shape.vertices,shape.ends))continue;
            }
            // Spikes and acid bypass ordinary enemy-hit invulnerability. They do
            // not start the enemy recoil/freeze coroutine or reset HP/soul.
            const unsigned type=hazard.hazardType();
            if((type!=2&&type!=3)||h.actor==Actor::Dead||h.testInvulnerable||h.transitionInvincible)continue;
            // HeroBox checks this component flag before TakeDamage. Ordinary
            // spikes remain harmful during Shade Cloak; only authored ones pass.
            if(h.shadeDashing&&(hazard.type&RoomHazard::ShadeDashPassable))continue;
            // Source Acid Armour Check sets only DamageHero's acid damage to zero.
            if(type==3&&h.abilities.acidArmour)continue;
            h.vitals.takeHealth(static_cast<int>(hazard.damage)*(h.charms.overcharmed?2:1));
            h.cancelSurfaceWater();h.cancelSuperDash();h.stopAttack();h.stopJump();h.stopDash();h.velocity={};h.damageVelocity={};
            h.focus=FocusPhase::None;h.recoilDirection=0;h.freezePhase=0;h.freezeTime=0;
            h.wallSliding=h.wallLocked=h.doubleJumping=h.bouncing=h.mushroomBouncing=false;
            h.fallTime=0;h.willHardLand=false;h.controlEnabled=false;h.gravityEnabled=false;timer_=0;
            if(h.vitals.health==0){h.actor=Actor::Dead;simulation.syncDeathFreeze();phase=RoomPhase::Dead;deathStage_=0;++progress.deaths;}
            else{h.actor=Actor::Idle;phase=RoomPhase::HazardDeath;hazardYield_=true;}
            return;
        }
        if(!progress.collapsed&&!config_->collapseShapes.empty())
        {
            if(!crumbling_&&roomOverlap(bounds,config_->collapseTrigger)){crumbling_=true;collapseTimer_=0;}
            else if(crumbling_)
            {
                collapseTimer_+=scaled;
                if(collapseTimer_>=config_->collapseWait)
                {
                    progress.collapsed=true;hardLandingPending_=true;
                    if(terrain)for(unsigned id:config_->collapseShapes)terrain->setEnabled(id,false);
                }
            }
        }
    }
    if(entryGate_){
        const auto& gate=*entryGate_;const auto& r=gate.trigger;
        // Source offsets can start outside the room. Crossing the arrival
        // trigger is part of entry; only leaving through it may load a scene.
        const bool cleared=!std::strncmp(gate.name,"left",4)?bounds.left>=r.right:
            !std::strncmp(gate.name,"right",5)?bounds.right<=r.left:
            !std::strncmp(gate.name,"top",3)?bounds.top<=r.bottom:
            !std::strncmp(gate.name,"bot",3)?bounds.bottom>=r.top:!roomOverlap(bounds,r);
        if(cleared||(interactDoor(gate)&&!up&&(!(gate.flags&RoomGate::DownInteract)||!down)))entryGate_=nullptr;
    }
    if(allowTransitions&&phase==RoomPhase::Playing)for(const auto& gate:config_->gates)
        if(gateEnabled(unsigned(&gate-config_->gates.data()))&& &gate!=entryGate_&&gate.scene[0]&&roomOverlap(bounds,gate.trigger)){
            const bool interact=interactDoor(gate);
            if(interact){
                const auto index=unsigned(&gate-config_->gates.data());
                if(doorStartup_[index].phase!=DoorStartupPhase::Ready)continue;
                auto& cancel=doorCancels_[index];
                if(cancel.phase!=DoorCancelPhase::None)continue;
                // Door Control / In Range shows its marker on trigger overlap.
                // CanInteract belongs to Can Enter?, not prompt visibility.
                if(gate.prompt[0])prompt_=&gate;
                if(!pressedUp&&!((gate.flags&RoomGate::DownInteract)&&pressedDown)&&
                   !(gate.entryKind==2&&!(gate.flags&RoomGate::Interact)&&up))continue;
                if(!canEnterDoor(h)){
                    // Can Enter? / CANCEL hides and waits for the next Update.
                    if(prompt_==&gate)prompt_=nullptr;
                    cancel.phase=DoorCancelPhase::NextFrame;continue;
                }
            }
            // The successful Enter -> Change Scene path never hides the marker;
            // PromptMarker recycles it on level unload (or owner deactivation).
            departureEntryPause_=gate.entryPause;
            transition=&gate;prompt_=interact&&gate.prompt[0]?&gate:nullptr;
            transitionTime_=departureFadeTime_=0;departureFadeDelay_=true;phase=RoomPhase::Transition;
            h.cancelSurfaceWater();
            if(interact){
                relinquishDoorControl(h);
                h.transitionInvincible=true;
                // Enter Anim? plays Exit then immediately Enter (Over Hero=false).
                // Tk2dPlayAnimation does not wait for either clip to finish.
                if(heroBank){
                    simulation.animator.overrideClip=heroBank->find("Enter");
                    simulation.animator.slash={};
                    simulation.animator.player.play(*heroBank,"Enter",0,true);
                }
            }
            else{h.controlEnabled=false;h.velocity={};h.stopAttack();h.stopJump();}
            break;
        }
    Rect limits=config_->cameraLimits;unsigned flags=0;
    for(std::size_t i=0;i<config_->locks.size();++i)
    {
        const auto& lock=config_->locks[i];
        if(cameraLockEnabled_&&!cameraLockEnabled_(cameraLockContext_,lock.owner))continue;
        if(progress.collapsed&&i==config_->collapseCamera)continue;
        if(roomOverlap(bounds,lock.trigger)){limits=lock.limits;flags=lock.flags;if(flags&4)break;}
    }
    if(encounterCamera_)limits=encounterLimits_;
    const float target=h.facingRight?config_->lookAhead:-config_->lookAhead;
    look_+=std::clamp(target-look_,-6*seconds,6*seconds);
    // Sample the end of this control tick on the shared physics display clock.
    // renderCamera applies the control remainder, matching the hero's delay.
    // Raw 50 Hz bodies otherwise make the 60 Hz follow alternately stall/jump.
    const auto pose=simulation.heroRenderPosition(tuning::controlStep);
    Vec2 desired{std::clamp(pose.x+look_,limits.left,limits.right),std::clamp(pose.y,limits.bottom,limits.top)};
    // Fitted 3DS viewport follow. Source lock bounds and look-ahead are retained;
    // the original two-stage CameraTarget/SmoothDamp curve is not claimed.
    camera.x=follow(camera.x,desired.x,seconds,config_->dampTime);
    camera.y=follow(camera.y,desired.y,seconds,config_->dampTime);
}
}
