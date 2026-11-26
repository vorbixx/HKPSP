#pragma once
#ifdef HKPSP_DIAG
#define HKPSP_DIAG_ONLY(...) __VA_ARGS__
namespace hk3ds {struct TutorialSession;}
namespace hkpsp {struct FrameInfo;struct PageCounters;}
namespace hkpsp::diag {
void initialize(unsigned& frames,unsigned& every,bool& test);
void start(hk3ds::TutorialSession& game);
void controller(unsigned& buttons,unsigned char& lx,unsigned char& ly);
void begin();
void draw();
void page(unsigned id,bool evicted,const void* slot);
void readback(void* offset);
void finishWork();
void displayed(const FrameInfo&,const PageCounters&,unsigned quads);
void nextWork();
void shutdown();
}
#else
#define HKPSP_DIAG_ONLY(...)
#endif
