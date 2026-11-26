#pragma once
#include <cstdint>
#include <array>
#include <vector>
namespace hk3ds
{
// Approximation: reserve 6 dB for overlapping transients until the original
// master compressor/EQ is reproduced. Keep authored per-source gains intact.
constexpr float tutorialAudioMasterGain=.5f;
// Copied source-player output: a centered 2D mono source splits its power
// equally between speakers; a stereo source retains each authored channel.
constexpr float centeredAudioGain(unsigned channels,float volume=1)
{
    return volume*(channels==1?.7071067811865475244f:1.f);
}
class GreatDoor;
class Stalactite;
class HollowShade;
class RoomAmbient;
class InitialFallPresentation;
class ConditionalEvents;
struct PcmSound
{
    unsigned rate=0,channels=0;
    std::vector<std::int16_t> samples;
    bool load(const char* path);
};

// Logical enemy sounds are kept separate from the platform playback backend.
// The backend observes voice generations and reports completed one-shots via finish().
enum class EnemySoundEvent : std::uint8_t
{
    Injury,
    Kill,
    Startle,
    CorpseSplat,
    CrawlerLoop,
    BuzzerLoop
};

enum class EnemyClip : std::uint8_t
{
    Crawler,
    Buzzer,
    EnemyDamage,
    EnemyDeathSword,
    Startle01,
    Startle02,
    Startle03,
    CorpseSplat01,
    CorpseSplat02,
    CorpseSplat03,
    GrassMove1, GrassMove2, GrassMove3, GrassMove4, GrassMove5, GrassMove6,
    GrassStub1, GrassStub2, GrassStub3, GrassStub4, GrassStub5,
    GrassCut1, GrassCut2, GrassCut3, GrassCut4,
    GreatDoorHit1, GreatDoorHit2, GreatDoorBreak,
    HealthCocoonMove, HealthCocoonBreak,
    StalactiteStart, StalactiteHit, StalactiteBreak,
    ShadeStartle, ShadeFly, ShadeSword, ShadeExplode, ShadeScream,
    ChestOpen, ChestBreak, ShinyLoop, ShinyPickup,
    TabletHum, TabletWake,
    WaterDrip1,WaterDrip2,WaterDrip3,WaterDrip4,
    PlatformDirtFall1,PlatformDirtFall2,PlatformDirtFall3,
    ScuttlerBounce,
    TabletInspect,
    MushroomTalk,MushroomRocket,GrimmTeleIn,GrimmDashPrepare,GrimmDash,GrimmTeleOut,GrimmDeath,GrimmShoot,GrimmSpiralPrepare,GrimmSpiral,GrimmFanfare,GrimmRing,GrimmPickup,
    // Append to preserve main's resident clip IDs.
    HeroDeath, HeroDeathDamage, HeroDeathDetails,
    InitialLand, InitialBird3, InitialBird2, OpeningSting,
    NailTink,
    CollapseAfterTutorial,
    HeroDash,
    SecretDiscovered,
    SpiritCast,
    StagArrive, StagVoice, StagBell,
    UumuuZapAntic,UumuuZap,UumuuMultiZap,UumuuIntro,UumuuEnter,UumuuExit,UumuuSlash,UumuuWounded,
    UumuuShout1,UumuuShout2,UumuuDialogue1,UumuuDialogue2,UumuuDialogue3,UumuuFinalHit,UumuuGushing,UumuuExplode,
    KnightArmour, KnightHead, KnightJump, KnightSlam, KnightStagger, KnightRage, KnightDeath, KnightSwing, KnightLand,
    FloorHit,
    TownLeverHit,
    CityCrestOpen, CityCrestMove, CityCrestBump,
    StagBellRise,
    ShamanPillarShake, ShamanPillarFinish,
    Tiktik, Gruzzer, HuskFootstep, Hatchling, Hatcher,
    CityTollActivate, CityLiftActivate, CityLiftArrive, CityLiftClose, CityLiftOpen, JijiKeyTurn,
    JijiOpenAntic, JijiOpenSlamEnd,
    Count
};
constexpr bool streamedEnemyClip(EnemyClip clip){
    return clip==EnemyClip::OpeningSting||clip==EnemyClip::CollapseAfterTutorial||clip==EnemyClip::SecretDiscovered||
        clip==EnemyClip::HeroDeathDetails||
        (clip>=EnemyClip::KnightArmour&&clip<=EnemyClip::KnightLand);
}

// Gain is the mean of the stereo coefficients; pan is (right-left)/(right+left).
// This is also the convention used by the PCM acceptance analyzer.
struct AudioStereoMix { float gain=1,pan=0; };
// Serialized two-key, unweighted AudioSource envelope. Curve time is distance /
// maxDistance; endpoints clamp independently of minDistance, including nonzero tails.
// Only the measured blend=1/spread=0 sources use this contract.
struct AudioRolloffKey { float time=0,value=0,tangent=0; };
struct AudioSourceRolloff {
    float minimum=39,maximum=50,volume=1;
    bool custom=false;
    AudioRolloffKey first{},last{}; // first.outSlope and last.inSlope
};
AudioStereoMix sourceSpatialMix(const AudioSourceRolloff& source,float dx,float dy,float dz);
enum class EnemyLoopProfile { Default, CrawlerCrossroads01 };
AudioStereoMix enemyLoopMix(EnemyClip clip,float listenerDeltaX,float listenerDeltaY,float sourceDepth=0,
    EnemyLoopProfile profile=EnemyLoopProfile::Default);
// Tutorial SetZ: retain the transform's depth until the scaled .5-second wait.
class EnemySourceDepth {
    friend struct SimulationStateHash;
public:
    void activate(float initialDepth,float minimumDepth,double scaledSeconds,std::uint32_t random);
    float value(double scaledSeconds)const{return scaledSeconds>=setAt_?target_:initial_;}
    double activatedAt()const{return setAt_-.5;}
private:
    double setAt_=0;
    float initial_=0,target_=0;
};
AudioStereoMix linearSpatialMix(float dx,float dy,float dz,float minimum,float maximum,float volume=1);
AudioStereoMix tabletHumMix(float dx,float dy,float dz,float volume);
AudioStereoMix shinyLoopMix(float dx,float dy,float dz);
AudioStereoMix shadeLoopMix(float dx,float dy);

struct AudioVoice
{
    bool active = false;
    bool loop = false;
    EnemyClip clip = EnemyClip::Crawler;
    float gain = 1.0f;
    float pitch = 1.0f;
    std::uint32_t owner = 0;
    std::uint32_t generation = 0;
    float pan = 0;
    std::uint32_t sourceId = 0; // Nonresident PCM; zero selects the resident enum.
};

struct AudioClipCounters
{
    std::uint32_t dispatched = 0;
    std::uint32_t started = 0;
    std::uint32_t stolen = 0;
    std::uint32_t dropped = 0;
};

class AudioVoices
{
public:
    static constexpr std::size_t EnemyLoopVoiceCount = 6;
    // The three play-on-awake tablet sources share PCM, pitch and sample phase.
    // Their independent envelopes sum into one protected hum voice.
    static constexpr std::size_t TabletHumVoice = EnemyLoopVoiceCount;
    static constexpr std::size_t LoopVoiceCount = EnemyLoopVoiceCount + 1;
    static constexpr std::size_t VoiceCount = LoopVoiceCount + 6;
    static constexpr std::size_t TransientVoiceCount = VoiceCount - LoopVoiceCount;

    // Dispatching an event is deterministic for a given random value and allocates no memory.
    void dispatch(EnemySoundEvent event, std::uint32_t owner, std::uint32_t random);
    bool play(EnemyClip clip, std::uint32_t owner, std::uint32_t random, float* actualPitch=nullptr);
    bool playAtPitch(EnemyClip clip, std::uint32_t owner, float pitch);
    std::size_t playCustom(std::uint32_t sourceId,std::uint32_t owner,float gain,float pitch);
    bool startLoop(EnemyClip clip, std::uint32_t owner, std::uint32_t random);
    void stopOwner(std::uint32_t owner);
    void updatePitch(std::uint32_t owner, float pitch);
    void updateLoopGain(std::uint32_t owner,float gain);
    void updateLoopMix(std::uint32_t owner,AudioStereoMix mix);
    void updateOwnerMix(std::uint32_t owner,AudioStereoMix mix);
    bool finish(std::size_t index, std::uint32_t generation);
    void reset();

    std::array<AudioVoice, VoiceCount>& voices() { return voices_; }
    const std::array<AudioVoice, VoiceCount>& voices() const { return voices_; }
    const std::array<AudioClipCounters, static_cast<std::size_t>(EnemyClip::Count)>& counters() const { return counters_; }

private:
    bool start(EnemyClip clip, std::uint32_t owner, std::uint32_t random, bool loop, float pitchOverride=-1.0f,
               std::uint32_t sourceId=0,float customGain=1);
    static std::size_t clipIndex(EnemyClip clip) { return static_cast<std::size_t>(clip); }
    std::array<AudioVoice, VoiceCount> voices_{};
    std::array<AudioClipCounters, static_cast<std::size_t>(EnemyClip::Count)> counters_{};
    std::uint32_t nextGeneration_ = 0;
    std::uint32_t nextOrder_ = 0;
    std::array<std::uint32_t, VoiceCount> order_{};
};

struct ActorAudioState
{
    std::uint32_t owner = 0;
    bool active = false;
    bool alive = false;
    bool crawler = false;
    bool chasePitch = false;
    std::uint32_t injurySerial = 0;
    std::uint32_t killSerial = 0;
    std::uint32_t startleSerial = 0;
    std::uint32_t corpseSplatSerial = 0;
    float listenerDeltaX = 0, listenerDeltaY = 0;
    float sourceDepth = 0;
};

class EnemyAudioEvents
{
public:
    static constexpr std::size_t OwnerCount = 32;
    static constexpr std::uint32_t ArenaOwner = 81;
    void observe(const ActorAudioState& state);
    // Crawler sources starting in the same render share PCM phase. Sum their
    // independent stereo envelopes, retaining all six transient reservations.
    void beginActorFrame(){++actorFrame_;for(auto& snapshot:snapshots_)snapshot.observed=false;}
    // Complete enumerations also stop sources absent from this frame; partial
    // observation batches retain unmentioned sources and their sample phase.
    void endActorFrame(bool complete=false);
    void observeSceneWalls(const std::array<unsigned,2>& cues,std::uint32_t roomEpoch);
    void observeGreatDoor(const GreatDoor& door, std::uint32_t roomEpoch);
    void observeTownLever(unsigned hits,std::uint32_t roomEpoch);
    void observeFury(unsigned opens,unsigned pickups,bool idle,std::uint32_t roomEpoch,unsigned tinks=0);
    void observeCocoon(unsigned moves,unsigned breaks,std::uint32_t roomEpoch,float dx,float dy,float depth);
    void observeConditional(const ConditionalEvents&,std::uint32_t roomEpoch);
    void observeScuttlers(unsigned bounces,unsigned deaths,std::uint32_t roomEpoch);
    void observeStalactite(const Stalactite&,unsigned index,std::uint32_t roomEpoch,
        float listenerX=0,float listenerY=0);
    void observeShade(const HollowShade&,std::uint32_t roomEpoch);
    void observeTablets(const RoomAmbient& ambient,std::uint32_t roomEpoch,float listenerX=0,float listenerY=0);
    void observeSecrets(const std::array<unsigned,2>& cues,std::uint32_t roomEpoch);
    void observeRoomMasks(const std::array<std::uint32_t,4>& discoveries,std::uint32_t roomEpoch);
    void observeFloor(unsigned hits,std::uint32_t roomEpoch);
    void observeCollapse(bool crumbling,bool collapsed,std::uint32_t roomEpoch);
    void observeInitialFall(const InitialFallPresentation&,std::uint32_t roomEpoch);
    void observeHeroDeath(bool dead,std::uint32_t roomEpoch);
    void observeHeroDash(bool dashing,std::uint32_t roomEpoch);
    void reset();
    AudioVoices& voices() { return voices_; }
    const AudioVoices& voices() const { return voices_; }

private:
    std::array<unsigned,13> conditionalSerials_{};
    unsigned conditionalEpoch_=~0u;

    struct Snapshot
    {
        bool seen = false;
        bool observed = false;
        std::uint32_t injurySerial = 0;
        std::uint32_t killSerial = 0;
        std::uint32_t startleSerial = 0;
        std::uint32_t corpseSplatSerial = 0;
    };
    struct GreatDoorSnapshot
    {
        bool seen=false;
        std::uint32_t epoch=0,hits=0;
    };
    void dispatchDelta(const ActorAudioState& state, EnemySoundEvent event, std::uint32_t previous, std::uint32_t current, std::uint32_t salt);
    AudioVoices voices_;
    std::array<Snapshot, OwnerCount+1> snapshots_{};
    struct CrawlerLoop {std::uint32_t cohort=0;bool active=false;AudioStereoMix mix{};};
    std::array<CrawlerLoop,17> crawlerLoops_{};
    std::uint32_t actorFrame_=0;
    GreatDoorSnapshot greatDoor_{},floor_{},townLever_{};
    struct WallSnapshot {std::uint32_t epoch=0;std::array<unsigned,2> cues{};} walls_{};
    struct CocoonSnapshot {bool seen=false;std::uint32_t epoch=0;unsigned moves=0,breaks=0;} cocoon_{};
    struct ScuttlerSnapshot {bool seen=false;std::uint32_t epoch=0;unsigned bounces=0,deaths=0;} scuttlers_{};
    struct StalactiteSnapshot {std::uint32_t epoch=0;std::array<unsigned,6> cues{};};
    std::array<StalactiteSnapshot,8> stalactites_{};
    struct ShadeSnapshot {std::uint32_t epoch=0;unsigned startle=0,slash=0,injury=0,blow=0;bool flying=false;} shade_{};
    struct FurySnapshot {bool seen=false;std::uint32_t epoch=0;unsigned opens=0,pickups=0,tinks=0;} fury_{};
    struct TabletSnapshot {bool seen=false;std::uint32_t epoch=0;std::array<unsigned,3> wakes{},inspections{};} tablets_{};
    struct SecretSnapshot {std::uint32_t epoch=0;std::array<unsigned,2> cues{};} secrets_{};
    struct MaskSnapshot {std::uint32_t epoch=0;std::array<std::uint32_t,4> discoveries{};} masks_{};
    struct CollapseSnapshot {std::uint32_t epoch=0;bool crumbling=false,collapsed=false;} collapse_{};
    struct InitialFallSnapshot {std::uint32_t epoch=0;std::array<unsigned,4> cues{};} initialFall_{};
    std::uint32_t presentationRandom_=0;
    bool heroDead_=false;
    std::uint32_t heroDeathEpoch_=0;
    bool heroDashing_=false;
    std::uint32_t heroDashEpoch_=0;
};
}
