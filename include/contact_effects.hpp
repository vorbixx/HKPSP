#pragma once
#include "breakable.hpp"
#include "audio.hpp"

namespace hk3ds {
enum class DripPhase : unsigned { Idle, Drip, Fall, Impact };
struct DripConfig {
    unsigned owner=0;
    // XYZ, affine XX/XY/YX/YY, collider L/B/R/T, idle min/max, fall speed,
    // impact translation, sorting layer/order. Source transforms have unit scale.
    std::array<float,17> v{};
};
struct DustyPlatformConfig {
    unsigned owner=0,collider=0;
    // XY, collider width/height, inset, rate area factor/constant, cooldown,
    // rock probability, stream offset XYZ, depth, ignored layers.
    std::array<float,14> v{};
};
struct ContactEffectBank {
    std::array<DripConfig,9> drips{};
    std::array<DustyPlatformConfig,18> platforms{};
    std::array<unsigned,3> emitters{}; // falling dust, stream, rocks
    struct Clip { float fps=0; unsigned count=0; std::array<unsigned,32> sprites{}; };
    std::array<Clip,4> clips{};
    struct SoundTable {
        float pitchMin=1,pitchMax=1;
        unsigned count=0;
        std::array<unsigned,8> clips{};
        std::array<float,8> weights{};
        // Max distance, first time/value/out tangent, last time/value/in
        // tangent, spatial blend. Times are normalized by max distance.
        std::array<float,8> spatial{};
    };
    std::array<SoundTable,2> sounds{};
    bool load(const char* path,unsigned spriteCount,unsigned emitterCount);
};
class RoomContactEffects {
    friend struct SimulationStateHash;
public:
    // Raw contact selection inputs, retained before audio dispatch for acceptance.
    struct Sound {unsigned serial=0,clip=0;float pitch=1,z=0;Vec2 position{};unsigned selectionSeed=0;};
    struct Drip {
        Vec2 position{},velocity{};
        PresentationPosition history;
        DripPhase phase=DripPhase::Idle;
        float elapsed=0,wait=0;
        // WaterDrip stores startPos as Vector2: the first reset writes Z=0.
        float audioZ=0;
        Vec2 renderPosition(PresentationTime t) const {return history.sample(position,t);}
    };
    bool load(const char* path,const DecorEffects& effects);
    void bind(const ContactEffectBank& bank,unsigned seed=1);
    void reset(unsigned seed=1);
    void fixedUpdate(float seconds,const World& world);
    void update(float seconds,const Contacts& heroContacts,DecorEffects& effects,const World& world);
    void present(RoomVisualBank& visuals,PresentationTime time) const;
    bool owns(unsigned owner) const;
    void dispatchAudio(AudioVoices& voices,std::array<unsigned,27>& serials,Vec2 listener={}) const;
    const Drip& drip(unsigned i) const {return drips_[i];}
    const ContactEffectBank& bank() const {return bank_;}
    unsigned platformSerial(unsigned i) const {return sounds_[9+i].serial;}
    float platformCooldown(unsigned i) const {return platforms_[i].cooldown;}
    bool ready() const {return ready_;}
    const auto& soundStates() const {return sounds_;}
private:
    struct Platform {float cooldown=0;bool contact=false;};
    ContactEffectBank bank_{};
    std::array<Drip,9> drips_{};
    std::array<Platform,18> platforms_{};
    std::array<Sound,27> sounds_{};
    unsigned seed_=1;
    bool ready_=false;
    float random(float low,float high);
    void sound(unsigned owner,unsigned table);
};
}
