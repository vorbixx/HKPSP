#include "uumuu_quirrel_parts.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
constexpr std::uint32_t absent=0xffffffffu;
std::uint32_t word(const unsigned char* p){
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|
           (std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
bool geometry(const SpriteFrame& s){
    return s.width&&s.height&&std::isfinite(s.left)&&std::isfinite(s.right)&&
           std::isfinite(s.bottom)&&std::isfinite(s.top)&&s.left<s.right&&s.bottom<s.top;
}
}
bool UumuuQuirrelParts::load(const AnimationBank& bank,const char* path){
    reset();
    if(!path)return false;
    auto file=std::fopen(path,"rb");if(!file)return false;
    std::array<unsigned char,encodedBytes> data{};
    const bool ok=std::fread(data.data(),1,data.size(),file)==data.size()&&std::fgetc(file)==EOF;
    std::fclose(file);
    return ok&&bind(bank,data.data(),data.size());
}
bool UumuuQuirrelParts::bind(const AnimationBank& bank,const void* data,std::size_t bytes){
    reset();
    if(!data||bytes!=encodedBytes||bank.sprites.size()!=totalTiles)return false;
    const auto p=static_cast<const unsigned char*>(data);
    if(std::memcmp(p,"HQP1",4)||word(p+4)!=sourceSprites||word(p+8)!=totalTiles)return false;
    std::array<bool,totalTiles> claimed{};
    std::array<std::uint32_t,sourceSprites> tails{};
    for(unsigned i=0;i<sourceSprites;++i){
        const auto first=word(p+12+i*8),tail=word(p+16+i*8);
        if(first!=i||!geometry(bank.sprites[i]))return false;
        claimed[i]=true;tails[i]=tail;
        if(tail==absent)continue;
        if(tail<sourceSprites||tail>=totalTiles||claimed[tail])return false;
        claimed[tail]=true;
        const auto& a=bank.sprites[i];const auto& b=bank.sprites[tail];
        if(!geometry(b)||a.right!=b.left||a.bottom!=b.bottom||a.top!=b.top||a.height!=b.height)return false;
    }
    for(bool used:claimed)if(!used)return false;
    for(auto frame:bank.frames)if(frame>=sourceSprites)return false;
    tails_=tails;bank_=&bank;return true;
}
std::array<const SpriteFrame*,2> UumuuQuirrelParts::current(const AnimationPlayer& player)const{
    if(!bank_||!player.clip)return {};
    // Reject a player from a different bank, including numerically valid indices.
    bool owned=false;for(const auto& clip:bank_->clips)if(&clip==player.clip){owned=true;break;}
    if(!owned||player.frame>=player.clip->count||player.clip->first>=bank_->frames.size()||
       player.frame>=bank_->frames.size()-player.clip->first)return {};
    const auto index=bank_->frames[player.clip->first+player.frame];
    if(index>=sourceSprites)return {};
    const auto tail=tails_[index];
    return {&bank_->sprites[index],tail==absent?nullptr:&bank_->sprites[tail]};
}
}
