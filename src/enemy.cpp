#include "enemy.hpp"
#include "crossroads_enemy.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds
{
namespace {
const char* crossroadsMoveClip(CrossroadsEnemyKind kind)
{return kind==CrossroadsEnemyKind::Tiktik?"Walk":kind==CrossroadsEnemyKind::Vengefly?"Idle":"Fly";}
const char* crossroadsDeathClip(CrossroadsEnemyKind kind)
{return kind==CrossroadsEnemyKind::AspidHatchling?"Death":"Death Air";}
}
void HatchlingSteering::chase(float dt,Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom random)
{
    // ChaseObject.DoBuzz checks the timer BEFORE incrementing it. Sampling
    // does not advance this timer, and acceleration is per callback, not /s.
    if(timer_>=period_){
        spread_={random.range(-1.5f,1.5f),random.range(-1.5f,1.5f)};
        timer_=0;period_=random.range(1,2);
    }else timer_+=dt;
    velocity.x=std::clamp(velocity.x+(position.x<target.x+spread_.x?.1f:-.1f),-5.f,5.f);
    velocity.y=std::clamp(velocity.y+(position.y<target.y+spread_.y?.1f:-.1f),-5.f,5.f);
}
void HatchlingSteering::start(Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom random)
{
    if(!entered_){chase(0,position,target,velocity,random);entered_=true;}
}
void HatchlingSteering::fixedUpdate(float dt,Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom random)
{
    start(position,target,velocity,random);
    chase(dt,position,target,velocity,random);
}
void HatchlingSteering::update(float dt,Vec2 velocity,float& scaleX)
{
    // FaceDirection retains its .4s pause and faces velocity, not the hero.
    if(facePause_>0){facePause_-=dt;return;}
    const float next=velocity.x>0?-std::abs(scaleX):std::abs(scaleX);
    if(next!=scaleX){scaleX=next;facePause_=.4f;}
}
const CrossroadsEnemyEntry* findCrossroadsEnemy(const char* prefab)
{
    if (!prefab) return nullptr;
    for (const auto& entry:crossroadsEnemyRegistry)
        if (!std::strcmp(prefab,entry.prefab)) return &entry;
    if(!std::strcmp(prefab,"Climber 1"))return &crossroadsEnemyRegistry[0];
    return nullptr;
}
bool CrossroadsEnemyConfig::load(const char* path)
{
    auto* f=std::fopen(path,"rb"); if(!f)return false;
    char magic[4]; unsigned u[5]; float v[33];
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"XEA1",4)&&
        std::fread(u,4,5,f)==5&&std::fread(v,4,33,f)==33&&std::fgetc(f)==EOF;
    std::fclose(f);
    if(!ok||u[0]>3||!u[1]||u[1]>10000||u[2]>100||u[3]>=32||u[4]>1000)return false;
    for(float x:v)if(!std::isfinite(x)||std::abs(x)>10000)return false;
    for(unsigned i=10;i<33;++i)if(i!=11&&(i<25||i>=29))if(v[i]<0)return false;
    if(v[2]>=v[4]||v[3]>=v[5]||v[6]>=v[8]||v[7]>=v[9]||v[25]>=v[27]||v[26]>=v[28]||v[12]>v[13])return false;
    CrossroadsEnemyConfig next;next.kind=static_cast<CrossroadsEnemyKind>(u[0]);
    auto& c=next.flight;c.hp=u[1];c.damage=u[2];c.layer=u[3];c.smallGeo=u[4];
    c.spawn={v[0],v[1]};c.body={v[2],v[3],v[4],v[5]};c.heroHurtbox={v[6],v[7],v[8],v[9]};
    c.alertRadius=v[10];c.alertOffsetY=v[11];c.idleMin=v[12];c.idleMax=v[13];
    c.idleSpeed=v[14];c.idleAcceleration=v[15];c.roamingRange=v[16];
    c.chaseSpeed=v[17];c.chaseAcceleration=v[18];c.attentionSpan=v[19];c.startleDuration=v[20];
    c.recoilSpeed=v[21];c.recoilDuration=v[22];c.stopDeceleration=v[23];c.stopDuration=v[24];
    next.corpse={v[25],v[26],v[27],v[28]};next.speed=v[29];next.acceleration=v[30];
    next.corpseGravity=v[31];next.flingSpeed=v[32];*this=next;return true;
}
bool CrossroadsEnemy::bind(const CrossroadsEnemyConfig& c,const AnimationBank& bank,CombatObject& object,
                          Vec2 spawn,const World& world,EnemyRandom random,float facing)
{
    if(static_cast<unsigned>(c.kind)>3||!c.flight.hp||c.flight.body.left>=c.flight.body.right||
       c.flight.body.bottom>=c.flight.body.top||(!random.unit&&!random.sample)||!std::isfinite(facing)||facing==0||
       !bank.find(crossroadsMoveClip(c.kind))||!bank.find(crossroadsDeathClip(c.kind))||
       (c.kind!=CrossroadsEnemyKind::AspidHatchling&&!bank.find("Death Land")))return false;
    if(c.kind==CrossroadsEnemyKind::Vengefly)
        for(const char* clip:{"Startle","Chase","TurnToIdle","TurnToFly"})if(!bank.find(clip))return false;
    *this=CrossroadsEnemy{};config_=c;bank_=&bank;object_=&object;random_=random;
    position=spawn;scaleX=facing<0?-1.0f:1.0f;
    if(c.kind==CrossroadsEnemyKind::Tiktik){
        const Rect body=boxBounds({},c.flight.body,scaleX);
        const Vec2 origin{position.x+(body.left+body.right)*.5f,position.y+(body.bottom+body.top)*.5f};
        float distance=0;
        if(rayTerrain(origin,{0,-1},2,world,distance,TerrainShape::terrainQueryExcluded))
            position={origin.x,origin.y-distance};
        previousSurfacePosition_=position;
    }
    presentation_.reset(position);
    if(c.kind==CrossroadsEnemyKind::Vengefly){auto flight=c.flight;flight.spawn=spawn;
        if(!buzzer_.bind(flight,object,random,&bank)){object_=nullptr;return false;}
        buzzer_.scaleX=scaleX;
    }else{
        object={};object.name=crossroadsEnemyRegistry[static_cast<unsigned>(c.kind)].prefab;
        object.layer=c.flight.layer;object.hasHealth=true;object.health.hp=static_cast<int>(c.flight.hp);
        object.receiver=this;object.onHit=receive;
        if(c.kind==CrossroadsEnemyKind::Gruzzer)gruzzerDirection(random.range(0,360));
        // FaceDirection.OnEnter precedes ChaseObject.OnEnter in Control.
        if(c.kind==CrossroadsEnemyKind::AspidHatchling)hatchling_.update(0,velocity,scaleX);
    }
    animation_.play(bank.require(crossroadsMoveClip(c.kind)));
    if(c.kind==CrossroadsEnemyKind::Tiktik)surfaceUpdate(0,world);
    sync();return true;
}
float CrossroadsEnemy::artScale()const{
    static constexpr const char* kinds[]={"crossroads-enemies-a/tiktik","crossroads-enemies-a/gruzzer",
        "crossroads-enemies-a/vengefly","crossroads-enemies-a/aspid-hatchling"};
    return animationFacingScale(kinds[unsigned(config_.kind)],config_.kind==CrossroadsEnemyKind::Tiktik?scaleX:-scaleX,std::abs(scaleX));
}
Rect CrossroadsEnemy::bounds()const{
    const Rect body=boxBounds({},config_.flight.body,scaleX);
    if(config_.kind!=CrossroadsEnemyKind::Tiktik)return boxBounds(position,config_.flight.body,scaleX);
    const float cosine=std::cos(rotation),sine=std::sin(rotation);
    Rect result{100000,100000,-100000,-100000};
    for(float x:{body.left,body.right})for(float y:{body.bottom,body.top}){
        const float px=position.x+x*cosine+y*sine,py=position.y-x*sine+y*cosine;
        result.left=std::min(result.left,px);result.right=std::max(result.right,px);
        result.bottom=std::min(result.bottom,py);result.top=std::max(result.top,py);
    }
    return result;
}
void CrossroadsEnemy::sync(){if(object_){object_->bounds=bounds();object_->colliderEnabled=alive();}}
Vec2 CrossroadsEnemy::renderPosition(PresentationTime t)const
{
    if(!alive())return corpsePresentation_.sample(corpsePosition_,t);
    return config_.kind==CrossroadsEnemyKind::Vengefly?buzzer_.renderPosition(t):presentation_.sample(position,t);
}
const AnimationPlayer& CrossroadsEnemy::animation()const
{return alive()&&config_.kind==CrossroadsEnemyKind::Vengefly?buzzer_.animation:animation_;}
void CrossroadsEnemy::receive(void* self,const NailHit& hit,HitOutcome outcome)
{static_cast<CrossroadsEnemy*>(self)->hit(hit,outcome);}
void CrossroadsEnemy::hit(const NailHit& hit,HitOutcome outcome)
{
    if(outcome!=HitOutcome::Damaged&&outcome!=HitOutcome::Killed)return;
    ++injurySerial;
    const Vec2 d=hit.direction==Strike::Right?Vec2{1,0}:hit.direction==Strike::Left?Vec2{-1,0}:
                 hit.direction==Strike::Up?Vec2{0,1}:Vec2{0,-1};
    velocity={d.x*config_.flight.recoilSpeed*hit.recoilMultiplier,d.y*config_.flight.recoilSpeed*hit.recoilMultiplier};recoil_=config_.flight.recoilDuration;
    if(outcome==HitOutcome::Killed&&!killSerial)startCorpse(hit.direction);
}
void CrossroadsEnemy::startCorpse(Strike strike)
{
    ++killSerial;geoReleased+=config_.flight.smallGeo;geoPosition=position;geoRotation=rotation;killFreezeRequested=true;corpseTime_=3;
    corpsePosition_=position;corpsePresentation_.reset(corpsePosition_);rotation=0;
    const float x=strike==Strike::Left?-.5f:strike==Strike::Right?.5f:0;
    corpseVelocity_={x*config_.flingSpeed,(strike==Strike::Down?-1.0f:.8660254f)*config_.flingSpeed};
    animation_.play(bank_->require(crossroadsDeathClip(config_.kind)),0,true);sync();
}
void CrossroadsEnemy::update(float dt,const Hero& hero,const World& world)
{
    if(!active()||dt<=0)return;
    if(config_.kind==CrossroadsEnemyKind::Vengefly){
        buzzer_.update(dt,hero,world);injurySerial=buzzer_.injurySerial;
        if(buzzer_.killSerial&&!killSerial)startCorpse(object_->health.lastDirection);
    }
    if(!alive())corpseTime_=std::max(0.0f,corpseTime_-dt);
    if(!alive()||config_.kind!=CrossroadsEnemyKind::Vengefly)animation_.advance(dt);
    if(alive()&&config_.kind==CrossroadsEnemyKind::AspidHatchling){
        hatchling_.start(position,hero.position,velocity,random_);
        hatchling_.update(dt,velocity,scaleX);
        sync();
    }
    if(alive()&&config_.kind==CrossroadsEnemyKind::Gruzzer){
        // Fly 2/FaceDirection runs in Update, including during recoil.
        scaleX=velocity.x>0?-1.f:1.f;
        if(gruzzerStaySides_)gruzzerTouching(world,true,true);
        sync();
    }
    if(alive()&&config_.kind==CrossroadsEnemyKind::Tiktik&&recoil_<=0){
        surfaceUpdate(dt,world);
        sync();
    }
    recoil_=std::max(0.0f,recoil_-dt);
}
void CrossroadsEnemy::surfaceUpdate(float dt,const World& world)
{
    if(turning_){
        turnTime_+=dt;
        const float progress=std::min(1.f,turnTime_/.25f);
        // Turn's coroutine samples Lerp while elapsed < spinTime; on exit it
        // sets only rotation, retaining the final sampled position.
        if(turnTime_<.25f)
            position={turnFrom_.x+(turnTo_.x-turnFrom_.x)*progress,turnFrom_.y+(turnTo_.y-turnFrom_.y)*progress};
        rotation=turnStartRotation_+turnDeltaRotation_*progress;
        velocity={};
        if(turnTime_>=.25f){
            turning_=false;
            surfaceNormal_=nextNormal_;
            previousSurfacePosition_=previousTurnPosition_=position;
            velocity={surfaceNormal_.y*scaleX*config_.speed,-surfaceNormal_.x*scaleX*config_.speed};
        }
        return;
    }
    // Climber.Walk constrains each axis against the previous accepted position,
    // and measures turn eligibility from the last turn, not accumulated travel.
    bool constrained=false;
    if(std::abs(position.x-previousSurfacePosition_.x)>.1f){
        position.x=previousSurfacePosition_.x;constrained=true;
    }
    if(std::abs(position.y-previousSurfacePosition_.y)>.1f){
        position.y=previousSurfacePosition_.y;constrained=true;
    }
    if(!constrained)previousSurfacePosition_=position;
    const Vec2 right{surfaceNormal_.y,-surfaceNormal_.x};
    const Vec2 tangent{right.x*scaleX,right.y*scaleX};
    const Rect local=boxBounds({},config_.flight.body,scaleX);
    const float centerX=(local.left+local.right)*.5f,centerY=(local.bottom+local.top)*.5f;
    const Vec2 origin{position.x+right.x*centerX+surfaceNormal_.x*centerY,
                      position.y+right.y*centerX+surfaceNormal_.y*centerY};
    float distance=0;
    const bool grounded=rayTerrain(origin,{-surfaceNormal_.x,-surfaceNormal_.y},1,world,distance,TerrainShape::terrainQueryExcluded);
    const float reach=(local.right-local.left)*.5f+.1f;
    const bool wall=grounded&&rayTerrain(origin,tangent,reach,world,distance,TerrainShape::terrainQueryExcluded);
    const Vec2 sinceTurn{position.x-previousTurnPosition_.x,position.y-previousTurnPosition_.y};
    if(sinceTurn.x*sinceTurn.x+sinceTurn.y*sinceTurn.y>=.25f*.25f&&(!grounded||wall)){
        nextNormal_=grounded?Vec2{-tangent.x,-tangent.y}:tangent;
        turnFrom_=turnTo_=position;
        if(wall){
            const float width=(local.right-local.left)*.5f,height=(local.top-local.bottom)*.5f;
            // GetTweenPos uses unrotated collider width/height in world axes.
            turnTo_={position.x+(tangent.x+surfaceNormal_.x)*width+tangent.x*.1f,
                     position.y+(tangent.y+surfaceNormal_.y)*height+tangent.y*.1f};
        }
        // Climber.Turn adds -90 degrees clockwise and +90 otherwise;
        // SetRotation2D writes that value directly to Transform.eulerAngles.z.
        turnStartRotation_=rotation;turnDeltaRotation_=(grounded?scaleX:-scaleX)*1.570796327f;
        turning_=true;turnTime_=0;velocity={};return;
    }
    velocity={tangent.x*config_.speed,tangent.y*config_.speed};
}
void CrossroadsEnemy::gruzzerDirection(float angle)
{
    // level43:6380 Fly 2: FaceDirection.OnEnter precedes
    // SetVelocityAsAngle.OnEnter and reads the previous Rigidbody velocity.
    scaleX=velocity.x>0?-1.f:1.f;
    flightAngle_=angle;
    const float radians=angle*0.01745329252f;
    flightVelocity_={std::cos(radians)*config_.speed,std::sin(radians)*config_.speed};
    velocity=flightVelocity_;
}
void CrossroadsEnemy::gruzzerBounce(unsigned side)
{
    // Bouncer Control: Hit Up/Down use Facing Right; Left/Right use Angle < 180.
    const bool right=flightAngle_<90||flightAngle_>=270,up=flightAngle_<180;
    float low=0,high=0;
    if(side==1){low=right?320.f:190.f;high=right?350.f:220.f;}
    else if(side==2){low=up?140.f:190.f;high=up?170.f:220.f;}
    else if(side==4){low=right?10.f:140.f;high=right?40.f:170.f;}
    else if(side==8){low=up?10.f:320.f;high=up?40.f:350.f;}
    if(high>low)gruzzerDirection(random_.range(low,high));
}
void CrossroadsEnemy::gruzzerTouching(const World& world,bool stay,bool deferred)
{
    const Rect enterBounds=bounds();unsigned pending=0;
    // CheckCollisionSide[Enter]: three .08 rays, top/right/bottom/left.
    // Enter caches all origins before its first event; Stay reads each side
    // after the preceding event (which can change facing and collider bounds).
    for(unsigned index=0;index<4;++index){
        const unsigned side=1u<<index;const Rect b=stay?bounds():enterBounds;
        const Vec2 center{(b.left+b.right)*.5f,(b.bottom+b.top)*.5f};
        const Vec2 origins[4][3]={
            {{b.left,b.top},{center.x,b.top},{b.right,b.top}},
            {{b.right,b.top},{b.right,center.y},{b.right,b.bottom}},
            {{b.right,b.bottom},{center.x,b.bottom},{b.left,b.bottom}},
            {{b.left,b.bottom},{b.left,center.y},{b.left,b.top}}
        };
        const Vec2 directions[]={{0,1},{1,0},{0,-1},{-1,0}};
        bool hit=false;
        for(const auto& origin:origins[index]){
            float distance=0;
            if(rayTerrain(origin,directions[index],.08f,world,distance,TerrainShape::terrainQueryExcluded)){
                hit=true;break;
            }
        }
        if(stay){gruzzerStaySides_&=~side;if(hit)gruzzerStaySides_|=side;}
        if(hit){if(deferred)pending=side;else gruzzerBounce(side);}
    }
    // Fsm.DoTransition replaces switchToState during Update. Apply only the
    // last event after all side checks, instead of sampling inside OnUpdate.
    if(pending)gruzzerBounce(pending);
}
void CrossroadsEnemy::gruzzerContacts(const Contacts& contacts,const World& world)
{
    const auto previous=gruzzerContacts_;const auto previousCount=gruzzerContactCount_;
    gruzzerContactCount_=0;
    for(std::size_t i=0;i<contacts.count;++i){
        const auto& contact=contacts.surfaces[i];
        if(contact.flags&TerrainShape::terrainQueryExcluded)continue;
        const auto end=gruzzerContacts_.begin()+gruzzerContactCount_;
        if(std::find(gruzzerContacts_.begin(),end,contact.id)!=end)continue;
        gruzzerContacts_[gruzzerContactCount_++]=contact.id;
        const bool stay=std::find(previous.begin(),previous.begin()+previousCount,contact.id)!=previous.begin()+previousCount;
        gruzzerTouching(world,stay,false);
    }
    // CheckCollisionSide registers Stay, not Exit. Its flags are cleared by a
    // later ray recheck, not by losing this frame's contact or re-entering Fly 2.
}
void CrossroadsEnemy::fixedUpdate(float dt,const Hero& hero,const World& world)
{
    if(!active()||dt<=0)return;
    PresentationPosition::Step step(presentation_,position);
    PresentationPosition::Step corpseStep(corpsePresentation_,corpsePosition_);
    if(config_.kind==CrossroadsEnemyKind::Vengefly){
        buzzer_.fixedUpdate(dt,hero,world);position=buzzer_.position;velocity=buzzer_.velocity;scaleX=buzzer_.scaleX;
        injurySerial=buzzer_.injurySerial;if(buzzer_.killSerial&&!killSerial)startCorpse(object_->health.lastDirection);
    }
    if(!alive()){
        if(corpseTime_>0){corpseVelocity_.y-=config_.corpseGravity*dt;
            auto contacts=moveBox(corpsePosition_,corpseVelocity_,boxBounds({},config_.corpse,scaleX),dt,world);
            if(contacts.ground&&!corpseLanded_){corpseLanded_=true;++corpseSplatSerial;
                if(config_.kind!=CrossroadsEnemyKind::AspidHatchling)animation_.play(bank_->require("Death Land"),0,true);}}
        return;
    }
    if(config_.kind==CrossroadsEnemyKind::Vengefly)return;
    const Rect local=boxBounds({},config_.flight.body,scaleX);
    if(recoil_>0){
        const auto contacts=moveBox(position,velocity,local,dt,world);
        if(config_.kind==CrossroadsEnemyKind::Gruzzer&&world.contact.enabled)gruzzerContacts(contacts,world);
    }
    else if(config_.kind==CrossroadsEnemyKind::Tiktik){
        position.x+=velocity.x*dt;position.y+=velocity.y*dt;
    }
    else if(config_.kind==CrossroadsEnemyKind::Gruzzer){
        // level43:6380, Bouncer Control: Fly 2 reapplies Angle/Speed;
        // Hit Up/Down branch on Facing Right, Hit Left/Right on Angle < 180.
        velocity=flightVelocity_;
        const auto contacts=moveBox(position,velocity,local,dt,world);
        if(world.contact.enabled)gruzzerContacts(contacts,world);
        else{
            // Legacy synthetic Solid worlds have no collider identities.
            const unsigned side=contacts.roof?1:contacts.right?2:contacts.ground?4:contacts.left?8:0;
            if(side)gruzzerBounce(side);else velocity=flightVelocity_;
        }
    }else{
        hatchling_.fixedUpdate(dt,position,hero.position,velocity,random_);
        moveBox(position,velocity,local,dt,world);
    }
    sync();
}
void CrossroadsEnemy::contactHero(Hero& hero)const
{
    if(config_.kind==CrossroadsEnemyKind::Vengefly){buzzer_.contactHero(hero);return;}
    if(alive()&&overlaps(bounds(),boxBounds(hero.position,config_.flight.heroHurtbox,hero.facingRight?-1.0f:1.0f)))
        hero.takeDamage(static_cast<int>(config_.flight.damage),hero.position.x<position.x?1:-1);
}
ActorAudioState CrossroadsEnemy::audioState(unsigned owner,Vec2 listener)const
{
    ActorAudioState state;state.owner=owner;state.active=active();state.alive=alive();
    state.crawler=config_.kind==CrossroadsEnemyKind::Tiktik;state.injurySerial=injurySerial;state.killSerial=killSerial;
    state.corpseSplatSerial=corpseSplatSerial;state.listenerDeltaX=position.x-listener.x;state.listenerDeltaY=position.y-listener.y;
    if(config_.kind==CrossroadsEnemyKind::Vengefly){state.startleSerial=buzzer_.startleSerial;
        state.chasePitch=buzzer_.state==BuzzerState::ChaseInSight||buzzer_.state==BuzzerState::ChaseOutOfSight;}
    return state;
}
namespace
{
Vec2 direction(Strike d)
{
    switch (d)
    {
    case Strike::Right: return {1,0};
    case Strike::Left: return {-1,0};
    case Strike::Up: return {0,1};
    case Strike::Down: return {0,-1};
    }
    return {};
}
float magnitude(Vec2 v) { return std::sqrt(v.x*v.x+v.y*v.y); }
}
bool CrawlerConfig::load(const char* path)
{
    auto* file=std::fopen(path,"rb");
    if (!file) return false;
    char magic[4]; unsigned h[6]; float v[31];
    static_assert(sizeof(unsigned)==4,"HKE1 requires 32-bit integers");
    bool ok=std::fread(magic,1,4,file)==4 && std::memcmp(magic,"HKE1",4)==0 &&
            std::fread(h,4,6,file)==6 && std::fread(v,4,31,file)==31 && std::fgetc(file)==EOF;
    std::fclose(file);
    if (!ok) return false;
    for (float x : v) if (!std::isfinite(x) || std::abs(x)>10000) return false;
    if (!h[0] || h[0]>10000 || h[1]>100 || h[2]>=32 || h[3]!=2 || h[4]>1000 || h[5]!=0 ||
        v[0]>=v[2] || v[1]>=v[3] || v[4]>=v[6] || v[5]>=v[7] || v[27]>=v[29] || v[28]>=v[30]) return false;
    for (int i=8; i<=13; ++i) if (v[i]<0) return false;
    for (int i=14; i<=17; ++i) if (v[i]!=1) return false;
    for (int i=20; i<=24; ++i) if (v[i]<0) return false;
    CrawlerConfig next;
    next.hp=h[0]; next.damage=h[1]; next.layer=h[2]; next.colliderCount=h[3]; next.smallGeo=h[4];
    next.body={v[0],v[1],v[2],v[3]}; next.corpse={v[4],v[5],v[6],v[7]};
    next.speed=v[8]; next.turnDelay=v[9]; next.recoilSpeed=v[10]; next.recoilDuration=v[11];
    next.gravity=v[12]; next.corpseGravity=v[13]; next.spawnOffset={v[18],v[19]};
    next.flingSpeed=v[20]; next.bounceFactor=v[21]; next.bounceThreshold=v[22];
    next.leftWait=v[23]; next.rightWait=v[24]; next.leftTranslate=v[25]; next.rightTranslate=v[26];
    next.heroHurtbox={v[27],v[28],v[29],v[30]};
    *this=next; return true;
}
float EnemyRandom::range(float low,float high) const
{ return sample?sample(context,low,high):low+(high-low)*std::clamp(unit(context),0.0f,1.0f); }
bool Crawler::canBind(const CrawlerConfig& config,const AnimationBank& bank,EnemyRandom random,float initialScale)
{
    return config.hp && (config.colliderCount==1||config.colliderCount==2) && std::isfinite(initialScale) && initialScale!=0 && (random.unit || random.sample) && bank.find("walk") && bank.find("turn") &&
        bank.find("Death Air") && bank.find("Death Land");
}
float Crawler::artScale()const
{return animationFacingScale("crawler/crawler",-scaleX,std::abs(scaleX));}
bool Crawler::bind(const CrawlerConfig& config,const AnimationBank& bank,CombatObject& object,
                   Vec2 p,const World& world,EnemyRandom random,float initialScale)
{
    if(!canBind(config,bank,random,initialScale))return false;
    *this={}; config_=config; bank_=&bank; object_=&object; position=transformPosition=previousPosition_=p; random_=random;scaleX=initialScale;
    object={}; object.name="Crawler 1"; object.layer=config.layer;
    object.colliderCount=config.colliderCount;
    object.hasHealth=true; object.health.hp=static_cast<int>(config.hp);
    object.receiver=this; object.onHit=receive;
    enter(WalkState::Walk,world); syncBounds(); return true;
}
Rect Crawler::bounds() const { return boxBounds(position,config_.body,scaleX); }
void Crawler::syncBounds() { object_->bounds=bounds(); }
WalkProbes Crawler::probes(const World& world) const
{
    const auto b=bounds();
    Vec2 origin{(b.left+b.right)/2,b.bottom+0.5f};
    const float reach=(b.right-b.left)/2+0.1f, facing=-scaleX;
    float distance;
    WalkProbes result;
    // WalkLeftRight.CheckIsGrounded/CheckWall/CheckFloor use mask 256.
    constexpr auto excluded=TerrainShape::terrainQueryExcluded;
    result.grounded=rayTerrain(origin,{0,-1},1,world,distance,excluded);
    result.wall=rayTerrain(origin,{facing,0},reach,world,distance,excluded);
    origin.x+=reach*facing;
    result.missingFloor=!rayTerrain(origin,{0,-1},1,world,distance,excluded);
    return result;
}
void Crawler::walk(const World& world)
{
    if (turning) return;
    velocity.x=-scaleX*config_.speed;
    const auto p=probes(world);
    auto& next=nextTurn_[static_cast<unsigned>(state)];
    if (shouldTurn_ || (p.grounded && (p.wall || p.missingFloor) && time_>=next))
    {
        shouldTurn_=false; next=time_+config_.turnDelay;
        turning=true; velocity.x=0;
        const auto* clip=bank_->find("turn"); turnTime_=clip->count/clip->fps;
        animation.play(clip,0,true);
    }
}
void Crawler::enter(WalkState next,const World& world)
{
    state=next; turning=false;
    const auto index=static_cast<unsigned>(next);
    const bool left=next==WalkState::StartLeft && !entered_[index];
    const bool right=next==WalkState::StartRight && !entered_[index];
    shouldTurn_=(left && scaleX<0) || (right && scaleX>0);
    if (!left && !right && next!=WalkState::Maintain) shouldTurn_=random_.range(0,100)<=50;
    entered_[index]=true; // Literal FsmBool startLeft/startRight are consumed by OnEnter.
    // tk2d Play preserves a clip that is already playing at start time zero.
    animation.play(bank_->require("walk"),0,animation.complete); walk(world);
}
void Crawler::command(bool right,const World& world)
{
    if (!alive() || (state!=WalkState::Walk && state!=WalkState::Maintain)) return;
    enter(right ? WalkState::StartRight : WalkState::StartLeft,world);
    commandTime_=right ? config_.rightWait : config_.leftWait;
    commandFresh_=true;
    turnBeganInPhysics_=turning;
    transformPosition.x+=right ? config_.rightTranslate : config_.leftTranslate;
    position=previousPosition_=transformPosition;
    syncBounds();
}
void Crawler::update(float scaled,float unscaled,const World& world)
{
    if (!active()) return;
    time_+=scaled;
    animation.advance(scaled);
    if (alive())
    {
        if (turning)
        {
            if(turnBeganInPhysics_)turnBeganInPhysics_=false;
            else turnTime_-=scaled;
            if (turnTime_<=0) { turning=false; scaleX=-scaleX; animation.play(bank_->require("walk"),0,true); }
        }
        if (state==WalkState::StartLeft || state==WalkState::StartRight)
        {
            // Realtime Wait starts at the callback's current frame clock;
            // its first OnUpdate observes zero elapsed. Scaled Wait instead
            // accumulates deltaTime on every OnUpdate, including entry.
            if(state==WalkState::StartRight || !commandFresh_)
                commandTime_-=state==WalkState::StartLeft ? unscaled : scaled;
            commandFresh_=false;
            if (commandTime_<=0) enter(WalkState::Maintain,world);
        }
        syncBounds();
    }
    else if (corpseState==CorpseState::InAir && corpsePosition.y < -10) corpseState=CorpseState::Removed;
    else if (corpseState==CorpseState::PendingLandEffects)
    {
        landTime_-=scaled;
        if (landTime_<=0) corpseState=CorpseState::Complete;
    }
}
bool translateRecoil(Vec2& rigidbody,Vec2& transform,Rect box,Vec2 direction,float distance,const World& world)
{
    float travel;
    const bool blocked=sweepBox(transform,box,direction,distance,world,travel);
    if(travel>0)
    {
        transform.x+=direction.x*travel;transform.y+=direction.y*travel;
        rigidbody=transform;
    }
    return blocked;
}
void Crawler::interpolate(float alpha)
{
    transformPosition=interpolatePosition(previousPosition_,position,alpha);
}
void Crawler::fixedUpdate(float dt,const World& world,bool resumeWalk)
{
    PresentationPosition::Step bodyStep(presentation_,position),corpseStep(corpsePresentation_,corpsePosition);
    if (!active()) return;
    if (!alive()) { corpsePhysics(dt,world); return; }
    if (recoilTime_>0)
    {
        if (sweeping_)
        {
            if (translateRecoil(position,transformPosition,recoilBox_,recoilDirection_,config_.recoilSpeed*recoilMultiplier_*dt,world))
                sweeping_=false;
        }
        recoilTime_-=dt;
    }
    previousPosition_=position;
    velocity.y+=tuning::gravity*config_.gravity*dt;
    terrainContacts=moveBox(position,velocity,boxBounds({},config_.body,scaleX),dt,world,config_.bodyMaterial,config_.colliderCount);
    transformPosition=position;
    syncBounds();
    if (resumeWalk) afterPhysics(world);
}
void Crawler::afterPhysics(const World& world)
{
    // The source Walk coroutine resumes after physics and collider callbacks.
    if (alive())
    {
        const bool wasTurning=turning;walk(world);
        if(!wasTurning && turning)turnBeganInPhysics_=true;
    }
}
void Crawler::receive(void* self,const NailHit& hit,HitOutcome outcome)
{ static_cast<Crawler*>(self)->hit(hit,outcome); }
void Crawler::hit(const NailHit& hit,HitOutcome outcome)
{
    if (outcome!=HitOutcome::Damaged && outcome!=HitOutcome::Killed) return;
    ++injurySerial;
    if (recoilTime_<=0)
    {
        recoilTime_=config_.recoilDuration; recoilMultiplier_=hit.recoilMultiplier; sweeping_=true;
        recoilDirection_=direction(hit.direction); recoilBox_=boxBounds({},config_.body,scaleX);
    }
    if (outcome!=HitOutcome::Killed || corpseState!=CorpseState::Absent) return;
    ++killSerial;
    object_->colliderEnabled=false; killFreezeRequested=true;
    corpsePosition={transformPosition.x+config_.spawnOffset.x*scaleX,transformPosition.y+config_.spawnOffset.y};
    corpseScaleX=scaleX;
    float angle=270, speed=config_.flingSpeed;
    switch (hit.direction)
    {
    case Strike::Right: angle=60; corpseScaleX=1; break;
    case Strike::Left: angle=120; corpseScaleX=-1; break;
    case Strike::Up: angle=random_.range(75,105); speed*=1.3f; break;
    case Strike::Down: break;
    }
    constexpr float radians=3.14159265358979323846f/180;
    corpseVelocity={std::cos(angle*radians)*speed,std::sin(angle*radians)*speed};
    corpseState=CorpseState::InAir; animation.play(bank_->require("Death Air"),0,true);
}
void Crawler::contactHero(Hero& hero) const
{
    if (alive() && overlaps(bounds(),boxBounds(hero.position,config_.heroHurtbox,hero.facingRight ? -1.0f : 1.0f)))
        for (unsigned c=0;c<config_.colliderCount;++c)
            hero.takeDamage(static_cast<int>(config_.damage),hero.position.x<position.x ? 1 : -1);
}
void Crawler::corpsePhysics(float dt,const World& world)
{
    if (corpseState==CorpseState::Absent || corpseState==CorpseState::Removed) return;
    if (++sampleStep_==4)
    {
        sampledDisplacement_={corpsePosition.x-lastSamplePosition_.x,corpsePosition.y-lastSamplePosition_.y};
        lastSamplePosition_=corpsePosition; sampledSpeed_=magnitude(corpseVelocity); sampleStep_=0;
    }
    corpseVelocity.y+=tuning::gravity*config_.corpseGravity*dt;
    const auto local=boxBounds({},config_.corpse,corpseScaleX);
    const auto c=moveBox(corpsePosition,corpseVelocity,local,dt,world,config_.corpseMaterial);
    terrainContacts=c;
    const unsigned mask=c.ground | (c.roof<<1) | (c.left<<2) | (c.right<<3);
    const unsigned entered=mask & ~corpseContacts_; corpseContacts_=mask;
    // Contacts retain manifold normals; callback ordering remains an explicit
    // scheduler limitation of the port.
    const auto bounce=[&](Vec2 normal)
    {
        const float length=magnitude(sampledDisplacement_);
        if (length>0)
        {
            const Vec2 d{sampledDisplacement_.x/length,sampledDisplacement_.y/length};
            const float dot=d.x*normal.x+d.y*normal.y;
            const float speed=sampledSpeed_*config_.bounceFactor*random_.range(0.8f,1.2f);
            corpseVelocity={(d.x-2*dot*normal.x)*speed,(d.y-2*dot*normal.y)*speed};
        }
        else corpseVelocity={};
    };
    if (sampledSpeed_>config_.bounceThreshold)
    {
        if (world.shapeCount)
        {
            for (std::size_t i=0;i<c.count;++i)
            {
                const auto& contact=c.surfaces[i];
                bool seen=false;
                for (std::size_t j=0;j<previousCorpseCount_;++j) seen |= previousCorpseContacts_[j]==contact.id;
                for (std::size_t j=0;j<i;++j) seen |= c.surfaces[j].id==contact.id;
                if (!seen) bounce(contact.normal);
            }
        }
        else if (entered) bounce((entered&1) ? Vec2{0,1} : (entered&2) ? Vec2{0,-1} : (entered&4) ? Vec2{1,0} : Vec2{-1,0});
    }
    previousCorpseCount_=c.count;
    for (std::size_t i=0;i<c.count;++i) previousCorpseContacts_[i]=c.surfaces[i].id;
    float clipped;
    if ((mask || c.count) && corpseState==CorpseState::InAir && sweepBox(corpsePosition,local,{0,-1},0.08f,world,clipped))
    {
        corpseState=CorpseState::PendingLandEffects; landTime_=1;
        animation.play(bank_->require("Death Land"),0,true);
    }
}
void KillFreeze::start(float currentScale)
{ phase=1; elapsed=0; startScale=scale=currentScale; }
void KillFreeze::update(float dt)
{
    if (!phase) { scale=1; return; }
    elapsed+=dt;
    if (phase==1)
    {
        scale=startScale*(1-std::min(1.0f,elapsed/0.04f));
        if (elapsed>=0.04f) { phase=2; elapsed=0; }
    }
    else if (phase==2)
    {
        scale=0;
        if (elapsed>=0.03f) { phase=3; elapsed=0; }
    }
    else
    {
        scale=std::min(1.0f,elapsed/0.04f);
        if (elapsed>=0.04f) { phase=0; elapsed=0; }
    }
    if (scale<=0.01f) scale=0;
}
}
