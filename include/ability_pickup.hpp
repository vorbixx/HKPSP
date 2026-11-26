#pragma once
#include "hero.hpp"
#include "collectibles.hpp"
#include "traversal_abilities.hpp"
#include "geo_visual.hpp"
#include "dream_dialogue.hpp"
#include "text_pack.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace hk3ds {
// Room interaction state; artwork and platform input remain in adapters.
struct AbilityPickup {
    enum class Kind { None, Spirit, Cloak, Claw, Wings, CrystalHeart, IsmasTear, ShadeCloak, ShadeSoul, SimpleKey, DreamNail, Memorial, CityCrest };
    enum class Phase { Idle, Dialogue, Item, Confirm, HeroDown, BigGetFlash, ItemClosing, HeroUp,
                       HeartMoveTo, HeartCollect, HeartBack, HeartFall, HeartLand,
                       MemorialPan, MemorialRead, MemorialClose, DreamerStart, DreamerRead,
                       DreamerCast, MothRead, MothLeave, DreamAnimation };
    Kind kind=Kind::None;
    Phase phase=Phase::Idle;
    Vec2 position{};
    float seconds=0,age=0,popupAge=-1;
    std::uint32_t observedDamage=0;
    std::uint32_t flashSerial=0;
    float flashAge=-1;
    Vec2 flashPosition{};
    Vec2 backOrigin{};
    bool collected=false,near=false,interactHeld=false,touchInside=false,contactPending=false;
    bool available=true,heartBlasted=false,heartBridgeAbsent=false;
    const DreamDialogueBank* dreamText=nullptr;
    unsigned dreamPage=0;
    bool mothFinished=false,memorialShield=false;
    void enter(const char* scene,bool spirit,bool cloak,const CollectibleProgress& progress={}){
        const auto* text=dreamText;*this={};dreamText=text;
        // Serialized Shaman Meeting and Shiny Item world transforms (build 22529139).
        if(!std::strcmp(scene,"Crossroads_ShamanTemple")){kind=Kind::Spirit;position={23.950031f,8.933798f};collected=spirit;}
        if(!std::strcmp(scene,"Fungus1_04")){kind=Kind::Cloak;position={23.22f,27.23f};collected=cloak;}
        if(!std::strcmp(scene,"Ruins1_17")){kind=Kind::SimpleKey;position={79.13999939f,49.11999893f};}
        if(!std::strcmp(scene,"Crossroads_10")){kind=Kind::CityCrest;position={38.66f,2.94f};available=false;}
        if(const auto* source=traversalReward(scene)){
            switch(source->reward){
            case AbilityReward::MantisClaw:kind=Kind::Claw;break;
            case AbilityReward::MonarchWings:kind=Kind::Wings;break;
            case AbilityReward::CrystalHeart:kind=Kind::CrystalHeart;break;
            case AbilityReward::IsmasTear:kind=Kind::IsmasTear;break;
            case AbilityReward::ShadeCloak:kind=Kind::ShadeCloak;break;
            case AbilityReward::ShadeSoul:kind=Kind::ShadeSoul;break;
            default:kind=Kind::None;break;
            }
            position=source->position;collected=progress.contains(source->sourceId);
        }
        // level270 Control6543 destroys this hierarchy only on an owned entry.
        heartBridgeAbsent=kind==Kind::CrystalHeart&&collected;
    }
    AbilityReward reward() const {
        switch(kind){
        case Kind::Spirit:return AbilityReward::VengefulSpirit;
        case Kind::Claw:return AbilityReward::MantisClaw;
        case Kind::Wings:return AbilityReward::MonarchWings;
        case Kind::CrystalHeart:return AbilityReward::CrystalHeart;
        case Kind::IsmasTear:return AbilityReward::IsmasTear;
        case Kind::ShadeCloak:return AbilityReward::ShadeCloak;
        case Kind::ShadeSoul:return AbilityReward::ShadeSoul;
        case Kind::DreamNail:return AbilityReward::DreamNail;
        default:return AbilityReward::MothwingCloak;
        }
    }
    bool hasStandaloneArt() const {return kind==Kind::Spirit||kind==Kind::Cloak;}
    template<class Emit> void visitHiddenOwners(Emit emit) const {
        if(heartBridgeAbsent){
            constexpr unsigned bridge[]={1682,1677,1681,1678,1673,1674,1669,1679,1676,1675,1671,1672,1670,1668,1680};
            for(const auto id:bridge)emit(id);
        }
        if(!collected||hasStandaloneArt()||trinketSequence()||kind==Kind::None)return;
        if(const auto* source=traversalReward(reward()))
            for(unsigned i=0;i<source->visualOwnerCount;++i)emit(source->visualOwners[i]);
    }
    bool hides(unsigned owner) const {
        bool hidden=false;visitHiddenOwners([&](unsigned id){hidden=hidden||id==owner;});return hidden;
    }
    bool locked() const{return phase!=Phase::Idle;}
    bool trinketSequence() const{return kind==Kind::SimpleKey||kind==Kind::CityCrest;}
    bool shinySequence() const{return kind==Kind::Cloak||kind==Kind::Claw||trinketSequence();}
    bool authoredSequence() const{return shinySequence()||kind==Kind::CrystalHeart;}
    // resources7318 Item Get Effect R, child Soul Burst clip1130. Its geometry
    // Geometry includes parent 1.15 * child 1.2. Hero Z=.004, spawn Z=-1,
    // and child local Z=.1 transformed by the parent's Z scale of 1.15.
    GeoVisualDraw flashDraw(const AnimationBank& bank,const char* clip="Fury Flash") const {
        GeoVisualDraw draw{};
        if(flashAge<0||flashAge>=.25f)return draw;
        draw.visible=true;draw.position=flashPosition;draw.depth=-.881f;
        draw.player.play(bank.find(clip),0,true);draw.player.advance(flashAge);
        return draw;
    }
    float flashOpacity() const {
        if(flashAge<0||flashAge>=.25f)return 0;
        // Native SpriteRenderer stores an overshoot here, but renders exactly
        // like alpha=1 (ability_reference effects, responsive half-alpha control).
        if(flashAge<=.2f)return 1;
        // Original Hermite keys: (.2,1,out=-10), (.25,0,in=-20).
        const float t=(flashAge-.2f)/.05f;
        return std::clamp((2*t*t*t-3*t*t+1)-.5f*(t*t*t-2*t*t+t)-(t*t*t-t*t),0.f,1.f);
    }
    bool panelVisible() const{return phase==Phase::Item||phase==Phase::ItemClosing||phase==Phase::Dialogue||phase==Phase::Confirm;}
    const char* heroClip() const {
        if(!authoredSequence()||!locked())return nullptr;
        if(kind==Kind::CrystalHeart){
            switch(phase){
            case Phase::HeartMoveTo:return "Walk";
            case Phase::HeartCollect:return "Collect SD 1";
            case Phase::HeartBack:return "Collect SD 2";
            case Phase::HeartFall:return "Airborne";
            case Phase::HeroUp:return "GetUpToIdle";
            default:return "Collect SD 4";
            }
        }
        return phase==Phase::HeroDown?"Collect Normal 1":phase==Phase::HeroUp?"Collect Normal 3":"Collect Normal 2";
    }
    bool visible() const{return available&&(kind==Kind::Spirit||(kind!=Kind::None&&!collected));}
    // Returns a reward exactly once, when its item panel opens.
    bool update(float dt,const Input& input,Hero& hero){
        seconds+=dt;age+=dt;if(popupAge>=0)popupAge+=dt;
        if(flashAge>=0)flashAge+=dt;
        const bool damaged=observedDamage!=hero.lightDamageSerial;
        observedDamage=hero.lightDamageSerial;
        // Shiny Control's HERO DAMAGED edges exist in Hero Down, Hero Up and
        // Trink Pause. Big Get Flash and the ability message states have none.
        // Do not reset damage recoil, its gravity lock, or an independent popup.
        if(shinySequence()&&damaged&&(phase==Phase::HeroDown||phase==Phase::HeroUp||
           (trinketSequence()&&phase==Phase::Item))){
            phase=Phase::Idle;age=0;hero.inputEnabled=true;return false;
        }
        const auto* source=traversalReward(reward());
        const bool collision=source&&source->collisionBounds.right>source->collisionBounds.left;
        const bool automatic=source&&(source->touchRadius>0||collision);
        const auto bounds=bodyBounds(hero.position);
        const float dx=position.x-std::clamp(position.x,bounds.left,bounds.right);
        const float dy=position.y-std::clamp(position.y,bounds.bottom,bounds.top);
        const auto contact=collision?source->collisionBounds:Rect{};
        const bool touching=automatic&&visible()&&(collision?
            bounds.left<=contact.right&&bounds.right>=contact.left&&bounds.bottom<=contact.top&&bounds.top>=contact.bottom:
            dx*dx+dy*dy<=source->touchRadius*source->touchRadius);
        const bool entered=touching&&!touchInside;touchInside=touching;
        // Get Shadow Dash waits one frame after collision ENTER, then checks
        // onGround once. Rejected airborne contact must exit before retrying.
        const bool contactReady=contactPending;contactPending=collision&&entered;
        near=!automatic&&visible()&&std::abs(hero.position.x-position.x)<2.5f&&std::abs(hero.position.y-position.y)<3;
        const bool sourceInspect=source&&source->inspectBounds.right>source->inspectBounds.left;
        if(sourceInspect){
            const auto b=source->inspectBounds;
            near=visible()&&bounds.left<b.right&&bounds.right>b.left&&bounds.bottom<b.top&&bounds.top>b.bottom;
        }
        if(kind==Kind::SimpleKey){
            const Rect b{77.88377f,48.94722f,80.39623f,49.32762f};
            near=visible()&&bounds.left<b.right&&bounds.right>b.left&&bounds.bottom<b.top&&bounds.top>b.bottom;
        }
        const bool inspectHeld=(kind==Kind::ShadeSoul||kind==Kind::CrystalHeart)?std::abs(input.y)>.6f:
            ((sourceInspect||shinySequence())?input.y>.5f:std::abs(input.y)>.5f);
        const bool interact=inspectHeld&&!interactHeld;interactHeld=inspectHeld;
        const bool confirm=input.isPressed(Jump)||input.isPressed(Attack);
        if(authoredSequence()&&locked()){
            // Shiny Control: Hero Down -> Big Get Flash -> reward-specific
            // message -> Hero Up. Only the grant transition returns true.
            const auto change=[&](Phase next){phase=next;age=0;};
            if(kind==Kind::CrystalHeart){
                // Mines_31 Control6570. Movement/gravity are integrated by the
                // ordinary fixed physics schedule, the knockback is authored tween.
                constexpr float toX=243.8200073242f,groundY=6.4056239128f;
                if(phase==Phase::HeartMoveTo&&hero.position.x>=toX){
                    hero.position.x=toX;hero.velocity={};change(Phase::HeartCollect);
                }else if(phase==Phase::HeartCollect&&age>=1.f){
                    backOrigin=hero.position;hero.gravityEnabled=false;heartBlasted=true;change(Phase::HeartBack);
                }else if(phase==Phase::HeartBack){
                    const float t=std::min(age/.5799999833f,1.f),u=1-t;
                    hero.position.x=backOrigin.x-5*(1-u*u*u);
                    if(age>=7.f/12.f){hero.gravityEnabled=true;change(Phase::HeartFall);}
                }else if(phase==Phase::HeartFall&&hero.position.y<=groundY+.1000000015f){
                    hero.position.y=groundY;hero.velocity={};change(Phase::HeartLand);
                }else if(phase==Phase::HeartLand&&age>=2.5f){
                    change(Phase::Item);collected=true;return true;
                }
            }
            if(phase==Phase::HeroDown&&age>=(kind==Kind::Claw?.5f:.75f)){
                // Big Get Flash / Trink Flash precedes the reward-specific
                // message. Audio and the independent effect share this event.
                ++flashSerial;flashAge=0;flashPosition={hero.position.x,hero.position.y-.76f};
                if(trinketSequence()){change(Phase::Item);popupAge=0;collected=true;return true;}
                change(Phase::BigGetFlash);
            }
            else if(phase==Phase::BigGetFlash&&age>=1.f){change(Phase::Item);collected=true;return true;}
            else if(phase==Phase::Item){
                if(trinketSequence()){if(age>=1.f)change(Phase::HeroUp);}
                // Msg Control Top Up2.5 / Bot Up3 / Stop Up.25. Only Detect
                // listens to Jump, Attack and Cast; early edges are discarded.
                else if(age>=5.75f&&(confirm||input.isPressed(Spell)||input.isPressed(Focus)))change(Phase::ItemClosing);
            }else if(phase==Phase::ItemClosing&&age>=1.f)change(Phase::HeroUp);
            else if(phase==Phase::HeroUp&&age>=(kind==Kind::CrystalHeart?.25f:5.f/12.f)){
                change(Phase::Idle);hero.inputEnabled=hero.controlEnabled=hero.gravityEnabled=true;
                hero.preventCastByDialogueEnd();
            }
            return false;
        }
        if(phase==Phase::Idle){
            const bool requested=collision?(contactReady&&hero.contacts.ground):
                (automatic?entered:(near&&hero.contacts.ground&&interact));
            if(!requested||!hero.controlEnabled||!hero.inputEnabled||
               hero.vitals.health<=0||hero.actor==Actor::Damage||hero.actor==Actor::Dead)return false;
            // Claw Inspect Region791 calls HeroController.CanInspect: a slash
            // becomes inspectable after ATTACK_RECOVERY_TIME, before it ends.
            if(authoredSequence()&&(hero.dashing||hero.freezePhase||
               (hero.nail.active&&hero.nail.elapsed<tuning::attackRecovery)))return false;
            // Shiny Item Acid Control6585 / Can Inspect? rejects all attack
            // directions and dashing before taking control for the item sequence.
            if((kind==Kind::IsmasTear||kind==Kind::ShadeSoul)&&(hero.nail.active||hero.dashing))return false;
            if(authoredSequence()){
                age=0;phase=kind==Kind::CrystalHeart?Phase::HeartMoveTo:Phase::HeroDown;
                hero.velocity={};hero.controlEnabled=hero.inputEnabled=false;
                hero.input={};
                hero.stopAttack();hero.stopDash();hero.stopJump();hero.focus=FocusPhase::None;
                if(kind==Kind::CrystalHeart){hero.facingRight=true;hero.velocity.x=8;}
                return false;
            }
            // Temporary (user 2026-11-26): the Shaman grants Vengeful Spirit at once on Listen;
            // the Shaman conversation and gate quest return once they are verified.
            age=0;phase=kind==Kind::ShadeSoul?Phase::Confirm:Phase::Item;
            if(phase==Phase::Item){collected=true;hero.velocity={};hero.controlEnabled=false;return true;}
        }else if(confirm&&age>.35f){
            age=0;
            if((phase==Phase::Dialogue||(phase==Phase::Confirm&&input.isPressed(Jump)))&&!collected){
                phase=Phase::Item;collected=true;return true;
            }
            phase=Phase::Idle;hero.inputEnabled=hero.controlEnabled=hero.gravityEnabled=true;
            return false;
        }
        if(locked()){hero.velocity={};hero.controlEnabled=false;}
        return false;
    }
    const char* title() const{
        switch(kind){
        case Kind::Spirit:return "VENGEFUL SPIRIT";
        case Kind::Claw:return "MANTIS CLAW";
        case Kind::Wings:return "MONARCH WINGS";
        case Kind::CrystalHeart:return "CRYSTAL HEART";
        case Kind::IsmasTear:return "ISMA'S TEAR";
        case Kind::ShadeCloak:return "SHADE CLOAK";
        case Kind::ShadeSoul:return "SHADE SOUL";
        case Kind::SimpleKey:return "SIMPLE KEY";
        case Kind::CityCrest:return "CITY CREST";
        case Kind::DreamNail:return "DREAM NAIL";
        default:return "MOTHWING CLOAK";
        }
    }
    // Original text rows are "@key"s resolved from the text pack.
    const char* line(unsigned row) const{return text::resolve(lineSource(row));}
    const char* lineSource(unsigned row) const{
        if(phase==Phase::Confirm){
            if(row==0)return title();
            if(row==1)return "Examine the figure?";
            if(row==2)return "B - Yes";
            return row==3?"X - No":"";
        }
        if(phase==Phase::Dialogue){
            static const char* words[]={"Oh ho! A traveller!","Let me offer you a gift.","@Shaman/SHAMAN_MEET","B / X - Continue"};
            return row<4?words[row]:"";
        }
        if(row==0)return title();
        if(trinketSequence())return "";
        if(kind==Kind::ShadeSoul){
            if(row==1)return "Tap Y or press ZR to cast.";
            if(row==2)return "Conjure a powerful shadow.";
            if(row==3)return "Spells consume SOUL.";
        }
        if(kind==Kind::ShadeCloak){
            if(row==1)return "@Prompts/GET_SHADOWDASH_2";
            if(row==2)return "Shadow power needs time to recharge.";
            if(row==3)return "Normal dash works while recharging.";
        }
        if(kind==Kind::CrystalHeart){
            if(row==1)return "Hold A on the ground or wall.";
            if(row==2)return "Release when charged to fly.";
            if(row==3)return "Press A or B to stop in flight.";
        }
        if(kind==Kind::IsmasTear){
            if(row==1)return "Acid no longer blocks your path.";
            if(row==2)return "Swim through pools of acid.";
            if(row==3)return "Protection is always active.";
        }
        if(kind==Kind::Claw){
            if(row==1)return "Press B while sliding against a wall to jump again.";
            if(row==2)return "@Prompts/GET_WALLJUMP_2";
            return "";
        }
        if(kind==Kind::Wings){
            if(row==1)return "Press B again in the air";
            if(row==2)return "to perform a double jump.";
            if(row==3)return "Reach greater heights.";
        }
        if(row==1)return kind==Kind::Spirit?"Tap Y or press ZR to cast.":"Press L to dash forwards.";
        if(row==2)return kind==Kind::Spirit?"Spells consume SOUL.":"@Prompts/GET_DASH_2";
        if(row==3)return kind==Kind::Spirit?"@UI/INV_DESC_SPELL_FIREBALL1":"";
        return "B / X - Continue";
    }
};
}
