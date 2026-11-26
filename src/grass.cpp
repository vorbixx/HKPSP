#include "grass.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds
{
namespace {
float sign(float value){return value<0?-1.f:1.f;} // Unity Mathf.Sign(0) is +1.
bool valid(const GrassConfig& c)
{
    if(!c.owner||!c.drawOwner||c.scaleX==0||c.left>=c.right||c.bottom>=c.top)return false;
    for(float v:{c.x,c.y,c.z,c.scaleX,c.left,c.bottom,c.right,c.top})if(!std::isfinite(v))return false;
    for(float v:c.stubValues)if(!std::isfinite(v))return false;
    for(float v:c.stubGrass)if(!std::isfinite(v))return false;
    for(float v:c.particleOffset)if(!std::isfinite(v))return false;
    return true;
}
}
bool grassShouldCut(GrassContact contact,bool superDashing)
{
    return contact==GrassContact::Nail||contact==GrassContact::SharpShadow||
        (contact==GrassContact::HeroBox&&superDashing);
}
float grassCurve(GrassReaction reaction,float u)
{
    if(!std::isfinite(u))return 0;
    // Serialized pre/post infinity is 2 (Loop in Unity bindings).
    // approximation: native serializer mapping/overshoot samples are unprobed.
    if(u<0||u>1)u-=std::floor(u);
    constexpr float times[]={0,.25f,.75f,1};
    constexpr float values[]={0,1,-.1f,0};
    unsigned i=u<=.25f?0:u<=.75f?1:2;
    const float length=times[i+1]-times[i],t=(u-times[i])/length;
    const float t2=t*t,t3=t2*t;
    const float tangent=reaction==GrassReaction::Attack&&i==0?8.989195f:0;
    return (2*t3-3*t2+1)*values[i]+(t3-2*t2+t)*length*tangent+
           (-2*t3+3*t2)*values[i+1];
}
bool GrassConfig::interactive() const {return std::fabs(z-.004f)<=1.8f;}
bool GrassBank::load(const char* path)
{
    if(!path)return false;
    std::FILE* file=std::fopen(path,"rb");if(!file)return false;
    GrassBank next;char magic[4];std::uint32_t header[3]{};
    auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
    bool ok=read(magic,4)&&std::memcmp(magic,"HKG1",4)==0&&read(header,sizeof header);
    next.count=header[0];next.spriteCount=header[1];next.drawCount=header[2];
    ok=ok&&next.count>0&&next.count<=grassCapacity&&next.spriteCount>0&&next.drawCount>=next.count;
    for(unsigned i=0;ok&&i<next.count;++i){
        auto& c=next.bushes[i];std::uint32_t ids[8]{};float values[8]{};
        ok=read(ids,sizeof ids)&&read(values,sizeof values)&&
            read(c.stubValues.data(),sizeof(float)*c.stubValues.size())&&
            read(c.stubGrass.data(),sizeof(float)*c.stubGrass.size())&&
            read(c.particleOffset.data(),sizeof(float)*c.particleOffset.size());
        c.owner=ids[0];c.drawOwner=ids[1];c.idleSprite=ids[2];
        std::copy(ids+3,ids+7,c.pushSprites.begin());c.stubSprite=ids[7];
        c.x=values[0];c.y=values[1];c.z=values[2];c.scaleX=values[3];
        c.left=values[4];c.bottom=values[5];c.right=values[6];c.top=values[7];
        ok=ok&&valid(c)&&c.idleSprite<next.spriteCount&&c.stubSprite<next.spriteCount;
        for(auto sprite:c.pushSprites)ok=ok&&sprite<next.spriteCount;
        for(unsigned j=0;j<i;++j)ok=ok&&next.bushes[j].owner!=c.owner&&next.bushes[j].drawOwner!=c.drawOwner;
    }
    ok=ok&&std::fgetc(file)==EOF&&!std::ferror(file);std::fclose(file);
    if(ok)*this=next;
    return ok;
}
bool GrassState::bind(const GrassConfig& config,float epsilon)
{
    if(!valid(config)||!std::isfinite(epsilon)||epsilon<-.01f||epsilon>.01f)return false;
    config_=&config;epsilon_=epsilon;restart();return true;
}
void GrassState::restart()
{
    reaction_=GrassReaction::Walk;push_=epsilon_;elapsed_=sampled_=0;direction_=1;
    returned_=true;cut_=player_=animatorPush_=false;
}
void GrassState::start(GrassReaction reaction,float direction,bool animator)
{
    reaction_=reaction;direction_=direction;returned_=false;elapsed_=sampled_=0;
    if(animator)animatorPush_=true;
}
GrassSound GrassState::enter(float colliderX,GrassContact contact,bool superDashing)
{
    if(!config_||!config_->interactive()||!returned_||!std::isfinite(colliderX))return GrassSound::None;
    const bool attack=grassShouldCut(contact,superDashing);
    start(attack?GrassReaction::Attack:GrassReaction::Walk,sign(config_->x-colliderX),true);
    if(!attack&&contact==GrassContact::Player)player_=true;
    return moveSound();
}
GrassSound GrassState::walk(float velocityX)
{
    if(!config_||!config_->interactive()||!player_||!returned_||!std::isfinite(velocityX)||std::fabs(velocityX)<.1f)return GrassSound::None;
    start(GrassReaction::Walk,sign(velocityX),true);return moveSound();
}
GrassSound GrassState::cut(float colliderX,GrassContact contact,bool superDashing)
{
    if(!config_||!config_->interactive()||cut_||!std::isfinite(colliderX)||!grassShouldCut(contact,superDashing))return GrassSound::None;
    // First cut restarts an existing walk/attack response. CutReact does not
    // set Animator.Push; the replacement is never sprite animated.
    start(GrassReaction::Attack,sign(config_->x-colliderX),false);cut_=true;
    return GrassSound::Cut;
}
void GrassState::advance(float seconds)
{
    if(!active()||!std::isfinite(seconds)||seconds<0)return;
    const float duration=reaction_==GrassReaction::Walk?.8f:1.f;
    const float amount=reaction_==GrassReaction::Walk?.15f:.3f;
    sampled_=elapsed_;
    push_=grassCurve(reaction_,elapsed_/duration)*amount*direction_*sign(config_->scaleX)+epsilon_;
    if(elapsed_>=duration){returned_=true;animatorPush_=false;}
    elapsed_+=seconds;
}
unsigned GrassState::frame() const
{
    // approximation: sample the authored four keys on the reaction clock;
    // native Animator Update/LateUpdate and offscreen culling were not probed.
    return sampled_<.0833333358f?0:sampled_<.1666666716f?1:sampled_<.25f?2:3;
}
std::uint32_t GrassState::sprite() const
{
    if(!config_)return 0;
    return cut_?config_->stubSprite:animatorPush_?config_->pushSprites[frame()]:config_->idleSprite;
}
}
