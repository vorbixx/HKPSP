#pragma once
#include <cstdint>

namespace hk3ds {
// Read-only ARM acceptance mailbox; separate from the stable gameplay observer ABI.
struct EnemyAudioProbe {
    std::uint32_t sequence=0,version=2,ready=0;
    std::int32_t error=0;
    std::uint32_t linearBefore=0,linearAfter=0,ordinaryBefore=0,ordinaryAfter=0,residentBytes=0;
    std::uint32_t active=0,peak=0,chasePlaying=0,stoppedLoops=0;
    std::uint32_t dispatched[10]{},submitted[10]{},playing[10]{},completed[10]{};
    std::uint32_t tabletHumChannel=0,tabletHumSubmitted=0,tabletHumAudibleFrames=0,tabletWakePlaying=0;
    std::uint32_t sequenceEnd=0;
};
}
extern "C" { extern volatile hk3ds::EnemyAudioProbe hk3ds_enemy_audio; }
