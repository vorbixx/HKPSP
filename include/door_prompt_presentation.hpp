#pragma once
#include "room.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace hk3ds {
struct PromptMarkerDraw {
    RoomGate gate{};
    const RoomGate* target=nullptr;
    float alpha=0,age=0;
    bool visible=false,down=false;
};

// Existing gameplay Arrow Prompt adapter; the newer door prefab has its own pool.
struct ArrowPromptPresentation : PromptMarkerDraw {
    bool authoredAnchor=false;
    void reset() {*this={};}
    void update(float seconds,const RoomGate* next) {
        if(!std::isfinite(seconds)||seconds<0)return;
        if(next!=target){
            target=next;age=0;down=!next;
            if(next){gate=*next;visible=true;}
        }
        if(!visible)return;
        age+=seconds;
        alpha=std::clamp(alpha+(down?-seconds/.233f:seconds/.4f),0.f,1.f);
        if(down&&age>=13.f/30.f)visible=false;
    }
};

// Arrow Prompt New: Hide clears the FSM reference immediately, while the old
// object continues Down until its scaled RecycleDelayed(.233) coroutine resumes.
struct DoorPromptPresentation : PromptMarkerDraw {
    struct Fading {
        PromptMarkerDraw draw;
        float recycleSeconds=0;
        bool startFrame=true;
    };
    std::vector<Fading> fading;

    DoorPromptPresentation(){fading.reserve(16);} // capacity, never a marker limit
    void reset(){static_cast<PromptMarkerDraw&>(*this)={};fading.clear();}
    void update(float seconds,const RoomGate* next){
        if(!std::isfinite(seconds)||seconds<0)return;
        if(next!=target){
            if(visible){
                auto old=static_cast<const PromptMarkerDraw&>(*this);
                old.target=nullptr;old.down=true;old.age=0;
                fading.push_back({old,0,true});
            }
            static_cast<PromptMarkerDraw&>(*this)={};
            if(next){gate=*next;target=next;visible=true;}
        }
        if(visible){age+=seconds;alpha=std::min(age/.4f,1.f);}
        for(auto i=fading.begin();i!=fading.end();){
            auto& old=*i;
            old.draw.age+=seconds;
            // FadeDown starts with fadeOutTime, independently of current alpha.
            old.draw.alpha=std::max(0.f,1.f-old.draw.age/.233f);
            if(!old.startFrame)old.recycleSeconds+=seconds;
            old.startFrame=false;
            if(old.recycleSeconds>=.233f)i=fading.erase(i);else ++i;
        }
    }
};
}
