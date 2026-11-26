#pragma once
#include "cartographer.hpp"
#include "cornifer_data.hpp"
#include "draw_list.hpp"
#include "platform.hpp"
#include <stdexcept>

namespace hkpsp {
class CorniferArt {
    hk3ds::AnimationBank bank_;
    hk3ds::AnimationPlayer card_;
    unsigned page_=0;
public:
    static constexpr float cardX=36.34f,cardY=33.44f;
    void clear(hk3ds::Cartographer& npc){
        npc.bind(nullptr);npc.animation={};card_={};bank_={};
    }
    void load(Platform& platform,std::vector<Texture>& textures,hk3ds::Cartographer& npc){
        const std::string prefix="rooms/Crossroads_33/cornifer-";
        bank_=readAnimation(platform.path(prefix+"visual.bin"));
        if(bank_.pageCount!=1)throw std::runtime_error("Cornifer page budget");
        auto texture=readTexture(platform.path(prefix+"page-0.hpt"));
        if(texture.width!=512||texture.height!=512)throw std::runtime_error("Cornifer page size");
        for(const auto* name:{"Idle","Talk L","Talk R","Card"})
            if(!bank_.find(name))throw std::runtime_error(std::string("Cornifer clip missing: ")+name);
        page_=unsigned(textures.size());textures.push_back(std::move(texture));
        card_.play(bank_.require("Card"));npc.bind(&bank_);
    }
    void append(DrawList& list,const hk3ds::Cartographer& npc,hk3ds::Vec2 camera)const{
        if(!bank_.pageCount)return;
        ActorPose pose;pose.position=npc.position;
        if(npc.present){
            pose.scale={npc.artScale(),hk3ds::corniferdata::scaleY};
            appendActor(list,bank_,npc.animation,pose,camera,page_);
        }else if(npc.card){
            // Original "Cornifer Card" (level64 GameObject 1132, Transform 2250, no parent, scale 1):
            // its own spot 1.25 units below Cornifer (x=36.31, y=34.69). The 3DS drew it at
            // Cornifer's pivot (main.cpp:560), so it floated above the floor (user 2026-11-18).
            pose.position={cardX,cardY};
            appendActor(list,bank_,card_,pose,camera,page_);
        }
    }
};
}
