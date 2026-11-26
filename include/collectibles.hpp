#pragma once
#include "combat.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace hk3ds {
enum class PickupKind : std::uint32_t { Grub, MaskShard, VesselFragment, GeoDeposit, RancidEgg, Journal };
// Original scene number + GameObject path ID, independent of conversion order.
struct PickupRecord {
    std::uint32_t id=0,owner=0;
    PickupKind kind=PickupKind::Grub;
    unsigned hits=1,value=1;
    Vec2 position{};
    Rect bounds{};
    std::vector<unsigned> visualOwners;
    // Bottle Control requires the hero inside its separate Hero Range collider.
    bool requireHeroRange=false;
    Rect heroRange{};
};
struct CollectibleProgress {
    static constexpr unsigned capacity=128;
    std::array<std::uint32_t,capacity> collected{};
    std::uint32_t count=0,grubs=0,maskShards=0,vesselFragments=0,rancidEggs=0,journals=0;
    bool contains(unsigned id) const;
    bool valid() const;
};
class Collectibles {
    friend struct SimulationStateHash;
public:
    bool load(const char* path);
    bool bind(const std::vector<PickupRecord>& records);
    void restore(const CollectibleProgress& progress,Vitals& vitals);
    // Returns true only when an item changes the persistent inventory.
    bool update(Hero& hero,const NailBank& nails,unsigned& geo);
    // Source one-shot world events share the existing stable object-ID ledger.
    // They do not grant pickup rewards or change collectible counters.
    bool recordSourceEvent(unsigned id);
    // Non-counted source pickups (abilities) share the existing durable ID ledger.
    // False means duplicate, invalid identity, or full ledger; callers must not grant.
    bool remember(unsigned sourceId);
    // Roll back only the immediately preceding non-counted remember operation.
    bool undoRemember(unsigned sourceId);
    bool hides(unsigned owner) const;
    const CollectibleProgress& saved() const {return progress_;}
    const std::vector<PickupRecord>& records() const {return records_;}
    bool collected(unsigned index) const {return progress_.contains(records_[index].id);}
private:
    std::vector<PickupRecord> records_;
    std::array<unsigned,128> hits_{},attacks_{};
    CollectibleProgress progress_{};
    void apply(Vitals& vitals,bool heal) const;
};
}
