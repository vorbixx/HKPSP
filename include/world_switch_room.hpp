#pragma once
#include "world_switch_runtime.hpp"
#include "room_visual.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// WSR1 carries authored transforms alongside the separately packed HKA1 bank.
// This is the researched two-gate subset; it does not allocate save flags.
struct WorldSwitchRoomData {
    char scene[32]{},id[32]{};
    TollSwitchRules rules{};
    Rect trigger{};
    std::array<unsigned,2> colliders{};
    std::array<unsigned,3> idleSprites{};
    unsigned playIdle=0;
    std::array<RoomDraw,3> draws{}; // switch, gate 0, gate 1
    bool valid()const {
        auto text=[](const char* s){
            const auto* end=static_cast<const char*>(std::memchr(s,0,32));
            if(!end||end==s)return false;
            for(auto* p=s;p!=end;++p)if(*p<32||*p>126)return false;
            return true;
        };
        auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=100000;};
        if(!text(scene)||!text(id)||!finite(rules.hitWait)||rules.hitWait<0||rules.freezeMoment!=1||
           rules.activatedFrame>10000||playIdle>1||!colliders[0]||!colliders[1]||colliders[0]==colliders[1]||
           !finite(trigger.left)||!finite(trigger.bottom)||!finite(trigger.right)||!finite(trigger.top)||
           trigger.left>=trigger.right||trigger.bottom>=trigger.top)return false;
        for(unsigned i=0;i<draws.size();++i){
            const auto& d=draws[i];if(!d.owner)return false;
            for(unsigned j=0;j<i;++j)if(d.owner==draws[j].owner)return false;
            for(float value:d.values)if(!finite(value))return false;
            for(unsigned c=7;c<=10;++c)if(d.values[c]<0||d.values[c]>1)return false;
            if(d.values[15]!=0)return false; // Only verified normal tk2d material.
        }
        return true;
    }
    bool load(const char* path){
        auto* file=std::fopen(path,"rb");if(!file)return false;
        WorldSwitchRoomData next;char magic[4];
        auto read=[&](void* p,std::size_t n){return std::fread(p,1,n,file)==n;};
        bool ok=read(magic,4)&&!std::memcmp(magic,"WSR1",4)&&read(next.scene,32)&&read(next.id,32)&&
            read(&next.rules.hitWait,4)&&read(&next.rules.freezeMoment,4)&&read(&next.rules.activatedFrame,4)&&
            read(&next.trigger,sizeof(Rect))&&read(next.colliders.data(),8)&&read(next.idleSprites.data(),12)&&read(&next.playIdle,4);
        for(auto& draw:next.draws)ok=ok&&read(&draw.owner,4)&&read(draw.values.data(),64);
        ok=ok&&std::fgetc(file)==EOF&&next.valid();std::fclose(file);
        if(ok)*this=next;
        return ok;
    }
    WorldSwitchBinding binding()const {
        WorldSwitchBinding b;b.rules=rules;b.nailTrigger=trigger;b.colliders=colliders;b.idleSprites=idleSprites;
        b.scene=scene;b.id=id;b.hitClip="Switch Hit";b.idleClip=playIdle?"Switch Idle":nullptr;
        b.gateClips={"Toll Gate Open","Toll Gate Open"};return b;
    }
};
// Reuses RoomRenderer's perspective, tint and sorting passes. Each current pose
// occupies its own sprite slot, so animation never changes unrelated room art.
// Room adapters retain this pair for the full binding lifetime. Load after
// releasing the previous scene's runtime; it borrows these names and sprites.
struct WorldSwitchRoomAssets {
    WorldSwitchRoomData data;
    AnimationBank art;
    bool load(const char* directory){
        if(!directory||!*directory)return false;
        WorldSwitchRoomAssets next;char path[384];
        auto name=[&](const char* file){
            const int length=std::snprintf(path,sizeof(path),"%s/%s",directory,file);
            return length>0&&static_cast<std::size_t>(length)<sizeof(path);
        };
        if(!name("world-switch-room.bin")||!next.data.load(path)||
           !name("world-switch.bin")||!next.art.load(path))return false;
        *this=std::move(next);return true;
    }
};
class WorldSwitchVisuals {
    friend struct SimulationStateHash;
public:
    bool prepare(const WorldSwitchRoomData& data,const AnimationBank& bank){
        if(!data.valid()||!bank.pageCount)return false;
        for(unsigned sprite:data.idleSprites)
            if(sprite>=bank.sprites.size()||bank.sprites[sprite].page>=bank.pageCount)return false;
        WorldSwitchVisuals next;next.artwork_=&bank;next.bank_.atlas.pageCount=bank.pageCount;
        next.bank_.atlas.sprites.resize(3);next.bank_.draws.resize(3);next.bank_.cameraBounds.resize(3);
        for(unsigned i=0;i<3;++i){
            next.authored_[i]=data.draws[i];next.authored_[i].sprite=i;
            next.bank_.atlas.sprites[i]=bank.sprites[data.idleSprites[i]];
        }
        next.bank_.draws.assign(next.authored_.begin(),next.authored_.end());
        std::sort(next.bank_.draws.begin(),next.bank_.draws.end(),[](const RoomDraw& a,const RoomDraw& b){
            if(a.values[11]!=b.values[11])return a.values[11]<b.values[11];
            if(a.values[12]!=b.values[12])return a.values[12]<b.values[12];
            if(a.values[6]!=b.values[6])return a.values[6]>b.values[6];
            return a.owner<b.owner;
        });
        *this=std::move(next);return true;
    }
    void update(const WorldSwitchRuntime& runtime){
        if(bank_.draws.empty())return;
        const SpriteFrame* frames[]={runtime.switchFrame(),runtime.gateFrame(0),runtime.gateFrame(1)};
        for(unsigned i=0;i<3;++i){
            auto& draw=bank_.draws[i];const unsigned part=draw.sprite;
            const auto* frame=runtime.artwork()==artwork_?frames[part]:nullptr;
            draw.values[10]=frame?authored_[part].values[10]:0;
            if(frame)bank_.atlas.sprites[part]=*frame;
            bank_.cameraBounds[i]=roomCameraBounds(bank_.atlas.sprites[part],draw);
        }
    }
    void clear(){*this={};}
    const RoomVisualBank& bank()const{return bank_;}
private:
    const AnimationBank* artwork_=nullptr;
    RoomVisualBank bank_;
    std::array<RoomDraw,3> authored_{};
};
}
