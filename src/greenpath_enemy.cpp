#include "text_pack.hpp"
#include "greenpath_enemy.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
// Native movement timings below approximate the source FSM sequences.
#include "greenpath_enemy_data.hpp"
float sign(float x){return x<0?-1.f:1.f;}
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
bool flying(GreenpathKind k){return k==GreenpathKind::Mossfly||k==GreenpathKind::Squit||k==GreenpathKind::Aluba;}
// Source Walk turn (Turn clip: 3 frames at 12 fps in the regeneration snapshot).
constexpr float mosscreepTurnTime=3.f/12;
// Lazy Flyer "Flight Distance" per placement (crossroads-scenes dumps, build 22529139); others 10.
float lazyFlyerDistance(const char* scene,std::uint32_t owner)
{
    static constexpr struct {const char* scene;std::uint32_t owner;float distance;} rows[]={
        {"Fungus3_40",1201,20},{"Fungus3_40",376,15},{"Fungus3_48",644,6.5f},
        {"Fungus3_50",186,13},{"Fungus3_50",19,9},{"Fungus3_50",227,12}};
    for(const auto& r:rows)if(owner==r.owner&&!std::strcmp(scene,r.scene))return r.distance;
    return 10;
}
bool lineOfSight(Vec2 from,Vec2 to,const World& world)
{
    // CheckCanSeeHero / LineOfSightDetector: terrain (layer 8) ray from pivot to hero.
    const Vec2 d{to.x-from.x,to.y-from.y};const float length=std::sqrt(d.x*d.x+d.y*d.y);float hit=0;
    return length<=1e-4f||!rayTerrain(from,{d.x/length,d.y/length},length,world,hit,TerrainShape::terrainQueryExcluded)||hit>=length;
}
bool inCircle(Vec2 center,float radius,Rect hero)
{
    const float x=std::clamp(center.x,hero.left,hero.right)-center.x,y=std::clamp(center.y,hero.bottom,hero.top)-center.y;
    return x*x+y*y<=radius*radius;
}
// PolygonCollider2D points in the actor's local space: Unity scale X is -facing for these
// left-facing arts, Y is the placement scale.
template<std::size_t N> bool strikes(const GreenpathActor& a,const Hero& hero,const float (&local)[N][2])
{
    Vec2 world[N];
    for(std::size_t i=0;i<N;++i)world[i]={a.position.x-local[i][0]*a.facing*a.size.x,a.position.y+local[i][1]*a.size.y};
    return polygonOverlapsBox(world,N,bodyBounds(hero.position));
}
// Decelerate (subtract) and DecelerateV2 (multiply), once per fixed update, per axis toward zero.
float decelerate(float v,float d){return v<0?std::min(0.f,v+d):std::max(0.f,v-d);}
}
float greenpathArtScale(GreenpathKind kind,float facing)
{
    char name[64];std::snprintf(name,sizeof(name),"greenpath-enemies/%s",greenpathProfile(kind).prefix);
    return animationFacingScale(name,facing);
}
const GreenpathProfile& greenpathProfile(GreenpathKind k){return profiles[std::min(unsigned(k),8u)];}
const GreenpathRegistration* findGreenpathEnemy(const char* name)
{
    if(!name)return nullptr;
    for(const auto& e:greenpathEnemyRegistry){
        const auto n=std::strlen(e.prefab);
        if(!std::strncmp(name,e.prefab,n)&&(name[n]==0||name[n]==' '||name[n]=='('))return &e;
    }
    return nullptr;
}
bool GreenpathEncounter::bind(const AnimationBank* bank,const NailBank* nails)
{bank_=bank;nails_=nails;clear();return bank_&&nails_&&nails_->ready();}
void GreenpathEncounter::clear(std::uint32_t flags)
{
    actors={};objects={};needle={};flags_=flags&hornetDefeatedFlag;hornetArena_=false;fixture=false;arena={};geoReleased=0;rejectedSpawns=0;
    phaseTwo_=false;sphereVisible_=false;titleAge_=-1;sphereAge_=sphereRadius_=0;spherePosition_={};
    phaseSerial_=staggerCount_=introPage_=0;choiceRepeats_={};choiceMissed_={};randomState_=0x48524e54;
    stunHits_=comboHits_=0;comboAge_=0;
    trigger_={};introPosition_={};authoredIntro_=false;
    rewards_.clear();owners_={};actorRewards_={};roomPath_[0]=0;
    for(unsigned i=0;i<capacity;++i){objects[i].colliderEnabled=false;receivers_[i]={this,i};}
    combat_.bind(objects.data(),capacity,nails_);
}
void GreenpathEncounter::play(unsigned i,const char* clip,bool hold)
{
    actors[i].clip=clip;
    if(!bank_)return;
    // These libraries have no death clip. Their separate source death effects
    // belong to the family FSM port; flight must never be relabelled as death.
    if(!std::strcmp(clip,"Death")&&(actors[i].kind==GreenpathKind::Mossfly||actors[i].kind==GreenpathKind::Aluba)){
        actors[i].animation={};return;
    }
    char name[48];std::snprintf(name,sizeof(name),"%s/%s",greenpathProfile(actors[i].kind).prefix,clip);
    // hold: start on the last frame, for poses such as a Moss Charger hidden in the grass.
    if(const auto* c=bank_->require(name))actors[i].animation.play(c,hold&&c->count?c->count-1:0,true);
}
void GreenpathEncounter::enter(unsigned i,GreenpathPhase phase,float time,const char* clip,bool hold)
{actors[i].phase=phase;actors[i].timer=time;++phaseSerial_;play(i,clip,hold);}
float GreenpathEncounter::random(float low,float high)
{
    if(random_.unit||random_.sample)return random_.range(low,high);
    // Deterministic fixture fallback; ordinary play uses the room RNG adapter.
    randomState_^=randomState_<<13;randomState_^=randomState_>>17;randomState_^=randomState_<<5;
    return low+(high-low)*float(randomState_&0xffffff)/float(0x1000000);
}
unsigned GreenpathEncounter::chooseAttack(bool canThrow)
{
    // Protector Control / Move Choice A/B: air dash, sphere, ground dash,
    // needle. Source repeat maxima 2/1/2/1 and missed maxima 5/7/5/3.
    constexpr unsigned repeatMax[]={2,1,2,1},missedMax[]={5,7,5,3};
    const unsigned count=canThrow?4:3;unsigned chosen=count;
    for(unsigned n=0;n<count;++n)if(choiceMissed_[n]>=missedMax[n]){chosen=n;break;}
    if(chosen==count){
        unsigned eligible[4]{},size=0;
        for(unsigned n=0;n<count;++n)if(choiceRepeats_[n]<repeatMax[n])eligible[size++]=n;
        chosen=eligible[std::min(unsigned(random(0,float(size))),size-1)];
    }
    for(unsigned n=0;n<count;++n){
        choiceRepeats_[n]=n==chosen?choiceRepeats_[n]+1:0;
        choiceMissed_[n]=n==chosen?0:choiceMissed_[n]+1;
    }
    return chosen;
}
bool GreenpathEncounter::spawn(GreenpathKind kind,Vec2 p,float facing,float scaleY)
{
    if(unsigned(kind)>=unsigned(GreenpathKind::Count)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(facing))return false;
    if(kind==GreenpathKind::Hornet&&(flags_&hornetDefeatedFlag))return true;
    unsigned i=0;while(i<capacity&&actors[i].visible())++i;
    if(i==capacity){++rejectedSpawns;return false;}
    auto& a=actors[i];a={};a.kind=kind;a.position=a.home=p;a.facing=sign(facing);a.presentation.reset(p);
    if(facing!=0){const float y=scaleY!=0&&std::isfinite(scaleY)?std::abs(scaleY):std::abs(facing);a.size={std::abs(facing),y};}
    owners_[i]=0;actorRewards_[i]=enemyRewardConfig(EnemyRewardKind(unsigned(EnemyRewardKind::Mosscreep)+unsigned(kind)));
    auto& o=objects[i];o={};o.name=greenpathProfile(kind).prefix;o.hasHealth=true;o.health.hp=greenpathProfile(kind).hp;
    o.health.invincible=greenpathProfile(kind).invincible;o.health.invincibleFromDirection=greenpathProfile(kind).invincibleDirection;o.receiver=&receivers_[i];o.onHit=receive;o.bounds=a.bounds();
    if(kind==GreenpathKind::Hornet){
        // Scene actors wait for the authored arena trigger. The standalone
        // fixture calls beginHornet(), which starts the same Intro state.
        enter(i,GreenpathPhase::Dormant,0,"Idle");
        o.colliderEnabled=false;
    }else enter(i,GreenpathPhase::Idle,.6f,"Idle",kind==GreenpathKind::MossCharger);
    return true;
}
bool GreenpathEncounter::beginHornet(Rect r,std::uint32_t flags)
{
    if(!std::isfinite(r.left)||!std::isfinite(r.right)||!std::isfinite(r.bottom)||!std::isfinite(r.top)||r.right-r.left<8||r.top-r.bottom<5)return false;
    clear(flags);arena=r;hornetArena_=true;
    if(!spawn(GreenpathKind::Hornet,{r.right-3,r.bottom-greenpathProfile(GreenpathKind::Hornet).body.bottom},-1))return false;
    beginIntro(0);return true;
}
bool GreenpathEncounter::locked()const
{
    if(!hornetArena_||(flags_&hornetDefeatedFlag))return false;
    for(const auto& a:actors)if(a.kind==GreenpathKind::Hornet&&a.visible()&&a.phase!=GreenpathPhase::Dormant&&a.phase!=GreenpathPhase::Gone)return true;
    return false;
}
float GreenpathEncounter::titleAlpha() const
{
    if(titleAge_<0)return 0;
    return std::min(1.f,titleAge_/.5f)*std::clamp(4.f-titleAge_,0.f,1.f);
}
const char* GreenpathEncounter::introText(unsigned page) const
{
    // EN_Hornet / HORNET_GREENPATH, copied build 22529139.
    static constexpr const char* lines[]={"@Hornet/HORNET_GREENPATH",
        "@Hornet/HORNET_GREENPATH#2",
        "@Hornet/HORNET_GREENPATH#3",
        "@Hornet/HORNET_GREENPATH#4"};
    return page<4?hk3ds::text::resolve(lines[page]):"";
}
bool GreenpathEncounter::introActive() const
{
    if(introPage_>=4)return false;
    for(const auto& a:actors)if(a.kind==GreenpathKind::Hornet&&a.phase==GreenpathPhase::Intro)return true;
    return false;
}
void GreenpathEncounter::advanceIntro(Hero& hero)
{
    if(!introActive())return;
    for(unsigned i=0;i<capacity;++i)if(actors[i].kind==GreenpathKind::Hornet&&actors[i].phase==GreenpathPhase::Intro){
        if(actors[i].timer>2.1f)return; // Original Box Up delay.
        if(++introPage_==4){actors[i].timer=.55f;play(i,"Evade");}
        hero.velocity={};return;
    }
}
void GreenpathEncounter::beginIntro(unsigned i)
{
    if(i>=capacity||actors[i].kind!=GreenpathKind::Hornet||!actors[i].visible())return;
    enter(i,GreenpathPhase::Intro,2.4f,"Idle");
    titleAge_=-1;introPage_=0;sphereVisible_=false;objects[i].colliderEnabled=false;
}
World GreenpathEncounter::arenaWorld()const
{
    const auto r=arena;
    arenaSolids_[0]={{r.left-2,r.bottom-2,r.right+2,r.bottom}};
    arenaSolids_[1]={{r.left-2,r.bottom,r.left,r.top}};
    arenaSolids_[2]={{r.right,r.bottom,r.right+2,r.top}};
    arenaSolids_[3]={{r.left,r.top,r.right,r.top+2}};
    return {arenaSolids_.data(),arenaSolids_.size()};
}
bool GreenpathEncounter::loadScene(const char* path,std::uint32_t flags)
{
    clear(flags);if(!path||std::strlen(path)>=sizeof(roomPath_))return false;
    std::snprintf(roomPath_,sizeof(roomPath_),"%s",path);
    FILE* f=std::fopen(path,"rb");if(!f)return true; // Rooms without this optional bank have no Greenpath actors.
    char magic[4];std::uint32_t count=0;float r[4];
    bool ok=std::fread(magic,1,4,f)==4&&(!std::memcmp(magic,"HKG1",4)||!std::memcmp(magic,"HKG2",4))&&std::fread(&count,4,1,f)==1&&count<=capacity&&std::fread(r,4,4,f)==4;
    if(ok)ok=std::isfinite(r[0])&&std::isfinite(r[1])&&std::isfinite(r[2])&&std::isfinite(r[3])&&r[2]-r[0]>2&&r[3]>r[1];
    if(ok&&!std::memcmp(magic,"HKG2",4)){
        float values[6];ok=std::fread(values,4,6,f)==6;
        if(ok)for(float value:values)ok=ok&&std::isfinite(value);
        if(ok){trigger_={values[0],values[1],values[2],values[3]};introPosition_={values[4],values[5]};authoredIntro_=true;
            ok=trigger_.left<trigger_.right&&trigger_.bottom<trigger_.top;}
    }
    std::array<bool,capacity> matched{};
    if(ok){arena={r[0],r[1],r[2],r[3]};for(unsigned i=0;i<count&&ok;++i){std::uint32_t k;float p[3];
        ok=std::fread(&k,4,1,f)==1&&std::fread(p,4,3,f)==3;
        if(ok){
            std::uint32_t owner=0;auto reward=enemyRewardConfig(EnemyRewardKind(unsigned(EnemyRewardKind::Mosscreep)+k));
            float scaleY=0;
            for(unsigned j=0;j<identityCount_;++j){const auto& id=identities_[j];
                if(!matched[j]&&unsigned(id.kind)==k&&std::abs(id.position.x-p[0])<.0001f&&std::abs(id.position.y-p[1])<.0001f){owner=id.owner;reward=id.reward;scaleY=id.scale.y;matched[j]=true;break;}}
            if(reward.persistent&&life_&&owner&&life_->dead(scene_,owner))continue;
            unsigned slot=0;while(slot<capacity&&actors[slot].visible())++slot;
            ok=spawn(static_cast<GreenpathKind>(k),{p[0],p[1]},p[2],scaleY);
            if(ok&&k==unsigned(GreenpathKind::Hornet)&&authoredIntro_&&slot<capacity){
                actors[slot].position=introPosition_;actors[slot].presentation.reset(introPosition_);
            }
            if(ok&&slot<capacity){owners_[slot]=owner;actorRewards_[slot]=reward;actors[slot].flightDistance=lazyFlyerDistance(scene_,owner);}
            hornetArena_|=k==unsigned(GreenpathKind::Hornet);
        }
    }}
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);if(!ok)clear(flags);return ok;
}
bool GreenpathEncounter::restartScene(std::uint32_t flags)
{
    rewards_.clear();
    if(!roomPath_[0])return true;
    char path[sizeof(roomPath_)];std::memcpy(path,roomPath_,sizeof(path));
    return loadScene(path,flags);
}
void GreenpathEncounter::setLifecycle(EnemyLifeState* life,const char* scene,const std::vector<SceneSpawn>& spawns){
    life_=life;std::snprintf(scene_,sizeof(scene_),"%s",scene);identityCount_=0;
    for(const auto& e:spawns)if(e.active)if(const auto* entry=findGreenpathEnemy(e.prefab)){
        if(entry->kind==GreenpathKind::Hornet)continue;
        auto reward=e.hasReward?e.reward:enemyRewardConfig(EnemyRewardKind(unsigned(EnemyRewardKind::Mosscreep)+unsigned(entry->kind)));
        reward.origin.x*=std::abs(e.scale.x);reward.origin.y*=e.scale.y;
        if(identityCount_<capacity)identities_[identityCount_++]={entry->kind,e.position,e.owner,reward,e.scale};
    }
}
void GreenpathEncounter::receive(void* context,const NailHit& hit,HitOutcome result)
{auto& r=*static_cast<Receiver*>(context);r.owner->hit(r.index,hit,result);}
void GreenpathEncounter::hit(unsigned i,const NailHit& hit,HitOutcome result)
{
    auto& a=actors[i];
    if(result==HitOutcome::Blocked&&a.kind==GreenpathKind::MossKnight&&a.phase==GreenpathPhase::Shield){
        // BLOCKED HIT -> Block Low/High (Shield Front/Top Bump) for 0.4 s.
        a.velocity.x=0;a.stateClock=0;enter(i,GreenpathPhase::Block,.4f,a.clip&&!std::strcmp(a.clip,"ShieldTop")?"BumpTop":"BumpFront");return;
    }
    if(!a.alive()||(result!=HitOutcome::Damaged&&result!=HitOutcome::Killed))return;
    if(a.kind==GreenpathKind::Hornet&&(a.phase==GreenpathPhase::Dormant||a.phase==GreenpathPhase::Intro||a.phase==GreenpathPhase::Dead||a.phase==GreenpathPhase::Escape))return;
    a.flash=.12f;++a.hits;
    if(a.kind==GreenpathKind::Squit&&a.phase==GreenpathPhase::Attack&&result==HitOutcome::Damaged){
        // Lunging: HIT RIGHT/LEFT/UP/DOWN re-aims the lunge (+-5 degrees) for Lunge Wait 0.5; recoil speed is 0.
        const float base=hit.direction==Strike::Right?0.f:hit.direction==Strike::Left?180.f:hit.direction==Strike::Up?90.f:270.f;
        const float angle=random(base-5,base+5)*3.14159265f/180;
        a.velocity={18*std::cos(angle),18*std::sin(angle)};a.facing=a.velocity.x<0?-1.f:1.f;a.timer=.5f;
    }else if(a.kind==GreenpathKind::MossCharger&&result==HitOutcome::Damaged&&
             (a.phase==GreenpathPhase::Antic||a.phase==GreenpathPhase::Attack)){
        // TAKE DAMAGE in Charge -> Line Loop -> Burst: Fly Right/Left 18 at 70/110 degrees, Up 20, Down 10.
        const float angle=hit.direction==Strike::Right?70.f:hit.direction==Strike::Left?110.f:hit.direction==Strike::Up?90.f:270.f;
        const float speed=hit.direction==Strike::Up?20.f:hit.direction==Strike::Down?10.f:18.f;
        a.velocity={speed*std::cos(angle*3.14159265f/180),speed*std::sin(angle*3.14159265f/180)};
        enter(i,GreenpathPhase::Stagger,.1f,"Stun");
    }else a.velocity.x=(hit.direction==Strike::Right?6:hit.direction==Strike::Left?-6:0)*hit.recoilMultiplier;
    if(a.kind==GreenpathKind::Hornet&&!phaseTwo_&&objects[i].health.hp<=90){
        phaseTwo_=true;
    }
    if(result==HitOutcome::Killed){objects[i].colliderEnabled=false;
        if(a.kind==GreenpathKind::Hornet)geoReleased+=greenpathProfile(a.kind).geo;
        else {
            const auto& reward=actorRewards_[i];
            if(!life_||life_->killed(scene_,owners_[i],reward))
                rewards_.push(reward,a.position,a.facing);
        }
        enter(i,GreenpathPhase::Dead,a.kind==GreenpathKind::Hornet?.6f:a.kind==GreenpathKind::VolatileMosskin?.6f:1.f,"Death");
    }else if(a.kind==GreenpathKind::Hornet&&a.phase==GreenpathPhase::Stagger){
        // The source Stun Land exits on TOOK DAMAGE; staggering never makes
        // the HealthManager invulnerable to the next nail/spell.
        stunHits_=comboHits_=0;enter(i,GreenpathPhase::Recover,.5f,"Recover");
    }else if(a.kind==GreenpathKind::Hornet){
        ++stunHits_;comboHits_=comboAge_<2?comboHits_+1:1;comboAge_=0;
        if(staggerCount_<5&&(stunHits_>=10||comboHits_>=6)){
            stunHits_=comboHits_=0;a.velocity={};++staggerCount_;
            enter(i,GreenpathPhase::Stagger,3.f,"Stun");needle.active=false;sphereVisible_=false;
        }
    }
}
void GreenpathEncounter::fixedUpdate(float dt,Hero& hero,const World& world)
{
    if(dt<=0||!std::isfinite(dt))return;
    if(locked()){hero.position.x=std::clamp(hero.position.x,arena.left+.5f,arena.right-.5f);}
    if(titleAge_>=0)titleAge_+=dt;
    comboAge_+=dt;
    for(unsigned i=0;i<capacity;++i){auto& a=actors[i];if(!a.visible())continue;
        PresentationPosition::Step history(a.presentation,a.position);
        const auto& p=greenpathProfile(a.kind);a.age+=dt;a.timer-=dt;a.flash=std::max(0.f,a.flash-dt);
        const Vec2 lunge=a.velocity;
        if(a.phase==GreenpathPhase::Dead){
            if(a.kind==GreenpathKind::VolatileMosskin&&a.timer<=0){
                const Vec2 d{hero.position.x-a.position.x,hero.position.y-a.position.y};if(d.x*d.x+d.y*d.y<9)hero.takeDamage(2,sign(-d.x));
            }
            if(a.timer<=0){
                if(a.kind==GreenpathKind::Hornet){
                    flags_|=hornetDefeatedFlag;needle.active=false;sphereVisible_=false;
                    a.velocity={a.facing*17,7};enter(i,GreenpathPhase::Escape,.45f,"Escape");
                }else a.phase=GreenpathPhase::Gone;
            }
            continue;
        }
        if(a.phase==GreenpathPhase::Escape){
            objects[i].colliderEnabled=false;
            a.position.x+=a.velocity.x*dt;a.position.y+=a.velocity.y*dt;a.velocity.y-=24*dt;
            if(a.timer<=0){a.phase=GreenpathPhase::Gone;a.velocity={};}
            continue;
        }
        const Vec2 delta{hero.position.x-a.position.x,hero.position.y-a.position.y};const float distance=std::sqrt(delta.x*delta.x+delta.y*delta.y);
        if(a.kind==GreenpathKind::Hornet){
            if(a.phase==GreenpathPhase::Dormant){
                const Rect trigger=authoredIntro_?trigger_:Rect{arena.left-1,arena.bottom-1,arena.right+1,arena.top+1};
                if(overlap(trigger,bodyBounds(hero.position)))beginIntro(i);
                objects[i].colliderEnabled=false;continue;
            }
            if(a.phase==GreenpathPhase::Intro){
                hero.controlEnabled=false;hero.inputEnabled=false;hero.velocity={};a.velocity={};objects[i].colliderEnabled=false;
                if(introPage_>=4){
                    if(authoredIntro_)a.position.x+=(a.home.x-a.position.x)*std::min(1.f,dt*12);
                    if(a.timer<=0){if(authoredIntro_)a.position=a.home;
                        hero.controlEnabled=hero.inputEnabled=true;objects[i].colliderEnabled=true;
                        titleAge_=.001f;enter(i,GreenpathPhase::Idle,.625f,"Idle");}
                }
                continue;
            }
            objects[i].colliderEnabled=true;
            if(a.phase==GreenpathPhase::Idle&&a.timer<=0){
                a.facing=sign(delta.x);
                // Source throw bands are 22.51/30.16 inside 16.06..36.53.
                // Normalize only for the standalone test arena.
                const float left=arena.left+(arena.right-arena.left)*(22.51f-16.06f)/(36.53f-16.06f);
                const float right=arena.left+(arena.right-arena.left)*(30.16f-16.06f)/(36.53f-16.06f);
                const bool canThrow=(a.position.x<left&&delta.x>0)||(a.position.x>right&&delta.x<0);
                const unsigned attack=chooseAttack(canThrow);++a.attacks;
                // approximation: collapse source proximity-trigger callbacks
                // to a body-range check, without making evade a cyclic attack.
                if(std::abs(delta.x)<2.75f&&std::abs(delta.y)<1.5f&&random(0,1)<.5f)
                    enter(i,GreenpathPhase::EvadeAntic,.25f,"EvadeAntic");
                else if(attack==0)enter(i,GreenpathPhase::JumpAntic,1.f/3,"JumpAntic");
                else if(attack==1)enter(i,GreenpathPhase::SphereAntic,10.f/12,"SphereAntic");
                else if(attack==2)enter(i,GreenpathPhase::Antic,10.f/18,"Antic");
                else enter(i,GreenpathPhase::Throw,14.f/18,"ThrowAntic");
            }else if(a.phase==GreenpathPhase::Antic&&a.timer<=0){
                a.velocity={a.facing*25.f,0};enter(i,GreenpathPhase::Attack,.35f,"Attack");
            }else if(a.phase==GreenpathPhase::Throw&&a.timer<=0){
                // Source Throw: spawn at pivot - 0.5 and aim the needle at the hero from the pivot.
                const Vec2 spawn{a.position.x,a.position.y+greenpathNeedleDropY};
                const float aim=std::atan2(hero.position.y-a.position.y,hero.position.x-a.position.x);
                needle={};needle.active=true;needle.angle=aim;needle.origin=needle.position=spawn;
                needle.velocity={greenpathNeedleSpeed*std::cos(aim),greenpathNeedleSpeed*std::sin(aim)};needle.presentation.reset(needle.position);
                enter(i,GreenpathPhase::Recover,1.7f,"Throw");
            }else if(a.phase==GreenpathPhase::JumpAntic&&a.timer<=0){
                a.velocity={a.facing*6,16};enter(i,GreenpathPhase::Jump,.38f,"Jump");
            }else if(a.phase==GreenpathPhase::Jump&&a.timer<=0){
                a.velocity={};enter(i,GreenpathPhase::AirAntic,.45f,"AirAntic");
            }else if(a.phase==GreenpathPhase::AirAntic&&a.timer<=0){
                const float length=std::max(distance,.01f);a.velocity={delta.x/length*30,delta.y/length*30};enter(i,GreenpathPhase::Attack,.42f,"AirAttack");
            }else if(a.phase==GreenpathPhase::SphereAntic&&a.timer<=0){
                sphereVisible_=true;sphereAge_=0;sphereRadius_=.4f;spherePosition_=a.position;
                enter(i,GreenpathPhase::Sphere,1.f,"Sphere");
            }else if(a.phase==GreenpathPhase::EvadeAntic&&a.timer<=0){
                a.velocity={-a.facing*22,0};enter(i,GreenpathPhase::Evade,.25f,"Evade");
            }else if(a.phase==GreenpathPhase::Evade&&a.timer<=0){
                a.velocity={};enter(i,GreenpathPhase::Recover,.25f,"Recover");
            }else if(a.phase==GreenpathPhase::Attack&&a.timer<=0){enter(i,GreenpathPhase::Recover,6.f/18,"Recover");}
            else if(a.phase==GreenpathPhase::Sphere&&a.timer<=0){sphereVisible_=false;sphereRadius_=0;a.velocity={};enter(i,GreenpathPhase::Recover,.3f,"Recover");}
            else if((a.phase==GreenpathPhase::Recover||a.phase==GreenpathPhase::Stagger)&&a.timer<=0){a.velocity.x=0;enter(i,GreenpathPhase::Idle,phaseTwo_?random(.1f,.4f):random(.5f,.75f),"Idle");}
            if(a.phase==GreenpathPhase::Recover)a.velocity.x*=std::pow(.77f,dt*60);
            if(a.phase==GreenpathPhase::Sphere){
                a.velocity={};spherePosition_=a.position;sphereAge_+=dt;sphereRadius_=std::min(3.2f,.4f+sphereAge_*5.2f);
                if(distance<sphereRadius_+1.1f)hero.takeDamage(1,sign(-delta.x));
            }
        }else if(flying(a.kind)){
            if(a.kind==GreenpathKind::Squit){
                // Fungus1_02 Mosquito (level130-925) FSM "Mozzie".
                // Alert Range New: circle radius .41109946 x scale 21.115948, plus CheckCanSeeHero.
                const auto alerted=[&]{
                    return inCircle(a.position,.41109946f*21.115948f*a.size.x,bodyBounds(hero.position))&&lineOfSight(a.position,hero.position,world);
                };
                // DistanceFly distance 8 speedMax 5.5 acceleration .1 targetsHeight height 1.
                const auto distanceFly=[&]{
                    const float toward=distance>8?.1f:-.1f;
                    a.velocity.x+=a.position.x<hero.position.x?toward:-toward;
                    if(a.position.y<hero.position.y+1)a.velocity.y+=.1f;
                    if(a.position.y>hero.position.y+1)a.velocity.y-=.1f;
                    a.velocity.x=std::clamp(a.velocity.x,-5.5f,5.5f);a.velocity.y=std::clamp(a.velocity.y,-5.5f,5.5f);
                    if(delta.x!=0)a.facing=sign(delta.x);
                };
                const auto chase=[&]{a.sightTimer=0;enter(i,GreenpathPhase::Move,0,"Idle");};
                switch(a.phase){
                case GreenpathPhase::Idle:
                    if(alerted()){if(delta.x!=0)a.facing=sign(delta.x);chase();break;}
                    // IdleBuzz waitMin .75 waitMax 1 speedMax 3 accelerationMax 19 roamingRange 1.
                    {
                        constexpr float accelerationMax=19.f;
                        const auto boundary=[&](float p,float origin,float& v,float& acc){
                            if((p<origin-1&&v<0)||(p>origin+1&&v>0)){acc=(p<origin?accelerationMax:-accelerationMax)/2000;v/=1.125f;a.buzzWait=random(.75f,1.f);}
                        };
                        boundary(a.position.y,a.home.y,a.velocity.y,a.buzzAcceleration.y);
                        boundary(a.position.x,a.home.x,a.velocity.x,a.buzzAcceleration.x);
                        if(a.buzzWait<=0){
                            const auto sample=[&](float p,float origin){return random(p<origin-1?0.f:-accelerationMax,p>origin+1?0.f:accelerationMax)/2000;};
                            a.buzzAcceleration.y=sample(a.position.y,a.home.y);a.buzzAcceleration.x=sample(a.position.x,a.home.x);
                            a.buzzWait=random(.75f,1.f);
                        }
                        if(a.buzzWait>0)a.buzzWait-=dt;
                        a.velocity.x=std::clamp(a.velocity.x+a.buzzAcceleration.x,-3.f,3.f);
                        a.velocity.y=std::clamp(a.velocity.y+a.buzzAcceleration.y,-3.f,3.f);
                        if(std::abs(a.velocity.x)>.01f)a.facing=sign(a.velocity.x);
                    }
                    break;
                case GreenpathPhase::Move:
                    // Chase In Sight / Out of Sight: attack within 10 units while seen; Attention Span 8 s unseen.
                    distanceFly();
                    if(alerted()){a.sightTimer=0;if(distance<=10)enter(i,GreenpathPhase::Pause,random(.25f,1.f),"Idle");}
                    else if((a.sightTimer+=dt)>=8)enter(i,GreenpathPhase::Stop,.75f,"Idle");
                    break;
                case GreenpathPhase::Pause:
                    // Attack Pause, then Still In Range? (alert range and sight) or back to the chase.
                    distanceFly();
                    if(a.timer<=0){
                        if(alerted()){if(delta.x!=0)a.facing=sign(delta.x);a.velocity={0,1};enter(i,GreenpathPhase::Antic,.25f,"Antic");}
                        else chase();
                    }
                    break;
                case GreenpathPhase::Antic:
                    // Attack Antic / Attack Aim rise at 1 unit/s until the Attack Antic clip completes.
                    a.velocity={0,1};
                    // A looping clip cannot hold the FSM: 1 s cap.
                    if(a.timer<=0&&(!a.animation.clip||a.animation.complete||a.timer<=-1)){
                        // GetAngleToTarget2D offset (0,-0.5); Lunge Speed 18 for Lunge Wait 1.25 s.
                        const float angle=std::atan2(delta.y-.5f,delta.x);
                        a.velocity={18*std::cos(angle),18*std::sin(angle)};a.facing=a.velocity.x<0?-1.f:1.f;
                        ++a.attacks;enter(i,GreenpathPhase::Attack,1.25f,"Attack");
                    }
                    break;
                case GreenpathPhase::Attack:
                    if(a.timer<=0)enter(i,GreenpathPhase::Recover,.5f,"Idle");
                    break;
                case GreenpathPhase::PullOut:
                    // Pull Out: Decelerate .2 while the Death Air clip plays.
                    a.velocity={decelerate(a.velocity.x,.2f),decelerate(a.velocity.y,.2f)};
                    if(a.timer<=0&&(!a.animation.clip||a.animation.complete||a.timer<=-1))enter(i,GreenpathPhase::Recover,.5f,"Idle");
                    break;
                case GreenpathPhase::Recover:
                    a.velocity={a.velocity.x*.9f,a.velocity.y*.9f};
                    if(a.timer<=0)chase();
                    break;
                case GreenpathPhase::Stop:
                    a.velocity={decelerate(a.velocity.x,.3f),decelerate(a.velocity.y,.3f)};
                    if(a.timer<=0){a.home=a.position;a.buzzWait=0;a.buzzAcceleration={};enter(i,GreenpathPhase::Idle,0,"Idle");}
                    break;
                default:break;
                }
            }else if(a.kind==GreenpathKind::Mossfly){
                // Maskfly (Pigeon): Start Size 0.8-1 (x and y), random Idle 01/02/03 at frame 0-41, 50%
                // FlipScale; Idle still until Hero Range (circle 6.34 at (0,0.752)) or Enemy Range
                // (3.53 at (0,0.82)) with a clear ray; Fly: up 0.5, Rise Force 10-35, Side Force 35-75
                // away from the hero (AddForce2d every frame, mass 1, gravity 0), FaceAngle 0 / 180,
                // trigger body (no terrain), DestroySelf after 5 s.
                if(!a.fsmStarted){
                    a.fsmStarted=true;const float s=random(.8f,1.f);a.size={s,s};a.velocity={};
                    static const char* idles[]={"Idle 01","Idle 02","Idle 03"};
                    const char* clip=idles[std::min(2,int(random(0,3)))];
                    a.clip=clip;
                    if(bank_){
                        char name[48];std::snprintf(name,sizeof(name),"%s/%s",greenpathProfile(a.kind).prefix,clip);
                        const unsigned frame=unsigned(std::min(41.f,random(0,42)));
                        if(const auto* c=bank_->require(name))a.animation.play(c,c->count?frame%c->count:0,true);
                    }
                    if(random(0,1)>=.5f)a.facing=-a.facing;
                }
                if(a.phase==GreenpathPhase::Idle){
                    a.velocity={};
                    const auto circle=[&](Vec2 c,float r){
                        c={a.position.x,a.position.y+c.y*a.size.y};r*=a.size.x;
                        const Rect h=bodyBounds(hero.position);
                        const float nx=std::clamp(c.x,h.left,h.right)-c.x,ny=std::clamp(c.y,h.bottom,h.top)-c.y;
                        return nx*nx+ny*ny<=r*r;
                    };
                    bool enemy=false;
                    const Vec2 ec{a.position.x,a.position.y+.82f*a.size.y};
                    for(unsigned k=0;k<actors.size();++k)if(k!=i&&actors[k].alive()){
                        const Rect b=actors[k].bounds();
                        const float nx=std::clamp(ec.x,b.left,b.right)-ec.x,ny=std::clamp(ec.y,b.bottom,b.top)-ec.y;
                        enemy|=nx*nx+ny*ny<=3.53f*a.size.x*3.53f*a.size.x;
                    }
                    if((circle({0,.752f},6.34f)&&lineOfSight(a.position,hero.position,world))||enemy){
                        a.position.y+=.5f;a.lungeSpeed=random(10,35);
                        const bool left=hero.position.x>a.position.x;
                        a.stateClock=left?random(-75,-35):random(35,75);
                        a.facing=left?-1.f:1.f;a.timer=5;enter(i,GreenpathPhase::Move,5,"Move");
                    }
                }else if(a.phase==GreenpathPhase::Move){
                    a.velocity.x+=a.stateClock*dt;a.velocity.y+=a.lungeSpeed*dt;
                    a.rotation=std::atan2(a.velocity.y,a.velocity.x)+(a.facing<0?3.14159265f:0.f);
                    if(a.timer<=0){a.phase=GreenpathPhase::Gone;objects[i].colliderEnabled=false;}
                }
            }else if(a.kind==GreenpathKind::Aluba){
                // Lazy Flyer: Start X +- Flight Distance (per placement); random first leg; Fly Speed
                // 3-4 toward the leg, Wait 2 s, then past a bound -> Turn Pause 0-1 s -> Turn clip
                // (0.5 s, DecelerateV2 0.95) -> the other leg. Gravity 0, no vertical motion.
                const auto leg=[&](bool right){
                    a.facing=right?1.f:-1.f;a.lungeSpeed=right?random(3,4):random(-4,-3);
                    enter(i,GreenpathPhase::Move,2,"Fly");
                };
                if(!a.fsmStarted){a.fsmStarted=true;a.home=a.position;leg(random(0,1)>=.5f);}
                if(a.phase==GreenpathPhase::Move){
                    a.velocity={a.lungeSpeed,0};
                    if(a.timer<=0&&(a.position.x<a.home.x-a.flightDistance||a.position.x>a.home.x+a.flightDistance))
                        {a.phase=GreenpathPhase::Pause;a.timer=random(0,1);}
                }else if(a.phase==GreenpathPhase::Pause){
                    a.velocity={a.lungeSpeed,0};
                    if(a.timer<=0)enter(i,GreenpathPhase::Turn,.5f,"Turn");
                }else if(a.phase==GreenpathPhase::Turn){
                    a.velocity={a.velocity.x*.95f,a.velocity.y*.95f};
                    if(a.timer<=0)leg(a.facing<0);
                }
            }else {a.velocity.x=a.facing*p.speed;}
        }else if(a.kind==GreenpathKind::MossKnight){
            // Fungus1_21 Moss Knight (level149-1839), FSM "Moss Knight Control" with its Walker
            // (3.5, no pauses, turnPause 1, turns to a seen hero inside Alert Range New).
            // Child regions (local, then the placement scale):
            //   Attack Range  (0,2.14) x (8.49,8.33): box offset Y -0.0756 size (1.18,0.8487)
            //   Evade Range   (0,5.36) x (1,8.33):    box offset Y -0.3117 size (4.62,0.3766)
            //   Spit Range    (0,-0.6) x (22.86,1.51): box offset (-0.5468,0.8800) size (0.1077,4.2507)
            //   Alert Range New (0,-0.6) x (22.86,1.51): box offset Y -0.3727 size (1.52,1.7454)
            const Rect heroBox=bodyBounds(hero.position);
            const float face=a.facing;
            const auto region=[&](float x,float y,float halfX,float halfY){
                // x is local: the art's left (+x) is mirrored by the facing; Unity scale X = -facing.
                x*=-face*a.size.x;y*=a.size.y;halfX*=a.size.x;halfY*=a.size.y;
                return overlap({a.position.x+x-halfX,a.position.y+y-halfY,a.position.x+x+halfX,a.position.y+y+halfY},heroBox);
            };
            const bool seen=lineOfSight(a.position,hero.position,world);
            const bool inAttack=region(0,2.14f-8.33f*.07564592f,1.18f*8.49f/2,.8487088f*8.33f/2);
            const bool inEvade=region(0,5.36f-8.33f*.3116935f,4.62f/2,.37661344f*8.33f/2);
            const bool inSpit=region(-.5468133f*22.86f,-.6000004f+.87995404f*1.51f,.107681006f*22.86f/2,4.250727f*1.51f/2);
            const bool inAlert=region(0,-.6f-.372705f*1.51f,1.52f*22.86f/2,1.7454097f*1.51f/2);
            const float toward=delta.x>0?1.f:-1.f;
            auto& health=objects[i].health;
            // Clip completion when a bank is bound; the original frame timings otherwise.
            const bool done=a.animation.clip&&a.animation.complete;
            a.stateClock+=dt;
            const auto state=[&](GreenpathPhase next,const char* clip){a.stateClock=0;enter(i,next,0,clip);};
            const auto at=[&](float frames,float fps){return a.stateClock>=frames/fps;};
            const bool quick=a.quickSlash; // Slash Quick after an evade (Evade Hero), else Slash
            const auto slash=[&](float speed,bool quickSlash){
                a.velocity.x=0;health.invincible=false;a.lungeSpeed=speed;a.quickSlash=quickSlash;++a.attacks;
                state(GreenpathPhase::Antic,quickSlash?"Attack":"Slash");
            };
            const auto shoot=[&]{
                // Shoot Check: Shot Repeats - 1, face the hero, Shoot clip (12 fps, trigger 6).
                a.velocity.x=0;health.invincible=false;--a.shotRepeats;a.shot=false;a.facing=toward;state(GreenpathPhase::Throw,"Shoot");
            };
            const auto evade=[&](bool slashAfter,bool quickSlash){
                // Evade Check: velocity 0, Evade clip, face the hero; Lunge1 13 afterwards; a wall
                // 4 units behind cancels: SPIT -> Attack 1 Antic, SLASH -> evade regardless.
                a.velocity.x=0;health.invincible=false;a.facing=toward;a.lungeSpeed=13;a.quickSlash=quickSlash;
                a.slashAfterEvade=slashAfter;
                float hit=0;
                const Rect b=a.bounds();
                const bool wall=rayTerrain({(b.left+b.right)/2,(b.bottom+b.top)/2},{-toward,0},4,world,hit,TerrainShape::terrainQueryExcluded);
                if(wall&&!slashAfter)slash(13,quickSlash);
                else state(GreenpathPhase::EvadeAntic,"Evade");
            };
            const auto attackChoice=[&]{
                // Attack Choice: SLASH 0.75 (Slash), JUMP BACK 0.25 -> After Evade SPIT or SLASH (0.5 each).
                a.velocity.x=0;
                if(random(0,1)<.75f)slash(11,false);
                else evade(random(0,1)>=.5f,false);
            };
            const auto shieldClip=[&](bool high){return high?"ShieldTop":"ShieldFront";};
            switch(a.phase){
            case GreenpathPhase::Idle:state(GreenpathPhase::Move,"Move");break;
            case GreenpathPhase::Move:{
                health.invincible=false;
                // Detect: SPIT, then EVADE, then ATTACK (sight and Attack Range).
                if(inSpit){a.shotRepeats=2;shoot();break;}
                if(inEvade){evade(true,true);break;}
                if(inAttack&&seen){
                    // Shield Start: StopWalker, Attack Pause 0.75-1.1 s, invincible.
                    a.velocity.x=0;a.facing=toward;a.attackPause=random(.75f,1.1f);
                    a.lungeSpeed=11;state(GreenpathPhase::Shield,shieldClip(hero.position.y>a.position.y+2.5f));break;
                }
                // Walker.UpdateWalking: wall sweep and hole check at extents + 0.5, turn to a seen hero.
                if(a.turnCooldown>0)a.turnCooldown-=dt;
                if(a.turnCooldown<=0){
                    const Rect b=a.bounds();const float half=(b.right-b.left)/2;float hit=0;
                    const bool wall=rayTerrain({a.position.x,(b.bottom+b.top)/2},{face,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded)||
                                    rayTerrain({a.position.x,b.bottom+.1f},{face,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded);
                    const bool hole=!rayTerrain({a.position.x+(half+.5f)*face,b.bottom+.1f},{0,-1},.35f,world,hit,TerrainShape::terrainQueryExcluded);
                    const bool turnToHero=toward!=face&&seen&&inAlert;
                    if(wall||hole||turnToHero){a.velocity.x=0;a.turnCooldown=1;state(GreenpathPhase::Turn,"Turn");break;}
                }
                a.velocity.x=face*3.5f;
                break;
            }
            case GreenpathPhase::Turn:
                a.velocity.x=0;
                if(done||at(2,12)){a.facing=-a.facing;state(GreenpathPhase::Move,"Move");}
                break;
            case GreenpathPhase::Shield:{
                // Shield Right/Left High/Low: high when Hero Y > Self Y + Y Adjust 2.5; blocks by direction.
                a.velocity.x=0;a.facing=toward;
                const bool high=hero.position.y>a.position.y+2.5f;
                if(!a.clip||std::strcmp(a.clip,shieldClip(high)))enter(i,GreenpathPhase::Shield,0,shieldClip(high));
                health.invincible=true;health.invincibleFromDirection=high?4:toward>0?6:5;
                a.attackPause-=dt;
                if(inEvade)evade(true,true);
                else if(!(inAttack&&seen)){health.invincible=false;state(GreenpathPhase::Unshield,high?"UnshieldTop":"Antic");}
                else if(a.attackPause<=0){health.invincible=false;attackChoice();}
                break;
            }
            case GreenpathPhase::Block:
                // Block Low/High: Shield Front/Top Bump for 0.4 s, then Attack Choice.
                a.velocity.x=0;
                if(a.stateClock>=.4f){health.invincible=false;attackChoice();}
                break;
            case GreenpathPhase::Unshield:
                a.velocity.x=0;health.invincible=false;
                if(done||at(2,12))state(GreenpathPhase::Move,"Move");
                break;
            case GreenpathPhase::EvadeAntic:
                // Evade (15 fps, 7 frames, triggers 2/5): Evade Speed 28 away between the triggers.
                a.velocity.x=at(2,15)&&!at(5,15)?-a.facing*28:0;
                if(at(7,15)){
                    a.velocity.x=0;
                    if(a.slashAfterEvade)slash(a.lungeSpeed,a.quickSlash);else{a.shotRepeats=2;shoot();}
                }
                break;
            case GreenpathPhase::Antic:{
                // Attack 1: Slash (18 fps, 21 frames, triggers 13-16) or Slash Quick (15 fps, 10 frames, 5-8):
                // lunge from the first trigger, Slash Hitbox between the 2nd and 3rd, stop at the 4th.
                const float fps=quick?15.f:18.f,first=quick?5.f:13.f,total=quick?10.f:21.f;
                a.velocity.x=at(first,fps)&&!at(first+3,fps)?a.facing*a.lungeSpeed:0;
                if(at(first+1,fps)&&!at(first+2,fps)){
                    static constexpr float hitbox[][2]={{-.33193f,1.21123f},{-1.07074f,-2.01454f},{-4.24039f,-1.41254f},
                        {-5.72115f,-.27646f},{-4.79734f,.8054f},{-2.44477f,1.28974f}};
                    if(strikes(a,hero,hitbox))hero.takeDamage(1,sign(a.position.x-hero.position.x));
                }
                if(at(total,fps)||(done&&at(first+3,fps))){
                    // Slash 2?: a hero on the facing side gets a second slash half the time.
                    if(toward==a.facing&&random(0,1)<.5f)state(GreenpathPhase::Attack,"Slash2");
                    else state(GreenpathPhase::Recover,"SlashEnd");
                }
                break;
            }
            case GreenpathPhase::Attack:
                // Slash 2 (24 fps, 29 frames, triggers 10/11): Slash2 Hitbox and the lunge between them.
                a.velocity.x=at(10,24)&&!at(11,24)?a.facing*a.lungeSpeed:0;
                if(at(10,24)&&!at(11,24)){
                    static constexpr float hitbox2[][2]={{-2.85537f*.8625001f,.85715f},{.3171f*.8625001f,.8037f},
                        {-1.31223f*.8625001f,-1.90888f},{-2.83668f*.8625001f,-2.12194f},{-4.64949f*.8625001f,-1.8764f},
                        {-5.81415f*.8625001f,-1.14449f},{-5.95956f*.8625001f,-.49266f},{-5.13145f*.8625001f,.33883f}};
                    if(strikes(a,hero,hitbox2))hero.takeDamage(1,sign(a.position.x-hero.position.x));
                }
                if(done||at(29,24))state(GreenpathPhase::Move,"Move");
                break;
            case GreenpathPhase::Recover:
                // Slash End (12 fps, 5 frames), then Reset.
                a.velocity.x=0;
                if(done||at(5,12))state(GreenpathPhase::Move,"Move");
                break;
            case GreenpathPhase::Throw:
                // Shoot Antic to trigger 6, then Shoot: Grass Ball from the Shot Point at
                // ((Hero X - Self X) * 1.1 + Random(-1,1), clamped to 15; 20 up); repeat check at the end.
                a.velocity.x=0;
                if(at(6,12)&&!a.shot){
                    a.shot=true;
                    const float spitX=std::clamp((hero.position.x-a.position.x)*1.1f+random(-1,1),-15.f,15.f);
                    spawnBall({a.position.x+2.07f*a.facing*a.size.x,a.position.y+1.17f*a.size.y},{spitX,20});
                }
                if(done||at(10,12)){
                    if(a.shotRepeats<=0||random(0,1)<.6f)state(GreenpathPhase::Stop,"ShootEnd");else shoot();
                }
                break;
            case GreenpathPhase::Stop:
                // Shot End (Shoot End clip), then Reset.
                a.velocity.x=0;
                if(done||at(2,12))state(GreenpathPhase::Move,"Move");
                break;
            default:break;
            }
        }else if(a.kind==GreenpathKind::MossCharger){
            // Fungus1_10 Moss Charger (level139-240), FSM "Mossy Control". Attack Range child:
            // Transform (0,-0.48) scale X 1.175, BoxCollider2D offset Y -0.99261045 size (28,1.3816643).
            // X Min/Max = Start X -/+ (Attack Range Length / 2 - 2); emerge 14 from the hero; Charge Speed 15.
            constexpr float halfRange=28*1.1750001f/2,centerY=-.48f-.99261045f,halfY=1.3816643f/2;
            // BoundsBoxCollider reads the world-space Attack Range, so X Min/Max follow the placement scale.
            const float reach=halfRange*a.size.x-2;
            const float charge=a.facing*15;
            const bool clipDone=!a.animation.clip||a.animation.complete||a.timer<=-1;
            objects[i].colliderEnabled=a.phase==GreenpathPhase::Antic||a.phase==GreenpathPhase::Attack||a.phase==GreenpathPhase::Stagger;
            if(a.phase==GreenpathPhase::Idle){
                // Hidden: CheckAlertRange against the hero collider.
                a.velocity={};
                const Rect range{a.home.x-halfRange*a.size.x,a.home.y+(centerY-halfY)*a.size.y,
                                 a.home.x+halfRange*a.size.x,a.home.y+(centerY+halfY)*a.size.y};
                if(overlap(range,bodyBounds(hero.position)))enter(i,GreenpathPhase::Pause,random(.5f,1.f),"Idle",true);
            }else if(a.phase==GreenpathPhase::Pause){
                // Emerge Pause, Hero Beyond?, then a random side; a side beyond X Min/Max takes the other.
                if(a.timer<=0){
                    if(hero.position.x>a.home.x+reach||hero.position.x<a.home.x-reach)enter(i,GreenpathPhase::Idle,0,"Idle",true);
                    else{
                        float direction=random(0,1)<.5f?-1.f:1.f;
                        if(direction<0&&hero.position.x+14>a.home.x+reach)direction=1;
                        else if(direction>0&&hero.position.x-14<a.home.x-reach)direction=-1;
                        a.facing=direction;a.position={hero.position.x-direction*14,a.home.y};
                        a.presentation.reset(a.position);
                        // Emerge: Emerge Speed = Current Charge Speed * 0.25 during the Appear clip.
                        a.velocity={direction*15*.25f,0};++a.attacks;
                        enter(i,GreenpathPhase::Antic,0,"Antic");
                    }
                }
            }else if(a.phase==GreenpathPhase::Antic){
                a.velocity.x=charge*.25f;
                if(clipDone)enter(i,GreenpathPhase::Attack,0,"Charge");
            }else if(a.phase==GreenpathPhase::Attack){
                // Charge: forward ray from (0,-0.5) 5.5 long, ground ray from (6.5 forward,-0.5) 3 down.
                a.velocity.x=charge;
                float hit=0;
                const bool wall=rayTerrain({a.position.x,a.position.y-.5f},{a.facing,0},5.5f,world,hit,TerrainShape::terrainQueryExcluded);
                const bool ground=rayTerrain({a.position.x+a.facing*6.5f,a.position.y-.5f},{0,-1},3,world,hit,TerrainShape::terrainQueryExcluded);
                if(wall||!ground)enter(i,GreenpathPhase::Recover,0,"Recover");
            }else if(a.phase==GreenpathPhase::Recover){
                // Submerge: Decelerate 0.7 through the Disappear clip; Submerge CD returns to Start after 0.35 s.
                a.velocity.x=a.velocity.x<0?std::min(0.f,a.velocity.x+.7f):std::max(0.f,a.velocity.x-.7f);
                if(clipDone){a.position=a.home;a.presentation.reset(a.position);a.velocity={};enter(i,GreenpathPhase::Stop,.35f,"Idle",true);}
            }else if(a.phase==GreenpathPhase::Stop){
                a.velocity={};
                if(a.timer<=0)enter(i,GreenpathPhase::Idle,0,"Idle",true);
            }else if(a.phase==GreenpathPhase::Stagger){
                // Burst / In Air: knocked away, gravity 1.5 until it lands, then it submerges.
                if(a.timer<=0&&a.velocity.y<=0&&std::abs(a.velocity.y)<.01f)enter(i,GreenpathPhase::Recover,0,"Recover");
            }
        }else if(a.kind==GreenpathKind::Mosscreep){
            if(a.phase==GreenpathPhase::Idle)enter(i,GreenpathPhase::Move,0,"Move");
            else if(a.phase==GreenpathPhase::Turn){a.velocity.x=0;if(a.timer<=0){a.facing=-a.facing;enter(i,GreenpathPhase::Move,0,"Move");}}
            else if(a.phase==GreenpathPhase::Move){
                a.velocity.x=a.facing*p.speed;
                // Leaving the home band turns through the Turn clip, like a wall, instead of snapping.
                if(std::fabs(a.position.x-a.home.x)>5&&sign(a.home.x-a.position.x)!=a.facing){a.velocity.x=0;enter(i,GreenpathPhase::Turn,mosscreepTurnTime,"Turn");}
            }
        }else if(a.kind==GreenpathKind::Durandoo){
            // Fungus1_11 Acid Walker (level140-196) FSM "Acid Walker": Check Dir random LEFT (scale 1)
            // / RIGHT (scale -1); Start Walk: Walk Speed 3.5, Walk clip, Wait 0.5; Walk turns while
            // Wall Range (terrain, layer 8) or Enemy Range (another enemy) is entered; Turn (5 @ 15),
            // FlipScale. Child triggers (local, art faces left): Wall Range (-0.828,-0.497) 2.655 x 4.744,
            // Enemy Range (-2.090,-0.497) 0.558 x 4.744. Gravity 1. BLOCKED DOWN only replays the
            // walk (BounceToWalk) on the invincible-from-above shell.
            const auto range=[&](float x,float w){
                const float cx=a.position.x-x*a.facing*a.size.x,cy=a.position.y-.49661255f*a.size.y;
                const float hw=w*a.size.x/2,hh=4.74364f*a.size.y/2;
                return Rect{cx-hw,cy-hh,cx+hw,cy+hh};
            };
            const auto startWalk=[&]{a.velocity.x=a.facing*3.5f;enter(i,GreenpathPhase::Recover,.5f,"Move");};
            if(a.phase==GreenpathPhase::Idle){a.facing=random(0,1)<.5f?-1.f:1.f;startWalk();}
            else if(a.phase==GreenpathPhase::Recover){if(a.timer<=0)a.phase=GreenpathPhase::Move;}
            else if(a.phase==GreenpathPhase::Move){
                const Rect enemy=range(-2.0895004f,.5576458f);
                bool other=false;
                for(unsigned k=0;k<actors.size();++k)if(k!=i&&actors[k].alive()&&overlap(enemy,actors[k].bounds()))other=true;
                if(other||terrainSensorOverlap(range(-.8276825f,2.6553497f),world)){a.velocity.x=0;enter(i,GreenpathPhase::Turn,5.f/15,"Turn");}
            }else if(a.phase==GreenpathPhase::Turn){
                a.velocity.x=0;
                if(a.timer<=0){a.facing=-a.facing;startWalk();}
            }
            a.velocity.y+=tuning::gravity*dt;
        }else if(a.kind==GreenpathKind::VolatileMosskin){
            // Fungus1_01 Mossman_Shaker (level128-1282): Walker (speed 2, walk 1.5-4 s, pause 1.5-2.5 s,
            // turnPause 1, wall/hole sweeps at extents + 0.5, turns to a seen hero in range) and FSM
            // "Fungus Zombie Attack": Ready (Can See Hero and Attack Range: (0,-0.48) x (11.14,3.45),
            // box 0.7 x 1) -> Attack Delay 0-0.75 -> Attack Antic (StopWalker, Attack clip, 0.75 s) ->
            // Attack (Gas Hit Box 0.2 -> 1 in 0.4 s, easeOutCirc, 0.8 s) -> CD 0.5 -> Idle Pause 0.5 ->
            // StartWalker. Clips: Walk 10 @ 12, Idle 5 @ 12, Turn 2 @ 12, Attack 8 @ 15 (loop from 5).
            const Rect heroBox=bodyBounds(hero.position);
            const float hx=3.899f*a.size.x,hy=1.725f*a.size.y,cy=a.position.y-.48f*a.size.y;
            const bool inRange=overlap({a.position.x-hx,cy-hy,a.position.x+hx,cy+hy},heroBox);
            const bool seen=lineOfSight(a.position,hero.position,world);
            const float toward=delta.x>0?1.f:-1.f;
            const auto walk=[&]{a.velocity.x=a.facing*2;a.attackPause=random(1.5f,4.f);enter(i,GreenpathPhase::Move,0,"Move");};
            const auto turn=[&]{a.velocity.x=0;a.turnCooldown=1;enter(i,GreenpathPhase::Turn,2.f/12,"Turn");};
            a.stateClock+=dt;
            if(a.turnCooldown>0)a.turnCooldown-=dt;
            const bool walker=a.phase==GreenpathPhase::Idle||a.phase==GreenpathPhase::Move||a.phase==GreenpathPhase::Turn;
            if(walker&&seen&&inRange){
                // Attack Delay (WaitRandom 0-0.75): the walker keeps going until Attack Antic stops it.
                a.stateClock=0;a.timer=random(0,.75f);a.phase=GreenpathPhase::Pause;
            }
            switch(a.phase){
            case GreenpathPhase::Idle:
                // Walker Stopped (Bored): Idle clip, pause 1.5-2.5 s, then walk on (turnAfterIdle 0%).
                a.velocity.x=0;
                if(!a.clip||std::strcmp(a.clip,"Idle")){a.timer=random(1.5f,2.5f);play(i,"Idle");}
                if(a.timer<=0)walk();
                break;
            case GreenpathPhase::Move:{
                if(!a.clip||std::strcmp(a.clip,"Move")){walk();break;}
                if(a.turnCooldown<=0){
                    const Rect b=a.bounds();const float half=(b.right-b.left)/2;float hit=0;
                    const bool wall=rayTerrain({a.position.x,(b.bottom+b.top)/2},{a.facing,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded)||
                                    rayTerrain({a.position.x,b.bottom+.1f},{a.facing,0},half+.5f,world,hit,TerrainShape::terrainQueryExcluded);
                    const bool hole=!rayTerrain({a.position.x+(half+.5f)*a.facing,b.bottom+.1f},{0,-1},.35f,world,hit,TerrainShape::terrainQueryExcluded);
                    if(wall||(toward!=a.facing&&seen&&inRange)||hole){turn();break;}
                }
                a.attackPause-=dt;
                if(a.attackPause<=0){a.velocity.x=0;a.timer=random(1.5f,2.5f);enter(i,GreenpathPhase::Idle,a.timer,"Idle");break;}
                a.velocity.x=a.facing*2;
                break;
            }
            case GreenpathPhase::Turn:
                a.velocity.x=0;
                if(a.timer<=0){a.facing=-a.facing;walk();}
                break;
            case GreenpathPhase::Pause:
                // The Walker still runs during Attack Delay: it turns to a seen hero behind it.
                if(a.turnCooldown<=0&&toward!=a.facing){a.velocity.x=0;a.turnCooldown=1;a.facing=toward;play(i,"Turn");}
                if(a.timer<=0){a.velocity.x=0;a.stateClock=0;enter(i,GreenpathPhase::Antic,.75f,"Attack");}
                break;
            case GreenpathPhase::Antic:
                a.velocity.x=0;
                if(a.timer<=0){a.stateClock=0;a.phase=GreenpathPhase::Attack;a.timer=.8f;}
                break;
            case GreenpathPhase::Attack:{
                a.velocity.x=0;
                const float t=std::clamp((a.stateClock-.005f)/.4f,0.f,1.f);
                const float s=.2f+.8f*std::sqrt(1-(t-1)*(t-1));
                static constexpr float gas[][2]={{-2.785633f,2.3829384f},{.022512436f,3.2527008f},{2.8026066f,2.476822f},
                    {4.3363056f,.16165924f},{4.5983067f,-1.4609756f},{-4.5192795f,-1.4412193f},{-4.270109f,.30901718f}};
                Vec2 points[7];
                for(unsigned k=0;k<7;++k)points[k]={a.position.x-a.facing*a.size.x*s*gas[k][0],
                                                   a.position.y+a.size.y*(-1.42f+s*(gas[k][1]+1.44f))};
                if(polygonOverlapsBox(points,7,heroBox))hero.takeDamage(1,sign(a.position.x-hero.position.x));
                if(a.timer<=0){a.phase=GreenpathPhase::Recover;a.timer=.5f;}
                break;
            }
            case GreenpathPhase::Recover:
                a.velocity.x=0;
                if(a.timer<=0)enter(i,GreenpathPhase::Stop,.5f,"Idle");
                break;
            case GreenpathPhase::Stop:
                a.velocity.x=0;
                if(a.timer<=0)walk();
                break;
            default:break;
            }
            a.velocity.y+=tuning::gravity*dt;
        }else{a.velocity.x=a.facing*p.speed;if(std::fabs(a.position.x-a.home.x)>5)a.facing=sign(a.home.x-a.position.x);if(a.phase==GreenpathPhase::Idle)enter(i,GreenpathPhase::Move,0,"Move");}
        if(p.gravity&&a.kind!=GreenpathKind::VolatileMosskin&&a.phase!=GreenpathPhase::Sphere&&a.phase!=GreenpathPhase::AirAntic)a.velocity.y-=p.gravity*dt;
        // Physics keeps its authored mirror convention independently of sprite art.
        // Acid Walker: both root boxes (body and legs) collide with terrain.
        const Rect terrainBody=a.kind==GreenpathKind::Durandoo?
            Rect{-1.42745352f*a.size.x,-2.9843445f*a.size.y,.90066509f*a.size.x,1.60990822f*a.size.y}:a.body();
        Contacts c{};
        if(a.kind==GreenpathKind::Mossfly){a.position.x+=a.velocity.x*dt;a.position.y+=a.velocity.y*dt;}
        else c=moveBox(a.position,a.velocity,boxBounds({},terrainBody,-a.facing),dt,world);
        // Mozzie Lunging: Collision2dEvent HIT WALL -> Pull Out bounces back at -0.3 of the lunge.
        if(a.kind==GreenpathKind::Squit&&a.phase==GreenpathPhase::Attack&&(c.left||c.right||c.ground||c.roof)){
            a.velocity={-.3f*lunge.x,-.3f*lunge.y};enter(i,GreenpathPhase::PullOut,0,"Death");
        }
        const bool ownWalls=a.kind==GreenpathKind::MossCharger||a.kind==GreenpathKind::Durandoo||a.kind==GreenpathKind::VolatileMosskin;

        if(a.kind==GreenpathKind::Mosscreep){
            if((c.left||c.right)&&a.phase==GreenpathPhase::Move){a.velocity.x=0;enter(i,GreenpathPhase::Turn,mosscreepTurnTime,"Turn");}
        }else if((c.left||c.right)&&a.kind!=GreenpathKind::Hornet&&a.kind!=GreenpathKind::Aluba&&!ownWalls){a.facing=-a.facing;a.velocity.x=-a.velocity.x;}
        // A charge into a wall stops and stuns (it used to reverse and charge back); walkers turn in place.
        if(a.kind==GreenpathKind::MossCharger&&a.phase==GreenpathPhase::Stagger&&c.ground&&a.timer<=0){a.velocity={};enter(i,GreenpathPhase::Recover,0,"Recover");}
        else if((c.left||c.right)&&ownWalls&&a.kind!=GreenpathKind::MossCharger&&a.kind!=GreenpathKind::Durandoo&&a.kind!=GreenpathKind::VolatileMosskin&&a.phase==GreenpathPhase::Move){a.facing=-a.facing;a.velocity.x=0;enter(i,GreenpathPhase::Recover,5.f/15,"Turn");}
        if(hornetArena_&&a.kind==GreenpathKind::Hornet){a.position.x=std::clamp(a.position.x,arena.left+.6f,arena.right-.6f);a.position.y=std::max(a.position.y,arena.bottom+.8f);if(a.position.y<=arena.bottom+.81f)a.velocity.y=std::max(0.f,a.velocity.y);}
        objects[i].bounds=a.bounds();
        if(p.damage&&a.phase!=GreenpathPhase::Stagger&&objects[i].colliderEnabled&&overlap(a.bounds(),bodyBounds(hero.position)))hero.takeDamage(p.damage,sign(a.position.x-hero.position.x));
    }
    combat_.fixedUpdate(hero);
    updateBalls(dt,hero,world);
    if(needle.active){PresentationPosition::Step history(needle.presentation,needle.position);needle.age+=dt;
        if(needle.returning){
            needle.returnAge+=dt;
            // approximation: source iTween ease 12 uses a curved one-second
            // return; interpolate its endpoints with a smooth ease here.
            const float t=std::min(1.f,needle.returnAge),ease=t*t*(3-2*t);
            needle.position={needle.returnStart.x+(needle.origin.x-needle.returnStart.x)*ease,
                             needle.returnStart.y+(needle.origin.y-needle.returnStart.y)*ease};
            if(needle.returnAge>=1)needle.active=false;
        }else{
            if(needle.age>=.3f)needle.velocity={needle.velocity.x*std::pow(.8f,dt*60),needle.velocity.y*std::pow(.8f,dt*60)};
            needle.position.x+=needle.velocity.x*dt;needle.position.y+=needle.velocity.y*dt;
            if(needle.age>=.3f&&std::hypot(needle.velocity.x,needle.velocity.y)<=.5f){
                needle.returning=true;needle.hitHero=false;needle.returnStart=needle.position;
            }
        }
        // Level133 Needle PolygonCollider2D (trigger) local points span x -1.817..0.660, y -0.133..0.099.
        {
        // Bounds of the source triangle rotated so its tip follows the velocity (approximation: AABB, not the exact triangle).
        const float turn=needle.angle+3.14159265f,c=std::cos(turn),s=std::sin(turn);
        Rect needleBox{1e9f,1e9f,-1e9f,-1e9f};
        for(const auto& q:greenpathNeedlePoints){
            const float x=needle.position.x+c*q[0]-s*q[1],y=needle.position.y+s*q[0]+c*q[1];
            needleBox.left=std::min(needleBox.left,x);needleBox.right=std::max(needleBox.right,x);
            needleBox.bottom=std::min(needleBox.bottom,y);needleBox.top=std::max(needleBox.top,y);
        }
        if(!needle.hitHero&&overlap(needleBox,bodyBounds(hero.position))){
            needle.hitHero=true;hero.takeDamage(1,sign(needle.position.x-hero.position.x));
        }
        }
    }
}
void GreenpathEncounter::spawnBall(Vec2 position,Vec2 velocity)
{
    for(auto& b:balls){
        if(b.active)continue;
        // Grass Ball Init: scale 0.5 -> 1 in 0.1 s; SpinSelf starts at Random.Range(0,360) degrees.
        b={};b.active=true;b.position=position;b.velocity=velocity;b.presentation.reset(position);
        b.rotation=float(int(random(0,360)))*3.14159265f/180;
        return;
    }
}
void GreenpathEncounter::updateBalls(float dt,Hero& hero,const World& world)
{
    for(auto& b:balls){
        if(!b.active)continue;
        PresentationPosition::Step history(b.presentation,b.position);
        b.age+=dt;
        if(b.broken){if(b.age>=1)b.active=false;continue;} // Break: Wait 1, Recycle
        // SpinSelf: on the second fixed step AddTorque(vx * 15) on a circle of mass 1, radius .4
        // (inertia .08); angular damping 0 keeps the spin.
        if(b.spin==0&&b.age>=1.5f*dt)b.spin=b.velocity.x*15*dt/.08f;
        b.rotation+=b.spin*dt;
        b.velocity.y+=tuning::gravity*dt;
        const float radius=.4f*std::min(1.f,.5f+.5f*b.age/.1f);
        const Vec2 step{b.velocity.x*dt,b.velocity.y*dt};
        const float length=std::sqrt(step.x*step.x+step.y*step.y);
        float hit=0;
        // Trigger2dEventLayer layer 8 (terrain) -> Break.
        const bool terrain=length>0&&rayTerrain(b.position,{step.x/length,step.y/length},length+radius,world,hit,TerrainShape::terrainQueryExcluded);
        if(terrain){const float travel=std::max(0.f,hit-radius);b.position.x+=step.x/length*travel;b.position.y+=step.y/length*travel;}
        else{b.position.x+=step.x;b.position.y+=step.y;}
        const CombatCircle circle{b.position,radius};
        if(circleOverlapsBox(circle,bodyBounds(hero.position)))hero.takeDamage(1,b.position.x>hero.position.x?1:-1);
        // Nail Attack tag -> Break.
        const bool nailed=nails_&&hero.nail.hitbox&&nailOverlapsCircle(hero,nails_->shapes[nailVariant(hero.nail)],circle);
        if(terrain||nailed){b.broken=true;b.age=0;b.velocity={};}
    }
}
void GreenpathEncounter::update(float dt,const Hero& hero)
{if(dt<0||!std::isfinite(dt))return;combat_.update(hero,dt);for(auto& a:actors)if(a.visible())a.animation.advance(dt);}
void GreenpathRenderState::capture(const GreenpathEncounter& source,PresentationTime time)
{
    for(unsigned i=0;i<actors.size();++i){const auto& a=source.actors[i];
        actors[i]={a.animation,a.presentation.sample(a.position,time),greenpathArtScale(a.kind,a.facing)*a.size.x,
            a.phase==GreenpathPhase::Dead?std::clamp(a.timer/.4f,0.f,1.f):1,a.size.y,a.visible(),a.rotation};}
    needleVisible=source.needle.active;needle=source.needle.presentation.sample(source.needle.position,time);needleAge=source.needle.age;
    fixture=source.fixture;arena=source.arena;titleAlpha=source.titleAlpha();introPage=source.introPage();phaseSerial=source.phaseSerial();
    introVisible=false;introText="";
    for(const auto& a:source.actors)if(a.kind==GreenpathKind::Hornet&&a.phase==GreenpathPhase::Intro){
        introVisible=source.introActive();introText=introVisible?source.introText(introPage):"";break;
    }
    sphereVisible=source.sphereVisible();spherePosition=source.spherePosition();sphereRadius=source.sphereRadius();sphereAge=source.sphereAge();
    ballCount=0;
    for(const auto& b:source.balls)if(b.active&&!b.broken)
        balls[ballCount++]={b.presentation.sample(b.position,time),b.rotation,std::min(1.f,.5f+.5f*b.age/.1f)};
}
}
