#pragma once
#include "world_switches.hpp"
#include <type_traits>
#include <utility>

namespace hk3ds {
// Adapts the save owner's SceneBoolSave interface without owning its schema.
// Store and its scene/object keys must outlive the room's switch binding.
template<class Store> SceneBoolHooks worldSwitchSaveHooks(Store& store)
{
    if(!store.valid())return {};
    return {&store,
        [](void* context,const char* scene,const char* id){
            const auto& saved=*static_cast<const Store*>(context);
            return saved.get(Store::key(scene,id));
        },
        [](void* context,const char* scene,const char* id,bool value){
            // Researched switches are latched; the source never writes false.
            return value&&static_cast<Store*>(context)->set(Store::key(scene,id));
        }};
}
// Older integration candidates have no scene table. Refuse admission there;
// never turn an absent save field into a transient unlocked gate.
template<class Save,class=void> struct WorldSwitchSavedState {
    static constexpr bool available=false;
    static SceneBoolHooks hooks(Save&){return {};}
    static void copy(Save&,const Save&){}
};
template<class Save> struct WorldSwitchSavedState<Save,
    std::void_t<decltype(std::declval<Save&>().sceneBools)>> {
    static constexpr bool available=true;
    static SceneBoolHooks hooks(Save& save){return worldSwitchSaveHooks(save.sceneBools);}
    static void copy(Save& to,const Save& from){to.sceneBools=from.sceneBools;}
};
}
