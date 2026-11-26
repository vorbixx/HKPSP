#include "conditional_events.hpp"
#include "decor_motion.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace hk3ds {
static_assert(sizeof(ConditionalEvents)<=28u*1024u,"conditional presentation permanent-state budget");
namespace {
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
bool circleOverlaps(Rect box,Vec2 center,float radius){const float x=std::clamp(center.x,box.left,box.right),y=std::clamp(center.y,box.bottom,box.top);return std::hypot(x-center.x,y-center.y)<radius;}
Rect grimmkinBox(Vec2 p){return {p.x-.7372496f,p.y-1.1693747f,p.x+.8337996f,p.y+.7913344f};}
float distance(Vec2 a,Vec2 b){return std::hypot(a.x-b.x,a.y-b.y);}

}
bool ConditionalEvents::bind(const AnimationBank* bank,const char* path)
{
    bank_=bank;
    if(!bank)return false;
    auto* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4];unsigned count=0;
    bool ok=std::fread(magic,4,1,f)==1&&std::memcmp(magic,"HKE1",4)==0&&std::fread(&count,4,1,f)==1&&count==texts_.size();
    for(unsigned i=0;ok&&i<count;++i){unsigned n=0;
        ok=std::fread(&n,4,1,f)==1&&n<texts_[i].size();
        if(ok){texts_[i].fill(0);ok=std::fread(texts_[i].data(),1,n,f)==n;}}
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    if(!ok)return false;
    const std::string textPath=path;
    const auto slash=textPath.find_last_of("/\\");
    const auto fxPath=textPath.substr(0,slash+1)+"events-fx.bin";
    f=std::fopen(fxPath.c_str(),"rb");if(!f)return false;
    ok=std::fread(magic,4,1,f)==1&&std::memcmp(magic,"HKF1",4)==0;
    static_assert(sizeof(EventEmitter)==10*sizeof(float));
    ok=ok&&std::fread(emitters_.data(),sizeof(EventEmitter),emitters_.size(),f)==emitters_.size()&&std::fgetc(f)==EOF;
    std::fclose(f);
    for(const auto& e:emitters_){
        const float values[]{e.rate,e.lifeMin,e.lifeMax,e.sizeMin,e.sizeMax,e.speedMin,e.speedMax,e.gravity,e.x,e.y};
        for(float v:values)ok=ok&&std::isfinite(v)&&std::abs(v)<100000;
        ok=ok&&e.rate>=0&&e.lifeMin>0&&e.lifeMax>=e.lifeMin&&e.sizeMin>=0&&e.sizeMax>=e.sizeMin;
    }
    if(!ok){emitters_={};return false;}
    const auto extraPath=textPath.substr(0,slash+1)+"events-fx-extra.bin";
    if(!loadSourceEffects(extraPath.c_str()))return false;
    reset(save_);return true;
}
void ConditionalEvents::mushroomState(MushroomPhase p,const char* clip,float duration)
{mushroom=p;mushroomTimer_=duration;mushroomAnimation.play(bank_&&clip?bank_->require(clip):nullptr,0,true);}
void ConditionalEvents::grimmkinState(GrimmkinPhase p,const char* clip,float duration)
{grimmkin=p;grimmkinTimer_=duration;grimmkinAnimation.play(bank_&&clip?bank_->require(clip):nullptr,0,true);}
void ConditionalEvents::reset(const ConditionalSave& s)
{
    save_=s.valid()?s:ConditionalSave{};dialogue=EventDialogue::None;listenPrompt=false;
    mushroomPosition={36.36f,12.27f};grimmkinPosition={147.67f,5.59f};velocity_={};facing=1;
    mushroomPresentation_.reset(mushroomPosition);grimmkinPresentation_.reset(grimmkinPosition);
    mushroomState(save_.mushroomPresent()?MushroomPhase::Rest:MushroomPhase::Off,"mushroom Rest");
    grimmkinState(save_.flamePresent()?GrimmkinPhase::Waiting:GrimmkinPhase::Off,nullptr,.25f);
    flameAnimation.play(bank_?bank_->require("Flame"):nullptr,0,true);flashAnimation={};
    projectiles={};particles={};emission_={};effectsRandom_=0x46584d52u;lastNailMushroom_=lastNailGrimmkin_=~0u;textPage_=lastPage_=0;
    if(sourceReady_&&(save_.mushroomPresent()||save_.flamePresent()))sourceParticles_.resize(sourceCapacity);
    else std::vector<HeroEffectParticle>().swap(sourceParticles_);
    for(auto& p:sourceParticles_)p.active=false;
    sourceEmissions_={};sourceEmitted_={};sourceLive_={};sourceDropped_=0;sourceRandom_=0x4d555348u;
    if(save_.flamePresent())for(unsigned i=2;i<5;++i)startSourceEffect(i,{146.47f,5.97f});
    random_=0x4752494du;lastAttack_=3;repeats_=0;missed_={};upHeld_=buttonHeld_=false;
    health={};constexpr int hp[]{75,120,180,210,240};health.hp=hp[save_.nailSmithUpgrades];health.enemyType=2;
    mushroomHitSerial=rocketSerial=dreamSerial=teleSerial=dashSerial=shootSerial=spiralSerial=deathSerial=pickupSerial=dashStartSerial=castSerial=ringSerial=teleInSerial=0;
}
float ConditionalEvents::random(float low,float high)
{random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return low+(high-low)*(float(random_>>8)/16777216.f);}
void ConditionalEvents::beginDialogue(EventDialogue kind,unsigned first,unsigned last)
{dialogue=kind;textPage_=first;lastPage_=last;if(kind!=EventDialogue::Dream)++dreamSerial;}
const char* ConditionalEvents::text() const
{
    return texts_[std::min(textPage_,unsigned(texts_.size()-1))].data();
}
void ConditionalEvents::finishDialogue(Hero& h)
{
    const auto kind=dialogue;dialogue=EventDialogue::None;
    if(kind==EventDialogue::Final){mushroomState(MushroomPhase::RocketPause,"mushroom Silent",.75f);return;}
    if(kind==EventDialogue::Farewell)mushroomState(MushroomPhase::Gone,nullptr);
    h.controlEnabled=true;
}
void ConditionalEvents::updateMushroom(float dt,Hero& h,const NailBank& nails)
{
    PresentationPosition::Step step(mushroomPresentation_,mushroomPosition);
    if(!mushroomVisible())return;
    mushroomAnimation.advance(dt);mushroomTimer_-=dt;
    // Source NPC trigger 8234; position includes its authored -2.1960793 offset.
    const bool near=overlap(bodyBounds(h.position),{34.36f,9.5606563f,38.36f,10.5871851f});
    const bool up=h.input.y>.5f,button=(h.input.pressed&(Jump|Attack|Focus|Dash))!=0;
    if(dialogue!=EventDialogue::None){
        if(button&&!buttonHeld_){if(textPage_<lastPage_)++textPage_;else finishDialogue(h);}
        buttonHeld_=button;upHeld_=up;listenPrompt=false;return;
    }
    listenPrompt=near&&mushroom==MushroomPhase::Idle;
    if(near&&(save_.flags&ConditionalSave::DreamNail)&&h.input.isPressed(Dream)){
        startSourceEffect(0,mushroomPosition);
        ++dreamSerial;beginDialogue(EventDialogue::Dream,6,7);
    }else if(listenPrompt&&up&&!upHeld_&&h.contacts.ground){
        if(save_.flags&ConditionalSave::SporeShroom){++save_.mrMushroomState;beginDialogue(EventDialogue::Final,0,4);}
        else {const unsigned page=8+unsigned(random(0,2.999f));beginDialogue(EventDialogue::Shroomish,page,page);}
    }
    upHeld_=up;buttonHeld_=button;
    if(h.nail.hitbox&&h.nail.serial!=lastNailMushroom_&&
       nailOverlaps(h,nails.shapes[nailVariant(h.nail)],{35.6948041f,9.7711153f,37.1738757f,13.4638233f})&&
       (mushroom==MushroomPhase::Rest||mushroom==MushroomPhase::Idle||mushroom==MushroomPhase::TurnBack)){
        lastNailMushroom_=h.nail.serial;++mushroomHitSerial;
        const bool resting=mushroom!=MushroomPhase::Idle;
        mushroomState(resting?MushroomPhase::Wake:MushroomPhase::Hit,resting?"mushroom Rest Hit":"mushroom Idle Hit",resting?41.f/12:3.f/18);
    }
    if(mushroomTimer_>0){
        if(mushroom==MushroomPhase::Lift){const float t=1-mushroomTimer_;mushroomPosition.y=12.27f+3.5f*(1-std::pow(2.f,-10*t));}
        if(mushroom==MushroomPhase::Fly){const float t=std::min(1.f,1.25f-mushroomTimer_);const float ease=t>=1?1:std::pow(2.f,10*(t-1));mushroomPosition={36.36f+.5f*ease,15.77f+15*ease};}
        return;
    }
    switch(mushroom){
    case MushroomPhase::Wake:case MushroomPhase::Hit:mushroomState(MushroomPhase::Idle,"mushroom Idle");break;
    case MushroomPhase::Idle:if(!near)mushroomState(MushroomPhase::TurnBack,"mushroom Turn Back",15.f/12);break;
    case MushroomPhase::TurnBack:mushroomState(MushroomPhase::Rest,"mushroom Rest");break;
    case MushroomPhase::RocketPause:++rocketSerial;mushroomState(MushroomPhase::RocketAntic,"mushroom Rocket Antic",8.f/12);break;
    case MushroomPhase::RocketAntic:startSourceEffect(1,mushroomPosition);mushroomState(MushroomPhase::Lift,"mushroom Jump",1);break;
    case MushroomPhase::Lift:mushroomPosition.y=15.77f;mushroomState(MushroomPhase::Fly,"mushroom Jump",1.25f);break;
    case MushroomPhase::Fly:mushroomState(MushroomPhase::TalkPause,nullptr,1.35f);break;
    case MushroomPhase::TalkPause:mushroomState(MushroomPhase::Gone,nullptr);beginDialogue(EventDialogue::Farewell,5,5);break;
    default:break;
    }
}
bool ConditionalEvents::grimmkinVisible() const
{return grimmkin!=GrimmkinPhase::Off&&grimmkin!=GrimmkinPhase::Waiting&&grimmkin!=GrimmkinPhase::Alert&&grimmkin!=GrimmkinPhase::Hidden&&grimmkin!=GrimmkinPhase::Absorb&&grimmkin!=GrimmkinPhase::Ring&&grimmkin!=GrimmkinPhase::Collected;}
void ConditionalEvents::shoot(bool spiral)
{
    const unsigned count=spiral?4:5;
    constexpr float angles[]{0,45,-45,22.5f,-22.5f};
    for(unsigned i=0;i<count;++i)for(auto& p:projectiles)if(!p.active){
        p={};p.active=true;p.spiral=spiral;p.position=grimmkinPosition;
        p.angle=spiral?float(i)*90:(facing>0?angles[i]:180-angles[i]);p.speed=spiral?12:(i<3?14:8);
        constexpr float radians=.01745329252f;p.velocity={p.speed*std::cos(p.angle*radians),p.speed*std::sin(p.angle*radians)};
        p.presentation.reset(p.position);p.animation.play(bank_?bank_->require("projectile Idle"):nullptr,0,true);break;
    }
}
void ConditionalEvents::attack(Hero& h)
{
    unsigned next=unsigned(random(0,2.999f));
    for(unsigned i=0;i<3;++i)if(missed_[i]>=3)next=i;
    const unsigned limit=next==2?2:3;
    if(next==lastAttack_&&repeats_>=limit)next=(next+1+unsigned(random(0,1.999f)))%3;
    repeats_=next==lastAttack_?repeats_+1:1;lastAttack_=next;
    for(unsigned i=0;i<3;++i)missed_[i]=i==next?0:missed_[i]+1;
    facing=h.position.x>grimmkinPosition.x?1:-1;
    if(next==0){++dashSerial;grimmkinState(GrimmkinPhase::DashAntic,"grimmkin Dash Antic",.5f);}
    else if(next==1){++shootSerial;grimmkinState(GrimmkinPhase::ShootAntic,"grimmkin Throw Antic",.5f);}
    else {++spiralSerial;grimmkinState(GrimmkinPhase::SpiralAntic,"grimmkin Throw Antic",.75f);}
}
void ConditionalEvents::updateGrimmkin(float dt,Hero& h,const NailBank& nails,bool frozen)
{
    PresentationPosition::Step step(grimmkinPresentation_,grimmkinPosition);
    if(grimmkin==GrimmkinPhase::Off||grimmkin==GrimmkinPhase::Collected)return;
    if(!frozen){grimmkinAnimation.advance(dt);flameAnimation.advance(dt);flashAnimation.advance(dt);grimmkinTimer_-=dt;}health.update(dt);
    const auto box=grimmkinBox(grimmkinPosition);
    if(grimmkinVisible()&&grimmkin!=GrimmkinPhase::Death&&grimmkin!=GrimmkinPhase::TeleOut){
        if(h.nail.hitbox&&lastNailGrimmkin_!=h.nail.serial&&nailOverlaps(h,nails.shapes[nailVariant(h.nail)],box)){
            lastNailGrimmkin_=h.nail.serial;const auto hit=health.hit({h.nail.direction,h.effectiveNailDamage(),1,false},h.vitals,h.charms);
            if(hit==HitOutcome::Killed||health.dead){++deathSerial;grimmkinState(GrimmkinPhase::Death,"grimmkin Death",1.5f);velocity_={};}
        }
        if(grimmkin!=GrimmkinPhase::Death&&overlap(bodyBounds(h.position),box))h.takeDamage(1,h.position.x<grimmkinPosition.x?-1:1);
    }
    if(frozen)return;
    if(grimmkin==GrimmkinPhase::Waiting){
        if(grimmkinTimer_<=0&&distance(h.position,{146.58f,5.94f})<=6.49671345f)grimmkinState(GrimmkinPhase::Alert,nullptr,1.5f);
        return;
    }
    if(grimmkin==GrimmkinPhase::Follow){
        // Approximation: acceleration integrates the serialized DistanceFlySmooth
        // parameters; Unity Rigidbody2D damping/contact solver is not reproduced.
        const Vec2 delta{h.position.x-grimmkinPosition.x,h.position.y-grimmkinPosition.y};const float d=std::hypot(delta.x,delta.y);
        if(d>1){velocity_.x+=delta.x/d*220*dt;velocity_.y+=delta.y/d*220*dt;const float speed=std::hypot(velocity_.x,velocity_.y);if(speed>30){velocity_.x*=30/speed;velocity_.y*=30/speed;}}
        else {velocity_.x*=.9f;velocity_.y*=.9f;}
        grimmkinPosition.x+=velocity_.x*dt;grimmkinPosition.y+=velocity_.y*dt;
    }
    if(grimmkin==GrimmkinPhase::DashAntic)grimmkinPosition.x-=12*facing*dt;
    if(grimmkin==GrimmkinPhase::Dash)grimmkinPosition.x+=50*facing*dt;
    if(grimmkinTimer_>0)return;
    switch(grimmkin){
    case GrimmkinPhase::Alert:++teleInSerial;grimmkinState(GrimmkinPhase::TeleIn,"grimmkin Tele In",9.f/20);break;
    case GrimmkinPhase::TeleIn:velocity_={};grimmkinState(GrimmkinPhase::Follow,"grimmkin Idle",random(.5f,.75f));break;
    case GrimmkinPhase::Follow:++teleSerial;grimmkinState(GrimmkinPhase::TeleOut,"grimmkin Tele Out",6.f/20);break;
    case GrimmkinPhase::TeleOut:grimmkinState(GrimmkinPhase::Hidden,nullptr,random(1,1.5f));break;
    case GrimmkinPhase::Hidden:{const float y=random(0,6),x=12-y;grimmkinPosition={h.position.x+(random(0,1)<.5f?-x:x),h.position.y+(random(0,1)<.5f?-y:y)};grimmkinPresentation_.reset(grimmkinPosition);attack(h);break;}
    case GrimmkinPhase::DashAntic:++dashStartSerial;grimmkinState(GrimmkinPhase::Dash,"grimmkin Dash",.425f);break;
    case GrimmkinPhase::Dash:grimmkinState(GrimmkinPhase::DashEnd,"grimmkin Dash End",.75f);break;
    case GrimmkinPhase::ShootAntic:shoot(false);grimmkinState(GrimmkinPhase::ShootEnd,"grimmkin Throw",1.5f);break;
    case GrimmkinPhase::SpiralAntic:++castSerial;shoot(true);grimmkinState(GrimmkinPhase::SpiralEnd,"grimmkin Throw",2);break;
    case GrimmkinPhase::DashEnd:case GrimmkinPhase::ShootEnd:case GrimmkinPhase::SpiralEnd:++teleSerial;grimmkinState(GrimmkinPhase::TeleOut,"grimmkin Tele Out",6.f/20);break;
    case GrimmkinPhase::Death:grimmkinState(GrimmkinPhase::Absorb,nullptr,1);break;
    case GrimmkinPhase::Absorb:++ringSerial;grimmkinState(GrimmkinPhase::Ring,nullptr,.5f);break;
    case GrimmkinPhase::Ring:++save_.flamesCollected;save_.flags|=ConditionalSave::TutorialFlameCollected|ConditionalSave::KilledGrimmkinMaster;++pickupSerial;flashAnimation.play(bank_?bank_->require("flash Flash"):nullptr,0,true);grimmkinState(GrimmkinPhase::Collected,nullptr,0);break;
    default:break;
    }
}
void ConditionalEvents::update(float dt,Hero& h,const NailBank& nails,bool freeze)
{
    if(!std::isfinite(dt)||dt<=0)return;
    if(h.actor==Actor::Dead){dialogue=EventDialogue::None;return;}
    // Farewell is displayed after the actor has left the screen.
    else if(mushroom==MushroomPhase::Gone&&dialogue==EventDialogue::Farewell){if(h.input.isPressed(Jump)||h.input.isPressed(Attack))finishDialogue(h);}
    else updateMushroom(dt,h,nails);
    if(locked()){h.controlEnabled=false;h.velocity={};h.nail={};h.focus=FocusPhase::None;}
    updateGrimmkin(dt,h,nails,freeze);
    if(grimmkin==GrimmkinPhase::Collected)flashAnimation.advance(dt);
    updateEffects(dt,h);
    if(freeze)return;
    for(auto& p:projectiles)if(p.active){
        PresentationPosition::Step step(p.presentation,p.position);p.age+=dt;p.animation.advance(dt);
        const float life=p.spiral?3.f:5.f;
        if(p.age>life+.5f){p.active=false;continue;}
        if(p.spiral){p.angle+=100*dt;p.speed+=(2+2*p.age)*dt;const float r=p.angle*.01745329252f;p.velocity={p.speed*std::cos(r),p.speed*std::sin(r)};}
        p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
        if(p.age<=life&&circleOverlaps(bodyBounds(h.position),{p.position.x+.04687503f,p.position.y+.03125f},.48f))h.takeDamage(1,h.position.x<p.position.x?-1:1);
    }
}

void ConditionalEvents::emit(unsigned type,Vec2 origin,float dt)
{
    const auto& e=emitters_[type];
    // Approximation: 1/32 of the source emission, spherical 2D shape and linear
    // lifetime fade. Source texture, material tint, ranges and offsets retained.
    emission_[type]+=e.rate*dt/32;
    const auto sample=[&](float lo,float hi){effectsRandom_^=effectsRandom_<<13;effectsRandom_^=effectsRandom_>>17;effectsRandom_^=effectsRandom_<<5;return lo+(hi-lo)*(float(effectsRandom_>>8)/16777216.f);};
    while(emission_[type]>=1){
        emission_[type]-=1;
        for(auto& p:particles)if(!p.active){
            p={};p.active=true;p.position={origin.x+e.x,origin.y+e.y};p.presentation.reset(p.position);
            p.life=sample(e.lifeMin,e.lifeMax);p.size=sample(e.sizeMin,e.sizeMax);p.gravity=e.gravity;
            const float angle=sample(0,6.2831853f),speed=sample(e.speedMin,e.speedMax);
            p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
            char name[32];std::snprintf(name,sizeof(name),"event particle %u",type);
            p.animation.play(bank_?bank_->require(name):nullptr);break;
        }
    }
}
void ConditionalEvents::updateEffects(float dt,Hero& h)
{
    // Blastoff2 simulates in world space: births follow the rising parent,
    // while particles already emitted keep their world positions.
    if(sourceEmissions_[1].active)sourceEmissions_[1].origin=mushroomPosition;
    // Start Fight disables the complete Active Effects hierarchy, including
    // its existing local particles, before the medium Grimmkin teleports in.
    if(grimmkin!=GrimmkinPhase::Waiting&&grimmkin!=GrimmkinPhase::Alert){
        for(unsigned i=2;i<5;++i)sourceEmissions_[i].active=false;
        if(sourceLive_[2]||sourceLive_[3]||sourceLive_[4])
            for(auto& p:sourceParticles_)if(p.active&&p.emitter>=2){p.active=false;--sourceLive_[p.emitter];}
    }
    updateSourceEffects(dt);
    for(auto& p:particles)if(p.active){
        PresentationPosition::Step step(p.presentation,p.position);p.age+=dt;
        if(p.age>=p.life){p.active=false;continue;}
        p.velocity.y+=p.gravity*dt;p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
    }
    if(mushroom==MushroomPhase::Wake||mushroom==MushroomPhase::RocketAntic)emit(0,mushroomPosition,dt);
    if(mushroom==MushroomPhase::Lift)emit(1,mushroomPosition,dt);
    if(mushroom==MushroomPhase::Fly)emit(2,mushroomPosition,dt);
    if(mushroom==MushroomPhase::Idle)emit(7,mushroomPosition,dt);
    if(grimmkin==GrimmkinPhase::TeleIn||grimmkin==GrimmkinPhase::TeleOut)emit(3,grimmkinPosition,dt);
    if(grimmkin==GrimmkinPhase::Death)emit(4,grimmkinPosition,dt);
    if(grimmkin==GrimmkinPhase::SpiralAntic)emit(5,grimmkinPosition,dt);
    // Approximation: pickup is anchored above the hero until the Grimmchild
    // follower is ported; the scene's source effect is attached to Grimmchild.
    if(grimmkin==GrimmkinPhase::Ring)emit(6,{h.position.x,h.position.y+1.5f},dt);
}

namespace {
float sourceSample(const EventSourceEmitter& e,unsigned offset,float t)
{
    const float x=std::clamp(t,0.f,1.f)*7;const unsigned i=std::min(6u,unsigned(x));
    return e.values[offset+i]+(e.values[offset+i+1]-e.values[offset+i])*(x-i);
}
Vec2 sourceProject(const std::array<float,86>& v,unsigned base,float x,float y,float z)
{return {v[base]*x+v[base+3]*y+v[base+6]*z,v[base+1]*x+v[base+4]*y+v[base+7]*z};}
}
bool ConditionalEvents::loadSourceEffects(const char* path)
{
    sourceReady_=false;
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    constexpr unsigned owners[]{2120,2124,3693,3769,3788};
    char magic[4];unsigned count=0;
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"HKX1",4)&&std::fread(&count,4,1,f)==1&&count==5;
    for(unsigned i=0;ok&&i<count;++i){
        auto& e=sourceEmitters_[i];unsigned fields[5]{};
        ok=std::fread(fields,4,5,f)==5&&std::fread(e.values.data(),4,86,f)==86;
        e.owner=fields[0];e.shape=fields[1];e.tiles=fields[2];e.flags=fields[3];e.capacity=fields[4];
        ok=ok&&e.owner==owners[i]&&(e.shape==10||e.shape==12)&&e.tiles>0&&e.tiles<=6&&e.flags<=3&&e.capacity>0;
        for(float v:e.values)ok=ok&&std::isfinite(v);
        ok=ok&&e.values[0]>0&&e.values[0]<=5&&e.values[1]>=0&&e.values[2]>0&&e.values[3]>=e.values[2]&&e.values[3]<=3;
        char name[48];std::snprintf(name,sizeof(name),"event source particle %u",e.owner);
        sourceClips_[i]=bank_->find(name);ok=ok&&sourceClips_[i]&&sourceClips_[i]->count==e.tiles;
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);sourceReady_=ok;return ok;
}
void ConditionalEvents::startSourceEffect(unsigned index,Vec2 origin)
{
    // PlayParticleEmitter.OnEnter skips Play while that system is still playing.
    if(sourceEmissions_[index].active||sourceLive_[index])return;
    sourceEmissions_[index]={origin,0,0,true};
}
float ConditionalEvents::sourceRandom(float lo,float hi)
{sourceRandom_=sourceRandom_*1664525u+1013904223u;return lo+(hi-lo)*(float(sourceRandom_>>8)/16777216.f);}
void ConditionalEvents::spawnSourceParticle(unsigned index,Vec2 origin)
{
    const auto& e=sourceEmitters_[index];const auto& v=e.values;
    if(sourceLive_[index]>=e.capacity)return;
    auto it=std::find_if(sourceParticles_.begin(),sourceParticles_.end(),[](const auto& p){return !p.active;});
    if(it==sourceParticles_.end()){++sourceDropped_;return;}
    auto& p=*it;p={};p.active=true;p.emitter=index;p.clip=sourceClips_[index];
    const float theta=sourceRandom(0,v[13]),r=std::sqrt(sourceRandom(0,1));
    float x=std::cos(theta)*r*v[11],y=std::sin(theta)*r*v[11],dx=std::cos(theta),dy=std::sin(theta);
    if(e.shape==12){x=sourceRandom(-v[11],v[11]);y=0;dx=0;dy=1;}
    const auto offset=sourceProject(v,18,x,y,0);p.position={origin.x+v[16]+offset.x,origin.y+v[17]+offset.y};
    const float speed=sourceRandom(v[4],v[5]);p.velocity=sourceProject(v,27,dx*speed,dy*speed,0);
    p.velocityZ=(v[29]*dx+v[32]*dy)*speed;
    for(unsigned k=0;k<2;++k){
        const unsigned b=36+k*6;const float a=sourceRandom(v[b],v[b+1]),by=sourceRandom(v[b+2],v[b+3]),bz=sourceRandom(v[b+4],v[b+5]);
        const auto xy=e.flags&(1u<<k)?sourceProject(v,27,a,by,bz):Vec2{a,by};
        const float z=e.flags&(1u<<k)?v[29]*a+v[32]*by+v[35]*bz:bz;
        if(k){p.module=xy;p.moduleZ=z;}else{p.force=xy;p.forceZ=z;}
    }
    p.force.y-=9.81f*v[10];p.life=sourceRandom(v[2],v[3]);p.size=sourceRandom(v[6],v[7]);
    p.angle=sourceRandom(v[8],v[9]);p.spin=sourceRandom(v[48],v[49]);p.mix=sourceRandom(0,1);
    p.history.reset(p.position);++sourceEmitted_[index];++sourceLive_[index];
}
void ConditionalEvents::updateSourceEffects(float dt)
{
    if(!sourceReady_||(!sourceActiveCount()&&std::none_of(sourceEmissions_.begin(),sourceEmissions_.end(),[](const auto& e){return e.active;})))return;
    for(auto& p:sourceParticles_)if(p.active){
        PresentationPosition::Step step(p.history,p.position);p.age+=dt;
        if(p.age>=p.life){p.active=false;--sourceLive_[p.emitter];continue;}
        const auto& v=sourceEmitters_[p.emitter].values;
        std::array<float,3> position{p.position.x,p.position.y,0},base{p.velocity.x,p.velocity.y,p.velocityZ};
        decorMotionStep(position,base,{p.module.x,p.module.y,p.moduleZ},
            {p.force.x*dt,p.force.y*dt,p.forceZ*dt},{},dt,v[15],decorRetention(v[14],dt));
        p.position={position[0],position[1]};p.velocity={base[0],base[1]};p.velocityZ=base[2];p.angle+=p.spin*dt;
    }
    for(unsigned i=0;i<5;++i){auto& emission=sourceEmissions_[i];if(!emission.active)continue;
        const auto& e=sourceEmitters_[i];const auto& v=e.values;
        const float before=std::max(0.f,emission.age-v[1]);emission.age+=dt;
        const float after=std::max(0.f,emission.age-v[1]);
        const float duration=i<2?std::max(0.f,std::min(after,v[0])-std::min(before,v[0])):after-before;
        emission.remainder+=duration*sourceSample(e,74,std::fmod((before+after)*.5f,v[0])/v[0]);
        while(emission.remainder>=.99999f){spawnSourceParticle(i,emission.origin);emission.remainder-=1;}
        if(i<2&&after>=v[0])emission.active=false;
    }
}
float ConditionalEvents::sourceParticleSize(const HeroEffectParticle& p) const
{const auto& e=sourceEmitters_[p.emitter];return p.size*(sourceSample(e,50,p.age/p.life)*(1-p.mix)+sourceSample(e,58,p.age/p.life)*p.mix);}
float ConditionalEvents::sourceParticleOpacity(const HeroEffectParticle& p) const
{return std::clamp(sourceSample(sourceEmitters_[p.emitter],66,p.age/p.life),0.f,1.f);}
}
