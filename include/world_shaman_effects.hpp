#pragma once
#include "hero_effects.hpp"
#include "room_visual.hpp"
#include <cstdio>
#include <cstring>
namespace hk3ds {
// Original non-colliding billboard systems; visual state never changes the gate outcome.
class ShamanPillarDust {
    friend struct SimulationStateHash;
public:
    // Keep the two actor draws and all 22 source frame-admission records
    // alongside the bounded particle pool without growing the draw vector.
    static constexpr unsigned capacity=80,frameCount=22,drawCapacity=2+frameCount+capacity;
    struct Emitter {unsigned owner=0,shape=0,first=0,frames=0,flags=0,capacity=0;std::array<float,89> v{};};
    bool load(const char* directory,const AnimationBank& art){
        char path[512];std::snprintf(path,sizeof(path),"%s/shaman-pillar-fx.bin",directory);
        auto* f=std::fopen(path,"rb");if(!f)return false;
        char magic[4];unsigned count=0;std::array<Emitter,6> next{};
        bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"SPF1",4)&&std::fread(&count,4,1,f)==1&&count==6;
        for(auto& e:next){unsigned fields[6]{};ok=ok&&std::fread(fields,4,6,f)==6&&std::fread(e.v.data(),4,89,f)==89;
            e.owner=fields[0];e.shape=fields[1];e.first=fields[2];e.frames=fields[3];e.flags=fields[4];e.capacity=fields[5];}
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);return ok&&bind(next,art.sprites.size());
    }
    bool bind(const std::array<Emitter,6>& next,unsigned sprites){
        constexpr unsigned owners[]{122,119,123,118,121,965},caps[]{20,17,22,4,10,6};
        for(unsigned i=0;i<6;++i){const auto& e=next[i];const auto& v=e.v;
            if(e.owner!=owners[i]||e.capacity!=caps[i]||e.shape!=(i==3?12u:5u)||e.flags!=(i==5?16u:0u)||
               e.frames!=(i==3?14u:i==5?4u:1u)||e.first>sprites||e.frames>sprites-e.first||
               !std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);})||
               v[0]<=0||v[0]>1.6f||v[1]<0||v[1]>.5f||v[2]<=0||v[3]<v[2]||v[3]>2.1f||v[6]<=0||v[7]<v[6]||v[74]<=0||v[74]>100)return false;
        }
        emitters_=next;particles_.resize(capacity);ready_=true;reset();return true;
    }
    void reset(unsigned seed=1){for(auto& p:particles_)p={};emissions_={};emitted_={};seed_=seed;dropped_=0;busy_=false;}
    void start(unsigned i){if(!ready_||i>=6||emissions_[i].started)return;emissions_[i].started=emissions_[i].active=true;busy_=true;}
    void update(float dt){
        if(!ready_||!busy_||!std::isfinite(dt)||dt<=0)return;
        for(auto& p:particles_)if(p.active){
            PresentationPosition::Step step(p.history,p.position);p.age+=dt;
            if(p.age>=p.life){p.active=false;continue;}
            p.velocity.y-=9.81f*emitters_[p.emitter].v[10]*dt;
            p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
            p.angle+=p.spin*((emitters_[p.emitter].flags&16)?std::clamp(std::hypot(p.velocity.x,p.velocity.y),0.f,1.f):1.f)*dt;
        }
        for(unsigned i=0;i<6;++i){auto& a=emissions_[i];if(!a.active)continue;const auto& v=emitters_[i].v;
            const float before=std::clamp(a.age-v[1],0.f,v[0]);a.age+=dt;
            a.remainder+=(std::clamp(a.age-v[1],0.f,v[0])-before)*v[74];
            while(a.remainder>=.9999f&&a.born<emitters_[i].capacity){spawn(i);a.remainder-=1;++a.born;}
            if(a.age>=v[0]+v[1])a.active=false;
        }
        busy_=activeCount()!=0||std::any_of(emissions_.begin(),emissions_.end(),[](const auto& a){return a.active;});
    }
    void present(RoomVisualBank& bank,PresentationTime time)const{
        if(!ready_)return;
        for(const auto& p:particles_)if(p.active){const auto& e=emitters_[p.emitter];const float t=p.age/p.life;
            const float size=p.size*(sample(e,50,t)*(1-p.mix)+sample(e,58,t)*p.mix),c=std::cos(p.angle)*size,s=std::sin(p.angle)*size;
            const auto pos=p.renderPosition(time);RoomDraw d;d.owner=e.owner;
            d.sprite=e.first+((e.flags&16)?p.row:std::min(e.frames-1,unsigned(t*e.frames)));
            d.values={c,-s,pos.x,s,c,pos.y,e.v[86],1,1,1,std::clamp(sample(e,66,t),0.f,1.f),e.v[87],e.v[88],0,0,0};bank.draws.push_back(d);
        }
    }
    void admit(RoomVisualBank& bank)const{
        // Old discovers required actor pages from source-depth draws at room bind.
        for(const auto& e:emitters_)for(unsigned i=0;i<e.frames;++i){RoomDraw d;d.sprite=e.first+i;d.values[6]=e.v[86];d.values[10]=0;bank.draws.push_back(d);}
    }
    bool ready()const{return ready_;}
    unsigned activeCount()const{unsigned n=0;for(const auto& p:particles_)n+=p.active;return n;}
    unsigned dropped()const{return dropped_;}
    const auto& emitted()const{return emitted_;}
    const auto& particles()const{return particles_;}
    const auto& emitters()const{return emitters_;}
private:
    struct Emission{float age=0,remainder=0;unsigned born=0;bool active=false,started=false;};
    struct Particle:HeroEffectParticle{unsigned row=0;};
    float random(float a,float b){seed_=seed_*1664525u+1013904223u;return a+(b-a)*(float(seed_>>8)/16777216.f);}
    static float sample(const Emitter& e,unsigned offset,float t){const float x=std::clamp(t,0.f,1.f)*7;const unsigned i=std::min(6u,unsigned(x));return e.v[offset+i]+(e.v[offset+i+1]-e.v[offset+i])*(x-i);}
    static Vec2 project(const Emitter& e,unsigned b,float x,float y,float z){return {e.v[b]*x+e.v[b+3]*y+e.v[b+6]*z,e.v[b+1]*x+e.v[b+4]*y+e.v[b+7]*z};}
    void spawn(unsigned i){
        auto it=std::find_if(particles_.begin(),particles_.end(),[](const auto& p){return !p.active;});if(it==particles_.end()){++dropped_;return;}
        const auto& e=emitters_[i];const auto& v=e.v;auto& p=*it;p={};p.active=true;p.emitter=i;
        float x=0,y=0,z=0;
        if(e.shape==5){x=random(-.5f,.5f)*v[82];y=random(-.5f,.5f)*v[83];z=random(-.5f,.5f)*v[84];}
        else{x=random(-v[11],v[11]);} // Unity Shape12: SingleSidedEdge, not a disk.
        // Both local-space emitters have static parents: the pillar animates sprites only.
        const auto offset=project(e,18,x,y,z);p.position={v[16]+offset.x,v[17]+offset.y};
        p.velocity=project(e,27,0,0,random(v[4],v[5]));p.life=random(v[2],v[3]);p.size=random(v[6],v[7]);
        p.angle=random(v[8],v[9]);p.spin=random(v[48],v[49]);p.mix=random(0,1);if(e.flags&16)p.row=std::min(e.frames-1,unsigned(random(0,float(e.frames))));
        p.history.reset(p.position);++emitted_[i];
    }
    std::array<Emitter,6> emitters_{};std::array<Emission,6> emissions_{};std::array<unsigned,6> emitted_{};
    std::vector<Particle> particles_;unsigned seed_=1,dropped_=0;bool ready_=false,busy_=false;
};
}
