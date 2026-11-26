#pragma once
#include "tutorial.hpp"
#include "draw_list.hpp"
#include "platform.hpp"
#include "tutorial_render_state.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

namespace hkpsp {
// Scene -> boss art bank (3DS romfs bank per boss: main.cpp:1926-1934). The generic path loads
// boss-art.bin + boss-page-N (Gruz Mother); the Dung Defender has dung-art.bin + dung-page-N; the
// False Knight has its own bank and effect configs.
// Staged by tools/psp/room_stage.py as rooms/<scene>/<prefix>-visual.bin and <prefix>-page-N.hpt.
struct BossArtSpec {
    const char* scene;
    const char* prefix;   // staged name prefix: "<prefix>-visual.bin", "<prefix>-page-N.hpt"
    unsigned pages;       // exact PSA1 page count (the staged table is checked against it)
    const char* effects;  // FKF4 effect configs (main.cpp:1933, False Knight only), or nullptr
};
inline const BossArtSpec* findBossArt(const char* scene){
    static const BossArtSpec specs[]={
        {"Crossroads_10","false-knight",11,"false-knight-fx.bin"}, // False Knight
        {"Crossroads_04","boss",10,nullptr},                         // Gruz Mother
        {"Waterways_05","dung",3,nullptr},                           // Dung Defender (dung-art.bin, main.cpp:1928)
    };
    for(const auto& spec:specs)if(!std::strcmp(spec.scene,scene))return &spec;
    return nullptr;
}

// Boss presentation only: the simulation (hk3ds::CrossroadsBoss) owns the animation state; this
// class supplies the PSA1 bank it binds. Draws mirror main.cpp:760-795 (no title).
class BossArt {
    AnimationAtlas atlas_;
    std::unique_ptr<hk3ds::BossRenderState> state_; // heap: the snapshot holds arrays of actor records
public:
    void clear(hk3ds::TutorialSession& game){
        game.bindBossArt(nullptr);atlas_={};
    }
    // Call after bindSceneEnemies: the boss is bound by then, so bindBossArt enters it with the bank.
    // A scene without a boss bank is a no-op after clear().
    void load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,hk3ds::TutorialSession& game,const char* scene){
        clear(game);
        const BossArtSpec* spec=findBossArt(scene);
        if(!spec)return;
        if(!state_)state_=std::make_unique<hk3ds::BossRenderState>();
        const std::string directory=std::string("rooms/")+spec->scene+"/";
        atlas_=pages.load(platform,textures,directory+spec->prefix+"-visual.bin",directory+spec->prefix);
        if(atlas_.bank.pageCount!=spec->pages)throw std::runtime_error(std::string("Boss page budget: ")+spec->scene);
        game.bindBossArt(&atlas_.bank);
        // main.cpp:1935: effect configs apply to the False Knight only.
        if(spec->effects&&game.crossroadsBoss.active()&&game.crossroadsBoss.kind==hk3ds::CrossroadsBossKind::FalseKnight&&
           !game.crossroadsBoss.loadEffects(platform.path(directory+spec->effects).c_str()))
            throw std::runtime_error("False Knight effects failed");
    }
    const AnimationAtlas& atlas()const{return atlas_;}
    // Draws the knight with its authored scale and facing (main.cpp:760-795). Requires load().
    void append(DrawList& list,const hk3ds::TutorialSession& game,hk3ds::Vec2 camera){
        if(!atlas_.bank.pageCount||!state_)return;
        auto& state=*state_;
        state.capture(game.crossroadsBoss,game.presentationTime());
        if(!state.active())return;
        const auto& bank=atlas_.bank;
        const auto drawPlayer=[&](const hk3ds::AnimationPlayer& player,hk3ds::Vec2 at,float scale,float flash=0){
            ActorPose pose;pose.position=at;pose.scale={scale,std::fabs(scale)};pose.flash=flash;
            appendActor(list,bank,player,pose,camera,atlas_.page);
        };
        const auto drawLayered=[&](const hk3ds::AnimationPlayer& player,hk3ds::Vec2 at,float scale,float flash=0){
            drawPlayer(player,at,scale,flash);
            // Native-resolution frames share up to four atlas pieces; companions share owner timing.
            if(!player.clip)return;
            for(unsigned layer=1;layer<4;++layer){
                char name[64];std::snprintf(name,sizeof(name),"%s/Part%u",player.clip->name,layer);
                auto part=player;part.clip=bank.find(name);
                if(part.clip)drawPlayer(part,at,scale,flash);
            }
        };
        if(state.visible)drawLayered(state.animation,state.position,state.scaleX,state.hitFlash>0?1.f:0.f);
        if(state.visible&&state.showHead)
            drawLayered(state.headAnimation,state.headPosition,state.scaleX,state.hitFlash>0?1.f:0.f);
        for(const auto& minion:state.minions)if(minion.alive)
            drawLayered(minion.animation,minion.position,minion.velocity.x<0?1.f:-1.f);
        for(const auto& e:state.effects){
            char name[16];std::snprintf(name,sizeof(name),"FX/%u",e.emitter);
            hk3ds::AnimationPlayer a;a.play(bank.find(name));
            if(!a.clip)continue;
            a.frame=std::min<unsigned>(a.clip->count-1,unsigned(e.progress*a.clip->count));
            ActorPose pose;pose.position=e.position;pose.scale=e.scale;pose.alpha=e.opacity;pose.rotation=e.angle;
            appendActor(list,bank,a,pose,camera,atlas_.page);
        }
        for(const auto& projectile:state.projectiles){
            if(state.kind==hk3ds::CrossroadsBossKind::FalseKnight){
                const auto* spurtClip=bank.find("Wave/Spurt");
                for(const auto& spurt:projectile.waveSpurts)if(spurt.phase&&spurtClip){
                    hk3ds::AnimationPlayer wave;wave.play(spurtClip);
                    wave.advance((spurt.phase==2?.05f:0.f)+spurt.elapsed);
                    drawPlayer(wave,spurt.position,projectile.position.x<state.position.x?-1.f:1.f);
                }
            }
            if(projectile.life<=0)continue;
            if(state.kind==hk3ds::CrossroadsBossKind::DungDefender){drawLayered(projectile.animation,projectile.position,1);continue;}
            if(projectile.ground)continue; // moving wave owner is invisible; its spawned spurts carry authored art
            hk3ds::AnimationPlayer barrel;barrel.play(bank.find("Barrel/Idle"));
            ActorPose pose;pose.position=projectile.position;pose.scale={projectile.barrelScale,projectile.barrelScale};
            pose.rotation=projectile.barrelAngle;
            appendActor(list,bank,barrel,pose,camera,atlas_.page);
        }
    }
};
}
