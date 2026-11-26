#pragma once
#include "room_visual.hpp"
#include <limits>

#ifndef HK3DS_ROOM_VISIBLE_LIST
#define HK3DS_ROOM_VISIBLE_LIST 1
#endif
namespace hk3ds {
// Build-time A/B selection (make ROOM_VISIBLE_LIST=0): every pass scans the bank again.
inline constexpr bool roomVisibleListEnabled=HK3DS_ROOM_VISIBLE_LIST!=0;
// Original bank indices stay sorted, so every painter-order subrange can use
// the same list. Dynamic alpha, light/blur splits and projection remain per pass.
class RoomVisibleDraws {
    const RoomVisualBank* bank_=nullptr;
    float x_=0,y_=0,maxDepth_=0;
    bool grass_=true;
    std::vector<std::uint16_t> indices_;
public:
    void invalidate() {bank_=nullptr;indices_.clear();}
    bool prepare(const RoomVisualBank& bank,float x,float y,bool grass,float maxDepth) {
        invalidate();
        if(bank.draws.size()>std::size_t(std::numeric_limits<std::uint16_t>::max())+1)return false;
        indices_.reserve(bank.draws.size());
        for(std::size_t i=0;i<bank.draws.size();++i){
            const auto& draw=bank.draws[i];
            if(!bank.cameraVisible(i,x,y)||!roomDrawHasOpacity(draw)||
               std::abs(draw.values[6])>=maxDepth)continue;
            if(!grass&&(draw.grass.amplitude!=0||draw.grass.push!=0))continue;
            indices_.push_back(std::uint16_t(i));
        }
        bank_=&bank;x_=x;y_=y;grass_=grass;maxDepth_=maxDepth;
        return true;
    }
    bool matches(const RoomVisualBank& bank,float x,float y,bool grass,float maxDepth) const {
        return bank_==&bank&&x_==x&&y_==y&&grass_==grass&&maxDepth_==maxDepth;
    }
    const auto& indices() const {return indices_;}
    std::size_t lowerBound(std::size_t index) const {
        return std::lower_bound(indices_.begin(),indices_.end(),index)-indices_.begin();
    }
};
}
