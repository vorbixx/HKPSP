#include "crossroads_boss.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
constexpr Rect gruzBody{-1.25f,-1.25f,1.25f,1.25f};
// level48 False Knight New BoxCollider2D 167, relative to the artwork pivot.
constexpr Rect knightBody{-1.625f,-4.296875f,1.734375f,-.421875f};
constexpr Rect broodBody{-.35f,-.35f,.35f,.35f};
// FalseyControl: S Attack's impact wait precedes the Hitter's completion;
// S Attack Recover and Land then finish their source clips before Idle.
constexpr float knightAttackImpact=.12f,knightAttackDuration=3.f/15;
constexpr float knightAttackTail=knightAttackDuration-knightAttackImpact;
constexpr float knightAttackRecovery=5.f/12,knightLandingDuration=5.f/10;
// JA Hit starts the Hitter before LAND. JA Slam leaves on the first of
// Hitter completion or Wait(.083); JA Recoil 2's Wait(.084) overlaps Hit 3,
// which JA End watches without restarting. PlayAnimation itself never waits.
constexpr float knightJumpHitterDuration=2.f/12,knightJumpSlamWait=.083f;
constexpr float knightJumpRecoilClipDuration=2.f/12,knightJumpRecoilWait=.084f;
constexpr float knightRageCycle=5.f/12,knightRageImpact=.249f;
float sign(float x){return x<0?-1.f:1.f;}
}
std::array<Vec2,4> BossProjectile::barrelQuad(float skin)const
{
    // sharedassets48 GO61/BoxCollider103: centered 1.18 x 1.11 trigger.
    const float x=.59f*barrelScale+skin,y=.555f*barrelScale+skin;
    const float angle=barrelAngle*.0174532925199433f,c=std::cos(angle),s=std::sin(angle);
    std::array<Vec2,4> quad{{{-x,-y},{x,-y},{x,y},{-x,y}}};
    for(auto& p:quad)p={position.x+c*p.x-s*p.y,position.y+s*p.x+c*p.y};
    return quad;
}
void CrossroadsBoss::reset(){*this=CrossroadsBoss{};}
bool CrossroadsBoss::loadEffects(const char* path)
{
    effectsReady_=false;
    auto* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4];bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"FKF4",4)&&
        std::fread(effectConfigs_.data(),sizeof(effectConfigs_),1,f)==1&&std::fgetc(f)==EOF;
    std::fclose(f);
    for(const auto& c:effectConfigs_)for(float v:c)ok=ok&&std::isfinite(v);
    for(const auto& c:effectConfigs_)ok=ok&&c[0]>0&&c[0]<=8&&c[1]>=0&&c[1]<=2000&&
        c[2]>0&&c[3]>=c[2]&&c[3]<=10&&c[6]>=0&&c[7]>=c[6]&&
        c[43]>=0&&c[43]<=2&&std::floor(c[43])==c[43]&&
        (c[44]==0||c[44]==1)&&c[45]>0&&c[46]>=0;
    effectImpact_=impactSerial;effectPhase_=phaseSerial;
    if(ok)effectParticles_.resize(effectCapacity);
    return effectsReady_=ok;
}
namespace {
float effectSample(const std::array<float,49>& c,unsigned offset,float age)
{
    const float t=std::clamp(age,0.f,1.f)*7;
    const unsigned index=std::min(6u,unsigned(t));
    return c[offset+index]+(c[offset+index+1]-c[offset+index])*(t-index);
}
}
float CrossroadsBoss::effectSize(const BossEffectParticle& p)const
{return p.size*effectSample(effectConfigs_[p.emitter],24,p.age/p.life);}
float CrossroadsBoss::effectOpacity(const BossEffectParticle& p)const
{return std::clamp(effectSample(effectConfigs_[p.emitter],32,p.age/p.life),0.f,1.f);}
BossEffectBillboard CrossroadsBoss::effectBillboard(const BossEffectParticle& p)const
{
    const auto& c=effectConfigs_[p.emitter];
    const float size=effectSize(p);
    BossEffectBillboard draw{{},{size,size},p.angle};
    if(c[44]==1){
        const float speed=std::hypot(p.velocity.x,p.velocity.y);
        // approximation: camera-facing XY stretched billboard, using source
        // length/speed scales; the original also supports camera motion.
        draw.scale.y=size*c[45]+speed*c[46];
        if(speed>0)draw.rotation=std::atan2(p.velocity.y,p.velocity.x)-1.57079632679f;
    }
    const float x=c[47]*draw.scale.x,y=c[48]*draw.scale.y;
    if(x==0&&y==0)return draw;
    const float cosine=std::cos(draw.rotation),sine=std::sin(draw.rotation);
    draw.offset={-x*cosine+y*sine,-x*sine-y*cosine};
    return draw;
}
void CrossroadsBoss::emitEffect(unsigned emitter,Vec2 origin,unsigned count,float speedLo,float speedHi,float angleLo,float angleHi)
{
    const auto& c=effectConfigs_[emitter];
    const auto random=[&](float lo,float hi){effectRandom_^=effectRandom_<<13;effectRandom_^=effectRandom_>>17;effectRandom_^=effectRandom_<<5;
        return lo+(hi-lo)*float(effectRandom_&0xffff)/65536.f;};
    for(unsigned i=0;i<count;++i){
        auto slot=std::find_if(effectParticles_.begin(),effectParticles_.end(),[](const auto& p){return p.life<=0;});
        if(slot==effectParticles_.end())break;
        auto& p=*slot;p={};p.emitter=emitter;
        const float angle=random(angleLo,angleHi)*.0174532925f,radius=c[11]*std::sqrt(random(0,1));
        const bool rubble=emitter==0,box=c[20]==5,edge=c[20]==12;
        const bool detached=emitter>=5;
        const float parentX=rubble||detached?1.f:scaleX,parentY=rubble||detached?1.f:std::abs(scaleX);
        const float localX=box?random(-.5f,.5f):edge?random(-c[11],c[11]):std::cos(angle)*radius;
        const float localY=box?random(-.5f,.5f):edge?0.f:std::sin(angle)*radius;
        // approximation: the scene-owned rubble box is anchored to the arena
        // ceiling; boss children retain their authored parent-space offsets.
        p.position={origin.x+((rubble?0.f:c[16])+localX*c[22])*parentX,
                    origin.y+((rubble?0.f:c[17])+localY*c[23])*parentY};
        const float speed=speedLo>=0?random(speedLo,speedHi):random(c[4],c[5]);
        const float projectedY=std::abs(c[19])>.00001f?c[23]/c[19]:1.f;
        p.velocity={(box||edge?0.f:std::cos(angle)*speed)+random(c[12],c[13]),
                    (box?-speed:edge?speed:std::sin(angle)*speed*projectedY)+random(c[14],c[15])};
        // ParticleSystemScalingMode.Shape scales emission positions only;
        // Local scales particle size by the emitter, Hierarchy by both levels.
        const float sizeScale=c[43]==2?1.f:std::abs(c[18])*(c[43]==0?parentY:1.f);
        p.life=random(c[2],c[3]);p.size=random(c[6],c[7])*sizeScale;p.angle=random(c[8],c[9]);
        if(emitter==5){
            // GlobalPrefabDefaults.SpawnBlood applies its 1.2 speed multiplier.
            p.velocity.x=std::cos(angle)*speed*1.2f;
            p.velocity.y=std::sin(angle)*speed*1.2f+random(c[14],c[15]);
        }
        p.accelerationY=random(c[40],c[41]);
        p.presentation.reset(p.position);
    }
}
void CrossroadsBoss::updateEffects(float dt)
{
    if(!effectsReady_||!std::isfinite(dt)||dt<=0||dt>.1f)return;
    std::array<Vec2,16> splashes{};unsigned splashCount=0;
    for(auto& p:effectParticles_)if(p.life>0){
        PresentationPosition::Step step(p.presentation,p.position);
        p.age+=dt;if(p.age>=p.life){p.life=0;continue;}
        const auto& config=effectConfigs_[p.emitter];
        p.velocity.y+=(p.accelerationY-9.81f*config[10])*dt;
        const float speed=std::sqrt(p.velocity.x*p.velocity.x+p.velocity.y*p.velocity.y);
        if(config[42]>0&&speed>config[42]){p.velocity.x*=config[42]/speed;p.velocity.y*=config[42]/speed;}
        const Vec2 delta{p.velocity.x*dt,p.velocity.y*dt};
        const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);float distance=0;
        // Source blood dies on Terrain and emits its color-inheriting splash.
        // Rays use the room's exported contours, including the collapsed floor.
        if(p.emitter==5&&length>0&&rayTerrain(p.position,{delta.x/length,delta.y/length},length+.03f,terrainWorld_,distance)){
            if(splashCount<splashes.size())splashes[splashCount++]={p.position.x+delta.x/length*distance,p.position.y+delta.y/length*distance};
            p.life=0;continue;
        }
        p.position.x+=delta.x;p.position.y+=delta.y;
    }
    for(unsigned i=0;i<splashCount;++i)emitEffect(6,splashes[i],30);
    const auto start=[&](unsigned i,Vec2 p){effectOrigins_[i]=p;effectClocks_[i]=effectConfigs_[i][0];effectRemainders_[i]=0;};
    if(introLanded_&&!effectIntroLanded_){effectIntroLanded_=true;start(3,position);}
    if(collapseLanded_&&!effectCollapseLanded_){effectCollapseLanded_=true;start(3,position);}
    // approximation: source emitter instances are exact; FSM frame attachment
    // follows the port's existing impact/stagger/death milestones.
    if(effectImpact_!=impactSerial){effectImpact_=impactSerial;start(2,position);}
    if(effectPhase_!=phaseSerial){
        effectPhase_=phaseSerial;
        // Source Stun Open/Stun Opened has no active Armour Smash or Dust
        // Hit Plume. These belong to impacts, not the exposed-head animation.
        if(phase==BossPhase::Collapse){start(1,position);start(2,position);start(0,{(arena_.left+arena_.right)*.5f,arena_.top});}
        if(phase==BossPhase::Dying&&corpsePresent_){
            // FalseyControl Death Anim Start: 50 blood, speed5..35,
            // arc20..160, offset(0,-4); global amount multiplier1.3.
            deathEffectAge_=0;steamRemainder_=0;
            emitEffect(5,{position.x,position.y-4},65,5,35,20,160);
            start(7,{position.x,position.y-4});
        }
    }
    if(deathEffectAge_>=0){
        const float before=deathEffectAge_;deathEffectAge_+=dt;
        // Death Anim Start waits1s; Steam's SpawnBloodTime runs for3s
        // at .1s intervals, 12..15 blood with global amount multiplier1.3.
        const float active=std::max(0.f,std::min(deathEffectAge_,4.f)-std::max(before,1.f));
        steamRemainder_+=active;
        while(steamRemainder_>=.1f){steamRemainder_-=.1f;
            effectRandom_^=effectRandom_<<13;effectRandom_^=effectRandom_>>17;effectRandom_^=effectRandom_<<5;
            emitEffect(5,{position.x,position.y-4},16+effectRandom_%5,24,30,10,170);
        }
    }
    for(unsigned i=0;i<effectClocks_.size();++i)if(effectClocks_[i]>0){
        const float step=std::min(dt,effectClocks_[i]);effectClocks_[i]-=step;
        effectRemainders_[i]+=step*effectConfigs_[i][1];const auto n=unsigned(effectRemainders_[i]);
        effectRemainders_[i]-=n;emitEffect(i,effectOrigins_[i],n);
    }
}
bool CrossroadsBoss::bind(CrossroadsBossKind type,Rect arena,CombatObject* objects,unsigned count,
                         const AnimationBank* bank,std::uint32_t flags,bool /*cityCrestSpent*/)
{
    if(type==CrossroadsBossKind::None||!objects||count<objectCapacity||
       !std::isfinite(arena.left)||!std::isfinite(arena.bottom)||!std::isfinite(arena.right)||
       !std::isfinite(arena.top)||arena.right-arena.left<12||arena.top-arena.bottom<7)return false;
    reset();kind=type;arena_=arena;objects_=objects;bank_=bank;savedFlags_=flags&TutorialSave::bossFlags;
    for(unsigned i=0;i<receivers_.size();++i){
        receivers_[i]={this,i};objects_[i]={};objects_[i].colliderEnabled=false;
        objects_[i].name=i?"Gruzzer brood":"Crossroads boss";objects_[i].hasHealth=true;
        objects_[i].receiver=&receivers_[i];objects_[i].onHit=receive;
        if(i>=projectileObjectBegin){objects_[i].name="False Knight falling barrel";
            objects_[i].health.hp=1;objects_[i].health.enemyType=6;objects_[i].health.specialDeath=true;}
    }
    const auto flag=kind==CrossroadsBossKind::DungDefender?TutorialSave::dungDefenderFlag:
        kind==CrossroadsBossKind::GruzMother?TutorialSave::gruzMotherFlag:TutorialSave::falseKnightFlag;
    if(savedFlags_&flag){
        phase=BossPhase::Defeated;
        // level46-8290 Give/Idle exposes reward children on falseKnightDefeated.
        // Reloading defeat must not collect level46-8338's inspectable City Key.
        return true;
    }
    position={(arena.left+arena.right)*.5f+3,arena.bottom+(kind==CrossroadsBossKind::GruzMother?2.f:kind==CrossroadsBossKind::DungDefender?1.8f:-knightBody.bottom)};
    presentation_.reset(position);health(kind==CrossroadsBossKind::DungDefender?700:kind==CrossroadsBossKind::GruzMother?90:65);
    enter(BossPhase::Dormant,0);sync();return true;
}
void CrossroadsBoss::health(int hp)
{
    auto& h=objects_[0].health;h={};h.hp=hp;
    // The armour grants no SOUL. The exposed maggot and Gruz Mother do.
    h.enemyType=kind==CrossroadsBossKind::FalseKnight?(phase==BossPhase::Stagger?5:6):0;
    h.specialDeath=kind==CrossroadsBossKind::FalseKnight;
}
void CrossroadsBoss::play(const char* clip)
{
    if(!bank_)return;
    char name[48];std::snprintf(name,sizeof(name),"%s/%s",kind==CrossroadsBossKind::DungDefender?"Dung":kind==CrossroadsBossKind::GruzMother?"Gruz":"Knight",clip);
    const bool loop=kind==CrossroadsBossKind::DungDefender||phase==BossPhase::Idle||phase==BossPhase::Dormant||phase==BossPhase::Rage;
    animation.play(bank_->require(name),0,loop);
}
void CrossroadsBoss::enter(BossPhase next,float duration)
{
    const auto previous=phase;
    phase=next;timer_=duration;++phaseSerial;
    if(next==BossPhase::Slam)slamImpact_=false;
    if(next==BossPhase::JumpHit)jumpHitterAge_=0;
    // JA Right/Left latch +/-3 before takeoff; recoil actions set X once.
    if(next==BossPhase::JumpRecover)velocity.x=-sign(scaleX)*3;
    if(next==BossPhase::JumpRecover2)velocity.x*=.5f;
    if(kind==CrossroadsBossKind::FalseKnight&&next==BossPhase::Dying){
        timer_=6.f;
        velocity={};
        deathHeadDetached_=deathHeadLanded_=false;
        savedFlags_|=TutorialSave::falseKnightFlag;corpsePresent_=true;
        if(bank_)headAnimation.play(bank_->require("Head/Head Spaz"),0,true);
    }
    const char* clip=kind==CrossroadsBossKind::GruzMother&&next!=BossPhase::Dormant?"Fly":"Idle";
    switch(next){
    case BossPhase::Intro:clip=kind==CrossroadsBossKind::GruzMother?"Wake":"Idle";break;
    case BossPhase::Aim:
        clip=kind==CrossroadsBossKind::GruzMother?"Fly":
            slamAttack_&&!smashReposition_?"Attack Antic":"Jump Antic";break;
    case BossPhase::Charge:clip="Charge";break;
    case BossPhase::Bounce:clip="Slam";break;
    case BossPhase::Jump:clip="Jump";break;
    // FalseyControl plays Blank on the separate Hitter and Jump Attack Hit 1
    // on the armour. This player owns the armour; hiding it here drops the boss.
    case BossPhase::JumpHit:clip=bank_&&bank_->find("Knight/Jump Attack Hit 1")?"Jump Attack Hit 1":"Fall";break;
    case BossPhase::Land:clip="Land";break;
    case BossPhase::Swing:case BossPhase::Slam:clip="Slam";break;
    case BossPhase::JumpRecover:clip="Jump Attack Hit 2";break;
    case BossPhase::JumpRecover2:case BossPhase::JumpEnd:clip="Jump Attack Hit 3";break;
    case BossPhase::Stagger:clip="Stagger";break;
    case BossPhase::Recover:clip="Recover";break;
    case BossPhase::StunJump:case BossPhase::StunLand:
        clip=bank_&&bank_->find("Knight/Stun Roll")?"Stun Roll":"Idle";break;
    case BossPhase::StunPause:
        clip=bank_&&bank_->find("Knight/Stun Roll End")?"Stun Roll End":"Idle";break;
    case BossPhase::StunOpen:clip="Stagger";break;
    case BossPhase::RageJumpAntic:
        clip=bank_&&bank_->find("Knight/Jump Antic")?"Jump Antic":"Idle";break;
    case BossPhase::RageJump:clip="Jump";break;
    case BossPhase::RageLand:clip="Land";break;
    case BossPhase::RageAntic:
        clip=bank_&&bank_->find("Knight/Attack Antic")?"Attack Antic":"Idle";break;
    case BossPhase::RageBegin:
    case BossPhase::Rage:clip="Rage";break;
    case BossPhase::Dying:case BossPhase::Collapse:clip="Death";break;
    case BossPhase::Brood:animation={};return;
    default:break;
    }
    if(kind==CrossroadsBossKind::DungDefender){
        switch(next){
        case BossPhase::DolphinAntic:clip="DolphinAntic";break;
        case BossPhase::Dolphin:clip="Dolphin";break;
        case BossPhase::DolphinRecover:clip="DolphinRecover";break;
        case BossPhase::DungEvade:clip="Evade";break;
        case BossPhase::DungEvadeBall:clip="EvadeBall";break;
        case BossPhase::DungRageIn:clip="DolphinRecover";break;
        case BossPhase::DungRoar:clip="Intro";break;
        case BossPhase::StunAir:clip="StunAir";break;
        case BossPhase::StunLand:clip="Stun";break;
        case BossPhase::StunRecover:clip="StunRecover";break;
        case BossPhase::Intro:clip="Intro";break;
        case BossPhase::Aim:clip="Aim";break;
        case BossPhase::Charge:clip="Dig";break;
        case BossPhase::Stagger:clip="Dig";break;
        case BossPhase::Jump:clip="Erupt";break;
        case BossPhase::Bounce:clip="Roll";break;
        case BossPhase::Recover:clip="Recover";break;
        case BossPhase::Rage:clip="Rage";break;
        case BossPhase::Dying:clip="Death";break;
        default:clip="Idle";break;
        }
    }
    if(kind==CrossroadsBossKind::DungDefender&&next==BossPhase::Intro)clip="Dig";
    if(kind==CrossroadsBossKind::FalseKnight&&corpsePresent_&&
       (next==BossPhase::Dying||next==BossPhase::Defeated))clip=next==BossPhase::Dying?"Death Spaz":"Body";
    const bool landing=kind==CrossroadsBossKind::FalseKnight&&previous==BossPhase::Intro&&next==BossPhase::Idle&&
        bank_&&animation.clip==bank_->find("Knight/Land")&&!animation.complete;
    // The source Hitter keeps playing Attack across S Attack -> Slam.
    if(!landing&&!(previous==BossPhase::Swing&&next==BossPhase::Slam)&&
       !(previous==BossPhase::JumpHit&&next==BossPhase::Slam)&&
       !(previous==BossPhase::JumpRecover2&&next==BossPhase::JumpEnd))play(clip);
    if(next==BossPhase::Stagger&&bank_)headAnimation.play(bank_->require("Head/Head Idle"),0,true);
}
Rect CrossroadsBoss::bounds()const
{
    // level355 BoxCollider57, including the authored 1.1 transform scale.
    if(kind==CrossroadsBossKind::DungDefender)return boxBounds(position,{
        -1.5f*bodyScale_,-2.78125f*bodyScale_,1.203125f*bodyScale_,.875f*bodyScale_});
    if(kind==CrossroadsBossKind::FalseKnight&&phase==BossPhase::Stagger)return headBounds();
    if(kind==CrossroadsBossKind::GruzMother)return boxBounds(position,gruzBody);
    const float offset=.0546875f*scaleX,half=1.6796875f*bodyScale_;
    return boxBounds(position,{offset-half,knightBody.bottom*bodyScale_,offset+half,knightBody.top*bodyScale_});
}
Rect CrossroadsBoss::headBounds()const{return boxBounds(headPosition(position),{-.8f*bodyScale_,-1.075f*bodyScale_,.8f*bodyScale_,.635f*bodyScale_});}
Rect CrossroadsBoss::weaponBounds()const
{
    if(kind!=CrossroadsBossKind::FalseKnight)return {};
    unsigned frame=0;bool rage=false,jump=false;
    if(phase==BossPhase::Swing){
        frame=unsigned(std::max(0.f,knightAttackImpact-timer_)*15);
    }
    else if(phase==BossPhase::JumpHit||(phase==BossPhase::Slam&&jumpAttack_)){
        jump=true;
        // A completed Once clip keeps its last sprite/collider until JA Recoil
        // explicitly deactivates Hitter; it does not become the armour collider.
        frame=std::min(1u,unsigned(std::max(0.f,jumpHitterAge_)*12));
    }
    else if(phase==BossPhase::Slam){
        frame=unsigned((knightAttackImpact+std::max(0.f,knightAttackTail-timer_))*15);
    }
    else if(phase==BossPhase::RageBegin){rage=true;frame=0;}
    else if(phase==BossPhase::Rage&&rageAge_<8*knightRageCycle){
        rage=true;frame=unsigned(rageAge_*12)%5;
    }else return {};
    Vec2 center{},half{};
    // sharedassets47:9 Hitter sprite collider vertices (level48 GO37).
    if(jump){
        if(frame==0){center={-2.1796875f,2.6640625f};half={.7109375f,1.6640625f};}
        else if(frame==1){center={3.6328125f,-.5390625f};half={1.6484375f,3.7578125f};}
        else return bounds();
    }else if(frame==0){center={-.6328125f,.0234375f};half={.8515625f,4.3203125f};}
    else if(frame==1){center={3.625f,-.078125f};half={2.84375f,1.296875f};}
    else if(frame==2){center={3.2265625f,-3.0234375f};half={2.9609375f,1.2734375f};}
    else if(rage){center={.0546875f,-2.359375f};half={1.6796875f,1.9375f};}
    else return {};
    center.x*=scaleX;center.y*=bodyScale_;half.x*=bodyScale_;half.y*=bodyScale_;
    return boxBounds({position.x+center.x,position.y+center.y},{-half.x,-half.y,half.x,half.y});
}
void CrossroadsBoss::sync()
{
    if(!objects_)return;
    objects_[0].bounds=bounds();
    objects_[0].health.invincible=kind==CrossroadsBossKind::FalseKnight&&
        // SetInvincible(false) runs on entering Idle Pause, concurrently with
        // its Wait; successful Recover still inherits Stun Start's guard.
        (breakingFloor_||stunPreparation()||(phase==BossPhase::Recover&&recoverToRage_));
    objects_[0].colliderEnabled=visible()&&!breakingFloor_&&phase!=BossPhase::Dying&&phase!=BossPhase::Brood&&phase!=BossPhase::Collapse;
    for(unsigned i=0;i<minions.size();++i){
        if(kind==CrossroadsBossKind::DungDefender){const auto& p=projectiles[i];objects_[i+1].bounds=boxBounds(p.position,{-.6f,-.6f,.6f,.6f});objects_[i+1].colliderEnabled=p.life>0&&!p.burst;}
        else {objects_[i+1].bounds=boxBounds(minions[i].position,broodBody);objects_[i+1].colliderEnabled=minions[i].alive;}
    }
    for(unsigned i=0;i<projectiles.size();++i){auto& o=objects_[projectileObjectBegin+i];auto& p=projectiles[i];
        o.colliderEnabled=kind==CrossroadsBossKind::FalseKnight&&p.life>0&&!p.ground;
        if(!o.colliderEnabled)continue;
        p.barrelHitQuad=p.barrelQuad();
        const auto first=p.barrelHitQuad[0];o.bounds={first.x,first.y,first.x,first.y};
        for(const auto v:p.barrelHitQuad){o.bounds.left=std::min(o.bounds.left,v.x);o.bounds.right=std::max(o.bounds.right,v.x);
            o.bounds.bottom=std::min(o.bounds.bottom,v.y);o.bounds.top=std::max(o.bounds.top,v.y);}
        o.hitPolygon=&p.barrelHitQuad;}
}
TerrainShape CrossroadsBoss::terrainBlock()const
{
    TerrainShape block;block.id=158;block.count=4;block.flags=8|TerrainShape::kinematic;
    block.enabled=kind==CrossroadsBossKind::FalseKnight&&
        (phase==BossPhase::StunPause||phase==BossPhase::StunOpen||phase==BossPhase::Stagger);
    // level48 BoxCollider158 under boss Transform93; NonThunker296 is active.
    const float x=position.x+.37306494f*scaleX,y=position.y-2.83f*bodyScale_;
    const float hx=1.2872365f*bodyScale_,hy=1.0257735f*bodyScale_;
    block.bounds={x-hx,y-hy,x+hx,y+hy};block.queryBoundsValid=true;
    block.material={.2f,0}; // sharedassets6 PhysicsMaterial2D68, Terrain.
    return block;
}
World CrossroadsBoss::arenaWorld()const
{
    const float l=arena_.left,r=arena_.right,b=arena_.bottom,t=arena_.top,mid=(l+r)*.5f;
    solids_[0]={{l-2,b-30,l,t+10}};solids_[1]={{r,b-30,r+2,t+10}};
    if(!locked()){solids_[0].bounds.top=b-29;solids_[1].bounds.top=b-29;}
    solids_[2]={{l,b-2,mid-3,b}};solids_[3]={{mid+3,b-2,r,b}};
    solids_[4]=floorCollapsed()?Solid{{l,b-13,r,b-11}}:Solid{{mid-3,b-2,mid+3,b}};
    solids_[5]={{l,t,r,t+2}};
    const auto block=terrainBlock();solids_[6]={block.bounds};
    return {solids_.data(),6u+unsigned(block.enabled)};
}
void CrossroadsBoss::receive(void* context,const NailHit& hit,HitOutcome outcome)
{
    const auto& receiver=*static_cast<Receiver*>(context);auto& boss=*receiver.boss;
    if(receiver.index>=projectileObjectBegin){
        auto& p=boss.projectiles[receiver.index-projectileObjectBegin];
        if(p.life<=0||p.barrelBreakUpdates||hit.type!=AttackType::Nail||outcome==HitOutcome::Ignored)return;
        p.deflected=true;p.velocity={};
        switch(hit.direction){case Strike::Right:p.velocity.x=45;break;case Strike::Left:p.velocity.x=-45;break;
        case Strike::Up:p.velocity.y=45;break;case Strike::Down:p.velocity.y=-45;break;}
        auto& h=boss.objects_[receiver.index].health;h.hp=1;h.dead=false;h.evasion=0;
        return;
    }
    boss.hit(receiver.index,outcome);
}
void CrossroadsBoss::hit(unsigned index,HitOutcome outcome)
{
    if(outcome!=HitOutcome::Damaged&&outcome!=HitOutcome::Killed&&outcome!=HitOutcome::SpecialDeath)return;
    if(index){
        if(kind==CrossroadsBossKind::DungDefender){auto& p=projectiles[index-1];p.burst=true;p.life=.2f;
            if(bank_)p.animation.play(bank_->require("Dung/BallBurst"),0,true);
        }else if(minions[index-1].alive&&objects_[index].health.hp<=0)minions[index-1].alive=false;
        sync();return;
    }
    if(!targetable()||phase==BossPhase::Dying||phase==BossPhase::Brood||phase==BossPhase::Collapse)return;
    ++hitSerial;hitFlash=.12f;
    if(kind==CrossroadsBossKind::DungDefender){
        if(objects_[0].health.hp<=0){velocity={};enter(BossPhase::Dying,2);}
        else if(phase==BossPhase::StunLand)enter(BossPhase::StunRecover,dungClipTime("StunRecover",.5f));
        else if(phase!=BossPhase::StunAir&&phase!=BossPhase::StunRecover&&
                phase!=BossPhase::Rage&&phase!=BossPhase::DungRageIn&&dungRages_==0&&
                !(phase==BossPhase::Jump&&velocity.y<0)){
            // Original Stun FSM: 8 hits less than 1 s apart, or 15 total; at most 3 stuns.
            ++dungHits_;++dungCombo_;dungComboTime_=1;
            if(staggerCount<3&&(dungHits_>=15||dungCombo_>=8)){
                ++staggerCount;dungHits_=dungCombo_=0;
                velocity={scaleX>0?-10.f:10.f,35};
                enter(BossPhase::StunAir,0);
            }
        }
        sync();return;
    }
    if(kind==CrossroadsBossKind::FalseKnight&&phase==BossPhase::Stagger){
        timer_=5.25f;
        headHp_=std::max(0,objects_[0].health.hp);
        if(bank_)headAnimation.play(bank_->require("Head/Head Hit"),0,false);
    }
    if(phase==BossPhase::Dormant)enter(BossPhase::Intro,.8f);
    if(objects_[0].health.hp>0)return;
    if(kind==CrossroadsBossKind::FalseKnight&&phase==BossPhase::Stagger&&effectsReady_){
        // Head Health Check/State 1 emits before sending STUN END to the body.
        emitEffect(5,headPosition(position),65,5,35,20,160);
    }
    velocity={};
    if(kind==CrossroadsBossKind::GruzMother){
        gruzGeoOrigin_=position;gruzGeoPending_=true;
        velocity={heroX_<position.x?8.f:-8.f,6.f};
        enter(BossPhase::Dying,2);
    }
    else if(phase!=BossPhase::Stagger){
        ++staggerCount;
        // Check Direction faces the hero, then Stun Start launches away.
        // Do not expose the head or erase an airborne knockdown's height.
        scaleX=heroX_<position.x?-bodyScale_:bodyScale_;
        velocity={-sign(scaleX)*10,20};
        enter(BossPhase::StunJump,0);health(65);
    }else if(finalExposure_)enter(BossPhase::Dying,1);
    else {headHp_=40;++completedStaggers_;recoverToRage_=true;enter(BossPhase::Recover,6.f/12);health(65);enraged_=true;}
    sync();
}
void CrossroadsBoss::launchWave()
{
    // FalseyControl S Attack Recover spawns one Shockwave and assigns its
    // Facing Right from S Right/S Left. The opposite side stays clear.
    // SpawnObjectFromGlobalPool adds +/-5.5 to Self's world X, without scale.
    for(auto& p:projectiles)if(p.reusable()){
        p.resetFlight();p.position={position.x+sign(scaleX)*5.5f,position.y-5.8f};p.life=3;p.ground=true;
        // sharedassets48 FSM150: Pause waits for Update; Start Move stores
        // Incrementer=Speed*2, then Speed*=.025 (FalseyControl supplies 22).
        p.velocity={sign(scaleX)*.55f,0};p.accelerationX=sign(scaleX)*44;
        p.awaitingWaveUpdate=true;
        p.presentation.reset(p.position);break;
    }
}
void CrossroadsBoss::launchRock(float x)
{
    // Summoner Transform64 at Y9.09 under root66 at Y31.89; GetPosition
    // reads world Y. Authored Crossroads_10 spawning is independent of arena.
    for(auto& p:projectiles)if(p.reusable()){
        p.resetFlight();
        // RandomScale.Start runs once; scaleOnEnable is false on pooled GO61.
        if(!p.barrelScaled){p.barrelScale=.8f+.2f*barrelRandomUnit();p.barrelScaled=true;}
        p.barrelSpin=barrelRandomUnit()<.5f?-720.f:720.f;
        p.position={x,authoredActivation_?40.98f:arena_.top-1};p.life=1;p.presentation.reset(p.position);break;
    }
}
void CrossroadsBoss::brood()
{
    enter(BossPhase::Brood,0);
    for(unsigned i=0;i<minions.size();++i){auto& m=minions[i];m.alive=true;m.position=position;
        m.velocity={(i&1?1.f:-1.f)*(3+float(i/2)),3+float(i%3)};m.presentation.reset(m.position);
        if(bank_)m.animation.play(bank_->require("Gruzzer/Fly"));
        auto& h=objects_[i+1].health;h={};h.hp=8;
    }
    sync();
}
void CrossroadsBoss::defeat()
{
    if(phase==BossPhase::Defeated)return;
    if(kind==CrossroadsBossKind::DungDefender)savedFlags_|=TutorialSave::dungDefenderFlag;
    else if(kind==CrossroadsBossKind::GruzMother)savedFlags_|=TutorialSave::gruzMotherFlag;
    // Give/Activate enables the reward hierarchy; Shiny Control/City Key owns
    // hasCityKey. Geo belongs to the separate chest, not the defeat callback.
    else savedFlags_|=TutorialSave::falseKnightFlag|TutorialSave::falseKnightFloorFlag;
    enter(BossPhase::Defeated,0);for(auto& p:projectiles)p={};sync();
}
void CrossroadsBoss::update(float dt)
{
    updateEffects(dt);
    if(!active()||!std::isfinite(dt)||dt<=0)return;
    animation.advance(dt);headAnimation.advance(dt);
    // Move's FloatAdd runs on entry and each Update with perSecond=true,
    // before SetVelocity2d. Physics consumes the resulting velocity.
    bool barrelBroke=false,barrelRotated=false;
    for(auto& p:projectiles){
        p.barrelRecycleWait=std::max(0.f,p.barrelRecycleWait-dt);
        if(p.barrelBreakUpdates&&!--p.barrelBreakUpdates){
            // FSM147 Pause's NextFrameEvent keeps motion/collider through
            // the contact frame. Break freezes/hides, then waits3 to recycle.
            p.life=0;p.velocity={};p.barrelAngle=0;p.barrelRecycleWait=3;barrelBroke=true;
        }
        // FSM147 Idle Rotate: world Z, perSecond/everyFrame, Update only.
        // Pause/Break do not execute Idle's Rotate action.
        if(p.life>0&&!p.ground&&!p.barrelBreakUpdates){
            p.barrelAngle=std::fmod(p.barrelAngle+p.barrelSpin*dt,360.f);barrelRotated=true;
        }
        // Spawned GO67/68 remain at their world position after the controller
        // moves/recycles. Damage timing: Wait .05 -> damage1/Wait .05 -> damage0.
        for(auto& spurt:p.waveSpurts)if(spurt.phase){
            spurt.elapsed+=dt;
            if(spurt.elapsed>=.05f){spurt.elapsed=0;spurt.phase=spurt.phase==1?2:0;}
        }
        if(p.life<=0||!p.ground)continue;
        if(p.waveEnding){
            if(p.waveEndPause>0){
                // WALL -> End Pause keeps spawning for .15s while the
                // Rigidbody retains its last velocity; End Particle does not.
                p.waveEndPause=std::max(0.f,p.waveEndPause-dt);
                if(p.waveEndPause>0)for(auto& spurt:p.waveSpurts)if(!spurt.phase){spurt={p.position,0,1};break;}
            }
            continue;
        }
        const bool moving=!p.awaitingWaveUpdate;
        p.awaitingWaveUpdate=false;p.velocity.x+=p.accelerationX*dt;
        // SpawnObjectFromGlobalPoolOverTimeV2 only runs on Update. Its .005-s
        // frequency expires every 60-Hz control tick and spawns once, not in a loop.
        if(moving)for(auto& spurt:p.waveSpurts)if(!spurt.phase){spurt={p.position,0,1};break;}
        // FSM150 Move: RayCast2d from Self+(+/-1,1), down1.6 on
        // layer8; BoolTest(On Ground=false) sends HIT -> End Particle.
        // End Particle has no spawner/velocity action and waits1 before recycle.
        float distance=0;
        if(terrainKnown_&&!rayTerrain({p.position.x+sign(p.velocity.x),p.position.y+1},{0,-1},1.6f,
                                     terrainWorld_,distance,TerrainShape::terrainQueryExcluded)){
            p.waveEnding=true;p.life=1;
        }
    }
    if(barrelBroke||barrelRotated)sync();
    if(kind==CrossroadsBossKind::FalseKnight&&phase==BossPhase::Stagger&&animation.complete&&bank_){
        if(const auto* opened=bank_->require("Knight/Opened"))animation.play(opened);
    }
    if(kind==CrossroadsBossKind::FalseKnight&&introLanded_&&(phase==BossPhase::Intro||phase==BossPhase::Idle)&&
       animation.complete&&bank_&&animation.clip==bank_->find("Knight/Land"))play("Idle");
    hitFlash=std::max(0.f,hitFlash-dt);
    for(auto& m:minions)if(m.alive)m.animation.advance(dt);
    for(auto& p:projectiles)if(p.life>0)p.animation.advance(dt);
}
void CrossroadsBoss::throwDung(float direction)
{
    // level355 Dung Defender / RJ Launch: X = Throw Speed Crt, Y = 35.
    for(unsigned i=0;i<minionCapacity;++i)if(projectiles[i].life<=0){
        auto& p=projectiles[i];p={};p.position={position.x+direction*1.5f,position.y+1};
        p.velocity={direction*dungThrowSpeed_,35};p.life=12;p.presentation.reset(p.position);
        if(bank_)p.animation.play(bank_->require("Dung/Ball"),0,true);
        auto& h=objects_[i+1].health;h={};h.hp=1;h.enemyType=3;
        break;
    }
}
float CrossroadsBoss::dungClipTime(const char* name,float fallback)const
{
    if(!bank_)return fallback;
    char key[48];std::snprintf(key,sizeof(key),"Dung/%s",name);
    const auto* clip=bank_->find(key);
    return clip&&clip->fps>0?float(clip->count)/clip->fps:fallback;
}
unsigned CrossroadsBoss::dungRandom(unsigned bound)
{
    dungRandomState_^=dungRandomState_<<13;dungRandomState_^=dungRandomState_>>17;
    dungRandomState_^=dungRandomState_<<5;
    return bound?dungRandomState_%bound:0;
}
unsigned CrossroadsBoss::chooseDungMove()
{
    // Move Choice: five equal weights, max one consecutive choice, max nine misses.
    std::array<unsigned,5> eligible{};unsigned count=0;
    for(unsigned i=0;i<5;++i)if(dungMissed_[i]>=9)eligible[count++]=i;
    if(!count)for(unsigned i=0;i<5;++i)if(int(i)!=dungLastMove_)eligible[count++]=i;
    const auto choice=eligible[dungRandom(count)];
    for(unsigned i=0;i<5;++i)dungMissed_[i]=i==choice?0:std::min(9,int(dungMissed_[i])+1);
    dungLastMove_=int(choice);return choice;
}
void CrossroadsBoss::beginDolphin()
{
    const float direction=position.x<(arena_.left+arena_.right)*.5f?1.f:-1.f;
    scaleX=direction*bodyScale_;velocity={direction*10,0};
    dolphinDives_=2+dungRandom(2);
    enter(BossPhase::DolphinAntic,dungClipTime("DolphinAntic",.3f));
}
void CrossroadsBoss::dungIdle(float delay)
{
    // Will Evade? / Reset Evade: a retreat cannot immediately chain another.
    dungMayEvade_=!dungEvaded_;dungEvaded_=false;enter(BossPhase::Idle,delay);
}
void CrossroadsBoss::fixedDung(float dt,Hero& hero)
{
    // BoxCollider2D bottom is -2.78125 local units; Erupt Out starts
    // buried at source Y=3.97 and rises to the authored absolute peak Y=12.
    const float floor=arena_.bottom+2.78125f*bodyScale_,peak=12.f;
    const float direction=sign(hero.position.x-position.x);
    dungComboTime_=std::max(0.f,dungComboTime_-dt);
    if(dungComboTime_==0)dungCombo_=0;
    if(phase==BossPhase::Dormant){
        const bool activated=authoredActivation_?overlaps(bodyBounds(hero.position),activationRegion_):std::abs(hero.position.x-position.x)<9;
        if(activated){dungIntro_=true;position.y=floor;enter(BossPhase::Intro,.5f);}
    }else if(phase==BossPhase::Intro){
        // Wake: sink, tunnel for four seconds, then the introductory eruption.
        if(timer_<=0){velocity={direction*15,0};enter(BossPhase::Charge,4);}
    }else if(phase==BossPhase::DungRoar){
        if(timer_<=0)enter(BossPhase::DolphinRecover,dungClipTime("DolphinRecover",.5f));
    }else if(phase==BossPhase::Rage){
        if(timer_<=0){
            // Set Rage: one eruption, then five Rage In / eruption repeats.
            dungRages_=5;dungTunnelTime_=1.5f;dungTunnelSpeed_=20;
            velocity={};enter(BossPhase::Stagger,dungClipTime("Dig",4.f/15));
        }
    }else if(phase==BossPhase::DungRageIn){
        position.y-=20*dt;
        if(timer_<=0){velocity={direction*30,0};enter(BossPhase::Charge,.3f);}
    }else if(phase==BossPhase::Idle){
        if(!enraged_&&objects_[0].health.hp<=350){
            enraged_=true;velocity={};enter(BossPhase::Rage,3.75f);sync();return;
        }
        // level355 GO9: Evade Range is a 4.19 x 9.25 trigger at local X 1.36.
        const float x=position.x+direction*1.36f*bodyScale_;
        const Rect range={x-2.095f*bodyScale_,position.y-4.625f*bodyScale_,
                          x+2.095f*bodyScale_,position.y+4.625f*bodyScale_};
        if(dungMayEvade_&&overlaps(range,bodyBounds(hero.position))){
            dungMayEvade_=false;dungEvaded_=true;scaleX=direction*bodyScale_;
            velocity={-direction*25,0};enter(BossPhase::DungEvade,dungClipTime("Evade",.4f));
        }else if(timer_<=0){
        scaleX=direction*bodyScale_;++attacks;dungThrows_=0;dungMove_=chooseDungMove();
        dungThrowLimit_=dungMove_==3?1:2;
        dungThrowSpeed_=dungMove_==1?(dungRandom(2)?18.f:12.f):6.f;
        dungTunnelTime_=dungMove_==2?4.5f:2.5f;dungTunnelSpeed_=15;
        if(dungMove_==0)beginDolphin();
        else if(dungMove_==4){velocity={};enter(BossPhase::Stagger,dungClipTime("Dig",4.f/15));}
        else enter(BossPhase::Aim,2.f/15);
        }
    }else if(phase==BossPhase::Aim&&timer_<=0){
        throwDung(direction);++dungThrows_;play(dungThrows_==1?"Throw":"Throw2");
        enter(BossPhase::Recover,dungThrows_==1?.65f:.4f);
        play(dungThrows_==1?"Throw":"Throw2");
    }else if(phase==BossPhase::Recover&&timer_<=0){
        if(dungThrows_<dungThrowLimit_)enter(BossPhase::Aim,2.f/15);
        else if(dungMove_==3)beginDolphin();
        else if(dungMove_==1){
            bounceCount=0;velocity={direction*dungThrowSpeed_,35};enter(BossPhase::Bounce,0);
        }else {velocity={};enter(BossPhase::Stagger,dungClipTime("Dig",4.f/15));}
    }else if(phase==BossPhase::Stagger&&timer_<=0){
        velocity={direction*dungTunnelSpeed_,0};enter(BossPhase::Charge,dungTunnelTime_);
    }else if(phase==BossPhase::Charge){
        // Tunneling L/R turns at Min/Max X (61.78/90.57 in Waterways_05)
        // and when more than ten units past the hero; its timer keeps running.
        const float left=arena_.left+1.78f,right=arena_.right-1.43f;
        position.x=std::clamp(position.x+velocity.x*dt,left,right);
        if((velocity.x<0&&(position.x<=left||position.x<hero.position.x-10))||
           (velocity.x>0&&(position.x>=right||position.x>hero.position.x+10)))
            velocity.x=-velocity.x;
        if(timer_<=0){velocity={};enter(BossPhase::Slam,dungRages_?.25f:.5f);}
    }else if(phase==BossPhase::Slam&&timer_<=0){
        position.y=3.97f;velocity={0,40};dungFalling_=false;
        enter(BossPhase::Jump,0);++impactSerial;
        // Erupt Out creates four balls, at X velocities -12, -6, 6 and 12.
        const float previousSpeed=dungThrowSpeed_;
        for(float speed:{6.f,12.f}){dungThrowSpeed_=speed;throwDung(-1);throwDung(1);}
        dungThrowSpeed_=previousSpeed;
    }else if(phase==BossPhase::DolphinAntic){
        if(timer_<=0)enter(BossPhase::Dolphin,dungClipTime("Dolphin",.8f));
    }else if(phase==BossPhase::Dolphin){
        position.x+=velocity.x*dt;
        const bool edge=position.x<=arena_.left+5.49f||position.x>=arena_.right-4.82f;
        if(timer_<=0){
            if(edge||--dolphinDives_==0){velocity={};enter(BossPhase::DolphinRecover,dungClipTime("DolphinRecover",.5f));}
            else enter(BossPhase::Dolphin,dungClipTime("Dolphin",.8f));
        }
    }else if(phase==BossPhase::DungEvade||phase==BossPhase::DungEvadeBall){
        position.x=std::clamp(position.x+velocity.x*dt,arena_.left+1.78f,arena_.right-1.43f);
        if(timer_<=0){
            if(phase==BossPhase::DungEvade)enter(BossPhase::DungEvadeBall,.2f);
            else {velocity={};enter(BossPhase::DolphinRecover,dungClipTime("DolphinRecover",.5f));}
        }
    }else if(phase==BossPhase::DolphinRecover){
        if(timer_<=0)dungIdle(1);
    }else if(phase==BossPhase::StunAir){
        velocity.y-=60*dt;position.x=std::clamp(position.x+velocity.x*dt,arena_.left+1.7f,arena_.right-1.7f);
        position.y+=velocity.y*dt;
        if(position.y<=floor&&velocity.y<0){position.y=floor;velocity={};enter(BossPhase::StunLand,3);}
    }else if(phase==BossPhase::StunLand){
        if(timer_<=0)enter(BossPhase::StunRecover,dungClipTime("StunRecover",.5f));
    }else if(phase==BossPhase::StunRecover){
        if(timer_<=0)dungIdle(1);
    }else if(phase==BossPhase::Jump||phase==BossPhase::Bounce){
        // Erupt Out rises kinematically to Y=12; Erupt Fall uses gravityScale=2.
        // Rolling uses the source gravityScale=1 instead.
        if(phase==BossPhase::Bounce)velocity.y-=30*dt;
        else if(dungFalling_)velocity.y-=60*dt;
        position.x+=velocity.x*dt;position.y+=velocity.y*dt;
        if(position.x<arena_.left+1.7f||position.x>arena_.right-1.7f){
            position.x=std::clamp(position.x,arena_.left+1.7f,arena_.right-1.7f);velocity.x=-velocity.x;
        }
        if(phase==BossPhase::Jump&&!dungFalling_&&position.y>=peak){
            position.y=peak;velocity.y=0;dungFalling_=true;play("Fall");
        }
        if(position.y<=floor&&velocity.y<0){
            position.y=floor;++impactSerial;
            if(phase==BossPhase::Bounce&&++bounceCount<5)velocity.y=35;
            else if(phase==BossPhase::Jump&&dungRages_){
                --dungRages_;velocity={0,-20};enter(BossPhase::DungRageIn,.25f);
            }else if(phase==BossPhase::Jump&&dungIntro_){
                dungIntro_=false;velocity={};enter(BossPhase::DungRoar,3.75f);
            }else {velocity={};enter(BossPhase::DolphinRecover,dungClipTime("DolphinRecover",.5f));}
        }
    }else if(phase==BossPhase::Dying&&timer_<=0)defeat();
    if(visible()&&phase!=BossPhase::Dying&&overlaps(bounds(),bodyBounds(hero.position)))
        hero.takeDamage(1,hero.position.x<position.x?-1:1);
    for(auto& p:projectiles)if(p.life>0){
        PresentationPosition::Step step(p.presentation,p.position);p.life-=dt;
        if(p.burst)continue;
        p.velocity.y-=30*dt;p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
        if(p.position.y<=arena_.bottom+.6f&&p.velocity.y<0){
            p.position.y=arena_.bottom+.6f;p.velocity.y=20;
            if(++p.bounces>=5){p.burst=true;p.life=.2f;if(bank_)p.animation.play(bank_->require("Dung/BallBurst"),0,true);}
        }
        if(p.position.x<arena_.left+.6f||p.position.x>arena_.right-.6f){
            p.position.x=std::clamp(p.position.x,arena_.left+.6f,arena_.right-.6f);p.velocity.x=-p.velocity.x;
        }
        if(!p.burst&&overlaps(boxBounds(p.position,{-.6f,-.6f,.6f,.6f}),bodyBounds(hero.position)))
            hero.takeDamage(1,hero.position.x<p.position.x?-1:1);
    }
    sync();
}
void CrossroadsBoss::fixedUpdate(float dt,Hero& hero,const World* terrain)
{
    if(terrain){terrainWorld_=*terrain;terrainKnown_=true;}
    if(!active()||phase==BossPhase::Defeated||!std::isfinite(dt)||dt<=0||dt>.1f)return;
    PresentationPosition::Step step(presentation_,position);
    heroX_=hero.position.x;
    const auto startingPhase=phase;
    timer_-=dt;elapsed_+=dt;
    if(titleAge_>=0)titleAge_+=dt;
    if(kind==CrossroadsBossKind::DungDefender){fixedDung(dt,hero);return;}
    const float bottom=arena_.bottom,top=arena_.top,left=arena_.left,right=arena_.right;
    const float knightFloor=bottom-knightBody.bottom*bodyScale_;
    if(phase==BossPhase::Dormant){
        const bool activated=authoredActivation_?overlaps(bodyBounds(hero.position),activationRegion_):std::abs(hero.position.x-position.x)<9;
        if(activated){
            enter(BossPhase::Intro,1);
            // FalseyControl Music starts the area title before State 1/Land.
            // Approximation: attach that earlier event to encounter activation.
            if(kind==CrossroadsBossKind::FalseKnight)titleAge_=0;
        }
    }
    else if(phase==BossPhase::Intro){
        // The room spawns the armour above the chamber. Land before attacking.
        bool landed=true;
        if(kind==CrossroadsBossKind::FalseKnight&&authoredActivation_){
            velocity.y-=35*dt;position.y+=velocity.y*dt;
            landed=position.y<=knightFloor;
            if(landed){position.y=knightFloor;velocity={};}
        }
        if(landed&&kind==CrossroadsBossKind::FalseKnight&&!introLanded_){introLanded_=true;++impactSerial;play("Land");}
        if(timer_<=0&&landed)enter(BossPhase::Idle,1);
    }
    else if(kind==CrossroadsBossKind::GruzMother){
        if(phase==BossPhase::Idle){
            velocity.y=(bottom+4-position.y)*2;
            if(timer_<=0){velocity={};enter(BossPhase::Aim,.65f);}
        }else if(phase==BossPhase::Aim&&timer_<=0){
            ++attacks;const float dx=hero.position.x-position.x,dy=hero.position.y-position.y;
            const float length=std::max(.1f,std::sqrt(dx*dx+dy*dy));velocity={dx/length*13,dy/length*13};
            scaleX=dx<0?1.f:-1.f;enter(BossPhase::Charge,1.3f);
        }else if(phase==BossPhase::Charge&&(timer_<=0||position.x<=left+1.3f||position.x>=right-1.3f||position.y<=bottom+1.3f)){
            bounceCount=0;velocity={sign(hero.position.x-position.x)*7,-15};enter(BossPhase::Bounce,0);
        }else if(phase==BossPhase::Bounce){
            if(position.y<=bottom+1.26f&&velocity.y<0){velocity.y=17;++bounceCount;++impactSerial;}
            if(position.y>=top-1.26f&&velocity.y>0){velocity.y=-17;++impactSerial;}
            if(bounceCount>=5){velocity={};enter(BossPhase::Idle,1);}
        }else if(phase==BossPhase::Dying){
            if(position.y>bottom+1.25f||velocity.y>0)velocity.y-=25*dt;
            else velocity={};
            if(timer_<=0&&position.y<=bottom+1.25f){velocity={};brood();}
        }else if(phase==BossPhase::Brood){
            bool any=false;for(const auto& m:minions)any|=m.alive;if(!any)defeat();
        }
        position.x=std::clamp(position.x+velocity.x*dt,left+1.25f,right-1.25f);
        position.y=std::clamp(position.y+velocity.y*dt,bottom+1.25f,top-1.25f);
        if(phase==BossPhase::Bounce&&(position.x<=left+1.26f||position.x>=right-1.26f))velocity.x=-velocity.x;
    }else {
        if(phase==BossPhase::Idle&&timer_<=0){
            // FalseyControl Turn L/R set local X to -/+1.3; the scene
            // parent has unit scale. Keep artwork and Hitter on that side.
            ++attacks;scaleX=hero.position.x<position.x?-bodyScale_:bodyScale_;
            // FalseyControl Run uses the serialized Run Speed of -14; the
            // sign is applied by the left/right target selection.
            if(std::abs(hero.position.x-position.x)>21){velocity.x=sign(hero.position.x-position.x)*14;enter(BossPhase::Charge,0);}
            else {
                unsigned choice=0;
                // Move Choice is an equal 1/1/1 random event. The FSM
                // rejects only the authored consecutive-action guards and
                // rerolls through Move Choice; it does not substitute a
                // preferred attack when a guard fires.
                for(unsigned tries=0;tries<8;++tries){
                    choice=unsigned(randomUnit()*3);
                    if(choice==0&&smashCount_>=3)continue;
                    if(choice==1&&jumpAttackCount_>=4)continue;
                    if(choice==2&&(completedStaggers_>=2||normalJumpCount_))continue;
                    break;
                }
                slamAttack_=choice==0;jumpAttack_=choice==1;
                smashCount_=slamAttack_?smashCount_+1:0;jumpAttackCount_=jumpAttack_?jumpAttackCount_+1:0;
                normalJumpCount_=choice==2?normalJumpCount_+1:0;
                smashReposition_=false;
                if(slamAttack_){
                    // FalseyControl S Left/Right -> S Antic: prepare a landing
                    // 12..18 units beyond the hero; at distance >=12 skip it.
                    const float offset=12+6*randomUnit();
                    smashJumpX_=std::clamp(.9f*(hero.position.x+sign(position.x-hero.position.x)*offset-position.x),-12.f,12.f);
                    smashReposition_=std::hypot(hero.position.x-position.x,hero.position.y-position.y)<12;
                }
                velocity={};enter(BossPhase::Aim,slamAttack_&&!smashReposition_?1.2f:.3f);
            }
        }else if(phase==BossPhase::Charge&&std::abs(hero.position.x-position.x)<14){velocity={};enter(BossPhase::Idle,0);}
        else if(phase==BossPhase::Aim&&timer_<=0){
            if(smashReposition_){
                // Walls Check 2: Terrain-only ray; a wall bypasses S Jump.
                const auto jumpWorld=terrain?*terrain:arenaWorld();float distance=0;
                if(rayTerrain(position,{smashJumpX_>0?1.f:-1.f,0},8,jumpWorld,distance,TerrainShape::terrainQueryExcluded)){
                    smashReposition_=false;enter(BossPhase::Aim,1.2f);
                }else {velocity={smashJumpX_,105};enter(BossPhase::Jump,0);}
            }
            else if(slamAttack_)enter(BossPhase::Swing,knightAttackImpact);
            else {
                const float target=std::clamp(hero.position.x,arena_.left+2,arena_.right-2);
                velocity={jumpAttack_?target+(hero.position.x<position.x?3:-3)-position.x:std::clamp(.9f*(target-position.x),-12.f,12.f),90};
                enter(BossPhase::Jump,0);
            }
        }else if(phase==BossPhase::Jump||phase==BossPhase::JumpHit){
            if(phase==BossPhase::Jump)velocity.y*=std::pow(velocity.y>0?.85f:1.15f,dt/.02f);
            else jumpHitterAge_+=dt;
            velocity.y-=60*(breakingFloor_?.2f:smashReposition_?.39f:jumpAttack_?.12f:.125f)*dt;
            if(phase==BossPhase::Jump&&jumpAttack_&&!breakingFloor_&&velocity.y<0){
                const auto jumpWorld=terrain?*terrain:arenaWorld();float distance=0;
                // JA Fall's layer-8 ray emits FALL into JA Hit. Its velocity
                // multiplier action exits here, before the separate LAND event.
                if(rayTerrain(position,{0,-1},9.5f,jumpWorld,distance,TerrainShape::terrainQueryExcluded))
                    enter(BossPhase::JumpHit,0);
            }
            if(position.y<=knightFloor+.01f&&velocity.y<0){
                position.y=knightFloor;velocity={};
                if(breakingFloor_){savedFlags_|=TutorialSave::falseKnightFloorFlag;velocity={0,15};enter(BossPhase::Collapse,0);}
                else if(smashReposition_)enter(BossPhase::Land,knightLandingDuration);
                else if(jumpAttack_)enter(BossPhase::Slam,std::min(knightJumpSlamWait,
                    std::max(0.f,knightJumpHitterDuration-jumpHitterAge_)));
                else enter(BossPhase::Idle,knightLandingDuration+idleDuration());
            }
        }else if(phase==BossPhase::Swing&&timer_<=0){
            // S Attack starts Hitter/Attack after the 1.2-s anticipation;
            // its separate 0.12-s wait precedes the Slam impact event.
            enter(BossPhase::Slam,knightAttackTail);slamImpact_=true;++impactSerial;
        }else if(phase==BossPhase::Land&&timer_<=0){
            smashReposition_=false;enter(BossPhase::Aim,1.2f);
        }else if(phase==BossPhase::Slam){
            if(jumpAttack_)jumpHitterAge_+=dt;
            if(!slamImpact_){
                slamImpact_=true;
                if(!jumpAttack_)++impactSerial;
            }
            if(timer_<=0){
                // Slam watches the Hitter completion before S Attack Recover
                // spawns its wave. Impact remains at S Attack's .12-s wait.
                if(!jumpAttack_)launchWave();
                // Source S Attack Recover chooses and summons barrels only
                // after the Hitter completion event. Keep the port's bounded
                // staggered launcher, but schedule it at that same boundary.
                if(completedStaggers_){
                    rocksPending_=completedStaggers_==1?2+unsigned(randomUnit()*2):jumpAttack_?2:3+unsigned(randomUnit()*2);
                    rockTimer_=.15f+.1f*randomUnit();
                }
                if(jumpAttack_)enter(BossPhase::JumpRecover,knightJumpRecoilClipDuration);
                else enter(BossPhase::Idle,knightAttackRecovery+idleDuration());
            }
        }
        else if(phase==BossPhase::JumpRecover&&timer_<=0)enter(BossPhase::JumpRecover2,knightJumpRecoilClipDuration);
        else if(phase==BossPhase::JumpRecover2&&timer_<=knightJumpRecoilClipDuration-knightJumpRecoilWait){
            // JA End stops movement, but watches the Hit 3 already in flight.
            const float remaining=std::max(0.f,timer_);velocity={};enter(BossPhase::JumpEnd,remaining);
        }
        else if(phase==BossPhase::JumpEnd&&timer_<=0)enter(BossPhase::Idle,idleDuration());
        else if(phase==BossPhase::StunLand&&(timer_<=0||velocity.x < -3)){
            // Source FloatCompare uses signed X Speed, lessThan END and
            // tolerance 3, alongside Wait(.5), reading the solved body velocity.
            velocity={};
            const bool first=!(savedFlags_&TutorialSave::falseKnightFirstPlopFlag);
            savedFlags_|=TutorialSave::falseKnightFirstPlopFlag;
            enter(BossPhase::StunPause,first?2.5f:1.2f);
        }
        else if(phase==BossPhase::StunPause&&timer_<=0)enter(BossPhase::StunOpen,4.f/12);
        else if(phase==BossPhase::StunOpen&&timer_<=0){
            enter(BossPhase::Stagger,5);health(headHp_);
            if(bank_)animation.play(bank_->require("Knight/Opened"));
        }
        else if(phase==BossPhase::Stagger&&timer_<=0&&!finalExposure_){recoverToRage_=false;enter(BossPhase::Recover,.5f);health(65);}
        else if(phase==BossPhase::Recover&&timer_<=0){
            if(!recoverToRage_)enter(BossPhase::Idle,completedStaggers_?.8f+.2f*randomUnit():1.f);
            else enter(BossPhase::RagePause,.5f);
        }
        else if(phase==BossPhase::RagePause&&timer_<=0){
            recoverToRage_=false;enter(BossPhase::RageJumpAntic,3.f/10);
        }
        else if(phase==BossPhase::RageJumpAntic&&timer_<=0){
            // FalseyControl Rage Jump Antic -> Jump 2. The standalone arena
            // fixture uses its centre; Crossroads_10 uses authored world X.
            const float target=authoredActivation_?28.9f:(left+right)*.5f;
            velocity={(target-position.x)*1.05f,105};enter(BossPhase::RageJump,0);
        }
        else if(phase==BossPhase::RageJump){
            velocity.y*=std::pow(velocity.y>0?.85f:1.15f,dt/.02f);
            velocity.y-=60*.3f*dt;
            if(position.y<=knightFloor+.01f&&velocity.y<0){
                position.y=knightFloor;velocity={};enter(BossPhase::RageLand,knightLandingDuration);
            }
        }
        else if(phase==BossPhase::RageLand&&timer_<=0)enter(BossPhase::RageAntic,.7f);
        else if(phase==BossPhase::RageAntic&&timer_<=0)enter(BossPhase::RageBegin,0);
        else if(phase==BossPhase::RageBegin){
            // Source Rage Begin waits NextFrameEvent before restarting Hitter.
            rageAge_=0;rageSlams_=0;rocksPending_=8;
            // Turn sends SUMMON after the first clip. The summoner then
            // waits .15-.25 seconds before its first barrel, as between drops.
            rockTimer_=knightRageCycle+.15f+.1f*randomUnit();
            enter(BossPhase::Rage,0);
        }
        else if(phase==BossPhase::Rage){
            const unsigned turns=std::min(8u,unsigned(rageAge_/knightRageCycle));
            rageAge_+=dt;
            if(rageSlams_<8&&rageAge_>=knightRageImpact+rageSlams_*knightRageCycle){++rageSlams_;++impactSerial;}
            // Anim End -> Turn decrements Rages and flips only after the
            // complete five-frame clip, never during the impact frame.
            if(std::min(8u,unsigned(rageAge_/knightRageCycle))!=turns)scaleX=-scaleX;
            if(rageAge_>=8*knightRageCycle){
                if(completedStaggers_>=3){
                    const float finalX=authoredActivation_&&left<34&&right>34?34:(left+right)*.5f;
                    breakingFloor_=true;velocity={(finalX-position.x)*.76f,90};enter(BossPhase::Jump,0);
                }
                else enter(BossPhase::RageEnd,.75f);
            }
        }else if(phase==BossPhase::RageEnd&&timer_<=0)enter(BossPhase::Idle,idleDuration());
        else if(phase==BossPhase::Dying){
            constexpr float flightSeconds=1.1f;
            if(!deathHeadDetached_&&timer_<=0){
                deathHeadOrigin_=deathHeadPosition_=headPosition(position);
                deathHeadDetached_=true;
                timer_=flightSeconds;
                play("Body");
                if(bank_)headAnimation.play(bank_->require("Head/Death Head 1"));
            }else if(deathHeadDetached_&&!deathHeadLanded_){
                const float progress=std::clamp(1.f-timer_/flightSeconds,0.f,1.f);
                const Vec2 landing{std::clamp(position.x+7.966f,left+1.f,right-1.f),position.y-2.573f};
                deathHeadPosition_={deathHeadOrigin_.x+(landing.x-deathHeadOrigin_.x)*progress,
                    deathHeadOrigin_.y+(landing.y-deathHeadOrigin_.y)*progress+11.f*progress*(1.f-progress)};
                if(timer_<=0){
                    deathHeadLanded_=true;
                    timer_=1.5f;
                    if(bank_)headAnimation.play(bank_->require("Head/Death Head 2"));
                }
            }else if(deathHeadLanded_&&timer_<=0)defeat();
        }
        else if(phase==BossPhase::Collapse){
            if(collapseLanded_&&timer_<=0){
                finalExposure_=true;breakingFloor_=false;velocity={};
                ++staggerCount;enter(BossPhase::Stagger,0);health(40);
            }
        }
        if(phase==BossPhase::Collapse&&(startingPhase!=BossPhase::Collapse||collapseLanded_)){
            // BREAK removes the upper floor for the next physics step. Do not
            // land against this tick's pre-BREAK collision-world snapshot.
        }else if(phase==BossPhase::StunJump||phase==BossPhase::StunLand||phase==BossPhase::Collapse){
            // Dynamic Rigidbody136 / BoxCollider167 remains under gravity until
            // Roll End. resources.assets1073 "Geo Small": friction .2, bounce 0.
            // Floor Break restores gravity and waits for the same bottom contact.
            const auto b=bounds();
            const Rect local{b.left-position.x,b.bottom-position.y,b.right-position.x,b.top-position.y};
            // Loaded contour terrain owns the broken floor and actual lower
            // landing. The synthetic arena still has upper side platforms.
            const auto stunWorld=terrain?*terrain:arenaWorld();
            velocity.y+=tuning::gravity*dt;
            const auto contact=moveBox(position,velocity,local,dt,stunWorld,{.2f,0});
            // Contour-backed rooms do not consume the arena's auxiliary AABB
            // walls. Keep the Rigidbody inside the authored chamber before
            // the next gravity step can leave the floor polygon.
            const float halfWidth=(local.right-local.left)*.5f;
            if(position.x<left+halfWidth){position.x=left+halfWidth;velocity.x=std::max(0.f,velocity.x);}
            if(position.x>right-halfWidth){position.x=right-halfWidth;velocity.x=std::min(0.f,velocity.x);}
            // Source Stun In Air's bottom collision sends FALL into Stun Land.
            if(phase==BossPhase::StunJump&&contact.ground)enter(BossPhase::StunLand,.5f);
            if(phase==BossPhase::Collapse&&contact.ground){
                collapseGround_=bounds().bottom;collapseLanded_=true;velocity={};timer_=2;
            }
        }else{
            position.x=std::clamp(position.x+velocity.x*dt,left+1.8f*bodyScale_,right-1.8f*bodyScale_);
            position.y+=velocity.y*dt;
            if(phase!=BossPhase::Collapse)position.y=std::max(position.y,finalExposure_?collapseGround_-knightBody.bottom*bodyScale_:knightFloor);
        }
    }
    if(rocksPending_){
        rockTimer_-=dt;
        if(rockTimer_<=0){
            // FSM245 Spawn: RandomFloat(Summon Min13.18, Summon Max44.27).
            const float minX=authoredActivation_?13.18f:left+2;
            const float maxX=authoredActivation_?44.27f:right-2;
            launchRock(minX+randomUnit()*(maxX-minX));
            --rocksPending_;rockTimer_=.15f+.1f*randomUnit();
        }
    }
    const bool bodyHurts=!breakingFloor_&&!stunPreparation()&&phase!=BossPhase::Dormant&&phase!=BossPhase::Stagger&&phase!=BossPhase::Dying&&phase!=BossPhase::Brood&&phase!=BossPhase::Collapse&&phase!=BossPhase::Defeated;
    if(bodyHurts&&overlaps(bounds(),bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<position.x?-1:1);
    const auto weapon=weaponBounds();
    if(weapon.right>weapon.left&&overlaps(weapon,bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<position.x?-1:1);
    for(const auto& p:projectiles)for(const auto& spurt:p.waveSpurts)if(spurt.phase==2){
        // GO67 rotates Y180; GO68 retains the source local offset. The wave
        // controller's terrain sensor is not a hero damage collider.
        const float x=p.velocity.x<0?.047309875f:-.047309875f;
        if(overlaps(boxBounds(spurt.position,{x-.202195048f,.87f-.871806145f,x+.202195048f,.87f+.871806145f}),bodyBounds(hero.position)))
            hero.takeDamage(1,hero.position.x<spurt.position.x?-1:1);
    }
    const World barrelTerrain=terrain?*terrain:arenaWorld();
    for(auto& p:projectiles)if(p.life>0){
        PresentationPosition::Step projectileStep(p.presentation,p.position);
        // FSM147 has no flight timer: only ground-wave controllers time out.
        if(p.ground)p.life-=dt;
        if(!p.ground)p.velocity.y-=(p.deflected?18.f:19.5f)*dt;
        if(!p.awaitingWaveUpdate){p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;}
        if(p.ground&&!p.awaitingWaveUpdate&&!p.waveEnding&&terrain){
            // GO63 BoxCollider101: offset(-.237421,1.044737),
            // size(.657059,1.443039); Left flips local X. Two .01 skins.
            const float x=-sign(p.velocity.x)*.237421036f;
            const Rect sensor=boxBounds(p.position,{x-.328529656f-.02f,.323217154f-.02f,
                                                    x+.328529656f+.02f,1.766256571f+.02f});
            if(terrainSensorOverlap(sensor,*terrain)){
                p.waveEnding=true;p.waveEndPause=.15f;p.life=1.15f;
            }
        }
        if(p.ground&&!p.waveEnding&&(p.position.x<left||p.position.x>right))p.life=0;
        if(p.life>0&&!p.ground){
            const auto quad=p.barrelQuad();
            const bool playerContact=polygonOverlapsBox(quad.data(),quad.size(),bodyBounds(hero.position));
            const bool bossContact=objects_[0].colliderEnabled&&polygonOverlapsBox(quad.data(),quad.size(),bounds());
            if(playerContact)hero.takeDamage(1,hero.position.x<p.position.x?-1:1);
            if(bossContact&&p.deflected&&!p.barrelBreakUpdates){
                const NailHit impact{p.velocity.x<0?Strike::Left:Strike::Right,10,1,false,AttackType::Generic};
                const auto outcome=objects_[0].hit(impact,hero.vitals,hero.charms);hit(0,outcome);
            }
            // Idle listens for Player/layer8/layer11 even before a nail hit.
            // Collision sensors include Terrain triggers and two .01 skins.
            if(!p.barrelBreakUpdates&&(playerContact||bossContact||
               terrainQuadOverlap(p.barrelQuad(.02f),barrelTerrain)))
                // Pause enters during the trigger callback. NextFrameEvent
                // sends FINISHED on the first following Fsm.Update.
                p.barrelBreakUpdates=1;
        }
    }
    for(unsigned i=0;i<minions.size();++i){auto& m=minions[i];if(!m.alive)continue;
        PresentationPosition::Step minionStep(m.presentation,m.position);
        m.position.x+=m.velocity.x*dt;m.position.y+=m.velocity.y*dt;
        if(m.position.x<left+.4f||m.position.x>right-.4f)m.velocity.x=-m.velocity.x;
        if(m.position.y<bottom+.4f||m.position.y>top-.4f)m.velocity.y=-m.velocity.y;
        m.position.x=std::clamp(m.position.x,left+.35f,right-.35f);m.position.y=std::clamp(m.position.y,bottom+.35f,top-.35f);
        if(overlaps(boxBounds(m.position,broodBody),bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<m.position.x?-1:1);
    }
    sync();
}
}
