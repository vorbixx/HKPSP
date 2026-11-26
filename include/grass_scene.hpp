#pragma once
#include "grass.hpp"
#include "room_visual.hpp"
#include "combat.hpp"
#include "audio.hpp"
namespace hk3ds {
class DecorEffects;
class RoomGrass {
    friend struct SimulationStateHash;
public:
    using AudioCursor=std::array<std::array<unsigned,3>,grassCapacity>;
    bool bind(const GrassBank&,RoomVisualBank&);
    void restart();
    void update(float seconds,const Hero&,const NailBank&,DecorEffects&,const World&);
    bool cut(unsigned owner,const Hero&,DecorEffects&,const World&);
    void dispatchAudio(AudioVoices&,AudioCursor&,Vec2 listener) const;
    bool ready() const {return bank_!=nullptr;}
    const GrassBank* bank() const {return bank_;}
    const GrassState& state(unsigned i) const {return states_[i];}
    const AudioCursor& soundCounters() const {return sounds_;}
    unsigned cuts() const;
private:
    void sound(unsigned,GrassSound);
    void present(unsigned);
    const GrassBank* bank_=nullptr;
    RoomVisualBank* room_=nullptr;
    std::array<GrassState,grassCapacity> states_{};
    std::array<RoomDraw,grassCapacity> originals_{};
    std::array<unsigned,grassCapacity> draws_{};
    std::array<bool,grassCapacity> players_{},nails_{};
    AudioCursor sounds_{};
};
}
