#include "hero.hpp"
#include "hero_tuning.hpp"

namespace hk3ds
{
void Hero::updateFocus(float dt)
{
    // Supported Spell Control branch: ordinary stationary focus, including
    // Quick/Deep Focus and first/refocus refunds share Y with tap-to-cast.
    if (actor == Actor::Dead || actor == Actor::Damage || castTime>0) return;
    if (focus == FocusPhase::None)
    {
        if (input.isPressed(Focus)) { focus = FocusPhase::ButtonHeld; focusTime = 0; }
        return;
    }
    focusTime += dt;
    if (focus == FocusPhase::ButtonHeld)
    {
        if (!input.isHeld(Focus)) { focus = FocusPhase::None; startCast(); return; }
        if (focusTime >= tuning::focusButtonTime)
        {
            if (!canFocus() || vitals.soul < tuning::focusCost) { focus = FocusPhase::None; return; }
            focus = FocusPhase::Starting; focusTime = 0; controlEnabled = false;
            velocity.x = 0; stopAttack(); stopJump(); refocusing = false;
        }
        return;
    }
    if ((focus == FocusPhase::Starting || focus == FocusPhase::Draining) && (!input.isHeld(Focus) || !contacts.ground))
    {
        if (focus == FocusPhase::Draining && drainElapsed <= (refocusing ? tuning::repeatFocusRefund : tuning::firstFocusRefund))
            vitals.soul = focusStartSoul;
        focus = FocusPhase::Cancelling; focusTime = 0;
        return;
    }
    if (focus == FocusPhase::Starting && focusTime >= tuning::focusStartTime)
    {
        focus = FocusPhase::Draining; focusTime = drainTime = drainElapsed = 0; drained = 0; focusStartSoul = vitals.soul;
        return;
    }
    if (focus == FocusPhase::Draining)
    {
        drainTime += dt; drainElapsed += dt;
        float interval = charms.quickFocus ? tuning::quickSoulDrain : tuning::soulDrain;
        if (charms.deepFocus) interval *= tuning::deepFocusMultiplier;
        while (drainTime >= interval)
        {
            drainTime -= interval;
            if (vitals.soul > 0) --vitals.soul;
            if (++drained == tuning::focusCost)
            {
                vitals.heal(charms.deepFocus ? 2 : 1);
                focus = FocusPhase::Healing; focusTime = 0; refocusing = true;
                break;
            }
        }
    }
    else if (focus == FocusPhase::Healing && focusTime >= tuning::focusHealWait)
    {
        if (vitals.health < vitals.maxHealth && vitals.soul >= tuning::focusCost)
        {
            focus = FocusPhase::Draining; focusTime = drainTime = drainElapsed = 0; drained = 0; focusStartSoul = vitals.soul;
        }
        else { focus = FocusPhase::Finishing; focusTime = 0; }
    }
    else if ((focus == FocusPhase::Finishing && focusTime >= tuning::focusFinishWait) ||
             (focus == FocusPhase::Cancelling && focusTime >= 3.0f / 12.0f))
    { focus = FocusPhase::None; controlEnabled = true; }
}
}
