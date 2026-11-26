#pragma once
#include <cstdint>

namespace hk3ds {
enum class TutorialReminder : unsigned { None, Attack, Jump, Interact, Focus };
enum class ReminderPhase : unsigned { Idle, Showing, MinimumWait, Pause, Fading };
struct PromptBounds { float left=0,bottom=0,right=0,top=0; };
struct PromptObservation {
    PromptBounds hero;
    float x=0;
    int health=5,maxHealth=5,soul=0;
    bool damaged=false,soulGained=false,attackPressed=false,focusStarted=false;
    bool attackDoorActive=true,jumpDoorActive=true,visitedDirtmouth=false;
};
// Pure presentation state. Tablet code sends these same source events; this
// adapter never takes control, heals, spends Soul, or changes the input latch.
class TutorialPrompts {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned seenFocusFlag=8,focusPromptedFlag=16,interactFlag=32;
    void reset(unsigned savedFlags=0);
    void step(float seconds,const PromptObservation& observation);
    bool remind(TutorialReminder reminder);
    void lorePromptUp();                 // LORE PROMPT UP / HIDE INTERACT REMINDER
    void focusEnd(bool quick=false);      // PROMPT FOCUS END [QUICK]
    void beginTabletFocus();             // Inspection/Foc Prompt Up
    void endTabletFocus();               // Inspection/Down
    TutorialReminder reminder() const {return reminder_;}
    ReminderPhase phase() const {return phase_;}
    float alpha() const {return alpha_;}
    float tabletAlpha() const {return tabletAlpha_;}
    // Small action reminders leave the live bottom page visible.
    bool readingPanelVisible() const {return tabletAlpha_>0;}
    bool tabletActive() const {return tabletActive_;}
    unsigned savedFlags() const {return flags_;}
    unsigned revision() const {return revision_;}
    const char* text() const;
    const char* button() const;
    bool hold() const {return reminder_==TutorialReminder::Focus;}
private:
    TutorialReminder reminder_=TutorialReminder::None;
    ReminderPhase phase_=ReminderPhase::Idle;
    unsigned flags_=0,revision_=0;
    bool attackSent_=false,jumpSent_=false,tabletActive_=false,tabletDown_=false;
    float age_=0,phaseTime_=0,alpha_=0,jumpTime_=0;
    float tabletTime_=0,tabletAlpha_=0,tabletFrom_=0;
    void down();
};
}
