#pragma once
#include "saved_player_data.hpp"
#include <cstdint>

namespace hk3ds {
// PersistentBoolItem is addressed by (base scene name, object id), independently
// of PlayerData and tutorial door bits. Empty slots form a canonical suffix.
struct SceneBoolSave {
    static constexpr unsigned capacity=64;
    struct Entry { std::uint32_t scene=0,object=0; };
    Entry entries[capacity]{};
    static bool less(Entry a,Entry b) {
        return a.scene<b.scene||(a.scene==b.scene&&a.object<b.object);
    }
    bool valid() const {
        Entry previous{};bool empty=false;
        for(auto entry:entries){
            if(!entry.scene&&!entry.object){empty=true;continue;}
            if(empty||!entry.scene||!entry.object||!less(previous,entry))return false;
            previous=entry;
        }
        return true;
    }
    bool get(Entry key) const {
        for(auto entry:entries){
            if(entry.scene==key.scene&&entry.object==key.object)return key.scene&&key.object;
            if(!entry.scene||less(key,entry))break;
        }
        return false;
    }
    bool set(Entry key) {
        if(!key.scene||!key.object||!valid())return false;
        unsigned index=0;
        while(index<capacity&&entries[index].scene&&less(entries[index],key))++index;
        if(index<capacity&&entries[index].scene==key.scene&&entries[index].object==key.object)return true;
        if(index==capacity||entries[capacity-1].scene)return false;
        for(unsigned i=capacity-1;i>index;--i)entries[i]=entries[i-1];
        entries[index]=key;return true;
    }
    static Entry key(const char* scene,const char* object) {
        return {SavedPlayerData::key(scene),SavedPlayerData::key(object)};
    }
};
}
