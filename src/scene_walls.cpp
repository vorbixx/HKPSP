#include "scene_walls.hpp"
#include "hero.hpp"
#include "room_visual.hpp"
#include "breakable.hpp"
#include <algorithm>
namespace hk3ds {
namespace {
bool hitsMantisLever(const Hero& hero,const NailShape& nail,const TerrainWall& wall){
    for(unsigned v=1;v+1<wall.nailTriggerCount;++v)
        if(nailOverlapsQuad(hero,nail,{{wall.nailTrigger[0],wall.nailTrigger[v],
                                     wall.nailTrigger[v+1],wall.nailTrigger[v+1]}}))return true;
    return false;
}
}
void SceneWalls::move(unsigned i,Vec2 position){
    auto& state=states_[i];auto& shape=terrain_->shapes[terrain_->walls[i].shape];
    const Vec2 delta{position.x-state.offset.x,position.y-state.offset.y};
    shape.bounds.left+=delta.x;shape.bounds.right+=delta.x;
    shape.bounds.bottom+=delta.y;shape.bounds.top+=delta.y;
    for(unsigned v=shape.first;v<shape.first+shape.count;++v){terrain_->vertices[v].x+=delta.x;terrain_->vertices[v].y+=delta.y;}
    state.offset=position;
}
void SceneWalls::bind(TerrainBank& terrain,SceneBoolSave& saved,const SavedPlayerData& pd,DecorEffects* effects){
    terrain_=&terrain;saved_=&saved;effects_=effects;states_.clear();changed_=false;playerData_=0;cues_={};
    states_.resize(terrain.walls.size());restore(pd);
}
void SceneWalls::restore(const SavedPlayerData& pd){
    if(!terrain_)return;
    resetPose();changed_=false;playerData_=0;cues_={};
    for(unsigned i=0;i<states_.size();++i){
        auto& state=states_[i];const auto& wall=terrain_->walls[i];
        const bool deactivated=state.deactivated;
        auto draws=std::move(state.draws);state=State{};state.draws=std::move(draws);state.deactivated=deactivated;
        state.remaining=wall.hits;
        state.broken=saved_->get({wall.scene,wall.object})||(wall.playerData&&pd.get(wall.playerData));
        state.fade=state.broken?(wall.kind==TerrainWall::MantisGate?wall.hideDelay:.5f):0;state.presentation.reset({});
    }
    applyCollision();
}
void SceneWalls::enter(float heroX){
    if(!terrain_)return;
    for(unsigned i=0;i<states_.size();++i){
        const auto& wall=terrain_->walls[i];
        // Source Ruin Lift? enters terminal Deactivate on a strict X comparison.
        // Only Masks is disabled: the wall sprite, collider and camera locks stay.
        states_[i].deactivated=wall.entryPolicy==TerrainWall::DeactivateLeftOf&&heroX<wall.entryX;
    }
}
void SceneWalls::resetPose(){
    if(terrain_)for(unsigned i=0;i<states_.size();++i)move(i,{});
}
void SceneWalls::applyCollision(){
    if(!terrain_)return;
    for(unsigned i=0;i<states_.size();++i){
        const auto& wall=terrain_->walls[i];
        const auto& state=states_[i];
        terrain_->shapes[wall.shape].enabled=!state.broken||
            (wall.kind==TerrainWall::MantisGate&&state.fade<wall.collisionDelay);
        for(const auto index:wall.linkedShapes)terrain_->shapes[index].enabled=!state.broken;
    }
}
bool SceneWalls::breakWall(unsigned i){
    auto& state=states_[i];const auto& wall=terrain_->walls[i];
    if(state.broken||!saved_->set({wall.scene,wall.object}))return false;
    if(wall.kind==TerrainWall::MantisGate){
        // The source lever persists Activated on the nail hit. OPEN finishes
        // the gate animation before removing collision; restoration uses ACTIVATE.
        state.broken=true;state.fade=0;changed_=true;return true;
    }
    move(i,{});state.presentation.reset({});state.broken=true;state.fade=0;state.delay=0;state.cooldown=0;
    terrain_->shapes[wall.shape].enabled=false;playerData_=wall.playerData;changed_=true;
    for(const auto index:wall.linkedShapes)terrain_->shapes[index].enabled=false;
    if(wall.contactFloor||wall.oneWay){state.fade=.5f;return true;}
    ++cues_[1];
    if(effects_)effects_->persistentWall(wall.origin,wall.depth,wall.facing,true,terrain_->world());
    return true;
}
bool SceneWalls::hit(unsigned i,AttackType type){
    if(i>=states_.size()||terrain_->walls[i].contactFloor)return false;
    auto& state=states_[i];
    if(state.broken||state.deactivated||state.cooldown>0||state.delay>0)return false;
    if(terrain_->walls[i].kind==TerrainWall::MantisGate)return type==AttackType::Nail&&breakWall(i);
    const auto& wall=terrain_->walls[i];
    if(wall.oneWay&&(type!=AttackType::Nail||!state.heroInRange))return false;
    if(type==AttackType::Spell)return breakWall(i);
    if(type!=AttackType::Nail)return false;
    ++cues_[0];
    if(effects_)effects_->persistentWall(wall.origin,wall.depth,wall.facing,false,terrain_->world());
    if(--state.remaining==0){if(wall.oneWay)breakWall(i);else state.delay=1.f/60;}
    else state.cooldown=wall.hitWait>0?wall.hitWait:(wall.oneWay?.15f:.2f);
    return true;
}
bool SceneWalls::wallBreaker(unsigned i){
    return i<states_.size()&&terrain_->walls[i].kind==TerrainWall::Breakable&&!terrain_->walls[i].contactFloor&&!terrain_->walls[i].oneWay&&!states_[i].broken&&!states_[i].deactivated&&states_[i].cooldown<=0&&states_[i].delay<=0&&breakWall(i);
}
void SceneWalls::spell(Rect swept){
    if(!terrain_)return;
    for(unsigned i=0;i<states_.size();++i){
        if(terrain_->walls[i].kind==TerrainWall::MantisGate)continue;
        const auto& shape=terrain_->shapes[terrain_->walls[i].shape];
        if(shape.enabled&&overlaps(swept,shape.bounds)&&
           polygonOverlapsBox(terrain_->vertices.data()+shape.first,shape.count,swept))hit(i,AttackType::Spell);
    }
}
void SceneWalls::update(float seconds,Hero& hero,const NailBank* nails){
    if(!terrain_)return;
    for(unsigned i=0;i<states_.size();++i){
        auto& state=states_[i];const auto& wall=terrain_->walls[i];
        PresentationPosition::Step step(state.presentation,state.offset);
        if(wall.kind==TerrainWall::MantisGate){
            if(state.broken){
                state.fade=std::min(wall.hideDelay,state.fade+seconds);
                terrain_->shapes[wall.shape].enabled=state.fade<wall.collisionDelay;
            }else if(nails&&hero.nail.hitbox&&hero.nail.serial!=state.serial&&
                     hitsMantisLever(hero,nails->shapes[nailVariant(hero.nail)],wall)){
                state.serial=hero.nail.serial;hit(i,AttackType::Nail);
            }
            continue;
        }
        if(state.broken){state.fade=std::min(.5f,state.fade+seconds);continue;}
        if(state.deactivated)continue;
        if(state.delay>0){state.delay=std::max(0.f,state.delay-seconds);if(state.delay==0)breakWall(i);continue;}
        if(wall.contactFloor){
            // Collision2dEvent(Player) enters Split; Break follows its .4s Wait.
            bool touching=false;
            for(std::size_t c=0;c<hero.contacts.count;++c)
                touching|=hero.contacts.surfaces[c].id==terrain_->shapes[wall.shape].id;
            // The solver can retain a rounded-body contact beyond sensor skins.
            if(touching||boxSensorOverlap(bodyBounds(hero.position),terrain_->shapes[wall.shape].bounds))state.delay=wall.contactDelay;
            continue;
        }
        state.heroInRange=wall.oneWay&&boxSensorOverlap(bodyBounds(hero.position),wall.heroRange);
        state.cooldown=std::max(0.f,state.cooldown-seconds);
        const float offset=wall.oneWay?0:std::min(state.cooldown,.2f-state.cooldown);
        move(i,wall.facing==0?Vec2{offset,0}:wall.facing==2?Vec2{-offset,0}:wall.facing==3?Vec2{0,-offset}:Vec2{});
        if(!nails||!hero.nail.hitbox||hero.nail.serial==state.serial)continue;
        const auto& shape=terrain_->shapes[wall.shape];std::array<Vec2,4> polygon;
        std::copy_n(terrain_->vertices.begin()+shape.first,4,polygon.begin());
        const auto& nail=nails->shapes[nailVariant(hero.nail)];
        const bool triggerHit=(!wall.rejectDown||hero.nail.direction!=Strike::Down)&&
            (wall.oneWay?(wall.hitTriangles.empty()?
                (wall.hitBounds.left<wall.hitBounds.right?nailOverlaps(hero,nail,wall.hitBounds):nailOverlapsQuad(hero,nail,polygon)):
                std::any_of(wall.hitTriangles.begin(),wall.hitTriangles.end(),[&](const auto& triangle){return nailOverlapsPolygon(hero,nail,triangle.data(),triangle.size());})):
                nailOverlapsQuad(hero,nail,polygon));
        if(shape.enabled&&triggerHit){
            state.serial=hero.nail.serial;hit(i,AttackType::Nail);
        }
    }
}
float SceneWalls::alpha(unsigned owner) const{
    if(!terrain_)return 1;
    for(unsigned i=0;i<states_.size();++i){
        const auto& state=states_[i];if(!state.broken&&!state.deactivated)continue;
        const auto& wall=terrain_->walls[i];
        if(wall.kind==TerrainWall::MantisGate){
            if(state.broken&&state.fade>=wall.hideDelay&&(wall.root==owner||
               std::find(wall.children.begin(),wall.children.end(),owner)!=wall.children.end()))return 0;
            continue;
        }
        if(wall.root==owner&&state.broken)return 0;
        if(std::find(wall.children.begin(),wall.children.end(),owner)!=wall.children.end()){
            if(state.deactivated)return 0;
            // Source iTween EaseType 21 is linear.
            return wall.oneWay?0:std::max(0.f,1-state.fade/.5f);
        }
    }
    return 1;
}
bool SceneWalls::hidesLock(unsigned owner) const{
    if(!terrain_)return false;
    for(unsigned i=0;i<states_.size();++i)if(states_[i].broken){
        const auto& locks=terrain_->walls[i].locks;
        if(std::find(locks.begin(),locks.end(),owner)!=locks.end())return true;
    }
    return false;
}
void SceneWalls::bindVisuals(RoomVisualBank& room){
    for(unsigned i=0;i<states_.size();++i){
        auto& draws=states_[i].draws;draws.clear();const auto& wall=terrain_->walls[i];
        for(unsigned j=0;j<room.draws.size();++j){const auto& d=room.draws[j];
            if(d.owner==wall.root||std::find(wall.children.begin(),wall.children.end(),d.owner)!=wall.children.end()){
                draws.push_back({j,d.values[2],d.values[5]});
                if(wall.kind!=TerrainWall::MantisGate&&room.cameraBounds.size()==room.draws.size()&&d.sprite<room.atlas.sprites.size()){
                    auto bounds=roomCameraBounds(room.atlas.sprites[d.sprite],d);
                    bounds.left-=.1f;bounds.right+=.1f;bounds.bottom-=.1f;bounds.top+=.1f;
                    room.cameraBounds[j]=bounds;
                }
            }
        }
    }
}
void SceneWalls::presentVisuals(RoomVisualBank& room,PresentationTime time){
    for(unsigned i=0;i<states_.size();++i){
        const auto& state=states_[i];const auto& wall=terrain_->walls[i];
        if(wall.kind==TerrainWall::MantisGate)room.presentSwitches(wall.root,state.fade,state.broken);
        const auto offset=state.presentation.sample(state.offset,time);
        for(const auto& draw:state.draws){room.draws[draw.index].values[2]=draw.x+offset.x;room.draws[draw.index].values[5]=draw.y+offset.y;}
    }
}
}
