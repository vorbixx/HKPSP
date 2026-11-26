#pragma once
#include "hero.hpp"
#include <array>
#include <cstdint>
namespace hk3ds {
enum class UumuuSound {ZapAntic,Zap,MultiZap,Intro,Enter,Exit,Slash,Wounded,
    Shout1,Shout2,Dialogue1,Dialogue2,Dialogue3,FinalHit,Gushing,Explode};
constexpr unsigned uumuuAudioCapBytes=9*1024*1024/4;
struct UumuuAudioEvent {std::uint32_t serial=0;UumuuSound sound{};Vec2 position{};float pitch=1,volume=1;};
// The bounded history survives several simulation steps per audio update.
struct UumuuAudioEvents {
    static constexpr unsigned capacity=64;
    std::array<UumuuAudioEvent,capacity> events{};
    std::uint32_t serial=0;
    void emit(UumuuSound sound,Vec2 position,float pitch=1,float volume=1){++serial;events[serial%capacity]={serial,sound,position,pitch,volume};}
};
}
