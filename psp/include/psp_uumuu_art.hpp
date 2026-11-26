#pragma once
#include "draw_list.hpp"
#include "text_draw.hpp"
#include "uumuu_room.hpp"
#include "uumuu_gates_data.hpp"
#include <stdexcept>

namespace hkpsp {
class UumuuArt {
    AnimationAtlas body,quirrel,gate,effects;

    static void sprite(DrawList& list,const AnimationAtlas& atlas,hk3ds::AnimationPlayer player,
                       const ActorPose& pose,hk3ds::Vec2 camera){
        if(!player.clip)return;
        const auto* render=atlas.bank.find(player.clip->name);
        if(!render||render->count!=player.clip->count)
            throw std::runtime_error(std::string("Archives animation mismatch: ")+player.clip->name);
        player.clip=render;
        appendActor(list,atlas.bank,player,pose,camera,atlas.page);
    }

public:
    void load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,const hk3ds::UumuuRoom& room){
        if(!room.active())return;
        const auto bank=[&](const char* name){
            const std::string prefix=std::string("rooms/scene-enemy/uumuu-")+name;
            return pages.load(platform,textures,prefix+"-art.bin",prefix);
        };
        body=bank("body");quirrel=bank("quirrel");gate=bank("gate");effects=bank("effects");
        if(!effects.bank.find("Blood"))throw std::runtime_error("Archives blood animation missing");
    }

    void append(DrawList& list,TextCanvas& canvas,const hk3ds::UumuuRoom& room,
                hk3ds::PresentationTime time,hk3ds::Vec2 camera)const{
        if(!room.active())return;
        hk3ds::UumuuRoomDraw draw;
        room.capture(draw,time);
        for(unsigned index=0;index<draw.gates.size();++index)if(draw.gates[index].visible){
            const auto& visual=hk3ds::uumuu_gates_data::visuals[index];
            sprite(list,gate,draw.gates[index].animation,{visual.position,visual.scale,.004f,visual.rotation},camera);
        }
        if(draw.visible)sprite(list,body,draw.body,{draw.position},camera);
        const auto companion=[&](const hk3ds::UumuuQuirrelSprite& actor,hk3ds::Vec2 position){
            if(actor.visible&&!draw.dead)
                sprite(list,quirrel,actor.animation,{position,{actor.scaleX,1},.004f,actor.rotationZ},camera);
        };
        companion(draw.land,draw.landPosition);
        companion(draw.slash,draw.slashPosition);
        const auto* bloodClip=effects.bank.find("Blood");
        for(unsigned index=0;index<draw.bloodCount;++index){
            const auto& blood=draw.blood[index];
            hk3ds::AnimationPlayer particle;
            particle.play(bloodClip,blood.tile,true);
            appendActor(list,effects.bank,particle,{blood.position,{blood.width,blood.height},.004f,blood.rotation,blood.alpha},camera,effects.page);
        }
        const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/38.104f;
        for(unsigned index=0;index<draw.zapCount;++index){
            const auto& zap=draw.zaps[index];
            if(zap.animation.clip){
                ActorPose pose{zap.position,{zap.scaleX,1},.004f,zap.rotation};
                pose.material=hk3ds::RoomMaterial::Additive;
                sprite(list,effects,zap.animation,pose,camera);
            }else{
                const float radius=(zap.active?.65f:.35f)*ppu;
                const float left=screenWidth*.5f+(zap.position.x-camera.x)*ppu-radius;
                const float top=screenHeight*.5f-(zap.position.y-camera.y)*ppu-radius;
                if(left+2*radius>0&&top+2*radius>0&&left<screenWidth&&top<screenHeight){
                    const auto drawCanvas=canvas.canvas();
                    const auto tint=zap.active?0xe6ffffdcu:0x64ffffdcu;
                    drawCanvas.fill(drawCanvas.user,{left+radius*.25f,top,1.5f*radius,2*radius},tint);
                    drawCanvas.fill(drawCanvas.user,{left,top+radius*.25f,.25f*radius,1.5f*radius},tint);
                    drawCanvas.fill(drawCanvas.user,{left+1.75f*radius,top+radius*.25f,.25f*radius,1.5f*radius},tint);
                }
            }
        }
    }
};
}
