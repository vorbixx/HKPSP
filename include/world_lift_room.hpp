#pragma once
#include "world_lifts.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// MLF1 is exactly 88 bytes: versioned magic, five LE float32 rules, NUL key[64].
// Keep this object alive and stable while MineExitLift borrows its save key.
struct WorldLiftRoomData {
    MineLiftRules rules{};
    char key[64]{};
    bool valid()const {
        const auto* end=static_cast<const char*>(std::memchr(key,0,sizeof(key)));
        if(!end||end==key)return false;
        for(auto* p=key;p!=end;++p)if(*p<32||*p>126)return false;
        auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=100000;};
        return finite(rules.bottom)&&finite(rules.top)&&finite(rules.speed)&&
            finite(rules.departureWait)&&finite(rules.arrivalWait)&&rules.bottom<rules.top&&
            rules.speed>0&&rules.departureWait>=0&&rules.arrivalWait>=0;
    }
    bool load(const char* path){
        auto* file=std::fopen(path,"rb");if(!file)return false;
        WorldLiftRoomData next;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"MLF1",4)&&
            read(&next.rules.bottom,4)&&read(&next.rules.top,4)&&read(&next.rules.speed,4)&&
            read(&next.rules.departureWait,4)&&read(&next.rules.arrivalWait,4)&&read(next.key,64);
        ok=ok&&std::fgetc(file)==EOF&&next.valid();std::fclose(file);
        if(ok)*this=next;
        return ok;
    }
    bool bind(MineExitLift& lift,LiftPlayerBoolHooks save,float initialY)const {
        return valid()&&lift.bind(rules,save,key,initialY);
    }
};
}
