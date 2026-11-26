#include "hero.hpp"
#include "hero_tuning.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds
{
namespace
{
bool locked(const Hero& h)
{
    return h.surfaceSwim.active() || !h.controlEnabled || h.castTime>0 || h.actor == Actor::Dead || h.actor == Actor::Damage || h.actor == Actor::HardLanding;
}
void tickQueue(int& q, int limit) { if (q >= 0 && q <= limit) ++q; }
void decay(float& time, float dt) { if (time > 0) time -= dt; }
bool dashBump(const Hero& hero, const World& world)
{
    // Steam22529139 HeroController.CheckForBump, L5126-5179. Only layer 8
    // participates, including query-only terrain, independently of hero contacts.
    const auto b = bodyBounds(hero.position);
    const float side = hero.facingRight ? 1.f : -1.f;
    const float x = hero.facingRight ? b.right - .2f : b.left + .2f;
    const auto ray = [&](Vec2 from, Vec2 direction, float length, float& distance) {
        return rayTerrain(from, direction, length, world, distance, TerrainShape::terrainQueryExcluded);
    };
    float low = 0, high = 0;
    if (!ray({x, b.bottom - .025f}, {side, 0}, .52f, low) ||
        ray({x, b.bottom + .2f}, {side, 0}, .52f, high)) return false;
    const Vec2 hit{x + side * low, b.bottom - .025f};
    float outside = 0, inside = 0;
    if (!ray({hit.x + side * .1f, hit.y + 1}, {0, -1}, 1.5f, outside)) return false;
    return !ray({hit.x - side * .1f, hit.y + 1}, {0, -1}, 1.5f, inside) || outside < inside;
}
}

void Vitals::takeHealth(int amount)
{
    if (amount <= 0) return;
    const int blue = std::min(blueHealth, amount);
    blueHealth -= blue;
    health = std::max(0, health - (amount - blue));
}
void Vitals::heal(int amount) { health = std::min(maxHealth, health + std::max(0, amount)); }
void Vitals::gainSoul(const Charms& c)
{
    ++soulGainSerial; // AddMPCharge sends MP GAIN even when the vessel is full.
    const bool main = soul < maxSoul;
    const int amount = (main ? 11 : 6) + (c.soulCatcher ? (main ? 3 : 2) : 0) + (c.soulEater ? (main ? 8 : 6) : 0);
    const int overflow = std::max(0, soul + amount - maxSoul);
    soul = std::min(maxSoul, soul + amount);
    reserve = std::min(reserveMax, reserve + overflow);
}
bool Hero::canJump() const
{
    return !locked(*this) && !wallSliding && !dashing && !jumping && !bouncing && !mushroomBouncing &&
        (contacts.ground || (ledgeSteps > 0 && headSteps <= 0 && !contacts.roof));
}
bool Hero::canDoubleJump() const
{
    return abilities.doubleJump && !locked(*this) && !doubleJumpUsed && !dashing && !wallSliding &&
        !bouncing && !mushroomBouncing && !contacts.ground;
}
bool Hero::canWallJump() const
{
    return !surfaceSwim.active() && castTime<=0 && abilities.wallJump && !contacts.nonSlider && (wallSliding || ((contacts.left || contacts.right) && !contacts.ground));
}
bool Hero::canWallSlide() const
{
    return abilities.wallJump && !locked(*this) && !contacts.nonSlider && !contacts.ground && !dashing &&
        (falling || wallSliding) && !doubleJumping;
}
bool Hero::canDash() const
{
    return !locked(*this) && abilities.dash && dashCooldown <= 0 && !dashing &&
        (!nail.active || nail.elapsed >= tuning::attackRecovery) && (contacts.ground || !airDashUsed || wallSliding);
}
bool Hero::canAttack() const
{
    return !locked(*this) && !dashing && !nail.active && nail.cooldown <= 0;
}
bool Hero::canFocus() const
{
    return !locked(*this) && !dashing && contacts.ground && (!nail.active || nail.elapsed >= tuning::attackRecovery);
}
bool Hero::canCast() const
{
    return inputEnabled && !locked(*this) && dialogueCastDelay<=0 && abilities.spiritLevel()!=0 && vitals.soul>=tuning::spiritCost &&
        !dashing && (focus==FocusPhase::None || focus==FocusPhase::ButtonHeld);
}
void Hero::preventCastByDialogueEnd()
{
    dialogueCastDelay=tuning::dialogueCastDelay;
}
bool Hero::startCast()
{
    if(!canCast())return false;
    vitals.soul-=tuning::spiritCost;
    stopAttack();stopJump();doubleJumping=wallSliding=wallLocked=bouncing=mushroomBouncing=false;
    jumpQueued=doubleQueued=dashQueued=attackQueued=-1;
    focus=FocusPhase::None;focusTime=0;lookTime=0;lookUp=lookDown=false;
    velocity={};castTime=tuning::spiritRecovery;++spellSerial;
    return true;
}
void Hero::stopJump() { jumping = false; jumpStep = 0; }
void Hero::stopDash() { dashing = shadeDashing = false; dashTime = 0; gravityEnabled = true; }
void Hero::stopAttack() { nail.active = nail.hitbox = false; nail.elapsed = 0; }
void Hero::startJump()
{
    ++groundJumpSerial;
    jumping = true; jumpedSteps = 0; jumpQueued = 0; doubleQueued = -1;
    ledgeSteps = 0; lookTime = 0; lookUp = lookDown = false;
}
void Hero::startDoubleJump()
{
    jumping = false; doubleJumping = doubleJumpUsed = true; doubleStep = 0;
    lookTime = 0; lookUp = lookDown = false;
}
void Hero::startWallJump()
{
    wallJumpDirection = contacts.left ? 1 : -1;
    facingRight = wallJumpDirection > 0;
    wallSliding = false; wallSide = 0;
    contacts.left = contacts.right = false;
    airDashUsed = doubleJumpUsed = false;
    wallSpeed = tuning::wallKick; wallLocked = true; wallLockStep = 0;
    jumping = true; jumpedSteps = 0; jumpQueued = 0;
}
void Hero::startDash()
{
    if (!contacts.ground) airDashUsed = true;
    stopAttack(); bouncing = mushroomBouncing = false; bounceTime = 0;
    if (wallSliding) facingRight = !facingRight;
    else if (input.x > 0) facingRight = true;
    else if (input.x < 0) facingRight = false;
    dashing = true; dashQueued = 0;
    downDash = input.y < 0 && input.x == 0 && !contacts.ground && charms.dashmaster;
    dashCooldown = charms.dashmaster ? tuning::dashmasterCooldown : tuning::dashCooldown;
    if (abilities.shadeDash && shadeCooldown <= 0)
    {
        shadeDashing = true; shadeCooldown = tuning::shadeCooldown;
    }
    if(contacts.ground&&!shadeDashing)++groundDashSerial;
    lookTime = 0; lookUp = lookDown = false;
}
void Hero::startAttack()
{
    nail.rangeScale = charms.markOfPride ? (charms.longnail ? 1.4f : 1.25f) : (charms.longnail ? 1.15f : 1.0f);
    nail.mantisEffect = charms.markOfPride;
    if (elapsed - nail.lastStart > tuning::alternateReset) nail.nextAlternate = false;
    nail.wall = wallSliding;
    if (wallSliding) nail.direction = facingRight ? Strike::Left : Strike::Right;
    else if (input.y > 0) nail.direction = Strike::Up;
    else if (input.y < 0 && actor != Actor::Idle && actor != Actor::Running) nail.direction = Strike::Down;
    else nail.direction = facingRight ? Strike::Right : Strike::Left;
    nail.alternate = false;
    if (!wallSliding && (nail.direction == Strike::Left || nail.direction == Strike::Right))
    {
        nail.alternate = nail.nextAlternate; nail.nextAlternate = !nail.nextAlternate;
    }
    nail.active = true; nail.hitbox = false; nail.elapsed = 0; nail.step = 0;
    nail.cooldown = charms.quickSlash ? tuning::quickAttackCooldown : tuning::attackCooldown;
    nail.lastStart = elapsed; ++nail.serial;
    lookTime = 0; lookUp = lookDown = false;
}

void Hero::update(const Input& command, float dt, const World& world)
{
    shellCooldown=std::max(0.f,shellCooldown-dt);
    dialogueCastDelay=std::max(0.f,dialogueCastDelay-dt);
    if(actor==Actor::Dead||actor==Actor::Damage)castTime=0;
    else castTime=std::max(0.f,castTime-dt);
    // Update's no_input / hard_landing branches do not call LookForInput,
    // including the update that restores the ordinary actor state.
    const bool readMovement = controlEnabled && actor != Actor::Damage && actor != Actor::HardLanding && actor != Actor::Dead;
    elapsed += dt;
    falling = velocity.y <= -0.000001f && !contacts.ground;
    if (falling)
    {
        if (wallSliding) fallTime = 0; else fallTime += dt;
        if (fallTime > tuning::bigFall) willHardLand = true;
    }
    else { fallTime = 0; willHardLand = false; }
    if (actor == Actor::HardLanding)
    {
        landingTime += dt;
        if (landingTime > tuning::hardLanding) { actor = Actor::Idle; landingTime = 0; }
    }
    if (actor == Actor::Damage && freezePhase == 0)
    {
        const float duration = charms.stalwart ? tuning::stalwartRecoilTime : tuning::damageRecoilTime;
        if (damageTime < duration) damageTime += dt;
        else
        {
            // CancelDamageRecoil calls ResetMotion before restoring gravity.
            // Keeping the recoil's upward velocity adds a second ballistic arc.
            velocity = {};
            actor = contacts.ground ? Actor::Idle : Actor::Airborne;
            gravityEnabled = true; damageTime = 0;
        }
    }
    // Queue buttons are sampled outside that branch; movement remains latched.
    const float moveX=input.x, moveY=input.y;
    input = inputEnabled?command:Input{};
    input.x = readMovement ? static_cast<float>(horizontal(input.x)) : moveX;
    input.y = readMovement ? static_cast<float>(vertical(input.y)) : moveY;
    if(updateSurfaceWater(dt)) {
        decay(nail.cooldown,dt);decay(dashCooldown,dt);decay(shadeCooldown,dt);return;
    }
    if(updateSuperDash(dt,world)) {
        decay(nail.cooldown,dt);decay(dashCooldown,dt);decay(shadeCooldown,dt);return;
    }
    if (readMovement && !locked(*this))
    {
        if (canWallSlide() && !nail.active && !wallSliding &&
            ((contacts.left && input.x < 0) || (contacts.right && input.x > 0)))
        {
            wallSide = contacts.left ? -1 : 1; wallSliding = true; facingRight = wallSide > 0;
            airDashUsed = doubleJumpUsed = false; willHardLand = false;
        }
        if (wallSliding && input.isPressed(Down)) { wallSliding = false; facingRight = !facingRight; }
        if (wallLocked && input.x * wallJumpDirection < 0 && wallLockStep >= tuning::wallLockShort) wallLocked = false;
        if (!input.isHeld(Jump))
        {
            if (velocity.y > 0 && jumpedSteps >= tuning::minimumJumpSteps && !mushroomBouncing)
            { velocity.y = 0; stopJump(); }
            jumpQueued = doubleQueued = -1;
        }
        if (!input.isHeld(Dash)) dashQueued = -1;
        if (!input.isHeld(Attack)) attackQueued = -1;
        if (nail.active && !dashing)
        {
            nail.elapsed += dt;
            if (nail.elapsed >= (charms.quickSlash ? tuning::quickAttackDuration : tuning::attackDuration)) stopAttack();
        }
        if (bouncing)
        {
            if (bounceTime < tuning::bounceTime) bounceTime += dt;
            else { bouncing = false; bounceTime = 0; velocity.y = 0; }
        }
        if (mushroomBouncing && velocity.y <= 0) mushroomBouncing = false;
    }
    // Jump -> dash -> attack is the observed queue priority. A wall jump is
    // checked before ordinary jump / double jump (including during recovery).
    if (controlEnabled && actor != Actor::Dead && freezePhase == 0)
    {
        if (input.isPressed(Spell))startCast();
        if (input.isPressed(Jump))
        {
            if (canWallJump()) startWallJump();
            else if (canJump()) startJump();
            else if (canDoubleJump()) startDoubleJump();
            else jumpQueued = doubleQueued = 0;
        }
        if (input.isPressed(Dash)) { if (canDash()) startDash(); else dashQueued = 0; }
        if (input.isPressed(Attack)) { if (canAttack()) startAttack(); else attackQueued = 0; }
        if (input.isHeld(Jump))
        {
            if (jumpQueued >= 0 && jumpQueued <= tuning::jumpQueue && canJump()) startJump();
            else if (doubleQueued >= 0 && doubleQueued <= tuning::doubleJumpQueue && canDoubleJump()) startDoubleJump();
        }
        if (input.isHeld(Dash) && dashQueued >= 0 && dashQueued <= tuning::dashQueue && canDash()) startDash();
        if (input.isHeld(Attack) && attackQueued >= 0 && attackQueued <= tuning::attackQueue && canAttack()) startAttack();
    }
    if (wallSliding)
    {
        airDashUsed = doubleJumpUsed = false;
        if (contacts.ground || (!contacts.left && !contacts.right))
        { facingRight = !facingRight; wallSliding = false; }
        if (!canWallSlide()) wallSliding = false;
    }
    if (nail.wall && !wallSliding) stopAttack();
    decay(nail.cooldown, dt); decay(dashCooldown, dt); decay(shadeCooldown, dt);
    if (actor == Actor::Idle && input.x == 0 && input.y != 0 && !locked(*this))
    {
        lookTime += dt; lookUp = input.y > 0 && lookTime >= 0.85f; lookDown = input.y < 0 && lookTime >= 0.85f;
    }
    else { lookTime = 0; lookUp = lookDown = false; }
    updateFocus(dt);
}

void Hero::fixedUpdate(float dt, const World& sourceWorld)
{
    auto world=sourceWorld;world.heroContacts=true;
    PresentationPosition::Step presentationStep(presentation,position);
    position.y+=platformDisplacement;platformDisplacement=0;
    if(surfaceSwim.active()){
        if(controlEnabled&&inputEnabled&&actor!=Actor::Damage&&actor!=Actor::Dead){fixedSurfaceWater(dt,world);return;}
        cancelSurfaceWater();
    }
    if(superDash.active()){fixedSuperDash(dt,world);return;}
    if (recoilDirection != 0)
    {
        if (recoilStep <= tuning::nailRecoilSteps) ++recoilStep;
        else { recoilDirection = 0; recoilStep = 0; }
    }
    if (actor == Actor::Dead) { velocity = {}; return; }
    if(castTime>0){velocity={};return;}
    if (actor == Actor::HardLanding) { velocity = {}; stopJump(); stopDash(); wallLocked = false; }
    else if (actor == Actor::Damage) velocity = freezePhase == 0 ? damageVelocity : Vec2{};
    else if (controlEnabled)
    {
        if (!dashing)
        {
            if (!wallSliding)
            {
                float speed = walkZone ? tuning::walk : tuning::run;
                if (!walkZone && charms.sprint && contacts.ground) speed = charms.dashmaster ? tuning::sprintDashmaster : tuning::sprint;
                velocity.x = input.x * speed;
            }
            if ((!nail.active || nail.elapsed >= tuning::attackRecovery) && !wallSliding && !wallLocked && input.x != 0)
            {
                if (facingRight != (input.x > 0)) { facingRight = input.x > 0; stopAttack(); }
            }
            if (recoilDirection != 0)
            {
                const float speed = largeRecoil ? tuning::longRecoil : tuning::nailRecoil;
                const float along = velocity.x * recoilDirection;
                velocity.x = recoilDirection * (along < speed ? speed : along + speed);
            }
        }
        if (jumping)
        {
            if (jumpStep <= tuning::jumpSteps) { velocity.y = tuning::jump; ++jumpStep; ++jumpedSteps; ledgeSteps = 0; }
            else stopJump();
        }
        if (doubleJumping)
        {
            if (doubleStep <= tuning::doubleJumpSteps) { if (doubleStep > 3) velocity.y = tuning::jump * 1.1f; ++doubleStep; }
            else { doubleJumping = false; doubleStep = 0; }
            if (contacts.ground) { doubleJumping = false; doubleStep = 0; }
        }
        if (dashing)
        {
            gravityEnabled = false; fallTime = 0; willHardLand = false;
            if (dashTime > tuning::dashDuration)
            {
                stopDash(); // Existing X velocity survives until next Move, as in the source.
                if (!contacts.ground && abilities.wallJump && (contacts.left || contacts.right))
                { wallSliding = true; wallSide = contacts.left ? -1 : 1; }
            }
            else
            {
                const float speed = shadeDashing && charms.sharpShadow ? tuning::sharpDash : tuning::dash;
                const float bump = !downDash && dashBump(*this, world) ?
                    (contacts.ground ? tuning::dashGroundBump : tuning::dashAirBump) : 0.f;
                velocity = downDash ? Vec2{0, -speed} : Vec2{facingRight ? speed : -speed, bump};
                dashTime += dt;
            }
        }
        if (bouncing) velocity.y = tuning::bounceSpeed;
        if (wallLocked)
        {
            velocity.x = wallJumpDirection * wallSpeed;
            if (++wallLockStep > tuning::wallLockLong) wallLocked = false;
            wallSpeed -= (tuning::wallKick - tuning::run) / tuning::wallLockLong;
        }
        if (wallSliding)
        {
            if (input.x * wallSide < 0) ++wallUnstickStep; else wallUnstickStep = 0;
            if (wallUnstickStep >= tuning::wallSticky) wallSliding = false;
        }
    }
    if (wallSliding)
    {
        if (velocity.y > tuning::wallSlide) velocity.y = std::max(tuning::wallSlide, velocity.y - tuning::wallDeceleration);
        if (velocity.y < tuning::wallSlide) velocity.y = std::max(tuning::wallSlide, velocity.y + tuning::wallDeceleration);
    }
    if (velocity.y < -tuning::maxFall && !shadeDashing && controlEnabled) velocity.y = -tuning::maxFall;
    tickQueue(jumpQueued, tuning::jumpQueue); tickQueue(doubleQueued, tuning::doubleJumpQueue);
    tickQueue(dashQueued, tuning::dashQueue); tickQueue(attackQueued, tuning::attackQueue);
    if (ledgeSteps > 0) --ledgeSteps;
    if (headSteps > 0) --headSteps;
    if (nail.active) { nail.hitbox = nail.step >= 1 && nail.step < 5; ++nail.step; }
    if (gravityEnabled) velocity.y += tuning::gravity * tuning::gravityScale * dt;
    const Contacts previous = contacts;
    contacts = moveBody(position, velocity, dt, world);
    if (actor == Actor::Damage || actor == Actor::HardLanding) return;
    // HeroController.OnCollisionStay2D uses three ground rays rather than
    // requiring a floor manifold. A wall contact can therefore land a falling
    // hero while its feet are still within the source 0.16 ray extension.
    if (falling || jumping)
    {
        bool stay=false,enteredFloor=false;
        for(std::size_t i=0;i<contacts.count;++i)
        {
            bool persisted=false;
            for(std::size_t j=0;j<previous.count;++j)
                if(contacts.surfaces[i].id==previous.surfaces[j].id)persisted=true;
            if(persisted && !(contacts.surfaces[i].flags&2))stay=true;
            if(!persisted && contacts.surfaces[i].normal.y>=.5f)enteredFloor=true;
        }
        if(stay)
        {
            const auto bounds=bodyBounds(position);
            const float center=(bounds.bottom+bounds.top)*.5f;
            bool ground=false;
            for(float x:{bounds.left,(bounds.left+bounds.right)*.5f,bounds.right})
            {
                float distance;
                if(rayTerrain({x,center},{0,-1},center-bounds.bottom+.16f,world,distance))
                {ground=true;break;}
            }
            if(!ground)contacts.ground=enteredFloor;
            else if(falling)contacts.ground=true;
        }
    }
    // Hero's top Enter cancels jumping before the Roof message below. On a
    // slanted ceiling the solver can leave upward tangent velocity: CancelJump
    // preserves it, and CancelHeroJump must see the already-cleared jump flag.
    bool topEntered=contacts.count==0 && contacts.roof && !previous.roof;
    for(std::size_t i=0;i<contacts.count;++i)
    {
        if(contacts.surfaces[i].normal.y>-.5f)continue;
        bool stayed=false;
        for(std::size_t j=0;j<previous.count;++j)
            if(contacts.surfaces[i].id==previous.surfaces[j].id)stayed=true;
        if(!stayed)topEntered=true;
    }
    if (topEntered)
    {
        headSteps = tuning::headBump;
        if (jumping) { stopJump(); doubleJumping = false; doubleStep = 0; }
        if (bouncing || mushroomBouncing) { bouncing = mushroomBouncing = false; bounceTime = 0; velocity.y = 0; }
    }
    // Roof.OnCollisionEnter2D sends CancelHeroJump independently of the
    // contact direction. Enter belongs to the collider, not its manifold normal.
    for(std::size_t i=0;i<contacts.count;++i)
    {
        if(!(contacts.surfaces[i].flags&16))continue;
        bool stay=false;
        for(std::size_t j=0;j<previous.count;++j)
            if(contacts.surfaces[i].id==previous.surfaces[j].id)stay=true;
        if(!stay && jumping)
        {
            stopJump();doubleJumping=false;doubleStep=0;
            if(velocity.y>0)velocity.y=0;
        }
    }
    if (contacts.ground)
    {
        if (!previous.ground)
        {
            if (nail.direction == Strike::Down) stopAttack();
            if (willHardLand && contacts.hardLanding)
            { actor = Actor::HardLanding; stopAttack(); landingTime = 0; }
            jumpStep = 0; airDashUsed = doubleJumpUsed = false;
            if (doubleJumping) startJump();
            fallTime = 0; willHardLand = false;
        }
        if (actor != Actor::HardLanding) actor = input.x == 0 ? Actor::Idle : Actor::Running;
    }
    else
    {
        actor = Actor::Airborne;
        // Only OnCollisionExit2D grants ledge grace. Losing ground through
        // Stay rays while the same collider persists does not grant a jump.
        bool exited=previous.count==0; // Legacy synthetic rectangle solver.
        for(std::size_t i=0;i<previous.count;++i)
        {
            bool persisted=false;
            for(std::size_t j=0;j<contacts.count;++j)
                if(previous.surfaces[i].id==contacts.surfaces[j].id)persisted=true;
            if(!persisted)exited=true;
        }
        if (previous.ground && exited) ledgeSteps = tuning::ledgeBuffer;
    }
}

void Hero::nailContact(bool bounceable, bool large, bool mushroom)
{
    if (!nail.hitbox || !bounceable) return;
    reactToNailContact(nail.direction, large, mushroom);
}

void Hero::reactToNailContact(Strike direction, bool large, bool mushroom)
{
    if (direction == Strike::Down)
    {
        // ShroomBounce restarts even during another bounce or relinquished
        // control. BounceHigh only excludes an ordinary bounce; Bounce also
        // excludes a mushroom launch. These source guards are not equivalent.
        if (mushroom)
        {
            doubleJumpUsed = airDashUsed = false;
            bouncing = false; mushroomBouncing = true; velocity.y = tuning::mushroomSpeed;
        }
        else if (controlEnabled && !bouncing && (large || !mushroomBouncing))
        {
            doubleJumpUsed = airDashUsed = false; bouncing = true;
            if (large) { bounceTime = -0.03f; velocity.y = tuning::bounceSpeed; }
        }
    }
    else if (direction == Strike::Up)
    {
        stopJump();
        if (controlEnabled && velocity.y > tuning::nailRecoilDown) velocity.y = tuning::nailRecoilDown;
    }
    else if (controlEnabled && recoilDirection == 0 && (!charms.steadyBody || large))
    {
        stopDash(); recoilStep = 0; largeRecoil = large;
        recoilDirection = direction == Strike::Right ? -1 : 1;
        velocity.x = recoilDirection * (large ? tuning::longRecoil : tuning::nailRecoil);
        if (large) stopAttack();
    }
}

void Hero::checkTerrainThunk(float dt,const World& world)
{
    // DoAttack starts an independent 0.12 s coroutine, before the slash
    // collider opens. Cancellation of the slash does not cancel this query.
    if(terrainNailSerial!=nail.serial)
    {terrainNailSerial=nail.serial;terrainNailTime=0.12f;terrainNailDirection=nail.direction;}
    if(terrainNailTime<=0)return;
    const auto bounds=bodyBounds(position);
    Vec2 origin{(bounds.left+bounds.right)*0.5f,(bounds.bottom+bounds.top)*0.5f+0.25f};
    Vec2 direction{(facingRight!=wallSliding)?1.0f:-1.0f,0};
    float length=2;
    if(terrainNailDirection==Strike::Up||terrainNailDirection==Strike::Down)
    {
        const bool up=terrainNailDirection==Strike::Up;
        origin.y=up?bounds.top:bounds.bottom;direction={0,up?1.0f:-1.0f};length=1.5f;
    }
    length*=1+(charms.longnail?0.2f:0)+(charms.markOfPride?0.3f:0);
    float distance;unsigned flags;
    if(castTerrainBox(origin,{0.225f,0.225f},direction,length,world,distance,flags)&&!(flags&9))
    {
        terrainNailTime=0;
        // The source's terrain callback is independent of NailSlash.hitbox.
        if(terrainNailDirection!=Strike::Down)
            reactToNailContact(terrainNailDirection==Strike::Up?Strike::Up:(facingRight?Strike::Right:Strike::Left));
    }
    else terrainNailTime-=dt;
}

bool Hero::takeDamage(int amount, int sourceSide)
{
    if(testInvulnerable||transitionInvincible)return false;
    if (amount <= 0 || invulnerableTime > 0 || actor == Actor::Damage || actor == Actor::Dead || shadeDashing) return false;
    if(charms.baldurShell&&shellHits&&shellCooldown<=0&&
       (focus==FocusPhase::Starting||focus==FocusPhase::Draining))
    {--shellHits;shellCooldown=.5f;focus=FocusPhase::Cancelling;focusTime=0;return false;}
    const bool red=vitals.blueHealth==0;
    vitals.takeHealth(charms.overcharmed ? amount * 2 : amount);
    if(charms.grubsong){vitals.soul=std::min(vitals.maxSoul,vitals.soul+15);++vitals.soulGainSerial;}
    if(charms.thorns&&vitals.health>0)++thornsSerial;
    ++lightDamageSerial;
    castTime=0;cancelSurfaceWater();cancelSuperDash();
    stopAttack(); stopJump(); stopDash(); doubleJumping = wallSliding = wallLocked = bouncing = mushroomBouncing = false;
    recoilDirection = 0; velocity = {}; focus = FocusPhase::None; controlEnabled = true;
    if (vitals.health == 0)
    { actor = Actor::Dead; velocity = {}; gravityEnabled = false; return true; }
    actor = Actor::Damage; damageTime = 0;
    if(red)++redDamageSerial;
    damageVelocity = sourceSide == 0 ? Vec2{} : Vec2{-sourceSide * tuning::damageRecoil, tuning::damageRecoil * 0.5f};
    if (sourceSide != 0) facingRight = sourceSide > 0;
    gravityEnabled = false; freezePhase = 1; freezeTime = 0;
    startInvulnerability();
    return true;
}
void Hero::startInvulnerability()
{
    invulnerableTime=charms.stalwart?tuning::stalwartInvulnerability:tuning::invulnerability;
    invulnerableDelay=tuning::freezeDown;
    invulnerabilityStarted=true;
}
void Hero::resumeCoroutines(float scaled)
{
    // Each source wait yields once before decrementing; overshoot in the first
    // wait is discarded when entering the second wait in that continuation.
    if(invulnerabilityStarted){invulnerabilityStarted=false;return;}
    if(invulnerableDelay>0)invulnerableDelay-=scaled;
    else if(invulnerableTime>0)invulnerableTime-=scaled;
}
void Hero::updateUnscaled(float dt)
{
    if (freezePhase == 0) { scale = 1; return; }
    freezeTime += dt;
    if (freezePhase == 1)
    {
        scale = 1 + (tuning::freezeScale - 1) * std::min(1.0f, freezeTime / tuning::freezeDown);
        if (freezeTime >= tuning::freezeDown) { freezePhase = 2; freezeTime = 0; }
    }
    else if (freezePhase == 2)
    {
        scale = 0; // GameManager.SetTimeScale clamps values <= 0.01 to zero.
        if (freezeTime >= tuning::freezeWait) { freezePhase = 3; freezeTime = 0; }
    }
    else
    {
        scale = std::min(1.0f, freezeTime / tuning::freezeUp);
        if (freezeTime >= tuning::freezeUp) { scale = 1; freezePhase = 0; freezeTime = 0; }
    }
    if (scale <= 0.01f) scale = 0;
}
}
