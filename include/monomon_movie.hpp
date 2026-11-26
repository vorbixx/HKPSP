#pragma once
#include <citro2d.h>
#include <cstdio>
#include <cstdint>

namespace hk3ds {
// Render-thread adapter for the original MaskShatter frame stream. Completion
// follows the last decoded/presented frame; a missing/truncated stream fails.
class MonomonMovie {
public:
    bool start(const char* path);
    bool draw(double seconds);
    void release();
    bool active()const{return file_!=nullptr;}
    bool complete()const{return complete_;}
private:
    std::FILE* file_=nullptr;
    C3D_Tex textures_[2]{};
    Tex3DS_SubTexture region_{};
    std::uint16_t pixels_[200*112]{};
    double seconds_=0;
    unsigned last_=~0u,slot_=0;
    bool allocated_[2]{},complete_=false;
};
}
