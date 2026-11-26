#pragma once
#include "animation.hpp"
#include <array>
#include <cstddef>

namespace hk3ds {
// Original Quirrel art has 33 sprites; two wide slash sprites each need one
// extra native-pixel tile. The bank must remain immutable and outlive this view.
class UumuuQuirrelParts {
public:
    static constexpr unsigned sourceSprites=33, totalTiles=35;
    static constexpr std::size_t encodedBytes=12+sourceSprites*8;
    bool load(const AnimationBank&,const char* path);
    bool bind(const AnimationBank&,const void* data,std::size_t bytes);
    void reset(){bank_=nullptr;}
    // Both parts use the actor's same transform, color, depth and facing.
    // Their authored local bounds join at the native-pixel split boundary.
    std::array<const SpriteFrame*,2> current(const AnimationPlayer&)const;
private:
    const AnimationBank* bank_=nullptr;
    std::array<std::uint32_t,sourceSprites> tails_{};
};
static_assert(sizeof(UumuuQuirrelParts)<=160,"Quirrel tile mapping must remain bounded");
}
