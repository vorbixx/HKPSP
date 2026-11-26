#include "hero_effects.hpp"
#include "decor_motion.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
static_assert(sizeof(HeroEffects)<=64u*1024u,"hero effects ordinary-heap budget");
namespace {
constexpr unsigned owners[]={4217,6465,7661,8103,8243,3992};
constexpr unsigned flingOwners[]={4359,5629};
float sample(const HeroEffectConfig& c,unsigned offset,float t)
{
    const float x=std::clamp(t,0.f,1.f)*7;const unsigned i=std::min(6u,unsigned(x));
    return c.v[offset+i]+(c.v[offset+i+1]-c.v[offset+i])*(x-i);
}
Vec2 project(const std::array<float,86>& v,unsigned base,float x,float y,float z)
{return {v[base]*x+v[base+3]*y+v[base+6]*z,v[base+1]*x+v[base+4]*y+v[base+7]*z};}
float flashCurve(const std::array<std::array<float,4>,3>& keys,float age)
{
    if(age<=keys.front()[0])return keys.front()[1];
    for(unsigned i=1;i<keys.size();++i)if(age<keys[i][0]){
        const auto& a=keys[i-1];const auto& b=keys[i];
        const float dt=b[0]-a[0],t=(age-a[0])/dt,t2=t*t,t3=t2*t;
        return (2*t3-3*t2+1)*a[1]+(t3-2*t2+t)*dt*a[3]+
               (-2*t3+3*t2)*b[1]+(t3-t2)*dt*b[2];
    }
    return keys.back()[1];
}
}
bool HeroEffects::load(const char* path,const AnimationBank& bank)
{
    ready_=false;reset();FILE* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4];unsigned count=0;
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"HFX4",4)&&std::fread(&count,4,1,f)==1&&count==emitterCount;
    for(unsigned i=0;ok&&i<emitterCount;++i){
        auto& c=configs_[i];unsigned fields[4]{};
        ok=std::fread(fields,4,4,f)==4&&std::fread(c.v.data(),4,c.v.size(),f)==c.v.size();
        c.owner=fields[0];c.shape=fields[1];c.tiles=fields[2];c.flags=fields[3];
        const bool fling=i>=6;
        ok=ok&&c.owner==(fling?flingOwners[i-6]:owners[i])&&c.tiles>0&&c.tiles<=(fling?7u:3u)&&
            (fling?(c.flags==4&&c.shape==0):(c.flags<=3&&(c.shape==4||c.shape==5||c.shape==10||c.shape==12)));
        for(float n:c.v)ok=ok&&std::isfinite(n);
        ok=ok&&c.v[0]>0&&c.v[0]<=2&&c.v[1]>=0&&c.v[2]>0&&c.v[3]>=c.v[2]&&c.v[3]<=5;
        if(fling)ok=ok&&c.v[49]>0&&c.v[0]==c.v[2]&&c.v[2]==c.v[3]&&
            c.v[4]>=0&&c.v[5]>=c.v[4]&&c.v[6]>0&&c.v[8]<=c.v[9]&&c.v[84]<=c.v[85]&&
            c.v[82]>=1&&c.v[83]>=c.v[82]&&c.v[83]<=3&&
            std::floor(c.v[82])==c.v[82]&&std::floor(c.v[83])==c.v[83];
        for(unsigned t=0;ok&&t<c.tiles;++t){
            char name[48];std::snprintf(name,sizeof(name),"Hero FX %u %u",c.owner,t);
            clips_[i][t]=bank.find(name);ok=clips_[i][t]!=nullptr;
        }
    }
    ok=ok&&std::fread(flashConfig_.data(),4,flashConfig_.size(),f)==flashConfig_.size();
    for(float n:flashConfig_)ok=ok&&std::isfinite(n);
    for(unsigned c=0;c<flashCurves_.size();++c){
        float previous=-1;
        for(auto& key:flashCurves_[c]){
            ok=ok&&std::fread(key.data(),4,key.size(),f)==key.size();
            for(float n:key)ok=ok&&std::isfinite(n);
            ok=ok&&key[0]>previous&&key[0]<=flashConfig_[4]&&key[1]>=0&&key[1]<=(c==2?1.f:10.f);
            previous=key[0];
        }
        ok=ok&&flashCurves_[c].front()[0]==0;
    }
    flashClip_=bank.find("Hero Damage Flash");
    ok=ok&&flashClip_&&flashConfig_[2]>0&&flashConfig_[3]>0&&flashConfig_[4]>0&&flashConfig_[4]<=1&&flashConfig_[5]<=flashConfig_[6];
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);ready_=ok;return ok;
}
void HeroEffects::reset()
{
    for(auto& p:particles_)p.active=false;
    flash_={};flashRemaining_=0;
    emissions_={};emitted_={};flung_={};jump_=dash_=damage_=dropped_=live_=0;seed_=0x484b4658u;
}
float HeroEffects::random(float low,float high)
{seed_=seed_*1664525u+1013904223u;return low+(high-low)*(float(seed_>>8)/16777216.f);}
void HeroEffects::start(unsigned i,const Hero& hero)
{
    auto& e=emissions_[i];e={};e.active=true;e.origin=hero.position;
    // HeroDash assigns backDashPrefab scale.x = -Knight.scale.x.
    e.mirror=i==5?(hero.facingRight?1.f:-1.f):i>=2?(hero.facingRight?-1.f:1.f):1.f;
}
void HeroEffects::spawn(unsigned index,Vec2 origin,float mirror)
{
    auto it=std::find_if(particles_.begin(),particles_.end(),[](const auto& p){return !p.active;});
    if(it==particles_.end()){++dropped_;return;}
    auto& p=*it;p={};p.active=true;p.emitter=index;
    const auto& c=configs_[index];const auto& v=c.v;
    if(c.flags&4){
        p.position=origin;p.life=v[2];p.size=v[6];
        const float speed=random(v[4],v[5]);p.angle=random(v[mirror<0?84:8],v[mirror<0?85:9]);
        p.velocity={std::cos(p.angle)*speed,std::sin(p.angle)*speed};
        p.clip=clips_[index][0];p.history.reset(p.position);++flung_[index-6];++live_;return;
    }
    const float theta=random(0,v[13]),r=std::sqrt(random(0,1)),x=std::cos(theta)*r,y=std::sin(theta)*r;
    float sx=x*v[11],sy=y*v[11],sz=0,dx=0,dy=0,dz=1;
    if(c.shape==10){dx=std::cos(theta);dy=std::sin(theta);dz=0;}
    else if(c.shape==5){sx=random(-.5f,.5f)*v[82];sy=random(-.5f,.5f)*v[83];sz=random(-.5f,.5f)*v[84];}
    else if(c.shape==12){
        // UnityEngine.ParticleSystemShapeType.SingleSidedEdge (copied assembly).
        sx=random(-v[11],v[11]);sy=0;dx=0;dy=1;dz=0;
    }
    else{
        const float norm=std::sqrt(1+v[12]*v[12]*r*r);dx=x*v[12]/norm;dy=y*v[12]/norm;dz=1/norm;
    }
    const auto offset=project(v,18,sx,sy,sz);
    p.position={origin.x+mirror*(v[16]+offset.x),origin.y+v[17]+offset.y};
    const float speed=random(v[4],v[5]);p.velocity=project(v,27,dx*speed,dy*speed,dz*speed);
    p.velocityZ=(v[29]*dx+v[32]*dy+v[35]*dz)*speed;
    p.velocity.x*=mirror;
    for(unsigned k=0;k<2;++k){
        const unsigned b=36+k*6;const float a=random(v[b],v[b+1]),bY=random(v[b+2],v[b+3]),bZ=random(v[b+4],v[b+5]);
        Vec2 result=c.flags&(1u<<k)?project(v,27,a,bY,bZ):Vec2{a,bY};
        const float resultZ=c.flags&(1u<<k)?v[29]*a+v[32]*bY+v[35]*bZ:bZ;
        if(c.flags&(1u<<k))result.x*=mirror;
        if(k){p.module=result;p.moduleZ=resultZ;}else{p.force=result;p.forceZ=resultZ;}
    }
    p.force.y-=9.81f*v[10];p.life=random(v[2],v[3]);p.size=random(v[6],v[7]);
    p.angle=random(v[8],v[9]);p.spin=random(v[48],v[49]);p.mix=random(0,1);
    p.clip=clips_[index][std::min(c.tiles-1,unsigned(random(0,float(c.tiles))))];
    p.history.reset(p.position);++emitted_[index];++live_;
}
void HeroEffects::update(float seconds,const Hero& hero)
{
    if(!ready_||!std::isfinite(seconds)||seconds<0)return;
    if(flash_.active){
        flash_.age+=seconds;
        // DeactivateAfterDelay checks the timer before subtracting this Update.
        if(flashRemaining_>0)flashRemaining_-=seconds;else flash_.active=false;
    }
    if(jump_!=hero.groundJumpSerial){jump_=hero.groundJumpSerial;start(0,hero);start(1,hero);}
    if(dash_!=hero.groundDashSerial){dash_=hero.groundDashSerial;start(5,hero);}
    if(damage_!=hero.redDamageSerial){
        damage_=hero.redDamageSerial;start(2,hero);start(3,hero);start(4,hero);
        const float mirror=hero.facingRight?-1.f:1.f;
        flash_={flashClip_,{hero.position.x+mirror*flashConfig_[0],hero.position.y+flashConfig_[1]},
                {mirror*flashConfig_[2],flashConfig_[3]},random(flashConfig_[5],flashConfig_[6]),seconds,1,true};
        flashRemaining_=flashConfig_[4];
        // FlingObjectsFromGlobalPool launches in world space, with inclusive counts.
        for(unsigned batch=0;batch<4;++batch){
            const unsigned i=6+batch%2;
            const auto& v=configs_[i].v;
            const unsigned count=std::min(unsigned(v[83]),unsigned(random(v[82],v[83]+1)));
            for(unsigned n=0;n<count;++n)spawn(i,hero.position,batch<2?1.f:-1.f);
        }
    }
    if(flash_.active){
        flash_.scale={std::copysign(flashCurve(flashCurves_[0],flash_.age),flash_.scale.x),
                      flashCurve(flashCurves_[1],flash_.age)};
        flash_.alpha=std::clamp(flashCurve(flashCurves_[2],flash_.age),0.f,1.f);
    }
    if(seconds==0)return;
    if(!live_&&std::none_of(emissions_.begin(),emissions_.end(),[](const auto& e){return e.active;}))return;
    for(unsigned i=0;i<6;++i){auto& e=emissions_[i];if(!e.active)continue;
        const auto& c=configs_[i];const auto& v=c.v;
        if(i==0&&e.age>.1f&&hero.velocity.y<=0){e.active=false;continue;}
        const float before=std::clamp(e.age-v[1],0.f,v[0]);e.age+=seconds;
        const float after=std::clamp(e.age-v[1],0.f,v[0]);
        e.remainder+=(after-before)*sample(c,74,(before+after)*.5f/v[0]);
        Vec2 origin=e.origin;
        if(i==0)origin={hero.position.x,hero.position.y-1.5f-v[17]};
        else if(i>=2&&i<=4)origin=hero.position;
        const float mirror=i>=2&&i<=4?(hero.facingRight?-1.f:1.f):e.mirror;
        while(e.remainder>=.99999f){spawn(i,origin,mirror);e.remainder-=1;}
        if(e.age>=v[1]+v[0])e.active=false;
    }
    for(auto& p:particles_)if(p.active){
        PresentationPosition::Step step(p.history,p.position);
        p.age+=seconds;if(p.age>=p.life){p.active=false;--live_;continue;}
        const auto& v=configs_[p.emitter].v;
        if(configs_[p.emitter].flags&4){
            p.position.x+=p.velocity.x*seconds;p.position.y+=p.velocity.y*seconds;
            p.clip=clips_[p.emitter][std::min(configs_[p.emitter].tiles-1,unsigned(p.age*v[49]))];continue;
        }
        std::array<float,3> position{p.position.x,p.position.y,0},base{p.velocity.x,p.velocity.y,p.velocityZ};
        decorMotionStep(position,base,{p.module.x,p.module.y,p.moduleZ},
            {p.force.x*seconds,p.force.y*seconds,p.forceZ*seconds},{},seconds,v[15],decorRetention(v[14],seconds));
        p.position={position[0],position[1]};p.velocity={base[0],base[1]};p.velocityZ=base[2];
        p.angle+=p.spin*seconds;
    }
}
unsigned HeroEffects::activeCount() const
{return live_;}
float HeroEffects::drawSize(const HeroEffectParticle& p) const
{const auto& c=configs_[p.emitter];return p.size*(sample(c,50,p.age/p.life)*(1-p.mix)+sample(c,58,p.age/p.life)*p.mix);}
float HeroEffects::opacity(const HeroEffectParticle& p) const
{return std::clamp(sample(configs_[p.emitter],66,p.age/p.life),0.f,1.f);}
}
