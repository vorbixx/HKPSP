#pragma once
#include "world_lifts.hpp"
#include "terrain.hpp"
#include "hero.hpp"
#include <algorithm>

namespace hk3ds {
// Applies source TranslateY/SetY in order; arrival detaches before the snap.
// Collider identities are borrowed from exported data, never inferred by name.
class MineLiftMotion {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=16;
    bool bind(TerrainBank& terrain,const unsigned* ids,unsigned count,float y){
        if(!ids||!count||count>capacity||!std::isfinite(y))return false;
        MineLiftMotion next;next.terrain_=&terrain;next.count_=count;next.position_.y=y;
        next.shapeCount_=terrain.shapes.size();next.vertexCount_=terrain.vertices.size();
        for(unsigned i=0;i<count;++i){
            for(unsigned j=0;j<i;++j)if(ids[j]==ids[i])return false;
            auto it=std::find_if(terrain.shapes.begin(),terrain.shapes.end(),[&](const TerrainShape& s){return s.id==ids[i];});
            if(!ids[i]||it==terrain.shapes.end()||it->first>terrain.vertices.size()||
               it->count>terrain.vertices.size()-it->first)return false;
            next.indices_[i]=unsigned(it-terrain.shapes.begin());next.ids_[i]=ids[i];
        }
        next.history_.reset(next.position_);*this=next;return true;
    }
    bool bound()const{return terrain_!=nullptr;}
    void clear(){*this={};}
    float y()const{return position_.y;}
    float presentedY(PresentationTime t)const{return history_.sample(position_,t).y;}
    bool carrying()const{return carrying_;}
    bool touching(const Hero& h)const{
        for(std::size_t c=0;c<h.contacts.count;++c)
            for(unsigned i=0;i<count_;++i)if(h.contacts.surfaces[c].id==ids_[i])return true;
        return false;
    }
    bool apply(const MineLiftEvents& events,Hero& hero){
        if(!valid()){clear();return false;}
        const PresentationPosition::Step capture(history_,position_);
        for(unsigned i=0;i<events.count;++i){const auto& a=events.actions[i];
            if(a.kind==MineLiftActionKind::DeactivateCarry)carrying_=false;
            if(a.kind==MineLiftActionKind::ActivateCarry)carrying_=touching(hero);
            if(a.kind!=MineLiftActionKind::TranslateY&&a.kind!=MineLiftActionKind::SetY)continue;
            const float delta=a.kind==MineLiftActionKind::SetY?a.value-position_.y:a.value;
            if(!std::isfinite(delta)||!std::isfinite(position_.y+delta))return false;
            for(unsigned s=0;s<count_;++s){auto& shape=terrain_->shapes[indices_[s]];
                shape.bounds.bottom+=delta;shape.bounds.top+=delta;
                for(unsigned v=shape.first;v<shape.first+shape.count;++v)terrain_->vertices[v].y+=delta;
            }
            position_.y+=delta;
            if(carrying_)hero.platformDisplacement+=delta;
            if(a.kind==MineLiftActionKind::SetY)history_.reset(position_);
        }
        return true;
    }
    void afterPhysics(const Hero& h){if(carrying_&&!touching(h))carrying_=false;}
private:
    bool valid()const{
        if(!terrain_||terrain_->shapes.size()!=shapeCount_||terrain_->vertices.size()!=vertexCount_)return false;
        for(unsigned i=0;i<count_;++i){const auto& s=terrain_->shapes[indices_[i]];
            if(s.id!=ids_[i]||s.first>vertexCount_||s.count>vertexCount_-s.first)return false;}
        return true;
    }
    TerrainBank* terrain_=nullptr;std::array<unsigned,capacity> indices_{},ids_{};
    std::size_t shapeCount_=0,vertexCount_=0;unsigned count_=0;
    Vec2 position_{};PresentationPosition history_;bool carrying_=false;
};
static_assert(sizeof(MineLiftMotion)<512,"Town moving collider budget");
}
