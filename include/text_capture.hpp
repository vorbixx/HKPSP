#pragma once
#include "build_config.hpp"
#include "test_frame_capture.hpp"
#if HK3DS_DIAGNOSTICS
#include <citro2d.h>
#include <algorithm>
#include <cmath>
extern "C" { extern volatile hk3ds::TestFrameCapture hk3ds_test_frames; }
#endif

namespace hk3ds::textcapture {
// Only diagnostic builds reserve/write the bounded mailbox. No heap or GPU work.
#if HK3DS_DIAGNOSTICS
inline unsigned screen=0;
inline TestTextBox boxes[2][textCaptureCapacity]{};
inline std::uint32_t counts[2]{},overflows[2]{};
inline TestTextBox* current=nullptr;
inline void select(unsigned value,bool clear=false) {
    screen=value;current=nullptr;
    if(clear){counts[value]=0;overflows[value]=0;}
}
struct String {
    TestTextBox* previous=current;
    explicit String(const char* value,std::uint32_t color=0xffffffff) {
        current=nullptr;
        if(!value||!*value)return;
        auto& count=counts[screen];
        if(count>=textCaptureCapacity){++overflows[screen];return;}
        current=&boxes[screen][count++];
        current->left=1e6f;current->top=1e6f;current->right=-1e6f;current->bottom=-1e6f;
        current->glyphHeight=0;current->color=color;
        unsigned i=0;for(;i<95&&value[i]&&value[i]!='\n';++i)current->text[i]=value[i];
        current->text[i]=0;
    }
    ~String(){current=previous;}
};
inline void glyph(float x,float y,float width,float height) {
    width=std::abs(width);height=std::abs(height);
    if(!current||width<=0||height<=0)return;
    C3D_Mtx view;C2D_ViewSave(&view);
    const float left=x*view.r[0].x+view.r[0].w,top=y*view.r[1].y+view.r[1].w;
    const float w=std::abs(width*view.r[0].x),h=std::abs(height*view.r[1].y);
    current->left=std::min(float(current->left),left);current->top=std::min(float(current->top),top);
    current->right=std::max(float(current->right),left+w);current->bottom=std::max(float(current->bottom),top+h);
    current->glyphHeight=std::max(float(current->glyphHeight),h);
}
// Keep the previous completed frame available throughout the next draw.
// An odd sequence guards the short publication copy against RPC reads.
inline void publish() {
    ++hk3ds_test_frames.textSequence;
    hk3ds_test_frames.textVersion=2;
    for(unsigned s=0;s<2;++s){
        for(unsigned i=0;i<counts[s];++i){
            const auto& source=boxes[s][i];auto& target=hk3ds_test_frames.textBoxes[s][i];
            target.left=source.left;target.top=source.top;
            target.right=source.right;target.bottom=source.bottom;
            target.glyphHeight=source.glyphHeight;target.color=source.color;
            for(unsigned j=0;j<96;++j)target.text[j]=source.text[j];
        }
        hk3ds_test_frames.textCount[s]=counts[s];
        hk3ds_test_frames.textOverflow[s]=overflows[s];
    }
    ++hk3ds_test_frames.textSequence;
}
#else
inline void select(unsigned,bool=false) {}
inline void publish() {}
struct String { explicit String(const char*,std::uint32_t=0xffffffff) {} };
inline void glyph(float,float,float,float) {}
#endif
// Preserve full multiline bounds without mistaking block height for glyph height.
inline void block(float x,float y,float width,float height,const char* text) {
    unsigned lines=1;for(auto p=text;p&&*p;++p)if(*p=='\n')++lines;
    const float lineHeight=height/lines;
    glyph(x,y,width,lineHeight);
    if(lines>1)glyph(x,y+height-lineHeight,width,lineHeight);
}
}
