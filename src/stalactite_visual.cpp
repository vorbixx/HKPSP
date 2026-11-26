#include "stalactite_visual.hpp"
#include "breakable.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
bool StalactiteVisuals::bind(const std::array<Record,capacity>& records,unsigned count,const AnimationBank& atlas) {
    if(!count||count>capacity)return false;
    for(unsigned i=0;i<count;++i){
        const auto&r=records[i];if(!r.owner)return false;
        for(unsigned j=0;j<i;++j)if(records[j].owner==r.owner)return false;
        for(const auto*d:{&r.hanging,&r.embedded}){
            if(d->sprite>=atlas.sprites.size())return false;
            for(float value:d->values)if(!std::isfinite(value))return false;
            if(d->values[10]<0||d->values[10]>1)return false;
        }
    }
    records_=records;count_=count;reset();return true;
}
bool StalactiteVisuals::load(const char* path,const AnimationBank& atlas) {
    FILE*f=std::fopen(path,"rb");if(!f)return false;
    std::array<Record,capacity> records{};char magic[4]{};unsigned count=0;
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"STV1",4)&&
        std::fread(&count,4,1,f)==1&&count&&count<=capacity;
    for(unsigned i=0;ok&&i<count;++i){auto&r=records[i];
        ok=std::fread(&r.owner,4,1,f)==1;
        for(auto*d:{&r.hanging,&r.embedded})ok=ok&&std::fread(&d->sprite,4,1,f)==1&&std::fread(d->values.data(),4,16,f)==16;
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    return ok&&bind(records,count,atlas);
}
void StalactiteVisuals::reset(){trailClocks_={};seen_={};}
void StalactiteVisuals::advance(float seconds,const Stalactite* actors,unsigned count,DecorEffects* effects,const World& world,PresentationTime time){
    if(!actors||seconds<=0||!std::isfinite(seconds))return;
    for(unsigned i=0;i<std::min(count,count_);++i){
        const auto&a=actors[i];const auto pose=a.renderPosition(time);
        if(effects){
            for(unsigned cue=0;cue<a.cues.size();++cue)for(unsigned n=seen_[i][cue];n<a.cues[cue];++n)
                effects->stalactiteCue(static_cast<StalactiteCue>(cue),pose,a.rotation,a.lastStrike,world);
            if(a.state==StalactiteState::Falling)effects->stalactiteTrail(pose,a.rotation,seconds,trailClocks_[i],world);
            else trailClocks_[i]=0;
        }
        seen_[i]=a.cues;
    }
}
RoomDraw StalactiteVisuals::draw(unsigned i,const Stalactite& actor,PresentationTime time) const {
    RoomDraw result=actor.state==StalactiteState::Embedded?records_[i].embedded:records_[i].hanging;
    const auto pose=actor.renderPosition(time);auto&v=result.values;
    const float angle=actor.rotation*.017453292519943295f,c=std::cos(angle),s=std::sin(angle);
    const auto source=v;
    v[0]=c*source[0]-s*source[3];v[1]=c*source[1]-s*source[4];
    v[3]=s*source[0]+c*source[3];v[4]=s*source[1]+c*source[4];
    v[2]=pose.x+c*source[2]-s*source[5];v[5]=pose.y+s*source[2]+c*source[5];
    // The source owner is hidden in the static room. A dynamic draw has no
    // static owner, so the shared visibility callback must not hide it too.
    result.owner=0;
    return result;
}
void StalactiteVisuals::present(const Stalactite* actors,unsigned count,PresentationTime time,RoomVisualBank& output) const {
    for(unsigned i=0;i<std::min(count,count_);++i)if(actors[i].active())output.draws.push_back(draw(i,actors[i],time));
}
bool StalactiteVisuals::hides(unsigned owner) const {
    for(unsigned i=0;i<count_;++i)if(records_[i].owner==owner)return true;
    return false;
}
}
