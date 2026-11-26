#include "great_door.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds {
bool GreatDoorConfig::load(const char* path)
{
    FILE* file=std::fopen(path,"rb");
    if (!file) return false;
    char magic[4]; unsigned integers[5]{}; float values[6]{};
    GreatDoorConfig next;
    bool ok=std::fread(magic,1,4,file)==4 && std::memcmp(magic,"GDR1",4)==0 &&
        std::fread(integers,4,5,file)==5 && std::fread(values,4,6,file)==6 &&
        std::fread(next.transition.scene,1,48,file)==48 &&
        std::fread(next.transition.gate,1,48,file)==48 && std::fgetc(file)==EOF;
    std::fclose(file);
    for (float v:values) ok=ok && std::isfinite(v);
    next.terrainId=integers[0]; next.layer=integers[1];
    next.thresholds={integers[2],integers[3],integers[4]};
    next.bounds={values[0],values[1],values[2],values[3]};
    next.hitWait=values[4]; next.transition.entryDelay=values[5];
    ok=ok && next.terrainId!=0 && next.layer<32 && next.thresholds[0]>0 &&
        next.thresholds[0]<next.thresholds[1] && next.thresholds[1]<next.thresholds[2] &&
        next.bounds.left<next.bounds.right && next.bounds.bottom<next.bounds.top &&
        next.hitWait>0 && next.transition.entryDelay>=0 &&
        next.transition.scene[0]!=0 && next.transition.gate[0]!=0 &&
        std::memchr(next.transition.scene,0,48) && std::memchr(next.transition.gate,0,48);
    if (ok) *this=next;
    return ok;
}
bool GreatDoor::bind(const GreatDoorConfig& config,TerrainBank& terrain,
                     CombatObject& object,bool persistedActivated)
{
    if (!config.terrainId || config.layer>=32 || config.thresholds[0]==0 ||
        config.thresholds[0]>=config.thresholds[1] || config.thresholds[1]>=config.thresholds[2] ||
        config.hitWait<=0) return false;
    bool found=false;
    for (const auto& shape:terrain.shapes) found=found || shape.id==config.terrainId;
    if (!found) return false;
    *this=GreatDoor{};
    config_=config; terrain_=&terrain; object_=&object; activated_=persistedActivated;
    // Bind is a room reset; restoring the intact branch is caller-authorized.
    terrain_->setEnabled(config_.terrainId,true);
    object={}; object.name="Great Door"; object.bounds=config.bounds;
    object.layer=config.layer; object.receiver=this; object.onHit=receive;
    return true;
}
void GreatDoor::update(float scaledDt)
{
    if (!object_) return;
    switch (state_) {
    case GreatDoorState::Pause:
        if (activated_) {
            terrain_->setEnabled(config_.terrainId,false);
            object_->colliderEnabled=false;
            visible_=false;
            state_=GreatDoorState::Activated;
        } else state_=GreatDoorState::Idle;
        break;
    case GreatDoorState::HitWait:
        if (scaledDt>0 && std::isfinite(scaledDt)) wait_-=scaledDt;
        if (wait_<=0) state_=GreatDoorState::Idle;
        break;
    case GreatDoorState::Break: state_=GreatDoorState::ExtraFrame; break;
    case GreatDoorState::ExtraFrame: state_=GreatDoorState::Move; break;
    case GreatDoorState::Idle:
    case GreatDoorState::Move:
    case GreatDoorState::Activated: break;
    }
}
bool GreatDoor::nail(const NailHit& hit)
{
    // ReceivedDamage requires positive damageDealt; Check If Nail admits attackType 0.
    // This entry point is deliberately nail-only; generic/spell callers must not use it.
    if (!object_ || state_!=GreatDoorState::Idle || hit.damage<=0) return false;
    ++hits_; ++hitEffects;
    if (hits_==config_.thresholds[0]) { stage_=1; ++crackEffects; }
    if (hits_==config_.thresholds[1]) { stage_=2; ++crackEffects; }
    if (hits_==config_.thresholds[2]) {
        activated_=true;
        state_=GreatDoorState::Break;
    } else {
        wait_=config_.hitWait;
        state_=GreatDoorState::HitWait;
    }
    return true;
}
void GreatDoor::receive(void* self,const NailHit& hit,HitOutcome)
{
    static_cast<GreatDoor*>(self)->nail(hit);
}
}
