#include "enemy_rewards.hpp"
namespace hk3ds {
namespace {
#include "enemy_reward_data.hpp"
static_assert(sizeof(profiles)/sizeof(profiles[0])==unsigned(EnemyRewardKind::Count),"reward source table coverage");
}
const EnemyRewardConfig& enemyRewardConfig(EnemyRewardKind kind) {
    static const EnemyRewardConfig none{};
    const auto i=unsigned(kind);return i<unsigned(EnemyRewardKind::Count)?profiles[i]:none;
}
}
