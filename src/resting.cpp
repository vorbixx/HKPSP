#include "resting.hpp"
#include "scene.hpp"
#include "stag_dialogue_data.hpp"
#include "text_pack.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds {
namespace {
// Original Bench Control "Adjust Vector" (0, 0.1, 0) added to the bench transform while seated.
constexpr float seatAdjustY=0.1f;
constexpr unsigned charmCount=13;
constexpr unsigned costs[]={1,2,2,3,1,1,3,4,2,4,2,3,2};
constexpr const char* names[]={"Sprintmaster","Dashmaster","Sharp Shadow","Quick Slash","Steady Body","Stalwart Shell","Quick Focus","Deep Focus","Soul Catcher","Soul Eater","Longnail","Mark of Pride","Fury of the Fallen"};
}
bool RestSave::valid() const {
    return validSceneIdentifier(benchScene,true)&&validSceneIdentifier(shadeScene)&&std::isfinite(benchX)&&std::isfinite(benchY)&&
        std::abs(benchX)<10000&&std::abs(benchY)<10000&&stations<(1u<<stagStationCount)&&ownedCharms<(1u<<charmCount)&&!(equippedCharms&~ownedCharms);
}
void Resting::applyCharms(Hero& h) const {
    const auto bits=saved.equippedCharms;
    bool* fields[]={&h.charms.sprint,&h.charms.dashmaster,&h.charms.sharpShadow,&h.charms.quickSlash,&h.charms.steadyBody,&h.charms.stalwart,
        &h.charms.quickFocus,&h.charms.deepFocus,&h.charms.soulCatcher,&h.charms.soulEater,&h.charms.longnail,&h.charms.markOfPride,&h.charms.fury};
    for(unsigned i=0;i<13;++i)*fields[i]=(bits&(1u<<i))!=0;
}
const char* Resting::charmLabel() const {return names[selection%charmCount];}
bool Resting::update(const Input& in,Hero& h,const std::vector<RestPoint>& points,const char* scene,float seconds,unsigned* geo,SavedPlayerData* playerData,unsigned blockedStations){
    const int direction=vertical(in.y);const bool move=direction&&direction!=vertical_;vertical_=direction;
    if(h.actor==Actor::Dead){leave();return false;}
    const bool stationChanged=updateStation(in,h,seconds,geo,playerData);
    if(stationChanged)return true;
    if(stagPhase==StagPhase::Opening||stagPhase==StagPhase::Toll||stagPhase==StagPhase::Poor||stagPhase==StagPhase::Dialogue||stagPhase==StagPhase::Menu||stagPhase==StagPhase::Departing||stagPhase==StagPhase::Riding||stagPhase==StagPhase::StepOff)return stationChanged;
    if(travelMenu||charmMenu){
        if(in.isPressed(Jump)||in.isPressed(Dash)){travelMenu=charmMenu=false;if(!sitting)h.controlEnabled=true;return false;}
        if(move)selection=(selection+(direction>0?(travelMenu?stagStationCount-1:charmCount-1):1))%(travelMenu?stagStationCount:charmCount);
        if(in.isPressed(Attack)){
            if(travelMenu){if((saved.stations&availableStations&(1u<<selection))&&std::strcmp(scene,stagStations[selection].scene)){
                travel_=int(selection);travelMenu=false;h.controlEnabled=true;return true;}}
            else if(saved.ownedCharms&(1u<<selection)){
                const auto next=saved.equippedCharms^(1u<<selection);unsigned cost=0;
                for(unsigned i=0;i<charmCount;++i)if(next&(1u<<i))cost+=costs[i];
                // approximation: default three notches until notch progression is ported.
                if(cost<=3||!(next&(1u<<selection))){saved.equippedCharms=next;applyCharms(h);return true;}
            }
        }return false;
    }
    if(sitting){
        if(in.isPressed(Attack)){charmMenu=true;selection=0;}
        else if(in.isPressed(Jump)||in.isPressed(Dash)||std::abs(in.x)>.3f||direction<0){
            sitting=false;h.position=standPosition_;h.velocity={};h.controlEnabled=true;h.gravityEnabled=true;
        }
        return false;
    }
    nearby=-1;prompt=false;
    if(h.actor==Actor::Damage||!h.contacts.ground)return false;
    for(unsigned i=0;i<points.size();++i){
        const auto& p=points[i];
        if(station.present&&p.kind)continue;
        if(p.kind&&(p.station>=32||(blockedStations&(1u<<p.station))))continue;
        if(std::abs(h.position.x-p.position.x)<3&&std::abs(h.position.y-p.position.y)<4){nearby=int(i);break;}
    }
    if(nearby<0)return false;
    prompt=true;if(!move||direction<0)return false;
    const auto& p=points[nearby];h.velocity={};h.stopAttack();
    h.controlEnabled=false;
    {
        // Original Bench Control: hero = bench transform + Adjust Vector (0, 0.1, 0).
        // The saved anchor stays the bench transform for respawn and bench lookup.
        standPosition_=h.position;sitting=true;h.gravityEnabled=false;h.position={p.position.x,p.position.y+seatAdjustY};
        h.vitals.health=h.vitals.maxHealth;
        std::snprintf(saved.benchScene,32,"%s",scene);saved.benchX=p.position.x;saved.benchY=p.position.y;
    }return true;
}

void Resting::stagState(StagPhase next){
    stagPhase=next;stagClock_=0;
    const char* clip=next==StagPhase::Arriving||next==StagPhase::Riding||next==StagPhase::Departing?"Arrive":
        "Idle";
    stagAnimation.play(stagBank_?stagBank_->require(clip):nullptr,0,true);
}
bool Resting::stagVisible() const {
    return station.present&&stagPhase!=StagPhase::Hidden&&stagPhase!=StagPhase::Opening&&stagPhase!=StagPhase::Ringing&&stagPhase!=StagPhase::Toll&&stagPhase!=StagPhase::Poor;
}
void Resting::enterStation(const StagConfig& config,bool arrival,Hero& hero,SavedPlayerData& data){
    station=config;travelMenu=false;destination_=-1;dialoguePage_=0;
    stagPosition=config.position;stagHistory.reset(stagPosition);stagState(StagPhase::Hidden);
    if(!config.present)return;
    for(unsigned i=0;i<stagStationCount;++i){
        if(data.get(stagStations[i].flag))saved.stations|=1u<<i;
        if(saved.stations&(1u<<i))data.set(stagStations[i].flag,1);
    }
    if(config.price==0 && config.station!=9){saved.stations|=1u<<config.station;data.set(stagStations[config.station].flag,1);}
    bellAnimation={};standAnimation={};smokeAnimation={};stationVertical_=0;
    bellAnimation.play(stagBank_?stagBank_->require("Bell_idle"):nullptr);
    standAnimation.play(stagBank_?stagBank_->require(stationOpen()?"Bell_stand":"Box_idle"):nullptr);
    if(stationOpen()&&data.get("stagPosition")==stagStations[config.station].originalId)stagState(StagPhase::Idle);
    if(arrival){
        data.set("stagPosition",stagStations[config.station].originalId);
        saved.stations|=1u<<config.station;data.set(stagStations[config.station].flag,1);
        standPosition_=hero.position;stagPosition.x=config.startX;stagHistory.reset(stagPosition);
        hero.controlEnabled=hero.gravityEnabled=false;hero.velocity={};
        stagState(StagPhase::Riding);smokeAnimation.play(stagBank_?stagBank_->require("Arrive_smoke"):nullptr,0,true);++arrivalSerial;
    }
}
const char* Resting::stationText() const {
    return text::resolve(firstMeeting_?stagdata::greeting[std::min(dialoguePage_,unsigned(stagdata::greetingCount-1))]:stagdata::local[station.station].pages[std::min(dialoguePage_,dialogueCount()-1)]);
}
unsigned Resting::dialogueCount() const {return firstMeeting_?stagdata::greetingCount:stagdata::local[station.station].count;}
bool Resting::updateStation(const Input& in,Hero& h,float seconds,unsigned* geo,SavedPlayerData* data){
    tollPrompt=bellPrompt=talkPrompt=false;
    if(!station.present)return false;
    PresentationPosition::Step history(stagHistory,stagPosition);
    const float dt=std::clamp(seconds,0.f,.1f);stagClock_+=dt;stagAnimation.advance(dt);bellAnimation.advance(dt);standAnimation.advance(dt);smokeAnimation.advance(dt);
    if(bellAnimation.complete)bellAnimation.play(stagBank_?stagBank_->require("Bell_idle"):nullptr);
    const bool up=in.y>.3f&&stationVertical_<=0;
    const int direction=vertical(in.y);const bool move=direction&&direction!=stationVertical_;stationVertical_=direction;
    auto near=[&](Vec2 p,float range){return std::abs(h.position.x-p.x)<range&&std::abs(h.position.y-p.y)<4;};
    auto unlock=[&](){saved.stations|=1u<<station.station;if(data)data->set(stagStations[station.station].flag,1);};
    auto regain=[&](){h.controlEnabled=h.gravityEnabled=true;travelMenu=false;};
    const bool back=in.isPressed(Jump)||in.isPressed(Dash);
    if(station.bellScale.x!=0&&stationOpen()&&near(station.bell,3.5f)&&in.isPressed(Attack)&&
       (stagPhase==StagPhase::Hidden||stagPhase==StagPhase::Idle)){
        ++bellSerial;bellAnimation.play(stagBank_?stagBank_->require("Bell_ring L"):nullptr,0,true);
    }
    switch(stagPhase){
    case StagPhase::Opening:
        if(standAnimation.complete||stagClock_>=1.5f){standAnimation.play(stagBank_?stagBank_->require("Bell_stand"):nullptr);stagState(StagPhase::Hidden);}
        break;
    case StagPhase::Toll:
        if(move)yes=!yes;
        if(back||(!yes&&in.isPressed(Attack))){stagState(StagPhase::Hidden);regain();}
        else if(in.isPressed(Attack)&&yes){
            if(!geo||*geo<station.price){stagState(StagPhase::Poor);break;}
            *geo-=station.price;unlock();stagState(StagPhase::Opening);
            standAnimation.play(stagBank_?stagBank_->require("Box_disappear"):nullptr,0,true);regain();return true;
        }break;
    case StagPhase::Poor:
        if(back||in.isPressed(Attack)){stagState(StagPhase::Hidden);regain();}break;
    case StagPhase::Hidden:
        if(!stationOpen()){
            tollPrompt=near(station.toll,3)&&h.contacts.ground;
            if(tollPrompt&&up){stagState(StagPhase::Toll);yes=true;h.controlEnabled=false;h.velocity={};h.stopAttack();}
        }else{
            bellPrompt=station.bellScale.x!=0&&near(station.bell,3.5f);
            if(bellPrompt&&in.isPressed(Attack)){stagState(StagPhase::Ringing);}
        }break;
    case StagPhase::Ringing:
        if(stagClock_>=1.f){if(data)data->set("stagPosition",stagStations[station.station].originalId);stagPosition.x=station.startX;stagState(StagPhase::Arriving);smokeAnimation.play(stagBank_?stagBank_->require("Arrive_smoke"):nullptr,0,true);++arrivalSerial;return true;}break;
    case StagPhase::Arriving:case StagPhase::Riding:{
        // The original Arrive clip contains its approach in the sprite geometry.
        stagPosition=station.position;
        if(stagPhase==StagPhase::Riding){h.position={stagPosition.x,stagPosition.y+2.4f};h.velocity={};}
        if(stagAnimation.complete||(!stagAnimation.clip&&stagClock_>=2.f)){
            if(stagPhase==StagPhase::Riding)stagState(StagPhase::StepOff);else stagState(StagPhase::Idle);
        }break;}
    case StagPhase::StepOff:
        // Step back onto the authored station entry point; never leave a rider airborne.
        h.position=standPosition_;h.velocity={};
        if(stagClock_>=.45f){stagState(StagPhase::Idle);regain();}break;
    case StagPhase::Idle:
        talkPrompt=near(station.position,5)&&h.contacts.ground;
        if(talkPrompt&&up){
            standPosition_=h.position;h.velocity={};h.controlEnabled=false;h.stopAttack();dialoguePage_=0;firstMeeting_=!data||!data->get("metStag");
            stagState(StagPhase::Dialogue);++voiceSerial;
        }break;
    case StagPhase::Dialogue:
        if(back){stagState(StagPhase::Idle);regain();}
        else if(in.isPressed(Attack)){
            if(++dialoguePage_<dialogueCount()){++voiceSerial;break;}
            const bool first=firstMeeting_;if(data)data->set("metStag",1);
            stagState(StagPhase::Menu);travelMenu=true;selection=station.station;
            for(unsigned i=0;i<stagStationCount;++i)if(destinations()&(1u<<i)){selection=i;break;}
            return first;
        }break;
    case StagPhase::Menu:
        if(back){stagState(StagPhase::Idle);regain();break;}
        if(move&&destinations())for(unsigned n=0;n<stagStationCount;++n){selection=(selection+(direction>0?stagStationCount-1:1))%stagStationCount;if(destinations()&(1u<<selection))break;}
        if(in.isPressed(Attack)&&(destinations()&(1u<<selection))){
            destination_=int(selection);travelMenu=false;h.controlEnabled=h.gravityEnabled=false;h.velocity={};
            h.position={stagPosition.x,stagPosition.y+2.4f};stagState(StagPhase::Departing);++departSerial;
        }break;
    case StagPhase::Departing:
        // approximation: reverse the native approach while the ride transition begins.
        if(stagAnimation.clip)stagAnimation.frame=stagAnimation.clip->count-1-std::min<std::uint32_t>(static_cast<std::uint32_t>(stagClock_*stagAnimation.clip->fps),stagAnimation.clip->count-1);
        h.position={stagPosition.x,stagPosition.y+2.4f};h.velocity={};
        if(stagClock_>=1.f&&destination_>=0){travel_=destination_;destination_=-1;}break;
    }
    return false;
}

}
