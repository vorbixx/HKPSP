#pragma once
#include <array>
#include <cstdint>

namespace hk3ds
{
constexpr unsigned grassCapacity=41;
enum class GrassReaction { Walk, Attack };
enum class GrassSound { None, Move, StubMove, Cut };
enum class GrassContact { Player, Nail, SharpShadow, HeroBox, Other };
bool grassShouldCut(GrassContact contact,bool superDashing=false);
float grassCurve(GrassReaction reaction,float normalized);
struct GrassConfig
{
    std::uint32_t owner=0,drawOwner=0,idleSprite=0;
    std::array<std::uint32_t,4> pushSprites{};
    std::uint32_t stubSprite=0;
    float x=0,y=0,z=0,scaleX=1,left=0,bottom=0,right=0,top=0;
    std::array<float,16> stubValues{};
    // speed, amplitude, phase, push, height; matches GrassSway.
    std::array<float,5> stubGrass{};
    std::array<float,3> particleOffset{};
    bool interactive() const;
};
// HKG1: magic; count/spriteCount/drawCount u32; records of 8 u32,
// 8 floats (world xyz, local scale x, trigger l/b/r/t), stub matrix 16f,
// stub grass sway 5f, and local particle child XYZ 3f.
// No compiler-dependent struct serialization.
struct GrassBank
{
    unsigned count=0,spriteCount=0,drawCount=0;
    std::array<GrassConfig,grassCapacity> bushes{};
    bool load(const char* path);
};
class GrassState
{
    friend struct SimulationStateHash;
public:
    bool bind(const GrassConfig& config,float epsilon=0);
    void restart();
    GrassSound enter(float colliderX,GrassContact contact=GrassContact::Player,
                     bool superDashing=false);
    void exitPlayer() {player_=false;}
    GrassSound walk(float velocityX);
    GrassSound cut(float colliderX,GrassContact contact=GrassContact::Nail,
                   bool superDashing=false);
    void advance(float seconds);
    bool active() const {return config_&&!returned_;}
    bool cut() const {return cut_;}
    bool hasPlayer() const {return player_;}
    bool animatorPush() const {return animatorPush_;}
    float push() const {return push_;}
    float elapsed() const {return elapsed_;}
    unsigned frame() const;
    std::uint32_t sprite() const;
private:
    void start(GrassReaction reaction,float direction,bool animator);
    GrassSound moveSound() const {return cut_?GrassSound::StubMove:GrassSound::Move;}
    const GrassConfig* config_=nullptr;
    GrassReaction reaction_=GrassReaction::Walk;
    float epsilon_=0,push_=0,elapsed_=0,sampled_=0,direction_=1;
    bool returned_=true,cut_=false,player_=false,animatorPush_=false;
};
}
