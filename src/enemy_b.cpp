#include "enemy_b.hpp"
#include "audio.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds
{
const EnemyBRegistration* findEnemyB(const char* name)
{
    if(name)for(const auto& entry:enemyBRegistry)if(!std::strcmp(name,entry.prefab))return &entry;
    return nullptr;
}
namespace
{
bool valid(const EnemyBConfig& c)
{
    const float values[]={c.body.left,c.body.bottom,c.body.right,c.body.top,c.speed,c.chaseSpeed,c.detectX,c.detectY,
        c.wait,c.jumpX,c.jumpY,c.gravity,c.recoilSpeed,c.recoilDuration,c.hideTime,c.upTime,c.downTime,c.exposedTime};
    for(float v:values)if(!std::isfinite(v)||std::abs(v)>10000)return false;
    return c.body.right>c.body.left&&c.body.top>c.body.bottom&&c.hp<=100000&&c.damage<=99&&c.smallGeo<=1000&&
        c.speed>=0&&c.chaseSpeed>=0&&c.detectX>=0&&c.detectY>=0&&c.wait>=0&&c.gravity>=0&&
        c.recoilSpeed>=0&&c.recoilDuration>=0&&c.hideTime>0&&c.upTime>0&&c.downTime>0&&c.exposedTime>0;
}
// "Attack Range" child (crossroads-enemies-b supplement.json): Transform local Y -0.48, scaled
// BoxCollider2D offset Y. The converter stores only the half extents.
float attackRangeCenterY(EnemyBKind kind)
{
    switch(kind)
    {
    case EnemyBKind::WanderingHusk:return -.48f+3.45f*-.15228271f;
    case EnemyBKind::HuskHornhead:return -.48f;
    case EnemyBKind::LeapingHusk:return -.48f+3.45f*.31246474f;
    default:return 0;
    }
}
// Zombie Swipe / Zombie Leap Idle Time after the cooldown or land clip.
float idleTime(EnemyBKind kind) { return kind==EnemyBKind::LeapingHusk?.5f:.25f; }
bool husk(EnemyBKind kind) { return kind==EnemyBKind::WanderingHusk||kind==EnemyBKind::HuskHornhead||kind==EnemyBKind::LeapingHusk; }
}
bool EnemyB::ready() const
{
    // The FSM's Ready state runs beside every Walker state; only the attack states stop the walker.
    return state==EnemyBState::Patrol||state==EnemyBState::Stopped||state==EnemyBState::Turn||state==EnemyBState::Waiting;
}
void EnemyB::beginAttack(Vec2 hero)
{
    // Left or Right?: StopWalker; Hero X <= Self X is HERO IS LEFT (scale X 1), otherwise scale X -1.
    scaleX=hero.x<=position.x?1.f:-1.f;
    if(kind_==EnemyBKind::LeapingHusk)leapSpeed_=(hero.x-position.x)*1.25f;
    enter(EnemyBState::Windup);
}
bool loadEnemyBConfigs(const char* path,EnemyBConfigs& destination)
{
    auto* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4];EnemyBConfigs next{};
    bool ok=std::fread(magic,1,4,file)==4&&!std::memcmp(magic,"HKB1",4);
    for(auto& c:next)
    {
        std::uint32_t integers[3]{};float v[18]{};
        ok=ok&&std::fread(integers,sizeof(integers),1,file)==1&&std::fread(v,sizeof(v),1,file)==1;
        c.hp=integers[0];c.damage=integers[1];c.smallGeo=integers[2];c.body={v[0],v[1],v[2],v[3]};
        c.speed=v[4];c.chaseSpeed=v[5];c.detectX=v[6];c.detectY=v[7];c.wait=v[8];c.jumpX=v[9];c.jumpY=v[10];
        c.gravity=v[11];c.recoilSpeed=v[12];c.recoilDuration=v[13];c.hideTime=v[14];c.upTime=v[15];
        c.downTime=v[16];c.exposedTime=v[17];ok=ok&&valid(c);
    }
    for(auto& shape:next[3].goamBodies)
    {
        float v[4]{};ok=ok&&std::fread(v,sizeof(v),1,file)==1;
        shape={v[0],v[1],v[2],v[3]};
        for(float value:v)ok=ok&&std::isfinite(value)&&std::abs(value)<100;
        ok=ok&&shape.right>shape.left&&shape.top>shape.bottom;
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(ok)destination=next;
    return ok;
}
const Clip* EnemyB::clip(const char* name) const
{
    if(kind_==EnemyBKind::FungCrawler)return bank_->require(name);
    char key[48]{};std::snprintf(key,sizeof(key),"%s/%s",enemyBRegistry[static_cast<unsigned>(kind_)].prefix,name);
    return bank_->require(key);
}
bool EnemyB::bind(EnemyBKind kind,const EnemyBConfig& config,const AnimationBank& bank,CombatObject& object,Vec2 p,float facing,float rotation,float authoredScaleY)
{
    const bool fungCrawler=kind==EnemyBKind::FungCrawler;
    if((!fungCrawler&&static_cast<unsigned>(kind)>=enemyBRegistry.size())||!valid(config)||!std::isfinite(p.x)||!std::isfinite(p.y)||
       !std::isfinite(facing)||facing==0||!std::isfinite(rotation)||std::abs(rotation)>10000||
       !std::isfinite(authoredScaleY)||authoredScaleY==0||std::abs(authoredScaleY)>10000||
       (kind!=EnemyBKind::Goam&&config.hp==0))return false;
    if(fungCrawler){
        for(const char* name:{"walk","turn","death","death_land"})if(!bank.find(name))return false;
    }else{
        char key[48]{};std::snprintf(key,sizeof(key),"%s/idle",enemyBRegistry[static_cast<unsigned>(kind)].prefix);
        if(!bank.find(key))return false;
    }
    *this={};kind_=kind;config_=config;bank_=&bank;object_=&object;position=p;
    scaleX=kind==EnemyBKind::Goam?facing:(facing>0?1.0f:-1.0f);
    rotation_=kind==EnemyBKind::Goam?rotation:0;authoredScaleY_=kind==EnemyBKind::Goam?authoredScaleY:1;
    rotationCosine_=rotation_==0?1:std::cos(rotation_);rotationSine_=rotation_==0?0:std::sin(rotation_);
    if(kind!=EnemyBKind::Goam)size_={std::abs(facing),std::abs(authoredScaleY)};
    presentation_.reset(p);object={};object.name=fungCrawler?"Fung Crawler":enemyBRegistry[static_cast<unsigned>(kind)].prefab;
    object.layer=11;object.hasHealth=true;object.health.hp=static_cast<int>(config.hp);
    object.health.invincible=kind==EnemyBKind::Goam;object.receiver=this;object.onHit=receive;
    enter(fungCrawler?EnemyBState::Patrol:kind==EnemyBKind::Goam?EnemyBState::Hidden:EnemyBState::Waiting);
    syncBounds();return true;
}
void EnemyB::startUp()
{
    if(kind_==EnemyBKind::Goam&&state==EnemyBState::Hidden){enter(EnemyBState::Exposed);syncBounds();}
}
void EnemyB::updateStartCondition(Vec2 camera,float depth)
{
    if(state!=EnemyBState::Waiting)return;
    const float dx=camera.x-position.x,dy=camera.y-position.y,dz=-38.1f-depth;
    // Native Vector3.sqrMagnitude < 3600, not a hero range or an XY test.
    // BeginStopped(Bored) is immediately followed by StartMoving on release.
    if(dx*dx+dy*dy+dz*dz<3600.f)
    {
        enter(EnemyBState::Stopped);
        enter(EnemyBState::Patrol);
    }
}
void EnemyB::updateWalker(float dt,Vec2 camera,float depth,EnemyRandom random,const World& world)
{
    if(!alive()||kind_==EnemyBKind::Goam||kind_==EnemyBKind::FungCrawler||!std::isfinite(dt)||dt<=0)return;
    walkerRandom_=random;
    if(random.walkerSample)walkerRandom_.sample=random.walkerSample;
    const bool waiting=state==EnemyBState::Waiting;
    updateStartCondition(camera,depth);
    if(waiting&&state==EnemyBState::Patrol)
    {
        const auto body=bounds();
        constexpr float skin=.1f;
        const float facing=-scaleX;
        const float halfWidth=(body.right-body.left)*.5f;
        const float reach=halfWidth+.5f;
        const float front=facing>0?body.right:body.left;
        float hit=0;
        bool wall=false,floor=false;
        for(float y:{body.bottom,(body.bottom+body.top)*.5f,body.top})
            if((wall=rayTerrain({front-facing*skin,y},{facing,0},reach+skin,world,hit,TerrainShape::terrainQueryExcluded)&&hit<reach+skin))break;
        if(!wall)for(float x:{body.left,(body.left+body.right)*.5f,body.right})
            if((floor=rayTerrain({x+facing*reach,body.bottom+skin},{0,-1},.25f+skin,world,hit,TerrainShape::terrainQueryExcluded)&&hit<.25f+skin))break;
        if(wall||!floor)enter(EnemyBState::Turn);
    }
    if(state==EnemyBState::Patrol||state==EnemyBState::Stopped)
    {
        walkerTime_-=dt;
        if(walkerTime_<=0)
        {
            // Native EndStopping draws even when turnAfterIdlePercentage is zero.
            if(state==EnemyBState::Stopped)walkerRandom_.range(0,100);
            enter(state==EnemyBState::Patrol?EnemyBState::Stopped:EnemyBState::Patrol);
        }
    }
}
float EnemyB::artScale() const
{
    if(kind_==EnemyBKind::FungCrawler)return scaleX;
    char name[64];std::snprintf(name,sizeof(name),"enemy-b/%s",enemyBRegistry[unsigned(kind_)].prefix);
    return animationFacingScale(name,kind_==EnemyBKind::Goam?scaleX:-scaleX,std::abs(scaleX)*size_.x);
}
Rect EnemyB::bounds() const
{
    auto shape=config_.body;
    if(kind_==EnemyBKind::Goam&&state!=EnemyBState::Hidden)
    {
        const unsigned first=state==EnemyBState::Rise?0:state==EnemyBState::Exposed?5:8;
        const unsigned count=state==EnemyBState::Exposed?3:5;
        const auto candidate=config_.goamBodies[first+std::min<std::uint32_t>(animation.frame,count-1)];
        if(candidate.right>candidate.left&&candidate.top>candidate.bottom)shape=candidate;
    }
    if(kind_!=EnemyBKind::Goam)return boxBounds(position,scaled(shape),scaleX);
    const float localX=(shape.left+shape.right)*.5f*scaleX,localY=(shape.bottom+shape.top)*.5f*authoredScaleY_;
    const float halfX=(shape.right-shape.left)*.5f*std::abs(scaleX),halfY=(shape.top-shape.bottom)*.5f*std::abs(authoredScaleY_);
    const float centerX=position.x+rotationCosine_*localX-rotationSine_*localY,centerY=position.y+rotationSine_*localX+rotationCosine_*localY;
    const float extentX=std::abs(rotationCosine_)*halfX+std::abs(rotationSine_)*halfY,extentY=std::abs(rotationSine_)*halfX+std::abs(rotationCosine_)*halfY;
    return {centerX-extentX,centerY-extentY,centerX+extentX,centerY+extentY};
}
bool EnemyB::harmful() const
{ return alive()&&(kind_!=EnemyBKind::Goam||state==EnemyBState::Rise||state==EnemyBState::Exposed||state==EnemyBState::Retract); }
void EnemyB::syncBounds() { object_->bounds=bounds();object_->colliderEnabled=harmful(); }
void EnemyB::enter(EnemyBState next)
{
    state=next;clock_=0;const char* name="idle";
    if(kind_==EnemyBKind::FungCrawler&&next==EnemyBState::Removed)return;
    switch(next)
    {
    case EnemyBState::Patrol:
        name="walk";++walkingPlaySerial_;velocity.x=-scaleX*config_.speed;
        // Placed Runner/Leaper: authored 4..1.5; Hornhead: 4.5..2.
        // Unity float Random.Range swaps reversed bounds and includes both ends.
        if(walkerRandom_.unit||walkerRandom_.sample)
            walkerTime_=walkerRandom_.range(kind_==EnemyBKind::HuskHornhead?2.f:1.5f,
                kind_==EnemyBKind::HuskHornhead?4.5f:4.f);
        break;
    case EnemyBState::Stopped:
        velocity.x=0;
        // All placed husks override the prefab with authored pauseTime 2.5..1.5.
        if(walkerRandom_.unit||walkerRandom_.sample)walkerTime_=walkerRandom_.range(1.5f,2.5f);
        break;
    case EnemyBState::Turn:name="turn";velocity.x=0;break;
    case EnemyBState::Windup:name="windup";velocity={};++anticipationSerial;break;
    case EnemyBState::Attack:name="attack";++attackSerial;break;
    case EnemyBState::Air:name="air";break;
    case EnemyBState::Land:name="land";velocity.x=0;break;
    case EnemyBState::Idle:velocity.x=0;break;
    case EnemyBState::Hidden:velocity={};break;
    case EnemyBState::Waiting:velocity={};break;
    case EnemyBState::Rise:name="rise";++emergeSerial;break;
    case EnemyBState::Exposed:name="exposed";break;
    case EnemyBState::Retract:name="retract";break;
    case EnemyBState::Dead:name="death";break;
    default:break;
    }
    const auto* selected=clip(name);animation.play(selected?selected:clip("idle"),0,true);
}
void EnemyB::fixedUpdate(float dt,Vec2 hero,const World& world)
{
    if(!active()||state==EnemyBState::Removed||!std::isfinite(dt)||dt<=0||dt>0.1f)return;
    PresentationPosition::Step step(presentation_,position);clock_+=dt;animation.advance(dt);object_->health.update(dt);
    if(kind_==EnemyBKind::Goam)
    {
        if(state==EnemyBState::Hidden&&clock_>=config_.hideTime)enter(EnemyBState::Rise);
        else if(state==EnemyBState::Rise&&clock_>=config_.upTime)enter(EnemyBState::Exposed);
        else if(state==EnemyBState::Exposed&&clock_>=config_.exposedTime)enter(EnemyBState::Retract);
        else if(state==EnemyBState::Retract&&clock_>=config_.downTime)enter(EnemyBState::Hidden);
        syncBounds();return;
    }
    if(state==EnemyBState::Dead)
    {
        velocity.y+=tuning::gravity*config_.gravity*dt;
        contacts=moveBox(position,velocity,boxBounds({},scaled(config_.body),scaleX),dt,world,{});
        if(contacts.ground&&animation.clip!=clip("death_land"))
        {animation.play(clip("death_land"),0,true);velocity.x=0;}
        if(clock_>=3)enter(EnemyBState::Removed);
        syncBounds();return;
    }
    hero_=hero;heroKnown_=true;
    // Ready: GetCanSeeHero && CheckAlertRange. AlertRange is the hero collider touching the
    // "Attack Range" trigger; LineOfSightDetector casts terrain (layer 8) from pivot to hero.
    bool target=false;
    if(husk(kind_))
    {
        const float centerY=position.y+attackRangeCenterY(kind_)*size_.y;
        const float halfX=config_.detectX*size_.x,halfY=config_.detectY*size_.y;
        const Rect range{position.x-halfX,centerY-halfY,position.x+halfX,centerY+halfY};
        if(overlaps(range,bodyBounds(hero)))
        {
            const Vec2 delta{hero.x-position.x,hero.y-position.y};
            const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);
            float distance=0;
            target=length<=1e-4f||!rayTerrain(position,{delta.x/length,delta.y/length},length,world,distance,
                TerrainShape::terrainQueryExcluded)||distance>=length;
        }
    }
    const bool clipFinished=animation.complete||(animation.clip&&animation.clip->wrap!=2&&animation.clip->wrap!=6&&
        clock_>=float(animation.clip->count)/std::max(animation.clip->fps,1.0f));
    if(recoil_>0)recoil_=std::max(0.0f,recoil_-dt);
    else if(target&&ready())beginAttack(hero);
    else if(state==EnemyBState::Patrol)
    {
        velocity.x=-scaleX*config_.speed;
        float distance=0;const auto b=bounds();const float reach=(b.right-b.left)/2+0.12f;
        const Vec2 origin{position.x,b.bottom+0.4f};
        const bool wall=rayTerrain(origin,{-scaleX,0},reach,world,distance,TerrainShape::terrainQueryExcluded);
        const bool floor=rayTerrain({origin.x-reach*scaleX,origin.y},{0,-1},0.8f,world,distance,TerrainShape::terrainQueryExcluded);
        if(contacts.ground&&(wall||!floor))enter(EnemyBState::Turn);
    }
    else if(state==EnemyBState::Turn&&clipFinished)
    { scaleX=-scaleX;enter(EnemyBState::Patrol); }
    else if(state==EnemyBState::Windup&&clock_>=config_.wait)
    {
        // Zombie Leap Launch: SetVelocity2d (Jump X Speed, 20) aimed at the latched hero distance.
        if(kind_==EnemyBKind::LeapingHusk)
        {state=EnemyBState::Air;clock_=0;++attackSerial;velocity={leapSpeed_,config_.jumpY};}
        else {enter(EnemyBState::Attack);velocity.x=-scaleX*config_.chaseSpeed;}
    }
    else if(state==EnemyBState::Attack)
    {
        // Lunge: SetVelocity2d every frame until the Attack Lunge clip completes; walls do not end it.
        velocity.x=-scaleX*config_.chaseSpeed;
        if(clipFinished)enter(EnemyBState::Land);
    }
    else if(state==EnemyBState::Air&&velocity.y<=0&&contacts.ground)enter(EnemyBState::Land);
    // Cooldown/Land clip, then Idle for Idle Time, then Reset (StartWalker).
    else if(state==EnemyBState::Land&&clipFinished)enter(EnemyBState::Idle);
    else if(state==EnemyBState::Idle&&clock_>=idleTime(kind_))enter(EnemyBState::Patrol);
    velocity.y+=tuning::gravity*config_.gravity*dt;
    contacts=moveBox(position,velocity,boxBounds({},scaled(config_.body),scaleX),dt,world,{});syncBounds();
}
void EnemyB::receive(void* self,const NailHit& hit,HitOutcome outcome) { static_cast<EnemyB*>(self)->hit(hit,outcome); }
void EnemyB::hit(const NailHit& hit,HitOutcome outcome)
{
    if(!alive()||(outcome!=HitOutcome::Damaged&&outcome!=HitOutcome::Killed))return;
    const bool recoiling=recoil_>0;
    if(outcome==HitOutcome::Damaged&&(kind_==EnemyBKind::WanderingHusk||kind_==EnemyBKind::HuskHornhead))
    {
        // HealthManager.TakeDamage sends TOOK DAMAGE (Ready -> Left or Right? -> attack), then
        // Recoil.RecoilByDirection sends RECOIL HORIZONTAL (global -> Reset, StartWalker) for a
        // side hit when not already recoiling. Zombie Leap handles neither event.
        if(ready()&&heroKnown_)beginAttack(hero_);
        const bool side=hit.direction==Strike::Left||hit.direction==Strike::Right;
        if(side&&!recoiling&&(state==EnemyBState::Windup||state==EnemyBState::Attack||
            state==EnemyBState::Land||state==EnemyBState::Idle))enter(EnemyBState::Patrol);
    }
    ++injurySerial;recoil_=config_.recoilDuration;
    velocity.x=(hit.direction==Strike::Left?-config_.recoilSpeed:hit.direction==Strike::Right?config_.recoilSpeed:0)*hit.recoilMultiplier;
    if(outcome==HitOutcome::Killed)
    {
        ++deathSerial;pendingGeo_=config_.smallGeo;geoPosition=position;killFreezeRequested=true;
        velocity.y=12;enter(EnemyBState::Dead);syncBounds();
    }
}
void EnemyB::contactHero(Hero& hero) const
{
    if(harmful()&&overlaps(bounds(),bodyBounds(hero.position)))hero.takeDamage(static_cast<int>(config_.damage),hero.position.x<position.x?1:-1);
}
void EnemyB::emitSounds(AudioVoices& voices,std::uint32_t owner,std::uint32_t random)
{
    if(soundedInjury_!=injurySerial){voices.dispatch(EnemySoundEvent::Injury,owner,random);soundedInjury_=injurySerial;}
    if(soundedDeath_!=deathSerial){voices.dispatch(EnemySoundEvent::Kill,owner,random);soundedDeath_=deathSerial;}
    if(soundedAttack_!=attackSerial){voices.dispatch(EnemySoundEvent::Startle,owner,random);soundedAttack_=attackSerial;}
}
}
