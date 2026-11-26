#include "text_pack.hpp"
#include "world_quests.hpp"
#include "area_title_data.hpp"
#include "saved_player_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
constexpr const char* elderbugKeys[]={
    "ELDERBUG_INTRO_NORMAL","ELDERBUG_INTRO_WALKEDBY","ELDERBUG_INTRO_VISITEDCROSSROAD",
    "ELDERBUG_INTRO_MAIN","ELDERBUG_GENERIC_2","ELDERBUG_BLACK_EGG_OPENED","ELDERBUG_CROSSROADS_INFECTED",
    "ELDERBUG_SHOPKEEP_RETURNED","ELDERBUG_STAG_STATION_REOPENED","ELDERBUG_TEMPLE_VISITED",
    "ELDERBUG_MAP_SHOP_OPEN","ELDERBUG_BRETTA_RETURNED","ELDERBUG_JIJI_DOOR","ELDERBUG_KINGS_PASS",
    "ELDERBUG_MINES_LIFT","ELDERBUG_HISTORY_1","ELDERBUG_HISTORY_2","ELDERBUG_HINT_RESTING_GROUNDS",
    "ELDERBUG_HINT_ANCIENT_BASIN","ELDERBUG_HINT_CRYSTAL_PEAK","ELDERBUG_HINT_CITY_OF_TEARS",
    "ELDERBUG_HINT_FUNGAL_WASTES","ELDERBUG_HINT_GREENPATH"};
constexpr unsigned elderbugCount=sizeof(elderbugKeys)/sizeof(*elderbugKeys);
static_assert(elderbugCount==unsigned(WorldQuestTalk::HintGreenpath)-unsigned(WorldQuestTalk::IntroNormal)+1);
}
bool WorldQuests::bind(const WorldQuestDefinition& d) {
    bound_=present_=inRoom_=awake_=false;talk_=WorldQuestTalk::None;dialogues_={};
    titleActive_=titleClosing_=false;titleAge_=0;
    elderbugDialogues_={};elderbugHintGiven_=false;
    const bool bretta=d.event==WorldQuestEvent::BrettaRescued;
    const bool elderbug=d.event==WorldQuestEvent::Elderbug;
    if((d.event!=WorldQuestEvent::SlyRescued&&!bretta&&!elderbug)||!d.owner||!d.sourceId||
       d.sourceId!=(elderbug?7000000u:bretta?187000000u:20000000u)+d.owner||d.owner>=1000000u||
       !std::memchr(d.scene,0,sizeof(d.scene))||std::strcmp(d.scene,elderbug?"Town":bretta?"Fungus2_23":"Room_ruinhouse")||
       !std::isfinite(d.range.left)||!std::isfinite(d.range.right)||
       !std::isfinite(d.range.bottom)||!std::isfinite(d.range.top)||
       !std::isfinite(d.position.x)||!std::isfinite(d.position.y)||
       !std::isfinite(d.scale.x)||!std::isfinite(d.scale.y)||d.scale.x==0||d.scale.y==0||
       d.range.left>=d.range.right||d.range.bottom>=d.range.top||d.ownerCount>d.owners.size())return false;
    for(unsigned i=0;i<d.ownerCount;++i){
        if(!d.owners[i])return false;
        for(unsigned j=0;j<i;++j)if(d.owners[i]==d.owners[j])return false;
    }
    definition_=d;bound_=true;return true;
}
bool WorldQuests::loadScene(const char* root,const char* scene) {
    const char* name=nullptr;
    if(scene&&!std::strcmp(scene,"Room_ruinhouse"))name="world-quests.bin";
    else if(scene&&!std::strcmp(scene,"Fungus2_23"))name="bretta-quest.bin";
    else if(scene&&!std::strcmp(scene,"Town"))name="elderbug.bin";
    if(!root||!name)return bind({});
    char path[512];
    const int length=std::snprintf(path,sizeof(path),"%s/%s",root,name);
    if(length<0||static_cast<unsigned>(length)>=sizeof(path))return bind({});
    return load(path);
}
const char* WorldQuests::artPrefix() const {
    if(definition_.event==WorldQuestEvent::Elderbug)return "elderbug";
    return definition_.event==WorldQuestEvent::BrettaRescued?"bretta-quest":"world-quest";
}
bool WorldQuests::load(const char* path) {
    // Source NPC GameObject key, using the existing scene*1,000,000+owner ledger.
    // No HKSV fields, flags, version or migration are allocated by this module.
    bound_=present_=inRoom_=awake_=false;talk_=WorldQuestTalk::None;dialogues_={};
    titleActive_=titleClosing_=false;titleAge_=0;
    auto* f=std::fopen(path,"rb");if(!f)return false;
    WorldQuestDefinition d;char magic[4];unsigned event=0;
    std::array<std::array<char,768>,5> dialogues{};
    bool ok=std::fread(magic,1,4,f)==4&&(!std::memcmp(magic,"WQ01",4)||!std::memcmp(magic,"WQ02",4)||!std::memcmp(magic,"WQ03",4)||!std::memcmp(magic,"WQ04",4))&&
        std::fread(&event,4,1,f)==1&&std::fread(&d.sourceId,4,1,f)==1&&
        std::fread(&d.owner,4,1,f)==1&&std::fread(d.scene,1,32,f)==32&&
        std::fread(&d.range,sizeof(Rect),1,f)==1;
    if(ok&&(!std::memcmp(magic,"WQ03",4)||!std::memcmp(magic,"WQ04",4)))ok=std::fread(&d.position,sizeof(Vec2),1,f)==1&&
        std::fread(&d.scale,sizeof(Vec2),1,f)==1&&std::isfinite(d.position.x)&&std::isfinite(d.position.y)&&
        std::isfinite(d.scale.x)&&std::isfinite(d.scale.y)&&d.scale.x!=0&&d.scale.y!=0;
    const bool elderbug=ok&&!std::memcmp(magic,"WQ04",4);
    ok=ok&&(elderbug==(event==unsigned(WorldQuestEvent::Elderbug)));
    std::vector<std::array<char,768>> elderbugDialogues(elderbug?elderbugCount:0);
    if(ok&&std::memcmp(magic,"WQ01",4)){
        ok=std::fread(&d.ownerCount,4,1,f)==1&&d.ownerCount>0&&d.ownerCount<=d.owners.size()&&
            std::fread(d.owners.data(),4,d.ownerCount,f)==d.ownerCount;
        for(unsigned i=0;i<(elderbug?elderbugCount:4u);++i){
            auto& text=elderbug?elderbugDialogues[i]:dialogues[i];
            unsigned length=0;
            if(!ok)break;
            ok=std::fread(&length,4,1,f)==1&&length>0&&length<text.size()&&
                std::fread(text.data(),1,length,f)==length&&!std::memchr(text.data(),0,length);
        }
    }
    ok=ok&&std::fgetc(f)==EOF&&!std::ferror(f);
    std::fclose(f);d.event=WorldQuestEvent(event);
    if(!ok||!bind(d))return false;
    dialogues_=dialogues;elderbugDialogues_.swap(elderbugDialogues);return true;
}
const char* WorldQuests::playerDataField(WorldQuestEvent event) {
    return event==WorldQuestEvent::SlyRescued?"slyRescued":event==WorldQuestEvent::BrettaRescued?"brettaRescued":nullptr;
}
void WorldQuests::enter(const char* scene,const CollectibleProgress& progress,bool rescued) {
    inRoom_=bound_&&scene&&!std::strcmp(scene,definition_.scene);
    present_=inRoom_&&!rescued&&!completed(definition_.event,progress);
    talk_=WorldQuestTalk::None;awake_=false;
    elderbugHintGiven_=false;
    titleActive_=titleClosing_=false;titleAge_=0;
    bindArt(art_);
}
void WorldQuests::bindArt(const AnimationBank* art) {
    art_=art;animation_={};
    if(art_)animation_.play(art_->require(animationClip(awake_?"Idle":"Dazed")));
}
const char* WorldQuests::animationClip(const char* phase) const {
    if(definition_.event==WorldQuestEvent::Elderbug)return "Elderbug Idle";
    const bool bretta=definition_.event==WorldQuestEvent::BrettaRescued;
    if(!std::strcmp(phase,"Wake"))return bretta?"Bretta Wake":"Sly Wake";
    if(!std::strcmp(phase,"Idle"))return bretta?"Bretta Wake":"Sly Idle";
    return bretta?"Bretta Dazed":"Sly Dazed";
}
void WorldQuests::step(float seconds) {
    if(!std::isfinite(seconds)||seconds<=0)return;
    if(titleActive_){
        // Shared Area Title FSM: NPC bypasses the six-second Wait and holds
        // until NPC TITLE DOWN. FadeGroup uses 1.5s up / 0.5s fast down.
        const float end=titleClosing_?.5f:areatitledata::fadeIn;
        titleAge_=std::min(titleAge_+seconds,end);
        if(titleClosing_&&titleAge_>=end)titleActive_=false;
    }
    if(!present_||!art_)return;
    animation_.advance(seconds);
    if(awake_&&animation_.complete)animation_.play(art_->require(animationClip("Idle")));
}
bool WorldQuests::nearby(Rect h) const {
    const auto r=definition_.range;
    return present_&&h.left<r.right&&h.right>r.left&&h.bottom<r.top&&h.top>r.bottom;
}
bool WorldQuests::elderbugWalkedBy(Rect hero,bool visitedCrossroads,const SavedPlayerData& data) const {
    constexpr Rect callRange{138.49287f,9.465004f,146.41555f,26.725105f};
    return present_&&definition_.event==WorldQuestEvent::Elderbug&&
           hero.left<callRange.right&&hero.right>callRange.left&&
           hero.bottom<callRange.top&&hero.top>callRange.bottom&&
           !visitedCrossroads&&!data.get("metElderbug")&&data.get("elderbug")==0;
}
bool WorldQuests::completed(WorldQuestEvent event,const CollectibleProgress& p) const {
    return bound_&&event==definition_.event&&playerDataField(event)&&p.contains(definition_.sourceId);
}
bool WorldQuests::complete(WorldQuestEvent event,Collectibles& collectibles) {
    if(!present_||event!=definition_.event||!playerDataField(event))return false;
    // Keep the rescued NPC in the current room; relocation is evaluated on entry.
    return collectibles.recordSourceEvent(definition_.sourceId);
}
bool WorldQuests::beginTalk(Collectibles& collectibles) {
    if(!present_||talking())return false;
    if(completed(definition_.event,collectibles.saved())){talk_=WorldQuestTalk::Repeat;showTitle();}
    else {
        if(!complete(definition_.event,collectibles))return false;
        talk_=WorldQuestTalk::Rescue1;
    }
    return true;
}
bool WorldQuests::finishDialogue() {
    if(definition_.event==WorldQuestEvent::Elderbug&&talk_>=WorldQuestTalk::IntroMain){cancelTalk();return true;}
    switch(talk_){
    case WorldQuestTalk::IntroNormal:case WorldQuestTalk::IntroWalkedBy:case WorldQuestTalk::IntroVisitedCrossroads:
        talk_=WorldQuestTalk::IntroMain;break;
    case WorldQuestTalk::Rescue1:talk_=WorldQuestTalk::Rescue2;awake_=true;
        if(art_)animation_.play(art_->require(animationClip("Wake")));
        break;
    case WorldQuestTalk::Rescue2:talk_=WorldQuestTalk::Rescue3;showTitle();break;
    case WorldQuestTalk::Rescue3:
        if(definition_.event==WorldQuestEvent::BrettaRescued){talk_=WorldQuestTalk::Rescue4;break;}
        cancelTalk();break;
    case WorldQuestTalk::Rescue4:case WorldQuestTalk::Repeat:cancelTalk();break;
    default:return false;
    }
    return true;
}
bool WorldQuests::beginElderbug(bool visitedCrossroads,const SavedPlayerData& data,const char*& flag) {
    flag=nullptr;
    if(!present_||talking()||definition_.event!=WorldQuestEvent::Elderbug)return false;
    if(!data.get("metElderbug")){
        talk_=visitedCrossroads?WorldQuestTalk::IntroVisitedCrossroads:
              data.get("elderbug")==1?WorldQuestTalk::IntroWalkedBy:WorldQuestTalk::IntroNormal;
        return true;
    }
    using Talk=WorldQuestTalk;
    static constexpr struct {Talk talk;const char* condition;const char* said;} conversations[]={
        {Talk::BlackEgg,"openedBlackEggDoor","elderbugSpeechFinalBossDoor"},
        {Talk::Infected,"visitedCrossroadsInfected","elderbugSpeechInfectedCrossroads"},
        {Talk::Sly,"slyRescued","elderbugSpeechSly"},
        {Talk::Station,"openedTownBuilding","elderbugSpeechStation"},
        {Talk::Temple,"eggTempleVisited","elderbugSpeechEggTemple"},
        {Talk::MapShop,"openedMapperShop","elderbugSpeechMapShop"},
        {Talk::Bretta,"brettaRescued","elderbugSpeechBretta"},
        {Talk::Jiji,"jijiDoorUnlocked","elderbugSpeechJiji"},
        {Talk::Cliffs,"visitedCliffs","elderbugSpeechKingsPass"},
        {Talk::MinesLift,"mineLiftOpened","elderbugSpeechMinesLift"},
        {Talk::History1,nullptr,"elderbugHistory1"},
        {Talk::History2,"visitedRuins","elderbugHistory2"}};
    for(const auto& entry:conversations){
        if(entry.talk==Talk::Jiji&&data.get("permadeathMode"))continue;
        if((!entry.condition||data.get(entry.condition))&&!data.get(entry.said)){
            talk_=entry.talk;flag=entry.said;return true;
        }
    }
    talk_=Talk::ElderbugGeneric;
    if(elderbugHintGiven_)return true;
    elderbugHintGiven_=true;
    static constexpr struct {Talk talk;const char* has;const char* missing;} hints[]={
        {Talk::HintRestingGrounds,"hasDoubleJump","hasDreamNail"},
        {Talk::HintBasin,"hasSuperDash","hasDoubleJump"},
        {Talk::HintMines,"mageLordDefeated","hasSuperDash"},
        {Talk::HintCity,"hasWalljump","mageLordDefeated"},
        {Talk::HintFungus,"hasDash","hasWalljump"},
        {Talk::HintGreenpath,"hasSpell","hasDash"}};
    for(const auto& hint:hints)if(data.get(hint.has)&&!data.get(hint.missing)){talk_=hint.talk;break;}
    return true;
}
void WorldQuests::showTitle() {
    if(definition_.event!=WorldQuestEvent::SlyRescued)return;
    titleActive_=true;titleClosing_=false;titleAge_=0;
}
float WorldQuests::titleAlpha() const {
    if(!titleActive_)return 0;
    return titleClosing_?titleCloseAlpha_*(1-titleAge_/.5f):titleAge_/areatitledata::fadeIn;
}
void WorldQuests::cancelTalk() {
    talk_=WorldQuestTalk::None;
    if(titleActive_&&!titleClosing_){
        titleCloseAlpha_=titleAlpha();titleClosing_=true;titleAge_=0;
    }
}
const char* WorldQuests::dialogueKey() const {
    if(talking()&&definition_.event==WorldQuestEvent::Elderbug)return elderbugKeys[dialogueIndex()];
    static const char* keys[]={"SLY_RESCUE_1","SLY_RESCUE_2","SLY_RESCUE_3","SLY_RESCUE_REPEAT"};
    static const char* bretta[]={"BRETTA_RESCUE_1","BRETTA_RESCUE_2","BRETTA_RESCUE_3","BRETTA_RESCUE_4","BRETTA_RESCUE_5"};
    return !talking()?"":definition_.event==WorldQuestEvent::BrettaRescued?bretta[dialogueIndex()]:keys[dialogueIndex()];
}
unsigned WorldQuests::dialogueIndex() const {
    if(definition_.event==WorldQuestEvent::Elderbug)return unsigned(talk_)-unsigned(WorldQuestTalk::IntroNormal);
    if(definition_.event==WorldQuestEvent::BrettaRescued){
        if(talk_==WorldQuestTalk::Rescue4)return 3;
        if(talk_==WorldQuestTalk::Repeat)return 4;
    }
    return unsigned(talk_)-1;
}
const char* WorldQuests::text() const {
    if(!talking())return "";
    const unsigned index=dialogueIndex();
    if(definition_.event==WorldQuestEvent::Elderbug)
        return index<elderbugDialogues_.size()?elderbugDialogues_[index].data():"";
    return dialogues_[index].data();
}
bool WorldQuests::hides(unsigned owner) const {
    if(!inRoom_)return false;
    // A dedicated actor replaces the root snapshot while the NPC is present.
    if(present_)return animation_.clip&&owner==definition_.owner;
    if(owner==definition_.owner)return true;
    for(unsigned i=0;i<definition_.ownerCount;++i)if(owner==definition_.owners[i])return true;
    return false;
}
void WorldQuestDialogue::approach(float heroX,float npcX,bool facingRight,float turnSeconds) {
    // Build 22529139, level20 FSM119: Move To Offset=1.5, Walk velocity=+/-6.
    faceRight_=heroX<npcX;targetX_=npcX+(faceRight_?-1.5f:1.5f);
    walkVelocity_=faceRight_?-6.f:6.f;
    turnSeconds_=std::isfinite(turnSeconds)&&turnSeconds>0?turnSeconds:0;
    age_=0;prompt=false;
    if(std::abs(heroX-npcX)<=1.5f)phase_=Phase::Walking;
    else if(facingRight!=faceRight_)phase_=Phase::Turning;
    else open();
}
void WorldQuestDialogue::approachEither(float heroX,float npcX,float offset,float speed,bool facingRight,float turnSeconds) {
    faceRight_=heroX<npcX;targetX_=npcX+(faceRight_?-offset:offset);
    walkVelocity_=faceRight_?-speed:speed;
    turnSeconds_=std::isfinite(turnSeconds)&&turnSeconds>0?turnSeconds:0;
    age_=0;prompt=false;
    if(std::abs(heroX-npcX)<=offset)phase_=Phase::Walking;
    else if(facingRight!=faceRight_)phase_=Phase::Turning;
    else open();
}
void WorldQuestDialogue::approachLeft(float heroX,float npcX,float offset,float speed,bool facingRight,float turnSeconds) {
    faceRight_=true;targetX_=npcX-offset;walkVelocity_=-speed;
    turnSeconds_=std::isfinite(turnSeconds)&&turnSeconds>0?turnSeconds:0;
    age_=0;prompt=false;
    if(heroX>=targetX_)phase_=Phase::Walking;
    else if(!facingRight)phase_=Phase::Turning;
    else open();
}
void WorldQuestDialogue::observePosition(float heroX) {
    // Source live position comparison, with its scaled three-second Wait fallback.
    if(phase_==Phase::Walking&&((walkVelocity_<0&&heroX<=targetX_)||
        (walkVelocity_>0&&heroX>=targetX_)||age_>=3.f)){
        phase_=Phase::Turning;age_=0;
    }
}
void WorldQuestDialogue::step(float seconds) {
    if(!std::isfinite(seconds)||seconds<=0)return;
    age_+=seconds;
    if(phase_==Phase::Intermission&&age_>=1.f)open();
    if(phase_==Phase::Turning&&age_>=turnSeconds_)open();
    // approximation: the existing lore presenter supplies the reveal cadence.
    if(phase_==Phase::Reading)characters_=std::min(float(std::strlen(text())),characters_+seconds*speed_);
    if(phase_==Phase::Closing&&age_>=.3f){phase_=Phase::Idle;age_=0;}
}
void WorldQuestDialogue::read(const char* value) {
    value=text::resolve(value);  // "@key" dialogue from the text pack.
    std::snprintf(conversation_.data(),conversation_.size(),"%s",value?value:"");
    offset_=0;phase_=Phase::Reading;age_=0;page();
}
void WorldQuestDialogue::page() {
    const char* start=conversation_.data()+offset_;
    const char* end=std::strstr(start,"<page>");
    const auto length=end?unsigned(end-start):unsigned(std::strlen(start));
    std::memcpy(page_.data(),start,length);page_[length]=0;
    next_=end?offset_+length+6:0;characters_=0;speed_=65;
}
bool WorldQuestDialogue::pageComplete() const {return characters_>=std::strlen(text());}
bool WorldQuestDialogue::hasNextPage() const {return next_!=0;}
bool WorldQuestDialogue::proceed() {
    if(phase_!=Phase::Reading)return false;
    if(!pageComplete()){speed_=200;return false;}
    if(next_){offset_=next_;page();return false;}
    return true;
}
}
