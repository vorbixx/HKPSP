#pragma once
#include "ui_window.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hk3ds {
constexpr bool loadNoticesAllowed(bool playing,bool network,bool replay,bool benchmark,bool commands){
    return playing||!(network||replay||benchmark||commands);
}
struct LoadMemory { std::uint32_t linear=0,vram=0,heap=0; };
struct LoadTiming { char name[40]{};std::uint32_t ms=0; };
struct LoadDiagnostic {
    bool slow=false;
    char from[40]{},to[40]{},gate[72]{},stage[64]{},detail[256]{};
    std::uint32_t elapsedMs=0;
    LoadMemory memory;
    std::array<LoadTiming,16> timings{};
    unsigned timingCount=0;
};

// No allocation on a failure path. The active entry remains until a fresh A press.
class LoadDiagnostics {
public:
    using Clock=std::uint64_t(*)();
    using Memory=LoadMemory(*)();
    static constexpr unsigned capacity=8;
    static constexpr std::uint32_t slowLoadMs=3000; // Diagnostic threshold, not a game timing rule.
    void bind(Clock clock,Memory memory){clock_=clock;memory_=memory;}
    void begin(const char* from,const char* to,const char* gate,bool measureSlow=true){
        context_={};copy(context_.from,from);copy(context_.to,to);copy(context_.gate,gate);
        started_=stageStarted_=now();active_=true;failed_=false;measureSlow_=measureSlow;stage_[0]=0;
    }
    void stage(const char* name){finishStage();copy(stage_,name);stageStarted_=now();}
    void error(const char* code,const char* detail=nullptr){
        finishStage();auto report=snapshot();copy(report.stage,code);copy(report.detail,detail);
        enqueue(report);failed_=true;
    }
    void finish(){
        finishStage();auto report=snapshot();
        if(active_&&measureSlow_&&!failed_&&report.elapsedMs>slowLoadMs){report.slow=true;copy(report.stage,"slow_load");enqueue(report);}
        active_=false;
    }
    bool visible()const{return count_!=0;}
    unsigned count()const{return count_;}
    const LoadDiagnostic* current()const{return visible()?&queue_[head_]:nullptr;}
    void input(bool close,bool previous=false,bool next=false){
        (void)previous;(void)next; // L/R no longer page; the details are on the top screen.
        if(!visible())return;
        if(close){head_=(head_+1)%capacity;--count_;
            if(!count_&&overflow_){LoadDiagnostic report{};copy(report.stage,"diagnostic_queue_full");
                std::snprintf(report.detail,sizeof(report.detail),"%u further failures. See the diagnostic files.",overflow_);
                overflow_=0;enqueue(report);}
        }
    }
    // RPC playback cannot supply its first input before a frame is published.
    // A slow-load warning may be acknowledged by that explicit test adapter;
    // real load errors must retain their normal acknowledgement requirement.
    unsigned acknowledgeSlowLoads(){
        unsigned acknowledged=0;
        while(visible()&&current()->slow){input(true);++acknowledged;}
        return acknowledged;
    }
    // Bottom screen: the summary and the close action only. Details are on the top screen (drawDetails).
    // Returns false when the summary does not fit its text area.
    bool draw(ui::Canvas canvas){
        if(!visible()||!canvas.text)return true;
        const auto& r=*current();char body[512],right[32];
        std::snprintf(body,sizeof(body),"From: %s  To: %s\nGate: %s\nStage: %s\n%s%sLoad: %lu ms",
            r.from,r.to,r.gate,r.stage,r.detail,r.detail[0]?"\n":"",static_cast<unsigned long>(r.elapsedMs));
        std::snprintf(right,sizeof(right),"%u queued",count_);
        ui::bottomBackground(canvas);ui::window(canvas,r.slow?"Slow load":"Load error",right);
        const auto text=ui::wrappedText(canvas,body,summaryArea,ui::Size::Heading);
        const ui::Action actions[]={{"A","Close"}};
        ui::promptBar(canvas,actions,1);
        return !text.overflow;
    }
    // Top screen: the memory line across the full width, then every stage timing in two Heading columns
    // (16 timings = 2 x 8 rows under the memory line). No paging. Returns false when a line is wider than
    // its box; the caller decides what to do.
    bool drawDetails(ui::Canvas top)const{
        if(!visible()||!top.text)return true;
        const auto& r=*current();
        char line[96];bool fits=true;
        const auto put=[&](const char* text,float x,float y,float width){
            if(top.measure(top.user,text,ui::Size::Heading)>width)fits=false;
            top.text(top.user,text,x,y,ui::Size::Heading,ui::white);
        };
        std::snprintf(line,sizeof(line),"Free KiB L%lu V%lu H%lu",
            static_cast<unsigned long>(r.memory.linear/1024),static_cast<unsigned long>(r.memory.vram/1024),
            static_cast<unsigned long>(r.memory.heap/1024));
        put(line,detailFullX,detailFirstRow,detailFullWidth);
        if(r.timingCount>detailColumns*detailRowsPerColumn)fits=false;
        for(unsigned i=0;i<r.timingCount&&i<detailColumns*detailRowsPerColumn;++i){
            const unsigned column=i/detailRowsPerColumn,row=i%detailRowsPerColumn;
            std::snprintf(line,sizeof(line),"%s: %lu ms",r.timings[i].name,static_cast<unsigned long>(r.timings[i].ms));
            put(line,detailColumnX[column],detailFirstRow+(row+1)*ui::lineHeight(ui::Size::Heading),detailColumnWidth);
        }
        return fits;
    }
    // Paging was removed: L/R no longer change anything, so page() stays 0.
    unsigned page()const{return 0;}
    static constexpr ui::Rect summaryArea{26,36,268,180}; // 7 Heading lines
    static constexpr unsigned detailColumns=2,detailRowsPerColumn=8;
    static constexpr float detailColumnX[detailColumns]={8,208},detailColumnWidth=192,detailFullX=8,detailFullWidth=384,detailFirstRow=12;
private:
    template<std::size_t N>static void copy(char (&out)[N],const char* text){std::snprintf(out,N,"%s",text?text:"");}
    std::uint64_t now()const{return clock_?clock_():0;}
    LoadDiagnostic snapshot()const{auto r=context_;r.elapsedMs=active_?static_cast<std::uint32_t>(now()-started_):0;r.memory=memory_?memory_():LoadMemory{};return r;}
    void finishStage(){
        if(active_&&stage_[0]&&context_.timingCount<context_.timings.size()){
            auto& t=context_.timings[context_.timingCount++];copy(t.name,stage_);t.ms=static_cast<std::uint32_t>(now()-stageStarted_);
        }
        stage_[0]=0;
    }
    void enqueue(const LoadDiagnostic& report){if(count_==capacity){++overflow_;return;}queue_[(head_+count_)%capacity]=report;++count_;}
    Clock clock_=nullptr;Memory memory_=nullptr;
    std::array<LoadDiagnostic,capacity> queue_{};
    LoadDiagnostic context_{};
    char stage_[40]{};
    std::uint64_t started_=0,stageStarted_=0;
    unsigned head_=0,count_=0,overflow_=0;
    bool active_=false,failed_=false,measureSlow_=true;
};
inline LoadDiagnostics loadDiagnostics;
}
