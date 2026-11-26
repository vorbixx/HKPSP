#pragma once
#include <array>
#include <cstddef>
namespace hk3ds::corniferdata {
inline constexpr float x=36.31f, y=34.69f, scaleX=1.1172f, scaleY=1.1451297f;
struct Text { const char* key; const char* text; };
inline constexpr Text texts[] = {
  {"CORNIFER_MEET", "@Cornifer/CORNIFER_MEET"},
  {"CROSSROADS_GREET", "@Cornifer/CROSSROADS_GREET"},
  {"CORNIFER_AGAIN", "@Cornifer/CORNIFER_AGAIN"},
  {"CORNIFER_PROMPT", "@Cornifer/CORNIFER_PROMPT"},
  {"CORNIFER_REFUSE", "@Cornifer/CORNIFER_REFUSE"},
  {"CORNIFER_NOT_ENOUGH", "@Cornifer/CORNIFER_NOT_ENOUGH"},
  {"CROSSROADS_BOUGHT", "@Cornifer/CROSSROADS_BOUGHT"},
  {"CORNIFER_CARD", "@Cornifer/CORNIFER_CARD"},
  {"MAP_NAME_CROSSROADS", "@Cornifer/MAP_NAME_CROSSROADS"},
};
struct Paper { const char* scene; float x,y,scaleX,scaleY; const char* clip; };
inline constexpr std::array<Paper,0> papers{{
}};
} // namespace hk3ds::corniferdata
