#pragma once
// The one set of helpers every bottom-screen window uses (old Iselda shop style).
// New UI MUST be built from these: no plates, no black bars, no boxed button glyphs.
// Geometry and colours come from the pre-2026-11-03 Iselda shop (HudRenderer::drawShop).
#include "ui.hpp"

namespace hk3ds::ui {
// The ornamented window: dark fill, "Dialogue Top" ornament at y=10, bottom ornament under the hints.
inline constexpr Rect windowRect{8,10,304,230};
inline constexpr std::uint32_t windowColor=0xff0f0806,highlightColor=0xff3e302b;
// Selection list rows: first baseline and step (three rows fit above the hint lines).
inline constexpr float rowFirstBaseline=64.f,rowStep=48.f;
// Window frame + optional left title (x=26) and right-hand label centred at x=265, both at baseline 29.
void window(Canvas,const char* title=nullptr,const char* right=nullptr);
// Window text: Heading size, white, struck twice half a pixel apart (as the old shop did).
void label(Canvas,const char*,float x,float baseline,bool centred=false,std::uint32_t colour=white);
// One list row in the window: highlight band and ">" marker when selected, name at x=65 (icon room) or
// x=36, value centred at x=268. Caller draws icons/coins; returns nothing.
void selectionRow(Canvas,float baseline,bool selected,const char* name,const char* value=nullptr,bool iconRoom=false);
// Bottom hints are ui::promptBar / ui::hintLines (plain text, "KEY: Label", last baseline 226).

// ---- Choice dialog (toll machines, "Insert Geo?", shop confirmation) -------------------------------------
// RULE (user 2026-11-08): a choice is not a prompt. Every Yes/No choice and every menu the player selects from
// goes on the BOTTOM screen in this window style, at Heading size (13 px capitals, the minimum for dialogs).
// The top screen keeps only world arrow prompts and passive text. Layout: ornamented window, the question
// centred in Heading size, the Geo coin with the price, "> Yes   No" (selection marker; Yes greyed with the
// marker kept when the player cannot pay), one info line ("You have N Geo" or "Not enough Geo."), plain hints.
struct DialogCanvas { Canvas body; Canvas menu; void* user; void (*icon)(void*,const char*,Rect); };
struct ChoiceDialog {
    const char* question;
    unsigned price;
    bool yes;                  // Yes is the selected choice
    bool affordable=true;      // false: Yes is greyed and the info line reads "Not enough Geo."
    const char* info=nullptr;  // optional line under the choices ("You have 500 Geo", a result message)
    const char* confirmKey="A";
    const char* cancelKey="B";
    bool showPrice=true,showChoices=true;
};
void choiceDialog(const DialogCanvas&,const ChoiceDialog&);

// ---- Stag station menu (user layout 1, 2026-11-08) --------------------------------------------------------
// Bottom screen: the "Stations" list in the window (opened stations only; the current one carries the
// stag icon and the highlight bar when it is selected). Top screen: the stag-network map over the dimmed
// scene with ornaments, opened stations as icons at their map position, the selected one ringed.
// The menu logic and station flags belong to the caller (task/stag-stations-merge); it only passes data.
struct StagStation {
    const char* name;
    bool opened;       // station already unlocked: listed and shown on the map
    bool current;      // the station the player is at (stag icon next to the name)
    float mapX,mapY;   // 0..1 inside the network map area (left/top = 0)
};
struct StagMenuView { const StagStation* stations; unsigned count; unsigned selected; /* index into stations */ };
inline constexpr unsigned stagVisibleRows=4;
inline constexpr float stagRowFirstBaseline=64.f,stagRowStep=36.f;
// Bottom window: title "Stations", list rows, hints "A: Travel   B: Cancel" (plain text).
void stagStationList(const DialogCanvas&,const StagMenuView&);
// Hit test for touch: returns the station index under the point (bottom screen coordinates) or -1.
int stagStationAt(const StagMenuView&,float x,float y);
// Top screen map; `icon` receives "Stag" (opened station), "StagCurrent" and "StagCursor" (selection ring).
inline constexpr Rect stagMapRect{56,40,288,150};
void stagStationMap(const DialogCanvas&,const StagMenuView&);
}
