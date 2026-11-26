#pragma once
#include "archive_shortcut_data.hpp"
#include "combat.hpp"
#include "terrain.hpp"
#include <algorithm>
#include <cstring>

namespace hk3ds {
class ArchiveShortcut {
    friend struct SimulationStateHash;
public:
    void bind(const char* scene,TerrainBank& terrain,bool savedOpen){
        *this={};active_=scene&&!std::strcmp(scene,archive_shortcut_data::scene);
        open_=active_&&savedOpen;applyCollision(terrain);
    }
    void applyCollision(TerrainBank& terrain) const{
        if(active_&&open_)for(auto id:archive_shortcut_data::colliders)terrain.setEnabled(id,false);
    }
    void open(TerrainBank& terrain){open_=true;applyCollision(terrain);}
    bool ready() const{return active_&&!open_&&hits_==archive_shortcut_data::hits;}
    bool hit(unsigned serial,bool heroInRange){
        if(!active_||open_||!serial||serial==serial_)return false;
        serial_=serial;
        if(wait_>0||!heroInRange||ready())return false;
        ++hits_;wait_=archive_shortcut_data::hitWait;return true;
    }
    void advance(float seconds){wait_=std::max(0.f,wait_-seconds);}
    void update(float seconds,const Hero& hero,const NailBank* nails,Rect heroBounds){
        if(!active_||open_)return;
        advance(seconds);
        if(nails&&hero.nail.hitbox&&nailOverlapsQuad(hero,nails->shapes[nailVariant(hero.nail)],archive_shortcut_data::attack))
            hit(hero.nail.serial,polygonOverlapsBox(archive_shortcut_data::heroRange.data(),4,heroBounds));
    }
    float alpha(unsigned owner) const{
        return active_&&open_&&std::binary_search(archive_shortcut_data::children.begin(),archive_shortcut_data::children.end(),owner)?0.f:1.f;
    }
    template<class Emit> void visitHiddenOwners(Emit emit)const{
        if(active_&&open_)for(const auto owner:archive_shortcut_data::children)emit(owner);
    }
private:
    unsigned hits_=0,serial_=0;
    float wait_=0;
    bool active_=false,open_=false;
};
}
