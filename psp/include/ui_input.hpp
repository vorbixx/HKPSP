#pragma once
#include "input.hpp"
#include <cstdint>

namespace hkpsp {
// Presentation input is separate from the 60 Hz simulation latch and its replay.
enum UiButton : unsigned {
    UiUp=1u<<0, UiDown=1u<<1, UiLeft=1u<<2, UiRight=1u<<3,
    UiCircle=1u<<4, UiCross=1u<<5, UiStart=1u<<6, UiSelect=1u<<7,
    UiTriangle=1u<<8
};
struct UiInput {
    unsigned held=0,pressed=0,released=0;
    bool selectTap=false,selectHold=false,selectHoldPressed=false;
    int vertical()const{return int(bool(pressed&UiDown))-int(bool(pressed&UiUp));}
    int horizontal()const{return int(bool(pressed&UiRight))-int(bool(pressed&UiLeft));}
};
class UiInputRouter {
    unsigned previous_=0;
    double selectSeconds_=0;
    bool selectWasHold_=false;
public:
    // PSP tap/hold reservation for M4 / quick map; 8 locked-30-Hz samples reach it.
    static constexpr double selectHoldSeconds=.25;
    UiInput sample(unsigned held,double elapsed){
        UiInput out{held,held&~previous_,previous_&~held};
        if(held&UiSelect){
            if(!(previous_&UiSelect)){selectSeconds_=0;selectWasHold_=false;}
            if(elapsed>0)selectSeconds_+=elapsed;
            out.selectHold=selectSeconds_>=selectHoldSeconds;
            out.selectHoldPressed=out.selectHold&&!selectWasHold_;
            selectWasHold_=out.selectHold;
        }else if(previous_&UiSelect){
            out.selectTap=!selectWasHold_;
            selectSeconds_=0;selectWasHold_=false;
        }
        previous_=held;return out;
    }
};
// A menu's held buttons must be released before they can drive the hero again.
class GameplayMenuGuard {
    std::uint16_t blocked_=0;
public:
    void block(std::uint16_t held){blocked_|=held;}
    std::uint16_t filter(std::uint16_t held){blocked_&=held;return held&~blocked_;}
};
}
