#pragma once
#include "geo.hpp"
#include "enemy_life_save.hpp"
#include "scene_bool_save.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>

namespace hk3ds {
// Stable order for the generated source table; bosses deliberately excluded.
enum class EnemyRewardKind : unsigned { Crawler,Tiktik,Gruzzer,Vengefly,Hatchling,
    Wandering,Hornhead,Leaping,Goam,Bully,Baldur,Mother,
    Mosscreep,Maskfly,MossKnight,Squit,Aluba,Durandoo,VolatileMosskin,MossCharger,Count };
struct EnemyRewardConfig {
    unsigned small=0,medium=0,large=0;
    Vec2 origin{};
    bool persistent=false,semiPersistent=false,megaFling=false;
};
const EnemyRewardConfig& enemyRewardConfig(EnemyRewardKind);

struct EnemyRewardDrop { EnemyRewardConfig config{};Vec2 position{}; };
template<unsigned Capacity> class EnemyRewardQueue {
    friend struct SimulationStateHash;
public:
    void setGreed(bool enabled){greed_=enabled;}
    bool push(EnemyRewardKind kind,Vec2 position,float scaleX=1) {
        return push(enemyRewardConfig(kind),position,scaleX);
    }
    bool push(const EnemyRewardConfig& c,Vec2 position,float scaleX=1,float scaleY=1,float rotation=0) {
        if(!c.small&&!c.medium&&!c.large)return true;
        if(count==Capacity){++rejected;return false;}
        // FlingUtils.SpawnAndFling applies TransformPoint to effectOrigin.
        const Vec2 local{c.origin.x*scaleX,c.origin.y*scaleY};
        const float cosine=std::cos(rotation),sine=std::sin(rotation);
        position.x+=local.x*cosine-local.y*sine;
        position.y+=local.x*sine+local.y*cosine;
        auto reward=c;
        // HealthManager adds ceil(20%) to each denomination at death. Snapshot
        // the result here so a full Geo pool cannot apply the charm twice.
        if(greed_){
            reward.small+=c.small/5+(c.small%5!=0);
            reward.medium+=c.medium/5+(c.medium%5!=0);
            reward.large+=c.large/5+(c.large%5!=0);
        }
        drops[count++]={reward,position};return true;
    }
    void release(GeoBank& bank) {
        unsigned remaining=0;
        for(unsigned i=0;i<count;++i){const auto& d=drops[i];const auto& c=d.config;
            if(!bank.spawnReward(c.small,c.medium,c.large,d.position,c.megaFling))drops[remaining++]=d;
        }
        count=remaining;
    }
    void clear(){count=rejected=0;greed_=false;}
    unsigned count=0,rejected=0;
private:
    bool greed_=false;
    std::array<EnemyRewardDrop,Capacity> drops{};
};

static_assert(sizeof(EnemyDeathRecord)*4096<=160*1024,"ordinary enemy lifetime state budget");
// SceneData's ordinary enemy state; permanent deaths use canonical scene booleans.
class EnemyLifeState {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=4096;
    const std::vector<EnemyDeathRecord>& saved()const{return records;}
    EnemyLifeSnapshot snapshot()const {
        if(records.empty()){snapshot_.reset();return {};}
        if(!snapshot_||snapshotRevision_!=revision_){
            snapshot_=std::make_shared<const EnemyDeathLedger>(records);snapshotRevision_=revision_;
        }
        return snapshot_;
    }
    std::uint32_t revision()const{return revision_;}
    bool restore(const std::vector<EnemyDeathRecord>& state) {
        if(state.size()>capacity)return false;
        std::vector<const EnemyDeathRecord*> keys;keys.reserve(state.size());
        for(const auto& r:state){
            if(!r.owner||!r.scene[0]||!std::memchr(r.scene,0,sizeof(r.scene)))return false;
            keys.push_back(&r);
        }
        // A duplicate key could otherwise retain a permanent death when the
        // conflicting semi-persistent record is cleared at a bench.
        std::sort(keys.begin(),keys.end(),[](const auto* a,const auto* b){
            return a->owner<b->owner||(a->owner==b->owner&&std::strcmp(a->scene,b->scene)<0);
        });
        for(std::size_t i=1;i<keys.size();++i)
            if(keys[i-1]->owner==keys[i]->owner&&!std::strcmp(keys[i-1]->scene,keys[i]->scene))return false;
        records=state;persistent_={};rejected=0;++revision_;snapshot_.reset();return true;
    }
    bool restoreSnapshot(const EnemyLifeSnapshot& state,const SceneBoolSave& persistent={}) {
        if(!persistent.valid())return false;
        const EnemyDeathLedger empty;
        // The caller may retain a mutable alias; own the validated restored data.
        EnemyLifeState restored;
        if(!restored.restore(state?*state:empty))return false;
        restored.persistent_=persistent;
        auto canonical=persistent;
        if(!restored.mergePersistent(canonical))return false;
        records=std::move(restored.records);persistent_=canonical;
        if(livePersistent_)*livePersistent_=canonical;
        rejected=0;++revision_;snapshot_.reset();return true;
    }
    // The session owns this table; walls and enemies must reserve the same slots.
    bool bindPersistent(SceneBoolSave& canonical) {
        if(!mergePersistent(canonical))return false;
        livePersistent_=&canonical;return true;
    }
    bool mergePersistent(SceneBoolSave& target)const {
        if(!target.valid())return false;
        auto merged=target;
        for(auto entry:persistent_.entries)if(entry.scene&&!merged.set(entry))return false;
        for(const auto& r:records)
            if(!r.semiPersistent&&!merged.set(persistentKey(r.scene,r.owner)))return false;
        target=merged;return true;
    }
    bool dead(const char* scene,std::uint32_t owner)const {
        if(!scene||!owner)return false;
        if((livePersistent_?*livePersistent_:persistent_).get(persistentKey(scene,owner)))return true;
        for(const auto& r:records)if(r.owner==owner&&!std::strcmp(r.scene,scene))return true;
        return false;
    }
    bool killed(const char* scene,std::uint32_t owner,const EnemyRewardConfig& config) {
        if(!config.persistent)return true;
        if(!owner||!scene||!scene[0]||std::strlen(scene)>=32){++rejected;return false;}
        // Admission authorizes the payout: restored/already recorded deaths cannot pay twice.
        if(dead(scene,owner))return false;
        if(records.size()==capacity){++rejected;return false;}
        SceneBoolSave admitted;
        if(!config.semiPersistent){
            admitted=livePersistent_?*livePersistent_:persistent_;
            if(!mergePersistent(admitted)||!admitted.set(persistentKey(scene,owner))){++rejected;return false;}
        }
        // A partially populated restored vector must not grow past the budget.
        if(records.size()==records.capacity())records.reserve(std::min(capacity,std::max(16u,unsigned(records.capacity())*2)));
        EnemyDeathRecord r;r.owner=owner;r.semiPersistent=config.semiPersistent;
        std::memcpy(r.scene,scene,std::strlen(scene)+1);records.push_back(r);
        if(!config.semiPersistent&&livePersistent_)*livePersistent_=admitted;
        ++revision_;return true;
    }
    void resetSemiPersistent() {
        const auto before=records.size();
        records.erase(std::remove_if(records.begin(),records.end(),[](const auto& r){return r.semiPersistent;}),records.end());
        if(records.size()!=before)++revision_;
    }
    void clear(){if(!records.empty()||persistent_.entries[0].scene)++revision_;records.clear();persistent_={};snapshot_.reset();rejected=0;}
    unsigned rejected=0;
private:
    static SceneBoolSave::Entry persistentKey(const char* scene,std::uint32_t owner) {
        return {SavedPlayerData::key(scene),owner};
    }
    SceneBoolSave persistent_{};
    SceneBoolSave* livePersistent_=nullptr;
    std::vector<EnemyDeathRecord> records;
    std::uint32_t revision_=0;
    mutable EnemyLifeSnapshot snapshot_;
    mutable std::uint32_t snapshotRevision_=0;
};
}
