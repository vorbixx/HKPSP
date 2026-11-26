#include "uumuu_body.hpp"
#include <cmath>
#include <cstring>

namespace hk3ds {
namespace {
struct ClipContract {
    const char* name;
    float fps;
    std::uint32_t wrap,loopStart,count;
};
constexpr ClipContract contracts[]={
    {"Uumuu/Idle",12,0,0,6}, {"Uumuu/Attack",12,1,2,5},
    {"Uumuu/Wound",12,1,2,9}, {"Uumuu/Recover Antic",15,0,0,3},
    {"Uumuu/Recover",12,1,1,7}
};
bool matches(const Clip* clip,const ClipContract& expected){
    return clip&&std::strcmp(clip->name,expected.name)==0&&clip->fps==expected.fps&&
        clip->wrap==expected.wrap&&clip->loopStart==expected.loopStart&&clip->count==expected.count;
}
unsigned index(UumuuBodyAnimation animation){return static_cast<unsigned>(animation);}
}

bool UumuuBodyActor::bind(const AnimationBank& bank){
    const Clip* next[5]{};
    for(unsigned i=0;i<5;++i){
        next[i]=bank.find(contracts[i].name);
        if(!matches(next[i],contracts[i]))return false;
    }
    reset();bank_=&bank;
    for(unsigned i=0;i<5;++i)clips_[i]=next[i];
    player_.play(clips_[index(animation_)],0,true);
    return true;
}

void UumuuBodyActor::reset(){
    bank_=nullptr;for(auto& clip:clips_)clip=nullptr;
    player_={};animation_=UumuuBodyAnimation::Idle;events_={};
}

bool UumuuBodyActor::enter(UumuuBodyAnimation animation){
    if(!bank_||index(animation)>=5)return false;
    animation_=animation;
    player_.play(clips_[index(animation_)],0,true);
    return true;
}

bool UumuuBodyActor::advance(float dt){
    if(!bank_||!std::isfinite(dt)||dt<=0)return false;
    const bool wasComplete=player_.complete;
    player_.advance(dt);
    if(!wasComplete&&player_.complete){events_.animationFinished=true;events_.finishedAnimation=animation_;}
    return true;
}
}
