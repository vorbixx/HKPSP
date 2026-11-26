#pragma once
#include "route_tolls.hpp"
#include "draw_list.hpp"
#include "platform.hpp"
#include <stdexcept>

namespace hkpsp {
// Toll machines of Crossroads_49b (city lift), Mines_33 (gate) and Fungus1_31 (greenpath bench).
// Presentation only: hk3ds::RouteTolls owns the simulation and the animation player; this class
// supplies the two-page PSA1 bank it binds (room_stage.py stages rooms/<scene>/toll-visual.bin and
// toll-page-0/1.hpt). Mirrors main.cpp:532-537 (one draw per visible, non-hidden machine) and
// tutorial_render_state.cpp:79 (scale.x from artScale(i)).
class RouteTollArt {
    hk3ds::AnimationBank bank_;
    unsigned firstPage_=0;
public:
    void clear(hk3ds::RouteTolls& tolls){
        tolls.bindArt(nullptr);bank_={};firstPage_=0;
    }
    bool bound()const{return bank_.pageCount!=0;}
    // Call after bindSceneEnemies: RouteTolls::load runs there (enterScene resets the toll state first).
    void load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,
              hk3ds::RouteTolls& tolls,const char* scene){
        clear(tolls);
        if(!tolls.active())return;
        const std::string directory=std::string("rooms/")+scene+"/";
        auto atlas=pages.load(platform,textures,directory+"toll-visual.bin",directory+"toll");
        if(atlas.bank.pageCount!=2)throw std::runtime_error(std::string("Toll page budget: ")+scene);
        for(const auto* name:{"Box_idle","Box_disappear"})
            if(!atlas.bank.find(name))throw std::runtime_error(std::string("Toll clip missing: ")+name);
        bank_=std::move(atlas.bank);firstPage_=atlas.page;
        tolls.bindArt(&bank_);
    }
    void append(DrawList& list,const hk3ds::RouteTolls& tolls,hk3ds::Vec2 camera)const{
        if(!bank_.pageCount||!tolls.art.visible)return;
        for(unsigned i=0;i<tolls.art.count;++i){
            if(tolls.art.hidden[i])continue;
            ActorPose pose;
            pose.position=tolls.art.poses[i].position;
            pose.scale={tolls.artScale(i),tolls.art.poses[i].scale.y};
            appendActor(list,bank_,tolls.art.animation,pose,camera,firstPage_);
        }
    }
};
}
