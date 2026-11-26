#include "room_ambient.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace hk3ds {
void TabletLight::reset() {
    alpha_=from_=elapsed_=hum_=humElapsed_=0;
    close_=transition_=humTransition_=humStopped_=false;wakes_=inspections_=0;
}
void TabletLight::update(float dt,Vec2 hero,float heroDepth) {
    if(!std::isfinite(dt)||dt<=0)return;
    const float x=hero.x-config.position.x,y=hero.y-config.position.y,z=heroDepth-config.depth;
    const float distance2=x*x+y*y+z*z;
    const bool next=close_?distance2<=config.exit*config.exit:distance2<config.enter*config.enter;
    if(next!=close_){
        close_=next;from_=alpha_;elapsed_=humElapsed_=0;transition_=humTransition_=true;
        if(close_)++wakes_;
    }
    if(transition_){
        elapsed_+=dt;const float t=std::min(1.f,elapsed_/(close_?config.upTime:config.downTime));
        alpha_=from_+((close_?1.f:0.f)-from_)*t;transition_=t<1;
    }
    if(humTransition_){
        // FadeAudio uses its authored startVolume on every state entry.
        humElapsed_+=dt;const float t=std::min(1.f,humElapsed_/(close_?config.humUp:config.humDown));
        hum_=config.humGain*(close_?t:1-t);humTransition_=t<1;
    }
}
float GlowBug::uniform(float low,float high) {
    // Approximation: deterministic xorshift replaces Unity's shared random stream.
    random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
    return low+(high-low)*float(random_>>8)/16777216.f;
}
void GlowBug::bind(Vec2 origin,BuzzSettings settings,unsigned seed) {
    origin_=position_=origin;velocity_=acceleration_={};settings_=settings;
    random_=seed?seed:1;wait_=0;history_.reset(position_);
    buzz(0); // IdleBuzz.OnEnter calls DoBuzz before the first physics step.
}
void GlowBug::buzz(float dt) {
    const auto reverse=[&](float p,float start,float& velocity,float& acceleration){
        if((p<start-settings_.range&&velocity<0)||(p>start+settings_.range&&velocity>0)){
            acceleration=(p<start?1.f:-1.f)*settings_.acceleration/2000.f;
            velocity/=1.125f;wait_=uniform(settings_.waitMin,settings_.waitMax);
        }
    };
    reverse(position_.y,origin_.y,velocity_.y,acceleration_.y);
    reverse(position_.x,origin_.x,velocity_.x,acceleration_.x);
    if(wait_<=0){
        const auto acceleration=[&](float p,float start){
            return uniform(p>start+settings_.range?-settings_.acceleration:
                           p<start-settings_.range?0.f:-settings_.acceleration,
                           p>start+settings_.range?0.f:settings_.acceleration)/2000.f;
        };
        acceleration_.y=acceleration(position_.y,origin_.y);
        acceleration_.x=acceleration(position_.x,origin_.x);
        wait_=uniform(settings_.waitMin,settings_.waitMax);
    }
    wait_-=dt;
    velocity_.x=std::clamp(velocity_.x+acceleration_.x,-settings_.speed,settings_.speed);
    velocity_.y=std::clamp(velocity_.y+acceleration_.y,-settings_.speed,settings_.speed);
}
void GlowBug::fixedUpdate(float dt) {
    if(!std::isfinite(dt)||dt<=0)return;
    PresentationPosition::Step step(history_,position_);
    buzz(dt);position_.x+=velocity_.x*dt;position_.y+=velocity_.y*dt;
}
bool RoomAmbient::bind(RoomVisualBank& bank,const char* path) {
    if(!path)return false;
    auto* f=std::fopen(path,"rb");if(!f)return false;
    auto storage=std::make_unique<RoomAmbient>();auto& next=*storage;
    char magic[4]{};unsigned h[5]{};
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"HKA1",4)&&
        std::fread(h,4,5,f)==5&&h[0]==tabletCount&&h[1]==bugCount&&h[2]==visualCount&&
        h[3]==bank.atlas.sprites.size()&&h[4]==bank.draws.size();
    const auto floats=[&](float* values,unsigned count){
        if(std::fread(values,4,count,f)!=count)return false;
        for(unsigned i=0;i<count;++i)if(!std::isfinite(values[i])||std::fabs(values[i])>100000)return false;
        return true;
    };
    for(auto& tablet:next.tablets_){
        unsigned ids[3]{};float v[11]{};
        ok=ok&&std::fread(ids,4,3,f)==3&&floats(v,11);
        auto& c=tablet.config;
        c={ids[0],ids[1],ids[2],{v[0],v[1]},v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10]};
        ok=ok&&ids[0]&&ids[1]&&ids[2]&&v[3]>0&&v[4]>v[3]&&v[5]>0&&v[6]>0&&
            v[7]>=0&&v[7]<=1&&v[8]>=0&&v[8]<=1&&v[9]>0&&v[10]>0;
        for(unsigned owner:{c.lit,c.light})ok=ok&&std::any_of(bank.draws.begin(),bank.draws.end(),[&](const RoomDraw& d){return d.owner==owner;});
    }
    for(auto& c:next.config_){
        float v[7]{};ok=ok&&std::fread(&c.owner,4,1,f)==1&&floats(v,7);
        c.position={v[0],v[1]};c.settings={v[2],v[3],v[4],v[5],v[6]};
        ok=ok&&c.owner&&v[2]>0&&v[3]>=v[2]&&v[4]>0&&v[4]<=2&&v[5]>0&&v[5]<=20&&v[6]>0&&v[6]<=1;
    }
    for(auto& v:next.visuals_){
        unsigned ids[10]{};ok=ok&&std::fread(ids,4,10,f)==10&&floats(&v.fps,1);
        v.owner=ids[0];v.draw=ids[2];v.count=ids[3];
        const auto bug=std::find_if(next.config_.begin(),next.config_.end(),[&](const BugConfig& c){return c.owner==ids[1];});
        ok=ok&&bug!=next.config_.end()&&v.draw<bank.draws.size()&&v.count>0&&v.count<=6&&v.fps>0&&v.fps<=120;
        if(!ok)break;
        v.bug=unsigned(bug-next.config_.begin());v.base=bank.draws[v.draw];ok=v.base.owner==v.owner;
        for(unsigned i=0;i<6;++i){v.frames[i]=ids[4+i];ok=ok&&ids[4+i]<bank.atlas.sprites.size();}
    }
    unsigned ph[3]{};
    ok=ok&&std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"HKP2",4)&&
        std::fread(ph,4,3,f)==3&&ph[0]==6&&ph[1]==2&&ph[2]==particleCapacity;
    for(auto& e:next.emitters_){
        unsigned ids[4]{};
        ok=ok&&std::fread(ids,4,4,f)==4&&floats(e.v.data(),e.v.size())&&floats(e.keys.data(),e.keys.size());
        e.owner=ids[0];e.tablet=ids[1];e.rim=ids[2];e.group=ids[3];
        ok=ok&&e.owner&&e.tablet<tabletCount&&e.rim<=1&&e.group<2&&e.v[7]>0&&e.v[8]>=e.v[7]&&
            e.v[8]<=2&&e.v[9]>0&&e.v[10]>=e.v[9]&&e.v[10]<=1.2f&&e.v[11]>=0&&e.v[11]<=1&&
            e.v[12]>=0&&e.v[13]>=e.v[12]&&e.v[13]<=.75f&&e.v[14]==(e.rim?500.f:100.f)&&
            e.v[15]==.2f&&e.v[16]==1.38f&&e.v[17]==.11f;
        for(unsigned side:{0u,16u}){
            ok=ok&&e.keys[side]==0&&e.keys[side+12]==1;
            for(unsigned k=0;k<4;++k){
                ok=ok&&e.keys[side+k*4+1]>=0&&e.keys[side+k*4+1]<=1;
                if(k)ok=ok&&e.keys[side+k*4]>=e.keys[side+(k-1)*4];
            }
        }
    }
    unsigned total=0;
    for(unsigned group=0;group<2;++group){
        unsigned ids[3]{};ok=ok&&std::fread(ids,4,3,f)==3;
        auto& g=next.particleGroups_[group];g={ids[0],ids[1],ids[2]};total+=g.count;
        ok=ok&&g.count==(group?300u:200u)&&g.draw<bank.draws.size()&&g.count<=bank.draws.size()-g.draw&&
            g.sprite<bank.atlas.sprites.size();
        if(ok)for(unsigned i=0;i<g.count;++i){
            const auto& d=bank.draws[g.draw+i];
            ok=ok&&d.owner==0xfffffe00u+group&&d.sprite==g.sprite&&roomMaterial(d)==RoomMaterial::Screen;
            for(const auto& e:next.emitters_)if(e.group==group)ok=ok&&d.values[6]==e.v[2];
        }
    }
    ok=ok&&total==particleCapacity&&std::fgetc(f)==EOF;std::fclose(f);if(!ok)return false;
    // The shared capacity proof requires that two tablets cannot stay Close
    // at the same time, even across the source enter/exit hysteresis.
    for(unsigned i=0;i<tabletCount;++i)for(unsigned j=i+1;j<tabletCount;++j){
        const auto a=next.tablets_[i].config,b=next.tablets_[j].config;
        if(std::hypot(a.position.x-b.position.x,a.position.y-b.position.y)<=a.exit+b.exit)return false;
    }
    for(unsigned group=0;group<2;++group){
        RoomCameraBounds envelope{1e9f,1e9f,-1e9f,-1e9f};
        const auto& g=next.particleGroups_[group];
        for(const auto& e:next.emitters_)if(e.group==group){
            // Hermite segment bounds: endpoint magnitudes plus |slope|*dt.
            // This deliberately includes overshoot between authored keys.
            float curveMax=1;
            for(unsigned side:{0u,16u})for(unsigned k=0;k<3;++k){
                const auto* a=e.keys.data()+side+k*4;const auto* b=a+4;
                curveMax=std::max(curveMax,std::max(a[1],b[1])+(b[0]-a[0])*(std::fabs(a[3])+std::fabs(b[2])));
            }
            const float radiusX=e.v[16]*std::hypot(e.v[3],e.v[5]);
            const float radiusY=e.v[16]*std::hypot(e.v[4],e.v[6]);
            for(float x:{e.v[0]-radiusX,e.v[0]+radiusX})for(float y:{e.v[1]-radiusY,e.v[1]+radiusY+.5f*e.v[13]*e.v[8]*e.v[8]}){
                auto d=bank.draws[g.draw];d.values[0]=d.values[4]=e.v[10]*curveMax;d.values[2]=x;d.values[5]=y;
                if(!roomEffectSplitSupported(bank.atlas.sprites[g.sprite],d,bank.blurSplitDepth))return false;
                const auto b=roomCameraBounds(bank.atlas.sprites[g.sprite],d);
                envelope.left=std::min(envelope.left,b.left);envelope.bottom=std::min(envelope.bottom,b.bottom);
                envelope.right=std::max(envelope.right,b.right);envelope.top=std::max(envelope.top,b.top);
            }
        }
        for(unsigned i=0;i<g.count;++i)bank.cameraBounds[g.draw+i]=envelope;
    }
    // Admission bounds include every animation frame and the complete roaming
    // range plus speed/damping overshoot (<.4 units at the source .02s step).
    for(const auto& v:next.visuals_){
        RoomCameraBounds bounds{1e9f,1e9f,-1e9f,-1e9f};
        for(unsigned i=0;i<v.count;++i)for(float dx:{-2.f,2.f})for(float dy:{-2.f,2.f}){
            auto d=v.base;d.values[2]+=dx;d.values[5]+=dy;
            if(!roomEffectSplitSupported(bank.atlas.sprites[v.frames[i]],d,bank.blurSplitDepth))return false;
            auto b=roomCameraBounds(bank.atlas.sprites[v.frames[i]],d);
            bounds.left=std::min(bounds.left,b.left);bounds.bottom=std::min(bounds.bottom,b.bottom);
            bounds.right=std::max(bounds.right,b.right);bounds.top=std::max(bounds.top,b.top);
        }
        bank.cameraBounds[v.draw]=bounds;
    }
    next.bank_=&bank;*this=next;reset();return true;
}
void RoomAmbient::reset(unsigned seed) {
    for(auto& t:tablets_)t.reset();
    for(unsigned i=0;i<bugCount;++i)bugs_[i].bind(config_[i].position,config_[i].settings,seed^(config_[i].owner*2654435761u));
    for(auto& p:particles_)p.alive=false;
    for(auto& e:emitters_){e.credit=0;e.remaining=0;e.delay=e.rim?0:e.v[17];e.births=0;}
    particleRandom_=seed?seed:1;particleDrops_=0;particleStep_=0;
    if(bank_)present({},0);
}
void RoomAmbient::fixedUpdate(float dt,Vec2 hero) {
    if(!bank_||!std::isfinite(dt)||dt<=0||dt>.1f)return;
    std::array<bool,tabletCount> entered{};
    for(unsigned i=0;i<tabletCount;++i){const bool close=tablets_[i].close();tablets_[i].update(dt,hero);entered[i]=!close&&tablets_[i].close();}
    updateParticles(dt,entered);
    for(auto& b:bugs_)b.fixedUpdate(dt);
}
float RoomAmbient::alpha(unsigned owner) const {
    if(bank_)for(const auto& t:tablets_){
        if(owner==t.config.lit)return t.alpha();
        if(owner==t.config.light)return t.alpha()*t.config.lightAlpha;
    }
    return 1;
}
void RoomAmbient::present(PresentationTime time,float seconds) {
    if(!bank_||!std::isfinite(seconds))return;
    for(const auto& v:visuals_){
        auto& d=bank_->draws[v.draw];d=v.base;
        const auto p=bugs_[v.bug].renderPosition(time),origin=bugs_[v.bug].origin();
        d.values[2]+=p.x-origin.x;d.values[5]+=p.y-origin.y;
        const unsigned frame=unsigned(std::fmod(std::max(0.f,seconds),v.count/v.fps)*v.fps)%v.count;
        d.sprite=v.frames[frame];
    }
    unsigned slot=0;
    for(const auto& g:particleGroups_)for(unsigned i=0;i<g.count;++i,++slot){
        auto& d=bank_->draws[g.draw+i];const auto& p=particles_[slot];
        d.values[10]=0;d.values[0]=d.values[4]=0;
        if(!p.alive){d.values[2]=d.values[5]=0;continue;}
        const auto& e=emitters_[p.emitter];
        const float age=std::max(0.f,p.age+(std::clamp(time.alpha,-1.f,1.f)-1)*particleStep_);
        if(age>=p.life)continue;
        const auto sample=[&](unsigned side){
            const float t=age/p.life;const float* a=e.keys.data()+side;
            for(unsigned k=1;k<4;++k){const float* b=e.keys.data()+side+k*4;
                if(t<=b[0]&&b[0]>a[0]){const float dt=b[0]-a[0],u=(t-a[0])/dt,u2=u*u,u3=u2*u;
                    return (2*u3-3*u2+1)*a[1]+(u3-2*u2+u)*dt*a[3]+(-2*u3+3*u2)*b[1]+(u3-u2)*dt*b[2];}
                a=b;
            }return a[1];
        };
        const float low=sample(0),size=p.size*(low+(sample(16)-low)*p.blend);
        const auto position=p.history.sample(p.position,time);
        d.values[0]=d.values[4]=std::max(0.f,size);d.values[2]=position.x;d.values[5]=position.y;d.values[10]=e.v[11];
    }
}

unsigned RoomAmbient::particleCount(unsigned owner) const {
    unsigned count=0;for(const auto& p:particles_)if(p.alive&&(!owner||emitters_[p.emitter].owner==owner))++count;return count;
}
unsigned RoomAmbient::particleBirths(unsigned owner) const {
    for(const auto& e:emitters_)if(e.owner==owner)return e.births;
    return 0;
}
float RoomAmbient::particleRandom() {
    particleRandom_^=particleRandom_<<13;particleRandom_^=particleRandom_>>17;particleRandom_^=particleRandom_<<5;
    return float(particleRandom_>>8)*(1.f/16777216.f);
}
void RoomAmbient::spawnParticle(unsigned index,float age) {
    auto& e=emitters_[index];const unsigned begin=e.group?200:0,end=e.group?500:200;
    for(unsigned i=begin;i<end;++i)if(!particles_[i].alive){
        auto& p=particles_[i];p=Particle{};p.alive=true;p.emitter=index;p.age=age;
        const auto range=[&](float a,float b){return a+(b-a)*particleRandom();};
        p.life=range(e.v[7],e.v[8]);p.size=range(e.v[9],e.v[10]);p.force=range(e.v[12],e.v[13]);p.blend=particleRandom();
        const float angle=particleRandom()*6.28318530718f,radius=e.v[16]*(e.rim?1:std::sqrt(particleRandom()));
        const float x=std::cos(angle)*radius,y=std::sin(angle)*radius;
        p.origin={e.v[0]+e.v[3]*x+e.v[5]*y,e.v[1]+e.v[4]*x+e.v[6]*y};
        p.position=p.origin;p.position.y+=.5f*p.force*age*age;p.history.reset(p.position);++e.births;return;
    }
    ++particleDrops_;assert(false&&"source tablet particle overlap exceeded admitted fixed capacity");
}
void RoomAmbient::updateParticles(float dt,const std::array<bool,tabletCount>& entered) {
    particleStep_=dt;
    for(auto& p:particles_)if(p.alive){
        PresentationPosition::Step history(p.history,p.position);p.age+=dt;
        if(p.age+1e-6f>=p.life){p.alive=false;continue;}
        // Approximation: analytic constant world acceleration, rather than
        // reconstructing Unity's particle integration. Spawn shape is scaled;
        // billboard size and world force are not (source scalingMode=Shape).
        p.position=p.origin;p.position.y+=.5f*p.force*p.age*p.age;
    }
    for(unsigned i=0;i<emitters_.size();++i){
        auto& e=emitters_[i];
        if(e.rim&&entered[e.tablet]&&e.remaining<=0&&particleCount(e.owner)==0){e.remaining=e.v[15];e.delay=e.v[17];e.credit=0;}
        // Play's delay precedes the nonloop duration. Play-on-awake Glows pay
        // it once at room reset, even while Away has emission disabled.
        const float skipped=std::min(dt,e.delay);e.delay-=skipped;
        const float active=dt-skipped;
        const float span=e.rim?std::min(active,e.remaining):(tablets_[e.tablet].close()?active:0.f);
        if(span<=0)continue;
        const double before=e.credit,total=before+double(span)*e.v[14];
        const unsigned births=unsigned(std::floor(total+1e-5));e.credit=total-births;
        if(std::fabs(e.credit)<1e-5)e.credit=0;
        for(unsigned birth=0;birth<births;++birth){const float offset=skipped+float((birth+1-before)/e.v[14]);spawnParticle(i,std::max(0.f,dt-offset));}
        if(e.rim)e.remaining=std::max(0.f,e.remaining-span);
    }
}
}
