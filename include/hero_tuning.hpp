#pragma once

// Observations from Steam build 22529139. Sources and limitations:
// docs/reverse-engineering.md. Distances are Unity world units, Y points up.
namespace hk3ds::tuning
{
constexpr double physicsStep = 2822399.0 / 141120000.0;
constexpr double controlStep = 1.0 / 60.0; // Port scheduling choice, not Unity's Update rate.
constexpr float gravity = -60.0f;
constexpr float gravityScale = 0.79f;
constexpr float run = 8.3f, sprint = 10.0f, sprintDashmaster = 11.5f, walk = 6.0f;
constexpr float jump = 16.65f;
constexpr int jumpSteps = 9, minimumJumpSteps = 4, doubleJumpSteps = 9;
constexpr float dash = 20.0f, sharpDash = 28.0f, dashDuration = 0.25f;
constexpr float dashCooldown = 0.6f, dashmasterCooldown = 0.4f, shadeCooldown = 1.5f;
// HeroController.BUMP_VELOCITY / BUMP_VELOCITY_DASH (Steam22529139).
constexpr float dashGroundBump = 4.f, dashAirBump = 5.f;
constexpr float wallKick = 16.0f, wallSlide = -8.0f, wallDeceleration = 0.0f;
constexpr int wallLockShort = 5, wallLockLong = 10, wallSticky = 3;
constexpr float maxFall = 20.0f, bigFall = 1.1f, hardLanding = 0.8f;
constexpr float attackDuration = 0.35f, quickAttackDuration = 0.28f;
constexpr float attackCooldown = 0.41f, quickAttackCooldown = 0.25f;
constexpr float attackRecovery = 0.1f, alternateReset = 0.5f;
constexpr float bounceSpeed = 12.0f, bounceTime = 0.25f, mushroomSpeed = 25.0f;
constexpr float nailRecoil = 3.75f, longRecoil = 16.0f, nailRecoilDown = 0.0f;
constexpr int nailRecoilSteps = 8;
constexpr float damageRecoil = 15.0f, damageRecoilTime = 0.2f, stalwartRecoilTime = 0.08f;
constexpr float invulnerability = 1.3f, stalwartInvulnerability = 1.75f;
constexpr float freezeDown = 0.001f, freezeWait = 0.25f, freezeUp = 0.05f, freezeScale = 0.0001f;
constexpr int jumpQueue = 2, doubleJumpQueue = 10, dashQueue = 10, attackQueue = 5;
constexpr int ledgeBuffer = 2, headBump = 3;
constexpr float bodyWidth = 0.5f, bodyHeight = 1.28125f, bodyOffsetY = -0.75f;
constexpr float bodyEdgeRadius = 0.0025f;
constexpr float focusButtonTime = 0.25f, focusStartTime = 0.25f;
constexpr float soulDrain = 0.027f, quickSoulDrain = 0.018f, deepFocusMultiplier = 1.65f;
constexpr int focusCost = 33;
constexpr int spiritCost = 33, spiritDamage = 15;
// HeroController.PreventCastByDialogueEnd, copied build 22529139.
constexpr float dialogueCastDelay = .3f;
// FAST MODE estimates until the Spell Control / Fireball FSM export is available.
constexpr float spiritRecovery = .3f, spiritSpeed = 40.f, spiritLife = .7f;
// Build22529139 Fireball2 Top20711 and Spiral22074/13716: speed, active wait,
// shrink wait and collider. Acquisition still uses the shared cast recovery.
constexpr float shadeSoulSpeed=45.f, shadeSoulHitTime=.475f, shadeSoulLife=.55f;
constexpr float firstFocusRefund = 0.2f, repeatFocusRefund = 0.45f;
constexpr float focusHealWait = 0.2f, focusFinishWait = 0.23f;
}
