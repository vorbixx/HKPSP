#include "tutorial.hpp"
#include <algorithm>
#include <cstring>

namespace hk3ds {
namespace {
constexpr unsigned platformColliders[]={2538,2519,2526};
constexpr unsigned platformOwners[]={763,538,646};
constexpr unsigned memorialShieldCollider=1527;
constexpr Rect memorialShieldTrigger{53.562369f,4.245678f,60.479431f,32.654209f};
constexpr Rect memorialInspect{54.46f,5.5767476f,57.46f,6.3632524f};
constexpr Rect nailInspect{263.21f,50.63f,265.21f,51.63f};
constexpr Rect mothInspect{263.15087f,50.60087f,266.24913f,51.16373f};
float revealTime(bool repeat,unsigned index){return (repeat?8.75f:10.75f)+.25f*index;}
}

void TutorialSession::enterDreamNail(){
    using Kind=AbilityPickup::Kind;
    dreamPlatformAge_=-1;
    if(!std::strcmp(sceneName(),"RestingGrounds_04"))abilityPickup.kind=Kind::Memorial;
    else if(!std::strcmp(sceneName(),"Dream_Nailcollection"))abilityPickup.kind=Kind::DreamNail;
    else return;
    abilityPickup.collected=hasAbility(AbilityReward::DreamNail);
    if(abilityPickup.kind==Kind::Memorial&&terrain_)terrain_->setEnabled(memorialShieldCollider,false);
    if(abilityPickup.kind==Kind::DreamNail){
        if(flow.phase==RoomPhase::DoorArrival)flow.fadeIn();
        if(abilityPickup.collected)abilityPickup.phase=AbilityPickup::Phase::Item;
        dreamPlatformsRepeat_=playerData_.get("enteredDreamWorld")!=0;
        if(terrain_)for(const auto collider:platformColliders)terrain_->setEnabled(collider,false);
    }
}

float TutorialSession::dreamPlatformAlpha(unsigned owner) const {
    if(abilityPickup.kind==AbilityPickup::Kind::DreamNail)
        for(unsigned index=0;index<3;++index)if(owner==platformOwners[index])
            return std::clamp((dreamPlatformAge_-revealTime(dreamPlatformsRepeat_,index))/.2f,0.f,1.f);
    return 1;
}

void TutorialSession::captureDreamPlatformAlpha(unsigned source,std::vector<VisualAlphaInput>& output) const {
    if(abilityPickup.kind!=AbilityPickup::Kind::DreamNail)return;
    for(const auto owner:platformOwners){
        const float alpha=dreamPlatformAlpha(owner);
        if(alpha!=1.f)output.push_back({owner,source,alpha});
    }
}

void TutorialSession::updateDreamNail(float dt,const Input& input){
    using Kind=AbilityPickup::Kind;
    using Phase=AbilityPickup::Phase;
    auto& pickup=abilityPickup;
    auto& hero=simulation.hero;
    const bool memorial=pickup.kind==Kind::Memorial;
    const bool awake=hero.contacts.ground&&hero.controlEnabled&&hero.inputEnabled;
    if(!memorial&&(dreamPlatformAge_>=0||awake)){
        const float previous=dreamPlatformAge_;
        if(previous<0){setPlayerData("enteredDreamWorld",1);dreamPlatformAge_=0;}
        dreamPlatformAge_+=dt;
        if(terrain_)for(unsigned index=0;index<3;++index){
            const float reveal=revealTime(dreamPlatformsRepeat_,index);
            if(previous<reveal&&dreamPlatformAge_>=reveal)terrain_->setEnabled(platformColliders[index],true);
        }
    }
    const Rect inspect=memorial?memorialInspect:pickup.mothFinished?nailInspect:mothInspect;
    const Rect body=bodyBounds(hero.position);
    if(memorial&&terrain_){
        const auto& trigger=memorialShieldTrigger;
        if(body.left<trigger.right&&body.right>trigger.left&&body.bottom<trigger.top&&body.top>trigger.bottom)
            pickup.memorialShield=true;
        terrain_->setEnabled(memorialShieldCollider,pickup.memorialShield&&!pickup.collected);
    }
    pickup.near=(memorial||!pickup.collected)&&pickup.dreamText&&pickup.dreamText->count&&
        body.left<inspect.right&&body.right>inspect.left&&
        body.bottom<inspect.top&&body.top>inspect.bottom;
    const bool held=input.y>.5f;
    const bool inspectPressed=held&&!pickup.interactHeld;
    pickup.interactHeld=held;
    const auto phase=[&](Phase next){pickup.phase=next;pickup.age=0;};
    const auto release=[&]{phase(Phase::Idle);hero.controlEnabled=true;};
    if(pickup.phase==Phase::Idle){
        if(!pickup.near||!inspectPressed||!awake||hero.vitals.health<=0||hero.nail.active||hero.dashing||
           hero.actor==Actor::Damage||hero.actor==Actor::Dead||hero.actor==Actor::HardLanding)return;
        hero.stopAttack();hero.stopDash();hero.stopJump();hero.focus=FocusPhase::None;
        hero.jumpQueued=hero.dashQueued=hero.attackQueued=-1;
        if(memorial){pickup.dreamPage=0;phase(Phase::MemorialPan);}
        else if(!pickup.mothFinished){
            const bool met=playerData_.get("dreamNailConvo")!=0;
            pickup.dreamPage=pickup.dreamText->first(met?11:10);
            if(!met)setPlayerData("dreamNailConvo",1);
            phase(Phase::MothRead);
        }else{
            hero.position.x=264.21f;
            if(!collectAbility(AbilityReward::DreamNail))return;
            pickup.collected=true;phase(Phase::DreamAnimation);
        }
    }else{
        pickup.age+=dt;
        const bool confirm=pickup.age>.35f&&(input.isPressed(Jump)||input.isPressed(Attack));
        const auto nextInGroup=[&]{
            const auto& text=*pickup.dreamText;
            const unsigned group=text.pages[pickup.dreamPage++].group;
            return pickup.dreamPage<text.count&&text.pages[pickup.dreamPage].group==group;
        };
        switch(pickup.phase){
        case Phase::MemorialPan:if(pickup.age>=8.5f)phase(Phase::MemorialRead);break;
        case Phase::MemorialRead:if(confirm)phase(nextInGroup()?Phase::MemorialRead:Phase::MemorialClose);break;
        case Phase::MemorialClose:
            if(pickup.age>=2.75f){if(pickup.collected){release();return;}phase(Phase::DreamerStart);}
            break;
        case Phase::DreamerStart:if(pickup.age>=5.3f)phase(Phase::DreamerRead);break;
        case Phase::DreamerRead:
            if(confirm){
                const auto& text=*pickup.dreamText;
                const unsigned group=text.pages[pickup.dreamPage++].group;
                const unsigned next=text.pages[pickup.dreamPage].group;
                if(group==9&&next!=9){release();requestRestTransition("Dream_Nailcollection","door_dreamReturn");return;}
                phase(group!=9&&next==9?Phase::DreamerCast:Phase::DreamerRead);
            }
            break;
        case Phase::DreamerCast:if(pickup.age>=8.9f)phase(Phase::DreamerRead);break;
        case Phase::MothRead:if(confirm)phase(nextInGroup()?Phase::MothRead:Phase::MothLeave);break;
        case Phase::MothLeave:if(pickup.age>=1.6f){pickup.mothFinished=true;release();return;}break;
        case Phase::DreamAnimation:if(pickup.age>=6.2f)phase(Phase::Item);break;
        case Phase::Item:if(confirm){release();requestRestTransition("RestingGrounds_07","door_dreamReturn");return;}break;
        default:break;
        }
    }
    hero.velocity={};hero.controlEnabled=false;
}
}
