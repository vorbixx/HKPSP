#pragma once
#include "draw_list.hpp"
#include "monomon_data.hpp"
#include "monomon_room.hpp"
#include "text_draw.hpp"
#include <stdexcept>

namespace hkpsp {
class MonomonArt {
    AnimationAtlas atlas;

public:
    void load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,const hk3ds::MonomonRoom& room){
        if(!room.hasArt())return;
        const std::string prefix=room.guardian()?"rooms/scene-enemy/monomon-guardian":"rooms/scene-enemy/monomon-archive";
        atlas=pages.load(platform,textures,prefix+"-art.bin",prefix);
        for(const auto& clip:room.art().clips){
            const auto* render=atlas.bank.find(clip.name);
            if(!render||render->count!=clip.count)
                throw std::runtime_error(std::string("Monomon animation mismatch: ")+clip.name);
        }
    }

    void append(DrawList& list,const hk3ds::MonomonRoomDraw& draw,hk3ds::Vec2 camera)const{
        const auto sprite=[&](bool visible,hk3ds::AnimationPlayer player,hk3ds::Vec2 position,float scaleX,float scaleY,float alpha=1){
            if(!visible||!player.clip)return;
            player.clip=atlas.bank.find(player.clip->name);
            ActorPose pose{position,{scaleX,scaleY}};
            pose.alpha=alpha;
            appendActor(list,atlas.bank,player,pose,camera,atlas.page);
        };
        sprite(draw.bodyVisible,draw.bodyAnimation,draw.body,draw.bodyScale,std::abs(draw.bodyScale));
        constexpr float glowScale=hk3ds::monomon_data::glowScale;
        sprite(draw.glowAlpha>0,draw.glowAnimation,draw.glow,glowScale,glowScale,draw.glowAlpha);
        sprite(draw.quirrelVisible,draw.quirrelAnimation,draw.quirrel,draw.quirrelScale,std::abs(draw.quirrelScale));
        sprite(draw.maskVisible,draw.maskAnimation,draw.mask,1,1);
    }

    static void overlay(TextCanvas& text,const hk3ds::MonomonRoomDraw& draw){
        auto canvas=text.canvas();
        if(draw.whiteFade>0)
            canvas.fill(canvas.user,{0,0,screenWidth,screenHeight},0xffffffu|(unsigned(255*draw.whiteFade)<<24));
        if(draw.prompt)canvas.text(canvas.user,"LISTEN",218,190,hk3ds::ui::Size::Body,hk3ds::ui::white);
        if(draw.text){
            canvas.fill(canvas.user,{12,159,456,102},0xdc000000u);
            const std::string page(draw.text);
            const auto visible=page.substr(0,page.find('\f'));
            hk3ds::ui::wrappedText(canvas,visible.c_str(),{26,175,428,78},hk3ds::ui::Size::Body);
        }
    }
};
}
