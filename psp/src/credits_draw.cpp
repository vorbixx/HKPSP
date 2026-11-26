#include "credits_draw.hpp"
#include "opening_credits.hpp"
#include <algorithm>

namespace hkpsp {
namespace {
// Original 1920x1080 frames (results/opening-1628, opening-1972) downscaled exactly 4x to 480x270 [M].
// All credit lines are left-aligned at x 348 (first ink column of "a Game by", "Team Cherry", "Ari Gibson").
// Names: cap height 8.5 px, baseline 232. Captions: baseline 219.5, cap height about 5.5 px [M].
constexpr float leftX=348.f;
constexpr float captionBaseline=219.5f;
constexpr float nameBaseline=232.f;
}
void appendCredits(TextCanvas& text,const hk3ds::OpeningCredits& credits,bool crossroadsBossActive)
{
    if(crossroadsBossActive||!credits.active())return;
    // Perpetua body size only (docs/psp/fonts.md accepted sizes). Names match the original's Perpetua
    // 15 px (9 px caps). Captions are drawn at the same size: the original's caption caps are about
    // 5.5 px, below the 9 px floor, so they are larger than the original [open difference].
    const auto size=hk3ds::ui::Size::Body;
    const auto c=text.canvas();
    for(const auto& label:hk3ds::creditsdata::texts){
        const float alpha=credits.alpha(label);
        if(!(alpha>0))continue;
        const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
        const bool caption=label.scale<.6f;
        const float baseline=caption?captionBaseline:nameBaseline;
        c.text(c.user,label.value,leftX,baseline-hk3ds::ui::baselineOffset(size),size,tint);
    }
}
}
