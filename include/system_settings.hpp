#pragma once
#include <cstdint>
#include "graphics_profile.hpp"
namespace hk3ds {
// System-wide options are separate from character/slot progress.
struct SystemSettings {
    enum class CameraShake : std::uint8_t { On, Reduced, Off };
    enum class HudAppearance : std::uint8_t { Large, Small };
    std::uint8_t musicVolume=10,soundVolume=10;
    GraphicsMode graphics=GraphicsMode::Auto;
    std::uint8_t masterVolume=10;
    CameraShake cameraShake=CameraShake::On;
    HudAppearance hudAppearance=HudAppearance::Small;
    bool backerCredits=false;
    float cameraShakeMultiplier()const {
        return cameraShake==CameraShake::On?1.f:cameraShake==CameraShake::Reduced?.2f:0.f;
    }
    bool load(const char* path);
    bool write(const char* path) const;
};
}
