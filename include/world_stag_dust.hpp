#pragma once
#include "hero_effects.hpp"
#include "room_visual.hpp"
#include "decor_motion.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// PS1176: source SingleSidedEdge, world-space, no collision.
class StagBellDust {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=57;
    bool load(const char* path,unsigned sprites){
        auto* f=std::fopen(path,"rb");if(!f)return false;
        char magic[4];unsigned count=0,fields[6]{};std::array<float,89> values{};
        const bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"SDF1",4)&&
            std::fread(&count,4,1,f)==1&&count==1&&std::fread(fields,4,6,f)==6&&
            std::fread(values.data(),4,89,f)==89&&std::fgetc(f)==EOF;
        std::fclose(f);
        return ok&&fields[0]==1176&&fields[1]==12&&fields[3]==1&&fields[4]<=3&&fields[5]==capacity&&
            fields[2]<sprites&&bind(fields[2],values);
    }
    bool bind(unsigned sprite,const std::array<float,89>& v){
        if(!std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);})||
           v[0]!=.3f||v[1]!=0||v[2]<=0||v[3]!=1||v[4]!=0||v[5]!=12||v[6]<=0||v[7]<=0||v[10]!=0||v[74]!=190)return false;
        sprite_=sprite;v_=v;bound_=true;reset();return true;
    }
    void reset(unsigned seed=1){particles_={};age_=-1;born_=0;remainder_=0;seed_=seed;}
    void start(){if(bound_){reset(seed_);age_=0;}}
    void admitPage(RoomVisualBank& bank)const{
        if(bound_){RoomDraw d;d.owner=1176;d.sprite=sprite_;d.values[6]=v_[86];bank.draws.push_back(d);}
    }
    void update(float dt){
        if(!bound_||!std::isfinite(dt)||dt<=0)return;
        for(auto& p:particles_)if(p.active){
            PresentationPosition::Step step(p.history,p.position);
            p.age+=dt;if(p.age>=p.life){p.active=false;continue;}
            std::array<float,3> position{p.position.x,p.position.y,0},base{p.velocity.x,p.velocity.y,p.velocityZ};
            decorMotionStep(position,base,{},{},{},dt,v_[15],decorRetention(v_[14],dt));
            p.position={position[0],position[1]};p.velocity={base[0],base[1]};p.velocityZ=base[2];p.angle+=p.spin*dt;
        }
        if(age_<0)return;
        const auto before=std::min(age_,v_[0]);age_+=dt;
        remainder_+=(std::min(age_,v_[0])-before)*v_[74];
        while(remainder_>=.9999f&&born_<capacity){spawn();remainder_-=1;}
        if(age_>=v_[0])age_=-1;
    }
    void append(RoomVisualBank& bank,PresentationTime time={})const{
        for(const auto& p:particles_)if(p.active){
            const float t=p.age/p.life,size=p.size*(sample(50,t)*(1-p.mix)+sample(58,t)*p.mix);
            const float c=std::cos(p.angle)*size,s=std::sin(p.angle)*size;const auto pos=p.renderPosition(time);
            RoomDraw d;d.sprite=sprite_;d.owner=1176;
            d.values={c,-s,pos.x,s,c,pos.y,v_[86],1,1,1,std::clamp(sample(66,t),0.f,1.f),v_[87],v_[88],0,0,0};
            bank.draws.push_back(d);
        }
    }
    unsigned born()const{return born_;}
    unsigned active()const{unsigned count=0;for(const auto& p:particles_)count+=p.active;return count;}
    const auto& particles()const{return particles_;}
private:
    float random(float lo,float hi){seed_=seed_*1664525u+1013904223u;return lo+(hi-lo)*(float(seed_>>8)/16777216.f);}
    float sample(unsigned offset,float t)const{
        const float x=std::clamp(t,0.f,1.f)*7;const unsigned i=std::min(6u,unsigned(x));
        return v_[offset+i]+(v_[offset+i+1]-v_[offset+i])*(x-i);
    }
    Vec2 project(unsigned b,float x,float y,float z)const{
        return {v_[b]*x+v_[b+3]*y+v_[b+6]*z,v_[b+1]*x+v_[b+4]*y+v_[b+7]*z};
    }
    void spawn(){
        auto& p=particles_[born_++];p={};p.active=true;
        const auto offset=project(18,random(-v_[11],v_[11]),0,0);
        p.position={v_[16]+offset.x,v_[17]+offset.y};p.history.reset(p.position);
        const float speed=random(v_[4],v_[5]);p.velocity=project(27,0,speed,0);p.velocityZ=v_[32]*speed;
        p.life=random(v_[2],v_[3]);p.size=random(v_[6],v_[7]);p.angle=random(v_[8],v_[9]);
        p.spin=random(v_[48],v_[49]);p.mix=random(0,1);
    }
    std::array<HeroEffectParticle,capacity> particles_{};
    std::array<float,89> v_{};unsigned sprite_=0,born_=0,seed_=1;
    float age_=-1,remainder_=0;bool bound_=false;
};
}
