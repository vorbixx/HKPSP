#include "health_cocoon.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
bool finiteRect(Rect r) {
    return std::isfinite(r.left)&&std::isfinite(r.bottom)&&
           std::isfinite(r.right)&&std::isfinite(r.top)&&
           r.left<r.right&&r.bottom<r.top;
}
}

bool HealthCocoonConfig::load(const char* path) {
    if(!path)return false;
    std::FILE* file=std::fopen(path,"rb");
    if(!file)return false;
    char magic[4]{};std::uint32_t sourceOwner=0;float values[16]{};
    std::array<std::uint32_t,2> disabled{};
    HealthCocoonFxConfig fx;
    const bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"HCC3",4)==0&&
        std::fread(&sourceOwner,sizeof(sourceOwner),1,file)==1&&
        std::fread(values,sizeof(float),16,file)==16&&
        std::fread(disabled.data(),sizeof(std::uint32_t),2,file)==2&&fx.read(file)&&std::fgetc(file)==EOF;
    std::fclose(file);
    if(!ok)return false;
    HealthCocoonConfig next;
    next.owner=sourceOwner;
    next.fx=fx;
    next.disabledColliders=disabled;
    if(!disabled[0]||!disabled[1]||disabled[0]==disabled[1])return false;
    next.position={values[0],values[1]};next.depth=values[2];
    next.scale={values[3],values[4]};
    next.nailBounds={values[6],values[7],values[8],values[9]};
    next.splatPosition={values[10],values[11]};next.splatDepth=values[12];
    next.splatScale={values[13],values[14]};
    if(!std::isfinite(next.position.x)||!std::isfinite(next.position.y)||
       !std::isfinite(next.depth)||!std::isfinite(values[5])||
       !std::isfinite(next.scale.x)||!std::isfinite(next.scale.y)||
       !std::isfinite(next.splatPosition.x)||!std::isfinite(next.splatPosition.y)||
       !std::isfinite(next.splatDepth)||!std::isfinite(values[15])||
       !std::isfinite(next.splatScale.x)||!std::isfinite(next.splatScale.y)||
       next.scale.x<=0||next.scale.y<=0||next.splatScale.x<=0||next.splatScale.y<=0||
       !finiteRect(next.nailBounds))return false;
    *this=next;return true;
}

bool HealthCocoon::bind(const AnimationBank& bank,const HealthCocoonConfig& config,
                        std::uint32_t cosmeticSeed) {
    if(!bank.find("Cocoon Idle")||!bank.find("Cocoon Sweat")||
       !finiteRect(config.nailBounds)||!std::isfinite(config.position.x)||
       !std::isfinite(config.position.y)||!std::isfinite(config.depth)||
       !std::isfinite(config.scale.x)||!std::isfinite(config.scale.y)||
       !std::isfinite(config.splatPosition.x)||!std::isfinite(config.splatPosition.y)||
       !std::isfinite(config.splatDepth)||!std::isfinite(config.splatScale.x)||
       !std::isfinite(config.splatScale.y)||config.scale.x<=0||config.scale.y<=0||
       config.splatScale.x<=0||config.splatScale.y<=0)return false;
    bank_=&bank;config_=config;seed_=cosmeticSeed?cosmeticSeed:1;reset();return true;
}

bool HealthCocoon::bind(const AnimationBank& bank,const char* configPath,
                        std::uint32_t cosmeticSeed) {
    HealthCocoonConfig config;
    return config.load(configPath)&&bind(bank,config,cosmeticSeed);
}

float HealthCocoon::random(float low,float high) {
    random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
    const float unit=static_cast<float>(random_>>8)*(1.0f/16777216.0f);
    return low+(high-low)*unit;
}

void HealthCocoon::startIdleWait() { wait_=random(2.0f,6.0f); }

void HealthCocoon::reset() {
    visual_={};splat_={};lastAttack_=moveSerial_=breakSerial_=0;
    moveElapsed_=splatElapsed_=0;hasAttack_=broken_=false;random_=seed_;
    if(!bank_) { wait_=0;return; }
    visual_.position=config_.position;visual_.depth=config_.depth;
    visual_.scale=config_.scale;visual_.visible=true;
    visual_.player.play(bank_->find("Cocoon Idle"));startIdleWait();
}

void HealthCocoon::update(float dt,const Hero& hero,const NailBank& nails,
                          BreakEffect effect,void* effectContext) {
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    if(splat_.visible) {
        splatElapsed_+=dt;splat_.player.advance(dt);
        const auto* clip=splat_.player.clip;
        if(!clip||splatElapsed_>=static_cast<float>(clip->count)/clip->fps)
            splat_.visible=false;
    }
    if(broken_)return;
    // This observes the same nail polygon as the geo rock presentation. It
    // never registers a CombatObject or changes any hero/terrain state.
    if(hero.nail.active&&hero.nail.hitbox&&
       (!hasAttack_||lastAttack_!=hero.nail.serial)&&
       nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],config_.nailBounds)) {
        hasAttack_=true;lastAttack_=hero.nail.serial;broken_=true;
        visual_.visible=false;++breakSerial_;
        const auto* splash=bank_->find("Splat2");
        if(splash) {
            splat_.position=config_.splatPosition;splat_.depth=config_.splatDepth;
            splat_.scale=config_.splatScale;splat_.visible=true;
            splat_.player.play(splash,0,true);splatElapsed_=0;
        }
        if(effect)effect(effectContext,config_.position,hero.nail.direction,
                         nailPoint(hero,nails.shapes[nailVariant(hero.nail)],{}));
        return;
    }
    if(visual_.player.clip==bank_->find("Cocoon Sweat")) {
        moveElapsed_+=dt;visual_.player.advance(dt);
        const auto* move=bank_->find("Cocoon Sweat");
        if(moveElapsed_>=static_cast<float>(move->count)/move->fps) {
            visual_.player.play(bank_->find("Cocoon Idle"),0,true);
            moveElapsed_=0;startIdleWait();
        }
    } else {
        wait_-=dt;
        if(wait_<=0) {
            visual_.player.play(bank_->find("Cocoon Sweat"),0,true);
            moveElapsed_=0;++moveSerial_;
        }
    }
}

}
