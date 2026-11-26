#pragma once
#include "physics.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
struct CityTransportData {
    // Original trigger rectangles, root/landing/hero placement, waits and
    // tween speeds. iTween's non-None speed takes precedence over its time.
    std::array<float,22> values{};
    char destination[32]{},entry[32]{};
    Rect lever()const{return {values[0],values[1],values[2],values[3]};}
    Rect heroCheck()const{return {values[4],values[5],values[6],values[7]};}
    float x()const{return values[8];}
    float initial()const{return values[9];}
    float bottom()const{return values[10];}
    Vec2 heroOffset()const{return {values[11],values[12]};}
    float closeWait()const{return values[13];}
    float departureDelta()const{return values[14];}
    float departureSpeed()const{return values[15];}
    float departureWait()const{return values[16];}
    float fadeWait()const{return values[17];}
    float fadeTime()const{return values[18];}
    float arrivalOffset()const{return values[19];}
    float arrivalSpeed()const{return values[20];}
    float landWait()const{return values[21];}
    bool valid()const{
        for(float v:values)if(!std::isfinite(v)||std::abs(v)>100000)return false;
        return values[0]<values[2]&&values[1]<values[3]&&values[4]<values[6]&&values[5]<values[7]&&
            departureDelta()!=0&&arrivalOffset()!=0&&departureSpeed()>0&&arrivalSpeed()>0&&closeWait()>=0&&
            departureWait()>0&&fadeWait()>0&&fadeTime()>0&&landWait()>=0&&*destination&&*entry&&
            std::memchr(destination,0,sizeof(destination))&&std::memchr(entry,0,sizeof(entry));
    }
    bool load(const char* directory){
        if(!directory)return false;
        char path[512];if(std::snprintf(path,sizeof(path),"%s/city-transport.bin",directory)>=int(sizeof(path)))return false;
        auto* f=std::fopen(path,"rb");if(!f)return false;CityTransportData next;char magic[4];
        const bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"CTP1",4)&&
            std::fread(next.values.data(),4,22,f)==22&&std::fread(next.destination,1,32,f)==32&&
            std::fread(next.entry,1,32,f)==32&&std::fgetc(f)==EOF&&next.valid();
        std::fclose(f);if(ok)*this=next;return ok;
    }
};
// FSM851/1106. Persistence, scene loading, art and physics belong to adapters.
// The departure tween continues during Fade (source stopOnExit=false).
class CityLiftTransport {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,Closing,Departing,Fading,NewScene,Arriving,Landing};
    bool bind(const CityTransportData& data){if(!data.valid())return false;clear();data_=data;y_=data.bottom();phase_=Phase::Idle;return true;}
    void clear(){*this={};}
    bool bound()const{return phase_!=Phase::Unbound;}
    bool locked()const{return bound()&&phase_!=Phase::Idle;}
    bool ready()const{return phase_==Phase::NewScene;}
    Phase phase()const{return phase_;}
    float y()const{return y_;}
    float fade()const{return phase_==Phase::NewScene?1:phase_==Phase::Fading?std::min(1.f,(elapsed_-data_.departureWait())/data_.fadeTime()):0;}
    const CityTransportData& data()const{return data_;}
    bool takeRelease(){const bool r=release_;release_=false;return r;}
    bool depart(bool unlocked,bool heroInside,bool nailHit){return depart(unlocked,heroInside,nailHit,data_.bottom());}
    // fromY is the cage's current height: the upper station leaves a raised, unpaid cage
    // (original Move Out is iTweenMoveBy from the current position, not from the lowered endpoint).
    bool depart(bool unlocked,bool heroInside,bool nailHit,float fromY){
        if(phase_!=Phase::Idle||!unlocked||!heroInside||!nailHit||!std::isfinite(fromY))return false;
        phase_=Phase::Closing;elapsed_=0;departFrom_=y_=fromY;return true;
    }
    bool arrive(){
        if(phase_!=Phase::Idle)return false;
        phase_=Phase::Arriving;elapsed_=0;y_=data_.bottom()+data_.arrivalOffset();return true;
    }
    bool cancelArrival(){
        if(phase_!=Phase::Arriving&&phase_!=Phase::Landing)return false;
        phase_=Phase::Idle;elapsed_=0;y_=data_.bottom();release_=false;return true;
    }
    void update(float dt){
        if(!bound()||!std::isfinite(dt)||dt<0||phase_==Phase::Idle||ready())return;
        elapsed_+=dt;
        if(phase_==Phase::Closing){if(elapsed_>=data_.closeWait()){phase_=Phase::Departing;elapsed_=0;}}
        else if(phase_==Phase::Departing||phase_==Phase::Fading){
            const float u=std::min(1.f,elapsed_*data_.departureSpeed()/std::abs(data_.departureDelta()));
            y_=departFrom_+data_.departureDelta()*u*u*u;
            if(elapsed_>=data_.departureWait()+data_.fadeWait())phase_=Phase::NewScene;
            else if(elapsed_>=data_.departureWait())phase_=Phase::Fading;
        }else if(phase_==Phase::Arriving){
            const float u=std::min(1.f,elapsed_*data_.arrivalSpeed()/std::abs(data_.arrivalOffset())),v=1-u;
            y_=data_.bottom()+data_.arrivalOffset()*v*v*v;
            if(u>=1){phase_=Phase::Landing;elapsed_=0;}
        }else if(phase_==Phase::Landing&&elapsed_>=data_.landWait()){phase_=Phase::Idle;elapsed_=0;release_=true;}
    }
private:
    CityTransportData data_{};float y_=0,departFrom_=0,elapsed_=0;Phase phase_=Phase::Unbound;bool release_=false;
};
static_assert(sizeof(CityLiftTransport)<256,"City transport has fixed state and no allocation");
}
