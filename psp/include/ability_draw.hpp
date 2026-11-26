#pragma once
#include "ability_pickup.hpp"
#include "hud_draw.hpp"

namespace hkpsp {
// The original item-message overlay; interaction clocks live in AbilityPickup.
void appendAbility(DrawList& list,const hk3ds::AbilityPickup& pickup,
                   const hk3ds::AnimationBank& art,unsigned page,const HudArt& text,
                   const hk3ds::AnimationBank* inventory=nullptr,unsigned inventoryPage=0);
void appendAbilityFlash(DrawList&,const hk3ds::AbilityPickup&,const hk3ds::AnimationBank&,unsigned page,hk3ds::Vec2 camera);
}
