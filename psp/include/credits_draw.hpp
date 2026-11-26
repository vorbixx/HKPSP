#pragma once
#include "text_draw.hpp"

namespace hk3ds { class OpeningCredits; }
namespace hkpsp {
// main.cpp:1015-1018: the opening credits draw before the blanker, so the blanker covers them.
// Borrows the TextCanvas; the Crossroads boss suppresses them as on the 3DS.
void appendCredits(TextCanvas&,const hk3ds::OpeningCredits&,bool crossroadsBossActive);
}
