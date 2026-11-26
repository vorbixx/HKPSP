#include "scene.hpp"
#include "world_obstacles.hpp"
#include "graphics_profile.hpp"
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
namespace hk3ds {
bool startupSceneMetadataAvailable(const char* root,const char* scene) {
    if(!root||!validSceneIdentifier(scene))return false;
    if(!std::strcmp(scene,"Tutorial_01"))return true;
    char path[256];
    const int n=std::snprintf(path,sizeof(path),"%s/scenes/%s/scene.bin",root,scene);
    if(n<0||static_cast<std::size_t>(n)>=sizeof(path))return false;
    auto* file=std::fopen(path,"rb");
    if(!file)return false;
    std::fclose(file);
    return true;
}
bool scenePackageAvailable(const char* root,const char* scene,const char* roomPrefix) {
    if(!root||!roomPrefix||!validSceneIdentifier(scene))return false;
    static const char* const blocked[]={
        "Ruins1_31"
    };
    for(const auto* name:blocked){
        const auto* left=scene;const auto* right=name;
        while(*left&&*right&&std::tolower(static_cast<unsigned char>(*left))==std::tolower(static_cast<unsigned char>(*right))){++left;++right;}
        if(!*left&&!*right)return false;
    }
    char directory[512],path[640];
    const bool tutorial=!std::strcmp(scene,"Tutorial_01");
    const int size=std::snprintf(directory,sizeof(directory),tutorial?"%s":"%s/scenes/%s",root,scene);
    if(size<0||size>=int(sizeof(directory)))return false;
    const auto open=[&](const char* leaf){
        const int length=std::snprintf(path,sizeof(path),"%s/%s",directory,leaf);
        return length>=0&&length<int(sizeof(path))?std::fopen(path,"rb"):nullptr;
    };
    char visualName[96];
    const int visualSize=std::snprintf(visualName,sizeof(visualName),"%svisual.bin",roomPrefix);
    if(visualSize<0||visualSize>=int(sizeof(visualName)))return false;
    const char* const files[]={tutorial?"room-rules.bin":"scene.bin","tutorial-room.bin",visualName,tutorial?"room-rules.bin":"scene-lighting.bin"};
    for(const auto* file:files){auto* handle=open(file);if(!handle)return false;std::fclose(handle);}
    auto* visual=open(visualName);if(!visual)return false;
    // Resident pages are native 1024x1024 RGBA4. Admit the selected bank
    // before opening a gate or releasing the current room's textures.
    std::size_t pageLimit=64;
    for(const auto& profile:graphicsProfiles)
        if(!std::strcmp(roomPrefix,profile.roomPrefix))pageLimit=profile.roomTextureBytes/(2u*1024u*1024u);
    char magic[4];std::uint32_t pages=0;
    const bool valid=std::fread(magic,1,4,visual)==4&&
        (!std::memcmp(magic,"HKR1",4)||!std::memcmp(magic,"HKR2",4)||!std::memcmp(magic,"HKR3",4)||!std::memcmp(magic,"HKR4",4))&&
        std::fread(&pages,4,1,visual)==1&&pages>0&&pages<=pageLimit;
    std::fclose(visual);if(!valid)return false;
    for(unsigned index=0;index<pages;++index){
        char leaf[128];const int size=std::snprintf(leaf,sizeof(leaf),"%spage-%u.t3x",roomPrefix,index);
        if(size<0||size>=int(sizeof(leaf)))return false;
        auto* handle=open(leaf);if(!handle)return false;std::fclose(handle);
    }
    return true;
}
void closeUnavailableSceneGates(RoomConfig& rules,const char* root,const char* roomPrefix) {
    for(auto& gate:rules.gates){
        if(!gate.scene[0])continue;
        if(!scenePackageAvailable(root,gate.scene,roomPrefix)){gate.scene[0]=0;gate.unavailable=true;continue;}
        if(!std::strcmp(gate.scene,"Tutorial_01"))continue;
        char path[640];
        const int length=std::snprintf(path,sizeof(path),"%s/scenes/%s/scene.bin",root,gate.scene);
        SceneBank destination;
        if(length<0||length>=int(sizeof(path))||!destination.load(path)||!destination.enter(gate.entry)){gate.scene[0]=0;gate.unavailable=true;}
    }
}
void addUnavailableGateBarriers(const RoomConfig& rules,TerrainBank& terrain) {
    unsigned id=~0u;
    for(const auto& gate:rules.gates){
        auto bounds=gate.trigger;
        // A closed interactive door only stops accepting Up; the hero walks past it as in the
        // original. A wall there blocked the street at every Town door whose room the package
        // lacks (user report "an obstacle at every door"; Grimm tent door at x 71-73).
        if(!gate.unavailable||interactiveDoorGate(gate)||bounds.left>=bounds.right||bounds.bottom>=bounds.top)continue;
        const auto spawn=bodyBounds(rules.initial);
        if(!std::strncmp(gate.name,"top",3)&&rules.entrySpeed>0&&spawn.left<bounds.right&&spawn.right>bounds.left&&spawn.bottom>=bounds.top){
            // A scripted top entry starts outside its destination trigger.
            // Keep the ceiling beyond that spawn so a closed return route
            // cannot turn the opening fall into a landing on the gate.
            const float offset=spawn.top+.1f-bounds.bottom;
            bounds.bottom+=offset;bounds.top+=offset;
        }
        while(id&&std::any_of(terrain.shapes.begin(),terrain.shapes.end(),[id](const TerrainShape& shape){return shape.id==id;}))--id;
        TerrainShape shape;shape.id=id--;shape.kind=0;shape.first=unsigned(terrain.vertices.size());shape.count=4;
        shape.bounds=bounds;shape.queryBoundsValid=true;
        terrain.vertices.push_back({bounds.left,bounds.bottom});terrain.vertices.push_back({bounds.right,bounds.bottom});
        terrain.vertices.push_back({bounds.right,bounds.top});terrain.vertices.push_back({bounds.left,bounds.top});
        terrain.shapes.push_back(shape);
    }
}
namespace {
bool sidecarPath(char* output,std::size_t size,const char* path,const char* leaf)
{
    const char* slash=std::strrchr(path,'/');
    const char* backslash=std::strrchr(path,'\\');
    if(backslash&&(!slash||backslash>slash))slash=backslash;
    const int prefix=slash?int(slash-path+1):0;
    const int written=std::snprintf(output,size,"%.*s%s",prefix,path,leaf);
    return written>=0&&written<int(size);
}
bool goamPrefab(const char* name){
    if(!name)return false;
    if(!std::strcmp(name,"Worm"))return true;
    if(std::strncmp(name,"Worm ",5))return false;
    const char* p=name+5;const bool parenthesized=*p=='(';if(parenthesized)++p;
    if(*p<'0'||*p>'9')return false;
    while(*p>='0'&&*p<='9')++p;
    return parenthesized?(*p==')'&&p[1]==0):*p==0;
}
bool loadBattle(const char* scenePath,SceneBattleData& battle){
    char path[512];if(!sidecarPath(path,sizeof(path),scenePath,"battle-gates.bin"))return false;
    auto* f=std::fopen(path,"rb");if(!f)return true;
    unsigned ng=0,ne=0;char magic[4];
    bool ok=std::fread(magic,4,1,f)==1&&(!std::memcmp(magic,"HKG1",4)||!std::memcmp(magic,"HKG2",4))&&
        std::fread(&ng,4,1,f)==1&&ng<=32&&std::fread(&ne,4,1,f)==1&&ne<=16;
    auto ids=[&](std::vector<unsigned>& ids,unsigned limit){unsigned n=0;
        if(std::fread(&n,4,1,f)!=1||n>limit)return false;
        ids.resize(n);return !n||std::fread(ids.data(),4,n,f)==n;};
    if(ok){battle.gates.resize(ng);battle.encounters.resize(ne);}
    for(auto& gate:battle.gates)ok=ok&&std::fread(&gate.collider,4,1,f)==1&&gate.collider&&ids(gate.owners,256);
    for(auto& e:battle.encounters){
        ok=ok&&std::fread(&e.key,8,1,f)==1&&std::fread(&e.trigger,16,1,f)==1&&std::fread(&e.endDelay,4,1,f)==1&&ids(e.enemies,64);
        ok=ok&&std::isfinite(e.trigger.left)&&std::isfinite(e.trigger.right)&&std::isfinite(e.trigger.bottom)&&std::isfinite(e.trigger.top)&&
            e.trigger.left<e.trigger.right&&e.trigger.bottom<e.trigger.top&&std::isfinite(e.endDelay)&&e.endDelay>=0&&e.endDelay<=60;
        if(ok&&!std::memcmp(magic,"HKG2",4)){
            unsigned count=0;ok=std::fread(&count,4,1,f)==1&&count<=16;
            if(ok)e.waves.resize(count);
            for(auto& wave:e.waves){
                unsigned n=0;
                ok=ok&&std::fread(&wave.delay,4,1,f)==1&&std::fread(&wave.timeout,4,1,f)==1&&
                    std::fread(&wave.healthOwner,4,1,f)==1&&std::fread(&wave.healthBelow,4,1,f)==1&&
                    std::fread(&n,4,1,f)==1&&n>0&&n<=64&&std::isfinite(wave.delay)&&wave.delay>=0&&wave.delay<=60&&
                    std::isfinite(wave.timeout)&&wave.timeout>=0&&wave.timeout<=60&&wave.healthBelow>=0;
                if(!ok)break;
                wave.spawns.resize(n);
                for(auto& spawn:wave.spawns){unsigned reposition=0;
                    ok=ok&&std::fread(&spawn.owner,4,1,f)==1&&std::fread(&spawn.position,8,1,f)==1&&
                        std::fread(&reposition,4,1,f)==1&&reposition<=1&&spawn.owner&&
                        std::isfinite(spawn.position.x)&&std::isfinite(spawn.position.y)&&
                        std::find(e.enemies.begin(),e.enemies.end(),spawn.owner)!=e.enemies.end();
                    spawn.reposition=reposition!=0;
                }
            }
        }
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);return ok;
}
bool loadBossRoom(const char* scenePath,SceneBossRoom& room){
    char path[512];if(!sidecarPath(path,sizeof(path),scenePath,"boss-room.bin"))return false;
    auto* f=std::fopen(path,"rb");if(!f)return true;
    const auto rect=[&](Rect& r){return std::fread(&r,sizeof(r),1,f)==1&&
        std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&std::isfinite(r.top)&&
        r.left<r.right&&r.bottom<r.top;};
    const auto ids=[&](std::vector<unsigned>& v,unsigned limit){unsigned n=0;
        if(std::fread(&n,4,1,f)!=1||n>limit)return false;
        v.resize(n);return n==0||std::fread(v.data(),4,n,f)==n;};
    char magic[4];unsigned n=0;
    bool ok=std::fread(magic,4,1,f)==1&&
        (!std::memcmp(magic,"HKB1",4)||!std::memcmp(magic,"HKB2",4))&&rect(room.arena)&&rect(room.trigger)&&
        std::fread(&n,4,1,f)==1&&n>0&&n<=2;
    if(ok)room.gates.resize(n);
    for(auto& g:room.gates)ok=ok&&std::fread(&g.collider,4,1,f)==1&&rect(g.bounds)&&ids(g.owners,128);
    ok=ok&&ids(room.floorColliders,64)&&ids(room.floorOwners,256);
    if(ok&&!std::memcmp(magic,"HKB2",4))ok=ids(room.brokenFloorOwners,256);
    ok=ok&&std::fgetc(f)==EOF;
    std::fclose(f);room.authored=ok;return ok;
}
}
bool validSceneIdentifier(const char* s,bool empty) {
    if(!s||(!empty&&!s[0]))return false;
    for(unsigned i=0;i<32;++i){const char c=s[i];if(!c)return true;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;}
    return false;
}
bool loadSourceSceneRemasks(FILE* remasks,std::vector<RoomRemask>& masks)
{
    bool ok=false;std::vector<RoomRemask> parsed;
    char tag[4];unsigned total=0;
    ok=std::fread(tag,4,1,remasks)==1&&(!std::memcmp(tag,"HKS1",4)||!std::memcmp(tag,"HKS2",4)||!std::memcmp(tag,"HKS3",4)||!std::memcmp(tag,"HKS4",4)||!std::memcmp(tag,"HKS5",4))&&
        std::fread(&total,4,1,remasks)==1&&total<=RoomConfig::maxRemasks;
    const bool flexibleFormat=ok&&!std::memcmp(tag,"HKS5",4);
    const bool gatedFormat=ok&&(!std::memcmp(tag,"HKS4",4)||flexibleFormat);
    const bool multipartFormat=ok&&(!std::memcmp(tag,"HKS3",4)||gatedFormat);
    const bool polygonFormat=ok&&std::memcmp(tag,"HKS1",4);
    if(ok)parsed.resize(total);
    for(auto& mask:parsed){
        unsigned owners=0,flags=0;
        ok=ok&&std::fread(&mask.trigger,sizeof(Rect),1,remasks)==1&&
            std::fread(&owners,4,1,remasks)==1&&owners>0&&owners<=128;
        if(ok)mask.owners.resize(owners);
        if(ok&&owners)ok=std::fread(mask.owners.data(),4,owners,remasks)==owners;
        for(unsigned i=0;i<mask.owners.size();++i)
            ok=ok&&mask.owners[i]>0&&(!i||mask.owners[i]>mask.owners[i-1]);
        ok=ok&&std::fread(&mask.seconds,4,1,remasks)==1&&std::fread(&flags,4,1,remasks)==1&&
            std::fread(&mask.startupPause,4,1,remasks)==1&&std::fread(&mask.startupFade,4,1,remasks)==1;
        mask.opaqueInside=(flags&1)!=0;mask.startupHeroPositioned=(flags&2)!=0;
        ok=ok&&!(flags&~3u)&&mask.seconds>0&&mask.seconds<=10&&mask.startupPause>=0&&mask.startupPause<=10&&
            mask.startupFade>=0&&mask.startupFade<=10&&mask.trigger.left<mask.trigger.right&&
            mask.trigger.bottom<mask.trigger.top&&std::isfinite(mask.trigger.left)&&
            std::isfinite(mask.trigger.right)&&std::isfinite(mask.trigger.bottom)&&std::isfinite(mask.trigger.top);
        if(polygonFormat){
            unsigned geometry=0;ok=ok&&std::fread(&geometry,4,1,remasks)==1&&
                (multipartFormat?geometry<=16:(geometry==0||geometry==4));
            if(flexibleFormat){
                for(unsigned contour=0;ok&&contour<geometry;++contour){
                    unsigned points=0;ok=std::fread(&points,4,1,remasks)==1&&points>=3&&points<=128-mask.polygon.size();
                    if(!ok)break;
                    const auto first=mask.polygon.size();mask.polygon.resize(first+points);
                    ok=std::fread(mask.polygon.data()+first,sizeof(Vec2),points,remasks)==points;
                    if(ok)mask.polygonEnds.push_back(static_cast<unsigned>(mask.polygon.size()));
                }
            }else{
                const unsigned points=multipartFormat?geometry*4:geometry;
                if(ok)mask.polygon.resize(points);
                if(ok&&points)ok=std::fread(mask.polygon.data(),sizeof(Vec2),points,remasks)==points;
                if(ok&&multipartFormat)for(unsigned i=1;i<=geometry;++i)mask.polygonEnds.push_back(i*4);
            }
            for(const auto& point:mask.polygon)
                ok=ok&&std::isfinite(point.x)&&std::isfinite(point.y)&&point.x>=mask.trigger.left&&
                    point.x<=mask.trigger.right&&point.y>=mask.trigger.bottom&&point.y<=mask.trigger.top;
        }
        if(gatedFormat)ok=ok&&std::fread(&mask.requiredCharm,4,1,remasks)==1&&mask.requiredCharm<=32&&
            (!mask.requiredCharm||(mask.opaqueInside&&!mask.startupHeroPositioned&&mask.startupPause==0&&mask.startupFade==0));
    }
    ok=ok&&std::fgetc(remasks)==EOF;if(ok)masks=std::move(parsed);return ok;
}

bool loadSceneRemasks(const char* scenePath,std::vector<RoomRemask>& masks)
{
    char path[512];if(!sidecarPath(path,sizeof(path),scenePath,"scene-remasks.bin"))return false;
    auto* f=std::fopen(path,"rb");if(!f)return errno==ENOENT;
    char magic[4]{};unsigned count=0;
    bool ok=std::fread(magic,4,1,f)==1;
    if(ok&&(!std::memcmp(magic,"HKS1",4)||!std::memcmp(magic,"HKS2",4)||
            !std::memcmp(magic,"HKS3",4)||!std::memcmp(magic,"HKS4",4)||!std::memcmp(magic,"HKS5",4))){
        std::rewind(f);ok=loadSourceSceneRemasks(f,masks);std::fclose(f);return ok;
    }
    const bool inverse=ok&&!std::memcmp(magic,"HMR1",4);
    const bool normal=ok&&!std::memcmp(magic,"HRM1",4);
    const bool combined=ok&&!std::memcmp(magic,"HMR2",4);
    ok=ok&&(inverse||normal||combined)&&std::fread(&count,4,1,f)==1&&count<=16;
    std::vector<RoomRemask> next;if(ok)next.resize(count);
    for(auto& mask:next){
        float v[7]{};unsigned owners=0,points=0,flags=inverse?3u:0u;
        const unsigned values=normal?6:7;
        ok=ok&&std::fread(v,4,values,f)==values;
        if(combined)ok=ok&&std::fread(&flags,4,1,f)==1&&flags<=3;
        ok=ok&&std::fread(&owners,4,1,f)==1&&
            std::fread(&points,4,1,f)==1&&owners>0&&owners<=128&&
            (points==0||(points>=3&&points<=128));
        for(float value:v)ok=ok&&std::isfinite(value);
        ok=ok&&v[0]<v[2]&&v[1]<v[3]&&v[4]>=0&&v[4]<=60&&
            v[5]>=0&&v[5]<=60&&v[6]>=0&&v[6]<=60;
        if(!ok)break;
        mask.trigger={v[0],v[1],v[2],v[3]};mask.seconds=v[4];
        mask.startupPause=v[5];mask.startupFade=v[6];
        mask.opaqueInside=flags&1u;mask.startupHeroPositioned=flags&2u;
        mask.owners.resize(owners);mask.polygon.resize(points);
        for(auto& owner:mask.owners)ok=ok&&std::fread(&owner,4,1,f)==1&&owner!=0;
        for(auto& point:mask.polygon)ok=ok&&std::fread(&point,4,2,f)==2&&
            std::isfinite(point.x)&&std::isfinite(point.y)&&
            point.x>=v[0]&&point.x<=v[2]&&point.y>=v[1]&&point.y<=v[3];
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    if(ok)masks=std::move(next);
    return ok;
}

bool validGateIdentifier(const char* name,bool empty) {
    if(!name||(!empty&&!name[0]))return false;
    for(unsigned i=0;i<32;++i){const auto c=static_cast<unsigned char>(name[i]);
        if(!c)return true;
        if(c<32||c>126)return false;
    }
    return false;
}
bool SceneBank::load(const char* path) {
    auto* f=std::fopen(path,"rb");if(!f)return false;
    SceneBank next;char magic[4];unsigned count=0;
    bool ok=std::fread(magic,4,1,f)==1&&(!std::memcmp(magic,"HKS1",4)||!std::memcmp(magic,"HKS2",4)||!std::memcmp(magic,"HKS3",4))&&std::fread(&count,4,1,f)==1&&count<=32&&
        std::fread(&next.rules.cameraLimits,sizeof(Rect),1,f)==1;
    const auto& r=next.rules.cameraLimits;
    ok=ok&&std::isfinite(r.left)&&std::isfinite(r.right)&&std::isfinite(r.top)&&std::isfinite(r.bottom)&&r.left<=r.right&&r.bottom<=r.top;
    if(ok)next.rules.gates.resize(count);
    for(auto& g:next.rules.gates){ok=ok&&std::fread(&g.trigger,sizeof(Rect),1,f)==1&&std::fread(&g.offset,sizeof(Vec2),1,f)==1&&
        std::fread(&g.delay,4,1,f)==1&&std::fread(g.name,32,1,f)==1&&std::fread(g.scene,32,1,f)==1&&std::fread(g.entry,32,1,f)==1;
        ok=ok&&validGateIdentifier(g.name)&&validSceneIdentifier(g.scene,true)&&validGateIdentifier(g.entry,true)&&
            std::isfinite(g.trigger.left)&&std::isfinite(g.trigger.right)&&std::isfinite(g.trigger.bottom)&&std::isfinite(g.trigger.top)&&
            g.trigger.left<=g.trigger.right&&g.trigger.bottom<=g.trigger.top&&std::isfinite(g.offset.x)&&std::isfinite(g.offset.y)&&std::isfinite(g.delay)&&g.delay>=0;}
    if(ok&&(!std::memcmp(magic,"HKS2",4)||!std::memcmp(magic,"HKS3",4))){
        unsigned spawns=0;ok=std::fread(&spawns,4,1,f)==1&&spawns<=128;
        if(ok)next.enemies.resize(spawns);
        for(auto& e:next.enemies){
            unsigned active=0;
            ok=ok&&std::fread(&e.owner,4,1,f)==1&&std::fread(e.prefab,64,1,f)==1&&
                std::fread(&e.position,sizeof(Vec2),1,f)==1&&std::fread(&e.scale,sizeof(Vec2),1,f)==1&&std::fread(&active,4,1,f)==1;
            ok=ok&&std::memchr(e.prefab,0,64)&&e.prefab[0]&&active<=1&&
                std::isfinite(e.position.x)&&std::isfinite(e.position.y)&&std::isfinite(e.scale.x)&&std::isfinite(e.scale.y)&&e.scale.x!=0&&e.scale.y!=0;
            e.active=active!=0;
        }
    }
    if(ok&&!std::memcmp(magic,"HKS3",4)){
        unsigned waterCount=0;ok=std::fread(&waterCount,4,1,f)==1&&waterCount<=32;
        if(ok)next.rules.water.resize(waterCount);
        for(auto& w:next.rules.water)ok=ok&&std::fread(&w,sizeof(w),1,f)==1&&
            std::isfinite(w.left)&&std::isfinite(w.right)&&std::isfinite(w.bottom)&&std::isfinite(w.top)&&
            w.left<w.right&&w.bottom<w.top;
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);if(!ok)return false;
    char doorPath[512];if(!sidecarPath(doorPath,sizeof(doorPath),path,"doors.bin"))return false;
    {
        if(auto* doors=std::fopen(doorPath,"rb")){
            char tag[4];unsigned total=0;
            ok=std::fread(tag,4,1,doors)==1&&(!std::memcmp(tag,"HKD1",4)||!std::memcmp(tag,"HKD2",4)||!std::memcmp(tag,"HKD3",4))&&
                std::fread(&total,4,1,doors)==1&&total==next.rules.gates.size();
            const bool version3=ok&&!std::memcmp(tag,"HKD3",4);
            const bool startup=version3||(ok&&!std::memcmp(tag,"HKD2",4));
            for(auto& g:next.rules.gates){
                ok=ok&&std::fread(&g.flags,4,1,doors)==1&&std::fread(&g.position,sizeof(Vec2),1,doors)==1&&
                    std::fread(&g.promptPosition,sizeof(Vec2),1,doors)==1&&std::fread(g.prompt,32,1,doors)==1;
                ok=ok&&!(g.flags&~(version3?255u:127u))&&(!(g.flags&RoomGate::JustFade)||(g.flags&RoomGate::Interact))&&
                    std::memchr(g.prompt,0,32)&&std::isfinite(g.position.x)&&
                    std::isfinite(g.position.y)&&std::isfinite(g.promptPosition.x)&&std::isfinite(g.promptPosition.y);
                if(startup){
                    auto& s=g.startup;
                    ok=ok&&std::fread(&s.flags,4,1,doors)==1&&std::fread(&s.pause,4,1,doors)==1&&
                        std::fread(&s.normal,4,1,doors)==1&&std::fread(&s.visited,4,1,doors)==1&&
                        std::fread(&s.unvisited,4,1,doors)==1;
                    ok=ok&&!(s.flags&~7u)&&(!s.flags||(s.flags&RoomDoorStartup::Enabled))&&
                        (!s.flags||(g.flags&RoomGate::Interact))&&
                        std::isfinite(s.pause)&&s.pause>=0&&std::isfinite(s.normal)&&s.normal>=0&&
                        std::isfinite(s.visited)&&s.visited>=0&&std::isfinite(s.unvisited)&&s.unvisited>=0&&
                        ((s.flags&RoomDoorStartup::WaitForScene)||s.pause==0)&&
                        ((s.flags&RoomDoorStartup::CrossroadsAscent)||(s.visited==0&&s.unvisited==0))&&
                        (s.flags||(s.pause==0&&s.normal==0&&s.visited==0&&s.unvisited==0));
                }
                if(version3)ok=ok&&std::fread(&g.entryPause,4,1,doors)==1&&
                    std::isfinite(g.entryPause)&&g.entryPause>=0;
            }
            ok=ok&&std::fgetc(doors)==EOF;std::fclose(doors);if(!ok)return false;
        }
    }
    char rewardPath[512];if(!sidecarPath(rewardPath,sizeof(rewardPath),path,"scene-enemy-rewards.bin"))return false;
    if(auto* rewards=std::fopen(rewardPath,"rb")){
        char tag[4];unsigned total=0;
        ok=std::fread(tag,4,1,rewards)==1&&!std::memcmp(tag,"HKE1",4)&&
            std::fread(&total,4,1,rewards)==1&&total<=next.enemies.size();
        for(unsigned i=0;i<total&&ok;++i){
            std::uint32_t owner=0,flags=0;EnemyRewardConfig config;
            ok=std::fread(&owner,4,1,rewards)==1&&std::fread(&config.small,4,1,rewards)==1&&
                std::fread(&config.medium,4,1,rewards)==1&&std::fread(&config.large,4,1,rewards)==1&&
                std::fread(&config.origin,sizeof(Vec2),1,rewards)==1&&std::fread(&flags,4,1,rewards)==1;
            ok=ok&&owner&&!(flags&~7u)&&(!(flags&2u)||(flags&1u))&&
                config.small<=GeoBank::capacity&&config.medium<=GeoBank::capacity-config.small&&
                config.large<=GeoBank::capacity-config.small-config.medium&&
                std::isfinite(config.origin.x)&&std::isfinite(config.origin.y);
            SceneSpawn* spawn=nullptr;
            for(auto& e:next.enemies)if(e.owner==owner){if(spawn){ok=false;break;}spawn=&e;}
            ok=ok&&spawn&&!spawn->hasReward;
            if(ok){config.persistent=flags&1u;config.semiPersistent=flags&2u;config.megaFling=flags&4u;spawn->reward=config;spawn->hasReward=true;}
        }
        ok=ok&&std::fgetc(rewards)==EOF;std::fclose(rewards);if(!ok)return false;
    }
    char goamPath[512];if(!sidecarPath(goamPath,sizeof(goamPath),path,"scene-goam-visuals.bin"))return false;
    if(auto* goams=std::fopen(goamPath,"rb")){
        char tag[4];unsigned total=0;
        ok=std::fread(tag,4,1,goams)==1&&!std::memcmp(tag,"HGV1",4)&&
            std::fread(&total,4,1,goams)==1&&total<=next.enemies.size();
        for(unsigned i=0;i<total&&ok;++i){
            std::uint32_t owner=0;float rotation=0;SceneSpawn* spawn=nullptr;
            ok=std::fread(&owner,4,1,goams)==1&&std::fread(&rotation,4,1,goams)==1&&
                owner&&std::isfinite(rotation)&&std::abs(rotation)<=6.284f;
            for(auto& e:next.enemies)if(e.owner==owner){if(spawn){ok=false;break;}spawn=&e;}
            ok=ok&&spawn&&!spawn->goamRotationSet&&goamPrefab(spawn->prefab);
            if(ok){spawn->goamRotation=rotation;spawn->goamRotationSet=true;}
        }
        ok=ok&&std::fgetc(goams)==EOF;std::fclose(goams);if(!ok)return false;
    }
    next.rules.dampTime=.15f;next.rules.entrySpeed=20;next.rules.deathWait=1;next.rules.wakeWait=.2f;
    // Optional sidecar keeps HKS1 gate packages and older converted rooms readable.
    char sidecar[512];if(!sidecarPath(sidecar,sizeof(sidecar),path,"rest.bin"))return false;
    {
        char ambientPath[512];if(!sidecarPath(ambientPath,sizeof(ambientPath),path,"scene-ambient.bin"))return false;
        if(auto* ambient=std::fopen(ambientPath,"rb")){
            char tag[4];unsigned bank=0;
            ok=std::fread(tag,4,1,ambient)==1&&std::fread(&bank,4,1,ambient)==1;
            const bool legacy=ok&&!std::memcmp(tag,"HAP1",4);
            ok=ok&&(legacy?bank<=1:(!std::memcmp(tag,"HAP2",4)&&
                (bank==0||bank==5||bank==9||bank==10||bank==25)))&&std::fgetc(ambient)==EOF;
            std::fclose(ambient);if(!ok)return false;
            next.ambientZone=legacy?(bank?5:0):bank;
            next.defaultAmbient=next.ambientZone!=0;
        }
    }
    {
        if(auto* rests=std::fopen(sidecar,"rb")){
            char tag[4];unsigned total=0;
            ok=std::fread(tag,4,1,rests)==1&&!std::memcmp(tag,"HKR1",4)&&std::fread(&total,4,1,rests)==1&&total<=32;
            if(ok)next.rules.restPoints.resize(total);
            for(auto& p:next.rules.restPoints)ok=ok&&std::fread(&p,sizeof(p),1,rests)==1&&p.kind<=1&&p.station<stagStationCount&&
                std::isfinite(p.position.x)&&std::isfinite(p.position.y)&&std::abs(p.position.x)<10000&&std::abs(p.position.y)<10000;
            ok=ok&&std::fgetc(rests)==EOF;std::fclose(rests);if(!ok)return false;
        }
    }
    const char* directory=std::strrchr(path,'/');
    const char* backslash=std::strrchr(path,'\\');
    if(backslash&&(!directory||backslash>directory))directory=backslash;
    const int prefix=directory?int(directory-path+1):0;
    if(std::snprintf(sidecar,sizeof(sidecar),"%.*sworld-obstacles.bin",prefix,path)>=int(sizeof(sidecar))||
       !loadWorldObstacles(sidecar,next.rules))return false;
    if(std::snprintf(sidecar,sizeof(sidecar),"%.*sgate-inputs.bin",prefix,path)>=int(sizeof(sidecar))||
       !loadWorldGateInputs(sidecar,next.rules))return false;
    if(!sidecarPath(sidecar,sizeof(sidecar),path,"stag.bin"))return false;
    if(auto* stag=std::fopen(sidecar,"rb")){
        char tag[4];unsigned count=0;
        ok=std::fread(tag,4,1,stag)==1&&!std::memcmp(tag,"HSG2",4)&&std::fread(&count,4,1,stag)==1&&count<=1;
        auto& c=next.rules.stag;
        if(ok&&count){
            c.present=true;
            ok=std::fread(&c.station,4,1,stag)==1&&std::fread(&c.price,4,1,stag)==1&&c.station<stagStationCount&&c.price<=10000;
            Vec2* vectors[]={&c.position,&c.scale,&c.bell,&c.toll,&c.bellScale,&c.standScale,&c.arrival,&c.dust};
            for(auto* v:vectors)ok=ok&&std::fread(v,sizeof(*v),1,stag)==1&&std::isfinite(v->x)&&std::isfinite(v->y)&&std::abs(v->x)<10000&&std::abs(v->y)<10000;
            ok=ok&&std::abs(c.scale.x)>.001f&&std::abs(c.scale.y)>.001f;
            c.startX=c.position.x;
        }
        ok=ok&&std::fgetc(stag)==EOF;std::fclose(stag);if(!ok)return false;
    }
    char activationPath[512];
    if(!sidecarPath(activationPath,sizeof(activationPath),path,"activation.bin")||!next.rules.activation.load(activationPath))return false;
    for(const auto& target:next.rules.activation.targets)
        if(target.kind==SceneActivationTarget::Gate&&target.id>=next.rules.gates.size())return false;
    if(!sidecarPath(sidecar,sizeof(sidecar),path,"scene-camera.bin"))return false;
    {
        if(auto* camera=std::fopen(sidecar,"rb")){
            char tag[4];unsigned total=0;
            ok=std::fread(tag,4,1,camera)==1&&(!std::memcmp(tag,"HCL1",4)||!std::memcmp(tag,"HCL2",4))&&
                std::fread(&total,4,1,camera)==1&&total<=64;
            const bool owners=ok&&!std::memcmp(tag,"HCL2",4);
            if(ok)next.rules.locks.resize(total);
            for(auto& l:next.rules.locks){
                ok=ok&&std::fread(&l.trigger,sizeof(Rect),1,camera)==1&&
                    std::fread(&l.limits,sizeof(Rect),1,camera)==1&&
                    std::fread(&l.flags,4,1,camera)==1;
                if(owners)ok=ok&&std::fread(&l.owner,4,1,camera)==1;
                const auto& t=l.trigger;const auto& b=l.limits;
                ok=ok&&std::isfinite(t.left)&&std::isfinite(t.bottom)&&std::isfinite(t.right)&&std::isfinite(t.top)&&
                    std::isfinite(b.left)&&std::isfinite(b.bottom)&&std::isfinite(b.right)&&std::isfinite(b.top)&&
                    t.left<t.right&&t.bottom<t.top&&b.left<=b.right&&b.bottom<=b.top&&!(l.flags&~7u);
            }
            ok=ok&&std::fgetc(camera)==EOF;std::fclose(camera);if(!ok)return false;
        }
    }
    char acidPath[512];if(!sidecarPath(acidPath,sizeof(acidPath),path,"acid.bin"))return false;
    if(auto* acid=std::fopen(acidPath,"rb")){
        char tag[4];unsigned hazards=0,surfaces=0;
        ok=std::fread(tag,4,1,acid)==1&&!std::memcmp(tag,"HKA1",4)&&
            std::fread(&hazards,4,1,acid)==1&&std::fread(&surfaces,4,1,acid)==1&&hazards<=64&&surfaces<=16;
        const auto validRect=[](Rect r){return std::isfinite(r.left)&&std::isfinite(r.bottom)&&
            std::isfinite(r.right)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;};
        const auto previousHazards=next.rules.hazards.size();
        if(ok){next.rules.hazards.resize(previousHazards+hazards);next.rules.acidWater.resize(surfaces);}
        for(std::size_t index=previousHazards;index<next.rules.hazards.size();++index){auto& h=next.rules.hazards[index];ok=ok&&std::fread(&h,sizeof(h),1,acid)==1&&
            validRect(h.trigger)&&h.type==3&&h.damage<=100;
        }
        for(auto& r:next.rules.acidWater)ok=ok&&std::fread(&r,sizeof(r),1,acid)==1&&validRect(r);
        ok=ok&&std::fgetc(acid)==EOF;std::fclose(acid);if(!ok)return false;
    }
    if(!loadSceneRemasks(path,next.rules.remasks))return false;
    if(!loadBossRoom(path,next.bossRoom))return false;
    if(!loadBattle(path,next.battle))return false;
    *this=std::move(next);return true;
}
bool SceneBank::enter(const char* name,const Vec2* checkpoint) {
    if(!std::strcmp(name,"bench")&&checkpoint){
        if(!std::isfinite(checkpoint->x)||!std::isfinite(checkpoint->y))return false;
        rules.initial=rules.retry=rules.initialHazard=*checkpoint;return true;
    }
    if(!std::strcmp(name,"bench")||!std::strcmp(name,"stag")){
        for(const auto& p:rules.restPoints)if(p.kind==unsigned(!std::strcmp(name,"stag"))){rules.initial=rules.retry=rules.initialHazard=p.position;return true;}
        return false;
    }
    for(const auto& g:rules.gates)if(!std::strcmp(name,g.name)){
        Vec2 p{(g.trigger.left+g.trigger.right)*.5f,(g.trigger.bottom+g.trigger.top)*.5f};
        if(!std::strncmp(name,"left",4)){p.x=g.trigger.right+1.2f;rules.initialRight=true;}
        else if(!std::strncmp(name,"right",5)){p.x=g.trigger.left-1.2f;rules.initialRight=false;}
        else if(!std::strncmp(name,"top",3))p.y=g.trigger.bottom-1.5f;
        else p.y=g.trigger.top+1.5f;
        p.x+=g.offset.x;p.y+=g.offset.y;
        // Entry offsets can point outside the room (top1 in Crossroads_01).
        // Without the original entry coroutine, keep the hero on the inward
        // side so falling through the gate cannot immediately send it back.
        if(!std::strncmp(name,"left",4))p.x=std::max(p.x,g.trigger.right+1.2f);
        else if(!std::strncmp(name,"right",5))p.x=std::min(p.x,g.trigger.left-1.2f);
        else if(!std::strncmp(name,"top",3))p.y=std::min(p.y,g.trigger.bottom-1.5f);
        else p.y=std::max(p.y,g.trigger.top+1.5f);
        if(g.flags&RoomGate::Door)p={g.position.x+g.offset.x,g.position.y+g.offset.y};
        if(g.flags&RoomGate::FaceRight)rules.initialRight=true;
        if(g.flags&RoomGate::FaceLeft)rules.initialRight=false;
        rules.initial=rules.retry=rules.initialHazard=p;
        rules.retryRight=rules.initialHazardRight=rules.initialRight;return true;
    }return false;
}
}
