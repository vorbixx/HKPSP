#pragma once
#include "draw_list.hpp"
#include "platform.hpp"
#include <algorithm>
#include <stdexcept>

namespace hkpsp {
inline hk3ds::AnimationBank loadWorldActorArt(Platform& platform,std::vector<Texture>& textures,const std::string& prefix){
    auto art=readAnimation(platform.path(prefix+"-visual.bin"));
    const unsigned first=unsigned(textures.size());
    for(unsigned index=0;index<art.pageCount;++index){
        auto page=readTexture(platform.path(prefix+"-page-"+std::to_string(index)+".hpt"));
        if(page.width!=512||page.height!=512)throw std::runtime_error("World actor texture size: "+prefix);
        textures.push_back(std::move(page));
    }
    for(auto& sprite:art.sprites)sprite.page+=first;
    return art;
}

class WorldActorArt {
    struct Resident {unsigned index;hk3ds::RoomDraw authored;bool replaced;};
    std::vector<Resident> resident_;
    unsigned firstSprite_=0,spriteCount_=0;
    bool refreshSprites_=false;
public:
    void clear(){*this=WorldActorArt{};}
    bool dynamic(unsigned index)const{
        return std::any_of(resident_.begin(),resident_.end(),[&](const auto& record){return record.index==index;});
    }
    template<class MovesResident>
    void bind(hk3ds::RoomVisualBank& room,const hk3ds::RoomVisualBank& bank,MovesResident movesResident,
              const hk3ds::AnimationBank* remap=nullptr){
        clear();
        const auto& atlas=remap?*remap:bank.atlas;
        if(atlas.sprites.size()!=bank.atlas.sprites.size())throw std::runtime_error("World actor atlas size mismatch");
        firstSprite_=unsigned(room.atlas.sprites.size());
        spriteCount_=unsigned(atlas.sprites.size());
        room.atlas.sprites.insert(room.atlas.sprites.end(),atlas.sprites.begin(),atlas.sprites.end());
        refreshSprites_=!remap;
        if(!remap){
            for(unsigned index=0;index<room.draws.size();++index){
                const auto& draw=room.draws[index];
                const bool replaced=std::any_of(bank.draws.begin(),bank.draws.end(),
                                               [&](const auto& actor){return actor.owner==draw.owner;});
                if(replaced||movesResident(draw.owner))resident_.push_back({index,draw,replaced});
            }
        }
    }
    template<class PresentDraw>
    void present(hk3ds::RoomVisualBank& room,hk3ds::RoomVisualBank& extras,const hk3ds::RoomVisualBank& bank,
                 PresentDraw presentDraw)const{
        if(!spriteCount_)return;
        if(bank.atlas.sprites.size()!=spriteCount_)throw std::runtime_error("World actor atlas changed after bind");
        if(refreshSprites_)std::copy(bank.atlas.sprites.begin(),bank.atlas.sprites.end(),room.atlas.sprites.begin()+firstSprite_);
        for(const auto& record:resident_){
            auto& draw=room.draws[record.index];draw=record.authored;presentDraw(draw);
            if(record.replaced)draw.values[10]=0;
        }
        for(auto draw:bank.draws){
            if(draw.values[10]<=0)continue;
            if(draw.sprite>=spriteCount_)throw std::runtime_error("World actor sprite outside admitted atlas");
            presentDraw(draw);
            draw.sprite+=firstSprite_;
            extras.draws.push_back(draw);
        }
    }
};
}
