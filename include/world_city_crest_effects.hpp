#pragma once
#include "world_city_crest.hpp"
#include "hero_effects.hpp"
#include "room_visual.hpp"
#include "decor_motion.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// Visual-only FSM5613 dust. No save, movement, collision or audio ownership.
class CityCrestDust {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=936; // 906 dust + at most 30 Bump rocks.
    static constexpr unsigned drawCapacity=capacity+1;
    struct Emitter {
        unsigned owner=0,shape=0,first=0,frames=0,flags=0,capacity=0;
        std::array<float,89> v{};
    };
    bool load(const char* directory,bool sparseDust=false){
        clear();if(!directory)return false;char path[512];
        std::snprintf(path,sizeof(path),"%s/city-crest-fx-art.bin",directory);
        if(!visuals.atlas.load(path))return false;
        std::snprintf(path,sizeof(path),"%s/city-crest-fx.bin",directory);
        auto* f=std::fopen(path,"rb");if(!f){clear();return false;}
        char magic[4];unsigned count=0;
        bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CCP1",4)&&std::fread(&count,4,1,f)==1&&count==7;
        for(auto& e:emitters_){
            unsigned fields[6]{};
            ok=ok&&std::fread(fields,4,6,f)==6&&std::fread(e.v.data(),4,89,f)==89;
            e.owner=fields[0];e.shape=fields[1];e.first=fields[2];e.frames=fields[3];e.flags=fields[4];e.capacity=fields[5];
        }
        ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
        if(!ok||!bind(emitters_)){clear();return false;}
        std::snprintf(path,sizeof(path),"%s/city-crest-slam.bin",directory);
        f=std::fopen(path,"rb");if(!f){clear();return false;}
        RoomDraw slam;
        ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CCS1",4)&&
            std::fread(&slam.sprite,4,1,f)==1&&std::fread(&slam.owner,4,1,f)==1&&
            std::fread(slam.values.data(),4,16,f)==16&&std::fgetc(f)==EOF;
        std::fclose(f);
        if(!ok||!bindSlam(slam)){clear();return false;}
        std::snprintf(path,sizeof(path),"%s/city-crest-rocks.bin",directory);
        f=std::fopen(path,"rb");if(!f){clear();return false;}
        Emitter rock;unsigned fields[6]{};std::array<float,6> ranges{};
        ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CCR1",4)&&std::fread(&count,4,1,f)==1&&count==1&&
            std::fread(fields,4,6,f)==6&&std::fread(rock.v.data(),4,89,f)==89&&
            std::fread(ranges.data(),4,6,f)==6&&std::fgetc(f)==EOF;
        std::fclose(f);rock.owner=fields[0];rock.shape=fields[1];rock.first=fields[2];rock.frames=fields[3];rock.flags=fields[4];rock.capacity=fields[5];
        if(!ok||!bindRocks(rock,ranges)){clear();return false;}
        if(sparseDust){
            unsigned budget=emitters_.back().capacity+rocks_.capacity;
            for(unsigned index=0;index+1<emitters_.size();++index){
                auto& dust=emitters_[index];dust.capacity/=4;dust.v[74]*=.25f;budget+=dust.capacity;
            }
            particles_ = std::vector<Particle>(budget);
            visuals.draws = std::vector<RoomDraw>();visuals.draws.reserve(budget+1);
        }
        // Old admission discovers the page from these invisible source-depth records.
        for(const auto& e:emitters_){RoomDraw draw;draw.sprite=e.first;draw.values[6]=e.v[86];visuals.draws.push_back(draw);}
        const auto& clip=visuals.atlas.clips[slamClip_];
        for(unsigned i=0;i<clip.count;++i){auto draw=slamDraw_;draw.sprite=visuals.atlas.frames[clip.first+i];draw.values[10]=0;visuals.draws.push_back(draw);}
        for(unsigned i=0;i<rocks_.frames;++i){RoomDraw draw;draw.sprite=rocks_.first+i;draw.values[6]=rocks_.v[86];visuals.draws.push_back(draw);}
        return true;
    }
    bool bind(const std::array<Emitter,7>& data){
        constexpr unsigned owners[]{4030,4034,4050,4027,4047,4051,4033};
        constexpr unsigned caps[]{160,200,160,60,200,120,6};
        for(unsigned i=0;i<7;++i){const auto& e=data[i];const auto& v=e.v;
            if(e.owner!=owners[i]||e.capacity!=caps[i]||(e.shape!=5&&e.shape!=12)||e.flags>15||
               e.frames!=(i==6?14u:1u)||e.first+e.frames>visuals.atlas.sprites.size()||
               !std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);})||
               v[0]<=0||v[0]>1||v[1]<0||v[2]<=0||v[3]<v[2]||v[3]>2||v[6]<=0||v[7]<v[6]||
               v[14]<0||v[14]>1||v[15]<0||v[74]<0||v[74]>2000)return false;
        }
        emitters_=data;particles_.resize(capacity);visuals.draws.reserve(drawCapacity);ready_=true;
        reset({},1,true);return true;
    }
    bool bindSlam(const RoomDraw& draw){
        const auto* clip=visuals.atlas.find("Slam");
        if(!ready_||!clip||clip->fps!=18||clip->count!=6||clip->wrap!=2||
           clip->first>visuals.atlas.frames.size()||clip->count>visuals.atlas.frames.size()-clip->first||
           draw.owner!=97||!std::all_of(draw.values.begin(),draw.values.end(),[](float x){return std::isfinite(x);}))return false;
        for(unsigned i=0;i<clip->count;++i)if(visuals.atlas.frames[clip->first+i]>=visuals.atlas.sprites.size())return false;
        if(draw.sprite!=visuals.atlas.frames[clip->first])return false;
        slamDraw_=draw;slamClip_=unsigned(clip-visuals.atlas.clips.data());slamAge_=-1;return true;
    }
    bool bindRocks(const Emitter& e,const std::array<float,6>& ranges){
        if(!ready_||e.owner!=4031||e.shape!=5||e.frames!=4||e.capacity!=30||e.flags>3||
           e.first>visuals.atlas.sprites.size()||e.frames>visuals.atlas.sprites.size()-e.first||
           !std::all_of(e.v.begin(),e.v.end(),[](float x){return std::isfinite(x);})||
           e.v[0]!=1.5f||e.v[1]!=.25f||e.v[2]!=1.5f||e.v[3]!=2||e.v[4]!=0||e.v[5]!=0||
           !std::all_of(ranges.begin(),ranges.end(),[](float x){return std::isfinite(x);})||
           ranges[0]!=2||ranges[1]!=2||ranges[2]!=20||ranges[3]!=20||ranges[4]!=0||ranges[5]!=1)return false;
        rocks_=e;rockRanges_=ranges;rocksReady_=true;return true;
    }
    void clear(){*this={};}
    bool ready()const{return ready_;}
    void reset(CityCrestSounds sounds={},unsigned seed=1,bool awake=false){
        observed_=sounds;seed_=seed;lastGateY_=0;slamAge_=-1;emissions_={};live_={};emitted_={};dropped_=0;
        for(auto& p:particles_)p.active=false;
        visuals.draws.clear();if(ready_&&awake)start(6);
    }
    void update(float dt,CityCrestSounds sounds,float gateY){
        if(!ready_||!std::isfinite(dt)||dt<0||!std::isfinite(gateY))return;
        if(sounds.open!=observed_.open){start(0);start(6);}
        if(sounds.bump!=observed_.bump){start(2);if(rocksReady_)start(7);}
        if(sounds.drop!=observed_.drop)start(3);
        if(sounds.slam!=observed_.slam){start(1);start(4);start(5);if(slamClip_<visuals.atlas.clips.size())slamAge_=0;}
        observed_=sounds;
        if(dt==0)return;
        if(slamAge_>=0){slamAge_+=dt;if(slamAge_>=6.f/18)slamAge_=-1;}
        // The six dust clouds use world space; the local stream follows its gate.
        for(auto& p:particles_)if(p.active){
            PresentationPosition::Step step(p.history,p.position);
            if((emitter(p.emitter).flags&12)==12)p.position.y+=gateY-lastGateY_;
            p.age+=dt;
            if(p.age>=p.life){p.active=false;--live_[p.emitter];continue;}
            const auto& v=emitter(p.emitter).v;
            std::array<float,3> position{p.position.x,p.position.y,0},base{p.velocity.x,p.velocity.y,p.velocityZ};
            decorMotionStep(position,base,{p.module.x,p.module.y,p.moduleZ},
                {p.force.x*dt,p.force.y*dt,p.forceZ*dt},{},dt,v[15],decorRetention(v[14],dt));
            p.position={position[0],position[1]};p.velocity={base[0],base[1]};p.velocityZ=base[2];
            const float rotation=p.emitter==7?std::clamp((std::hypot(p.velocity.x,p.velocity.y)-rockRanges_[4])/(rockRanges_[5]-rockRanges_[4]),0.f,1.f):1;
            p.angle+=p.spin*rotation*dt;
        }
        lastGateY_=gateY;
        for(unsigned i=0;i<8;++i){auto& a=emissions_[i];if(!a.active)continue;const auto& v=emitter(i).v;
            const float before=std::clamp(a.age-v[1],0.f,v[0]);a.age+=dt;
            const float after=std::clamp(a.age-v[1],0.f,v[0]);a.remainder+=(after-before)*a.rate;
            while(a.remainder>=.9999f&&a.born<emitter(i).capacity){spawn(i,gateY);a.remainder-=1;++a.born;}
            if(a.age>=v[0]+v[1])a.active=false;
        }
    }
    void present(PresentationTime time){
        if(!ready_)return;
        visuals.draws.clear();
        for(const auto& p:particles_)if(p.active){const auto& e=emitter(p.emitter);
            const float t=p.age/p.life,size=p.size*(sample(e,50,t)*(1-p.mix)+sample(e,58,t)*p.mix);
            const auto pos=p.renderPosition(time);const float c=std::cos(p.angle)*size,s=std::sin(p.angle)*size;
            RoomDraw d;d.owner=e.owner;d.sprite=e.first+(p.emitter==7?p.row:std::min(e.frames-1,unsigned(t*e.frames)));
            d.values={c,-s,pos.x,s,c,pos.y,e.v[86],1,1,1,std::clamp(sample(e,66,t),0.f,1.f),e.v[87],e.v[88],0,0,0};
            visuals.draws.push_back(d);
        }
        if(slamPlaying()){
            const auto& clip=visuals.atlas.clips[slamClip_];auto draw=slamDraw_;
            draw.sprite=visuals.atlas.frames[clip.first+std::min(5u,unsigned(slamAge_*18))];
            draw.values[5]+=lastGateY_;visuals.draws.push_back(draw);
        }
    }
    bool slamPlaying()const{return slamAge_>=0;}
    unsigned activeCount()const{unsigned n=0;for(auto count:live_)n+=count;return n;}
    unsigned dropped()const{return dropped_;}
    const auto& emitted()const{return emitted_;}
    const auto& particles()const{return particles_;}
    const auto& emitters()const{return emitters_;}
    RoomVisualBank visuals;
private:
    struct Emission{float age=0,remainder=0;unsigned born=0;bool active=false;float rate=0;};
    struct Particle:HeroEffectParticle{unsigned row=0;};
    const Emitter& emitter(unsigned i)const{return i==7?rocks_:emitters_[i];}
    void start(unsigned i){if(emissions_[i].active||live_[i])return;emissions_[i]={0,0,0,true,emitter(i).v[74]};}
    float random(float a,float b){seed_=seed_*1664525u+1013904223u;return a+(b-a)*(float(seed_>>8)/16777216.f);}
    static float sample(const Emitter& e,unsigned offset,float t){
        const float x=std::clamp(t,0.f,1.f)*7;const unsigned i=std::min(6u,unsigned(x));
        return e.v[offset+i]+(e.v[offset+i+1]-e.v[offset+i])*(x-i);
    }
    static Vec2 project(const Emitter& e,unsigned b,float x,float y,float z){
        return {e.v[b]*x+e.v[b+3]*y+e.v[b+6]*z,e.v[b+1]*x+e.v[b+4]*y+e.v[b+7]*z};
    }
    void spawn(unsigned index,float gateY){
        const auto& e=emitter(index);const auto& v=e.v;
        auto it=std::find_if(particles_.begin(),particles_.end(),[](const auto& p){return !p.active;});
        if(it==particles_.end()){++dropped_;return;}
        auto& p=*it;p={};p.active=true;p.emitter=index;
        float x=0,y=0,z=0,dx=0,dy=0,dz=1;
        if(e.shape==5){x=random(-.5f,.5f)*v[82];y=random(-.5f,.5f)*v[83];z=random(-.5f,.5f)*v[84];}
        else{x=random(-v[11],v[11]);dy=1;dz=0;}
        const auto offset=project(e,18,x,y,z);
        p.position={v[16]+offset.x,v[17]+offset.y+((e.flags&4)?gateY:0)};
        const float speed=random(v[4],v[5]);p.velocity=project(e,27,dx*speed,dy*speed,dz*speed);
        p.velocityZ=(v[29]*dx+v[32]*dy+v[35]*dz)*speed;
        for(unsigned k=0;k<2;++k){const unsigned b=36+k*6;
            const float a=random(v[b],v[b+1]),by=random(v[b+2],v[b+3]),bz=random(v[b+4],v[b+5]);
            const auto xy=e.flags&(1u<<k)?project(e,27,a,by,bz):Vec2{a,by};
            const float vz=e.flags&(1u<<k)?v[29]*a+v[32]*by+v[35]*bz:bz;
            if(k){p.module=xy;p.moduleZ=vz;}else{p.force=xy;p.forceZ=vz;}
        }
        p.force.y-=9.81f*(index==7?random(rockRanges_[0],rockRanges_[1]):v[10]);p.life=random(v[2],v[3]);p.size=random(v[6],v[7]);
        p.angle=random(v[8],v[9]);p.spin=random(v[48],v[49]);p.mix=random(0,1);
        // SingleRow + Random row mode, one column: retain one original rock cell.
        if(index==7)p.row=std::min(3u,unsigned(random(0,4)));
        p.history.reset(p.position);++live_[index];++emitted_[index];
    }
    std::array<Emitter,7> emitters_{};
    Emitter rocks_{};std::array<float,6> rockRanges_{};bool rocksReady_=false;
    std::array<Emission,8> emissions_{};
    std::array<unsigned,8> live_{},emitted_{};
    std::vector<Particle> particles_;
    CityCrestSounds observed_{};
    RoomDraw slamDraw_{};
    unsigned slamClip_=~0u,seed_=1,dropped_=0;float lastGateY_=0,slamAge_=-1;bool ready_=false;
};
}
