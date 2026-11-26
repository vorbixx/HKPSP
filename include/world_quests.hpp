#pragma once
#include "collectibles.hpp"
#include "animation.hpp"

namespace hk3ds {
struct SavedPlayerData;
// Typed hand-off to regional NPC scripts and the Dirtmouth shop owner.
// Boss combat and Dreamer/seal events belong to their respective owners.
enum class WorldQuestEvent : unsigned { SlyRescued=1, BrettaRescued=2, Elderbug=3 };
enum class WorldQuestTalk : unsigned { None, Rescue1, Rescue2, Rescue3, Repeat, Rescue4,
    IntroNormal, IntroWalkedBy, IntroVisitedCrossroads, IntroMain, ElderbugGeneric,
    BlackEgg, Infected, Sly, Station, Temple, MapShop, Bretta, Jiji, Cliffs, MinesLift,
    History1, History2, HintRestingGrounds, HintBasin, HintMines, HintCity, HintFungus, HintGreenpath };
// Presentation hand-off: opened follows Box Up; finished means CONVO_FINISH.
enum class WorldQuestInteraction : unsigned { DialogueOpened, ConversationFinished, Cancel };
struct WorldQuestDefinition {
    WorldQuestEvent event=WorldQuestEvent::SlyRescued;
    unsigned sourceId=0,owner=0;
    char scene[32]{};
    Rect range{};
    Vec2 position{},scale{1,1};
    unsigned ownerCount=0;
    std::array<unsigned,16> owners{};
};
class WorldQuests {
    friend struct SimulationStateHash;
public:
    bool load(const char* path);
    // Select one regional source definition; leaving its room clears it.
    bool loadScene(const char* root,const char* scene);
    const char* artPrefix() const;
    void bindArt(const AnimationBank*);
    void step(float seconds);
    const AnimationPlayer& animation() const {return animation_;}
    bool bind(const WorldQuestDefinition&);
    void enter(const char* scene,const CollectibleProgress&,bool rescued=false);
    static const char* playerDataField(WorldQuestEvent);
    // Source FSM120 sets slyRescued on entering Meet, before SLY_RESCUE_1.
    // False includes duplicate completion, unavailable source data and a full ledger.
    bool complete(WorldQuestEvent,Collectibles&);
    bool completed(WorldQuestEvent,const CollectibleProgress&) const;
    // The NPC presentation calls beginTalk after its source Box Up state.
    bool beginTalk(Collectibles&);
    bool beginElderbug(bool visitedCrossroads,const SavedPlayerData&,const char*& flag);
    bool elderbugWalkedBy(Rect hero,bool visitedCrossroads,const SavedPlayerData&) const;
    bool finishDialogue();
    void cancelTalk();
    float titleAlpha() const;
    WorldQuestTalk talk() const {return talk_;}
    bool talking() const {return talk_!=WorldQuestTalk::None;}
    bool awake() const {return awake_;}
    const char* dialogueKey() const;
    const char* text() const;
    bool hides(unsigned owner) const;
    bool present() const {return present_;}
    bool nearby(Rect hero) const;
    const WorldQuestDefinition& definition() const {return definition_;}
    float artScale() const {
        const char* id=definition_.event==WorldQuestEvent::BrettaRescued?"bretta-quest-art/bretta":"world-quest-art/sly";
        return animationFacingScale(id,-definition_.scale.x,std::abs(definition_.scale.x));
    }
private:
    WorldQuestDefinition definition_{};
    std::array<std::array<char,768>,5> dialogues_{};
    std::vector<std::array<char,768>> elderbugDialogues_;
    bool elderbugHintGiven_=false;
    unsigned dialogueIndex() const;
    const char* animationClip(const char* phase) const;
    WorldQuestTalk talk_=WorldQuestTalk::None;
    bool bound_=false,present_=false,inRoom_=false,awake_=false;
    bool titleActive_=false,titleClosing_=false;
    float titleAge_=0,titleCloseAlpha_=0;
    void showTitle();
    const AnimationBank* art_=nullptr;
    AnimationPlayer animation_{};
};

// Presentation is separate from durable quest state. The session owns input and
// dispatches the typed callbacks after Box Up / the last authored page.
class WorldQuestDialogue {
    friend struct SimulationStateHash;
public:
    enum class Phase { Idle, Walking, Turning, Opening, Reading, Closing, Intermission };
    void reset() {*this=WorldQuestDialogue{};}
    void step(float seconds);
    void open() {phase_=Phase::Opening;age_=0;prompt=false;}
    void approach(float heroX,float npcX,bool facingRight,float turnSeconds);
    void observePosition(float heroX);
    void approachEither(float heroX,float npcX,float offset,float speed,bool facingRight,float turnSeconds);
    void approachLeft(float heroX,float npcX,float offset,float speed,bool facingRight,float turnSeconds);
    bool moving() const {return phase_==Phase::Walking||phase_==Phase::Turning;}
    float velocity() const {return phase_==Phase::Walking?walkVelocity_:0;}
    bool facingRight() const {return phase_==Phase::Walking?walkVelocity_>0:faceRight_;}
    // FSM119 Convo Start plays Idle. FSM120's Hero Look animations are disabled.
    const char* heroClip() const {return phase_==Phase::Walking?"Walk":phase_==Phase::Turning?"Turn":locked()?"Idle":nullptr;}
    void read(const char* text);
    // True only when the last page has finished revealing.
    bool proceed();
    void close() {phase_=Phase::Closing;age_=0;}
    // Bretta FSM5960: BOX DOWN, one-second Wait, then BOX UP before Meet 3/4.
    void intermission() {phase_=Phase::Intermission;age_=0;}
    bool locked() const {return phase_!=Phase::Idle;}
    bool visible() const {return phase_==Phase::Opening||phase_==Phase::Reading||phase_==Phase::Closing||
                                (phase_==Phase::Intermission&&age_<.3f);}
    Phase phase() const {return phase_;}
    bool opened() const {return phase_==Phase::Opening&&age_>=.3f;}
    bool pageComplete() const;
    bool hasNextPage() const;
    const char* text() const {return page_.data();}
    unsigned visibleCharacters() const {return unsigned(characters_);}
    bool prompt=false,upHeld=false;
    // Optional original-language choice adapter; ordinary quests default off.
    bool choiceVisible=false,choiceYes=true;
    bool tollLayout=false;
    char yesLabel[32]{},noLabel[32]{},promptLabel[32]{};
private:
    void page();
    Phase phase_=Phase::Idle;
    float age_=0,characters_=0,speed_=65;
    float targetX_=0,walkVelocity_=0,turnSeconds_=0;
    bool faceRight_=false;
    unsigned offset_=0,next_=0;
    std::array<char,768> conversation_{},page_{};
};
}
