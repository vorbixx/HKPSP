#include "geo_visual.hpp"
#include "breakable.hpp"
#include "room_visual.hpp"
#include "render_memory.hpp"
#include "cosmetic_terrain.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
static_assert(sizeof(GeoChip)*GeoVisuals::chipCapacity<=geoChipsStateBudgetBytes,"Geo chip CPU pool budget");
namespace {
constexpr float pi=3.14159265358979323846f;
bool valid(const GeoChipConfig& c) {
    for(const auto&p:c.profiles)if(!p.minimum||p.maximum<p.minimum||p.maximum>32||
        !std::isfinite(p.speedMin)||!std::isfinite(p.speedMax)||!std::isfinite(p.angleMin)||!std::isfinite(p.angleMax)||
        p.speedMin<0||p.speedMax<p.speedMin||p.angleMax<p.angleMin)return false;
    const float values[]={c.variationX,c.variationY,c.scaleMin,c.scaleMax,c.blackChance,c.startZ,c.zRange,
        c.angularDamping,c.gravity,c.mass,c.width,c.height,c.offsetY,c.bounce,c.threshold,c.wait,c.shrink};
    for(float v:values)if(!std::isfinite(v)||std::abs(v)>1000)return false;
    return c.variationX>=0&&c.variationY>=0&&c.scaleMin>0&&c.scaleMax>=c.scaleMin&&c.blackChance>=0&&c.blackChance<=1&&
        c.zRange>=0&&c.angularDamping>=0&&c.gravity<=0&&c.mass>0&&c.width>0&&c.height>0&&c.bounce>=0&&c.bounce<=1&&c.threshold>0&&c.wait>0&&c.shrink>0;
}
bool valid(const GeoRockRecord& r) {
    const float values[]={r.position.x,r.position.y,r.depth,r.scale.x,r.scale.y,r.collider.left,r.collider.bottom,r.collider.right,r.collider.top};
    for(float x:values)if(!std::isfinite(x))return false;
    return r.owner&&r.hits&&(r.variant==1||r.variant==2)&&r.scale.x!=0&&r.scale.y!=0&&
        r.collider.left<r.collider.right&&r.collider.bottom<r.collider.top&&
        r.idle[0]&&r.gleam[0]&&r.broken[0]&&std::memchr(r.idle,0,48)&&std::memchr(r.gleam,0,48)&&std::memchr(r.broken,0,48);
}
Rect bounds(const GeoRockRecord& r) {
    const float x1=r.collider.left*r.scale.x,x2=r.collider.right*r.scale.x;
    const float y1=r.collider.bottom*r.scale.y,y2=r.collider.top*r.scale.y;
    return {r.position.x+std::min(x1,x2),r.position.y+std::min(y1,y2),r.position.x+std::max(x1,x2),r.position.y+std::max(y1,y2)};
}
}
bool GeoRockConfig::load(const char* path) {
    FILE*f=std::fopen(path,"rb");if(!f)return false;
    GeoRockConfig c;char magic[4]{};
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"GRK1",4)&&std::fread(&c.count,4,1,f)==1&&c.count<=capacity;
    for(unsigned i=0;ok&&i<c.count;++i){auto&r=c.rocks[i];float v[9]{};
        ok=std::fread(&r.owner,4,1,f)==1&&std::fread(&r.variant,4,1,f)==1&&std::fread(&r.hits,4,1,f)==1&&std::fread(v,4,9,f)==9&&
            std::fread(r.idle,1,48,f)==48&&std::fread(r.gleam,1,48,f)==48&&std::fread(r.broken,1,48,f)==48;
        r.position={v[0],v[1]};r.depth=v[2];r.scale={v[3],v[4]};r.collider={v[5],v[6],v[7],v[8]};ok=ok&&valid(r);
        for(unsigned j=0;j<i;++j)ok=ok&&c.rocks[j].owner!=r.owner;
    }
    static_assert(sizeof(GeoChipConfig)==164,"GCH2 layout");
    ok=ok&&std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"GCH2",4)&&
        std::fread(&c.chips,1,sizeof(c.chips),f)==sizeof(c.chips)&&valid(c.chips)&&std::fgetc(f)==EOF;
    std::fclose(f);if(ok)*this=c;return ok;
}
float GeoVisuals::random(float lo,float hi) {
    random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
    return lo+(hi-lo)*static_cast<float>(random_>>8)/16777216.0f;
}
bool GeoVisuals::bind(const AnimationBank& bank,const GeoRockConfig& config,std::uint32_t cosmeticSeed) {
    const auto* chips=bank.find("Rock Chips");
    if(config.count>GeoRockConfig::capacity||!valid(config.chips)||!chips||chips->count!=8||!bank.find("Small Air")||!bank.find("Small Idle"))return false;
    for(unsigned i=0;i<config.count;++i){const auto&r=config.rocks[i];if(!valid(r)||!bank.find(r.idle)||!bank.find(r.gleam)||!bank.find(r.broken))return false;}
    bank_=&bank;config_=config;seed_=cosmeticSeed?cosmeticSeed:1;reset();return true;
}
void GeoVisuals::reset() {
    coins_={};rocks_={};for(auto& p:chips_)p={};random_=seed_;chipRandom_=seed_^0x43484950u;if(!chipRandom_)chipRandom_=1;
    if(!bank_)return;
    for(unsigned i=0;i<config_.count;++i){auto&s=rocks_[i];const auto&r=config_.rocks[i];
        s.hits=r.hits;s.timer=random(1.5f,4);s.draw.visible=true;s.draw.position=r.position;s.draw.depth=r.depth;s.draw.scale=r.scale;s.draw.player.play(bank_->find(r.idle));
    }
}
float GeoVisuals::chipRandom(float lo,float hi) {
    chipRandom_^=chipRandom_<<13;chipRandom_^=chipRandom_>>17;chipRandom_^=chipRandom_<<5;
    return lo+(hi-lo)*static_cast<float>(chipRandom_>>8)/16777216.f;
}
unsigned GeoVisuals::chipCount() const {
    unsigned n=0;for(const auto&p:chips_)n+=p.draw.visible;return n;
}
void GeoVisuals::emitChips(Vec2 origin,Strike direction,bool broken) {
    const auto&c=config_.chips;const auto&f=c.profiles[broken?3:direction==Strike::Right?0:direction==Strike::Left?1:2];
    const unsigned amount=unsigned(chipRandom(float(f.minimum),float(f.maximum+1)));
    unsigned emitted=0;
    for(auto&p:chips_){if(p.draw.visible)continue;if(emitted++==amount)break;p={};
        p.position={origin.x+chipRandom(-c.variationX,c.variationX),origin.y+chipRandom(-c.variationY,c.variationY)};
        const float speed=chipRandom(f.speedMin,f.speedMax),angle=chipRandom(f.angleMin,f.angleMax)*pi/180;
        p.velocity={speed*std::cos(angle),speed*std::sin(angle)};p.scale=chipRandom(c.scaleMin,c.scaleMax);
        const unsigned sprite=unsigned(chipRandom(0,4));const bool black=chipRandom(0,1)<c.blackChance;
        p.draw.visible=true;p.draw.position=p.position;p.draw.scale={p.scale,p.scale};
        p.draw.depth=c.startZ+chipRandom(0,c.zRange)-(black?.05f:0);
        p.draw.player.play(bank_->find("Rock Chips"),sprite+(black?4:0),true);p.history.reset(p.position);
        // DebrisParticle applies torque -vx once. Approximation: box inertia
        // and one .02 s force interval, with Box2D's per-step rotation cap;
        // the native tk2d rigidbody inertia is unavailable in serialized data.
        const float inertia=c.mass*p.scale*p.scale*(c.width*c.width+c.height*c.height)/12;
        p.spin=std::clamp(-p.velocity.x*.02f/inertia,-pi/.04f,pi/.04f);
    }
}
void GeoVisuals::updateChips(float dt,const World& world,const Vec2* camera) {
    const auto&c=config_.chips;
    for(auto&p:chips_){if(!p.draw.visible)continue;PresentationPosition::Step history(p.history,p.position);
        if(camera){const float ppu=roomFocalLength()/(38.1f+p.draw.depth);
            const float radius=p.scale*.4f;
            const bool outside=std::abs(p.position.x-camera->x)>200/ppu+radius||std::abs(p.position.y-camera->y)>120/ppu+radius;
            // Approximation: ten 60 Hz visual intervals, independent of draw rate.
            p.offscreenTime=outside?p.offscreenTime+dt:0;
            if(p.offscreenTime>=10.f/60){p.draw.visible=false;continue;}
        }
        const float scale=p.scale*std::clamp(1-(p.sleepTime-c.wait)/c.shrink,0.f,1.f);
        const Rect box={-c.width*p.scale/2,(c.offsetY-c.height/2)*p.scale,c.width*p.scale/2,(c.offsetY+c.height/2)*p.scale};
        p.velocity.y+=c.gravity*dt;
        const Vec2 incoming=p.velocity;
        const auto contacts=moveBox(p.position,p.velocity,box,dt,world);
        Vec2 normal{};
        const Vec2 beforeProjection=p.position;
        if(cosmetic_terrain::depenetrate(p.position,world)){
            const Vec2 correction{p.position.x-beforeProjection.x,p.position.y-beforeProjection.y};
            const float length=std::hypot(correction.x,correction.y);
            if(length>0)normal={correction.x/length,correction.y/length};
        }
        for(unsigned i=0;i<contacts.count;++i)if(incoming.x*contacts.surfaces[i].normal.x+incoming.y*contacts.surfaces[i].normal.y<0){normal=contacts.surfaces[i].normal;break;}
        if(normal.x==0&&normal.y==0)normal={contacts.left?1.f:contacts.right?-1.f:0,contacts.ground?1.f:contacts.roof?-1.f:0};
        const bool contact=normal.x!=0||normal.y!=0;
        const float speed=std::hypot(incoming.x,incoming.y);
        float supportDistance=0;
        bool supported=false;
        if(contacts.ground)for(float x:{0.f,box.left,box.right})
            supported=supported||rayTerrain({p.position.x+x,p.position.y},{0,-1},-box.bottom+.04f,world,supportDistance);
        // A supported sleeping body stays asleep under gravity. Testing the
        // newly integrated gravity as impact speed would wake it every step.
        p.asleep=contact&&supported&&(p.asleep||speed<=c.threshold);
        if(p.asleep){p.velocity={};p.spin=0;}
        else if(contact&&speed>c.threshold){const float dot=incoming.x*normal.x+incoming.y*normal.y;
            const float bounce=c.bounce*chipRandom(.8f,1.2f);
            p.velocity={(incoming.x-2*dot*normal.x)*bounce,(incoming.y-2*dot*normal.y)*bounce};
        }
        if(p.asleep||p.sleepTime>=c.wait)p.sleepTime+=dt;
        // Approximation: the existing non-rotating terrain box replaces native
        // rotating contacts and cached-every-fourth-FixedUpdate bounce velocity.
        p.spin/=1+c.angularDamping*dt;p.draw.rotation=std::remainder(p.draw.rotation+p.spin*dt,2*pi);
        p.draw.position=p.position;p.draw.scale={scale,scale};
        if(p.sleepTime>=c.wait+c.shrink)p.draw.visible=false;
    }
}
void GeoVisuals::update(float dt,const GeoBank& geo,const Hero& hero,const NailBank& nails,DecorEffects* effects,const World& world,const Vec2* camera) {
    if(!bank_||dt<=0||!std::isfinite(dt))return;
    updateChips(dt,world,camera);
    for(unsigned i=0;i<GeoBank::capacity;++i){auto&v=coins_[i];const auto&c=geo.coins()[i];
        if(!c.active){v.draw.visible=false;continue;}
        const bool fresh=!v.draw.visible||v.spawn!=c.spawnGeneration;
        if(fresh){v={};v.spawn=c.spawnGeneration;v.draw.player.play(bank_->find("Small Air"));}
        v.draw.visible=true;v.draw.position=c.position;v.draw.depth=c.depth;v.draw.scale={1.5f,1.5f};
        if((fresh&&c.idle)||v.collision!=c.collisionGeneration){const auto*clip=bank_->find("Small Idle");
            // Cosmetic RNG is independent of the fling/bounce stream.
            const unsigned enters=c.collisionGeneration-v.collision;
            unsigned start=0;
            for(unsigned event=0;event<std::max(1u,enters);++event)start=static_cast<unsigned>(random(0,static_cast<float>(clip->count)));
            v.draw.player.play(clip,start,true);
        }
        v.collision=c.collisionGeneration;v.draw.player.advance(dt);
    }
    for(unsigned i=0;i<config_.count;++i){auto&s=rocks_[i];const auto&r=config_.rocks[i];
        s.draw.position=r.position;
        // The FSM's final hit yields one control frame before Broken.
        if(s.phase==GeoRockPhase::BreakWait){s.phase=GeoRockPhase::Broken;s.draw.player.play(bank_->find(r.broken),0,true);emitChips(r.position,Strike::Up,true);}
        else if(s.phase==GeoRockPhase::Recoil||s.phase==GeoRockPhase::ReturnRecoil){
            float remaining=dt;
            while(remaining>0&&(s.phase==GeoRockPhase::Recoil||s.phase==GeoRockPhase::ReturnRecoil)){
                const float step=std::min(remaining,s.timer);remaining-=step;s.timer-=step;s.jitterClock+=step;
                // ObjectJitter.DoTranslate samples relative to the position captured
                // on state entry (allowMovement=false), not relative to last sample.
                // approximation: independent 50 Hz cosmetic clock, rather than
                // reconstructing Unity's FixedUpdate phase or consuming gameplay RNG.
                while(s.jitterClock>=.019999f){s.jitterClock=std::max(0.f,s.jitterClock-.02f);s.recoil={s.recoilOrigin.x+random(-.05f,.05f),s.recoilOrigin.y+random(-.05f,.05f)};}
                if(s.timer<=0){
                    if(s.phase==GeoRockPhase::Recoil){s.phase=GeoRockPhase::ReturnRecoil;s.timer=.1f;s.jitterClock=0;s.recoilOrigin=s.recoil;}
                    else {s.phase=GeoRockPhase::Idle;s.timer=random(1.5f,4);s.recoil={};}
                }
            }
            s.draw.position.x+=s.recoil.x;s.draw.position.y+=s.recoil.y;
        }
        else if(s.phase==GeoRockPhase::Idle){s.timer-=dt;if(s.timer<=0){s.phase=GeoRockPhase::Gleam;s.draw.player.play(bank_->find(r.gleam),0,true);}}
        s.draw.player.advance(dt);
        if(s.phase==GeoRockPhase::Gleam&&s.draw.player.complete){s.phase=GeoRockPhase::Idle;s.timer=random(1.5f,4);s.draw.player.play(bank_->find(r.idle),0,true);}
        if((s.phase==GeoRockPhase::Idle||s.phase==GeoRockPhase::Gleam)&&s.hits&&hero.nail.hitbox&&s.lastAttack!=hero.nail.serial&&nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],bounds(r))){
            s.lastAttack=hero.nail.serial;--s.hits;
            if(effects)effects->geoRockHit(r.position,r.depth,hero.nail.direction,s.hits==0);
            if(!s.hits)s.phase=GeoRockPhase::BreakWait;
            else {emitChips(r.position,hero.nail.direction,false);s.phase=GeoRockPhase::Recoil;s.timer=.1f;s.jitterClock=0;s.recoil={};s.recoilOrigin={};s.draw.player.play(bank_->find(r.idle),0,true);}
        }
    }
}
}
