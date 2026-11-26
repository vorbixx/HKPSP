#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace hk3ds {
struct EnemyDeathRecord { char scene[32]{};std::uint32_t owner=0;bool semiPersistent=false; };
using EnemyDeathLedger=std::vector<EnemyDeathRecord>;
// Immutable snapshots can be polled by SaveWriter every frame without copying the ledger.
using EnemyLifeSnapshot=std::shared_ptr<const EnemyDeathLedger>;
}
