#pragma once
#include "presentation.hpp"
#include "room_visual.hpp"
#include "combat.hpp"
#include "terrain.hpp"
namespace hk3ds
{
struct GrassBank;
enum class StalactiteCue : unsigned;
enum class GreatDoorBreakStage { Break1, Break2, Final };
// Activation commands, in source event order. Adapters own renderer/GO ids,
// particle instances and event receivers; this state owns no platform objects.
enum class BreakableAction { StaticParts, Hit, Debris, ForwardBreak, CameraShake };
struct BreakableActivation
{
    bool broken=false, whole=true, remnants=true, collider=true, debris=false;
    bool interactive=false;
};
class BreakableState
{
    friend struct SimulationStateHash;
public:
    using Notify=void (*)(void*,BreakableAction,const BreakableActivation&);
    // Awake restore is separate from Start: Start hides remnants even after
    // restoring broken. The initial flags come from serialized activation.
    void awake(BreakableActivation initial,bool savedBroken,bool hasHitReceiver,
               bool forwardBreak,void* context=nullptr,Notify notify=nullptr);
    void start(float z,float foreground,float background,bool hasBoxCollider=true);
    bool breakNow();
    const BreakableActivation& activation() const { return state_; }
private:
    void staticParts();
    void emit(BreakableAction action);
    BreakableActivation state_{};
    bool hitReceiver_=false,forwardBreak_=false,started_=false;
    void* context_=nullptr;
    Notify notify_=nullptr;
};
struct DoorConfig
{
    static constexpr std::size_t capacity=8;
    unsigned trigger=0, count=0, layer=0;
    std::array<unsigned,capacity> disabledShapes{};
    bool load(const char* path);
};
// Selected Breakable collision branch. Debris, sound and save storage are separate.
class BreakableDoor
{
    friend struct SimulationStateHash;
public:
    bool bind(const DoorConfig& config,TerrainBank& terrain,CombatObject& object,bool savedBroken=false);
    bool broken() const { return state_.activation().broken; }
private:
    static void receive(void* self,const NailHit&,HitOutcome);
    void breakDoor();
    BreakableState state_{};
    TerrainBank* terrain_=nullptr;
    CombatObject* object_=nullptr;
    DoorConfig config_{};
};
// Static, nonpersistent decor only. Animation, debris and sound are separate.
struct DecorConfig
{
    unsigned whole=0, remnant=0, layer=0;
    Rect bounds{};
    std::array<Vec2,4> vertices{};
};
bool validDecor(const DecorConfig&);
struct DecorBank
{
    static constexpr unsigned capacity=32;
    std::array<DecorConfig,capacity> items{};
    unsigned count=0;
    bool load(const char* path);
};
class BreakableDecor
{
    friend struct SimulationStateHash;
public:
    using HitEffect=void(*)(void*,unsigned,const NailHit&);
    bool bind(const DecorConfig&,CombatObject&,void* context=nullptr,HitEffect effect=nullptr);
    bool broken() const { return state_.activation().broken; }
    float alpha(unsigned owner) const;
private:
    static void receive(void*,const NailHit&,HitOutcome);
    DecorConfig config_{};
    BreakableState state_{};
    CombatObject* object_=nullptr;
    void* context_=nullptr;
    HitEffect effect_=nullptr;
};

struct RoomDebrisConfig
{
    unsigned flags=0;
    std::array<float,16> physics{}; // origin xyz, initial angle range, torque factor, body properties
    std::array<float,17> hull{};
    std::array<float,2> finish{};
    std::vector<unsigned> owners;
};
inline unsigned roomBreakAudioHash(const char* name) {
    unsigned hash=2166136261u;
    for(;*name;++name)hash=(hash^static_cast<unsigned char>(*name))*16777619u;
    return hash;
}
inline unsigned roomBreakClipId(const char* name){return 0x40000000u|(roomBreakAudioHash(name)&0x3fffffffu);}
struct RoomBreakSoundChoice {
    std::array<char,80> file{};
    float weight=0;
};
struct RoomBreakSoundCue {
    std::vector<RoomBreakSoundChoice> choices;
    float gain=1,low=1,high=1;
};
struct RoomBreakSoundEvent {
    const RoomBreakSoundChoice* choice=nullptr;
    unsigned owner=0;
    float gain=1,pitch=1;
};
struct RoomBreakableConfig
{
    unsigned owner=0,layer=0,debris=0,effects=31;
    std::array<Vec2,4> vertices{};
    std::vector<std::array<Vec2,3>> triangles;
    std::vector<std::array<Vec2,2>> segments;
    std::vector<CombatCircle> circles;
    Vec2 origin{},effect{};
    float depth=0,angle=0,effectZ=0;
    std::vector<unsigned> whole,remnants,colliders;
    bool impulse=false;
    std::array<float,16> launch{{30,70,10,17,70,110,15,25.5f,120,160,10,17,160,380,10,17}};
    std::vector<RoomDebrisConfig> rigidDebris;
    bool audioAuthored=false;
    std::vector<RoomBreakSoundCue> audio;
};
struct RoomBreakableBank
{
    std::vector<RoomBreakableConfig> items;
    bool load(const char*);
};
// Room-local identities and activation; the same BreakableState drives Tutorial.
class RoomBreakables
{
    friend struct SimulationStateHash;
public:
    using Effect=void(*)(void*,const RoomBreakableConfig&,unsigned,const NailHit&);
    bool bind(const RoomBreakableBank&,TerrainBank&,CombatObject*,unsigned,void*,Effect);
    void clear(){actors_.clear();bank_.items.clear();visibility_.clear();bodies_.clear();debrisOwners_.clear();terrain_=nullptr;soundSerial_=0;audioSeed_=1;soundEvents_={};}
    float alpha(unsigned owner)const;
    template<class Emit> void visitAlphaOwners(Emit emit)const{
        for(const auto& item:visibility_)emit(item.owner);
        for(const auto& item:debrisOwners_)emit(item.owner);
    }
    bool isDebris(unsigned owner)const;
    void fixedUpdate(float seconds,const World&);
    void visualUpdate(float seconds,Vec2 camera,float aspect);
    void transform(RoomDraw&,PresentationTime)const;
    bool testBreak(unsigned owner,const NailHit&);
    unsigned brokenCount()const;
    unsigned count()const{return unsigned(bank_.items.size());}
    const RoomBreakableBank& bank()const{return bank_;}
    unsigned soundSerial()const{return soundSerial_;}
    bool audioBroken(unsigned index)const{return index<actors_.size()&&actors_[index].state.activation().broken;}
    unsigned audioSeedBefore(unsigned index)const{return index<actors_.size()?actors_[index].audioSeedBefore:0;}
    const RoomBreakSoundEvent& soundEvent(unsigned serial)const{return soundEvents_[(serial-1)%soundEvents_.size()];}
private:
    struct Actor {BreakableState state;RoomBreakables* room=nullptr;CombatObject* object=nullptr;unsigned index=0,audioSeedBefore=1;};
    static void receive(void*,const NailHit&,HitOutcome);
    RoomBreakableBank bank_;
    // At most two cues per owner, with 127 admitted owners; no contact-time allocation.
    std::array<RoomBreakSoundEvent,254> soundEvents_{};
    unsigned soundSerial_=0,audioSeed_=1;
    struct Visibility {unsigned owner=0,index=0;bool remnant=false;};
    std::vector<Visibility> visibility_;
    std::vector<Actor> actors_;
    struct Body {
        const RoomDebrisConfig* config=nullptr;
        unsigned actor=0;
        Vec2 position{},velocity{};
        PresentationPosition history,rotationHistory;
        Vec2 rotationPose{};
        float rotation=0,spin=0,sleep=0,floor=0,quiet=0,shrink=1,age=0;
        unsigned invisibleFrames=0;
        bool active=false,shrinking=false;
    };
    struct DebrisOwner {unsigned owner=0,body=0;};
    std::vector<Body> bodies_;
    std::vector<DebrisOwner> debrisOwners_;
    unsigned random_=1;
    float random(float,float);
    TerrainBank* terrain_=nullptr;
    void* context_=nullptr;
    Effect effect_=nullptr;
};

struct DecorEmitter
{
    RoomMaterial material=RoomMaterial::Normal;
    unsigned sprite=0,tiles=0,count=0;
    float rate=0; // authored constant rate; count is the one-shot admission cap
    // position(3), angle, arc, radius, duration; life/speed/size/rotation ranges;
    // gravity, force range, damp, limit, bounce, kill speed; size/alpha/spin(8 each), RGBA.
    std::array<float,60> v{}; // XY module ranges, then forceZ/moduleZ/dampen/radius
    // tan(cone angle), scaled disk XY bases, unit direction XY bases (X,Y,Z).
    std::array<float,11> cone{};
    // Radius-scaled shape XY columns: neutral, left, right (after local rotation).
    std::array<float,12> bases{};
    std::array<float,18> axes{}; // unscaled-radius XYZ columns after each strike
    std::array<float,9> shapeOrigins{}; // room shape center XYZ after neutral/left/right root rotation
};
struct DecorEffectProfile
{
    unsigned owner=0;
    std::array<unsigned,2> emitters{};
    std::array<float,6> v{}; // root xyz, effect offset xy, angleOffset
};
struct GrassCutProfile
{
    unsigned owner=0,count=0;
    Vec2 origin{};
    float z=0;
};
class DecorEffects
{
    friend struct SimulationStateHash;
public:
    // Combined source pool bound 7697, rounded to 128 slots.
    static constexpr unsigned capacity=7808;
    static constexpr unsigned sharedEmitterCapacity=107,roomEmitterCapacity=256;
    static constexpr unsigned emitterCapacity=sharedEmitterCapacity+roomEmitterCapacity,profileCapacity=26,grassProfileCapacity=41;
    bool load(const char* config,const char* artwork);
    bool loadStalactites(const char* config);
    bool loadInitialFall(const char* config);
    bool loadOwners(const char* config);
    bool loadRoomEffects(const char* config,unsigned sprites);
    void clearRoomEffects();
    const std::vector<RoomDraw>& roomDraws()const{return roomDraws_;}
    bool ownerBreak(unsigned owner,Strike direction,const World&);
    void initialFall(bool flock,Vec2 hero,const World&);
    void stalactiteCue(StalactiteCue,Vec2 origin,float rotation,Strike,const World&);
    void stalactiteTrail(Vec2 origin,float rotation,float seconds,float& clock,const World&);
    void bindGrass(const GrassBank& bank);
    void reset(unsigned seed=1);
    bool hit(unsigned owner,const NailHit&,Vec2 source,const World& world = {},bool plant=false,
             const std::array<float,2>* debrisZ=nullptr);
    bool roomHit(const RoomBreakableConfig&,unsigned slot,const NailHit&,Vec2 source,const World&);
    // Visual-only Geo Rock FSM effects; does not claim a decor owner or emit audio/shake.
    void geoRockHit(Vec2 origin,float depth,Strike direction,bool broken);
    void healthCocoonBreak(Vec2 origin,Strike direction);
    void nailHit(Vec2 origin,float depth=0);
    void enemyNailHit(Vec2 origin,Strike direction,float slashScale=1.5f);
    void platformContact(const std::array<unsigned,3>& emitters,Vec2 origin,Vec2 size,
                         float dustRate,Vec2 streamOffset,bool rocks,const World& world);
    unsigned loadedEmitters() const {return emitterCount_;}
    // Great Door FSM bursts. The caller supplies Self and Damager positions after
    // an accepted hit; the implementation applies the source action offsets.
    void floorBreak(Vec2 self,const World&);
    void greatDoorHit(Vec2 self,Vec2 damager,float depth,Strike direction,const World& world);
    void greatDoorBreak(Vec2 self,float depth,GreatDoorBreakStage stage,const World&);
    void persistentWall(Vec2 self,float depth,unsigned facing,bool broken,const World&);
    void setView(Vec2 camera,float aspect) {viewCamera_=camera;viewAspect_=aspect;}
    // Existing bounded approximation of the source CameraShake EnemyKillShake.
    void enemyKillShake() { shake_=.12f; }
    // Grass cut shares the loaded particle and Slash Impact assets, with owner-specific particle placement.
    void grassCut(unsigned owner,Vec2 impactOrigin,float depth,Strike direction,const World& world);
    // Geometry is borrowed only for this call; no room view survives an update.
    void fixedUpdate(float seconds,const World& world);
    // End of scaled Update: every ParticleSystem advances in its birth frame;
    // tk2d starts at .001. fixedUpdate only advances the authored camera shake.
    void visualUpdate(float seconds,const World& world);
    void present(PresentationTime,unsigned layers=7);
    bool ready() const {return ready_;}
    unsigned count() const {return count_;}
    unsigned soundSerial() const {return sounds_;}
    unsigned decorSoundSerial() const {return sounds_-plantSounds_;}
    unsigned plantSoundSerial() const {return plantSounds_;}
    float soundPitch() const {return pitch_;}
    float plantSoundPitch() const {return plantPitch_;}
    Vec2 shake() const;
    RoomVisualBank visuals;
    struct Particle
    {
        Vec2 position{},velocity{};
        PresentationPosition history;
        float age=0,life=0,size=1,rotation=0,force=0,floor=0,z=0,scaleX=1,scaleY=1;
        float velocityZ=0,moduleZ=0,forceZ=0,forceX=0,gravity=0;
        float spin=0,sleep=0,offscreen=0;
        unsigned emitter=0,sprite=0,flash=0;
        bool freshFlash=false,freshDebris=false;
        Vec2 moduleVelocity{};
        Vec2 renderPosition(PresentationTime t) const {return history.sample(position,t);}
    };
    const Particle& particle(unsigned i) const {return particles_[i];}
    // Shared hit dust/flash advance visually and intentionally have no debris collision.
    bool visualParticle(const Particle& p) const {return p.flash||p.emitter==dust_[0]||p.emitter==dust_[1]||
        ownerVisual_[p.emitter]||
        (doorReady_&&(p.emitter==doorEmitters_[4]||p.emitter==doorEmitters_[5]))||
        (initialFallReady_&&(p.emitter==initialEmitters_[0]||p.emitter==initialEmitters_[1]||p.emitter==initialEmitters_[2]))||
        p.emitter==platformEmitters_[0]||p.emitter==platformEmitters_[1]||
        (stalactiteReady_&&(p.emitter==stalactiteEmitters_[0]||p.emitter==stalactiteEmitters_[1]||p.emitter==stalactiteEmitters_[2]||p.emitter==stalactiteEmitters_[3]));}
private:
    struct OwnerProfile {unsigned owner=0,count=0;std::array<unsigned,6> emitters{};};
    std::array<OwnerProfile,11> ownerProfiles_{};
    std::array<bool,11> ownerBroken_{};
    unsigned ownerCount_=0;
    std::array<unsigned,emitterCapacity> ownerShapes_{};
    std::array<bool,emitterCapacity> ownerVisual_{};
    std::array<Vec2,emitterCapacity> ownerParticleScale_{},ownerForceX_{};
    std::array<std::array<float,2>,emitterCapacity> ownerSorting_{};
    struct Flash {unsigned sprite=0,count=0;float fps=0;};
    std::vector<DecorEmitter> emitters_=std::vector<DecorEmitter>(emitterCapacity);
    struct RoomEffectChoice {float weight=0;std::vector<unsigned> emitters;};
    struct RoomEffectProfile {unsigned owner=0;std::vector<unsigned> emitters,spells;std::vector<RoomEffectChoice> choices;};
    std::vector<RoomEffectProfile> roomProfiles_;
    struct RoomFling {unsigned minimum=0;std::array<float,5> v{};}; // variation xy, fps, splash scale range
    std::vector<RoomFling> roomFlings_;
    struct RoomForce {unsigned active=0;std::array<float,48> curves{};};
    std::vector<RoomForce> roomForces_;
    std::vector<RoomDraw> roomDraws_;
    unsigned roomEmitterBase_=0;
    bool roomEffectsBound_=false;
    std::array<bool,127> roomBroken_{};
    std::array<DecorEffectProfile,profileCapacity> profiles_{};
    std::array<GrassCutProfile,grassProfileCapacity> grassProfiles_{};
    std::array<Flash,2> flashes_{};
    std::array<float,4> nailTransform_{}; // uniform scale min/max, integer Z range [min,max)
    void nailStrike(Vec2 origin,float depth);
    Flash stalactiteThunk_{};
    std::array<float,2> stalactiteThunkSorting_{};
    std::array<unsigned,5> stalactiteEmitters_{};
    std::array<std::array<float,3>,5> stalactiteOffsets_{};
    bool stalactiteReady_=false;
    std::array<unsigned,3> initialEmitters_{};
    std::array<std::array<float,3>,3> initialOffsets_{};
    std::array<std::array<float,2>,3> initialSorting_{},initialForceX_{};
    Flash initialLines_{};
    std::array<float,3> initialLinesOffset_{};
    float flockCycles_=0;
    bool initialFallReady_=false;
    std::array<unsigned,2> dust_{};
    std::array<unsigned,3> platformEmitters_{{0xffffffffu,0xffffffffu,0xffffffffu}};
    std::array<unsigned,2> doorRockEmitters_{{0xffffffffu,0xffffffffu}};
    std::array<unsigned,7> doorEmitters_{}; // hit rock, transient rock, A/B, door/impact dust, rubble.
    std::array<std::array<float,2>,3> doorSorting_{};
    // spawn xyz, speed/angle, torque factor, angular/linear damping, mass/inertia,
    // gravityScale, friction, bounce/threshold, polygon radius, source fixed timestep.
    std::array<std::array<float,16>,2> doorBodies_{};
    // Two original four-key (time, alpha) gradients per dust emitter.
    std::array<std::array<float,16>,2> doorAlpha_{};
    std::array<std::array<float,16>,2> doorSize_{};
    std::array<float,3> doorDelays_{};
    std::array<std::array<float,17>,2> doorHulls_{};
    bool doorReady_=false;
    Vec2 viewCamera_{};
    float viewAspect_=0;
    std::array<float,2> dustSorting_{};
    std::array<std::array<float,2>,2> flashSorting_{};
    std::array<bool,profileCapacity> broken_{};
    struct DustEmission {
        Vec2 origin{};
        float angle=0,floor=0,elapsed=0;
        unsigned emitter=0,emitted=0,seed=0;
        float z=0;
        Vec2 scale{1,1};
        float rate=0;
        unsigned cap=0;
        bool box=false;
        bool owned=false;
        unsigned basis=0;
    };
    std::array<DustEmission,profileCapacity+127> emissions_{};
    // Independent of one-shot scenery profiles: rocks can be struck repeatedly.
    std::array<DustEmission,64> geoEmissions_{};
    std::array<DustEmission,54> platformEmissions_{};
    std::array<DustEmission,36> ownerEmissions_{};
    std::array<DustEmission,32> doorEmissions_{};
    std::vector<DustEmission> roomEmissions_;
    unsigned activeEmissions_=0;
    // One bounded allocation at construction; large effect pools must not grow
    // TutorialSession's stack footprint. No allocation occurs during emission.
    std::vector<Particle> particles_=std::vector<Particle>(capacity);
    unsigned emitterCount_=0,profileCount_=0,grassProfileCount_=0,grassEmitter_=0xffffffffu,grassParticleCount_=0,count_=0,sounds_=0,plantSounds_=0,seed_=1;
    float grassAnimationFps_=0; // GRS1 zero selects the authored fixed random texture row.
    bool ready_=false,visualActive_=false;
    float shake_=0,pitch_=1,plantPitch_=1;
    std::array<float,2> audioPitch_{};
    float random(float low,float high);
    void emit(const World& world,unsigned emitter,Vec2 origin,float angle,float floor,bool placed,unsigned amount,unsigned basis=0,Vec2 offset={});
    bool hitProfile(const DecorEffectProfile&,unsigned,const NailHit&,Vec2,const World&,bool,
                    const std::array<float,2>*,Vec2 offset={},unsigned effects=31,float effectZ=0,bool objectEffects=false);
    void doorEmission(unsigned kind,Vec2 origin,float depth);
    void stalactiteEmission(unsigned kind,Vec2 origin,float rotation);
    void update(float seconds,bool visual,const World& world);
};
}
