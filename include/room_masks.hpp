#pragma once
#include "physics.hpp"
#include "scene_bool_save.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
namespace hk3ds {
struct RoomMask {
    bool once=false,sourceTween=false,discoverySound=false;
    SceneBoolSave::Entry persistent{};
    float pause=0,seconds=.5f,leaveSeconds=.5f,reloadSeconds=.1f,inside=0,outside=1,initial=1;
    std::vector<unsigned> owners;
    std::vector<std::vector<Vec2>> polygons;
};
inline bool loadRoomMasks(const char* path,std::vector<RoomMask>& masks){
    auto* file=std::fopen(path,"rb");if(!file){masks.clear();return true;}
    const auto read=[file](void* out,std::size_t n){return std::fread(out,1,n,file)==n;};
    char magic[4];unsigned count=0;
    bool valid=read(magic,4);
    const bool extended=valid&&!std::memcmp(magic,"HKM2",4);
    valid=valid&&(extended||!std::memcmp(magic,"HKM1",4))&&read(&count,4)&&count<=128;
    std::vector<RoomMask> next;
    for(unsigned i=0;valid&&i<count;++i){
        RoomMask mask;unsigned flags=0,nOwners=0,nPolygons=0;float v[7]{};
        valid=read(&flags,4)&&flags<=(extended?7u:1u)&&(!(flags&6)||(flags&1))&&
            (!(flags&4)||(flags&2))&&read(&mask.persistent,sizeof(mask.persistent))&&read(v,sizeof(v))&&
            read(&nOwners,4)&&read(&nPolygons,4)&&nOwners>0&&nOwners<=2048&&nPolygons>0&&nPolygons<=32;
        if(!valid)break;
        for(float value:v)valid=valid&&std::isfinite(value)&&value>=0;
        valid=valid&&v[4]<=1&&v[5]<=1&&v[6]<=1;
        mask.once=flags&1;mask.sourceTween=flags&2;mask.discoverySound=flags&4;
        valid=valid&&(!mask.sourceTween||(v[0]==0&&mask.persistent.scene&&mask.persistent.object));
        mask.pause=v[0];mask.seconds=v[1];mask.leaveSeconds=v[2];mask.reloadSeconds=v[3];
        mask.inside=v[4];mask.outside=v[5];mask.initial=v[6];
        mask.owners.resize(nOwners);valid=valid&&read(mask.owners.data(),nOwners*4)&&std::is_sorted(mask.owners.begin(),mask.owners.end());
        for(unsigned j=0;valid&&j<nPolygons;++j){
            unsigned n=0;valid=read(&n,4)&&n>=3&&n<=1024;if(!valid)break;
            std::vector<Vec2> points(n);valid=read(points.data(),n*sizeof(Vec2));
            for(auto p:points)valid=valid&&std::isfinite(p.x)&&std::isfinite(p.y);
            mask.polygons.push_back(std::move(points));
        }
        next.push_back(std::move(mask));
    }
    valid=valid&&std::fgetc(file)==EOF;std::fclose(file);
    if(valid)masks=std::move(next);
    return valid;
}
class RoomMasks {
    friend struct SimulationStateHash;
public:
    void bind(const std::vector<RoomMask>& masks,SceneBoolSave* saved=nullptr){
        definitions_=&masks;saved_=saved;states_.clear();changed_=false;discoveries_={};frameSeconds_=0;
        for(const auto& mask:masks){State state;state.alpha=state.from=state.target=mask.initial;
            state.reloaded=state.activated=saved&&saved->get(mask.persistent);
            if(mask.sourceTween&&state.reloaded){state.target=mask.inside;state.duration=mask.reloadSeconds;}
            states_.push_back(state);}
    }
    void clear(){definitions_=nullptr;saved_=nullptr;states_.clear();changed_=false;discoveries_={};frameSeconds_=0;}
    // Called after hero placement, once per room frame, even during control locks.
    // iTween applies the current percentage before adding scaled deltaTime.
    void advance(float dt){
        if(!std::isfinite(dt)||dt<0)return;
        frameSeconds_=dt;
        if(!definitions_)return;
        for(unsigned i=0;i<states_.size();++i){
            if(!(*definitions_)[i].sourceTween)continue;
            auto& state=states_[i];
            state.alpha=state.from+(state.target-state.from)*(state.duration>0?std::min(1.f,state.elapsed/state.duration):1.f);
            state.elapsed=std::min(state.duration,state.elapsed+dt);
        }
    }
    void update(float dt,Rect hero,bool triggers=true){
        if(!definitions_)return;
        for(unsigned i=0;i<states_.size();++i){
            const auto& mask=(*definitions_)[i];auto& state=states_[i];state.age+=dt;
            if(state.age<mask.pause)continue;
            bool inside=false;if(triggers)for(const auto& polygon:mask.polygons)inside=inside||touches(polygon,hero);
            if(inside&&!state.activated&&mask.once){state.activated=true;
                if(mask.discoverySound&&i<128)discoveries_[i/32]|=std::uint32_t(1)<<(i%32);
                if(saved_&&mask.persistent.scene&&saved_->set(mask.persistent))changed_=true;}
            const float target=(mask.once?state.activated:inside)?mask.inside:mask.outside;
            if(target!=state.target){state.from=state.alpha;state.target=target;state.elapsed=0;
                state.duration=state.reloaded?mask.reloadSeconds:
                    (state.first&&target==mask.outside?0.f:(target==mask.inside?mask.seconds:mask.leaveSeconds));
                // Physics enters Fade after advance() in this frame. Its first
                // iTween Update still displays the starting alpha, then adds dt.
                if(mask.sourceTween)state.elapsed=frameSeconds_;}
            if(mask.sourceTween)continue;
            state.first=false;state.elapsed+=dt;
            state.alpha=state.from+(state.target-state.from)*(state.duration>0?std::min(1.f,state.elapsed/state.duration):1.f);
        }
    }
    float alpha(unsigned owner) const{
        float value=1;if(definitions_)for(unsigned i=0;i<states_.size();++i){
            const auto& owners=(*definitions_)[i].owners;
            if(std::binary_search(owners.begin(),owners.end(),owner))value=std::min(value,states_[i].alpha);}
        return value;
    }
    bool takeChanged(){const bool value=changed_;changed_=false;return value;}
    template<class Emit> void visitAlphaOwners(Emit emit)const{
        if(definitions_)for(const auto& mask:*definitions_)for(const auto owner:mask.owners)emit(owner);
    }
    const std::array<std::uint32_t,4>& discoveries() const{return discoveries_;}
private:
    struct State {float age=0,alpha=1,from=1,target=1,elapsed=0,duration=0;bool activated=false,reloaded=false,first=true;};
    const std::vector<RoomMask>* definitions_=nullptr;SceneBoolSave* saved_=nullptr;
    std::vector<State> states_;bool changed_=false;
    std::array<std::uint32_t,4> discoveries_{};float frameSeconds_=0;
    static bool touches(const std::vector<Vec2>& polygon,Rect box){
        bool inside=false;const Vec2 center{(box.left+box.right)*.5f,(box.bottom+box.top)*.5f};
        for(unsigned i=0,j=unsigned(polygon.size()-1);i<polygon.size();j=i++){
            const auto a=polygon[j],b=polygon[i];
            if(a.x>=box.left&&a.x<=box.right&&a.y>=box.bottom&&a.y<=box.top)return true;
            if((a.y>center.y)!=(b.y>center.y)&&center.x<(b.x-a.x)*(center.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
            float lo=0,hi=1;const Vec2 d{b.x-a.x,b.y-a.y};bool hit=true;
            for(const auto axis:{0,1}){const float origin=axis?a.y:a.x,delta=axis?d.y:d.x;
                const float low=axis?box.bottom:box.left,high=axis?box.top:box.right;
                if(delta==0){if(origin<low||origin>high)hit=false;}
                else {float t0=(low-origin)/delta,t1=(high-origin)/delta;if(t0>t1)std::swap(t0,t1);
                    lo=std::max(lo,t0);hi=std::min(hi,t1);if(lo>hi)hit=false;}}
            if(hit)return true;
        }
        return inside;
    }
};
}
