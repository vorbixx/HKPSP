#include "cartographer.hpp"
#include "cornifer_data.hpp"
#include "text_pack.hpp"
#include <cmath>
#include <cstring>

namespace hk3ds {
float Cartographer::artScale() const {
    return animationFacingScale("cornifer-original/cornifer-original",-corniferdata::scaleX,std::abs(corniferdata::scaleX));
}
namespace {
const char* localized(const char* key){
    for(const auto& entry:corniferdata::texts)if(!std::strcmp(entry.key,key))return text::resolve(entry.text);
    return "";
}
const char* dialogueKey(Cartographer::Dialogue dialogue){
    using Dialogue=Cartographer::Dialogue;
    switch(dialogue){
    case Dialogue::Meet:return "CORNIFER_MEET";
    case Dialogue::Greet:return "CROSSROADS_GREET";
    case Dialogue::Again:return "CORNIFER_AGAIN";
    case Dialogue::Prompt:return "CORNIFER_PROMPT";
    case Dialogue::Refuse:return "CORNIFER_REFUSE";
    case Dialogue::NotEnough:return "CORNIFER_NOT_ENOUGH";
    case Dialogue::Bought:return "CROSSROADS_BOUGHT";
    case Dialogue::Card:return "CORNIFER_CARD";
    case Dialogue::Iselda:return "CORNIFER_PROMPT";
    case Dialogue::Received:return "MAP_NAME_CROSSROADS";
    default:return "";
    }
}
}
void Cartographer::talk(Dialogue next){
    dialogue=next;page_=0;yes=true;
    if(next==Dialogue::Meet||next==Dialogue::Greet)spoken_=true;
    if(present&&next!=Dialogue::Prompt&&next!=Dialogue::Received)++voiceSerial;
}
void Cartographer::close(){dialogue=Dialogue::None;page_=0;}
void Cartographer::enter(const char* scene,bool mapperShopOpen){
    close();prompt=false;verticalHeld_=true;spoken_=false;
    if(progress.ownsCrossroads())progress.flags|=CartographySave::LeftCrossroads;
    const bool crossroads=scene&&!std::strcmp(scene,"Crossroads_33");
    present=crossroads&&!(progress.flags&(CartographySave::LeftCrossroads|CartographySave::AtHome));
    card=crossroads&&!present&&(progress.flags&CartographySave::LeftCrossroads);
    shop=mapperShopOpen&&scene&&!std::strcmp(scene,"Room_mapper")&&!progress.ownsCrossroads()&&(progress.flags&CartographySave::LeftCrossroads);
    position={corniferdata::x,corniferdata::y};
    if(bank_)animation.play(bank_->require("Idle"),0,true);
}
const char* Cartographer::text() const {
    const char* value=localized(dialogueKey(dialogue));
    for(unsigned page=0;page<page_&&*value;++page){const auto* end=std::strchr(value,'\f');if(!end)return "";value=end+1;}
    return value;
}
bool Cartographer::hasNextPage() const {return std::strchr(text(),'\f')!=nullptr;}
ShopMenuItem Cartographer::shopItem() const {
    return {localized("MAP_NAME_CROSSROADS"),localized("CORNIFER_PROMPT"),nullptr,CartographySave::iseldaPrice};
}
ShopMenuView Cartographer::shopMenu(const ShopMenuItem& item,unsigned geo) const {
    // The Prompt step is the confirm screen: Left/Right set yes, A buys, B backs out (update).
    return {"Iselda",&item,1,0,geo,true,yes};
}
bool Cartographer::update(float seconds,const Input& input,Hero& hero,unsigned& geo){
    const auto oldFlags=progress.flags;const unsigned oldGeo=geo;
    const bool vertical=std::abs(input.y)>.5f||input.isHeld(Down);
    const bool interact=vertical&&!verticalHeld_;verticalHeld_=vertical;
    if(bank_&&present){
        const char* clip=locked()?(hero.position.x<position.x?"Talk L":"Talk R"):"Idle";
        animation.play(bank_->require(clip));animation.advance(seconds);
    }
    const bool alive=hero.actor!=Actor::Dead&&hero.actor!=Actor::Damage;
    prompt=alive&&(present||card||shop)&&!locked()&&hero.contacts.ground&&
        (shop||(std::abs(hero.position.x-position.x)<3&&std::abs(hero.position.y-position.y)<3));
    if(!alive&&locked()){close();hero.inputEnabled=hero.controlEnabled=true;}
    if(locked()){
        hero.velocity={};hero.controlEnabled=hero.inputEnabled=false;
        if(dialogue==Dialogue::Prompt){
            if(input.x>.5f)yes=false;else if(input.x<-.5f)yes=true;
            if(input.isPressed(Attack)){talk(Dialogue::Refuse);}
            else if(input.isPressed(Jump)){
                if(!yes)talk(Dialogue::Refuse);
                else if(progress.buyCrossroads(geo,shop))talk(Dialogue::Received);
                else talk(Dialogue::NotEnough);
            }
        }else if(input.isPressed(Jump)||input.isPressed(Attack)||interact){
            if(hasNextPage())++page_;
            else if(dialogue==Dialogue::Meet||dialogue==Dialogue::Greet||dialogue==Dialogue::Again||dialogue==Dialogue::Iselda)talk(Dialogue::Prompt);
            else {close();hero.inputEnabled=hero.controlEnabled=true;}
        }
    }else if(prompt&&interact){
        if(shop)talk(Dialogue::Iselda);
        else if(card)talk(Dialogue::Card);
        else{
            progress.flags|=CartographySave::EncounteredCrossroads;
            // Convo Choice3889 stops on the first switching action: Meet,
            // then owned map, then the local Spoken branch, then Greet.
            if(!(progress.flags&CartographySave::MetCornifer)){
                progress.flags|=CartographySave::MetCornifer;talk(Dialogue::Meet);
            }else if(progress.ownsCrossroads())talk(Dialogue::Bought);
            else talk(spoken_?Dialogue::Again:Dialogue::Greet);
        }
        hero.velocity={};hero.inputEnabled=hero.controlEnabled=false;
    }
    return oldFlags!=progress.flags||oldGeo!=geo;
}
}
