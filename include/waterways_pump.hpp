#pragma once
#include "combat.hpp"
#include "hero.hpp"

namespace hk3ds {
// Waterways_05 GO577 Switch Control (level354-3001), build 22529139.
// Range Check GO576 and CompositeCollider2D 1986 have separate purposes:
// the hero must be in range while an actual Nail Attack touches the crank.
inline bool waterwaysPumpHit(const Hero& hero,const NailBank& nails)
{
    constexpr Rect range{98.220438f,12.501138f,109.341690f,24.939732f};
    constexpr std::array<Vec2,4> crank{{{103.938268f,17.151951f},
        {102.808881f,17.624627f},{101.798192f,14.065201f},{102.742887f,13.337450f}}};
    return hero.nail.hitbox&&overlaps(bodyBounds(hero.position),range)&&
        nailOverlapsQuad(hero,nails.shapes[nailVariant(hero.nail)],crank);
}
}
