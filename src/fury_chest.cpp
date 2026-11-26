#include "fury_chest.hpp"
#include "terrain.hpp"
#include "render_memory.hpp"
#include "fury_tutorial_data.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
float furyTutorialAlpha(FuryTutorialPart part,float seconds) {
    const auto index=static_cast<unsigned>(part);
    if(index>=furytutorialdata::reveals.size()||!std::isfinite(seconds))return 0;
    const auto& reveal=furytutorialdata::reveals[index];
    if(seconds<reveal.delay)return 0;
    if(part==FuryTutorialPart::Prompt){
        const auto& keys=furytutorialdata::prompt;
        const float time=std::fmod(seconds-reveal.delay,keys.back().time);
        for(unsigned i=1;i<keys.size();++i){
            const auto& a=keys[i-1];const auto& b=keys[i];
            if(time>b.time)continue;
            const float span=b.time-a.time,t=(time-a.time)/span,t2=t*t,t3=t2*t;
            // Unity's unweighted AnimationCurve uses cubic Hermite tangents.
            return std::clamp((2*t3-3*t2+1)*a.value+(t3-2*t2+t)*span*a.outSlope+
                              (-2*t3+3*t2)*b.value+(t3-t2)*span*b.inSlope,0.f,1.f);
        }
        return 0;
    }
    return reveal.alpha*(reveal.duration>0?std::clamp((seconds-reveal.delay)/reveal.duration,0.f,1.f):1.f);
}
float furyTutorialRowAlpha(unsigned row,float seconds) {
    return row<furytutorialdata::rows.size()?furyTutorialAlpha(static_cast<FuryTutorialPart>(furytutorialdata::rows[row]),seconds):0;
}
static_assert(sizeof(FuryChest)<=furyParticleStateBudgetBytes,"Fury fixed state exceeds its ordinary-heap budget");
namespace {
constexpr unsigned index(FuryPart p) { return static_cast<unsigned>(p); }
bool validRect(Rect r) { return std::isfinite(r.left)&&std::isfinite(r.right)&&std::isfinite(r.bottom)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top; }
}
bool FuryChestConfig::load(const char* path) {
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4]{};float v[61]{};unsigned range=0;
    FuryChestConfig next;
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"FCH3",4)&&std::fread(v,4,61,f)==61&&std::fread(&range,4,1,f)==1&&range<=1;
    for(auto& e:next.emitters){
        ok &= std::fread(e.v.data(),4,e.v.size(),f)==e.v.size();
        for(float x:e.v)ok &= std::isfinite(x);
        ok &= e.v[11]>=0&&e.v[11]<=1000&&e.v[12]>=0&&e.v[13]>0&&e.v[14]>0&&e.v[14]<=4;
    }
    float motion[13]{};ok &= std::fread(motion,4,13,f)==13;
    for(float x:motion)ok &= std::isfinite(x);
    next.shinyBounds={motion[0],motion[1],motion[2],motion[3]};next.shinyGravity=motion[4];
    for(unsigned i=0;i<3;++i){next.flingRight[i]=motion[5+i];next.flingLeft[i]=motion[8+i];}
    next.bounceFactor=motion[11];next.speedThreshold=motion[12];
    ok &= validRect(next.shinyBounds)&&next.shinyGravity<0&&next.bounceFactor>0&&next.bounceFactor<1&&next.speedThreshold>0;
    ok &= std::fgetc(f)==EOF;
    std::fclose(f);for(float x:v)ok &= std::isfinite(x);
    unsigned n=0;
    for(auto& p:next.parts){p.position={v[n],v[n+1]};p.depth=v[n+2];p.scale={v[n+3],v[n+4]};n+=5;ok &= p.scale.x!=0&&p.scale.y!=0;}
    next.nailBounds={v[40],v[41],v[42],v[43]};next.heroRange={v[44],v[45],v[46],v[47]};
    next.inspectRange={v[48],v[49],v[50],v[51]};next.spawnSeconds=v[52];
    if(!ok||!validRect(next.nailBounds)||!validRect(next.heroRange)||!validRect(next.inspectRange)||next.spawnSeconds<0)return false;
    next.rangeRequired=range;
    next.openedSides={{{v[53],v[54],v[55],v[56]},{v[57],v[58],v[59],v[60]}}};
    if(!validRect(next.openedSides[0])||!validRect(next.openedSides[1]))return false;
    *this=next;return true;
}
bool FuryChest::bind(const AnimationBank& bank,const char* path) {
    FuryChestConfig c;return c.load(path)&&bind(bank,c);
}
bool FuryChest::bind(const AnimationBank& bank,const FuryChestConfig& c) {
    for(const char* name:{"Fury Idle","Fury Open","Fury Opened","Fury Back","Fury Front","Fury Burst","Fury Wave","Fury Shiny","Fury Glow","Fury Icon",
                         "Fury Particle Idle","Fury Particle Ring","Fury Particle Trail","Fury Particle Flash","Fury Strike","Fury Flash","Fury Tink"})
        if(!bank.find(name))return false;
    if(!validRect(c.nailBounds)||!validRect(c.heroRange)||!validRect(c.inspectRange)||!std::isfinite(c.spawnSeconds)||c.spawnSeconds<0)return false;
    if(!validRect(c.shinyBounds)||!std::isfinite(c.shinyGravity)||c.shinyGravity>=0||
       !std::isfinite(c.bounceFactor)||c.bounceFactor<=0||c.bounceFactor>=1||
       !std::isfinite(c.speedThreshold)||c.speedThreshold<=0)return false;
    for(const auto& fling:{c.flingRight,c.flingLeft})
        if(!std::isfinite(fling[0])||!std::isfinite(fling[1])||!std::isfinite(fling[2])||fling[0]<=0||fling[1]>fling[2])return false;
    for(const auto& p:c.parts){
        for(float v:{p.position.x,p.position.y,p.depth,p.scale.x,p.scale.y})if(!std::isfinite(v))return false;
        if(p.scale.x==0||p.scale.y==0)return false;
    }
    bank_=&bank;config_=c;reset();return true;
}
void FuryChest::syncTerrain() {
    if(!terrain_)return;
    const bool open=phase_==FuryChestPhase::Opened;
    terrain_->setEnabled(8214,!open);terrain_->setEnabled(8215,open);terrain_->setEnabled(8216,open);
}
bool FuryChest::bindTerrain(TerrainBank& terrain) {
    bool root=false;for(const auto& s:terrain.shapes)root|=s.id==8214;
    if(!root||!validRect(config_.openedSides[0])||!validRect(config_.openedSides[1]))return false;
    for(unsigned i=0;i<2;++i){
        const unsigned id=8215+i;bool found=false;for(const auto& s:terrain.shapes)found|=s.id==id;
        if(found)continue;
        const auto b=config_.openedSides[i];TerrainShape s;s.id=id;s.flags=8;s.material={.2f,0};
        s.first=static_cast<unsigned>(terrain.vertices.size());s.count=4;s.bounds=b;s.enabled=false;
        terrain.vertices.insert(terrain.vertices.end(),{{b.left,b.bottom},{b.right,b.bottom},{b.right,b.top},{b.left,b.top}});
        terrain.shapes.push_back(s);
    }
    terrain_=&terrain;syncTerrain();return true;
}
void FuryChest::start(FuryPart part,const char* clip,bool visible) {
    auto& d=draws_[index(part)];d.visible=visible;if(bank_)d.player.play(bank_->find(clip),0,true);
}
unsigned FuryChest::flags() const { return (opened_?openedFlag:0)|(gotCharm6_?gotCharm6Flag:0)|(hasCharm_?hasCharmFlag:0); }
void FuryChest::restore(unsigned f) {
    opened_=(f&openedFlag)||(f&gotCharm6Flag);gotCharm6_=f&gotCharm6Flag;hasCharm_=f&hasCharmFlag;reset();
}
void FuryChest::reset(bool preserve,unsigned seed) {
    if(!preserve)opened_=gotCharm6_=hasCharm_=false;
    draws_=config_.parts;openSerial_=pickupSerial_=0;openingSeconds_=displaySeconds_=waveSeconds_=0;prompt_=false;
    particles_={};particlesLive_=false;emission_={};random_=seed?seed:1;ringSeconds_=-1;waveAccel_=3;waveFixed_=0;
    idleWaveSeconds_=shakeSeconds_=shakeFrame_=0;shake_={};
    shinyPosition_=config_.parts[index(FuryPart::Shiny)].position;shinyVelocity_={};
    shinyHistory_.reset(shinyPosition_);flying_=shinyContact_=false;shinySettled_=true;
    effects_={};flashSeconds_=-1;tinkDelay_=tinkRotation_=0;tinkSerial_=0;flashOrigin_={};
    phase_=opened_?FuryChestPhase::Opened:FuryChestPhase::Closed;
    pickup_=gotCharm6_?FuryPickupPhase::Collected:opened_?FuryPickupPhase::Idle:FuryPickupPhase::Hidden;
    start(FuryPart::Chest,opened_?"Fury Opened":"Fury Idle",!opened_);
    start(FuryPart::Back,"Fury Back",opened_);start(FuryPart::Front,"Fury Front",opened_);
    start(FuryPart::Burst,"Fury Burst",false);start(FuryPart::Wave,"Fury Wave",false);
    start(FuryPart::Shiny,"Fury Shiny",pickup_==FuryPickupPhase::Idle);
    start(FuryPart::Glow,"Fury Glow",pickup_==FuryPickupPhase::Idle);
    start(FuryPart::Icon,"Fury Icon",false);syncTerrain();
    if(pickup_==FuryPickupPhase::Idle){beginWave();fling();}
}
float FuryChest::random(float a,float b) {
    random_=1664525u*random_+1013904223u;
    return a+(b-a)*float(random_>>8)/16777216.f;
}
Rect FuryChest::inspectRange() const {
    const auto p=config_.parts[index(FuryPart::Shiny)].position;
    const float x=shinyPosition_.x-p.x,y=shinyPosition_.y-p.y;
    const auto r=config_.inspectRange;return {r.left+x,r.bottom+y,r.right+x,r.top+y};
}
GeoVisualDraw FuryChest::draw(FuryPart part,PresentationTime time) const {
    auto d=draws_[index(part)];
    if(part==FuryPart::Shiny||part==FuryPart::Wave){
        const auto p=shinyRenderPosition(time),origin=config_.parts[index(FuryPart::Shiny)].position;
        d.position.x+=p.x-origin.x;d.position.y+=p.y-origin.y;
    }
    return d;
}
void FuryChest::fling() {
    flying_=true;shinyContact_=shinySettled_=false;
    const auto& v=random(0,1)<.5f?config_.flingLeft:config_.flingRight;
    const float angle=random(v[1],v[2])*.01745329252f;
    shinyVelocity_={v[0]*std::cos(angle),v[0]*std::sin(angle)};
    shinyHistory_.reset(shinyPosition_);
}
void FuryChest::updateShiny(float dt) {
    PresentationPosition::Step step(shinyHistory_,shinyPosition_);
    if(shinySettled_)return;
    // Source Rigidbody gravity and ObjectBounce; shared terrain box solver
    // approximates native contacts, including walls and the opened chest sides.
    float remaining=dt;
    while(remaining>1e-6f&&!shinySettled_){
        const float seconds=std::min(.02f,remaining);remaining-=seconds;
        shinyVelocity_.y+=config_.shinyGravity*seconds;
        const Vec2 incoming=shinyVelocity_;
        const auto contacts=moveBox(shinyPosition_,shinyVelocity_,config_.shinyBounds,seconds,
                                    terrain_?terrain_->world():World{},{});
        const bool touching=contacts.ground||contacts.roof||contacts.left||contacts.right;
        if(flying_&&touching&&!shinyContact_&&std::hypot(incoming.x,incoming.y)>config_.speedThreshold){
            Vec2 normal=contacts.count?contacts.surfaces[0].normal:
                contacts.ground?Vec2{0,1}:contacts.roof?Vec2{0,-1}:contacts.left?Vec2{1,0}:Vec2{-1,0};
            const float dot=incoming.x*normal.x+incoming.y*normal.y;
            // A contact while leaving the authored overlapping spawn is not
            // an impact. Reflect only motion entering the contact surface.
            if(dot<0){
                const float bounce=config_.bounceFactor*random(.8f,1.2f);
                shinyVelocity_={(incoming.x-2*dot*normal.x)*bounce,(incoming.y-2*dot*normal.y)*bounce};
            }
        }
        shinyContact_=touching;
        if(flying_&&std::hypot(shinyVelocity_.x,shinyVelocity_.y)<config_.speedThreshold){
            flying_=false;shinyVelocity_={}; // Land stops Trail; Idle enables inspection.
        }
        // Land's SetGravity2dScale action is DISABLED in the source. After
        // StopBounce the body still falls until the terrain supports it.
        if(!flying_&&contacts.ground){shinyVelocity_={};shinySettled_=true;}
    }
}
void FuryChest::emit(unsigned emitter) {
    auto slot=particles_.end();for(auto i=particles_.begin();i!=particles_.end();++i)if(!i->draw.visible){slot=i;break;}
    if(slot==particles_.end())return;
    const auto& v=config_.emitters[emitter].v;
    const float angle=random(0,v[9]),x=std::cos(angle),y=std::sin(angle);
    const float dx=v[3]*x+v[6]*y,dy=v[4]*x+v[7]*y,dz=v[5]*x+v[8]*y;
    const float length=std::sqrt(dx*dx+dy*dy+dz*dz),speed=random(v[17],v[18]);
    *slot={};auto& p=*slot;p.emitter=emitter;
    // Idle/Ring have radiusThickness=0; Trail/Flash fill their source discs.
    const float radius=v[10]*(emitter>=2?std::sqrt(random(0,1)):1);
    p.draw.position={v[0]+dx*radius,v[1]+dy*radius};p.draw.depth=v[2]+dz*radius;
    if(emitter==2){const auto origin=config_.parts[index(FuryPart::Shiny)].position;
        p.draw.position.x+=shinyPosition_.x-origin.x;p.draw.position.y+=shinyPosition_.y-origin.y;}
    p.velocity={dx*speed/std::max(.0001f,length),dy*speed/std::max(.0001f,length)};p.velocityZ=dz*speed/std::max(.0001f,length);
    p.forceY=random(v[19],v[20]);p.size=random(v[15],v[16]);p.lifetime=random(v[13],v[14]);
    if(emitter==3){p.draw.position.x+=flashOrigin_.x;p.draw.position.y+=flashOrigin_.y;}
    static const char* names[]={"Fury Particle Idle","Fury Particle Ring","Fury Particle Trail","Fury Particle Flash"};
    p.draw.player.play(bank_->find(names[emitter]),0,true);p.draw.visible=true;p.draw.scale={0,0};particlesLive_=true;
}
void FuryChest::updateParticles(float dt) {
    if(!particlesLive_&&!flying_&&phase_==FuryChestPhase::Opened&&
       !(ringSeconds_>=0&&ringSeconds_<config_.emitters[1].v[12])&&
       !(flashSeconds_>=0&&flashSeconds_<config_.emitters[3].v[12]))return;
    particlesLive_=false;
    for(auto& p:particles_)if(p.draw.visible){
        p.age+=dt;if(p.age>=p.lifetime){p.draw.visible=false;continue;}particlesLive_=true;
        const auto& v=config_.emitters[p.emitter].v;
        p.velocity.y+=p.forceY*dt;
        const float speed=std::sqrt(p.velocity.x*p.velocity.x+p.velocity.y*p.velocity.y+p.velocityZ*p.velocityZ);
        if(v[21]>0&&speed>v[21]){
            // Approximation: time-normalized damping of the source Limit Velocity module.
            const float keep=(v[21]+(speed-v[21])*std::pow(1-v[22],dt*60))/speed;
            p.velocity.x*=keep;p.velocity.y*=keep;p.velocityZ*=keep;
        }
        p.draw.position.x+=p.velocity.x*dt;p.draw.position.y+=p.velocity.y*dt;p.draw.depth+=p.velocityZ*dt;
        const float t=std::min(31.f,p.age/p.lifetime*31);const unsigned i=std::min(30u,unsigned(t));
        const float size=p.size*(v[24+i]+(v[25+i]-v[24+i])*(t-i));p.draw.scale={size,size};
    }
    for(unsigned e=0;e<4;++e){
        const auto& v=config_.emitters[e].v;
        const float clock=e==1?ringSeconds_:flashSeconds_;
        const float seconds=e==0?(phase_==FuryChestPhase::Closed?dt:0):e==2?(flying_?dt:0):
            (e==1||e==3)&&clock>=0?std::min(dt,std::max(0.f,v[12]-clock)):0;
        emission_[e]+=seconds*v[11];
        while(emission_[e]+1e-5f>=1){emit(e);emission_[e]-=1;}
    }
    if(ringSeconds_>=0)ringSeconds_+=dt;
}
float FuryChest::flashOpacity() const {
    if(flashSeconds_<0||flashSeconds_>=.25f)return 0;
    if(flashSeconds_<=.2f)return 1;
    // Original Hermite alpha: (.2,1,out=-10), (.25,0,in=-20).
    const float t=(flashSeconds_-.2f)/.05f;
    return std::clamp((2*t*t*t-3*t*t+1)-.5f*(t*t*t-2*t*t+t)-(t*t*t-t*t),0.f,1.f);
}
void FuryChest::beginWave() {
    start(FuryPart::Wave,"Fury Wave");waveSeconds_=waveFixed_=0;waveAccel_=3;
    auto& d=draws_[index(FuryPart::Wave)];const auto parent=config_.parts[index(FuryPart::Wave)].scale;d.scale={.3f*parent.x,.3f*parent.y};
}
void FuryChest::update(float dt,const FuryChestInput& in) {
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    updateShiny(dt);
    updateParticles(dt);
    tinkDelay_=std::max(0.f,tinkDelay_-dt);
    if(flashSeconds_>=0)flashSeconds_+=dt;
    for(auto& effect:effects_)if(effect.visible){effect.player.advance(dt);if(effect.player.complete)effect.visible=false;}
    if(shakeSeconds_>0){
        shakeSeconds_=std::max(0.f,shakeSeconds_-dt);shakeFrame_+=dt;
        if(shakeFrame_+1e-6f>=1.f/60){
            shakeFrame_=std::fmod(shakeFrame_,1.f/60);
            const float extent=.105f*shakeSeconds_/.5f;
            shake_={random(-extent,extent),random(-extent,extent)};
        }
        if(shakeSeconds_==0)shake_={};
    }
    if(pickup_==FuryPickupPhase::Idle||pickup_==FuryPickupPhase::Aligning||pickup_==FuryPickupPhase::Lowering){
        idleWaveSeconds_+=dt;
        if(idleWaveSeconds_>=2.2f){idleWaveSeconds_-=2.2f;beginWave();}
    }
    for(auto& d:draws_)if(d.visible)d.player.advance(dt);
    for(auto part:{FuryPart::Burst})if(draws_[index(part)].player.complete)draws_[index(part)].visible=false;
    auto& wave=draws_[index(FuryPart::Wave)];
    if(wave.visible){
        waveFixed_+=dt;while(waveFixed_>=.02f){waveAccel_=std::max(.5f,waveAccel_*.95f);waveFixed_-=.02f;}
        waveSeconds_+=dt*waveAccel_;const float growth=(1+4*waveSeconds_)*.3f;
        const auto parent=config_.parts[index(FuryPart::Wave)].scale;
        wave.scale={growth*parent.x,growth*parent.y};if(waveSeconds_>=1)wave.visible=false;
    }
    if(phase_==FuryChestPhase::Closed&&in.nailHit&&(!config_.rangeRequired||in.heroRange)){
        opened_=true;phase_=FuryChestPhase::Opening;openingSeconds_=0;++openSerial_;
        shakeSeconds_=.5f;shakeFrame_=0;
        start(FuryPart::Chest,"Fury Open");
        auto& strike=effects_[0];strike=config_.parts[index(FuryPart::Chest)];strike.visible=true;
        // Source Strike FSM23926 randomizes uniform XY scale and Z rotation.
        const float scale=random(2.5f,3.f);strike.scale={scale,scale};strike.rotation=random(0,6.28318530718f);
        strike.player.play(bank_->find("Fury Strike"),0,true);
    } else if(phase_==FuryChestPhase::Opening){
        openingSeconds_+=dt;
        // Open frame 1 triggers Spawn Items; animation completion enters Opened.
        if(pickup_==FuryPickupPhase::Hidden&&openingSeconds_+1e-6f>=config_.spawnSeconds){
            pickup_=FuryPickupPhase::Idle;start(FuryPart::Shiny,"Fury Shiny");start(FuryPart::Glow,"Fury Glow");
            fling();
            start(FuryPart::Burst,"Fury Burst");beginWave();idleWaveSeconds_=0;ringSeconds_=0;
        }
        if(draws_[index(FuryPart::Chest)].player.complete){
            phase_=FuryChestPhase::Opened;start(FuryPart::Chest,"Fury Opened",false);
            start(FuryPart::Back,"Fury Back");start(FuryPart::Front,"Fury Front");syncTerrain();
        }
    }
    if(phase_==FuryChestPhase::Opened&&in.nailHit&&tinkDelay_==0){
        tinkDelay_=.25f;++tinkSerial_;shakeSeconds_=.5f;
        const auto& side=config_.openedSides[std::min(1u,in.openedSide)];
        auto& tink=effects_[2];tink={};tink.visible=true;tink.depth=.002f;tink.position=in.nailPosition;
        if(in.direction==Strike::Right){tink.position.x=side.left;tinkRotation_=0;}
        else if(in.direction==Strike::Up){tink.position.y=std::max(side.bottom,tink.position.y);tinkRotation_=90;}
        else if(in.direction==Strike::Left){tink.position.x=side.right;tinkRotation_=180;}
        else {tink.position.y=std::min(side.top,tink.position.y);tinkRotation_=270;}
        tink.player.play(bank_->find("Fury Tink"),0,true);
    }
    prompt_=pickup_==FuryPickupPhase::Idle&&!flying_&&in.inspectRange&&in.grounded;
    if(prompt_&&in.inspect){
        pickup_=in.aligned?FuryPickupPhase::Lowering:FuryPickupPhase::Aligning;displaySeconds_=0;prompt_=false;
    } else if(collecting()){
        displaySeconds_+=dt;
        if(in.damaged){
            pickup_=gotCharm6_?FuryPickupPhase::Collected:FuryPickupPhase::Idle;
            draws_[index(FuryPart::Icon)].visible=false;
        } else if(pickup_==FuryPickupPhase::Aligning&&(in.aligned||displaySeconds_+1e-5f>=1)){
            pickup_=FuryPickupPhase::Lowering;displaySeconds_=0;
        } else if(pickup_==FuryPickupPhase::Lowering&&displaySeconds_+1e-5f>=.75f){
            // Get Charm precedes Tute and the pickup sound; Hero Down takes .75s.
            gotCharm6_=hasCharm_=true;pickup_=FuryPickupPhase::Display;displaySeconds_=0;++pickupSerial_;
            draws_[index(FuryPart::Shiny)].visible=draws_[index(FuryPart::Glow)].visible=false;
            start(FuryPart::Icon,"Fury Icon");
            flashSeconds_=0;flashOrigin_={in.heroPosition.x,in.heroPosition.y-.76f};
            auto& flash=effects_[1];flash={};flash.visible=true;flash.position=flashOrigin_;flash.depth=-.9f;
            flash.player.play(bank_->find("Fury Flash"),0,true);
        } else if(pickup_==FuryPickupPhase::WaitButton&&in.button){
            pickup_=FuryPickupPhase::Raising;displaySeconds_=0;draws_[index(FuryPart::Icon)].visible=false;
        } else if(pickup_==FuryPickupPhase::Display&&displaySeconds_+1e-5f>=4.5f){
            pickup_=FuryPickupPhase::WaitButton;
        } else if(pickup_==FuryPickupPhase::Raising&&displaySeconds_+1e-5f>=5.f/12){
            pickup_=FuryPickupPhase::Collected;
        }
    }
}
}
