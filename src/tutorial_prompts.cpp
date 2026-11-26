#include "tutorial_prompts.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
namespace {
bool overlaps(PromptBounds a,PromptBounds b)
{return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
// Build 22529139: source BoxCollider2D corners composed through all parents.
constexpr PromptBounds attackRange{55.5400633f,7.64479864f,60.0900023f,17.78064364f};
constexpr PromptBounds interactRange{108.5230647f,26.6747955f,114.2899981f,40.1246365f};
constexpr PromptBounds jumpRange{108.8030657f,4.6747989f,114.5699991f,9.9046773f};
constexpr PromptBounds seenRange{101.99346885f,27.982099f,103.72200115f,37.906703f};
}
void TutorialPrompts::reset(unsigned savedFlags)
{
    *this={};flags_=savedFlags&(seenFocusFlag|focusPromptedFlag|interactFlag);
    ++revision_;
}
bool TutorialPrompts::remind(TutorialReminder reminder)
{
    if(phase_!=ReminderPhase::Idle||reminder==TutorialReminder::None)return false;
    reminder_=reminder;phase_=ReminderPhase::Showing;age_=phaseTime_=alpha_=0;
    ++revision_;return true;
}
void TutorialPrompts::down()
{
    phase_=ReminderPhase::Fading;phaseTime_=0;
    // FadeGroup.FadeDown starts its full-duration timer (unlike ColorFader).
    alpha_=1;++revision_;
}
void TutorialPrompts::lorePromptUp()
{
    if(reminder_==TutorialReminder::Interact&&phase_==ReminderPhase::Showing)down();
    focusEnd(true);
}
void TutorialPrompts::focusEnd(bool quick)
{
    if(reminder_!=TutorialReminder::Focus||phase_!=ReminderPhase::Showing)return;
    if(quick)down();
    else {phase_=ReminderPhase::Pause;phaseTime_=0;++revision_;}
}
void TutorialPrompts::beginTabletFocus()
{
    tabletActive_=true;tabletDown_=false;tabletFrom_=tabletAlpha_;tabletTime_=0;
    ++revision_;
}
void TutorialPrompts::endTabletFocus()
{
    if(!tabletActive_)return;
    tabletDown_=true;tabletFrom_=tabletAlpha_;tabletTime_=0;++revision_;
}
void TutorialPrompts::step(float dt,const PromptObservation& o)
{
    if(!std::isfinite(dt)||dt<=0)return;
    const auto oldFlags=flags_;const auto oldAlpha=alpha_,oldTabletAlpha=tabletAlpha_;
    if(o.visitedDirtmouth)flags_|=interactFlag;
    if(overlaps(o.hero,seenRange))flags_|=seenFocusFlag;
    if(!attackSent_&&o.attackDoorActive&&overlaps(o.hero,attackRange))
    {attackSent_=true;remind(TutorialReminder::Attack);}
    if(!jumpSent_&&o.jumpDoorActive&&overlaps(o.hero,jumpRange)){
        jumpTime_+=dt;
        if(jumpTime_>=4){jumpSent_=true;remind(TutorialReminder::Jump);}
    }else jumpTime_=0;
    if(!(flags_&interactFlag)&&overlaps(o.hero,interactRange))
    {flags_|=interactFlag;remind(TutorialReminder::Interact);}
    // Knight Damage/Check Focus Prompt and Soul Orb/Check Focus Prompt.
    if((o.damaged||o.soulGained)&&(flags_&seenFocusFlag)&&!(flags_&focusPromptedFlag)&&
       o.health>0&&o.health<o.maxHealth&&o.soul>=33){
        flags_|=focusPromptedFlag;remind(TutorialReminder::Focus);
    }
    age_+=dt;phaseTime_+=dt;
    if(phase_!=ReminderPhase::Idle&&phase_!=ReminderPhase::Fading)alpha_=std::min(1.f,age_);
    if(phase_==ReminderPhase::Showing){
        if(reminder_==TutorialReminder::Focus&&o.focusStarted)focusEnd();
        else if((reminder_==TutorialReminder::Attack&&o.attackPressed)||
                (reminder_==TutorialReminder::Jump&&(o.x<109||o.x>114))||
                (reminder_==TutorialReminder::Interact&&age_>=4)){
            phase_=ReminderPhase::MinimumWait;phaseTime_=0;
        }
    }else if(phase_==ReminderPhase::MinimumWait&&age_>1){
        phase_=ReminderPhase::Pause;phaseTime_=0;
    }else if(phase_==ReminderPhase::Pause&&phaseTime_>=1)down();
    else if(phase_==ReminderPhase::Fading){
        alpha_=std::max(0.f,1-phaseTime_);
        if(phaseTime_>=1){phase_=ReminderPhase::Idle;reminder_=TutorialReminder::None;}
    }
    if(tabletActive_){
        tabletTime_+=dt;
        if(tabletDown_){
            tabletAlpha_=tabletFrom_*(1-std::min(1.f,tabletTime_/.15f));
            // Fixed-step float accumulation must still assign the exact target
            // at the authored deadline (ColorFader's final assignment).
            if(tabletTime_+1e-6f>=.15f){tabletAlpha_=0;tabletActive_=false;}
        // Accumulating 240 float ticks falls a few microseconds short of four
        // seconds. ColorFader assigns the exact target at its deadline.
        }else if(tabletTime_+1e-5f>=4)tabletAlpha_=1;
        else if(tabletTime_>=3)
            tabletAlpha_=tabletFrom_+(1-tabletFrom_)*std::min(1.f,tabletTime_-3);
    }
    if(flags_!=oldFlags||alpha_!=oldAlpha||tabletAlpha_!=oldTabletAlpha)++revision_;
}
const char* TutorialPrompts::text() const
{
    switch(reminder_){
    case TutorialReminder::Attack:return "Attack";
    case TutorialReminder::Jump:return "Jump";
    case TutorialReminder::Interact:return "Interact";
    case TutorialReminder::Focus:return "Focus";
    default:return "";
    }
}
const char* TutorialPrompts::button() const
{
    switch(reminder_){
    case TutorialReminder::Attack:return "X";
    case TutorialReminder::Jump:return "B";
    case TutorialReminder::Interact:return "^";
    case TutorialReminder::Focus:return "Y";
    default:return "";
    }
}
}
