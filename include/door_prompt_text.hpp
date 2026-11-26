#pragma once
#include "door_prompt_glyphs.hpp"

namespace hk3ds::doorfont {
constexpr unsigned uppercase(unsigned value) {
    return value>='a'&&value<='z'?value-'a'+'A':value;
}
inline bool measure(const char* text,float& width) {
    width=0;
    if(!text||!*text)return false;
    for(const auto* character=text;*character;++character){
        const unsigned value=uppercase(static_cast<unsigned char>(*character));
        if(value<32||value>95)return false;
        width+=advance[value-32];
    }
    return true;
}
}
