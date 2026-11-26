#pragma once
#include "room_entities.hpp"
#include "room_visual.hpp"
#include "render_memory.hpp"

namespace hk3ds {
class DecorEffects;
// STV1 templates use the resident decor atlas; XY is relative to the actor body.
class StalactiteVisuals {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=stalactiteSpriteCapacity;
    struct Record {unsigned owner=0;RoomDraw hanging{},embedded{};};
    bool load(const char* path,const AnimationBank& atlas);
    bool bind(const std::array<Record,capacity>& records,unsigned count,const AnimationBank& atlas);
    void reset();
    void advance(float seconds,const Stalactite* actors,unsigned count,DecorEffects* effects=nullptr,const World& world={},PresentationTime time={1});
    void present(const Stalactite* actors,unsigned count,PresentationTime,RoomVisualBank& output) const;
    bool hides(unsigned owner) const;
    bool ready() const {return count_!=0;}
    unsigned count() const {return count_;}
    const Record& record(unsigned i) const {return records_[i];}
    RoomDraw draw(unsigned i,const Stalactite&,PresentationTime) const;
private:
    std::array<Record,capacity> records_{};
    std::array<float,capacity> trailClocks_{};
    std::array<std::array<unsigned,static_cast<unsigned>(StalactiteCue::Count)>,capacity> seen_{};
    unsigned count_=0;
};
}
