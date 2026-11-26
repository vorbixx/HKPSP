#pragma once

namespace hk3ds {
enum class SurfaceSwimPhase { None, Enter, Idle, Moving, Jump, JumpFrame };
struct SurfaceSwimState {
    SurfaceSwimPhase phase=SurfaceSwimPhase::None;
    bool introductory=true;
    float entrySeconds=0;
    bool active() const { return phase!=SurfaceSwimPhase::None; }
};
static_assert(sizeof(SurfaceSwimState)<=32,"Surface swim state stays within its New 3DS estimate");
}
