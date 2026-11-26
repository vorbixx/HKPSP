#pragma once
#include "world_key_doors.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
// JJD1 is exactly 184 bytes. Keep this owner at a stable address while the
// bound JijiKeyDoor borrows keyName and unlockName.
struct WorldKeyDoorRoomData {
    char keyName[64]{};
    char unlockName[64]{};
    JijiKeyDoorRules rules{};

    bool valid() const {
        auto validName=[](const char* value,std::size_t size){
            const auto* end=static_cast<const char*>(std::memchr(value,0,size));
            if(!end||end==value)return false;
            for(auto* p=value;p!=end;++p)if(*p<32||*p>126)return false;
            for(auto* p=end+1;p!=value+size;++p)if(*p!=0)return false;
            return true;
        };
        const float values[]={rules.boxUpWait,rules.choiceUpWait,rules.declineWait,
            rules.boxDownWait,rules.yesWait,rules.activateWait,rules.anticWait,
            rules.slamEndWait,rules.anticTweenTime,rules.anticDeltaY,
            rules.slamTweenTime,rules.slamDeltaY};
        for(float value:values)if(!std::isfinite(value)||std::abs(value)>100000)return false;
        return validName(keyName,sizeof(keyName))&&validName(unlockName,sizeof(unlockName))&&
            rules.boxUpWait>=0&&rules.choiceUpWait>=0&&rules.declineWait>=0&&
            rules.boxDownWait>=0&&rules.yesWait>=0&&rules.activateWait>=0&&
            rules.anticWait>=0&&rules.slamEndWait>=0&&rules.anticTweenTime>=0&&
            rules.slamTweenTime>=0&&rules.anticDeltaY<0&&rules.slamDeltaY<0&&
            rules.decrement<0;
    }

    bool load(const char* path){
        auto* file=std::fopen(path,"rb");if(!file)return false;
        WorldKeyDoorRoomData next;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"JJD1",4);
        float* values[]={&next.rules.boxUpWait,&next.rules.choiceUpWait,
            &next.rules.declineWait,&next.rules.boxDownWait,&next.rules.yesWait,
            &next.rules.activateWait,&next.rules.anticWait,&next.rules.slamEndWait,
            &next.rules.anticTweenTime,&next.rules.anticDeltaY,
            &next.rules.slamTweenTime,&next.rules.slamDeltaY};
        for(float* value:values)ok=ok&&read(value,sizeof(*value));
        ok=ok&&read(&next.rules.decrement,sizeof(next.rules.decrement))&&
            read(next.keyName,sizeof(next.keyName))&&read(next.unlockName,sizeof(next.unlockName));
        ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
        if(ok)ok=next.valid();
        if(ok)*this=next;
        return ok;
    }

    bool bind(JijiKeyDoor& door,JijiKeyDoorSaveHooks save) const {
        if(!valid())return false;
        auto ownedRules=rules;
        ownedRules.keyName=keyName;ownedRules.unlockName=unlockName;
        return door.bind(ownedRules,save);
    }
};
static_assert(sizeof(float)==4&&sizeof(int)==4,"JJD1 needs 32-bit float and int");
}
