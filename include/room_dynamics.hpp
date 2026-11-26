#pragma once
#include "enemy.hpp"
#include "room_visual.hpp"

namespace hk3ds {
struct SpikeKey {float time=0;unsigned sprite=0;};
struct SpikeConfig {unsigned owner=0,authored=0;};
struct CrumbleDrawConfig {unsigned draw=0,owner=0;std::array<float,6> values{};};
struct SimpleRockConfig {
    unsigned owner=0;
    Vec2 spawn{};
    Rect collider{},trigger{};
    PhysicsMaterial material{};
    float mass=1,inertia=1,gravity=0,damping=0,bounce=.5f,threshold=1;
};
struct RoomDynamicsBank {
    unsigned spriteCount=0,drawCount=0;
    float duration=0;
    std::array<SpikeKey,4> keys{};
    std::array<SpikeConfig,32> spikes{};
    std::array<SimpleRockConfig,8> rocks{};
    unsigned crumbleCount=0;
    std::array<CrumbleDrawConfig,32> crumble{};
    bool load(const char*);
    unsigned sprite(float seconds) const;
};
struct SpikeAnimation {
    float seconds=0;
    bool visible=false,evaluated=false;
    void advance(float dt,bool nowVisible,float duration);
};
struct SimpleRock {
    Vec2 position{},velocity{},rotation{};
    PresentationPosition presentation,rotationPresentation;
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
    float renderAngle(PresentationTime t)const{return rotationPresentation.sample(rotation,t).x;}
    float angularVelocity=0,depthJitter=0;
    unsigned steps=0,enters=0,bounces=0,sampleStep=0,contactCount=0;
    std::uint64_t touching=0;
    Vec2 lastSample{},displacement{};
    float sampleSpeed=0;
    std::array<unsigned,16> contactIds{};
    const TerrainShape* supportShapes=nullptr;
    unsigned supportIndex=0;
    bool sleeping=false;
    void reset(const SimpleRockConfig&,EnemyRandom);
    Rect collider(const SimpleRockConfig&) const;
    bool touches(const SimpleRockConfig&,Rect)const;
    void contact(const SimpleRockConfig&,unsigned actor,bool overlap,float dt,EnemyRandom);
    void fixedUpdate(const SimpleRockConfig&,float dt,const World&,EnemyRandom);
};
class RoomDynamics {
    friend struct SimulationStateHash;
public:
    bool bind(const RoomDynamicsBank&,RoomVisualBank&);
    void restart();
    void fixedUpdate(float dt,const World&,Rect hero,const CombatObject* enemies,unsigned count);
    void present(float dt,float cameraX,float cameraY,PresentationTime);
    // Observe the rule state; source SetRotation is instantaneous in scaled Update.
    void observeCrumble(bool crumbling,bool collapsed);
    bool crumbleApplied()const{return crumbleApplied_;}
    bool collapseOwner(unsigned owner)const;
    template<class Emit> void visitCollapseOwners(Emit emit)const{
        if(bank_)for(unsigned i=0;i<bank_->crumbleCount;++i)emit(bank_->crumble[i].owner);
    }
    bool ready()const{return bank_!=nullptr;}
    const SimpleRock& rock(unsigned i)const{return rocks_[i];}
    const SpikeAnimation& spike(unsigned i)const{return spikes_[i];}
private:
    const RoomDynamicsBank* bank_=nullptr;
    RoomVisualBank* room_=nullptr;
    EnemyRandom random_;
    unsigned randomSeed_=0x524f434bu;
    static float randomUnit(void*);
    std::array<SpikeAnimation,32> spikes_{};
    std::array<SimpleRock,8> rocks_{};
    std::array<unsigned,32> spikeDraws_{};
    std::array<unsigned,8> rockDraws_{};
    std::array<RoomDraw,8> authored_{};
    std::array<std::array<float,6>,32> crumbleAuthored_{};
    bool crumbleApplied_=false;
};
}
