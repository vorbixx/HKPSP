// Generated from build 22529139, scene level229 by tools/convert_uumuu_quirrel_watch.py.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace hk3ds::uumuu_quirrel_watch_data {
struct WatchPosition { float x, y, scaleX; std::string_view name, next; };
struct AudioReference { std::uint32_t file, path; };
inline constexpr std::string_view start="Idle";
inline constexpr std::string_view watchEvent="WATCH";
inline constexpr bool initialRoam=false;
inline constexpr bool initialVoiceIntro=false;
inline constexpr std::string_view initialPrevPos="";
inline constexpr std::array<std::string_view, 8> watchChoiceNames{{"BBL", "BBR", "BL", "BR", "TL", "TR", "TTL", "TTR"}};
inline constexpr std::array<WatchPosition, 8> watchPositions{{
    WatchPosition{42.799999237f, 117.819999695f, -1.000000000f, "BL", "BR"},
    WatchPosition{62.500000000f, 117.819999695f, 1.000000000f, "BR", "TL"},
    WatchPosition{38.630001068f, 122.919998169f, -1.000000000f, "TL", "TR"},
    WatchPosition{66.889999390f, 122.919998169f, 1.000000000f, "TR", "BBL"},
    WatchPosition{40.639999390f, 108.900001526f, -1.000000000f, "BBL", "BBR"},
    WatchPosition{64.309997559f, 108.900001526f, 1.000000000f, "BBR", "TTL"},
    WatchPosition{46.049999237f, 127.819999695f, -1.000000000f, "TTL", "TTR"},
    WatchPosition{60.290000916f, 127.819999695f, 1.000000000f, "TTR", "BL"},
}};
inline constexpr float pauseMin=0.500000000f;
inline constexpr float pauseMax=0.750000000f;
inline constexpr std::uint32_t positionSpace=0u;
inline constexpr float watchPositionZ=0.000000000f;
inline constexpr bool watchPositionZUseVariable=true;
inline constexpr float watchScaleY=0.000000000f;
inline constexpr bool watchScaleYUseVariable=true;
inline constexpr float watchScaleZ=0.000000000f;
inline constexpr bool watchScaleZUseVariable=true;
inline constexpr std::string_view watchClip="Quirrel Watch";
inline constexpr std::string_view watchBankClip="Quirrel/Quirrel Watch";
inline constexpr float animationWait=1.100000000f;
inline constexpr std::array<AudioReference, 5> voiceClips{{AudioReference{3u, 14u}, AudioReference{3u, 10u}, AudioReference{3u, 13u}, AudioReference{0u, 0u}, AudioReference{0u, 0u}}};
inline constexpr std::array<float, 5> voiceWeights{{1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f}};
inline constexpr AudioReference voiceIntroClip{3u, 11u};
inline constexpr AudioReference watchStartClip{3u, 7u};
inline constexpr AudioReference watchLeapClip{3u, 18u};
inline constexpr AudioReference audioPlayer{4u, 4126u};
inline constexpr float watchPitchMin=0.900000000f;
inline constexpr float watchPitchMax=1.100000000f;
}
