#pragma once
#include "hud_draw.hpp"
#include "ui.hpp"

namespace hk3ds { class TutorialSession; struct ShopMenuView; struct TollMenu; struct LoreTabletBank; class LoreInspection; class AreaTitle; class TutorialPrompts; class ConditionalEvents; class Cartographer; class Resting; class GreenpathEncounter; }
namespace hkpsp {
struct ActorArt;
// Original TextContainer resources:23239 and RectTransform resources:18836,
// projected at the same 1080p HUD density / 4 as the other single-screen UI.
inline constexpr hk3ds::ui::Rect loreTextRect{
    240-17.947506f*hudPixelsPerUnit*.5f,
    136-(4.49f+4.110867f*.5f)*hudPixelsPerUnit,
    17.947506f*hudPixelsPerUnit,4.110867f*hudPixelsPerUnit};
// main.cpp:953-957 precedence: the Crossroads boss title (not while Dormant), else the Hornet
// title, else the area title. A boss title suppresses the area title even at zero alpha.
enum class TitleKind { None, Boss, Hornet, Area };
inline TitleKind selectTitle(bool bossActive,bool bossDormant,bool inspectionLocked,float hornetAlpha){
    if(bossActive&&!bossDormant)return TitleKind::Boss;
    if(hornetAlpha>0)return TitleKind::Hornet;
    return inspectionLocked?TitleKind::None:TitleKind::Area;
}
// Borrows the resident fonts and draw list. Font quads always use native texels.
class TextCanvas {
    DrawList& list;
    const FontBank& fonts;
    unsigned fontPage;
    const HudArt* art=nullptr;
    unsigned solidPage=0;
    Point solid{};
    const AnimationAtlas* shopIcons=nullptr;
    const AnimationAtlas* regionTitleArt=nullptr;
    static unsigned font(hk3ds::ui::Size);
    float measure(const char*,hk3ds::ui::Size)const;
    void text(const char*,float,float,hk3ds::ui::Size,std::uint32_t);
    void fill(hk3ds::ui::Rect,std::uint32_t);
    void sprite(const char*,Point,float,float,float,const AnimationAtlas* = nullptr,float seconds=0.f);
    // Menu atlas page that holds the selection wings; wings are skipped until it is set.
    unsigned menuArrowPage=0;
    bool hasMenuArrowPage=false;
    // Shared Sly/Iselda shop screen (shop_draw.cpp); returns the description page count.
    unsigned drawShopView(const hk3ds::ShopMenuView&);
public:
    // One original NPC dialogue page (panel, ornaments, marker); the test fixture draws it directly.
    void dialoguePage(const char* source,bool pageEnd,bool more,const ActorArt&);
    TextCanvas(DrawList&,const FontBank&,unsigned fontPage);
    TextCanvas(DrawList&,const HudArt&);
    void setMenuArrowPage(unsigned page){menuArrowPage=page;hasMenuArrowPage=true;}
    hk3ds::ui::Canvas canvas();
    void prepareLore(hk3ds::LoreTabletBank&);
    void setShopIcons(const AnimationAtlas& icons){shopIcons=&icons;}
    void setRegionTitleArt(const AnimationAtlas& atlas){regionTitleArt=&atlas;}
    void slyShop(const hk3ds::TutorialSession&);
    // Iselda's purchase step (Prompt) on the same shop screen; geo is the save's Geo.
    void iseldaShop(const hk3ds::Cartographer&,unsigned geo);
    void lore(const hk3ds::LoreInspection&);
    void areaTitle(const hk3ds::AreaTitle&);
    // Boss or Hornet title text at the original fight-start place (see text_draw.cpp).
    void bossTitle(const char* name,float alpha);
    void focus(const hk3ds::TutorialPrompts&);
    void reminder(const hk3ds::TutorialPrompts&);
    void dialogue(const hk3ds::ConditionalEvents&,const ActorArt&);
    // Cornifer: the same original dialogue box as NPC events (no heading, no hints).
    void cartographer(const hk3ds::Cartographer&,unsigned geo,const ActorArt&);
    // Hornet's Greenpath intro lines in the same original dialogue box; nothing while the intro is inactive.
    void greenpathIntro(const hk3ds::GreenpathEncounter&,const ActorArt&);
    // Original Yes/No box (yn-original frame-210): question, Geo price, Yes/No, wings on the choice.
    // Defaults draw Cornifer's box unchanged. wrapQuestion re-wraps a long question over several lines;
    // info is the line under the choices (replaced by "Not enough Geo." when !affordable, which greys Yes).
    void choiceBox(const char* question,unsigned price,bool yes,const ActorArt&,
                   const char* info=nullptr,bool affordable=true,bool showPrice=true,bool showChoices=true,
                   bool wrapQuestion=false,const char* yesLabel="Yes",const char* noLabel="No");
    // Route toll machine (HudRenderer::drawRouteToll port): crest, or a gate/lift toll with the Geo price.
    void stagToll(const hk3ds::Resting&,unsigned geo,const ActorArt&);
    void stagDialogue(const hk3ds::Resting&,const ActorArt&);
    void stagTravel(const hk3ds::Resting&,const ActorArt&);
    void routeToll(const hk3ds::TollMenu&,unsigned geo,const ActorArt&);
};
// menu_runtime.cpp: 16x16 selection wing from the menu atlas page.
void appendMenuArrow(DrawList&,unsigned page,float x,float y,bool right);
void appendText(DrawList&,TextCanvas&,const ActorArt&,
                const hk3ds::TutorialSession&,hk3ds::Vec2 camera);
// True while an NPC dialogue box (events, Cornifer, Hornet intro) is on screen; the HUD hides then, as in the original.
bool dialogueBoxOpen(const hk3ds::TutorialSession&);
}
