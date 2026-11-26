#include "animation.hpp"
#include "actor_facing_data.hpp"
#include "combat.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hk3ds
{
namespace {
template<class T> bool readItems(std::FILE* file, T* data, std::size_t count)
{ return std::fread(data, sizeof(T), count, file) == count; }
std::uint32_t missingPlayTotal=0;
bool readEvents(std::FILE* file, std::size_t frames, AnimationEventTable& events)
{
    static_assert(sizeof(AnimationEvent)==76,"Event layout changed");
    char magic[4];std::uint32_t count=0;
    if(!readItems(file,magic,4)||std::memcmp(magic,"AEV1",4)||!readItems(file,&count,1)||count>frames)return false;
    std::vector<AnimationEvent> next(count);
    if(!readItems(file,next.data(),next.size())||std::fgetc(file)!=EOF)return false;
    for(std::size_t i=0;i<next.size();++i)
        if(next[i].frame>=frames||(i&&next[i].frame<=next[i-1].frame)||
           !std::memchr(next[i].info,0,sizeof(next[i].info))||!std::isfinite(next[i].value))return false;
    events.replace(std::move(next));return true;
}
}
AnimationEventTable::AnimationEventTable(const AnimationEventTable& source)
{ replace(source.entries()); }
AnimationEventTable& AnimationEventTable::operator=(const AnimationEventTable& source)
{
    if(this!=&source)replace(source.entries());
    return *this;
}
const std::vector<AnimationEvent>& AnimationEventTable::entries() const
{
    static const std::vector<AnimationEvent> empty;
    return entries_ ? *entries_ : empty;
}
void AnimationEventTable::replace(std::vector<AnimationEvent> next)
{
    if(next.empty())entries_.reset();
    else entries_=std::make_unique<std::vector<AnimationEvent>>(std::move(next));
}
std::uint32_t animationMissingPlayCount(){return missingPlayTotal;}
float animationFacingScale(const char* kind, float direction, float magnitude)
{
    if (kind) {
        const auto* end = actorfacingdata::entries + sizeof(actorfacingdata::entries)/sizeof(actorfacingdata::entries[0]);
        const auto* actor = std::lower_bound(actorfacingdata::entries, end, kind,
            [](const auto& entry, const char* key){return std::strcmp(entry.kind, key) < 0;});
        if (actor != end && std::strcmp(actor->kind, kind) == 0)
            return animationFacingScale(actor->rightSign, direction, magnitude);
    }
    assert(false && "Missing converted actor facing convention");
    return 0;
}
float animationFacingScale(int rightSign, float direction, float magnitude)
{
    assert((rightSign == -1 || rightSign == 1) && std::isfinite(direction) && direction != 0 &&
           std::isfinite(magnitude) && magnitude > 0);
    if ((rightSign != -1 && rightSign != 1) || !std::isfinite(direction) || direction == 0 ||
        !std::isfinite(magnitude) || magnitude <= 0) return 0;
    return (direction < 0 ? -rightSign : rightSign) * magnitude;
}
bool AnimationFacingConfig::load(const char* path)
{
    static_assert(sizeof(ActorFacing) == 68, "Facing record layout changed");
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    char magic[4]; std::uint32_t count = 0;
    bool ok = readItems(file, magic, 4) && std::memcmp(magic, "AFC1", 4) == 0 &&
              readItems(file, &count, 1) && count > 0 && count <= 512;
    std::vector<ActorFacing> next;
    if (ok) {
        next.resize(count);
        ok = readItems(file, next.data(), count) && std::fgetc(file) == EOF;
    }
    std::fclose(file);
    if (!ok) return false;
    for (std::size_t i = 0; i < next.size(); ++i) {
        const auto& actor = next[i];
        if (!actor.kind[0] || !std::memchr(actor.kind, 0, sizeof(actor.kind)) ||
            (actor.rightSign != -1 && actor.rightSign != 1)) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (std::strcmp(actor.kind, next[j].kind) == 0) return false;
    }
    actors_ = std::move(next);
    return true;
}
float AnimationFacingConfig::scale(const char* kind, float direction, float magnitude) const
{
    if (kind)
        for (const auto& actor : actors_)
            if (std::strcmp(actor.kind, kind) == 0)
                return animationFacingScale(actor.rightSign, direction, magnitude);
    return 0; // An absent kind has no inferred convention.
}
bool AnimationBank::load(const char* path)
{
    static_assert(offsetof(SpriteFrame, clut) == 36 && sizeof(Clip) == 68, "Pack layout changed");
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    char magic[4]; std::uint32_t header[4]{};
    bool ok = readItems(file, magic, 4) && std::memcmp(magic, "HKA1", 4) == 0 && readItems(file, header, 4);
    // A bounded loader: malformed local files must not cause huge allocations.
    ok = ok && header[0] > 0 && header[0] <= 32 && header[1] > 0 && header[1] <= 8192 &&
        header[2] > 0 && header[2] <= 512 && header[3] > 0 && header[3] <= 65536;
    AnimationBank next;
    if (ok)
    {
        next.pageCount = header[0]; next.sprites.resize(header[1]); next.clips.resize(header[2]); next.frames.resize(header[3]);
        // HKA1 stores the original 36-byte prefix; platform CLUT IDs stay zero.
        for (auto& sprite : next.sprites)
            if (std::fread(&sprite, 36, 1, file) != 1) { ok = false; break; }
        ok = ok && readItems(file, next.clips.data(), next.clips.size()) &&
            readItems(file, next.frames.data(), next.frames.size());
        if(ok){const int tail=std::fgetc(file);if(tail!=EOF){std::ungetc(tail,file);ok=readEvents(file,next.frames.size(),next.events);}}
    }
    std::fclose(file);
    if (!ok) return false;
    for (const auto& s : next.sprites)
        if (s.page >= next.pageCount || s.width == 0 || s.height == 0 || s.width > 1024 || s.height > 1024 ||
            s.x > 1024 - s.width || s.y > 1024 - s.height || !std::isfinite(s.left) || !std::isfinite(s.bottom) ||
            !std::isfinite(s.right) || !std::isfinite(s.top) || s.right <= s.left || s.top <= s.bottom) return false;
    for (const auto& c : next.clips)
        if (!std::memchr(c.name, 0, sizeof(c.name)) || !std::isfinite(c.fps) || c.fps <= 0 || c.count == 0 ||
            c.first > next.frames.size() || c.count > next.frames.size() - c.first || (c.wrap==1&&c.loopStart >= c.count) ||
            !(c.wrap <= 3 || c.wrap == 6)) return false;
    for (auto index : next.frames) if (index >= next.sprites.size()) return false;
    *this = std::move(next);
    return true;
}
const Clip* AnimationBank::find(const char* name) const
{
    if (!name) return nullptr;
    for (const auto& c : clips) if (std::strcmp(c.name, name) == 0) return &c;
    return nullptr;
}
const Clip* AnimationBank::require(const char* name) const
{
    const auto* result = find(name);
    if (result) return result;
    ++missingPlayCount;
    ++missingPlayTotal;
    const char* label = name ? name : "<null>";
    auto found = std::find_if(missingPlays.begin(), missingPlays.end(),
                             [label](const AnimationMiss& miss) { return miss.name == label; });
    if (found == missingPlays.end()) {
        missingPlays.push_back({label, 1});
        std::fprintf(stderr, "animation bank missing play: %s\n", label);
    } else ++found->count;
#if !defined(__3DS__) && !defined(__PSP__) && !defined(__psp__) && !defined(HK3DS_ANIMATION_MISS_COUNTER_ONLY)
    assert(result && "animation bank missing required play (clip named above)");
#endif
    return nullptr;
}
void AnimationPlayer::play(const AnimationBank& bank, const char* name, std::uint32_t start, bool restart)
{
    play(bank.require(name), start, restart);
}
void AnimationPlayer::play(const Clip* next, std::uint32_t start, bool restart)
{
    if (!next || (clip == next && !restart)) return;
    clip = next; frame = std::min(start, clip->count - 1); clock = static_cast<float>(frame); complete = false;initialEvent=true;
}
void AnimationPlayer::advance(float seconds,const AnimationBank& bank,EventCallback callback,void* context)
{
    if(!callback){advance(seconds);return;}
    if (!clip || complete || seconds <= 0 || !std::isfinite(seconds)) return;
    {
        const auto* active=clip;
        const auto& entries=bank.events.entries();
        const auto changed=[&](){return clip!=active||initialEvent;};
        const auto dispatch=[&](){
            const auto index=active->first+frame;
            auto event=std::lower_bound(entries.begin(),entries.end(),index,
                [](const AnimationEvent& value,std::uint32_t key){return value.frame<key;});
            if(event!=entries.end()&&event->frame==index)callback(context,*active,frame,*event);
        };
        if(initialEvent){initialEvent=false;dispatch();if(changed())return;}
        if(active->wrap==6){frame=0;clock=0;complete=true;return;}
        // Visit every crossed integer clock boundary, including repeated loop frames.
        double remaining=double(seconds)*active->fps;
        while(remaining>0&&!complete){
            const double until=std::floor(double(clock))+1-double(clock);
            const bool crossed=remaining>=until;
            const double step=crossed?until:remaining;
            clock=static_cast<float>(double(clock)+step);remaining-=step;
            if(crossed){
                if(active->wrap==0&&clock>=active->count)clock=0;
                else if(active->wrap==1&&clock>=active->count)clock=static_cast<float>(active->loopStart);
                else if(active->wrap==2&&clock>=active->count){clock=static_cast<float>(active->count-1);complete=true;break;}
            }
            if(active->wrap==3&&active->count>1){
                const auto span=(active->count-1)*2;
                clock=std::fmod(clock,static_cast<float>(span));const auto at=static_cast<std::uint32_t>(clock);
                frame=at<active->count?at:span-at;
            }else frame=std::min(static_cast<std::uint32_t>(clock),active->count-1);
            if(crossed){dispatch();if(changed())return;}
        }
        return;
    }
}
void AnimationPlayer::advance(float seconds)
{
    if (!clip || complete || seconds <= 0 || !std::isfinite(seconds)) return;
    initialEvent=false;
    if (clip->wrap == 6) { frame = 0; clock = 0; complete = true; return; }
    clock += seconds * clip->fps;
    const auto count = clip->count;
    if (clock >= count)
    {
        if (clip->wrap == 0) clock = std::fmod(clock, static_cast<float>(count));
        else if (clip->wrap == 1) clock = clip->loopStart + std::fmod(clock - clip->loopStart, static_cast<float>(count - clip->loopStart));
        else if (clip->wrap == 2 || clip->wrap == 6) { clock = static_cast<float>(count - 1); complete = true; }
    }
    if (clip->wrap == 3 && count > 1)
    {
        const auto span = (count - 1) * 2;
#if HK3DS_PARTICLE_ANIMATION_FAST
        if (!(clock >= 0 && clock < static_cast<float>(span)))
#endif
            clock = std::fmod(clock, static_cast<float>(span));
        const auto index = static_cast<std::uint32_t>(clock);
        frame = index < count ? index : span - index;
    }
    else frame = std::min(static_cast<std::uint32_t>(clock), count - 1);
}
const SpriteFrame* AnimationPlayer::current(const AnimationBank& bank) const
{
    if (!clip || clip->first + frame >= bank.frames.size()) return nullptr;
    const auto index = bank.frames[clip->first + frame];
    return index < bank.sprites.size() ? &bank.sprites[index] : nullptr;
}
void AnimationTurn::begin(AnimationPlayer& player,const AnimationBank& bank,const char* turn,
                          const char* walk,float nextScale)
{
    resume=bank.require(walk);targetScale=nextScale;active=true;
    player.play(bank,turn,0,true);
}
bool AnimationTurn::finish(AnimationPlayer& player,float& scale)
{
    if(!active||!player.complete)return false;
    scale=targetScale;player.play(resume,0,true);active=false;return true;
}
void HeroAnimator::update(const Hero& h, const AnimationBank& bank, float dt, const NailBank* nails)
{
    if(overrideClip&&h.actor!=Actor::Damage&&h.actor!=Actor::Dead){
        slash={};player.play(overrideClip);player.advance(dt);return;
    }
    if (!h.nail.active) slash={};
    else if (lastAttack!=h.nail.serial && nails && nails->ready())
    {
        slashVariant=nailVariant(h.nail);
        char clip[64];
        std::snprintf(clip,sizeof(clip),"%s%s",nails->shapes[slashVariant].clip,h.nail.mantisEffect ? " M" : "");
        slash.play(bank.find(clip),0,true);
    }
    slash.advance(dt);
    const char* name = nullptr;
    std::uint32_t start = 0;
    bool restart = false;
    if (h.actor == Actor::Damage) name = h.freezePhase ? "Stun" : "Recoil";
    else if (h.surfaceSwim.active()) name = h.surfaceWaterClip();
    // Spell Control Fireball 1/2 plays its source clip through the recoil state.
    else if (h.castTime>0) { name = h.abilities.shadeSoul ? "Fireball2 Cast" : "Fireball1 Cast"; restart = h.spellSerial != lastSpell; }
    else if (h.focus == FocusPhase::Starting || h.focus == FocusPhase::Draining)
        name = h.refocusing ? "Focus Get" : "Focus";
    else if (h.focus == FocusPhase::Healing) { name = "Focus Get"; restart = previousFocus != FocusPhase::Healing; }
    else if (h.focus == FocusPhase::Finishing) name = "Focus Get Once";
    else if (h.focus == FocusPhase::Cancelling) name = "Focus End";
    else if (h.dashing)
    {
        if (h.shadeDashing) name = h.downDash ? (h.charms.sharpShadow ? "Shadow Dash Down Sharp" : "Shadow Dash Down") :
            (h.charms.sharpShadow ? "Shadow Dash Sharp" : "Shadow Dash");
        else name = h.downDash ? "Dash Down" : "Dash";
    }
    else if (h.nail.active)
    {
        if (h.nail.wall) name = "Wall Slash";
        else if (h.nail.direction == Strike::Up) name = "UpSlash";
        else if (h.nail.direction == Strike::Down) name = "DownSlash";
        // The source animator reads the flag AFTER the controller toggles it;
        // the first normal hit therefore plays SlashAlt on the hero sprite.
        else name = h.nail.nextAlternate ? "SlashAlt" : "Slash";
        restart = lastAttack != h.nail.serial;
    }
    else if (h.wallSliding) name = "Wall Slide";
    else if (h.actor == Actor::HardLanding) name = "HardLand";
    else if (h.actor == Actor::Airborne)
    {
        if (h.wallLocked) name = "Walljump";
        else if (h.doubleJumping) name = "Double Jump";
        else { name = "Airborne"; start = h.jumping ? 0 : (h.falling ? 5 : 3); }
    }
    else if (h.actor != Actor::Dead)
    {
        if (previousActor == Actor::Airborne && h.actor == Actor::Idle) name = "Land";
        else if (previousActor == Actor::Running && h.actor == Actor::Idle && !h.walkZone) name = "Run To Idle";
        else if (previousDash && h.actor == Actor::Idle) name = "Dash To Idle";
        else if (previousFacing != h.facingRight) name = "Turn";
        else
        {
            bool transition = player.clip && !player.complete &&
                (std::strcmp(player.clip->name,"Land") == 0 || std::strcmp(player.clip->name,"Run To Idle") == 0 ||
                 std::strcmp(player.clip->name,"Dash To Idle") == 0 || std::strcmp(player.clip->name,"Turn") == 0);
            if (!transition)
            {
                if (h.actor == Actor::Running) name = h.walkZone ? "Walk" : (h.charms.sprint ? "Sprint" : "Run");
                else if (h.lookTime >= 0.25f && h.input.y != 0) name = h.input.y > 0 ? "LookUp" : "LookDown";
                else name = h.vitals.health == 1 && h.vitals.blueHealth == 0 ? "Idle Hurt" : "Idle";
            }
        }
    }
    if (name) player.play(bank.find(name), start, restart);
    player.advance(dt);
    lastAttack = h.nail.serial; lastSpell = h.spellSerial; previousActor = h.actor; previousFocus = h.focus;
    previousDash = h.dashing; previousFacing = h.facingRight;
}
}
