#include "monomon_room.hpp"
#include "text_pack.hpp"
#include "monomon_admission_data.hpp"
#include "monomon_presentation_data.hpp"
#include "monomon_mask_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
namespace hk3ds {
namespace d=monomon_admission_data;
namespace {
constexpr const char* conversations[]={monomon_presentation_data::conversation0,
    monomon_presentation_data::conversation1,monomon_presentation_data::conversation2};
}
void MonomonRoom::reset(){art_={};body_={};quirrel_={};mask_={};glow_={};combat_={};object_={};
    dreamer_={};transition_={};phase_=Phase::Hidden;timer_=0;orbAge_=glowAge_=-1;stage_=conversation_=0;text_=nullptr;
    archive_=guardian_=maskScene_=prompt_=verticalHeld_=transitionReady_=defeated_=rewardCommitted_=false;}
void MonomonRoom::play(AnimationPlayer& animation,const char* clip){
    char name[80];std::snprintf(name,sizeof(name),"Monomon/%s",clip);animation.play(art_.require(name));
}
bool MonomonRoom::enter(const char* root,const char* scene,const NailBank& nails,const SavedPlayerData& data){
    reset();archive_=!std::strcmp(scene,"Fungus3_archive_02");guardian_=!std::strcmp(scene,"Dream_Guardian_Monomon");
    maskScene_=!std::strcmp(scene,"Cutscene_Boss_Door");
    if(!active())return true;
    // Bounded encoding of the original string field: 1 = Fungus3_archive_02.
    // Reject an unsupported caller instead of silently returning to the wrong room.
    if(maskScene_){
        if(data.get("maskToBreak")!=2||data.get("dreamReturnScene")!=1)return false;
        // A save can land after the flag commits but before the room returns.
        // Resume that return immediately instead of replaying the mask sequence.
        if(data.get("maskBrokenMonomon"))timer_=d::maskShakePause+d::maskShake+d::maskFade;
        return true;
    }
    char path[300];std::snprintf(path,sizeof(path),"%s/monomon-%s-art.bin",root,archive_?"archive":"guardian");
    if(!art_.load(path))return false;
    defeated_=data.get("monomonDefeated")!=0;
    if(archive_){
        phase_=data.get("summonedMonomon")?Phase::Ready:Phase::Hidden;
        play(body_,"Monomon");play(quirrel_,phase_==Phase::Ready?"Wounded":"Stand");play(mask_,"Mask Tendrils");
    }else{
        dreamer_.reset(defeated_);play(body_,"Idle");
        object_.bounds=d::guardianBox;object_.name="Dreamer Monomon";object_.layer=11;
        object_.hasHealth=true;object_.health.hp=std::numeric_limits<int>::max();
        // The source has a damage FSM, not HealthManager. This contact bridge
        // grants no soul and leaves its cooldown to the source FSM's Hit state.
        object_.health.enemyType=6;object_.health.alternateHitAnimation=true;
        object_.receiver=this;object_.onHit=[](void* context,const NailHit& hit,HitOutcome outcome){
            if(outcome==HitOutcome::Damaged)static_cast<MonomonRoom*>(context)->dreamer_.hit(hit);};
        if(!combat_.bind(&object_,1,&nails))return false;
    }
    return true;
}
bool MonomonRoom::locked()const{
    return active()&&(maskScene_||text_||phase_==Phase::Summoning||phase_==Phase::Entering||
        (guardian_&&dreamer_.locked()));
}
float MonomonRoom::visualAlpha(unsigned owner)const{
    if(maskScene_){
        if(owner==35)return std::clamp(monomon_mask_data::sample(monomon_mask_data::faderAlpha,timer_),0.f,1.f);
        if(owner==61)return std::clamp(monomon_mask_data::sample(monomon_mask_data::blackMaskAlpha,timer_),0.f,1.f);
    }
    if(!guardian_||orbAge_<d::appearOrbPause)return 1;
    for(auto orbOwner:d::appearOrbOwners)if(owner==orbOwner)
        return std::max(0.f,1-(orbAge_-d::appearOrbPause)/d::appearOrbFade);
    return 1;
}
void MonomonRoom::present(RoomDraw& draw)const{
    if(!maskScene_)return;
    namespace m=monomon_mask_data;
    if(draw.owner==18||draw.owner==26){draw.values[10]=0;return;}
    if(draw.owner>=m::ownerBase&&draw.owner<m::ownerBase+48){
        const auto index=draw.owner-m::ownerBase;
        const bool crack=index>=24;
        const auto selected=unsigned((crack?m::sample(m::crackSprite,timer_):m::sample(m::bodySprite,timer_))+.5f);
        if(index!=selected||(crack&&m::sample(m::crackActive,timer_)<.5f)){
            draw.values[10]=0;return;
        }
        if(crack){
            // Source actor26 local transform; its parent has unit world XY scale.
            draw.values[2]+=m::sample(m::crackX,timer_)-(-.3899993896484375f);
            draw.values[5]+=m::sample(m::crackY,timer_)-.010000228881835938f;
            const float sx=m::sample(m::crackScaleX,timer_)/4.914999485015869f;
            const float sy=m::sample(m::crackScaleY,timer_)/2.191577196121216f;
            draw.values[0]*=sx;draw.values[3]*=sx;draw.values[1]*=sy;draw.values[4]*=sy;
        }
        draw.values[2]+=m::sample(m::rootX,timer_)-19.337779998779297f;
        draw.values[5]+=m::sample(m::rootY,timer_)-23.96939468383789f;
        draw.values[6]+=m::sample(m::rootZ,timer_)-1.6299999952316284f;
        return;
    }
    for(auto owner:monomon_mask_data::children)if(draw.owner==owner){
        draw.values[2]+=m::sample(m::rootX,timer_)-19.337779998779297f;
        draw.values[5]+=m::sample(m::rootY,timer_)-23.96939468383789f;
        draw.values[6]+=monomon_mask_data::sample(monomon_mask_data::rootZ,timer_)-1.6299999952316284f;
        break;
    }
}
void MonomonRoom::nextDialogue(){
    if(const char* next=std::strchr(text_,'\f')){text_=next+1;return;}
    if(++conversation_<3){text_=text::resolve(conversations[conversation_]);return;}
    text_=nullptr;phase_=Phase::Summoning;timer_=0;stage_=0;play(mask_,"Mask Hold");
}
void MonomonRoom::fixedUpdate(Hero& hero){
    if(guardian_)combat_.fixedUpdate(hero);
}
bool MonomonRoom::update(float dt,Hero& hero,const Input& input,SavedPlayerData& data,bool dreamNailOwned){
    if(!active()||!std::isfinite(dt)||dt<=0||dt>.25f)return false;
    if(guardian_){
        if(orbAge_<0&&overlaps(bodyBounds(hero.position),d::appearOrbBox))orbAge_=0;
        if(orbAge_>=0)orbAge_=std::min(orbAge_+dt,d::appearOrbPause+d::appearOrbFade);
    }
    bool changed=false;
    const bool vertical=std::abs(input.y)>.5f||input.isHeld(Down);
    const bool interact=vertical&&!verticalHeld_;verticalHeld_=vertical;
    const bool wasLocked=locked();
    if(maskScene_){
        if(!data.get("maskBrokenMonomon")){
            if(!data.set("maskBrokenMonomon",1))return false;
            changed=true;
        }
        timer_+=dt;
        if(timer_>=d::maskShakePause+d::maskShake+d::maskFade){
            std::snprintf(transition_.scene,32,"Fungus3_archive_02");
            std::snprintf(transition_.entry,32,"door_dreamReturn");transitionReady_=true;
        }
    }else if(guardian_){
        combat_.update(hero,dt);
        const bool canFocus=hero.contacts.ground&&hero.actor!=Actor::Dead&&hero.actor!=Actor::Damage;
        dreamer_.update(dt,input.isHeld(Focus),canFocus);
        if(dreamer_.phase()>=MonomonPhase::Wounded){
            if(glowAge_<0){glowAge_=0;glow_.play(art_.require("Glow/Wounded"));}
            glowAge_+=dt;glow_.advance(dt);
        }
        // A full save table must not consume the one-shot cinematic edge.
        // Retry the atomic reward before admitting the film or its callback.
        if(dreamer_.phase()==MonomonPhase::Cinematic&&!rewardCommitted_&&awardMonomon(data)){
            rewardCommitted_=true;changed=true;
        }
        object_.colliderEnabled=dreamer_.phase()==MonomonPhase::Guardian;
        play(body_,dreamer_.clip()?dreamer_.clip():"Idle");body_.clock=dreamer_.animationSeconds();
        body_.advance(0);
        if(dreamer_.phase()==MonomonPhase::Complete){
            // The source's intermediate Black Egg mask scene is handled by its
            // scene adapter; never synthesize masksBroken here.
            std::snprintf(transition_.scene,32,"Cutscene_Boss_Door");std::snprintf(transition_.entry,32,"door1");transitionReady_=true;
        }
    }else{
        prompt_=!locked()&&!defeated_&&hero.contacts.ground&&overlaps(bodyBounds(hero.position),d::inspectBox);
        timer_+=dt;
        if(phase_==Phase::Hidden&&prompt_&&interact){phase_=Phase::LeapPause;timer_=0;}
        else if(phase_==Phase::LeapPause&&timer_>=1.5f){phase_=Phase::Leap;timer_=0;play(quirrel_,"Leap In");}
        else if(phase_==Phase::Leap&&quirrel_.complete){phase_=Phase::Landing;timer_=0;play(quirrel_,"Stand");}
        else if(phase_==Phase::Landing&&timer_>=.5f){phase_=Phase::Idle;timer_=0;}
        else if(phase_==Phase::Idle&&prompt_&&interact){phase_=Phase::Talking;conversation_=0;text_=text::resolve(conversations[0]);}
        else if(phase_==Phase::Talking&&(input.isPressed(Jump)||input.isPressed(Attack)||interact))nextDialogue();
        else if(phase_==Phase::Summoning){
            const float waits[]={d::hold,d::going,d::tendrils,d::rise,d::finish};
            if(timer_>=waits[stage_]){timer_-=waits[stage_];++stage_;
                if(stage_==4)play(mask_,"Mask Drop");
                if(stage_==5){
                    if(data.set("summonedMonomon",1)){phase_=Phase::Ready;play(quirrel_,"Wounded");changed=true;}
                    else {stage_=4;timer_=waits[4];}
                }
            }
        }else if(phase_==Phase::Ready&&!defeated_&&dreamNailOwned&&input.isPressed(Dream)&&boxSensorOverlap(bodyBounds(hero.position),d::dreamBox)){
            if(data.set("dreamReturnScene",1)){phase_=Phase::Entering;timer_=0;changed=true;}
        }else if(phase_==Phase::Entering&&timer_>=d::dreamPause){
            std::snprintf(transition_.scene,32,"Dream_Guardian_Monomon");std::snprintf(transition_.entry,32,"door1");transitionReady_=true;
        }
        body_.advance(dt);quirrel_.advance(dt);mask_.advance(dt);
    }
    if(locked()){hero.velocity={};hero.controlEnabled=hero.inputEnabled=false;}
    else if(wasLocked)hero.controlEnabled=hero.inputEnabled=true;
    return changed;
}
void MonomonRoom::capture(MonomonRoomDraw& out)const{
    out={};out.archive=archive_;out.guardian=guardian_;out.maskScene=maskScene_;
    if(maskScene_)out.whiteFade=std::clamp((timer_-d::maskShakePause-d::maskShake)/d::maskWhiteFade,0.f,1.f);
    if(!archive_&&!guardian_)return;
    out.prompt=prompt_;out.text=text_;out.film=cinematicRequested();
    out.body=archive_?d::body:Vec2{60.459999084472656f,24.950000762939453f};out.bodyScale=archive_?d::bodyScale:1;
    out.bodyScale=archive_?animationFacingScale("monomon-archive-art/Monomon",-out.bodyScale,std::abs(out.bodyScale)):
        animationFacingScale("monomon-guardian-art/Monomon",out.bodyScale,std::abs(out.bodyScale));
    out.quirrelScale=animationFacingScale("uumuu-quirrel/Quirrel",-out.quirrelScale);
    if(guardian_&&glowAge_>=0&&!defeated_){
        out.glow={out.body.x+monomon_data::glowX,out.body.y+monomon_data::glowY};
        // approximation: linear fade; original EaseColor uses easing enum21.
        out.glowAlpha=monomon_data::glowAlpha*std::clamp(glowAge_/monomon_data::glowUp,0.f,1.f);
        out.glowAnimation=glow_;
    }
    out.bodyVisible=!defeated_&&(guardian_||phase_==Phase::Ready||(phase_==Phase::Summoning&&stage_>=3));
    out.quirrel=phase_==Phase::Ready?d::wounded:d::quirrel;out.mask=d::mask;
    out.quirrelVisible=archive_&&phase_>=Phase::Leap;out.maskVisible=archive_&&phase_==Phase::Summoning;
    out.bodyAnimation=body_;out.quirrelAnimation=quirrel_;out.maskAnimation=mask_;
}
}
