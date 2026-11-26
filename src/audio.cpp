#include "audio.hpp"
#include "initial_fall.hpp"
#include "room_entities.hpp"
#include "great_door.hpp"
#include "shade.hpp"
#include "conditional_events.hpp"
#include "room_ambient.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <utility>
#include <algorithm>
namespace hk3ds
{
namespace
{
std::uint32_t mix(std::uint32_t value)
{
    value += 0x9e3779b9u;
    value = (value ^ (value >> 16)) * 0x85ebca6bu;
    value = (value ^ (value >> 13)) * 0xc2b2ae35u;
    return value ^ (value >> 16);
}

float uniformPitch(std::uint32_t random, float low, float high)
{
    const float unit = static_cast<float>(random) / static_cast<float>(std::numeric_limits<std::uint32_t>::max());
    return low + (high - low) * unit;
}
}

AudioStereoMix sourceSpatialMix(const AudioSourceRolloff& source,float dx,float dy,float dz)
{
    if(!std::isfinite(dx)||!std::isfinite(dy)||!std::isfinite(dz)||
       !std::isfinite(source.volume)||source.volume<0||!std::isfinite(source.minimum)||
       !std::isfinite(source.maximum)||source.minimum<0||source.maximum<=source.minimum)return {0,0};
    const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
    float attenuation=0;
    if(source.custom){
        // Unity AudioSourceInspector.GetAxisScalars: curve time * maxDistance.
        // Clamp outside serialized keys; unweighted tangents use cubic Hermite.
        const auto& a=source.first;const auto& b=source.last;
        if(!std::isfinite(a.time)||!std::isfinite(b.time)||b.time<=a.time||
           !std::isfinite(a.value)||!std::isfinite(b.value)||
           !std::isfinite(a.tangent)||!std::isfinite(b.tangent))return {0,0};
        const float span=b.time-a.time;
        const float u=std::clamp((distance/source.maximum-a.time)/span,0.f,1.f);
        const float u2=u*u,u3=u2*u;
        attenuation=source.volume*std::max(0.f,(2*u3-3*u2+1)*a.value+
            (u3-2*u2+u)*span*a.tangent+(-2*u3+3*u2)*b.value+(u3-u2)*span*b.tangent);
    }else attenuation=source.volume*std::clamp((source.maximum-distance)/(source.maximum-source.minimum),0.f,1.f);
    // Approximation: equal-power stereo with horizontal direction cosine.
    // Unity's native speaker panner has not been measured. Preserve total power.
    const float direction=distance>0?std::clamp(dx/distance,-1.f,1.f):0;
    const float left=attenuation*std::sqrt((1-direction)*.5f);
    const float right=attenuation*std::sqrt((1+direction)*.5f);
    return {(left+right)*.5f,left+right>0?(right-left)/(right+left):0};
}

AudioStereoMix linearSpatialMix(float dx,float dy,float dz,float minimum,float maximum,float volume)
{
    return sourceSpatialMix({minimum,maximum,volume,false,{},{}},dx,dy,dz);
}

void EnemySourceDepth::activate(float initialDepth,float minimumDepth,double scaledSeconds,std::uint32_t random)
{
    initial_=initialDepth;
    // SetZ.OnEnable: Random.Range(z,z+.000999900046736002), selected once.
    // Use the existing audio-only hash so adding SetZ does not consume gameplay RNG.
    // Like EnemyRandom, this does not reproduce Unity's global random sequence.
    target_=uniformPitch(mix(random),minimumDepth,minimumDepth+.000999900046736002f);
    setAt_=scaledSeconds+.5;
}

AudioStereoMix enemyLoopMix(EnemyClip clip,float dx,float dy,float sourceDepth,EnemyLoopProfile profile)
{
    if(clip==EnemyClip::Hatchling||clip==EnemyClip::Hatcher)return linearSpatialMix(dx,dy,sourceDepth+38.1f,39,50,1);
    if(clip==EnemyClip::HuskFootstep)
        return sourceSpatialMix({1,500,1,true,
            {.0020000000949949026f,1,-1.0020040273666382f},
            {.1032201424241066f,0,-21.77566146850586f}},dx,dy,sourceDepth+38.1f);
    if(clip==EnemyClip::Tiktik)
        return sourceSpatialMix({39,50,.2770000100135803f,true,
            {.7799999713897705f,1,-5.535714149475098f},
            {.9921568632125854f,0,-2.655785083770752f}},dx,dy,sourceDepth+38.1f);
    if(clip==EnemyClip::Gruzzer)
        return sourceSpatialMix({39,50,.800000011920929f,true,
            {.7799999713897705f,1,-3.8928449153900146f},
            {.9450980424880981f,0,-13.040788650512695f}},dx,dy,sourceDepth+38.1f);
    if(clip!=EnemyClip::Crawler&&clip!=EnemyClip::Buzzer)return {};
    // Tutorial_01 level6:8379/8407/8433/8445 (custom), 8401/8436/8444 (linear).
    // Spatial blend1, spread0, doppler0. Camera listener world Z=-38.1.
    if(clip==EnemyClip::Buzzer)return linearSpatialMix(dx,dy,sourceDepth+38.1f,39,50,.8f);
    // Crossroads_01 level37:3210 has a different serialized first curve key.
    if(profile==EnemyLoopProfile::CrawlerCrossroads01)
        return sourceSpatialMix({39,50,1,true,
            {.806819498538971f,.0729604959487915f,-.4796079397201538f},
            {1,0,-.4697364270687103f}},dx,dy,sourceDepth+38.1f);
    return sourceSpatialMix({39,50,1,true,{.80985904f,.48881418f,-.47960794f},{1,0,-.46973643f}},
        dx,dy,sourceDepth+38.1f);
}

namespace {
AudioStereoMix customSpatialMix(float dx,float dy,float dz,float maximum,
    float first,float value,float tangent,float last,float endTangent,float volume)
{
    return sourceSpatialMix({0,maximum,volume,true,{first,value,tangent},{last,0,endTangent}},dx,dy,dz);
}
}
AudioStereoMix tabletHumMix(float dx,float dy,float dz,float volume)
{
    // AudioSources level6:8394/8410/8454; custom keys/tangents, not a linear fade.
    return customSpatialMix(dx,dy,dz,56.3f,.6927176f,1,-5.152806f,1,-1.62853f,volume);
}
AudioStereoMix shinyLoopMix(float dx,float dy,float dz)
{
    // AudioSource level6:8457, max distance 50.
    return customSpatialMix(dx,dy,dz,50,.78f,1,-4.545454f,.9411765f,-9.596051f,1);
}
AudioStereoMix shadeLoopMix(float dx,float dy)
{
    // AudioSource resources.assets:13904. Approximation: delayed SetZ .006..007 -> .006.
    return customSpatialMix(dx,dy,38.106f,47.71f,.8152941f,.50210524f,-6.7294025f,.94509804f,-2.6705122f,1);
}

bool PcmSound::load(const char* path)
{
    auto* f=std::fopen(path,"rb");if(!f)return false;
    char riff[4],wave[4];std::uint32_t bytes=0;
    bool ok=std::fread(riff,1,4,f)==4&&!std::memcmp(riff,"RIFF",4)&&std::fread(&bytes,4,1,f)==1&&
            std::fread(wave,1,4,f)==4&&!std::memcmp(wave,"WAVE",4)&&bytes>=36&&bytes<4000000;
    PcmSound next;bool format=false,data=false;unsigned consumed=4;
    while(ok&&consumed<bytes)
    {
        char kind[4];std::uint32_t size=0;
        ok=std::fread(kind,1,4,f)==4&&std::fread(&size,4,1,f)==1&&bytes-consumed>=8;
        consumed+=8;
        if(!ok||size>bytes-consumed){ok=false;break;}
        if(!std::memcmp(kind,"fmt ",4))
        {
            std::uint16_t encoding=0,channels=0,block=0,bits=0;std::uint32_t rate=0,speed=0;
            ok=!format&&size>=16&&std::fread(&encoding,2,1,f)==1&&std::fread(&channels,2,1,f)==1&&
               std::fread(&rate,4,1,f)==1&&std::fread(&speed,4,1,f)==1&&std::fread(&block,2,1,f)==1&&std::fread(&bits,2,1,f)==1;
            ok &= encoding==1&&(channels==1||channels==2)&&bits==16&&rate>=8000&&rate<=96000&&block==channels*2&&speed==rate*block;
            next.channels=channels;next.rate=rate;format=ok;
            if(size>16)ok &= std::fseek(f,size-16,SEEK_CUR)==0;
        }
        else if(!std::memcmp(kind,"data",4))
        {
            ok=!data&&format&&size&&size%(next.channels*2)==0;
            if(ok){next.samples.resize(size/2);ok=std::fread(next.samples.data(),1,size,f)==size;data=ok;}
        }
        else ok=std::fseek(f,size,SEEK_CUR)==0;
        if(size&1)ok &= std::fgetc(f)!=EOF;
        consumed+=size+(size&1);
    }
    ok &= consumed==bytes&&format&&data&&std::fgetc(f)==EOF;std::fclose(f);
    if(!ok)return false;
    *this=std::move(next);return true;
}

bool AudioVoices::start(EnemyClip clip, std::uint32_t owner, std::uint32_t random, bool loop, float pitchOverride,
                        std::uint32_t sourceId,float customGain)
{
    const auto index = clipIndex(clip);
    if (index >= counters_.size() && !sourceId) return false;
    auto* stats = sourceId ? nullptr : &counters_[index];

    std::size_t slot = VoiceCount;
    if (loop)
    {
        const auto first = clip == EnemyClip::TabletHum ? TabletHumVoice : 0;
        const auto end = clip == EnemyClip::TabletHum ? LoopVoiceCount : EnemyLoopVoiceCount;
        for (std::size_t i = first; i < end; ++i)
        {
            if (voices_[i].active && voices_[i].loop && voices_[i].owner == owner)
            {
                if (voices_[i].clip == clip) return true;
                slot = i;
                break;
            }
            if (slot == VoiceCount && !voices_[i].active) slot = i;
        }
    }
    else
    {
        for (std::size_t i = LoopVoiceCount; i < VoiceCount; ++i)
            if (!voices_[i].active) { slot = i; break; }
        if (slot == VoiceCount)
        {
            slot = LoopVoiceCount;
            for (std::size_t i = LoopVoiceCount + 1; i < VoiceCount; ++i)
                if (static_cast<std::int32_t>(order_[i] - order_[slot]) < 0) slot = i;
            if(!voices_[slot].sourceId)++counters_[clipIndex(voices_[slot].clip)].stolen;
        }
    }

    if(stats)++stats->dispatched;
    if (slot == VoiceCount)
    {
        if(stats)++stats->dropped;
        return false;
    }

    float gain = 1.0f;
    float pitch = 1.0f;
    switch (clip)
    {
    case EnemyClip::HeroDash: case EnemyClip::TownLeverHit: gain=centeredAudioGain(1);break;
    case EnemyClip::GrimmPickup: gain=.75f;break;
    case EnemyClip::Buzzer: gain = 0.8f; pitch = 1.0f; break;
    case EnemyClip::EnemyDamage:
    case EnemyClip::EnemyDeathSword: pitch = uniformPitch(random, 0.75f, 1.25f); break;
    case EnemyClip::Startle01:
    case EnemyClip::Startle02:
    case EnemyClip::Startle03: pitch = uniformPitch(random, 1.2f, 1.4f); break;
    case EnemyClip::GreatDoorHit1:
    case EnemyClip::GreatDoorHit2: case EnemyClip::NailTink: pitch = uniformPitch(random, 0.85f, 1.15f); break;
    case EnemyClip::ShadeSword: gain=1.15f;pitch=uniformPitch(random,.85f,1.0f);break;
    case EnemyClip::ScuttlerBounce: gain=.5f;pitch=uniformPitch(random,.9f,1.1f);break;
    default: break;
    }
    if (std::isfinite(pitchOverride) && pitchOverride > 0.0f) pitch=pitchOverride;
    auto& voice = voices_[slot];
    voice.active = true;
    voice.loop = loop;
    voice.clip = clip;
    voice.gain = sourceId ? customGain : gain;
    voice.sourceId = sourceId;
    voice.pitch = pitch;
    voice.owner = owner;
    ++nextGeneration_;
    if (nextGeneration_ == 0) ++nextGeneration_;
    voice.generation = nextGeneration_;
    voice.pan = 0;
    order_[slot] = ++nextOrder_;
    if(stats)++stats->started;
    return true;
}

void AudioVoices::dispatch(EnemySoundEvent event, std::uint32_t owner, std::uint32_t random)
{
    switch (event)
    {
    case EnemySoundEvent::Injury:
        start(EnemyClip::EnemyDamage, owner, random, false);
        break;
    case EnemySoundEvent::Kill:
        start(EnemyClip::EnemyDamage, owner, mix(random), false);
        start(EnemyClip::EnemyDeathSword, owner, mix(random ^ 0xa511e9b3u), false);
        break;
    case EnemySoundEvent::Startle:
    {
        const auto variant = (static_cast<std::uint64_t>(mix(random)) * 3u) >> 32;
        start(static_cast<EnemyClip>(static_cast<unsigned>(EnemyClip::Startle01) + variant), owner, mix(random ^ 0x63d83595u), false);
        break;
    }
    case EnemySoundEvent::CorpseSplat:
    {
        const auto variant = (static_cast<std::uint64_t>(mix(random)) * 3u) >> 32;
        start(static_cast<EnemyClip>(static_cast<unsigned>(EnemyClip::CorpseSplat01) + variant), owner, mix(random ^ 0x9e3779b9u), false);
        break;
    }
    case EnemySoundEvent::CrawlerLoop:
        start(EnemyClip::Crawler, owner, random, true);
        break;
    case EnemySoundEvent::BuzzerLoop:
        start(EnemyClip::Buzzer, owner, random, true);
        break;
    }
}

bool AudioVoices::startLoop(EnemyClip clip, std::uint32_t owner, std::uint32_t random)
{
    if (clip != EnemyClip::Crawler && clip != EnemyClip::Buzzer && clip != EnemyClip::ShadeFly && clip != EnemyClip::ShinyLoop && clip != EnemyClip::TabletHum && clip != EnemyClip::CityCrestMove && clip != EnemyClip::Tiktik && clip != EnemyClip::Gruzzer && clip != EnemyClip::HuskFootstep && clip != EnemyClip::Hatchling && clip != EnemyClip::Hatcher) return false;
    return start(clip, owner, random, true);
}

bool AudioVoices::play(EnemyClip clip, std::uint32_t owner, std::uint32_t random, float* actualPitch)
{
    if (clip == EnemyClip::Count) return false;
    if(actualPitch && (clip==EnemyClip::GreatDoorHit1||clip==EnemyClip::GreatDoorHit2))
        *actualPitch=uniformPitch(random,0.85f,1.15f);
    return start(clip, owner, random, false);
}

bool AudioVoices::playAtPitch(EnemyClip clip,std::uint32_t owner,float pitch)
{
    if(clip==EnemyClip::Count||!std::isfinite(pitch)||pitch<=0.0f)return false;
    return start(clip,owner,0,false,pitch);
}

std::size_t AudioVoices::playCustom(std::uint32_t sourceId,std::uint32_t owner,float gain,float pitch)
{
    if(!sourceId||!std::isfinite(gain)||gain<0||!std::isfinite(pitch)||pitch<=0||
       !start(EnemyClip::Count,owner,0,false,pitch,sourceId,gain))return VoiceCount;
    for(std::size_t i=LoopVoiceCount;i<VoiceCount;++i)
        if(voices_[i].generation==nextGeneration_)return i;
    return VoiceCount;
}

void AudioVoices::stopOwner(std::uint32_t owner)
{
    for (auto& voice : voices_)
        if (voice.active && voice.loop && voice.owner == owner) voice.active = false;
}

void AudioVoices::updatePitch(std::uint32_t owner, float pitch)
{
    for (auto& voice : voices_)
        if (voice.active && voice.loop && voice.owner == owner) voice.pitch = pitch;
}

bool AudioVoices::finish(std::size_t index, std::uint32_t generation)
{
    if (index >= VoiceCount) return false;
    auto& voice = voices_[index];
    if (!voice.active || voice.loop || voice.generation != generation) return false;
    voice.active = false;
    return true;
}

void AudioVoices::reset()
{
    for (auto& voice : voices_) voice.active = false;
}

void EnemyAudioEvents::dispatchDelta(const ActorAudioState& state, EnemySoundEvent event, std::uint32_t previous, std::uint32_t current, std::uint32_t salt)
{
    // A reused enemy slot starts new serials at zero. Never interpret its
    // reset as billions of queued events through unsigned subtraction.
    if(current<previous)previous=0;
    const std::uint32_t count = current - previous;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto serial = previous + i + 1;
        const auto random = mix(state.owner * 0x9e3779b9u ^ serial * 0x85ebca6bu ^ salt);
        voices_.dispatch(event, state.owner, random);
    }
}

void EnemyAudioEvents::observe(const ActorAudioState& state)
{
    const bool arena=state.owner==ArenaOwner;
    if (!arena&&(state.owner == 0 || state.owner > OwnerCount)) return;
    const bool ownerIsCrawler = state.owner <= 16;
    if (!arena&&state.crawler != ownerIsCrawler) return;
    if(arena&&!state.crawler)crawlerLoops_.back().active=false;

    auto& snapshot = snapshots_[arena?OwnerCount:state.owner - 1];
    const auto oldInjury = snapshot.seen ? snapshot.injurySerial : 0;
    const auto oldKill = snapshot.seen ? snapshot.killSerial : 0;
    const auto oldStartle = snapshot.seen ? snapshot.startleSerial : 0;
    const auto oldSplat = snapshot.seen ? snapshot.corpseSplatSerial : 0;
    dispatchDelta(state, EnemySoundEvent::Injury, oldInjury, state.injurySerial, 0x243f6a88u);
    dispatchDelta(state, EnemySoundEvent::Kill, oldKill, state.killSerial, 0x85a308d3u);
    dispatchDelta(state, EnemySoundEvent::Startle, oldStartle, state.startleSerial, 0x13198a2eu);
    dispatchDelta(state, EnemySoundEvent::CorpseSplat, oldSplat, state.corpseSplatSerial, 0x03707344u);
    snapshot.seen = true;
    snapshot.observed = true;
    snapshot.injurySerial = state.injurySerial;
    snapshot.killSerial = state.killSerial;
    snapshot.startleSerial = state.startleSerial;
    snapshot.corpseSplatSerial = state.corpseSplatSerial;

    if(state.crawler&&actorFrame_){
        if(arena)voices_.stopOwner(ArenaOwner);
        auto& source=crawlerLoops_[arena?16:state.owner-1];
        const bool active=state.active&&state.alive;
        if(active&&!source.active)source.cohort=actorFrame_;
        source.active=active;
        source.mix=enemyLoopMix(EnemyClip::Crawler,state.listenerDeltaX,state.listenerDeltaY,state.sourceDepth);
        return;
    }
    if (!state.active || !state.alive)
    {
        voices_.stopOwner(state.owner);
        return;
    }

    const auto loop = state.crawler ? EnemyClip::Crawler : EnemyClip::Buzzer;
    voices_.startLoop(loop, state.owner, mix(state.owner ^ 0xa4093822u));
    voices_.updatePitch(state.owner, state.chasePitch ? 1.25f : 1.0f);
    voices_.updateLoopMix(state.owner,enemyLoopMix(loop,state.listenerDeltaX,state.listenerDeltaY,state.sourceDepth));
}

void EnemyAudioEvents::endActorFrame(bool complete)
{
    constexpr std::uint32_t groupBit=0x80000000u;
    // The caller enumerates this frame's sources. Removed/deactivated sources
    // must not leave a cached loop running until a later room-epoch reset.
    for(unsigned i=0;complete&&i<snapshots_.size();++i)if(snapshots_[i].seen&&!snapshots_[i].observed){
        const auto owner=i==OwnerCount?ArenaOwner:i+1;
        voices_.stopOwner(owner);
        if(i<16||i==OwnerCount)crawlerLoops_[i==OwnerCount?16:i].active=false;
    }
    // Stop only a group whose final member stopped. Killing its first member
    // cannot restart the surviving members or change their source phase.
    for(const auto& voice:voices_.voices())if(voice.active&&voice.loop&&voice.clip==EnemyClip::Crawler&&(voice.owner&groupBit)){
        bool active=false;
        for(const auto& source:crawlerLoops_)active|=source.active&&(source.cohort|groupBit)==voice.owner;
        if(!active)voices_.stopOwner(voice.owner);
    }
    for(unsigned i=0;i<crawlerLoops_.size();++i){
        const auto& source=crawlerLoops_[i];if(!source.active)continue;
        bool previous=false;
        for(unsigned j=0;j<i;++j)previous|=crawlerLoops_[j].active&&crawlerLoops_[j].cohort==source.cohort;
        if(previous)continue;
        float left=0,right=0;
        for(const auto& member:crawlerLoops_)if(member.active&&member.cohort==source.cohort){
            left+=member.mix.gain*(1-member.mix.pan);right+=member.mix.gain*(1+member.mix.pan);
        }
        const auto owner=source.cohort|groupBit;
        voices_.startLoop(EnemyClip::Crawler,owner,0);
        voices_.updateLoopMix(owner,{(left+right)*.5f,left+right>0?(right-left)/(left+right):0});
    }
}

void EnemyAudioEvents::reset()
{
    voices_.reset();
    heroDashing_=false;heroDashEpoch_=0;
    snapshots_ = {};
    crawlerLoops_={};actorFrame_=0;
    greatDoor_={};floor_={};townLever_={};cocoon_={};scuttlers_={};stalactites_={};shade_={};fury_={};tablets_={};initialFall_={};collapse_={};secrets_={};masks_={};conditionalSerials_={};conditionalEpoch_=~0u;presentationRandom_=0;heroDead_=false;
}
void EnemyAudioEvents::observeSecrets(const std::array<unsigned,2>& cues,std::uint32_t epoch)
{
    if(secrets_.epoch!=epoch){
        for(std::size_t i=0;i<voices_.voices().size();++i){
            const auto& voice=voices_.voices()[i];
            if(voice.active&&voice.clip==EnemyClip::SecretDiscovered&&(voice.owner==58||voice.owner==59))voices_.finish(i,voice.generation);
        }
        secrets_={};secrets_.epoch=epoch;
    }
    for(unsigned i=0;i<2;++i){
        if(cues[i]>secrets_.cues[i])voices_.playAtPitch(EnemyClip::SecretDiscovered,58+i,1);
        secrets_.cues[i]=cues[i];
    }
}
void EnemyAudioEvents::observeRoomMasks(const std::array<std::uint32_t,4>& discoveries,std::uint32_t epoch)
{
    constexpr std::uint32_t firstOwner=0x4d000000u;
    if(masks_.epoch!=epoch){
        for(std::size_t i=0;i<voices_.voices().size();++i){
            const auto& voice=voices_.voices()[i];
            if(voice.active&&voice.clip==EnemyClip::SecretDiscovered&&voice.owner>=firstOwner&&voice.owner<firstOwner+128)
                voices_.finish(i,voice.generation);
        }
        masks_={};masks_.epoch=epoch;
    }
    for(unsigned word=0;word<discoveries.size();++word){
        const auto added=discoveries[word]&~masks_.discoveries[word];
        if(!added)continue;
        for(unsigned bit=0;bit<32;++bit)if(added&(std::uint32_t(1)<<bit))
            voices_.playAtPitch(EnemyClip::SecretDiscovered,firstOwner+word*32+bit,1);
    }
    masks_.discoveries=discoveries;
}
void EnemyAudioEvents::observeFloor(unsigned hits,std::uint32_t epoch)
{
    if(!floor_.seen||floor_.epoch!=epoch){floor_={true,epoch,hits};return;}
    if(hits>floor_.hits){
        voices_.playAtPitch(EnemyClip::FloorHit,35,1);
        if(hits==3)voices_.playAtPitch(EnemyClip::GreatDoorBreak,35,1);
    }
    floor_.hits=hits;
}
void EnemyAudioEvents::observeCollapse(bool crumbling,bool collapsed,std::uint32_t epoch)
{
    // Persisted Activated=true suppresses replay; Crumble precedes Break by the
    // unchanged room timer. Source FSM 12043 uses pitch/volume 1 for every cue.
    if(collapse_.epoch!=epoch)collapse_={epoch,false,collapsed};
    constexpr unsigned owner=57;
    if(crumbling&&!collapse_.crumbling)voices_.playAtPitch(EnemyClip::GreatDoorHit2,owner,1);
    if(collapsed&&!collapse_.collapsed){
        voices_.playAtPitch(EnemyClip::GreatDoorBreak,owner,1);
        voices_.playAtPitch(EnemyClip::CollapseAfterTutorial,owner,1);
        voices_.playAtPitch(EnemyClip::InitialLand,owner,1);
    }
    collapse_.crumbling=crumbling;collapse_.collapsed=collapsed;
}

void EnemyAudioEvents::observeInitialFall(const InitialFallPresentation& fall,std::uint32_t epoch)
{
    constexpr EnemyClip clips[]={EnemyClip::InitialLand,EnemyClip::InitialBird3,EnemyClip::InitialBird2,EnemyClip::OpeningSting};
    constexpr unsigned owner=0x20000u;
    if(initialFall_.epoch!=epoch){voices_.stopOwner(owner);initialFall_={};initialFall_.epoch=epoch;}
    for(unsigned i=0;i<4;++i){
        if(fall.cues()[i]>initialFall_.cues[i])voices_.playAtPitch(clips[i],owner,1);
        initialFall_.cues[i]=fall.cues()[i];
    }
}

void EnemyAudioEvents::observeStalactite(const Stalactite& actor,unsigned index,std::uint32_t epoch,float listenerX,float listenerY)
{
    if(index>=stalactites_.size())return;
    auto& previous=stalactites_[index];
    if(previous.epoch!=epoch){voices_.stopOwner(40+index);previous={};previous.epoch=epoch;}
    for(unsigned cue=0;cue<actor.cues.size();++cue){
        const auto kind=static_cast<StalactiteCue>(cue);
        if(kind==StalactiteCue::Start||kind==StalactiteCue::Hit||kind==StalactiteCue::Break||kind==StalactiteCue::UpBreak){
            const auto clip=kind==StalactiteCue::Start?EnemyClip::StalactiteStart:kind==StalactiteCue::Hit?EnemyClip::StalactiteHit:EnemyClip::StalactiteBreak;
            for(unsigned n=previous.cues[cue];n<actor.cues[cue];++n){
                // Embedded Breakable's stal_death table uses pitch .9..1.1.
                const float pitch=kind==StalactiteCue::Break?
                    .9f+.2f*float(mix(epoch^(index*0x9e3779b9u)^n))/4294967295.f:1.f;
                voices_.playAtPitch(clip,40+index,pitch);
            }
        }
    }
    // Tutorial level6 main sources 8384/8389/8395/8404/8431/8437/8441/8442:
    // linear 39..50, blend/volume 1, world Z .01; listener is camera Z -38.1.
    // StalactiteControl disables this source on landing or upward break. Its
    // new upward-break source and Breakable's spawned source have separate lives.
    const bool mainActive=actor.state!=StalactiteState::Embedded&&actor.state!=StalactiteState::Broken;
    AudioStereoMix spatial{};bool mixed=false;
    for(auto& voice:voices_.voices())if(voice.active&&voice.owner==40+index&&
        (voice.clip==EnemyClip::StalactiteStart||voice.clip==EnemyClip::StalactiteHit)){
        if(!mainActive){voice.active=false;continue;}
        if(!mixed){spatial=linearSpatialMix(actor.position.x-listenerX,actor.position.y-listenerY,38.11f,39,50);mixed=true;}
        voice.gain=spatial.gain;voice.pan=spatial.pan;
    }
    previous.cues=actor.cues;
}

void EnemyAudioEvents::observeFury(unsigned opens,unsigned pickups,bool idle,std::uint32_t roomEpoch,unsigned tinks)
{
    if(!fury_.seen||fury_.epoch!=roomEpoch||opens<fury_.opens||pickups<fury_.pickups||tinks<fury_.tinks){
        voices_.stopOwner(35);voices_.stopOwner(36);voices_.stopOwner(37);fury_={true,roomEpoch,0,0};
    }
    for(auto i=fury_.opens;i<opens;++i){voices_.playAtPitch(EnemyClip::ChestOpen,35,1);voices_.playAtPitch(EnemyClip::ChestBreak,35,1);}
    if(idle)voices_.startLoop(EnemyClip::ShinyLoop,37,0);else voices_.stopOwner(37);
    for(auto i=fury_.pickups;i<pickups;++i)voices_.playAtPitch(EnemyClip::ShinyPickup,36,1);
    for(auto i=fury_.tinks;i<tinks;++i)voices_.play(EnemyClip::NailTink,35,mix(++presentationRandom_));
    fury_.opens=opens;fury_.pickups=pickups;fury_.tinks=tinks;
}
void EnemyAudioEvents::observeHeroDeath(bool dead,std::uint32_t epoch)
{
    // Owners 40-47 are stalactites, 48 scuttlers, 49/50 conditional events.
    if(epoch!=heroDeathEpoch_){voices_.stopOwner(51);heroDeathEpoch_=epoch;heroDead_=false;}
    if(dead&&!heroDead_){
        voices_.playAtPitch(EnemyClip::HeroDeathDetails,51,1);
        voices_.playAtPitch(EnemyClip::HeroDeath,51,1);
        voices_.playAtPitch(EnemyClip::HeroDeathDamage,51,1);
    }
    heroDead_=dead;
}

void EnemyAudioEvents::observeHeroDash(bool dashing,std::uint32_t epoch)
{
    // HeroAudioController.DASH plays source13891 once when an accepted dash starts.
    // AudioPlayer resets the whole pool before observing a new room epoch.
    if(epoch!=heroDashEpoch_){heroDashEpoch_=epoch;heroDashing_=false;}
    if(dashing&&!heroDashing_)voices_.playAtPitch(EnemyClip::HeroDash,52,1);
    heroDashing_=dashing;
}

void EnemyAudioEvents::observeScuttlers(unsigned bounces,unsigned deaths,std::uint32_t roomEpoch)
{
    if(!scuttlers_.seen||scuttlers_.epoch!=roomEpoch||bounces<scuttlers_.bounces||deaths<scuttlers_.deaths){
        // Owners 38..39 belong to Shade and 40..47 to stalactites.
        voices_.stopOwner(48);scuttlers_={true,roomEpoch,0,0};
    }
    const auto pitch=[&](){return uniformPitch(mix(++presentationRandom_),.9f,1.1f);};
    for(auto i=scuttlers_.bounces;i<bounces;++i)voices_.playAtPitch(EnemyClip::ScuttlerBounce,48,pitch());
    for(auto i=scuttlers_.deaths;i<deaths;++i){
        voices_.playAtPitch(EnemyClip::EnemyDeathSword,48,pitch());
        voices_.playAtPitch(EnemyClip::EnemyDamage,48,pitch());
    }
    scuttlers_.bounces=bounces;scuttlers_.deaths=deaths;
}

void EnemyAudioEvents::observeCocoon(unsigned moves,unsigned breaks,std::uint32_t roomEpoch,float dx,float dy,float depth)
{
    constexpr std::uint32_t owner=0x30000u; // Separate from Great Door/secret cue owner 34.
    if(!cocoon_.seen||cocoon_.epoch!=roomEpoch||moves<cocoon_.moves||breaks<cocoon_.breaks){
        for(std::size_t i=0;i<voices_.voices().size();++i){
            const auto& voice=voices_.voices()[i];
            if(voice.active&&voice.owner==owner)voices_.finish(i,voice.generation);
        }
        cocoon_={true,roomEpoch,0,0};
    }
    // Tutorial source 8456: custom 3D rolloff, volume/pitch=1, listener Z=-38.1.
    // SetBroken hides the shell/colliders but leaves this source and its tails alive.
    for(auto i=cocoon_.moves;i<moves;++i)voices_.playAtPitch(EnemyClip::HealthCocoonMove,owner,1);
    for(auto i=cocoon_.breaks;i<breaks;++i)voices_.playAtPitch(EnemyClip::HealthCocoonBreak,owner,1);
    voices_.updateOwnerMix(owner,customSpatialMix(dx,dy,depth+38.1f,50,
        .7167469263076782f,1,-2.441073179244995f,1,-3.786637544631958f,1));
    cocoon_.moves=moves;cocoon_.breaks=breaks;
}
void EnemyAudioEvents::observeConditional(const ConditionalEvents& e,std::uint32_t epoch)
{
    const std::array<unsigned,13> serials{{e.mushroomHitSerial,e.rocketSerial,e.dreamSerial,e.teleSerial,e.dashSerial,e.shootSerial,e.spiralSerial,e.deathSerial,e.pickupSerial,e.dashStartSerial,e.castSerial,e.ringSerial,e.teleInSerial}};
    // Stalactites own 40..47 and scuttlers own 48 in the shared voice pool.
    constexpr unsigned mushroomOwner=49,grimmkinOwner=50;
    if(conditionalEpoch_!=epoch){voices_.stopOwner(mushroomOwner);voices_.stopOwner(grimmkinOwner);conditionalSerials_={};conditionalEpoch_=epoch;}
    const EnemyClip clips[]{EnemyClip::MushroomTalk,EnemyClip::MushroomRocket,EnemyClip::MushroomTalk,EnemyClip::GrimmTeleOut,EnemyClip::GrimmDashPrepare,EnemyClip::GrimmShoot,EnemyClip::GrimmSpiralPrepare,EnemyClip::GrimmDeath,EnemyClip::GrimmPickup,EnemyClip::GrimmDash,EnemyClip::GrimmSpiral,EnemyClip::GrimmRing,EnemyClip::GrimmTeleIn};
    for(unsigned i=0;i<serials.size();++i)if(serials[i]>conditionalSerials_[i])voices_.playAtPitch(clips[i],i<3?mushroomOwner:grimmkinOwner,i==12?1.25f:1);
    if(e.ringSerial>conditionalSerials_[11])voices_.playAtPitch(EnemyClip::GrimmFanfare,grimmkinOwner,1);
    conditionalSerials_=serials;
}
void EnemyAudioEvents::observeShade(const HollowShade& actor,std::uint32_t epoch)
{
    // Fury owns 35..37. Keep both Shade owners distinct so retries and pickup
    // tails cannot stop the other actor's voices in this shared channel pool.
    if(shade_.epoch!=epoch||actor.startleSerial<shade_.startle){voices_.stopOwner(38);voices_.stopOwner(39);shade_={};shade_.epoch=epoch;}
    if(actor.startleSerial>shade_.startle)voices_.playAtPitch(EnemyClip::ShadeStartle,38,1);
    if(actor.slashSerial>shade_.slash)voices_.play(EnemyClip::ShadeSword,38,mix(++presentationRandom_));
    if(actor.injurySerial>shade_.injury)voices_.play(EnemyClip::EnemyDamage,38,mix(++presentationRandom_));
    if(actor.blowSerial>shade_.blow){voices_.playAtPitch(EnemyClip::ShadeExplode,38,1);voices_.playAtPitch(EnemyClip::ShadeScream,38,1);}
    const bool flying=actor.alive()&&actor.state!=ShadeState::Idle&&actor.state!=ShadeState::Startle;
    if(shade_.flying&&!flying)voices_.stopOwner(39);
    if(flying)voices_.startLoop(EnemyClip::ShadeFly,39,0);
    shade_={epoch,actor.startleSerial,actor.slashSerial,actor.injurySerial,actor.blowSerial,flying};
}

void AudioVoices::updateLoopGain(std::uint32_t owner,float gain)
{
    if(!std::isfinite(gain))return;
    for(auto& voice:voices_)if(voice.active&&voice.loop&&voice.owner==owner)
    {
        // Aggregate voices retain the sum of their bounded source gains.
        const float limit=voice.clip==EnemyClip::TabletHum?3.f:
            voice.clip==EnemyClip::Crawler&&(owner&0x80000000u)?16.f:1.f;
        voice.gain=std::clamp(gain,0.f,limit);
    }
}
void AudioVoices::updateLoopMix(std::uint32_t owner,AudioStereoMix mix)
{
    if(!std::isfinite(mix.gain)||!std::isfinite(mix.pan))return;
    updateLoopGain(owner,mix.gain);
    for(auto& voice:voices_)if(voice.active&&voice.loop&&voice.owner==owner)
        voice.pan=std::clamp(mix.pan,-1.f,1.f);
}
void AudioVoices::updateOwnerMix(std::uint32_t owner,AudioStereoMix mix)
{
    if(!std::isfinite(mix.gain)||!std::isfinite(mix.pan))return;
    for(auto& voice:voices_)if(voice.active&&voice.owner==owner){
        voice.gain=std::max(0.f,mix.gain);voice.pan=std::clamp(mix.pan,-1.f,1.f);
    }
}
void EnemyAudioEvents::observeTablets(const RoomAmbient& ambient,std::uint32_t roomEpoch,float listenerX,float listenerY)
{
    if(!ambient.ready())return;
    if(!tablets_.seen||tablets_.epoch!=roomEpoch)tablets_={true,roomEpoch,{}};
    unsigned i=0;
    float left=0,right=0;
    for(const auto& tablet:ambient.tablets()){
        const unsigned owner=0x10000u+tablet.config.owner;
        const auto mix=tabletHumMix(tablet.config.position.x-listenerX,tablet.config.position.y-listenerY,
            tablet.config.depth+38.1f,tablet.hum());
        left+=mix.gain*(1-mix.pan);right+=mix.gain*(1+mix.pan);
        if(tablet.wakes()<tablets_.wakes[i])tablets_.wakes[i]=0;
        for(unsigned j=tablets_.wakes[i];j<tablet.wakes();++j)
            voices_.playAtPitch(EnemyClip::TabletWake,owner,1);
        if(tablet.inspections()<tablets_.inspections[i])tablets_.inspections[i]=0;
        for(unsigned j=tablets_.inspections[i];j<tablet.inspections();++j)
            voices_.playAtPitch(EnemyClip::TabletInspect,owner,1);
        tablets_.inspections[i]=tablet.inspections();
        tablets_.wakes[i++]=tablet.wakes();
    }
    // All three source AudioSources play the same loop/pitch on awake. Summing
    // envelopes preserves their common sample phase, including silent Away.
    constexpr unsigned humOwner=0x10000u;
    voices_.startLoop(EnemyClip::TabletHum,humOwner,0);
    voices_.updateLoopMix(humOwner,{(left+right)*.5f,left+right>0?(right-left)/(right+left):0});
}

void EnemyAudioEvents::observeSceneWalls(const std::array<unsigned,2>& cues,std::uint32_t roomEpoch)
{
    if(walls_.epoch!=roomEpoch)walls_={roomEpoch,{}};
    for(unsigned n=walls_.cues[0];n<cues[0];++n){
        presentationRandom_=mix(presentationRandom_+0x9e3779b9u);
        voices_.play(presentationRandom_&1?EnemyClip::GreatDoorHit1:EnemyClip::GreatDoorHit2,34,presentationRandom_);
    }
    for(unsigned n=walls_.cues[1];n<cues[1];++n){
        voices_.playAtPitch(EnemyClip::GreatDoorBreak,34,1);
        voices_.playAtPitch(EnemyClip::SecretDiscovered,34,1);
    }
    walls_.cues=cues;
}
void EnemyAudioEvents::observeGreatDoor(const GreatDoor& door,std::uint32_t roomEpoch)
{
    if(!greatDoor_.seen||greatDoor_.epoch!=roomEpoch||door.hits()<greatDoor_.hits)
    {
        greatDoor_={true,roomEpoch,0};
        presentationRandom_=mix(roomEpoch^0x47445231u);
    }
    const auto hits=door.hits();
    const auto thresholds=door.thresholds();
    for(auto hit=greatDoor_.hits+1;hit<=hits;++hit)
    {
        // Presentation RNG is isolated from combat and selects the two source hit clips equally.
        presentationRandom_=mix(presentationRandom_+0x9e3779b9u);
        const auto variant=(static_cast<std::uint64_t>(presentationRandom_)*2u)>>32;
        const auto clip=variant?EnemyClip::GreatDoorHit2:EnemyClip::GreatDoorHit1;
        float hitPitch=1.0f;
        voices_.play(clip,33,mix(presentationRandom_^hit),&hitPitch);
        if(hit==thresholds[0]||hit==thresholds[1]||hit==thresholds[2])
            voices_.playAtPitch(EnemyClip::GreatDoorBreak,33,hitPitch);
    }
    greatDoor_.hits=hits;
}
void EnemyAudioEvents::observeTownLever(unsigned hits,std::uint32_t epoch)
{
    if(!townLever_.seen||townLever_.epoch!=epoch||hits<townLever_.hits){
        if(townLever_.seen)for(std::size_t i=0;i<voices_.voices().size();++i){
            const auto& voice=voices_.voices()[i];
            if(voice.active&&voice.owner==316&&voice.clip==EnemyClip::TownLeverHit)
                voices_.finish(i,voice.generation);
        }
        townLever_={true,epoch,0};
    }
    if(hits>townLever_.hits)voices_.playAtPitch(EnemyClip::TownLeverHit,316,1);
    townLever_.hits=hits;
}
}
