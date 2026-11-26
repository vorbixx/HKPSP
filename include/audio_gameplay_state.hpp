#pragma once
#include "tutorial.hpp"
#include "wind_audio.hpp"
#include <cstdio>

namespace hk3ds {
// Independent of NDSP and AudioVoices. Record gameplay trigger counters and
// contact RNG input before clip/pitch selection, never voice submissions.
struct AudioGameplayState {
    unsigned frame,kind,owner,a=0,b=0,c=0,d=0,e=0;
    float x=0,y=0,z=0,w=0;
    bool write(FILE* f)const {
        return std::fprintf(f,"# audio-state %u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g\n",
            frame,kind,owner,a,b,c,d,e,x,y,z,w)>0;
    }
};
static_assert(sizeof(AudioGameplayState)==48,"bounded source-state evidence");

template<class Sink> void captureAudioGameplay(unsigned frame,const TutorialSession& t,Sink sink)
{
    unsigned families=0;
    const auto emit=[&](const AudioGameplayState& state){families|=1u<<state.kind;sink(state);};
    // One-based IDs distinguish captured cue metadata from legacy zero fields.
    const auto cue=unsigned(sceneAudioId(t.sceneName()))+1;
    const auto next=t.flow.transition?unsigned(sceneAudioId(t.flow.transition->scene))+1:0;
    emit(AudioGameplayState{frame,0,0,t.audioEpoch,unsigned(t.flow.transition==nullptr),unsigned(t.death.active()),cue,next,t.flow.camera.x,t.flow.camera.y});
    for(unsigned i=0;i<t.crawlerCount;++i){const auto& a=t.crawlers[i];
        emit(AudioGameplayState{frame,1,i+1,unsigned(a.active())|(unsigned(a.alive())<<1)|4,
            a.injurySerial,a.killSerial,0,0,a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y});}
    for(unsigned i=0;i<t.buzzerCount;++i){const auto& a=t.buzzers[i];
        const bool chase=a.state==BuzzerState::ChaseInSight||a.state==BuzzerState::ChaseOutOfSight;
        emit(AudioGameplayState{frame,1,17+i,1u|(unsigned(a.alive())<<1)|(unsigned(chase)<<3),
            a.injurySerial,a.killSerial,a.startleSerial,a.corpseSplatSerial,a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y});}
    const auto thresholds=t.greatDoor.thresholds();
    emit(AudioGameplayState{frame,2,33,t.greatDoor.hits(),thresholds[0],thresholds[1],thresholds[2],t.greatDoor.stage(),190.48000015f,68.42000008f,.61f});
    const auto lever=t.townLift.soundPosition();
    emit(AudioGameplayState{frame,15,316,t.townLift.hitSerial(),0,0,0,0,lever.x,lever.y,t.townLift.soundDepth()});
    const auto& shiny=t.furyChest.draws()[static_cast<unsigned>(FuryPart::Shiny)];
    const auto shinyPosition=t.furyChest.shinyPosition();
    emit(AudioGameplayState{frame,3,35,t.furyChest.openSerial(),t.furyChest.pickupSerial(),unsigned(t.furyChest.pickup()),t.furyChest.tinkSerial(),0,shinyPosition.x,shinyPosition.y,shiny.depth});
    for(const auto& a:t.roomAmbient.tablets())emit(AudioGameplayState{frame,4,a.config.owner,a.wakes(),a.inspections(),0,0,0,a.config.position.x,a.config.position.y,a.config.depth,a.hum()});
    emit(AudioGameplayState{frame,5,34,t.healthCocoon.moveSerial(),t.healthCocoon.breakSerial()});
    emit(AudioGameplayState{frame,6,48,t.healthScuttlers.bounceSerial(),t.healthScuttlers.deathSerial()});
    for(unsigned i=0;i<t.stalactiteCount;++i){const auto& a=t.stalactites[i];
        emit(AudioGameplayState{frame,7,40+i,a.cues[0],a.cues[3],a.cues[4],a.cues[5]});}
    emit(AudioGameplayState{frame,12,57,unsigned(t.flow.crumbling()),unsigned(t.flow.progress.collapsed)});
    for(unsigned i=0;i<2;++i)emit(AudioGameplayState{frame,14,i?3005u:2002u,t.secretCues[i]});
    const auto& opening=t.initialFall.cues();
    emit(AudioGameplayState{frame,13,56,opening[0],opening[1],opening[2],opening[3]});
    const auto& a=t.shade;
    emit(AudioGameplayState{frame,8,38,a.startleSerial,a.slashSerial,a.injurySerial,a.blowSerial,unsigned(a.alive()&&a.state!=ShadeState::Idle),a.position.x,a.position.y});
    const auto& e=t.conditionalEvents;
    const unsigned serials[]={e.mushroomHitSerial,e.rocketSerial,e.dreamSerial,e.teleSerial,e.dashSerial,e.shootSerial,e.spiralSerial,e.deathSerial,e.pickupSerial,e.dashStartSerial,e.castSerial,e.ringSerial,e.teleInSerial};
    for(unsigned i=0;i<13;++i)emit(AudioGameplayState{frame,9,i,serials[i]});
    if(const auto* bank=t.grass.bank())for(unsigned i=0;i<bank->count;++i){const auto& c=t.grass.soundCounters()[i];
        emit(AudioGameplayState{frame,10,bank->bushes[i].owner,c[0],c[1],c[2]});}
    if(t.contactEffects.ready())for(unsigned i=0;i<t.contactEffects.soundStates().size();++i){const auto& s=t.contactEffects.soundStates()[i];
        emit(AudioGameplayState{frame,11,i,s.serial,s.selectionSeed,0,0,0,s.position.x,s.position.y,s.z});}
    // These are rule-layer FSM actions, before AudioPlayer/NDSP consumption.
    // Keep every history slot so several fixed steps in one draw lose no cue.
    if(t.uumuu.active())for(unsigned i=0;i<UumuuAudioEvents::capacity;++i){
        const auto& e=t.uumuu.audio().events[i];
        emit(AudioGameplayState{frame,15,300+i,e.serial,unsigned(e.sound),0,0,0,
            e.position.x,e.position.y,e.volume,e.pitch});
    }
    for(unsigned i=0;i<t.roomBreakables.count();++i){
        const auto& config=t.roomBreakables.bank().items[i];
        if(config.audioAuthored)emit(AudioGameplayState{frame,16,config.owner,
            unsigned(t.roomBreakables.audioBroken(i)),t.roomBreakables.audioSeedBefore(i),t.audioEpoch,
            roomBreakAudioHash(t.sceneName())});
    }
    // Enumerated empty families are evidence too. The reserved owner has no
    // playback meaning; it distinguishes zero instances from missing coverage.
    for(unsigned kind:{1u,4u,7u,10u,11u,15u,16u})if(!(families&(1u<<kind)))
        sink(AudioGameplayState{frame,kind,0xffffffffu});
}
}
