#pragma once
#include "presentation.hpp"
#include "room_visual.hpp"
#include <array>

namespace hk3ds {
struct TabletConfig {
    unsigned owner=0,lit=0,light=0;
    Vec2 position{};
    float depth=0,enter=10,exit=12,upTime=1,downTime=1,lightAlpha=.209f;
    float humGain=.874f,humUp=.25f,humDown=1;
};
class TabletLight {
    friend struct SimulationStateHash;
public:
    TabletConfig config;
    void reset();
    void update(float dt,Vec2 hero,float heroDepth=.004f);
    float alpha() const {return alpha_;}
    float hum() const {return humStopped_?0:hum_;}
    void inspect() {humStopped_=true;++inspections_;}
    unsigned inspections() const {return inspections_;}
    bool close() const {return close_;}
    unsigned wakes() const {return wakes_;}
private:
    float alpha_=0,from_=0,elapsed_=0,hum_=0,humElapsed_=0;
    bool close_=false,transition_=false,humTransition_=false;
    unsigned wakes_=0;
    bool humStopped_=false;
    unsigned inspections_=0;
};
struct BuzzSettings {float waitMin=.75f,waitMax=1,speed=1.75f,acceleration=15,range=1;};
class GlowBug {
    friend struct SimulationStateHash;
public:
    void bind(Vec2 origin,BuzzSettings settings,unsigned seed);
    void fixedUpdate(float dt);
    Vec2 position() const {return position_;}
    Vec2 origin() const {return origin_;}
    Vec2 renderPosition(PresentationTime time) const {return history_.sample(position_,time);}
private:
    float uniform(float low,float high);
    void buzz(float dt);
    Vec2 origin_{},position_{},velocity_{},acceleration_{};
    PresentationPosition history_;
    BuzzSettings settings_;
    float wait_=0;
    unsigned random_=1;
};
class RoomAmbient {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned tabletCount=3,bugCount=11,visualCount=22;
    bool bind(RoomVisualBank& bank,const char* path);
    void reset(unsigned seed=1);
    void fixedUpdate(float dt,Vec2 hero);
    void inspect(unsigned owner) {for(auto& t:tablets_)if(t.config.owner==owner)t.inspect();}
    void present(PresentationTime time,float seconds);
    float alpha(unsigned owner) const;
    bool ready() const {return bank_!=nullptr;}
    // Consumers caching room geometry must retain the live path for these draws.
    bool animatesDraw(unsigned index) const {
        if(!bank_)return false;
        for(const auto& v:visuals_)if(v.draw==index)return true;
        for(const auto& g:particleGroups_)if(index>=g.draw&&index-g.draw<g.count)return true;
        return false;
    }
    const std::array<TabletLight,tabletCount>& tablets() const {return tablets_;}
    const std::array<GlowBug,bugCount>& bugs() const {return bugs_;}
    unsigned particleCount(unsigned owner=0) const;
    unsigned particleBirths(unsigned owner) const;
    unsigned particleDrops() const {return particleDrops_;}
    static constexpr unsigned particleCapacity=500;
private:
    struct BugConfig {unsigned owner=0;Vec2 position{};BuzzSettings settings;};
    struct Visual {unsigned owner=0,bug=0,draw=0,count=0;std::array<unsigned,6> frames{};float fps=0;RoomDraw base;};
    std::array<TabletLight,tabletCount> tablets_{};
    std::array<BugConfig,bugCount> config_{};
    std::array<GlowBug,bugCount> bugs_{};
    std::array<Visual,visualCount> visuals_{};
    // The two source depth groups share draws among their emitters. Disjoint
    // tablet activation ranges bound continuous overlap to 100 per group;
    // the single-playing Rim systems add 100 (upper) and 200 (lower pair).
    struct Emitter {
        unsigned owner=0,tablet=0,rim=0,group=0;
        // origin xyz, shape basis xy/xy, life range, size range, alpha,
        // world acceleration range, rate, duration, radius, start delay.
        std::array<float,18> v{};
        std::array<float,32> keys{};
        double credit=0;
        float remaining=0,delay=0;
        unsigned births=0;
    };
    struct Particle {
        Vec2 origin{},position{};
        PresentationPosition history;
        float age=0,life=0,size=0,blend=0,force=0;
        unsigned emitter=0;
        bool alive=false;
    };
    struct ParticleGroup {unsigned draw=0,count=0,sprite=0;};
    std::array<Emitter,6> emitters_{};
    std::array<ParticleGroup,2> particleGroups_{};
    std::array<Particle,particleCapacity> particles_{};
    unsigned particleRandom_=1,particleDrops_=0;
    float particleStep_=0;
    float particleRandom();
    void spawnParticle(unsigned emitter,float age);
    void updateParticles(float dt,const std::array<bool,tabletCount>& entered);
    RoomVisualBank* bank_=nullptr;
};
static_assert(sizeof(RoomAmbient)+RoomAmbient::particleCapacity*(sizeof(RoomDraw)+sizeof(RoomCameraBounds))<=192*1024,
              "tablet particles, draw metadata and bounds exceed their ordinary-heap cap");
}
