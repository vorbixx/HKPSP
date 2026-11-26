#include "enemy_c.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>

namespace hk3ds {
namespace {
#include "enemy_c_data.hpp"
float sign(float x){return x<0?-1.f:1.f;}
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
bool motherAlert(const EnemyCActor& actor,const Hero& hero,const World& world)
{
    // Crossroads_19 Hatcher: Alert Range New CircleCollider2774 radius .5,
    // Transform1312 scale15.608528137207031; both ancestors have unit scale.
    constexpr float radius=7.804264068603515625f;
    const Rect body=bodyBounds(hero.position);
    const float x=std::clamp(actor.position.x,body.left,body.right)-actor.position.x;
    const float y=std::clamp(actor.position.y,body.bottom,body.top)-actor.position.y;
    if(x*x+y*y>radius*radius)return false;
    // alert_range FSM4335 raycasts to Hero against Terrain layer8.
    const Vec2 delta{hero.position.x-actor.position.x,hero.position.y-actor.position.y};
    const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);
    float distance=0;
    return !rayTerrain(actor.position,length>0?Vec2{delta.x/length,delta.y/length}:Vec2{},
                       length,world,distance,TerrainShape::terrainQueryExcluded);
}
bool lineOfSight(Vec2 from,Vec2 to,const World& world)
{
    // LineOfSightDetector.Update casts from actor to hero, against layer8 only.
    const Vec2 delta{to.x-from.x,to.y-from.y};
    const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);
    float distance=0;
    return length<=1e-4f||!rayTerrain(from,{delta.x/length,delta.y/length},length,world,distance,
                                      TerrainShape::terrainQueryExcluded)||distance>=length;
}
bool bullyAlert(const EnemyCActor& actor,const Hero& hero,const World& world)
{
    // Zombie Barger "Attack Range" (Abyss_02 level328 GameObjects 1702/563): Transform
    // (0,-0.48) scale (8.88,3.45), BoxCollider2D offset Y -0.0812297, size (1,0.64640874).
    constexpr float centerY=-.48f+3.45f*-.0812297f,halfY=3.45f*.64640874f/2;
    const float x=4.44f*actor.size.x,y=centerY*actor.size.y,h=halfY*actor.size.y;
    const Rect range{actor.position.x-x,actor.position.y+y-h,actor.position.x+x,actor.position.y+y+h};
    return overlap(range,bodyBounds(hero.position))&&lineOfSight(actor.position,hero.position,world);
}
// Ruins Sentry (Ruins1_17 level97-1353) "Ruins Sentry" FSM steps.
enum SentryStep : std::uint8_t { Patrol, Startle, Run, Turn, RunEnd, Cooldown, Single, Triple, Evade, EvadeCool, AttackCd,
    // Ruins Sentry Fat (Ruins1_05 level92-1340) only.
    JumpAntic, Air, Land, ChargeAntic, Charge, ChargeEnd };
// Ruins Flying Sentry "Flying Sentry Nail" states (Lost Check, Check Dir and Can See? are instant).
enum FlyingStep : std::uint8_t { FlyWall, FlyWake, FlyBuzz, FlyIdle, FlyPosition, FlyAntic, FlySlashForward, FlySlash, FlyRecover };
// Mushroom Brawler "Shroom Brawler" states (Change Movement, Return, Set Wait Time are instant).
enum BrawlerStep : std::uint8_t { BrSleep, BrWake, BrIdle, BrTurn, BrSpitAntic, BrSpit, BrSpitCd, BrBounce, BrSmashAntic,
    BrSmashing, BrSmashRecover };
// Brooding Mawlek "Mawlek Control" states (Title, Music, Detect Hero Pos and Repeat Check are instant).
enum MawlekStep : std::uint8_t { MwDormant, MwWake, MwWakeJump, MwWakeAir, MwWakeLand, MwRoar, MwRoarEnd, MwIdle,
    MwSuperReady, MwSpitAntic, MwShoot, MwSuperCooldown, MwJumpAntic, MwJump, MwInAir, MwLand, MwJump2, MwInAir2, MwLand2 };
// Terrain Box child (offset (0.0115,0.4132), size (1.31,2.7907)): the flying body's terrain collider.
Rect flyingTerrainBox(const EnemyCActor& a)
{ return boxBounds({},{.011478424f-.655f,(.41323853f-1.39536f)*a.size.y,.011478424f+.655f,(.41323853f+1.39536f)*a.size.y},a.scaleX); }
bool sentryRegion(const EnemyCActor& actor,const Hero& hero,float halfX,float centerY,float halfY)
{
    // Child BoxCollider2D regions (local values) under the placement scale.
    halfX*=actor.size.x;centerY*=actor.size.y;halfY*=actor.size.y;
    const Rect range{actor.position.x-halfX,actor.position.y+centerY-halfY,actor.position.x+halfX,actor.position.y+centerY+halfY};
    return overlap(range,bodyBounds(hero.position));
}
// A PolygonCollider2D hitbox in the actor's local space (Unity mirrors X by the signed scale).
template<std::size_t N> bool strikes(const EnemyCActor& actor,const Hero& hero,const float (&local)[N][2])
{
    Vec2 world[N];
    for(std::size_t i=0;i<N;++i)world[i]={actor.position.x+local[i][0]*actor.scaleX,actor.position.y+local[i][1]*actor.size.y};
    return polygonOverlapsBox(world,N,bodyBounds(hero.position));
}
bool bullyReady(const EnemyCActor& actor)
{ return actor.kind==EnemyCKind::HuskBully&&(actor.phase==EnemyCPhase::Idle||actor.phase==EnemyCPhase::Move); }
bool baldurAlert(const EnemyCActor& actor,const Hero& hero,const World& world)
{
    // build22529139 Roller 3: Alert Range New Transform4589/BoxCollider9150;
    // all eleven ShamanTemple placements have these dimensions and offset.
    const Rect range=boxBounds(actor.position,{-10.57f,-.64f*actor.size.y,10.57f,1.26f*actor.size.y},actor.scaleX);
    return overlap(range,bodyBounds(hero.position))&&lineOfSight(actor.position,hero.position,world);
}
}
const EnemyCProfile& enemyCProfile(EnemyCKind kind){
    static_assert(sizeof(profiles)/sizeof(profiles[0])==unsigned(EnemyCKind::Count),"Enemy C profile identities");
    return profiles[std::min(unsigned(kind),unsigned(EnemyCKind::Count)-1)];
}
bool EnemyCEncounter::bind(const AnimationBank* bank,const NailBank* nails,EnemyRandom random)
{
    bank_=bank;nails_=nails;random_=random;clear();return bank_&&nails_&&nails_->ready();
}
void EnemyCEncounter::clear()
{
    actors={};objects={};shots={};globs={};mawlek={};drops={};dropCount=0;rejectedSpawns=0;
    for(unsigned i=0;i<capacity;++i){objects[i].colliderEnabled=false;receivers_[i]={this,i};}
    combat_.bind(objects.data(),capacity,nails_);
}
bool EnemyCEncounter::active()const
{
    for(const auto& a:actors)if(a.visible())return true;
    for(const auto& s:shots)if(s.active)return true;
    return false;
}
void EnemyCEncounter::play(unsigned i,const char* clip)
{
    if(!bank_)return;
    if(actors[i].kind==EnemyCKind::Sentry&&!std::strcmp(clip,"Antic"))clip="Attack";
    char name[48];std::snprintf(name,sizeof(name),"%s/%s",enemyCProfile(actors[i].kind).prefix,clip);
    actors[i].animation.play(*bank_,name,0,true);
}
void EnemyCEncounter::enter(unsigned i,EnemyCPhase phase,float seconds,const char* clip)
{auto& a=actors[i];a.phase=phase;a.timer=seconds;play(i,clip);}
bool EnemyCEncounter::spawn(EnemyCKind kind,Vec2 p,float sourceDepth,Vec2 scale)
{
    if(unsigned(kind)>=unsigned(EnemyCKind::Count)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(sourceDepth))return false;
    // Glowing Womb is a charm; reserve its task identity until clarified.
    if(kind==EnemyCKind::GlowingWomb)return false;
    if((kind==EnemyCKind::Hatchling||kind==EnemyCKind::AspidMother)&&!random_.unit&&!random_.sample)return false;
    static constexpr unsigned cageOwners[]={237,1000,790,1010,643,492,808,863,79,929,813,363,588,738,876};
    if(kind==EnemyCKind::AspidMother){
        unsigned free=0;
        for(const auto& actor:actors){
            free+=!actor.occupied();
            if(actor.cageOwner)return false;
        }
        if(free<1+sizeof(cageOwners)/sizeof(*cageOwners)){++rejectedSpawns;return false;}
    }
    unsigned i=0;while(i<capacity&&actors[i].occupied())++i;
    if(i==capacity){++rejectedSpawns;return false;}
    auto& a=actors[i];a={};a.kind=kind;a.position=a.home=p;a.presentation.reset(p);
    if(std::isfinite(scale.x)&&std::isfinite(scale.y)&&scale.x!=0&&scale.y!=0){
        a.size={std::abs(scale.x),std::abs(scale.y)};a.face(scale.x);
    }
    if(!++nextAudioSerial_)++nextAudioSerial_;
    a.audioSerial=nextAudioSerial_;
    a.audioDepth.activate(sourceDepth,.006f,0,a.audioSerial);
    auto& o=objects[i];o={};o.hasHealth=true;o.health.hp=enemyCProfile(kind).hp;
    o.name=enemyCProfile(kind).prefix;o.receiver=&receivers_[i];o.onHit=receive;o.bounds=a.bounds();
    enter(i,EnemyCPhase::Idle,kind==EnemyCKind::AspidMother?1.2f:.5f,"Idle");
    if(kind==EnemyCKind::Hatchling)a.hatchling.update(0,a.velocity,a.scaleX);
    if(kind==EnemyCKind::AspidMother){
        unsigned child=0;
        for(unsigned index=i+1;index<capacity&&child<sizeof(cageOwners)/sizeof(*cageOwners);++index){
            if(actors[index].occupied())continue;
            auto& baby=actors[index];
            baby={};baby.kind=EnemyCKind::Hatchling;baby.cageOwner=cageOwners[child++];
            if(!++nextAudioSerial_)++nextAudioSerial_;
            baby.audioSerial=nextAudioSerial_;
            baby.audioDepth.activate(.375f,.006f,0,baby.audioSerial);
            returnToCage(index);
        }
    }
    return true;
}
float EnemyCEncounter::roll(float low,float high)
{
    if(random_.unit||random_.sample)return random_.range(low,high);
    randomState_^=randomState_<<13;randomState_^=randomState_>>17;randomState_^=randomState_<<5;
    return low+(high-low)*float(randomState_&0xffffff)/float(0x1000000);
}
void EnemyCEncounter::returnToCage(unsigned index)
{
    auto& baby=actors[index];
    baby.phase=EnemyCPhase::Caged;baby.position=baby.home={100,100};baby.velocity={};
    baby.presentation.reset(baby.position);baby.hatchling={};baby.flash=0;
    auto& object=objects[index];
    object={};object.hasHealth=true;object.health.hp=enemyCProfile(baby.kind).hp;
    object.name=enemyCProfile(baby.kind).prefix;object.receiver=&receivers_[index];object.onHit=receive;
    object.bounds=baby.bounds();object.colliderEnabled=false;
    if(!baby.animation.clip)play(index,"Idle");
}
bool EnemyCEncounter::releaseFromCage(Vec2 position)
{
    std::array<unsigned,capacity> choices{};
    unsigned count=0;
    for(unsigned index=0;index<capacity;++index)
        if(actors[index].phase==EnemyCPhase::Caged)choices[count++]=index;
    if(!count)return false;
    const unsigned index=choices[std::min(count-1,unsigned(random_.range(0,float(count))))];
    auto& baby=actors[index];
    baby.position=baby.home=position;baby.velocity={0,-5};
    baby.presentation.reset(position);baby.phase=EnemyCPhase::Move;
    objects[index].bounds=baby.bounds();objects[index].colliderEnabled=true;
    return true;
}
void EnemyCEncounter::receive(void* context,const NailHit& hit,HitOutcome outcome)
{auto& r=*static_cast<Receiver*>(context);r.encounter->hit(r.index,hit,outcome);}
void EnemyCEncounter::hit(unsigned i,const NailHit& hit,HitOutcome outcome)
{
    auto& a=actors[i];
    // Shroom Brawler: BLOCKED HIT -> Bounce from Idle, Turn L/R, Spit and Spit CD.
    if(a.alive()&&outcome==HitOutcome::Blocked&&a.kind==EnemyCKind::MushroomBrawler&&
       (a.step==BrIdle||a.step==BrTurn||a.step==BrSpit||a.step==BrSpitCd)){a.step=BrBounce;a.clipTime=0;play(i,"Bounce");}
    if(!a.alive()||(outcome!=HitOutcome::Damaged&&outcome!=HitOutcome::Killed))return;
    ++a.injurySerial;++injurySerial;a.flash=.12f;
    // Ruins Sentry Idle: TOOK DAMAGE -> Init Chase.
    if((a.kind==EnemyCKind::Sentry||a.kind==EnemyCKind::HeavySentry)&&a.step==Patrol&&outcome==HitOutcome::Damaged){
        a.alerted=true;a.velocity.x=0;a.face(hero_.x<=a.position.x?1.f:-1.f);
        a.step=Startle;a.clipTime=0;play(i,"Startle");
    }
    if(outcome==HitOutcome::Damaged&&a.kind==EnemyCKind::HuskBully){
        // Zombie Swipe: TOOK DAMAGE in Ready faces the hero and attacks; the following
        // RECOIL HORIZONTAL (global -> Reset) cancels the attack and restarts the walker.
        if(bullyReady(a)){a.face(hero_.x<=a.position.x?1.f:-1.f);a.velocity={};enter(i,EnemyCPhase::Windup,.4f,"Antic");}
        if((hit.direction==Strike::Left||hit.direction==Strike::Right)&&
           (a.phase==EnemyCPhase::Windup||a.phase==EnemyCPhase::Attack||a.phase==EnemyCPhase::Recover))
            enter(i,EnemyCPhase::Move,random_.range(1.5f,4.f),"Move");
    }
    if(a.kind==EnemyCKind::FlyingSentry||a.kind==EnemyCKind::Baldur){
        // Roller: RECOIL HORIZONTAL during a roll -> Recoil Decel (roll speed 0). Recoil speed 25.
        if(a.kind==EnemyCKind::Baldur&&a.phase==EnemyCPhase::Move&&(hit.direction==Strike::Left||hit.direction==Strike::Right))
            a.velocity.x=0;
        // TOOK DAMAGE: Wall -> Wake, Idle Buzz -> Idle. Recoil (speed 15, 0.15 s) when ready.
        if(a.kind!=EnemyCKind::FlyingSentry){}
        else if(outcome==HitOutcome::Damaged&&a.step==FlyWall){a.step=FlyWake;a.clipTime=0;play(i,"Wall Startle");}
        else if(outcome==HitOutcome::Damaged&&a.step==FlyBuzz){a.step=FlyIdle;a.clipTime=0;a.timer=roll(1.5f,2.f);play(i,"Idle");}
        if(a.recoilTime<=0){
            a.recoilTime=.15f;a.recoilMagnitude=hit.recoilMultiplier;a.recoilSweeping=true;
            a.recoilDirection=hit.direction==Strike::Right?Vec2{1,0}:hit.direction==Strike::Left?Vec2{-1,0}:
                              hit.direction==Strike::Up?Vec2{0,1}:Vec2{0,-1};
        }
    }else a.velocity.x=(hit.direction==Strike::Right?6:hit.direction==Strike::Left?-6:0)*hit.recoilMultiplier;
    if(outcome==HitOutcome::Killed){
        if(a.cageOwner){++a.killSerial;++killSerial;returnToCage(i);return;}
        ++a.killSerial;++killSerial;objects[i].colliderEnabled=false;a.velocity.y=8;
        enter(i,EnemyCPhase::Dead,1.1f,"Death");
        if(dropCount<capacity)drops[dropCount++]={a.position,enemyCProfile(a.kind).geo,a.kind,i};
    }
}
void EnemyCEncounter::shoot(Vec2 p,Vec2 v)
{
    // Shot Mawlek NoDrip: ScaleWithVelocity scale 1.15-1.45, Idle clip.
    for(auto& s:shots)if(!s.active){
        s={};s.active=true;s.position=p;s.velocity=v;s.presentation.reset(p);s.scale=roll(1.15f,1.45f);
        if(bank_)s.animation.play(*bank_,"shot/Idle",0,true);
        return;
    }
    ++rejectedSpawns;
}
void EnemyCEncounter::motherIdle(EnemyCActor& a,float dt)
{
    // Crossroads_19 Hatcher/Idle: IdleBuzz.DoBuzz (build22529139).
    // Acceleration is per callback, not multiplied by deltaTime. Boundary
    // damping and RNG order are Y, X, then direction expiry Y/X/wait.
    constexpr float acceleration=15.f/2000.f;
    const auto sampleIdle=[this](float low,float high){
        return random_.hatcherIdleSample?random_.hatcherIdleSample(random_.context,low,high):random_.range(low,high);
    };
    auto boundary=[&](float position,float home,float& velocity,float& accel){
        if((position < home-1&&velocity < 0)||(position > home+1&&velocity > 0)){
            accel=position < home-1?acceleration:-acceleration;
            velocity/=1.125f;a.motherIdleWait=sampleIdle(.75f,1.f);
        }
    };
    boundary(a.position.y,a.home.y,a.velocity.y,a.motherIdleAcceleration.y);
    boundary(a.position.x,a.home.x,a.velocity.x,a.motherIdleAcceleration.x);
    if(a.motherIdleWait<=std::numeric_limits<float>::denorm_min()){
        auto direction=[&](float position,float home){
            return sampleIdle(position < home-1?0.f:-15.f,position > home+1?0.f:15.f)/2000.f;
        };
        a.motherIdleAcceleration.y=direction(a.position.y,a.home.y);
        a.motherIdleAcceleration.x=direction(a.position.x,a.home.x);
        a.motherIdleWait=sampleIdle(.75f,1.f);
    }
    if(a.motherIdleWait>std::numeric_limits<float>::denorm_min())a.motherIdleWait-=dt;
    a.velocity.x=std::clamp(a.velocity.x+a.motherIdleAcceleration.x,-1.75f,1.75f);
    a.velocity.y=std::clamp(a.velocity.y+a.motherIdleAcceleration.y,-1.75f,1.75f);
}
void EnemyCEncounter::fixedUpdate(float dt,Hero& hero,const World& world)
{
    if(!active()||dt<=0||!std::isfinite(dt))return;
    for(unsigned i=0;i<capacity;++i){
        auto& a=actors[i];if(!a.visible())continue;
        PresentationPosition::Step history(a.presentation,a.position);
        const auto& p=enemyCProfile(a.kind);if(a.kind!=EnemyCKind::AspidMother||!a.alive())a.timer-=dt;
        a.age+=dt;a.flash=std::max(0.f,a.flash-dt);
        if(a.phase==EnemyCPhase::Dead){
            a.velocity.y-=30*dt;moveBox(a.position,a.velocity,a.body(),dt,world);
            if(a.timer<=0)a.phase=EnemyCPhase::Gone;
            continue;
        }
        const float dx=hero.position.x-a.position.x,dy=hero.position.y-a.position.y;
        const float facing=sign(dx);hero_=hero.position;
        if(a.kind==EnemyCKind::HuskBully){
            // Zombie Barger: Walker (speed 1.5, walk 1.5-4 s, pause 1-1.5 s) beside the Zombie
            // Swipe FSM (husk_bully_candidate level40-5099). Ready fires on Attack Range + line
            // of sight from any walker state: Attack Anticipate 4 @ 10 fps, Attack Lunge 2 @ 10
            // at Lunge Speed 14, Attack Cooldown 1 @ 10, Idle Time 0.25, then StartWalker.
            if(bullyReady(a)&&bullyAlert(a,hero,world)){
                a.face(dx<=0?1.f:-1.f);a.velocity={};enter(i,EnemyCPhase::Windup,.4f,"Antic");
            }else if(a.phase==EnemyCPhase::Idle&&a.timer<=0)enter(i,EnemyCPhase::Move,random_.range(1.5f,4.f),"Move");
            else if(a.phase==EnemyCPhase::Move&&a.timer<=0)enter(i,EnemyCPhase::Idle,random_.range(1.f,1.5f),"Idle");
            else if(a.phase==EnemyCPhase::Windup&&a.timer<=0){enter(i,EnemyCPhase::Attack,.2f,"Attack");++a.attacks;}
            else if(a.phase==EnemyCPhase::Attack&&a.timer<=0)enter(i,EnemyCPhase::Recover,.1f+.25f,"Recover");
            else if(a.phase==EnemyCPhase::Recover&&a.timer<=.25f&&a.timer+dt>.25f)play(i,"Idle");
            else if(a.phase==EnemyCPhase::Recover&&a.timer<=0)enter(i,EnemyCPhase::Move,random_.range(1.5f,4.f),"Move");
            a.velocity.x=a.phase==EnemyCPhase::Move?-a.scaleX*p.speed:a.phase==EnemyCPhase::Attack?-a.scaleX*14:0;
        }else if(a.kind==EnemyCKind::Baldur){
            // Crossroads_ShamanTemple Roller (level76-1108) FSM "Roller": Idle faces the hero (sprite
            // faces left) until Alert Range New + sight; Start (4 @ 10) with Roll time 2-3 s; Roll
            // accelerates 0.45 per frame to Max Speed 11; a side wall bounces it off at 12 (115 / 65
            // degrees) into In Air, and landing rolls on the other way; RECOIL HORIZONTAL zeroes the
            // roll speed; Roll time <= 0 -> Stop (4 @ 10) -> Rest (Idle, 0.5 s).
            a.airPause=std::max(0.f,a.airPause-dt);
            const float rolling=a.facingRight?1.f:-1.f;
            if(a.phase==EnemyCPhase::Idle){
                a.velocity.x=0;
                if(dx!=0)a.face(-facing);
                if(a.airPause<=0&&baldurAlert(a,hero,world)){
                    a.facingRight=dx>=0;a.face(a.facingRight?-1.f:1.f);
                    enter(i,EnemyCPhase::Windup,.4f,"Antic");a.jumpSpeed=roll(2,3);++a.attacks;
                }
            }else if(a.phase==EnemyCPhase::Windup){
                a.velocity.x=0;
                if(a.timer<=0)enter(i,EnemyCPhase::Move,0,"Move");
            }
            if(a.phase==EnemyCPhase::Move||a.phase==EnemyCPhase::Jump)a.jumpSpeed-=dt;
            if(a.phase==EnemyCPhase::Move){
                a.face(a.facingRight?-1.f:1.f);
                a.velocity.x=std::clamp(a.velocity.x+rolling*.45f*dt*60,-11.f,11.f);
                if(a.jumpSpeed<=0){a.velocity={};enter(i,EnemyCPhase::Recover,.4f,"Recover");}
            }else if(a.phase==EnemyCPhase::Recover&&a.timer<=0){a.airPause=.5f;enter(i,EnemyCPhase::Idle,0,"Idle");}
            if(a.recoilTime>0){
                // Recoil.UpdatePhysics: sweep-clipped Translate (25 x magnitude), velocity untouched.
                Vec2 transform=a.position;
                if(a.recoilSweeping&&translateRecoil(a.position,transform,a.body(),a.recoilDirection,25*a.recoilMagnitude*dt,world))
                    a.recoilSweeping=false;
                a.recoilTime-=dt;
            }
        }else if(a.kind==EnemyCKind::AspidMother){
            if(!a.motherAlerted){
                if(a.motherIdleStarted)motherIdle(a,dt);
            }else{
                if(a.phase==EnemyCPhase::Move){
                    a.face(-facing);
                    a.velocity.x=std::clamp(a.velocity.x+(std::abs(dx)>6?facing:-facing)*.1f,-3.5f,3.5f);
                    a.velocity.y=std::clamp(a.velocity.y+(dy+3.5f>0?.1f:-.1f),-3.5f,3.5f);
                }else a.velocity={};
            }
        }else if(a.kind==EnemyCKind::Hatchling){
            a.hatchling.fixedUpdate(dt,a.position,hero.position,a.velocity,random_);
            if(a.phase==EnemyCPhase::Idle)enter(i,EnemyCPhase::Move,100,"Move");
        }else if(a.kind==EnemyCKind::Sentry){
            // Walker 2.4 without pauses; Run 7.5, Dash 23, Evade 15. Regions (child scale 0.8):
            // Alert 20.51 x 5.70 at Y -0.538, Unalert 31.9 x 5.97 at Y -0.673, Attack 11.2 x 5.12 at
            // Y -0.246, Evade 7.14 x 5.88 at Y -4.377.
            const bool alertRegion=sentryRegion(a,hero,20.51f*.4f,-.5381727f*.8f,5.7015696f*.4f);
            const bool unalertRegion=sentryRegion(a,hero,31.900003f*.4f,-.6734419f*.8f,5.97203f*.4f);
            const bool attackRegion=sentryRegion(a,hero,11.2f*.4f,-.2456379f*.8f,5.1161146f*.4f);
            const bool evadeRegion=sentryRegion(a,hero,7.14f*.4f,-4.377079f*.8f,5.881938f*.4f);
            const float forward=-sign(a.scaleX);
            a.clipTime+=dt;
            const auto step=[&](SentryStep next,const char* clip){a.step=next;a.clipTime=0;if(clip)play(i,clip);};
            const auto frame=[&](float n){return a.clipTime>=n/24.f;};
            const auto groundAhead=[&]{
                float hit=0;
                return rayTerrain({a.position.x+forward*.8f,a.position.y-1.4f},{0,-1},1.f,world,hit,TerrainShape::terrainQueryExcluded);
            };
            // Slash1 / Slash2 PolygonCollider2D points (Ruins1_17), damage 1.
            static constexpr float slash1[][2]={{-2.9579468f,.33481598f},{-.5774536f,1.5261612f},{.24812317f,-.8231735f},
                {-2.4135132f,-.93639374f},{-3.22966f,-.34201813f}};
            static constexpr float slash2[][2]={{-2.7882538f,.66524506f},{-.7097473f,.9012985f},{2.2892456f,.6331024f},
                {-.32206726f,-.6298981f},{-2.6810913f,-.34092712f},{-3.628067f,-.016548157f},{-3.4350586f,.33670044f}};
            const auto swipe=[&](bool second){
                if(second?strikes(a,hero,slash2):strikes(a,hero,slash1))hero.takeDamage(1,forward>0?-1:1);
            };
            const auto attackChoice=[&]{
                // Attack Choice: SINGLE 0.6, TRIPLE 0.4; a triple first evades half the time if there
                // is ground behind (Evade Check ray), else it attacks at once.
                a.velocity.x=0;++a.attacks;
                if(roll(0,1)<.6f)step(Single,"Attack 01");
                else if(roll(0,1)<.5f){
                    float hit=0;
                    if(rayTerrain({a.position.x-forward*1.5f,a.position.y-1.4f},{0,-1},1.f,world,hit,TerrainShape::terrainQueryExcluded))step(Evade,"Evade");
                    else step(Single,"Attack 01");
                }else step(Triple,"Attack 03");
            };
            switch(SentryStep(a.step)){
            case Patrol:
                if(a.phase==EnemyCPhase::Idle){a.phase=EnemyCPhase::Move;play(i,"Walk");}
                if(!groundAhead())a.scaleX=-a.scaleX;
                a.velocity.x=-sign(a.scaleX)*2.4f;
                if(alertRegion){a.alerted=true;a.velocity.x=0;a.face(dx<0?1.f:-1.f);step(Startle,"Startle");}
                break;
            case Startle:a.velocity.x=0;if(frame(4*2))step(Run,"Run");break;
            case Run:
                if(dx*forward<0){a.velocity.x=0;step(Turn,"Turn B");break;}
                if(evadeRegion){step(Evade,"Evade");break;}
                if(!unalertRegion){a.velocity.x=0;step(RunEnd,"Run End");break;}
                if(attackRegion){attackChoice();break;}
                // Run Safe: a ray 1 unit down from the Run Check point; no ground stops the run.
                a.velocity.x=groundAhead()?forward*p.speed:0;
                break;
            case Turn:
                a.velocity.x=0;
                if(frame(2*2)){a.scaleX=-a.scaleX;step(unalertRegion?Run:Cooldown,unalertRegion?"Run":"Idle");if(a.step==Cooldown)a.timer=roll(2,3);}
                break;
            case RunEnd:
                a.velocity.x=0;
                if(unalertRegion)step(Run,"Run");
                else if(frame(2*2)){step(Cooldown,"Idle");a.timer=roll(2,3);}
                break;
            case Cooldown:
                a.velocity.x=0;
                if(unalertRegion)step(Run,"Run");
                else if(a.timer<=0){a.alerted=false;step(Patrol,"Walk");}
                break;
            case Single:
                // Attack 01 (24 fps, 20 frames, triggers 8/9/10): antic, Dash 23 from 8, Slash1 9-10.
                a.velocity.x=frame(8)&&!frame(10)?forward*23:0;
                if(frame(9)&&!frame(10))swipe(false);
                if(frame(20)){step(AttackCd,"Idle");a.timer=roll(.25f,.5f);}
                break;
            case Triple:
                // Attack 03 (24 fps, 35 frames, triggers 9,10,11,16,17,22,23,24): dash 9-11, Slash1 10-11,
                // Slash2 16-17, dash 22-24, Slash1 23-24.
                a.velocity.x=(frame(9)&&!frame(11))||(frame(22)&&!frame(24))?forward*23:0;
                if((frame(10)&&!frame(11))||(frame(23)&&!frame(24)))swipe(false);
                if(frame(16)&&!frame(17))swipe(true);
                if(frame(35)){step(AttackCd,"Idle");a.timer=roll(.25f,.5f);}
                break;
            case Evade:
                // Evade (24 fps, 12 frames, triggers 4/10): backward at 15 between the triggers.
                a.velocity.x=frame(4)&&!frame(10)?-forward*15:0;
                if(frame(12)){step(EvadeCool,"Idle");a.timer=.3f;}
                break;
            case EvadeCool:
                a.velocity.x=0;
                if(a.timer<=0){if(roll(0,1)<.5f){++a.attacks;step(Triple,"Attack 03");}else step(Run,"Run");}
                break;
            case AttackCd:
                a.velocity.x=0;
                if(a.timer<=0){if(unalertRegion)step(Run,"Run");else{step(Cooldown,"Idle");a.timer=roll(2,3);}}
                break;
            default:break;
            }
        }else if(a.kind==EnemyCKind::HeavySentry){
            // "Ruins Sentry Fat": Walker 2.4, Run 7.5, Charge 14. Regions (scale 0.8): Attack 8.48 x 5.85 at
            // Y 0.123, Alert 23.55 x 7.14 at Y -0.473, Unalert 31.9 wide, Air 13.8 x 7.81 at Y -0.855,
            // Charge 6.26 x 5.85 centred 8.77 ahead.
            const float forward=-sign(a.scaleX);
            const bool attackRegion=sentryRegion(a,hero,8.48f*.4f,.12308121f*.8f,5.85352f*.4f);
            const bool alertRegion=sentryRegion(a,hero,23.55f*.4f,-.47326088f*.8f,7.142546f*.4f);
            const bool unalertRegion=sentryRegion(a,hero,31.900003f*.4f,-.47326088f*.8f,7.142546f*.4f);
            const bool airRegion=sentryRegion(a,hero,13.8f*.4f,-.85463524f*.8f,7.8091917f*.4f);
            EnemyCActor ahead=a;ahead.position.x+=forward*8.77f*a.size.x;
            const bool chargeRegion=sentryRegion(ahead,hero,6.26f*.4f,.12308121f*.8f,5.85352f*.4f);
            a.clipTime+=dt;
            const auto step=[&](SentryStep next,const char* clip){a.step=next;a.clipTime=0;if(clip)play(i,clip);};
            const auto groundAhead=[&]{
                float hit=0;
                return rayTerrain({a.position.x+forward*1.f,a.position.y-2.2f},{0,-1},1.f,world,hit,TerrainShape::terrainQueryExcluded);
            };
            // Ground Slash and Charge Hit PolygonCollider2D points (Ruins1_05), damage 1.
            static constexpr float groundSlash[][2]={{-2.377472f,1.363617f},{-.9254608f,2.2633133f},{.6154175f,2.3788147f},
                {-1.4540482f,-2.2354965f},{-3.7204514f,-2.263466f},{-3.4683762f,-.2503891f}};
            static constexpr float chargeHit[][2]={{-.9278946f,-.88347006f},{-.6761131f,-1.5328505f},{-2.3151588f,-1.322422f}};
            const auto slash=[&]{if(strikes(a,hero,groundSlash))hero.takeDamage(1,forward>0?-1:1);};
            const auto jump=[&]{
                // Get Jump Distance: a ceiling within 5 cancels to Single; then 40% evade (2 units away),
                // else (Hero X -/+ 2.7 - Self X) * 1.9 toward the hero, launched at Y 30.
                float hit=0;a.velocity.x=0;
                if(rayTerrain(a.position,{0,1},5,world,hit,TerrainShape::terrainQueryExcluded)){step(Single,"Attack");return;}
                const bool right=hero.position.x>=a.position.x;
                if(roll(0,1)<.4f)a.jumpSpeed=right?-2.f:2.f;
                else a.jumpSpeed=(hero.position.x+(right?-2.7f:2.7f)-a.position.x)*1.9f;
                a.face(dx<0?1.f:-1.f);
                step(JumpAntic,"Jump Antic");
            };
            const auto attackChoice=[&]{
                // Attack Choice: SINGLE 0.44, CHARGE 0.28, JUMP 0.28.
                a.velocity.x=0;++a.attacks;
                const float r=roll(0,1);
                if(r<.44f){a.canCharge=true;step(Single,"Attack");}
                else if(r<.72f)step(ChargeAntic,"Charge Antic");
                else jump();
            };
            if(a.airPause>0)a.airPause=std::max(0.f,a.airPause-dt);
            switch(SentryStep(a.step)){
            case Patrol:
                if(a.phase==EnemyCPhase::Idle){a.phase=EnemyCPhase::Move;play(i,"Walk");}
                if(!groundAhead())a.scaleX=-a.scaleX;
                a.velocity.x=-sign(a.scaleX)*2.4f;
                if(alertRegion){a.alerted=true;a.velocity.x=0;a.face(dx<0?1.f:-1.f);step(Startle,"Startle");}
                break;
            case Startle:a.velocity.x=0;if(a.clipTime>=4.f/12)step(Run,"Run");break;
            case Run:
                if(dx*forward<0){a.velocity.x=0;step(Turn,"Turn B");break;}
                if(!unalertRegion){a.velocity.x=0;step(RunEnd,"Idle");break;}
                if(attackRegion){attackChoice();break;}
                if(airRegion&&a.airPause<=0){
                    // Air Attack?: a ceiling within 5 ends it; else 50% jump attack. Either way 1 s pause.
                    float hit=0;a.airPause=1;
                    if(!rayTerrain(a.position,{0,1},5,world,hit,TerrainShape::terrainQueryExcluded)&&roll(0,1)<.5f){++a.attacks;jump();break;}
                }
                if(a.canCharge&&chargeRegion){a.velocity.x=0;++a.attacks;step(ChargeAntic,"Charge Antic");break;}
                a.velocity.x=groundAhead()?forward*p.speed:0;
                break;
            case Turn:
                a.velocity.x=0;
                if(a.clipTime>=2.f/12){a.scaleX=-a.scaleX;if(unalertRegion)step(Run,"Run");else{step(Cooldown,"Idle");a.timer=roll(2,3);}}
                break;
            case RunEnd:
            case Cooldown:
                a.velocity.x=0;
                if(alertRegion)step(Run,"Run");
                else if(a.step==RunEnd){step(Cooldown,"Idle");a.timer=roll(2,3);}
                else if(a.timer<=0){a.alerted=false;step(Patrol,"Walk");}
                break;
            case Single:
                // Attack (24 fps, 16 frames, triggers 10/11): Ground Slash 10-11.
                a.velocity.x=0;
                if(a.clipTime>=10.f/24&&a.clipTime<11.f/24)slash();
                if(a.clipTime>=16.f/24){step(AttackCd,"Idle");a.timer=roll(.25f,.5f);}
                break;
            case JumpAntic:
                a.velocity.x=0;
                if(a.clipTime>=4.f/12){a.velocity={a.jumpSpeed,30};step(Air,"Jump Air");}
                break;
            case Air:
                a.velocity.x=a.jumpSpeed;
                break;
            case Land:
                // Jump Attack (18 fps, 6 frames, triggers 1/2): Ground Slash 1-2.
                a.velocity.x=0;
                if(a.clipTime>=1.f/18&&a.clipTime<2.f/18)slash();
                if(a.clipTime>=6.f/18){step(AttackCd,"Idle");a.timer=roll(.25f,.5f);}
                break;
            case ChargeAntic:
                // Charge Antic (8 @ 15 fps); no ground ahead cancels to Single.
                a.velocity.x=0;a.canCharge=false;
                if(!groundAhead())step(Single,"Attack");
                else if(a.clipTime>=8.f/15)step(Charge,"Charge");
                break;
            case Charge:
                // Charge at 14 for 1 s (Charge Hit active); no ground ahead ends it at once.
                a.velocity.x=forward*14;
                if(strikes(a,hero,chargeHit))hero.takeDamage(1,forward>0?-1:1);
                if(!groundAhead()){a.velocity.x=0;step(ChargeEnd,"Charge End");}
                else if(a.clipTime>=1){step(ChargeEnd,"Charge End");}
                break;
            case ChargeEnd:
                // DecelerateXY 0.85 through Charge End (5 @ 12 fps).
                a.velocity.x*=.85f;
                if(a.clipTime>=5.f/12){a.velocity.x=0;step(AttackCd,"Idle");a.timer=roll(.25f,.5f);}
                break;
            case AttackCd:
                a.velocity.x=0;
                if(a.timer<=0){if(unalertRegion)step(Run,"Run");else{step(Cooldown,"Idle");a.timer=roll(2,3);}}
                break;
            default:break;
            }
        }else if(a.kind==EnemyCKind::FlyingSentry){
            // Ruins Flying Sentry (Ruins1_05 level92-215) FSM "Flying Sentry Nail" with "Lost Hero
            // Check". Clips (sharedassets90 library 152): Wall Startle 2 @ 12, Slash Antic 7 @ 20,
            // Slash Forward 1 @ 20, Slash 1 @ 20, Slash Recover 5 @ 12; Turn To Idle loops a section.
            // Regions are circles against the hero collider, scaled by the placement: Wake Region
            // 9.34 and Lost hero Region 12.32 x 1.1898848 (battle-wave B: 18.2 and 24.42 x 1.1898848).
            const Rect heroBox=bodyBounds(hero.position);
            const auto inCircle=[&](float radius){
                radius*=std::max(a.size.x,a.size.y);
                const float nx=std::clamp(a.position.x,heroBox.left,heroBox.right)-a.position.x;
                const float ny=std::clamp(a.position.y,heroBox.bottom,heroBox.top)-a.position.y;
                return nx*nx+ny*ny<=radius*radius;
            };
            const bool wakeRegion=inCircle(a.battleRegions?18.2f:9.34f);
            const bool lostRegion=inCircle((a.battleRegions?24.42f:12.32f)*1.1898848f);
            // Lost Hero Check: Out counts seconds (clamped 0-1000), In resets.
            a.lostCounter=lostRegion?0:std::min(1000.f,a.lostCounter+dt);
            a.clipTime+=dt;a.turnPause=std::max(0.f,a.turnPause-dt);
            const auto step=[&](FlyingStep next,const char* clip){a.step=next;a.clipTime=0;if(clip)play(i,clip);};
            // DistanceFly: per fixed step +-acceleration per axis toward (beyond `range`) or away
            // from the hero; Y aims at Hero Y + height; both clamp to speedMax 6.
            const auto distanceFly=[&](float range,float height){
                const float distance=std::hypot(dx,dy),acceleration=.3f;
                const float x=distance>range?acceleration:-acceleration;
                a.velocity.x+=a.position.x<hero.position.x?x:-x;
                if(a.position.y<hero.position.y+height)a.velocity.y+=acceleration;
                if(a.position.y>hero.position.y+height)a.velocity.y-=acceleration;
                a.velocity.x=std::clamp(a.velocity.x,-6.f,6.f);a.velocity.y=std::clamp(a.velocity.y,-6.f,6.f);
            };
            // FaceObject (sprite faces left) with Turn To Idle on a change of facing.
            const auto faceHero=[&]{
                const float scale=dx>0?-a.size.x:a.size.x;
                if(dx!=0&&scale!=a.scaleX){a.scaleX=scale;play(i,"Turn To Idle");}
            };
            const auto idle=[&]{step(FlyIdle,"Idle");a.timer=roll(1.5f,2.f);distanceFly(8,4.25f);faceHero();};
            const auto buzzStart=[&]{
                // IdleBuzz OnEnter keeps the current position as the roaming origin.
                a.home=a.position;a.buzzWait=0;a.buzzAcceleration={};step(FlyBuzz,"Idle");
            };
            const auto position=[&]{step(FlyPosition,nullptr);a.timer=8;distanceFly(5,-.5f);faceHero();};
            if(!a.fsmStarted){
                a.fsmStarted=true;a.velocity={};
                if(a.startIdle)buzzStart();else{step(FlyWall,"Wall Rest");}
            }
            switch(FlyingStep(a.step)){
            case FlyWall:
                a.velocity={};
                if(wakeRegion)step(FlyWake,"Wall Startle");
                break;
            case FlyWake:
                // Wake (also TOOK DAMAGE on the wall): Kick Off Force 200 x (-scale X) as one
                // AddForce2d (mass 1) on the next fixed step, then Idle after Wall Startle.
                if(a.clipTime<1.5f*dt)a.velocity.x+=200*-a.scaleX*dt;
                if(a.clipTime>=2.f/12)idle();
                break;
            case FlyBuzz:{
                if(wakeRegion){idle();break;}
                // IdleBuzz waitMin .75 waitMax 1 speedMax 4 accelerationMax 20 roamingRange 2.
                constexpr float accelerationMax=20.f,range=2.f;
                const auto boundary=[&](float p,float origin,float& v,float& acc){
                    if((p<origin-range&&v<0)||(p>origin+range&&v>0)){acc=(p<origin?accelerationMax:-accelerationMax)/2000;v/=1.125f;a.buzzWait=roll(.75f,1.f);}
                };
                boundary(a.position.y,a.home.y,a.velocity.y,a.buzzAcceleration.y);
                boundary(a.position.x,a.home.x,a.velocity.x,a.buzzAcceleration.x);
                if(a.buzzWait<=0){
                    const auto sample=[&](float p,float origin){return roll(p<origin-range?0.f:-accelerationMax,p>origin+range?0.f:accelerationMax)/2000;};
                    a.buzzAcceleration.y=sample(a.position.y,a.home.y);a.buzzAcceleration.x=sample(a.position.x,a.home.x);
                    a.buzzWait=roll(.75f,1.f);
                }
                if(a.buzzWait>0)a.buzzWait-=dt;
                a.velocity.x=std::clamp(a.velocity.x+a.buzzAcceleration.x,-4.f,4.f);
                a.velocity.y=std::clamp(a.velocity.y+a.buzzAcceleration.y,-4.f,4.f);
                // FaceDirection with Turn To Idle and a 0.5 s pause between turns.
                if(a.velocity.x!=0&&a.turnPause<=0){
                    const float scale=a.velocity.x>0?-a.size.x:a.size.x;
                    if(scale!=a.scaleX){a.scaleX=scale;a.turnPause=.5f;play(i,"Turn To Idle");}
                }
                break;
            }
            case FlyIdle:
                distanceFly(8,4.25f);faceHero();
                // WaitRandom 1.5-2 -> Lost Check: Lost Counter > 6 -> Idle Buzz, else Position.
                if(a.timer<=0){if(a.lostCounter>6)buzzStart();else position();}
                break;
            case FlyPosition:{
                distanceFly(5,-.5f);faceHero();
                const bool inSlashRange=std::hypot(dx,dy)<5.25f,sameY=std::abs(dy)<=.75f;
                if(inSlashRange&&sameY){step(FlyAntic,"Slash Antic");distanceFly(4,-.5f);}
                // Wait 8 -> CANCEL -> Can See?: Wake Region -> Idle, else Idle Buzz.
                else if(a.timer<=0){if(wakeRegion)idle();else buzzStart();}
                break;
            }
            case FlyAntic:
                distanceFly(4,-.5f);
                if(a.clipTime>=7.f/20){
                    // Check Dir: scale X < 0 (facing right) -> 18, else -18; Y is set to 0.
                    a.velocity={a.scaleX<0?18.f:-18.f,0};
                    step(FlySlashForward,"Slash Forward");++a.attacks;
                }
                break;
            case FlySlashForward:
                if(a.clipTime>=1.f/20)step(FlySlash,"Slash");
                break;
            case FlySlash:{
                // Slash Hit (PolygonCollider2D, child of the scaled body) while the Slash clip plays.
                static constexpr float slashHit[][2]={{-1.6203461f,1.5279846f},{.39477539f,-.070137024f},
                    {-1.3429565f,-.36462402f},{-2.8532104f,-.33564758f},{-3.7010651f,.19233704f},{-3.1176147f,.87341309f}};
                if(strikes(a,hero,slashHit))hero.takeDamage(1,hero.position.x<a.position.x?1:-1);
                if(a.clipTime>=1.f/20)step(FlyRecover,"Slash Recover");
                break;
            }
            case FlyRecover:
                // DecelerateV2 0.95 per fixed step through Slash Recover, then Idle.
                a.velocity.x*=.95f;a.velocity.y*=.95f;
                if(a.clipTime>=5.f/12)idle();
                break;
            default:break;
            }
            if(a.recoilTime>0){
                // Recoil.UpdatePhysics: sweep-clipped Translate, velocity untouched.
                Vec2 transform=a.position;
                if(a.recoilSweeping&&translateRecoil(a.position,transform,flyingTerrainBox(a),a.recoilDirection,15*a.recoilMagnitude*dt,world))
                    a.recoilSweeping=false;
                a.recoilTime-=dt;
            }
        }else if(a.kind==EnemyCKind::MushroomBrawler){
            // Fungus2_05 Mushroom Brawler (level169-551) FSM "Shroom Brawler". Clips (sharedassets169
            // library 223): Wake 6, Run 5 loop, Turn 2, Spit 5 (trigger 2), Bounce 2, Antic 8,
            // Smash to Run 3 (12 fps); Smash Loop 16 @ 24 on Smash Hits (main renderer hidden).
            // Child boxes (local): Wake Region (0,-0.69)+(0,1.387) 16.98 x 6.40; Smash Region
            // (0,-0.69)+(0,2.535) 10.84 x 8.70; Smash Hits (-0.0703,-1.281) 3.33 x 2.22; Spit Point (-1.95,0.04).
            auto& health=objects[i].health;
            const Rect heroBox=bodyBounds(hero.position);
            const auto region=[&](float x,float y,float w,float h){
                x=a.position.x+x*a.scaleX;y=a.position.y+y*a.size.y;w*=a.size.x/2;h*=a.size.y/2;
                return overlap({x-w,y-h,x+w,y+h},heroBox);
            };
            a.clipTime+=dt;
            const auto step=[&](BrawlerStep next,const char* clip){a.step=next;a.clipTime=0;if(clip)play(i,clip);};
            const bool heroLeft=hero.position.x<a.position.x;
            // SetScale X 1 (left) / -1 (right) with Facing Right; speeds follow the facing.
            const auto setFacing=[&](bool right){a.facingRight=right;a.face(right?-1.f:1.f);};
            const auto idle=[&]{step(BrIdle,"Run");a.velocity.x=a.moveSpeed;a.timer=a.moveTime;};
            const auto setWait=[&]{a.waitTime=roll(1.5f,1.75f);a.waitTimer=0;};
            const auto changeMovement=[&]{
                // Move Time 1-2, Move Speed 1-3, then RIGHT -> Idle or LEFT: negate, Set Wait Time.
                a.moveTime=roll(1,2);a.moveSpeed=roll(1,3);
                if(roll(0,1)>=.5f){a.moveSpeed=-a.moveSpeed;setWait();}
                idle();
            };
            const auto spitAntic=[&]{
                a.waitTime=roll(1.25f,1.5f);
                if(std::hypot(dx,dy)>25){setWait();idle();}
                else if(region(0,-.69000244f+2.5347137f,10.84f,8.696884f)){a.velocity.x=0;step(BrSmashAntic,"Antic");}
                else{health.invincible=false;a.velocity.x=0;step(BrSpitAntic,"Spit");a.waitTimer=0;}
            };
            if(!a.fsmStarted){
                // Init: Min/Max X = spawn X -+ Roam Distance 2; Facing Right?; Sleep (invincible, no collider).
                a.fsmStarted=true;a.home=a.position;setFacing(a.facingRight);
                health.invincible=true;health.invincibleFromDirection=0;objects[i].colliderEnabled=false;
                step(BrSleep,"Sleep");
            }
            const bool awake=a.step!=BrSleep;
            switch(BrawlerStep(a.step)){
            case BrSleep:
                a.velocity={};
                if(region(0,-.69f+1.3867188f,16.98f,6.400882f)){
                    // Wake: collider on, gravity 1, invincible from above (direction 4).
                    objects[i].colliderEnabled=true;health.invincible=true;health.invincibleFromDirection=4;
                    step(BrWake,"Wake");
                }
                break;
            case BrWake:if(a.clipTime>=6.f/12)changeMovement();break;
            case BrIdle:
                if(a.timer<=0){changeMovement();break;}
                // Return Left/Right re-roll the walk while outside the roam range.
                if(a.position.x>a.home.x+2){a.moveTime=roll(1,2);a.moveSpeed=roll(-3,-1);idle();break;}
                if(a.position.x<a.home.x-2){a.moveTime=roll(1,2);a.moveSpeed=roll(1,3);idle();break;}
                if(a.facingRight&&heroLeft){setFacing(false);step(BrTurn,"Turn");break;}
                if(!a.facingRight&&!heroLeft){setFacing(true);step(BrTurn,"Turn");break;}
                a.waitTimer+=dt;
                if(a.waitTimer>a.waitTime)spitAntic();
                break;
            case BrTurn:
                a.waitTimer+=dt;
                if(a.clipTime>=2.f/12)idle();
                break;
            case BrSpitAntic:
                a.velocity.x=0;
                if(a.clipTime>=2.f/12){
                    // Spit: Target X = clamp((Hero X - Self X) x 0.8, 2.5-13 toward the facing); 2-4
                    // Vomit Globs at Target X +-1.5, Y 23-26 from the Spit Point.
                    health.invincible=true;
                    const float low=a.facingRight?2.5f:-13.f,high=a.facingRight?13.f:-2.5f;
                    const float target=std::clamp((hero.position.x-a.position.x)*.8f,low,high);
                    const Vec2 from{a.position.x-1.95f*a.scaleX,a.position.y+.04f*a.size.y};
                    const unsigned count=std::min(4u,2u+unsigned(roll(0,3)));
                    unsigned spawned[4]{};unsigned n=0;
                    for(unsigned k=0;k<count;++k){
                        const float vx=roll(target-1.5f,target+1.5f),vy=roll(23,26);
                        spawned[n]=spawnGlob(from,{vx,vy});if(spawned[n]<globCapacity)++n;
                    }
                    // Each glob's Init (RandomFloat 1-1.75 -> SetScale) runs after the fling.
                    for(unsigned k=0;k<n;++k)globs[spawned[k]].scale=roll(1,1.75f);
                    a.step=BrSpit;++a.attacks;
                }
                break;
            case BrSpit:
                a.velocity.x=0;
                if(a.clipTime>=5.f/12){step(BrSpitCd,"Run");a.timer=1;}
                break;
            case BrSpitCd:if(a.timer<=0)idle();break;
            case BrBounce:if(a.clipTime>=2.f/12)idle();break;
            case BrSmashAntic:
                a.velocity.x=0;
                if(a.clipTime>=8.f/12){health.invincible=false;step(BrSmashing,"Smash Loop");a.timer=2.25f;++a.attacks;}
                break;
            case BrSmashing:{
                a.velocity.x=a.facingRight?.85f:-.85f;
                const float cx=a.position.x-.0703125f*a.scaleX,cy=a.position.y-1.28125f*a.size.y;
                const float hw=3.328125f*a.size.x/2,hh=2.21875f*a.size.y/2;
                if(overlap({cx-hw,cy-hh,cx+hw,cy+hh},heroBox))hero.takeDamage(1,heroLeft?1:-1);
                // Smashing ends after 2.25 s or as soon as the hero is behind.
                if(a.timer<=0||a.facingRight==heroLeft){health.invincible=true;step(BrSmashRecover,"Smash to Run");}
                break;
            }
            case BrSmashRecover:if(a.clipTime>=3.f/12)idle();break;
            default:break;
            }
            if(awake)a.velocity.y+=tuning::gravity*dt;
        }else if(a.kind==EnemyCKind::BroodingMawlek){
            // Crossroads_09 Mawlek Body (scale 1.2 x 1.1, never flipped: Walker preventScaleChange).
            // Body "Mawlek Control": Dormant until Alert Range New -> Wake (Dummy Intro Jump 0.166 s,
            // up 53, gravity 3, Dummy Intro Land, Dummy Roar 2 s, Roar Cooldown) -> Start (parts WAKE,
            // vulnerable, Walker) -> Idle 2-3 s -> Super Ready (parts idle) -> Super Select 50/50:
            // Super Spit (Dummy Shoot Antic, 25 shots 32-35 at 92-105 / 75-88 degrees, cooldown 1.75)
            // or Super Jump (Dummy Jump Antic, (Hero X - X) x 1.25 and up 68, land 0.5 s, jump back
            // to Start X, cooldown 0.25); at most 3 in a row of each; 25% chance of a second super.
            auto& m=mawlek;auto& health=objects[i].health;
            const float sx=a.size.x,sy=a.size.y;
            const Rect heroBox=bodyBounds(hero.position);
            const auto at=[&](float x,float y){return Vec2{a.position.x+x*sx,a.position.y+y*sy};};
            const auto part=[&](AnimationPlayer& player,const char* clip){
                if(!bank_)return;
                char name[48];std::snprintf(name,sizeof(name),"mawlek/%s",clip);player.play(*bank_,name,0,true);
            };
            const auto partDone=[](const AnimationPlayer& player){return !player.clip||player.complete;};
            const auto body=[&](std::uint8_t next,float wait=0){m.body=next;m.bodyClock=0;m.wait=wait;};
            const bool selfLeftOfHero=a.position.x<hero.position.x;
            m.bodyClock+=dt;m.wait-=dt;
            if(!a.fsmStarted){
                a.fsmStarted=true;m={};m.startX=a.position.x;m.bodyVisible=true;
                health.invincible=true;health.invincibleFromDirection=0;
                play(i,"Dummy Blank");part(m.dummy,"Dummy Blank");part(m.head_,"Dummy Blank");
                part(m.arms[0],"Dummy Blank");part(m.arms[1],"Dummy Blank");
            }
            const auto sleepParts=[&]{
                // SLEEP: Head and Arm FSMs go Dormant (Dummy Blank, arm polygons off, not active).
                m.partsAwake=false;m.head=0;m.arm[0]=m.arm[1]=0;
                part(m.head_,"Dummy Blank");part(m.arms[0],"Dummy Blank");part(m.arms[1],"Dummy Blank");
            };
            const auto start=[&]{
                part(m.dummy,"Dummy Blank");m.superRepeated=false;health.invincible=false;
                play(i,"Body Idle");m.bodyVisible=true;
                // WAKE to the parts: Head Idle 0.3-0.6 s, Arms Idle.
                m.partsAwake=true;m.head=1;m.headClock=0;m.headWait=roll(.3f,.6f);part(m.head_,"Head Idle");
                for(unsigned k=0;k<2;++k){m.arm[k]=1;m.armClock[k]=0;part(m.arms[k],"Arm Idle");}
                // StartWalker: walk in the current facing.
                m.walker=1;m.walkerClock=roll(1,3);a.velocity.x=m.walkDirection*3;play(i,"Body Walk");
                body(MwIdle,roll(2,3));
            };
            const auto superCooldown=[&]{
                play(i,"Body Idle");m.bodyVisible=true;part(m.dummy,"Dummy Blank");
                part(m.arms[0],"Arm Idle");part(m.arms[1],"Arm Idle");part(m.head_,"Head Idle");
                body(MwSuperCooldown,m.cooldown);
            };
            std::function<void()> superJump;
            const auto superSpit=[&]{
                if(m.spits>3){superJump();return;}
                ++m.spits;m.jumps=0;m.dummyScale=selfLeftOfHero?-1.f:1.f;
                sleepParts();play(i,"Dummy Blank");m.bodyVisible=false;part(m.dummy,"Dummy Shoot Antic");
                body(MwSpitAntic);
            };
            superJump=[&]{
                if(m.jumps>3){superSpit();return;}
                ++m.jumps;m.spits=0;
                sleepParts();play(i,"Dummy Blank");m.bodyVisible=false;part(m.dummy,"Dummy Jump Antic");
                body(MwJumpAntic);
            };
            switch(MawlekStep(m.body)){
            case MwDormant:{
                a.velocity={};
                // Alert Range New: (0,0.31) x (13.65,5.2), box offset (0,1.338) size 1.15 x 4.526.
                const Vec2 c=at(0,.31f+1.3380738f*5.2f);const float hx=1.15f*13.65f/2*sx,hy=4.5264764f*5.2f/2*sy;
                if(overlap({c.x-hx,c.y-hy,c.x+hx,c.y+hy},heroBox)){part(m.dummy,"Dummy Intro Jump");body(MwWake,.166f);}
                break;
            }
            case MwWake:if(m.wait<=0){a.velocity={0,53};body(MwWakeJump,.1f);}break;
            case MwWakeJump:if(m.wait<=0)body(MwWakeAir);break;
            case MwWakeAir:break;
            case MwWakeLand:
                a.velocity.x=0;
                if(partDone(m.dummy)){part(m.dummy,"Dummy Roar");body(MwRoar,2);}
                break;
            case MwRoar:if(m.wait<=0){part(m.dummy,"Roar Cooldown");body(MwRoarEnd);}break;
            case MwRoarEnd:if(partDone(m.dummy))start();break;
            case MwIdle:
                if(m.wait<=0){
                    // Super Ready: StopWalker, Body Idle; waits until head and arms are inactive.
                    m.walker=0;a.velocity.x=0;play(i,"Body Idle");body(MwSuperReady);
                }
                break;
            case MwSuperReady:{
                a.velocity.x=0;
                const bool active=m.head==2||m.head==3||(m.arm[0]>=2&&m.arm[0]<=4)||(m.arm[1]>=2&&m.arm[1]<=4);
                if(!active){if(roll(0,1)<.5f)superSpit();else superJump();}
                break;
            }
            case MwSpitAntic:
                if(partDone(m.dummy)){
                    const float low=selfLeftOfHero?75.f:92.f,high=selfLeftOfHero?88.f:105.f;
                    for(unsigned n=0;n<25;++n){
                        const float speed=roll(32,35),angle=roll(low,high)*3.14159265f/180;
                        shoot({a.position.x,a.position.y+.5f},{speed*std::cos(angle),speed*std::sin(angle)});
                    }
                    ++a.attacks;part(m.dummy,"Dummy Shoot");m.cooldown=1.75f;body(MwShoot);
                }
                break;
            case MwShoot:if(partDone(m.dummy))superCooldown();break;
            case MwSuperCooldown:
                if(m.wait<=0){
                    // Repeat Check: a second super after the first with Chooser > 75.
                    if(!m.superRepeated&&roll(0,100)>75){m.superRepeated=true;a.velocity.x=0;body(MwSuperReady);}
                    else start();
                }
                break;
            case MwJumpAntic:
                if(partDone(m.dummy)){
                    m.xDistance=(hero.position.x-a.position.x)*1.25f;m.dummyScale=selfLeftOfHero?1.f:-1.f;
                    part(m.dummy,"Dummy Jump");a.velocity={m.xDistance,68};++a.attacks;body(MwJump,.1f);
                }
                break;
            case MwJump:if(m.wait<=0)body(MwInAir);break;
            case MwInAir:a.velocity.x=m.xDistance;break;
            case MwLand:
                if(m.wait<=0){
                    // Aim Return: X Distance = (Start X - Self X) x 1.25.
                    m.xDistance=(m.startX-a.position.x)*1.25f;m.dummyScale=m.xDistance==0?1.f:-1.f;
                    part(m.dummy,"Dummy Jump");a.velocity={m.xDistance,68};body(MwJump2,.1f);
                }
                break;
            case MwJump2:if(m.wait<=0)body(MwInAir2);break;
            case MwInAir2:a.velocity.x=m.xDistance;break;
            case MwLand2:if(m.wait<=0)superCooldown();break;
            default:break;
            }
            if(m.walker&&m.body==MwIdle){
                // Walker: Walking (Body Walk, 1-3 s) with wall/hole sweeps at extents + 0.5 + edge 0.5,
                // Stopped (Body Idle, 1-3 s), Turning (Idle Turn until complete).
                m.walkerClock-=dt;
                if(m.walker==1){
                    const Rect b=a.bounds();const float half=(b.right-b.left)/2;float hit=0;
                    const float front=m.walkDirection;
                    const bool wall=rayTerrain({a.position.x,(b.bottom+b.top)/2},{front,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded)||
                                    rayTerrain({a.position.x,b.bottom+.1f},{front,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded);
                    const bool hole=!rayTerrain({a.position.x+(half+1)*front,b.bottom+.1f},{0,-1},.35f,world,hit,TerrainShape::terrainQueryExcluded);
                    if(wall||hole){m.walker=3;a.velocity.x=0;play(i,"Idle Turn");}
                    else if(m.walkerClock<=0){m.walker=2;a.velocity.x=0;m.walkerClock=roll(1,3);play(i,"Body Idle");}
                    else a.velocity.x=m.walkDirection*3;
                }else if(m.walker==2){a.velocity.x=0;if(m.walkerClock<=0){m.walker=1;m.walkerClock=roll(1,3);play(i,"Body Walk");}}
                else if(m.walker==3){
                    a.velocity.x=0;
                    if(!a.animation.clip||a.animation.complete){m.walkDirection=-m.walkDirection;m.walker=1;m.walkerClock=roll(1,3);play(i,"Body Walk");}
                }
            }
            if(m.partsAwake){
                // Mawlek Head: Idle 0.3-0.6 s -> Shoot Antic (Head Spit, 0.083 s) -> Shoot: one shot at 27,
                // 95-105 degrees (hero left) or 75-85, until the clip ends or 0.25 s -> Idle.
                const Vec2 headAt=at(-.05f,1.67f);
                m.headClock+=dt;
                if(m.head==1&&m.headClock>=m.headWait){m.head=2;m.headClock=0;part(m.head_,"Head Spit");}
                else if(m.head==2&&m.headClock>=.083f){
                    const bool left=headAt.x>=hero.position.x;
                    const float angle=(left?roll(95,105):roll(75,85))*3.14159265f/180;
                    shoot({headAt.x,headAt.y+.5f},{27*std::cos(angle),27*std::sin(angle)});++a.attacks;
                    m.head=3;m.headClock=0;
                }else if(m.head==3&&(partDone(m.head_)||m.headClock>=.25f)){
                    m.head=1;m.headClock=0;m.headWait=roll(.3f,.6f);part(m.head_,"Head Idle");
                }
                // Mawlek Arm Control: Attack Range (0,-0.48) x (4.95,1.95), box (-0.0999,0.0142) 0.80 x 2.479;
                // Swipe Antic (0.8125 s) -> Swipe (polygon on) -> Swipe Cooldown -> 0.15 s -> Idle.
                for(unsigned k=0;k<2;++k){
                    const float mirror=k==0?-1.f:1.f,armX=k==0?-2.67f:2.56f;
                    m.armClock[k]+=dt;
                    if(m.arm[k]==1){
                        const Vec2 c=at(armX+mirror*-.09990115f*4.95f,.07f-.48f+.014169628f*1.95f);
                        const float hx=.8001976f*4.95f/2*sx,hy=2.478861f*1.95f/2*sy;
                        if(overlap({c.x-hx,c.y-hy,c.x+hx,c.y+hy},heroBox)){m.arm[k]=2;m.armClock[k]=0;part(m.arms[k],"Arm Swipe Antic");}
                    }else if(m.arm[k]==2&&partDone(m.arms[k])){m.arm[k]=3;m.armClock[k]=0;m.armStruck[k]=false;part(m.arms[k],"Arm Swipe");++a.attacks;}
                    else if(m.arm[k]==3){
                        static constexpr float swipe[][2]={{-.21875f,1.921875f},{-1.4453125f,1.8125f},{-1.4453125f,-2.234375f},
                            {.296875f,-2.078125f},{1.4375f,-.84375f},{1.6875f,.28125f},{1.046875f,1.453125f}};
                        Vec2 polygon[7];
                        for(unsigned n=0;n<7;++n)polygon[n]=at(armX+mirror*swipe[n][0],.07f+swipe[n][1]);
                        if(polygonOverlapsBox(polygon,7,heroBox))hero.takeDamage(1,hero.position.x<polygon[0].x?1:-1);
                        if(partDone(m.arms[k])){m.arm[k]=4;m.armClock[k]=0;part(m.arms[k],"Arm Swipe Cooldown");}
                    }else if(m.arm[k]==4&&partDone(m.arms[k])){m.arm[k]=5;m.armClock[k]=0;}
                    else if(m.arm[k]==5&&m.armClock[k]>=.15f){m.arm[k]=1;m.armClock[k]=0;part(m.arms[k],"Arm Idle");}
                }
            }
            // Rigidbody gravity 0 until Wake In Air sets 3.
            if(m.body>=MwWakeAir)a.velocity.y+=tuning::gravity*3*dt;
        }
        if(p.gravity&&a.kind!=EnemyCKind::BroodingMawlek)a.velocity.y-=p.gravity*dt;
        const Rect body=a.kind==EnemyCKind::FlyingSentry?flyingTerrainBox(a):a.body();
        const auto contacts=moveBox(a.position,a.velocity,body,dt,world);
        if(a.kind==EnemyCKind::BroodingMawlek){
            // CheckCollisionSide bottom -> LANDED (Wake In Air, In Air, In Air 2).
            auto& m=mawlek;
            if(contacts.ground&&a.velocity.y<=0){
                if(m.body==MwWakeAir){a.velocity={};m.body=MwWakeLand;m.bodyClock=0;if(bank_)m.dummy.play(*bank_,"mawlek/Dummy Intro Land",0,true);}
                else if(m.body==MwInAir){a.velocity={};m.body=MwLand;m.bodyClock=0;m.wait=.5f;if(bank_)m.dummy.play(*bank_,"mawlek/Dummy Land",0,true);}
                else if(m.body==MwInAir2){a.velocity={};m.body=MwLand2;m.bodyClock=0;m.wait=.25f;m.cooldown=.25f;if(bank_)m.dummy.play(*bank_,"mawlek/Dummy Intro Land",0,true);}
            }
        }else if(a.kind==EnemyCKind::Baldur){
            // Collide Right/Left: velocity 12 at 115 / 65 degrees, Moving Right flips, scale X 1.
            if(a.phase==EnemyCPhase::Move&&((a.facingRight&&contacts.right)||(!a.facingRight&&contacts.left))){
                const float angle=(a.facingRight?115.f:65.f)*3.14159265f/180;
                a.facingRight=!a.facingRight;a.face(1);a.velocity={12*std::cos(angle),12*std::sin(angle)};
                a.phase=EnemyCPhase::Jump;
            }else if(a.phase==EnemyCPhase::Jump&&contacts.ground&&a.velocity.y<=0){a.phase=EnemyCPhase::Move;play(i,"Move");}
        }else{
        if(a.kind!=EnemyCKind::Hatchling&&(contacts.left||contacts.right)&&a.phase==EnemyCPhase::Move){a.scaleX=-a.scaleX;a.velocity.x=-a.velocity.x;}
        if(a.phase==EnemyCPhase::Jump&&contacts.ground)enter(i,EnemyCPhase::Recover,1,"Land");
        }
        // Ruins Sentry Fat In Air: bottom collision -> Land (Jump Attack).
        if(a.kind==EnemyCKind::HeavySentry&&a.step==Air&&contacts.ground&&a.velocity.y<=0){
            a.velocity.x=0;a.step=Land;a.clipTime=0;play(i,"Jump Attack");
        }
        objects[i].bounds=a.bounds();
        if(objects[i].colliderEnabled&&!(a.kind==EnemyCKind::BroodingMawlek&&mawlek.body==MwDormant)&&overlap(a.bounds(),bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<a.position.x?1:-1);
    }
    combat_.fixedUpdate(hero);
    updateGlobs(dt,hero,world);
    for(auto& s:shots)if(s.active){
        PresentationPosition::Step history(s.presentation,s.position);
        s.age+=dt;
        if(s.impact){if(!s.animation.clip||s.animation.complete)s.active=false;continue;}
        // Gravity 0.6; box (0.0078,0) 0.64 x 0.56 under the shot scale; terrain -> Impact.
        s.velocity.y+=tuning::gravity*.6f*dt;
        const Rect box{(.0078125f-.3203125f)*s.scale,-.28125f*s.scale,(.0078125f+.3203125f)*s.scale,.28125f*s.scale};
        const auto contact=moveBox(s.position,s.velocity,box,dt,world);
        if(overlap({s.position.x+box.left,s.position.y+box.bottom,s.position.x+box.right,s.position.y+box.top},bodyBounds(hero.position)))
            hero.takeDamage(1,hero.position.x<s.position.x?1:-1);
        if(contact.ground||contact.roof||contact.left||contact.right){
            s.impact=true;s.velocity={};if(bank_)s.animation.play(*bank_,"shot/Impact",0,true);
        }else if(s.age>5)s.active=false;
    }
}
unsigned EnemyCEncounter::spawnGlob(Vec2 p,Vec2 v)
{
    for(unsigned k=0;k<globCapacity;++k)if(!globs[k].active){
        auto& g=globs[k];g={};g.active=true;g.position=p;g.velocity=v;g.presentation.reset(p);
        if(bank_)g.animation.play(*bank_,"glob/Idle",0,true);
        return k;
    }
    ++rejectedSpawns;return globCapacity;
}
void EnemyCEncounter::updateGlobs(float dt,Hero& hero,const World& world)
{
    // Vomit Glob NoiseFix (sharedassets34 path 276): gravity 0.7, body box (0,0.37) 0.59 x 0.61 and
    // Puddle Box (-0.0107,0.09) 1.277 x 0.264, both scaled. In Air -> bottom hit: Land (velocity 0,
    // up 0.25, puddle on, body collider off) for 1.25-1.75 s; Shrink to 0.1 in 0.25 s (linear, no
    // damage); hidden for 1 s; Recycle.
    const Rect heroBox=bodyBounds(hero.position);
    for(auto& g:globs)if(g.active){
        PresentationPosition::Step history(g.presentation,g.position);
        g.timer-=dt;
        const float s=g.scale;
        if(g.stage==0){
            g.velocity.y+=tuning::gravity*.7f*dt;
            const Rect body{-.295f*s,(.37f-.305f)*s,.295f*s,(.37f+.305f)*s};
            const auto contact=moveBox(g.position,g.velocity,body,dt,world);
            if(overlap({g.position.x+body.left,g.position.y+body.bottom,g.position.x+body.right,g.position.y+body.top},heroBox))
                hero.takeDamage(1,hero.position.x<g.position.x?1:-1);
            if(contact.ground){
                g.stage=1;g.velocity={};g.position.y+=.25f;g.timer=roll(1.25f,1.75f);
                if(bank_)g.animation.play(*bank_,"glob/Land",0,true);
            }
        }else if(g.stage==1){
            const Rect puddle{(-.0106659f-.638711f)*s,(.09f-.131956f)*s,(-.0106659f+.638711f)*s,(.09f+.131956f)*s};
            if(overlap({g.position.x+puddle.left,g.position.y+puddle.bottom,g.position.x+puddle.right,g.position.y+puddle.top},heroBox))
                hero.takeDamage(1,hero.position.x<g.position.x?1:-1);
            if(g.timer<=0){g.stage=2;g.timer=.25f;g.shrinkFrom=s;}
        }else if(g.stage==2){
            g.scale=g.shrinkFrom+(.1f-g.shrinkFrom)*std::clamp(1-g.timer/.25f,0.f,1.f);
            if(g.timer<=0){g.stage=3;g.timer=1;}
        }else if(g.timer<=0)g.active=false;
    }
}
void EnemyCEncounter::update(float dt,const Hero& hero,const World& world)
{
    if(!active()||dt<0||!std::isfinite(dt))return;
    combat_.update(hero,dt);for(auto& a:actors)if(a.occupied()){
        // Hatcher Idle --ALERT--> Distance Fly sets pitch once. FSM4335
        // RayCast2d repeatInterval1 polls each update, independently of physics.
        // A frozen (zero-dt) update must not activate the retained latch.
        if(dt>0&&a.alive()&&a.kind==EnemyCKind::AspidMother&&!a.motherAlerted){
            if(!a.motherIdleStarted){
                a.motherIdleStarted=true;a.home=a.position;motherIdle(a,dt);
            }
            a.motherAlerted=motherAlert(a,hero,world);
            if(a.motherAlerted)enter(unsigned(&a-actors.data()),EnemyCPhase::Move,random_.range(2,3),"Idle");
        }else if(dt>0&&a.alive()&&a.kind==EnemyCKind::AspidMother){
            const unsigned index=unsigned(&a-actors.data());
            a.timer-=dt;
            if(a.phase==EnemyCPhase::Move&&a.timer<=0){
                if(a.attacks<5){a.velocity={};enter(index,EnemyCPhase::Windup,.335f,"Attack");}
                else a.timer=random_.range(2,3);
            }else if(a.phase==EnemyCPhase::Windup&&a.timer<=0){
                if(releaseFromCage({a.position.x,a.position.y-1}))++a.attacks;
                a.phase=EnemyCPhase::Attack;
            }else if(a.phase==EnemyCPhase::Attack&&(a.animation.complete||!a.animation.clip)){
                enter(index,EnemyCPhase::Move,random_.range(2,3),"Idle");
            }
        }
        a.audioSeconds+=dt;
        a.animation.advance(dt);
        if(a.alive()&&a.kind==EnemyCKind::Hatchling){
            a.hatchling.start(a.position,hero.position,a.velocity,random_);
            a.hatchling.update(dt,a.velocity,a.scaleX);
            objects[&a-actors.data()].bounds=a.bounds();
        }
    }
    // Pooled art clocks: Mawlek parts, shots and Vomit Globs.
    if(dt>0){
        mawlek.head_.advance(dt);mawlek.arms[0].advance(dt);mawlek.arms[1].advance(dt);mawlek.dummy.advance(dt);
        for(auto& s:shots)if(s.active)s.animation.advance(dt);
        for(auto& g:globs)if(g.active)g.animation.advance(dt);
    }
}
void EnemyCRenderState::capture(const EnemyCEncounter& source,PresentationTime time)
{
    for(unsigned i=0;i<actors.size();++i){const auto& a=source.actors[i];
        char name[64];std::snprintf(name,sizeof(name),"enemy-c/%s",enemyCProfile(a.kind).prefix);
        actors[i]={a.animation,a.presentation.sample(a.position,time),a.kind==EnemyCKind::GlowingWomb?a.scaleX:animationFacingScale(name,-a.scaleX,std::abs(a.scaleX)),a.phase==EnemyCPhase::Dead?std::clamp(a.timer/.4f,0.f,1.f):1,a.flash,a.visible(),a.size.y};}
    shotCount=0;for(const auto& s:source.shots)if(s.active)shots[shotCount++]={s.animation,s.presentation.sample(s.position,time),s.scale};
    // Brooding Mawlek parts (child transforms under the root scale; Arm L mirrored, Dummy scale).
    partCount=0;mawlekActor=EnemyCEncounter::capacity;
    for(unsigned i=0;i<source.actors.size();++i){
        const auto& a=source.actors[i];
        if(a.kind!=EnemyCKind::BroodingMawlek||!a.visible())continue;
        const auto& m=source.mawlek;mawlekActor=i;
        const Vec2 at=a.presentation.sample(a.position,time);const float sx=a.size.x,sy=a.size.y;
        // The body's art-facing correction applies to every part sprite as well.
        const float art=actors[i].scaleX<0?-1.f:1.f;
        const auto add=[&](const AnimationPlayer& player,float x,float y,float mirror,bool front){
            if(player.clip&&partCount<parts.size())parts[partCount++]={player,{at.x+x*sx,at.y+y*sy},art*mirror*sx,sy,front};
        };
        add(m.arms[0],-2.67f,.07f,-1,false);add(m.arms[1],2.56f,.07f,1,false);add(m.head_,-.05f,1.67f,1,false);
        add(m.dummy,0,.2422967f,m.dummyScale,true);
        if(!m.bodyVisible)actors[i].visible=false;
    }
    globCount=0;for(const auto& g:source.globs)if(g.active&&g.stage<3)globs[globCount++]={g.animation,g.presentation.sample(g.position,time),g.scale};
}
}
