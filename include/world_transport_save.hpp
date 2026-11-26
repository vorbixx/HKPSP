#pragma once
#include "saved_player_data.hpp"
#include "world_lifts.hpp"
#include "world_key_doors.hpp"
#include <cstdint>
namespace hk3ds {
// The save owner supplies stable storage; these adapters introduce no schema.
inline LiftPlayerBoolHooks mineExitLiftSaveHooks(SavedPlayerData& store)
{
    if(!store.valid())return {};
    return {&store,
        [](void* p,const char* key){return static_cast<SavedPlayerData*>(p)->get(key)!=0;},
        [](void* p,const char* key,bool value){
            return key&&*key&&value&&static_cast<SavedPlayerData*>(p)->set(key,1);
        }};
}
inline JijiKeyDoorSaveHooks jijiKeyDoorSaveHooks(SavedPlayerData& store)
{
    if(!store.valid())return {};
    return {&store,
        [](void* p,const char* key)->int{return static_cast<SavedPlayerData*>(p)->get(key);},
        [](void* p,const char* key){return static_cast<SavedPlayerData*>(p)->get(key)!=0;},
        [](void* p,const char* key,int delta,const char* flag){
            auto& saved=*static_cast<SavedPlayerData*>(p);
            if(!saved.valid()||!key||!*key||!flag||!*flag||delta>=0)return false;
            const auto keyId=SavedPlayerData::key(key),flagId=SavedPlayerData::key(flag);
            if(keyId==flagId||saved.get(flagId)!=0)return false;
            const auto keys=saved.get(keyId);
            const auto remaining=static_cast<std::int64_t>(keys)+delta;
            if(keys<=0||remaining<0)return false;
            // A full table may accept the key update but refuse the new flag.
            // Publish both changes only after the bounded copy accepts both.
            auto next=saved;
            if(!next.set(keyId,static_cast<std::int32_t>(remaining))||
               !next.set(flagId,1))return false;
            saved=next;return true;
        }};
}
}
