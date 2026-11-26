#pragma once
#include <cstddef>
#include <cstdint>

namespace hk3ds::ui {
struct Rect { float x,y,w,h; };
enum class Size { Body, Heading, Title, Compact };
enum class Art { FrameTop, FrameBottom, Pointer, PointerRight };
// Use native-LCD rasters; the smaller .75 raster failed visual review.
constexpr float scale(Size size) { return size==Size::Body?1.15f:1.4f; }
constexpr float lineHeight(Size size) { return size==Size::Compact?16.f:size==Size::Body?20.f:size==Size::Heading?24.f:28.f; }
constexpr float baselineOffset(Size size) { return size==Size::Compact?13.f:size==Size::Body?14.f:size==Size::Heading?17.f:19.f; }
constexpr std::uint32_t white=0xfff7f3ef,secondary=0xffd8c9bf;
constexpr std::uint32_t bottomBackgroundColor=0xff140c09;
// Fixed-capacity, allocation-free adapter. Text coordinates are left/top.
struct Canvas {
    void* user;
    float (*measure)(void*,const char*,Size);
    void (*text)(void*,const char*,float,float,Size,std::uint32_t);
    void (*fill)(void*,Rect,std::uint32_t);
    void (*art)(void*,Art,Rect)=nullptr;
};
struct TextResult { std::size_t consumed; unsigned lines; bool overflow; };
struct PageResult { unsigned page,pages; };
struct Action { const char* key; const char* label; };
void panel(Canvas,Rect,bool backing=true);
void bottomBackground(Canvas);
void unavailableMap(Canvas);
TextResult wrappedText(Canvas,const char*,Rect,Size=Size::Body,std::uint32_t=white);
// Wrap the full page, then reveal bytes within each centred line. Unrevealed
// characters still determine line breaks and origins; UTF-8 scalars stay whole.
TextResult readingText(Canvas,const char*,Rect,unsigned visible,std::uint32_t=white);
PageResult pagedText(Canvas,const char*,Rect,unsigned page,Size=Size::Body);
void title(Canvas,const char*,Rect);
TextResult detail(Canvas,const char* name,const char* value,const char* description,Rect,bool backing=true);
void listRow(Canvas,Rect,const char* name,const char* value,bool selected,const char* icon=nullptr);
// Source-style menus use centred small caps and paired fleur arrows, without boxes.
void menuRow(Canvas,Rect,const char* name,const char* value,bool selected);
// Bottom-screen hints: plain centred text lines inside the interface window, in the
// window's own font and colour (the pre-2026-11-03 Iselda shop style: no plate, no
// background bar, no boxed button glyphs). Baselines 226 for the last line, 16 px apart,
// above the window's bottom ornament. Returns the height reserved at the screen bottom.
constexpr float hintLineStep=16.f,hintLastBaseline=226.f;
float hintLines(Canvas,const char* const* lines,unsigned count);
// Joins actions as "KEY: Label" with three spaces, wraps to the screen width and draws them
// through hintLines. A key "Left/Right" is written "L/R", "Up/Down" "U/D".
float promptBar(Canvas,const Action*,unsigned,float width=320);
TextResult dialogue(Canvas,const char* speaker,const char* body,const char* action="Continue");
unsigned firstVisible(unsigned selected,unsigned first,unsigned count,unsigned visible);
// Stable across typewriter frames; changes when a reused dialogue buffer changes.
std::uint64_t textIdentity(const char*);
}

#ifdef __3DS__
#include <citro2d.h>
namespace hk3ds::ui {
// Native system-font diagnostics use the shared submission boundary without
// changing the game's original-font sizes or style.
void diagnosticText(C2D_TextBuf,const char*,float x,float y,float scale=.4f);
}
#endif
