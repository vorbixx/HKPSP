#include "contact_effects.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
static_assert(unsigned(EnemyClip::WaterDrip1)==44,"CFX1 audio indices match contact_effect_artwork.py");
bool ContactEffectBank::load(const char* path,unsigned spriteCount,unsigned emitterCount)
{
    auto* f=std::fopen(path,"rb");if(!f)return false;
    ContactEffectBank next;char magic[4]{};
    const auto read=[&](auto& value){return std::fread(&value,sizeof(value),1,f)==1;};
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CFX1",4)&&read(next.emitters);
    for(auto id:next.emitters)ok=ok&&id<emitterCount;
    for(auto& d:next.drips){
        ok=ok&&read(d.owner)&&read(d.v);
        for(float v:d.v)ok=ok&&std::isfinite(v)&&std::abs(v)<100000;
        ok=ok&&d.owner&&d.v[7]<d.v[9]&&d.v[8]<d.v[10]&&d.v[11]>0&&d.v[12]>=d.v[11]&&d.v[13]<0;
    }
    for(auto& p:next.platforms){
        ok=ok&&read(p.owner)&&read(p.collider)&&read(p.v);
        for(float v:p.v)ok=ok&&std::isfinite(v)&&std::abs(v)<1000000;
        ok=ok&&p.owner&&p.collider&&p.v[2]>p.v[4]&&p.v[3]>p.v[4]&&p.v[7]>0&&p.v[8]>=0&&p.v[8]<=1;
    }
    for(auto& c:next.clips){
        ok=ok&&read(c.fps)&&read(c.count)&&c.count>0&&c.count<=c.sprites.size()&&std::isfinite(c.fps)&&c.fps>0;
        for(unsigned j=0;ok&&j<c.count;++j)ok=read(c.sprites[j])&&c.sprites[j]<spriteCount;
    }
    for(auto& t:next.sounds){
        ok=ok&&read(t.pitchMin)&&read(t.pitchMax)&&read(t.count)&&t.count>0&&t.count<=t.clips.size()&&
            std::isfinite(t.pitchMin)&&std::isfinite(t.pitchMax)&&t.pitchMin>0&&t.pitchMax>=t.pitchMin&&t.pitchMax<=4;
        float sum=0;
        for(unsigned j=0;ok&&j<t.count;++j){
            ok=read(t.clips[j])&&read(t.weights[j])&&t.clips[j]<unsigned(EnemyClip::Count)&&std::isfinite(t.weights[j])&&t.weights[j]>=0;
            sum+=t.weights[j];
        }
        ok=ok&&sum>0;
        ok=ok&&read(t.spatial);
        for(float v:t.spatial)ok=ok&&std::isfinite(v);
        ok=ok&&t.spatial[0]>0&&t.spatial[4]>t.spatial[1]&&t.spatial[7]>=0&&t.spatial[7]<=1;
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    if(ok)*this=next;
    return ok;
}
bool RoomContactEffects::load(const char* path,const DecorEffects& effects)
{
    ContactEffectBank next;
    if(!effects.ready()||!next.load(path,unsigned(effects.visuals.atlas.sprites.size()),effects.loadedEmitters()))return false;
    bind(next);return true;
}
void RoomContactEffects::bind(const ContactEffectBank& bank,unsigned seed)
{
    bank_=bank;ready_=true;reset(seed);
}
float RoomContactEffects::random(float low,float high)
{
    seed_=seed_*1664525u+1013904223u;
    return low+(high-low)*float(seed_>>8)*(1.0f/16777216.0f);
}
void RoomContactEffects::reset(unsigned seed)
{
    seed_=seed?seed:1;platforms_={};sounds_={};
    for(unsigned i=0;i<drips_.size();++i){
        auto& d=drips_[i];const auto& v=bank_.drips[i].v;
        d={};d.position={v[0],v[1]};d.audioZ=v[2];d.history.reset(d.position);d.wait=random(v[11],v[12]);
    }
}
void RoomContactEffects::sound(unsigned owner,unsigned table)
{
    const auto& t=bank_.sounds[table];if(!t.count)return;
    float total=0;for(unsigned i=0;i<t.count;++i)total+=t.weights[i];
    sounds_[owner].selectionSeed=seed_;
    float choice=random(0,total);unsigned index=t.count-1;
    for(unsigned i=0;i<t.count;++i){choice-=t.weights[i];if(choice<0){index=i;break;}}
    auto& s=sounds_[owner];++s.serial;s.clip=t.clips[index];s.pitch=random(t.pitchMin,t.pitchMax);
    if(owner<9){s.position=drips_[owner].position;s.z=drips_[owner].audioZ;}
    else{const auto& v=bank_.platforms[owner-9].v;s.position={v[0],v[1]};s.z=-.1f;}
}
void RoomContactEffects::fixedUpdate(float seconds,const World& world)
{
    if(!ready_||!std::isfinite(seconds)||seconds<=0)return;
    for(unsigned i=0;i<drips_.size();++i){
        auto& d=drips_[i];PresentationPosition::Step step(d.history,d.position);
        if(d.phase!=DripPhase::Fall)continue;
        const auto& v=bank_.drips[i].v;
        d.velocity.y-=9.81f*seconds;
        const float length=-d.velocity.y*seconds;
        float distance=0;
        // Visual-only downward CCD uses the room's real collider contours.
        if(sweepBox(d.position,{v[7],v[8],v[9],v[10]},{0,-1},length,world,distance)){
            d.position.y-=distance;d.position.y+=v[14];d.velocity={};
            d.phase=DripPhase::Impact;d.elapsed=0;d.history.reset(d.position);sound(i,0);
        }else d.position.y-=length;
    }
}
void RoomContactEffects::update(float seconds,const Contacts& contacts,DecorEffects& effects,const World& world)
{
    if(!ready_||!std::isfinite(seconds)||seconds<=0)return;
    for(unsigned i=0;i<drips_.size();++i){
        auto& d=drips_[i];const auto& v=bank_.drips[i].v;
        d.elapsed+=seconds;
        if(d.phase==DripPhase::Fall)continue;
        const auto& clip=bank_.clips[unsigned(d.phase)];
        const float duration=d.phase==DripPhase::Idle?d.wait:float(clip.count)/clip.fps;
        if(d.elapsed+1e-5f<duration)continue;
        d.elapsed=0;
        if(d.phase==DripPhase::Idle)d.phase=DripPhase::Drip;
        else if(d.phase==DripPhase::Drip){d.phase=DripPhase::Fall;d.velocity={0,v[13]};}
        else{
            d.phase=DripPhase::Idle;d.position={v[0],v[1]};d.audioZ=0;d.history.reset(d.position);
            d.wait=random(v[11],v[12]);
        }
    }
    for(unsigned i=0;i<platforms_.size();++i){
        auto& state=platforms_[i];const auto& config=bank_.platforms[i];const auto& v=config.v;
        state.cooldown=std::max(0.0f,state.cooldown-seconds);
        bool touching=false,top=false;
        for(unsigned c=0;c<contacts.count;++c)if(contacts.surfaces[c].id==config.collider){
            touching=true;top=top||std::abs(contacts.surfaces[c].normal.y-1)<.1f;
        }
        // Unity receives OnCollisionEnter only once for a continuous contact,
        // even if the source cooldown elapses while the hero walks on the top.
        if(touching&&!state.contact&&top&&state.cooldown<=1e-6f&&!(unsigned(v[13])&(1u<<9))){
            const Vec2 size{v[2]-v[4],v[3]-v[4]},origin{v[0],v[1]};
            sound(9+i,1);
            effects.platformContact(bank_.emitters,origin,size,size.x*size.y*v[5]+v[6],
                {v[9],-.5f*v[3]+v[10]},random(0,1)<v[8],world);
            state.cooldown=v[7];
        }
        state.contact=touching;
    }
}
void RoomContactEffects::present(RoomVisualBank& visuals,PresentationTime time) const
{
    if(!ready_)return;
    for(unsigned i=0;i<drips_.size();++i){
        const auto& d=drips_[i];const auto& v=bank_.drips[i].v;const auto& c=bank_.clips[unsigned(d.phase)];
        if(!c.count)continue;
        const auto pose=d.renderPosition(time);
        const unsigned frame=d.phase==DripPhase::Idle||d.phase==DripPhase::Fall?unsigned(d.elapsed*c.fps)%c.count:
            std::min(c.count-1,unsigned(d.elapsed*c.fps));
        RoomDraw draw;draw.owner=bank_.drips[i].owner;draw.sprite=c.sprites[frame];
        draw.values={v[3],v[4],pose.x,v[5],v[6],pose.y,v[2],1,1,1,1,v[15],v[16],0,0,0};
        visuals.draws.push_back(draw);
    }
}
bool RoomContactEffects::owns(unsigned owner) const
{
    if(ready_)for(const auto& d:bank_.drips)if(d.owner==owner)return true;
    return false;
}
void RoomContactEffects::dispatchAudio(AudioVoices& voices,std::array<unsigned,27>& serials,Vec2 listener) const
{
    if(!ready_)return;
    for(unsigned i=0;i<sounds_.size();++i){
        const auto& s=sounds_[i];
        if(i<drips_.size()){
            const unsigned owner=0xCF000000u+i;
            if(serials[i]!=s.serial){
                serials[i]=s.serial;
                if(s.serial)voices.playAtPitch(EnemyClip(s.clip),owner,s.pitch);
            }
            const auto& active=voices.voices();
            if(!std::any_of(active.begin(),active.end(),[&](const AudioVoice& v){return v.active&&v.owner==owner;}))continue;
            // WaterDrip plays on its own AudioSource, which returns to startPos
            // after Impact without stopping its one-shot. Follow that live source.
            const auto& r=bank_.sounds[0].spatial;
            AudioStereoMix mix{1,0};
            if(r[0]>0){
                const float dx=drips_[i].position.x-listener.x,dy=drips_[i].position.y-listener.y,dz=drips_[i].audioZ+38.1f;
                const float distance=std::sqrt(dx*dx+dy*dy+dz*dz),normalized=distance/r[0];
                const float span=r[4]-r[1],t=std::clamp((normalized-r[1])/span,0.0f,1.0f),t2=t*t,t3=t2*t;
                const float rolloff=std::clamp((2*t3-3*t2+1)*r[2]+(t3-2*t2+t)*span*r[3]+
                    (-2*t3+3*t2)*r[5]+(t3-t2)*span*r[6],0.0f,1.0f);
                const float direction=distance>0?std::clamp(dx/distance,-1.0f,1.0f):0;
                const float left=(1-r[7])+r[7]*rolloff*std::sqrt((1-direction)*.5f);
                const float right=(1-r[7])+r[7]*rolloff*std::sqrt((1+direction)*.5f);
                mix.gain=(left+right)*.5f;mix.pan=left+right>0?(right-left)/(right+left):0;
            }
            voices.updateOwnerMix(owner,mix);
            continue;
        }
        if(serials[i]==s.serial)continue;
        serials[i]=s.serial;if(!s.serial)continue;
        float gain=1;
        const auto& r=bank_.sounds[i<9?0:1].spatial;
        if(r[0]>0){
            // Source listener is on the Tutorial camera at Z=-38.1.
            const float dx=s.position.x-listener.x,dy=s.position.y-listener.y,dz=s.z+38.1f;
            const float distance=std::sqrt(dx*dx+dy*dy+dz*dz)/r[0];
            const float span=r[4]-r[1],t=std::clamp((distance-r[1])/span,0.0f,1.0f),t2=t*t,t3=t2*t;
            const float rolloff=std::clamp((2*t3-3*t2+1)*r[2]+(t3-2*t2+t)*span*r[3]+
                (-2*t3+3*t2)*r[5]+(t3-t2)*span*r[6],0.0f,1.0f);
            gain=1-r[7]+r[7]*rolloff;
        }
        const unsigned owner=0xCF000000u+i;
        if(gain>0&&voices.playAtPitch(EnemyClip(s.clip),owner,s.pitch))
            for(auto& voice:voices.voices())if(voice.active&&voice.owner==owner)voice.gain*=gain;
    }
}
}
