#include "uumuu_room.hpp"
#include "uumuu_room_data.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace hk3ds {
void UumuuRoom::reset(){
    blood_.reset();bloodClock_=0;
    quirrel_.reset();encounter_.reset();body_.reset();gates_.reset();
    combat_.bind(nullptr,0,nullptr);object_={};bodyArt_={};quirrelArt_={};gateArt_={};effectsArt_={};gateAnimations_={};lightningAnimations_={};
    audio_={};active_=faulted_=corpse_=titleRequested_=false;solids_.clear();gateOpenTime_={};recoilWorld_={};recoilBody_=recoilTransform_={};
}
bool UumuuRoom::enter(const char* root,const char* scene,const NailBank& nails,
                     EnemyRandom random,UumuuProgress& progress){
    reset();
    if(!UumuuEncounter::archiveOverlayRequired(scene,progress.defeatedMegaJelly))return true;
    char path[512];
    std::snprintf(path,sizeof(path),"%s/uumuu-body-art.bin",root);
    if(!bodyArt_.load(path))return false;
    std::snprintf(path,sizeof(path),"%s/uumuu-quirrel-art.bin",root);
    if(!quirrelArt_.load(path)||!body_.bind(bodyArt_))return false;
    std::snprintf(path,sizeof(path),"%s/uumuu-gate-art.bin",root);
    if(!gateArt_.load(path))return false;
    std::snprintf(path,sizeof(path),"%s/uumuu-effects-art.bin",root);
    if(!effectsArt_.load(path)||!effectsArt_.find("Blood"))return false;
    random_=random;
    if(!encounter_.bindArchiveRoom(object_,scene,random,progress)||
       !encounter_.bindBody(body_)||!encounter_.bindBodyColliders())return false;
    UumuuHitAdapter hitAdapter;hitAdapter.context=this;
    hitAdapter.capture=[](void* context,const NailHit& hit,const NailContact*,UumuuRecoilFrame& frame){
        auto& room=*static_cast<UumuuRoom*>(context);
        room.recoilBody_=room.recoilTransform_=room.position();
        const auto p=room.position();const auto box=room.object_.bounds;
        frame.collider={box.left-p.x,box.bottom-p.y,box.right-p.x,box.top-p.y};
        frame.magnitude=hit.recoilMultiplier;frame.body=&room.recoilBody_;
        // approximation: ten omnidirectional source blood particles per accepted
        // damaging nail hit. Corpse FSM counts/timing below are source-authored.
        room.blood_.emit(p,10,5,25,0,360);
        frame.transform=&room.recoilTransform_;frame.world=&room.recoilWorld_;return true;
    };
    if(!encounter_.bindHitAdapter(hitAdapter))return false;
    gates_.bind({});
    if(!encounter_.bindGates(gates_))return false;
    UumuuQuirrelAdapter adapter;adapter.context=this;
    adapter.audio=[](void* context,UumuuQuirrelCue cue,Vec2 at){static_cast<UumuuRoom*>(context)->quirrelAudio(cue,at);};
    adapter.floatRange=[](void* context,float lo,float hi){return static_cast<UumuuRoom*>(context)->random_.range(lo,hi);};
    adapter.intRange=[](void* context,int lo,int hi){
        return std::min(hi-1,int(static_cast<UumuuRoom*>(context)->random_.range(float(lo),float(hi))));
    };
    if(!quirrel_.bind(encounter_,quirrelArt_,quirrelArt_,adapter)||!combat_.bind(&object_,1,&nails))return false;
    active_=true;return true;
}
// level229 Quirrel/Control and Watch audio actions. Weighted selection runs
// before the actor samples its slash pose, as AudioPlayerOneShot does.
void UumuuRoom::quirrelAudio(UumuuQuirrelCue cue,Vec2 at){
    UumuuSound sound=UumuuSound::Intro;float pitch=1;
    switch(cue){
    case UumuuQuirrelCue::CheckPosition:case UumuuQuirrelCue::WatchIntro:break;
    case UumuuQuirrelCue::Arrive:case UumuuQuirrelCue::SlashAntic:sound=UumuuSound::Enter;break;
    case UumuuQuirrelCue::Leave:sound=UumuuSound::Exit;break;
    case UumuuQuirrelCue::Slash:sound=UumuuSound::Slash;break;
    case UumuuQuirrelCue::Wound:sound=UumuuSound::Wounded;break;
    case UumuuQuirrelCue::SlashVoice:{
        const float choice=random_.range(0,2);
        if(choice>=2)return;
        sound=choice<1?UumuuSound::Shout1:UumuuSound::Shout2;
        pitch=random_.range(1,1);break;
    }
    case UumuuQuirrelCue::WatchVoice:{
        const float choice=random_.range(0,5);
        if(choice>=3)return; // Two equal-weight null entries; no pitch draw.
        sound=choice<1?UumuuSound::Dialogue1:choice<2?UumuuSound::Dialogue2:UumuuSound::Dialogue3;
        pitch=random_.range(1,1);break;
    }
    case UumuuQuirrelCue::WatchArrive:sound=UumuuSound::Enter;pitch=random_.range(.9f,1.1f);break;
    case UumuuQuirrelCue::WatchLeave:sound=UumuuSound::Exit;pitch=random_.range(.9f,1.1f);break;
    }
    audio_.emit(sound,at,pitch);
}
World UumuuRoom::world(const World& base){
    if(!active_)return base;
    solids_.clear();
    if(base.count)solids_.insert(solids_.end(),base.solids,base.solids+base.count);
    for(unsigned i=0;i<UumuuGates::capacity;++i)
        if(gates_.colliderEnabled(i))solids_.push_back({uumuu_gates_data::gates[i].bounds,false,true});
    World result=base;result.solids=solids_.data();result.count=solids_.size();recoilWorld_=result;return result;
}
void UumuuRoom::fixedUpdate(float dt,Hero& hero,bool frozen){
    if(!active_)return;
    if(!frozen){
        blood_.fixedUpdate(dt,recoilWorld_);
        const auto phase=encounter_.deathPhase();
        const float* source=phase==UumuuDeathPhase::Init?uumuu_blood_data::init:
                            phase==UumuuDeathPhase::Steam?uumuu_blood_data::steam:nullptr;
        if(source&&std::isfinite(dt)&&dt>0){
            bloodClock_+=dt;
            while(bloodClock_>=source[2]){bloodClock_-=source[2];blood_.emitSource(position(),source);}
        }
    }
    if(!frozen&&!corpse_){
        auto& motion=encounter_.motion();motion.fixedUpdate(dt,hero.position);
        if(encounter_.recoil().active()){
            recoilBody_=recoilTransform_=motion.position();
            encounter_.recoil().fixedUpdate(dt,recoilBody_,recoilTransform_,recoilWorld_);
            motion.applyRecoil(recoilBody_);
        }
        motion.sampleBounce();encounter_.syncBodyColliders();
        encounter_.contactBody(hero,bodyBounds(hero.position));
        encounter_.contactLightning(hero,bodyBounds(hero.position));
    }
    combat_.fixedUpdate(hero);
}
void UumuuRoom::update(float dt,Hero& hero,bool frozen){
    if(!active_||!std::isfinite(dt)||dt<=0)return;
    combat_.update(hero,dt);
    if(frozen)return;
    const auto box=bodyBounds(hero.position);
    if(box.right>uumuu_room_data::triggerLeft&&box.left<uumuu_room_data::triggerRight&&
       box.top>uumuu_room_data::triggerBottom&&box.bottom<uumuu_room_data::triggerTop)
        encounter_.arenaEntered();
    if(!corpse_){
        body_.advance(dt);
        if(!quirrel_.animationUpdate(dt,hero.position)||!quirrel_.consume())faulted_=true;
        encounter_.wakeTweenUpdate(dt);
        encounter_.controlUpdate(dt,hero.position);
        const auto attackEvents=encounter_.takeAttackEvents();
        if(attackEvents.callQuirrel)encounter_.arrivePause(hero.position);
        // One original pattern cue at Hero, separate from pooled chase zaps.
        if(attackEvents.activateRandomPattern)audio_.emit(UumuuSound::MultiZap,hero.position,1,1.5f);
        const auto wake=encounter_.takeWakeEvents();
        if(wake.start)titleRequested_=true;
        if(wake.ready)encounter_.motion().awakeBounds();
        encounter_.motion().updateBounds();encounter_.syncBodyColliders();
        if(!quirrel_.consume()||!quirrel_.controlUpdate(dt))faulted_=true;
        encounter_.lightningUpdate(dt);
        for(unsigned i=0;i<UumuuEncounter::zapCapacity;++i){
            auto* zap=encounter_.zap(i);const auto events=encounter_.takeZapEvents(i);
            if(events.antic)audio_.emit(UumuuSound::ZapAntic,zap->position());
            if(events.strike)audio_.emit(UumuuSound::Zap,zap->position());
        }
        auto& multizaps=encounter_.multizaps();
        for(unsigned i=0;i<UumuuMultizaps::capacity;++i){
            auto& animation=lightningAnimations_[i];
            const auto events=multizaps.takeEvents(i);
            if(events.hide||events.deactivate)animation={};
            if(events.antic)animation.play(effectsArt_.require("Lightning/Zap Antic"),0,true);
            if(events.strike)animation.play(effectsArt_.require("Lightning/Zap"),0,true);
            if(events.end)animation.play(effectsArt_.require("Lightning/Zap End"),0,true);
            animation.advance(dt);
            if(multizaps.actors[i].phase==UumuuMultizapPhase::Ending&&animation.complete)
                multizaps.endAnimationFinished(i);
        }
        if(const auto token=encounter_.takeBounceShroomDestroyRequest())encounter_.bounceShroomDestroyed(token);
        if(encounter_.takeCorpseSpawnRequest()){
            corpse_=true;encounter_.corpseActivated(false);encounter_.multizaps().deactivate();
        }
    }else encounter_.corpseUpdate(dt);
    const auto death=encounter_.takeDeathEvents();
    if(death.init||death.steam)bloodClock_=0;
    if(death.init)audio_.emit(UumuuSound::FinalHit,encounter_.motion().position());
    if(death.steam)audio_.emit(UumuuSound::Gushing,encounter_.motion().position());
    if(death.blow){
        audio_.emit(UumuuSound::Explode,encounter_.motion().position());
        blood_.emitSource(encounter_.motion().position(),uumuu_blood_data::blow);
    }
    encounter_.battleUpdate(dt);
    for(unsigned i=0;i<UumuuGates::capacity;++i){
        if(const char* name=UumuuGates::clipName(gates_.clip(i));name&&*name){
            char clip[64];std::snprintf(clip,sizeof(clip),"Gate/%s",name);
            gateAnimations_[i].play(gateArt_.require(clip));gateAnimations_[i].advance(dt);
        }
        if(gates_.phase(i)==UumuuGatePhase::Open){
            gateOpenTime_[i]+=dt;
            if(gateOpenTime_[i]>=uumuu_gates_data::openDuration)gates_.animationFinished(i);
        }else gateOpenTime_[i]=0;
    }
}
bool UumuuRoom::gatesOpen()const{
    for(unsigned i=0;i<UumuuGates::capacity;++i)if(gates_.colliderEnabled(i))return false;
    return true;
}
void UumuuRoom::capture(UumuuRoomDraw& draw,PresentationTime time)const{
    draw={};draw.active=active_;if(!active_)return;
    draw.position=encounter_.motion().renderPosition(time);draw.body=body_.player();
    draw.bloodCount=blood_.capture(draw.blood,time);
    draw.visible=encounter_.wake().bodyVisible();draw.dead=corpse_;draw.locked=locked();
    if(corpse_){
        // The corpse prefab uses collection86 sprite20, also Wound frame1.
        // approximation: retain its base sprite until the source Blow callback;
        // The steam plume and explosion wave are separate from the blood.
        draw.body.play(bodyArt_.require("Uumuu/Wound"),1,true);
        draw.visible=encounter_.deathPhase()!=UumuuDeathPhase::Blown;
    }
    for(unsigned i=0;i<UumuuGates::capacity;++i){
        draw.gates[i].visible=gates_.visible(i);draw.gates[i].animation=gateAnimations_[i];
    }
    draw.land=quirrel_.land();draw.slash=quirrel_.slash();
    draw.landPosition=quirrel_.renderLandPosition(time);draw.slashPosition=quirrel_.renderSlashPosition(time);
    for(unsigned i=0;i<UumuuEncounter::zapCapacity;++i){const auto* zap=encounter_.zap(i);
        if(zap->phase()==UumuuZapPhase::Antic||zap->phase()==UumuuZapPhase::Active)
            draw.zaps[draw.zapCount++]={zap->position(),zap->phase()==UumuuZapPhase::Active};
    }
    const auto& multizaps=encounter_.multizaps();
    for(unsigned i=0;i<UumuuMultizaps::capacity;++i){const auto phase=multizaps.actors[i].phase;
        if(phase==UumuuMultizapPhase::Antic||phase==UumuuMultizapPhase::Active||phase==UumuuMultizapPhase::Ending){
            const auto& actor=multizaps.actors[i];
            draw.zaps[draw.zapCount++]={UumuuMultizaps::position(i),phase==UumuuMultizapPhase::Active,
                lightningAnimations_[i],actor.scaleX,actor.rotation};
        }
    }
}
}
