#pragma once
#include "world_quests.hpp"
#include "draw_list.hpp"
#include "platform.hpp"
#include <stdexcept>
#include <vector>

namespace hkpsp {
// The rescued world quest NPC: Sly (Room_ruinhouse) or Bretta (Fungus2_23). Presentation only: the
// simulation (hk3ds::WorldQuests) owns the animation player; this class supplies the PSA1 bank it binds.
// Mirrors main.cpp:538-541 (worldQuestAnimation at worldQuestPosition with worldQuestScale).
// Staged names follow room_stage.py: rooms/<scene>/<artPrefix>-visual.bin and -page-0.hpt, where the
// prefix is WorldQuests::artPrefix() ("world-quest" or "bretta-quest"), as main.cpp:1870 names them.
class WorldQuestArt {
    hk3ds::AnimationBank bank_;
    unsigned page_=0;
public:
    void clear(hk3ds::WorldQuests& quests){
        quests.bindArt(nullptr);bank_={};
    }
    // Call only after the scene is entered (enterScene -> WorldQuests::enter), so the definition and
    // presence are final. An absent quest loads nothing.
    void load(Platform& platform,std::vector<Texture>& textures,hk3ds::WorldQuests& quests){
        clear(quests);
        if(!quests.present())return;
        const auto& definition=quests.definition();
        const std::string prefix=std::string("rooms/")+definition.scene+"/"+quests.artPrefix()+"-";
        bank_=readAnimation(platform.path(prefix+"visual.bin"));
        if(bank_.pageCount!=1)throw std::runtime_error("World quest page budget");
        auto texture=readTexture(platform.path(prefix+"page-0.hpt"));
        if(texture.width!=512||texture.height!=512)throw std::runtime_error("World quest page size");
        const bool bretta=definition.event==hk3ds::WorldQuestEvent::BrettaRescued;
        const bool elderbug=definition.event==hk3ds::WorldQuestEvent::Elderbug;
        const bool tiso=definition.event==hk3ds::WorldQuestEvent::Tiso;
        const bool quirrel=definition.event==hk3ds::WorldQuestEvent::Quirrel;
        const bool seer=definition.event==hk3ds::WorldQuestEvent::Seer;
        const bool lemm=definition.event==hk3ds::WorldQuestEvent::Lemm;
        const std::vector<const char*> clips=lemm?std::vector<const char*>{"Lemm Idle"}:
            seer?std::vector<const char*>{"Seer Idle"}:
            quirrel?std::vector<const char*>{"Quirrel Idle"}:
            tiso?std::vector<const char*>{"Tiso Idle"}:
            elderbug?std::vector<const char*>{"Elderbug Idle"}:
            bretta?std::vector<const char*>{"Bretta Dazed","Bretta Wake"}:
            std::vector<const char*>{"Sly Dazed","Sly Wake","Sly Idle"};
        for(const auto* name:clips)
            if(!bank_.find(name))throw std::runtime_error(std::string("World quest clip missing: ")+name);
        page_=unsigned(textures.size());textures.push_back(std::move(texture));
        quests.bindArt(&bank_);
    }
    // Guard after the scene entry: a present quest must have its bank bound before the first frame.
    void require(const hk3ds::WorldQuests& quests)const{
        if(quests.present()&&!bank_.pageCount)throw std::logic_error("World quest present without art");
    }
    void append(DrawList& list,const hk3ds::WorldQuests& quests,hk3ds::Vec2 camera)const{
        if(!bank_.pageCount||!quests.present())return;
        ActorPose pose;
        pose.position=quests.definition().position;
        pose.scale=quests.definition().scale;
        appendActor(list,bank_,quests.animation(),pose,camera,page_);
    }
};
}
