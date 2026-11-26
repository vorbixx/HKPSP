#pragma once
#include "opening_credits_data.hpp"
#include "opening_blanker_data.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
// Source Animator uses ordinary, unweighted Hermite curves on TMP font alpha.
class OpeningCredits {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned playedFlag=512;
    void begin(bool previouslyPlayed=false)
    {
        active_=!played_&&!previouslyPlayed;played_=true;seconds_=0;
    }
    void advance(float seconds)
    {
        if(active_&&std::isfinite(seconds)&&seconds>0){
            seconds_+=seconds;
            if(seconds_>=creditsdata::keys[29].seconds)active_=false;
        }
    }
    bool active() const {return active_;}
    float seconds() const {return seconds_;}
    float blankerAlpha() const
    {
        // HeroController top entry -> Pre Fade/NextFrameEvent -> FadeIn/FADE OUT.
        // Source EaseColor is linear and unscaled; the opening has no slow motion.
        constexpr float start=openingdata::entryWait+1.f/60+openingdata::fadeDelay;
        return active_?std::clamp(1-(seconds_-start)/openingdata::fadeTime,0.f,1.f):0;
    }
    unsigned savedFlags() const {return played_?playedFlag:0;}
    float alpha(const creditsdata::Text& text) const {return active_?sample(text,seconds_):0;}
    static float sample(const creditsdata::Text& text,float seconds)
    {
        if(!std::isfinite(seconds))return 0;
        const auto* keys=creditsdata::keys+text.first;
        if(seconds<=keys[0].seconds)return keys[0].value;
        for(unsigned i=1;i<text.count;++i)if(seconds<keys[i].seconds){
            const auto& a=keys[i-1];const auto& b=keys[i];const float dt=b.seconds-a.seconds;
            const float t=(seconds-a.seconds)/dt,t2=t*t,t3=t2*t;
            return std::clamp((2*t3-3*t2+1)*a.value+(t3-2*t2+t)*dt*a.outSlope+
                              (-2*t3+3*t2)*b.value+(t3-t2)*dt*b.inSlope,0.0f,1.0f);
        }
        return keys[text.count-1].value;
    }
private:
    float seconds_=0;
    bool active_=false,played_=false;
};
}
