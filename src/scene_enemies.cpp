#include "scene_enemies.hpp"
#include "broken_vessel_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
// Flying Sentry Nail "Start Idle" per placement (crossroads-scenes dumps, build 22529139):
// false (wall rest until woken) for these owners, true for every other Ruins Flying Sentry.
bool flyingSentryStartIdle(const char* scene,std::uint32_t owner){
    static constexpr struct {const char* scene;std::uint32_t owner;} wall[]={{"Ruins1_05",215},{"Ruins1_05",30},
        {"Ruins2_01",845},{"Ruins2_03b",628},{"Abyss_01",927},{"Waterways_13",1602},{"Waterways_13",766}};
    for(const auto& w:wall)if(owner==w.owner&&!std::strcmp(scene,w.scene))return false;
    return true;
}
// Shroom Brawler "Facing Right" per placement (crossroads-scenes dumps, build 22529139).
bool brawlerFacingRight(const char* scene,std::uint32_t owner){
    return (owner==76&&!std::strcmp(scene,"Fungus2_05"))||(owner==2678&&!std::strcmp(scene,"Fungus2_30"));
}
// Worm Control "Start Down" false per placement (crossroads-scenes dumps, build 22529139).
bool goamStartDown(const char* scene,std::uint32_t owner){
    static constexpr struct {const char* scene;std::uint32_t owner;} up[]={{"Crossroads_12",596},{"Crossroads_13",120},
        {"Crossroads_13",437},{"Crossroads_13",527},{"Crossroads_13",588},{"Crossroads_13",685},{"Crossroads_42",1285},
        {"Crossroads_42",199},{"Crossroads_42",539},{"Crossroads_42",872},{"Deepnest_44",1113},{"Deepnest_44",2100},
        {"Deepnest_44",79},{"Deepnest_44",931}};
    for(const auto& u:up)if(owner==u.owner&&!std::strcmp(scene,u.scene))return false;
    return true;
}
bool prefab(const char* name,const char* base){
    const auto n=std::strlen(base);if(std::strncmp(name,base,n))return false;
    if(!name[n])return true;
    const char* p=name+n;if(*p++!=' ')return false;
    const bool bracket=*p=='(';if(bracket)++p;
    if(*p<'0'||*p>'9')return false;
    while(*p>='0'&&*p<='9')++p;
    if(bracket&&*p++!=')')return false;
    return !*p;
}
bool enabled(const SceneSpawn& e){
    const auto kind=sceneEnemyKind(e.prefab);
    // Boss prefabs start inactive in the source; their encounter owns activation.
    return e.saveActive&&(e.active||kind==SceneEnemyKind::Mawlek||
        kind>=SceneEnemyKind::GruzMother);
}
bool isJellyfish(SceneEnemyKind kind){
    return kind>=SceneEnemyKind::Jellyfish&&kind<=SceneEnemyKind::JellyfishInert;
}
unsigned asset(SceneEnemyKind k){
    if(k==SceneEnemyKind::FungoonBaby)return 13;
    if(k==SceneEnemyKind::FungCrawler)return 14;
    if(isJellyfish(k))return 12;
    if(k==SceneEnemyKind::ZombieGuard)return 9;
    if(k==SceneEnemyKind::Spitter)return 10;
    if(k==SceneEnemyKind::FungusFlyer)return 11;
    if(k==SceneEnemyKind::ElderBaldur)return 8;
    if(k==SceneEnemyKind::Crawler)return 0;
    if(k>=SceneEnemyKind::Tiktik&&k<=SceneEnemyKind::Hatchling)return 1+unsigned(k)-unsigned(SceneEnemyKind::Tiktik);
    if(k>=SceneEnemyKind::Wandering&&k<=SceneEnemyKind::Goam)return 5;
    return 6;
}
EnemyCKind cKind(SceneEnemyKind k){
    switch(k){
    case SceneEnemyKind::Bully:return EnemyCKind::HuskBully;
    case SceneEnemyKind::Baldur:return EnemyCKind::Baldur;
    case SceneEnemyKind::Mother:return EnemyCKind::AspidMother;
    case SceneEnemyKind::Brawler:return EnemyCKind::MushroomBrawler;
    case SceneEnemyKind::Sentry:return EnemyCKind::Sentry;
    case SceneEnemyKind::HeavySentry:return EnemyCKind::HeavySentry;
    case SceneEnemyKind::FlyingSentry:return EnemyCKind::FlyingSentry;
    default:return EnemyCKind::BroodingMawlek;
    }
}
const EnemyRewardConfig& rewardFor(const SceneSpawn& e,SceneEnemyKind k){
    static const EnemyRewardConfig empty{};
    static const EnemyRewardConfig elder{25,5,0,{},true,false,false}; // level50-4585/4637
    static const EnemyRewardConfig guard{20,5,0,{},false,true,false};
    static const EnemyRewardConfig spitter{6,0,0,{},false,false,false};
    static const EnemyRewardConfig jellyfish{5,0,0,{},false,false,false};
    static const EnemyRewardConfig fungoonBaby{2,0,0,{},false,false,false};
    if(e.hasReward)return e.reward;
    if(k==SceneEnemyKind::FungoonBaby||k==SceneEnemyKind::FungCrawler)return fungoonBaby;
    if(k==SceneEnemyKind::Jellyfish)return jellyfish;
    if(k==SceneEnemyKind::ZombieGuard)return guard;
    if(k==SceneEnemyKind::Spitter||k==SceneEnemyKind::FungusFlyer)return spitter;
    if(k==SceneEnemyKind::ElderBaldur)return elder;
    if(k>=SceneEnemyKind::Brawler)return empty;
    return enemyRewardConfig(EnemyRewardKind(unsigned(k)-unsigned(SceneEnemyKind::Crawler)));
}
struct ElderPlacement { unsigned owner, terrainOwner, collider; };
ElderPlacement elderPlacement(const char* scene){
    if(!std::strcmp(scene,"Crossroads_11_alt"))return {729,731,2936};
    if(!std::strcmp(scene,"Crossroads_ShamanTemple"))return {2371,2369,9301};
    return {};
}
SceneEnemyKind placementKind(const SceneSpawn& e,const char* scene){
    const auto k=sceneEnemyKind(e.prefab);
    if(k==SceneEnemyKind::ZombieGuard&&(std::strcmp(scene,"Crossroads_21")||e.owner!=1207))return SceneEnemyKind::Unsupported;
    // Other Blocker instances need their own source placement/range audit.
    if(k==SceneEnemyKind::ElderBaldur){
        const auto placement=elderPlacement(scene);
        if(!placement.owner||placement.owner!=e.owner)return SceneEnemyKind::Unsupported;
    }
    return k;
}
bool usesEnemyB(SceneEnemyKind kind){
    return (kind>=SceneEnemyKind::Wandering&&kind<=SceneEnemyKind::Goam)||kind==SceneEnemyKind::FungCrawler;
}
constexpr EnemyBConfig fungCrawlerConfig(){
    EnemyBConfig config{};
    config.hp=12;config.damage=1;config.smallGeo=2;
    config.body={-0.6874996f,-0.989348f,0.5332104f,0.044912f};
    config.speed=4;config.gravity=1;config.recoilSpeed=15;config.recoilDuration=.15f;
    config.hideTime=config.upTime=config.downTime=config.exposedTime=1;
    return config;
}
}
SceneEnemyKind sceneEnemyKind(const char* name){
    if(!name)return SceneEnemyKind::Unsupported;
    if(prefab(name,"Crawler"))return SceneEnemyKind::Crawler;
    for(unsigned i=0;i<4;++i)if(prefab(name,crossroadsEnemyRegistry[i].prefab))return SceneEnemyKind(unsigned(SceneEnemyKind::Tiktik)+i);
    for(unsigned i=0;i<4;++i)if(prefab(name,enemyBRegistry[i].prefab))return SceneEnemyKind(unsigned(SceneEnemyKind::Wandering)+i);
    if(prefab(name,"Zombie Runner Sp"))return SceneEnemyKind::Wandering;
    if(prefab(name,"Zombie Hornhead Sp"))return SceneEnemyKind::Hornhead;
    if(prefab(name,"Zombie Barger"))return SceneEnemyKind::Bully;
    if(prefab(name,"Roller"))return SceneEnemyKind::Baldur;
    if(prefab(name,"Blocker"))return SceneEnemyKind::ElderBaldur;
    if(prefab(name,"Zombie Guard"))return SceneEnemyKind::ZombieGuard;
    if(prefab(name,"Spitter"))return SceneEnemyKind::Spitter;
    if(prefab(name,"Fungus Flyer"))return SceneEnemyKind::FungusFlyer;
    if(prefab(name,"Jellyfish"))return SceneEnemyKind::Jellyfish;
    if(prefab(name,"Jellyfish Baby"))return SceneEnemyKind::JellyfishBaby;
    if(prefab(name,"Jellyfish Baby Inert"))return SceneEnemyKind::JellyfishInert;
    if(prefab(name,"Fungoon Baby"))return SceneEnemyKind::FungoonBaby;
    if(prefab(name,"Fung Crawler"))return SceneEnemyKind::FungCrawler;
    if(prefab(name,"Hatcher"))return SceneEnemyKind::Mother;
    if(prefab(name,"Mawlek Body"))return SceneEnemyKind::Mawlek;
    if(prefab(name,"Mushroom Brawler"))return SceneEnemyKind::Brawler;
    if(prefab(name,"Ruins Sentry")||prefab(name,"Ruins Sentry 1")||prefab(name,"Ruins SentryB"))return SceneEnemyKind::Sentry;
    if(prefab(name,"Ruins Sentry Fat")||prefab(name,"Ruins Sentry FatB")||prefab(name,"Ruins Sentry Fat B"))return SceneEnemyKind::HeavySentry;
    if(prefab(name,"Ruins Flying Sentry")||prefab(name,"Ruins Flying SentryB"))return SceneEnemyKind::FlyingSentry;
    if(prefab(name,"Giant Fly"))return SceneEnemyKind::GruzMother;
    if(prefab(name,"False Knight New"))return SceneEnemyKind::FalseKnight;
    if(prefab(name,"Dung Defender"))return SceneEnemyKind::DungDefender;
    return SceneEnemyKind::Unsupported;
}
void SceneEnemies::clear(){
    audioSerials_={};audioSeconds_=0;
    battle.bind({});
    count_=geo_=unsupported=0;freeze_=false;spawns_.clear();activationSpawns_.clear();used_.fill(false);
    rewards_.clear();life_=nullptr;scene_[0]=0;
    elder_={};elderPlacement_=false;sceneBools_=nullptr;
    c_.clear();watchers.reset();mantis.reset();brokenVessel.reset();watcherRoom_=mantisRoom_=mantisDefeated_=brokenRoom_=false;boss_=nullptr;mawlek_=EnemyCEncounter::capacity;mawlekStarted_=false;
    for(auto& o:objects_)o.colliderEnabled=false;
}
bool SceneEnemies::bind(const SceneBank& scene,const char* root,const NailBank& nails,const AnimationBank& art,
                        const CrawlerConfig& crawler,EnemyRandom random,const World& terrain,std::uint32_t flags,CrossroadsBoss& boss,
                        WatcherProgress progress,unsigned upgrades,const char* sceneName,EnemyLifeState* life,const SceneActivationState* activation,SceneBoolSave* sceneBools,bool mantisDefeated,BrokenVesselProgress brokenProgress,bool cityCrestSpent){
    clear();cityCrestSpent_=cityCrestSpent;spawns_=scene.enemies;bossRoom_=scene.bossRoom;nails_=&nails;crawlerArt_=&art;crawlerConfig_=crawler;random_=random;
    life_=life;std::snprintf(scene_,sizeof(scene_),"%s",sceneName?sceneName:"");
    sceneBools_=sceneBools;
    const auto placement=elderPlacement(scene_);
    elderPlacement_=placement.owner&&std::any_of(scene.enemies.begin(),scene.enemies.end(),
        [placement](const SceneSpawn& e){return e.owner==placement.owner&&sceneEnemyKind(e.prefab)==SceneEnemyKind::ElderBaldur;});
    battle.bind(scene.battle,sceneBools);
    for(unsigned i=0;i<spawns_.size();++i)for(const auto& target:scene.rules.activation.targets)
        if(target.kind==SceneActivationTarget::Spawn&&target.id==spawns_[i].owner){
            activationSpawns_.push_back(i);
            if(activation){spawns_[i].saveActive=activation->enabled(target.kind,target.id);spawns_[i].active=spawns_[i].saveActive;}
            break;
        }
    boss_=&boss;
    watcherRoom_=sceneName&&!std::strcmp(sceneName,"Ruins2_03");watcherProgress_=progress;nailSmithUpgrades_=upgrades;
    mantisRoom_=!std::strcmp(scene_,"Fungus2_15");mantisDefeated_=mantisDefeated;
    brokenRoom_=!std::strcmp(scene_,"Abyss_19");brokenProgress_=brokenProgress;
    if(watcherRoom_||mantisRoom_||brokenRoom_)used_[7]=true;
    for(unsigned i=0;i<spawns_.size();++i){const auto& e=spawns_[i];
        const auto k=placementKind(e,scene_);
        const bool canEnable=std::find(activationSpawns_.begin(),activationSpawns_.end(),i)!=activationSpawns_.end();
        if(k!=SceneEnemyKind::Unsupported&&k<SceneEnemyKind::GruzMother&&
           (enabled(e)||canEnable||battle.staged(e.owner)))used_[asset(k)]=true;
    }
    char path[256];
    for(unsigned i=1;i<=4;++i)if(used_[i]){
        const auto* stem=crossroadsEnemyRegistry[i-1].assetStem;
        std::snprintf(path,sizeof(path),"%s/%s.bin",root,stem);if(!aConfig_[i-1].load(path))return false;
        std::snprintf(path,sizeof(path),"%s/%s-anim.bin",root,stem);if(!banks_[i-1].load(path))return false;
    }
    if(used_[5]){
        std::snprintf(path,sizeof(path),"%s/enemy-b.bin",root);if(!loadEnemyBConfigs(path,bConfig_))return false;
        std::snprintf(path,sizeof(path),"%s/enemy-b-anim.bin",root);if(!banks_[4].load(path))return false;
    }
    if(used_[6]){
        std::snprintf(path,sizeof(path),"%s/enemy-c-anim.bin",root);if(!banks_[5].load(path))return false;
    }
    if(used_[7]){
        std::snprintf(path,sizeof(path),"%s/%s-art.bin",root,encounterArtStem());if(!banks_[6].load(path))return false;
    }
    if(used_[8]){
        std::snprintf(path,sizeof(path),"%s/elder-baldur-art.bin",root);if(!banks_[7].load(path))return false;
    }
    if(used_[9]){
        std::snprintf(path,sizeof(path),"%s/zombie-guard-anim.bin",root);if(!banks_[8].load(path))return false;
    }
    if(used_[10]){
        std::snprintf(path,sizeof(path),"%s/spitter-anim.bin",root);if(!banks_[9].load(path))return false;
    }
    if(used_[11]){
        std::snprintf(path,sizeof(path),"%s/fungus-flyer-anim.bin",root);if(!banks_[10].load(path))return false;
    }
    if(used_[12]){
        std::snprintf(path,sizeof(path),"%s/jellyfish-anim.bin",root);if(!banks_[11].load(path))return false;
    }
    if(used_[13]){
        std::snprintf(path,sizeof(path),"%s/fungoon-baby-anim.bin",root);if(!banks_[12].load(path))return false;
    }
    if(used_[14]){
        std::snprintf(path,sizeof(path),"%s/fung-crawler-anim.bin",root);if(!banks_[13].load(path))return false;
    }
    solids_.reserve(terrain.count+3);
    lockShapes_.clear();lockVertices_.clear();
    if(terrain.shapeCount){
        lockShapes_.assign(terrain.shapes,terrain.shapes+terrain.shapeCount);
        std::uint32_t vertices=0,id=0;
        for(const auto& s:lockShapes_){vertices=std::max(vertices,s.first+s.count);id=std::max(id,s.id);}
        lockVertices_.assign(terrain.vertices,terrain.vertices+vertices);
        for(unsigned i=0;i<3;++i){
            TerrainShape s;s.id=id+1+i;s.first=vertices+4*i;s.count=4;s.queryBoundsValid=true;
            lockShapes_.push_back(s);
        }
        lockVertices_.resize(vertices+12);
    }
    return restart(terrain,flags);
}
bool SceneEnemies::restart(const World& terrain,std::uint32_t flags){
    audioSerials_={};audioSeconds_=0;
    battle.restart();
    count_=geo_=unsupported=0;flags_=flags;freeze_=false;mawlekStarted_=false;mawlek_=EnemyCEncounter::capacity;
    rewards_.clear();
    c_.bind(used_[6]?&banks_[5]:nullptr,nails_,random_);
    elder_={};guard_={};spitters_={};flyers_={};
    unsigned na=0,nb=0,nc=0,spitterCount=0,flyerCount=0;
    for(auto& o:objects_){o={};o.colliderEnabled=false;}
    for(const auto& e:spawns_)if(enabled(e)&&!battle.staged(e.owner)){
        const auto k=placementKind(e,scene_);
        if(k==SceneEnemyKind::Unsupported){++unsupported;continue;}
        if(k>=SceneEnemyKind::GruzMother)continue;
        if(k==SceneEnemyKind::ElderBaldur&&elderDefeated())continue;
        if(k==SceneEnemyKind::Mawlek&&(flags&TutorialSave::mawlekFlag))continue;
        const auto& reward=rewardFor(e,k);
        if(k!=SceneEnemyKind::Mawlek&&reward.persistent&&life_&&life_->dead(scene_,e.owner))continue;
        if(count_==capacity-(elderPlacement_?1u:0u))return false;
        const unsigned spawn=unsigned(&e-spawns_.data());
        auto& slot=slots_[count_];slot={k,0,asset(k),spawn,e.scale.y,true,e.owner,reward};slot.goamRotation=e.goamRotation;auto& object=objects_[count_];
        if(k==SceneEnemyKind::ElderBaldur){
            if(!elder_.bind(object,banks_[7],e.position,e.scale.x,random_,&objects_[capacity-1]))return false;
        }else if(k==SceneEnemyKind::ZombieGuard){
            if(!guard_.bind(object,banks_[8],e.position,e.scale.x))return false;
        }else if(k==SceneEnemyKind::Spitter){
            if(spitterCount==spitters_.size())return false;
            slot.actor=spitterCount++;
            if(!spitters_[slot.actor].bind(object,banks_[9],e.position,e.scale.x))return false;
        }else if(k==SceneEnemyKind::FungusFlyer){
            if(flyerCount==flyers_.size())return false;
            slot.actor=flyerCount++;
            if(!flyers_[slot.actor].bind(object,banks_[10],e.position,e.scale.x))return false;
        }else if(isJellyfish(k)){
            slot.actor=count_;
            if(!jellyfish_[count_].bind(object,banks_[11],k,e.position,e.scale.x,random_))return false;
        }else if(k==SceneEnemyKind::FungoonBaby){
            slot.actor=count_;
            if(!fungoonBabies_[count_].bind(object,banks_[12],e.position,e.scale.x))return false;
        }else if(k==SceneEnemyKind::Crawler){
            if(nc==crawlers_.size())return false;
            slot.actor=nc++;
            if(!crawlers_[slot.actor].bind(crawlerConfig_,*crawlerArt_,object,e.position,terrain,random_,e.scale.x))return false;
        }else if(k<=SceneEnemyKind::Hatchling){
            if(na==a_.size())return false;
            slot.actor=na++;
            if(!a_[slot.actor].bind(aConfig_[slot.bank-1],banks_[slot.bank-1],object,e.position,terrain,random_,e.scale.x))return false;
        }else if(usesEnemyB(k)){
            if(nb==b_.size())return false;
            slot.actor=nb++;
            const bool fungCrawler=k==SceneEnemyKind::FungCrawler;
            const auto kind=fungCrawler?EnemyBKind::FungCrawler:EnemyBKind(unsigned(k)-unsigned(SceneEnemyKind::Wandering));
            if(!b_[slot.actor].bind(kind,fungCrawler?fungCrawlerConfig():bConfig_[unsigned(kind)],banks_[slot.bank-1],object,e.position,e.scale.x,
                k==SceneEnemyKind::Goam?e.goamRotation:0,e.scale.y))return false;
            if(k==SceneEnemyKind::Goam&&!goamStartDown(scene_,e.owner))b_[slot.actor].startUp();
        }else{
            const auto kind=cKind(k);
            unsigned index=0;while(index<c_.capacity&&c_.actors[index].occupied())++index;
            if(!c_.spawn(kind,e.position,0,e.scale))return false;
            c_.actors[index].scaleX=e.scale.x;
            if(k==SceneEnemyKind::FlyingSentry){
                c_.actors[index].battleRegions=prefab(e.prefab,"Ruins Flying SentryB");
                c_.actors[index].startIdle=!c_.actors[index].battleRegions&&flyingSentryStartIdle(scene_,e.owner);
            }
            if(k==SceneEnemyKind::Brawler)c_.actors[index].facingRight=brawlerFacingRight(scene_,e.owner);
            if(k==SceneEnemyKind::Mawlek){mawlek_=index;mawlekArena_=arenaFor(e,terrain);}
            slot.actor=index;
            slot.cSerial=c_.actors[index].audioSerial;
        }
        activateMovementAudio(count_);
        ++count_;
    }
    if(boss_)boss_->reset();
    if(const auto* e=bossSpawn()){
        const auto kind=sceneEnemyKind(e->prefab)==SceneEnemyKind::DungDefender?CrossroadsBossKind::DungDefender:
            sceneEnemyKind(e->prefab)==SceneEnemyKind::GruzMother?CrossroadsBossKind::GruzMother:CrossroadsBossKind::FalseKnight;
        if(!boss_->bind(kind,arenaFor(*e,terrain),objects_.data()+capacity,CrossroadsBoss::objectCapacity,nullptr,flags,cityCrestSpent_))return false;
        boss_->place(e->position,e->scale.x);
        if(bossRoom_.authored)boss_->activationRegion(bossRoom_.trigger);
    }
    watchers.reset();
    mantis.reset();
    if(mantisRoom_&&!mantis.bind(objects_.data()+watcherObjectBegin,WatcherEncounter::capacity,&banks_[6],random_,mantisDefeated_))return false;
    if(watcherRoom_){
        if(!watchers.bind(objects_.data()+watcherObjectBegin,WatcherEncounter::capacity,&banks_[6],random_,watcherProgress_,nailSmithUpgrades_))return false;
    }
    brokenVessel.reset();
    if(brokenRoom_&&!brokenVessel.bind(objects_.data()+watcherObjectBegin,&banks_[6],random_,brokenProgress_,WatcherEncounter::capacity))return false;
    return combat_.bind(objects_.data(),combatObjectCount(),nails_);
}
unsigned SceneEnemies::combatObjectCount() const{
    if(elderPlacement_)return capacity; // Last object is the spawned Roller, independently spell-targetable.
    if(watcherRoom_||mantisRoom_||brokenRoom_)return objects_.size();
    return bossSpawn()?watcherObjectBegin:count_;
}
bool SceneEnemies::activateSpawn(unsigned spawn,const World& terrain){
    if(spawn>=spawns_.size())return false;
    const auto& e=spawns_[spawn];const auto k=placementKind(e,scene_);
    if(k==SceneEnemyKind::Unsupported)return true;
    if(k>=SceneEnemyKind::GruzMother)return false;
    if(k==SceneEnemyKind::ElderBaldur&&elderDefeated())return true;
    if(k==SceneEnemyKind::Mawlek&&(flags_&TutorialSave::mawlekFlag))return true;
    const auto& reward=rewardFor(e,k);
    if(k!=SceneEnemyKind::Mawlek&&reward.persistent&&life_&&life_->dead(scene_,e.owner))return true;
    unsigned slotIndex=count_;
    for(unsigned i=0;i<count_;++i)if(slots_[i].spawn==spawn){slotIndex=i;break;}
    if(slotIndex==count_){
        for(unsigned i=0;i<count_;++i)if(!slots_[i].active){slotIndex=i;break;}
        if(slotIndex==count_&&count_==capacity-(elderPlacement_?1u:0u))return false;
    }
    if(k==SceneEnemyKind::ElderBaldur){
        if(!elder_.bind(objects_[slotIndex],banks_[7],e.position,e.scale.x,random_,&objects_[capacity-1]))return false;
        slots_[slotIndex]={k,0,asset(k),spawn,e.scale.y,true,e.owner,reward};
        if(slotIndex==count_)++count_;
        return true;
    }
    if(k==SceneEnemyKind::ZombieGuard){
        if(!guard_.bind(objects_[slotIndex],banks_[8],e.position,e.scale.x))return false;
        slots_[slotIndex]={k,0,asset(k),spawn,e.scale.y,true,e.owner,reward};
        if(slotIndex==count_)++count_;
        return true;
    }
    if(k==SceneEnemyKind::Spitter||k==SceneEnemyKind::FungusFlyer){
        unsigned actor=0;
        const auto limit=k==SceneEnemyKind::Spitter?spitters_.size():flyers_.size();
        for(;actor<limit;++actor){
            bool occupied=false;
            for(unsigned i=0;i<count_;++i)
                occupied|=i!=slotIndex&&slots_[i].active&&slots_[i].kind==k&&slots_[i].actor==actor;
            if(!occupied)break;
        }
        if(actor==limit)return false;
        auto& object=objects_[slotIndex];
        if(k==SceneEnemyKind::Spitter?!spitters_[actor].bind(object,banks_[9],e.position,e.scale.x):
            !flyers_[actor].bind(object,banks_[10],e.position,e.scale.x))return false;
        slots_[slotIndex]={k,actor,asset(k),spawn,e.scale.y,true,e.owner,reward};
        if(slotIndex==count_)++count_;
        return true;
    }
    if(k==SceneEnemyKind::FungoonBaby){
        if(!fungoonBabies_[slotIndex].bind(objects_[slotIndex],banks_[12],e.position,e.scale.x))return false;
        slots_[slotIndex]={k,slotIndex,asset(k),spawn,e.scale.y,true,e.owner,reward};
        if(slotIndex==count_)++count_;
        return true;
    }
    if(isJellyfish(k)){
        if(!jellyfish_[slotIndex].bind(objects_[slotIndex],banks_[11],k,e.position,e.scale.x,random_))return false;
        slots_[slotIndex]={k,slotIndex,asset(k),spawn,e.scale.y,true,e.owner,reward};
        if(slotIndex==count_)++count_;
        return true;
    }
    if(k>=SceneEnemyKind::Bully&&!usesEnemyB(k)){
        unsigned actor=0;while(actor<c_.capacity&&c_.actors[actor].occupied())++actor;
        if(actor==c_.capacity||!c_.spawn(cKind(k),e.position,0,e.scale))return false;
        c_.actors[actor].scaleX=e.scale.x;
        if(k==SceneEnemyKind::FlyingSentry){
            c_.actors[actor].battleRegions=prefab(e.prefab,"Ruins Flying SentryB");
            c_.actors[actor].startIdle=!c_.actors[actor].battleRegions&&flyingSentryStartIdle(scene_,e.owner);
        }
        if(k==SceneEnemyKind::Brawler)c_.actors[actor].facingRight=brawlerFacingRight(scene_,e.owner);
        if(k==SceneEnemyKind::Mawlek){mawlek_=actor;mawlekArena_=arenaFor(e,terrain);}
        slots_[slotIndex]={k,actor,asset(k),spawn,e.scale.y,true,e.owner,reward};
        slots_[slotIndex].cSerial=c_.actors[actor].audioSerial;
        if(slotIndex==count_)++count_;
        return true;
    }
    const unsigned limit=k==SceneEnemyKind::Crawler?crawlers_.size():k<=SceneEnemyKind::Hatchling?a_.size():b_.size();
    unsigned actor=0;
    const auto pool=[](SceneEnemyKind kind){return kind==SceneEnemyKind::Crawler?0:kind<=SceneEnemyKind::Hatchling?1:2;};
    for(;actor<limit;++actor){
        bool used=false;
        for(unsigned i=0;i<count_;++i){
            if(i!=slotIndex&&slots_[i].active&&(slots_[i].kind<SceneEnemyKind::Bully||usesEnemyB(slots_[i].kind))&&
               pool(slots_[i].kind)==pool(k)&&slots_[i].actor==actor){used=true;break;}
        }
        if(!used)break;
    }
    if(actor==limit)return false;
    auto& object=objects_[slotIndex];object={};
    bool ok=false;
    if(k==SceneEnemyKind::Crawler)
        ok=crawlers_[actor].bind(crawlerConfig_,*crawlerArt_,object,e.position,terrain,random_,e.scale.x);
    else if(k<=SceneEnemyKind::Hatchling)
        ok=a_[actor].bind(aConfig_[asset(k)-1],banks_[asset(k)-1],object,e.position,terrain,random_,e.scale.x);
    else {
        const bool fungCrawler=k==SceneEnemyKind::FungCrawler;
        const auto kind=fungCrawler?EnemyBKind::FungCrawler:EnemyBKind(unsigned(k)-unsigned(SceneEnemyKind::Wandering));
        ok=b_[actor].bind(kind,fungCrawler?fungCrawlerConfig():bConfig_[unsigned(kind)],banks_[asset(k)-1],object,e.position,e.scale.x,
            k==SceneEnemyKind::Goam?e.goamRotation:0,e.scale.y);
        if(ok&&k==SceneEnemyKind::Goam&&!goamStartDown(scene_,e.owner))b_[actor].startUp();
    }
    if(!ok)return false;
    slots_[slotIndex]={k,actor,asset(k),spawn,e.scale.y,true,e.owner,reward};
    activateMovementAudio(slotIndex);
    if(slotIndex==count_)++count_;
    return true;
}
bool SceneEnemies::updateActivation(const SceneActivationState& activation,const World& terrain){
    bool changed=false;
    // Release disabled actors before admitting replacements into fixed pools.
    for(unsigned pass=0;pass<2;++pass)for(const auto spawn:activationSpawns_){
        auto& e=spawns_[spawn];const bool wanted=activation.enabled(SceneActivationTarget::Spawn,e.owner);
        if(wanted!=(pass!=0)||wanted==e.saveActive)continue;
        if(!wanted){
            e.saveActive=e.active=false;changed=true;
            for(unsigned i=0;i<count_;++i)if(slots_[i].spawn==spawn&&slots_[i].active){
                auto& slot=slots_[i];slot.active=false;
                if(slot.kind>=SceneEnemyKind::Bully&&slot.kind<=SceneEnemyKind::FlyingSentry){
                    c_.actors[slot.actor].phase=EnemyCPhase::Gone;
                    if(slot.kind==SceneEnemyKind::Mawlek&&mawlek_==slot.actor){
                        mawlek_=EnemyCEncounter::capacity;mawlekStarted_=false;mawlekArena_={};
                    }
                }
                else if(slot.kind<SceneEnemyKind::GruzMother)objects_[i].colliderEnabled=false;
                break;
            }
            if(sceneEnemyKind(e.prefab)>=SceneEnemyKind::GruzMother)boss_->reset();
        }else if(sceneEnemyKind(e.prefab)>=SceneEnemyKind::GruzMother){
            const auto* boss=bossSpawn();
            if(!boss)return false;
            const auto kind=sceneEnemyKind(boss->prefab)==SceneEnemyKind::DungDefender?CrossroadsBossKind::DungDefender:
                sceneEnemyKind(boss->prefab)==SceneEnemyKind::GruzMother?CrossroadsBossKind::GruzMother:CrossroadsBossKind::FalseKnight;
            if(!boss_->bind(kind,arenaFor(*boss,terrain),objects_.data()+capacity,CrossroadsBoss::objectCapacity,nullptr,flags_,cityCrestSpent_))return false;
            boss_->place(boss->position,boss->scale.x);
            e.saveActive=e.active=true;changed=true;
        }else if(!activateSpawn(spawn,terrain))return false;
        else {e.saveActive=e.active=true;changed=true;}
    }
    if(changed){
        if(!combat_.bind(objects_.data(),combatObjectCount(),nails_))return false;
    }
    return true;
}
void SceneEnemies::ZombieGuard::enter(Phase next,const AnimationBank& bank,const char* clip,float duration){
    phase=next;
    timer=duration;
    if(clip)animation.play(bank.require(clip),0,true);
}
bool SceneEnemies::ZombieGuard::bind(CombatObject& object,const AnimationBank& bank,Vec2 spawn,float scale){
    for(const char* clip:{"Dormant","Wake","Idle","Turn","Walk","Run","Stop Walk","Stop Run","Startle",
                          "Anticipate","Attack2","Swipe","Stomp Antic","Stomp Jump","Stomp Land","Shockwave Spurt"})
        if(!bank.find(clip))return false;
    *this={};
    home=position=spawn;
    presentation.reset(spawn);
    scaleX=scale<0?-1.f:1.f;
    object={};object.hasHealth=true;object.health.hp=70;object.health.enemyType=5;object.name="Zombie Guard";
    object.bounds=boxBounds(position,{-1.625f,-3.875f,.984375f,.34375f},scaleX);
    enter(Phase::Dormant,bank,"Dormant");
    return true;
}
void SceneEnemies::ZombieGuard::step(CombatObject& object,const AnimationBank& bank,EnemyRandom random,
                                    float dt,Hero& hero,const World& terrain){
    if(object.health.dead||phase==Phase::Dead)return;
    PresentationPosition::Step history(presentation,position);
    animation.advance(dt);
    swipe.advance(dt);
    timer-=dt;
    const Rect heroBox=bodyBounds(hero.position);
    const float delta=hero.position.x-position.x;
    const float distance=std::abs(delta);
    const float direction=delta<0?-1.f:1.f;
    const bool roam=std::abs(hero.position.x-home.x)<=23.5f;
    // Attack Range: (0,-0.56) x (16.29,6.39); Wake sets its X scale to 10.
    const float reach=phase==Phase::Dormant?8.145f:5.f;
    const bool inAttackRange=overlaps(boxBounds(position,{-reach,-3.755f,reach,2.635f}),heroBox);
    const bool inAlertRange=roam&&overlaps(boxBounds(position,{-16.83f,-3.965f,16.83f,2.765f}),heroBox);
    const Vec2 toHero{delta,hero.position.y-position.y};
    const float length=std::sqrt(toHero.x*toHero.x+toHero.y*toHero.y);
    float hitDistance=0;
    const bool visible=(inAttackRange||inAlertRange)&&(length==0||!rayTerrain(
        position,{toHero.x/length,toHero.y/length},length,terrain,hitDistance,TerrainShape::terrainQueryExcluded));
    const bool chase=visible&&inAlertRange;
    const bool attack=visible&&inAttackRange;
    const auto next=[&](Phase state,const char* clip,float wait=0){enter(state,bank,clip,wait);};
    const auto chooseAttack=[&]{
        const bool stomp=clubRepeats>=4||(stompRepeats<2&&random.range(0,1)<.25f);
        if(stomp){++stompRepeats;clubRepeats=0;next(Phase::StompAntic,"Stomp Antic",.35f);}
        else{++clubRepeats;stompRepeats=0;next(Phase::Anticipate,"Anticipate");}
    };
    velocity.x=0;
    switch(phase){
    case Phase::Dormant:
        if(object.health.hp<70||attack)next(Phase::Wake,"Wake");
        break;
    case Phase::Wake:if(animation.complete)next(Phase::Cooldown,"Idle",.21f);break;
    case Phase::Startle:if(animation.complete)next(Phase::Idle,"Idle");break;
    case Phase::Idle:
    case Phase::Walk:
    case Phase::Run:
    case Phase::Return: {
        // Idle: Wait 4 -> Return Check (Idle Spot +-1), walking back at 5; Return clears Woken.
        // ALERT with Woken false plays Startle first.
        if(phase==Phase::Idle&&!attack&&chase&&!woken){woken=true;next(Phase::Startle,"Startle");break;}
        const bool returning=!chase&&!attack&&std::abs(position.x-home.x)>1&&(phase==Phase::Return||(phase==Phase::Idle&&timer<=-4));
        if(returning)woken=false;
        const float facing=returning?(home.x<position.x?-1.f:1.f):direction;
        if((attack||chase||returning)&&scaleX!=facing){scaleX=facing;next(Phase::Turn,"Turn");}
        else if(attack)chooseAttack();
        else if(!chase&&(phase==Phase::Walk||phase==Phase::Run))
            next(Phase::Stop,phase==Phase::Run?"Stop Run":"Stop Walk");
        else if(chase||returning){
            const Phase moving=returning?Phase::Return:(phase==Phase::Run||distance>9?Phase::Run:Phase::Walk);
            if(phase!=moving)next(moving,moving==Phase::Run?"Run":"Walk");
            velocity.x=scaleX*(moving==Phase::Run?10.f:5.f);
        }else if(phase!=Phase::Idle)next(Phase::Idle,"Idle");
        break;
    }
    case Phase::Turn:{
        // Turn Left/Right: HERO ABOVE (Overhead Detect (0,1.963) 3.09 x 3.533) -> Anticipate.
        const Rect overhead{position.x-1.545f,position.y+1.9629359f-1.76641f,position.x+1.545f,position.y+1.9629359f+1.76641f};
        if(overlaps(overhead,heroBox)){++clubRepeats;stompRepeats=0;next(Phase::Anticipate,"Anticipate");}
        else if(animation.complete)next(Phase::Idle,"Idle");
        break;
    }
    case Phase::Stop:
        if(animation.complete)next(Phase::Idle,"Idle");
        break;
    case Phase::Anticipate:
        if(animation.complete){
            next(Phase::Strike,"Attack2",.201f);
            swipe.play(bank.require("Swipe"),0,true);
        }
        break;
    case Phase::Strike:if(timer<=0)next(Phase::Recoil,nullptr,.14f);break;
    case Phase::Recoil:
        velocity.x=-scaleX*5;
        if(timer<=0)next(Phase::StrikeEnd,nullptr);
        break;
    case Phase::StrikeEnd:if(animation.complete)next(Phase::Cooldown,"Idle",.21f);break;
    case Phase::StompAntic:if(timer<=0)next(Phase::StompJump,"Stomp Jump");break;
    case Phase::StompJump:
        velocity.x=-scaleX*10;
        if(animation.complete){
            velocity.x=0;
            for(auto& wave:waves){wave={};wave.position={position.x,position.y-4};wave.active=true;wave.speed=18*.025f;}
            next(Phase::Land,"Stomp Land");
        }
        break;
    case Phase::Land:if(animation.complete)next(Phase::Cooldown,"Idle",.61f);break;
    case Phase::Cooldown:if(timer<=0)next(Phase::Idle,"Idle");break;
    case Phase::Dead:break;
    }
    const Rect body=boxBounds({}, {-1.625f,-3.875f,.984375f,.34375f},scaleX);
    velocity.y-=60*dt;
    moveBox(position,velocity,body,dt,terrain);
    object.bounds=boxBounds(position,body);
    if(overlaps(object.bounds,heroBox))hero.takeDamage(1,position.x<hero.position.x?-1:1);
    if(swipe.clip&&!swipe.complete){
        Vec2 polygon[]={{-1,4.5625f},{-2.875f,3.703125f},{-4.9375f,1.484375f},
                        {-3.96875f,-1.078125f},{2.171875f,2.65625f}};
        for(auto& vertex:polygon){vertex.x=position.x+vertex.x*scaleX;vertex.y+=position.y;}
        // Swipe DamageHero damageDealt 2.
        if(polygonOverlapsBox(polygon,5,heroBox))hero.takeDamage(2,position.x<hero.position.x?-1:1);
    }
    for(unsigned index=0;index<waves.size();++index){
        auto& wave=waves[index];
        const float sign=index==0?-1.f:1.f;
        auto& trail=spurts[index];
        for(auto& spurt:trail){
            if(!spurt.animation.clip)continue;
            spurt.age+=dt;
            spurt.animation.advance(dt);
            if(spurt.animation.complete||spurt.age>=.3f){spurt={};continue;}
            // Spurts take the wave's Scale X 1.25.
            if(spurt.age>=.05f&&spurt.age<.10f&&overlaps(
                boxBounds(spurt.position,{-.249505f*1.25f,-.001806f,.154885f*1.25f,1.741806f},sign),heroBox))
                hero.takeDamage(1,spurt.position.x<hero.position.x?-1:1);
        }
        if(!wave.active)continue;
        // Move: Speed += 36/s; no ground 1.6 below (Raycast From (+-1,1)) -> HIT ends it; its box
        // (-0.237,1.045) 0.657 x 1.443 entering terrain -> WALL: 0.15 s more, then the end.
        float floorDistance=0;
        if(wave.endTimer>0){wave.endTimer-=dt;if(wave.endTimer<=0){wave.active=false;continue;}}
        else{
            if(!rayTerrain({wave.position.x+sign*1.25f,wave.position.y+1},{0,-1},1.6f,terrain,floorDistance,TerrainShape::terrainQueryExcluded)){
                wave.active=false;continue;
            }
            wave.speed+=36*dt;
            const Rect box=boxBounds(wave.position,{-.23742104f-.32852966f,1.0447369f-.72151971f,-.23742104f+.32852966f,1.0447369f+.72151971f},sign*1.25f);
            if(terrainSensorOverlap(box,terrain))wave.endTimer=.15f;
        }
        wave.position.x+=sign*wave.speed*dt;
        for(auto& spurt:trail){
            if(spurt.animation.clip)continue;
            spurt.position=wave.position;
            spurt.animation.play(bank.require("Shockwave Spurt"),0,true);
            break;
        }
    }
}
bool SceneEnemies::Spitter::bind(CombatObject& object,const AnimationBank& bank,Vec2 spawn,float scale){
    for(const char* clip:{"Fly","TurnToFly","Fire Long","Death Air","Shot Idle","Shot Impact"})
        if(!bank.find(clip))return false;
    *this={};
    position=home=spawn;
    presentation.reset(spawn);
    scaleX=scale<0?-1.f:1.f;
    object={};object.hasHealth=true;object.health.hp=15;object.health.enemyType=5;object.name="Spitter";
    object.bounds=boxBounds(position,{-.609375f,-.65625f,.484375f,.578125f},scaleX);
    animation.play(bank.require("Fly"));
    return true;
}
void SceneEnemies::Spitter::shoot(const AnimationBank& bank,float angle,float speed){
    for(auto& shot:shots){
        if(shot.animation.clip)continue;
        shot.position=position;
        shot.presentation.reset(position);
        shot.velocity={speed*std::cos(angle),speed*std::sin(angle)};
        shot.animation.play(bank.require("Shot Idle"));
        return;
    }
}
void SceneEnemies::Spitter::step(CombatObject& object,const AnimationBank& bank,EnemyRandom random,
                                float dt,Hero& hero,const World& terrain){
    const Rect heroBox=bodyBounds(hero.position);
    for(auto& shot:shots){
        if(!shot.animation.clip)continue;
        if(shot.impact){
            shot.animation.advance(dt);
            if(shot.animation.complete)shot={};
            continue;
        }
        shot.age+=dt;
        if(shot.age>=5.f){shot={};continue;}
        PresentationPosition::Step history(shot.presentation,shot.position);
        const float speed=std::hypot(shot.velocity.x,shot.velocity.y);
        const Vec2 direction{shot.velocity.x/speed,shot.velocity.y/speed};
        float travel=speed*dt;
        float hitDistance=0;
        const bool hitTerrain=rayTerrain(shot.position,direction,travel,terrain,hitDistance,
                                        TerrainShape::terrainQueryExcluded);
        if(hitTerrain)travel=hitDistance;
        shot.position.x+=direction.x*travel;
        shot.position.y+=direction.y*travel;
        const bool hitHero=overlaps(boxBounds(shot.position,{-.25f,-.25f,.25f,.25f}),heroBox);
        if(hitHero)hero.takeDamage(1,direction.x<0?-1:1);
        if(hitTerrain||hitHero){
            shot.impact=true;
            shot.animation.play(bank.require("Shot Impact"),0,true);
        }else shot.animation.advance(dt);
    }
    animation.advance(dt);
    if(object.health.dead)return;
    PresentationPosition::Step history(presentation,position);
    timer-=dt;
    const Vec2 delta{hero.position.x-position.x,hero.position.y-position.y};
    const float distance=std::hypot(delta.x,delta.y);
    const Vec2 direction=distance>0?Vec2{delta.x/distance,delta.y/distance}:Vec2{};
    const auto enter=[&](Phase next,const char* clip,float delay){
        phase=next;timer=delay;animation.play(bank.require(clip),0,true);
    };
    // Crossroads_19 Spitter (level57-670) FSM "spitter" 4449; PSP keeps the same states.
    const auto chase=[&]{enter(Phase::Chase,"Fly",random.range(1.5f,2.25f));};
    // DistanceFly.DoBuzz, once per fixed update: each axis gains or loses `acceleration`
    // toward (beyond `range`) or away from the hero, then clamps to speedMax.
    const auto distanceFly=[&](float range,float speedMax){
        const float toward=distance>range?.1f:-.1f;
        velocity.x=std::clamp(velocity.x+(position.x<hero.position.x?toward:-toward),-speedMax,speedMax);
        velocity.y=std::clamp(velocity.y+(position.y<hero.position.y?toward:-toward),-speedMax,speedMax);
    };
    const auto clear=[&]{
        float hitDistance=0;
        return distance<=0||!rayTerrain(position,direction,distance,terrain,hitDistance,
                                         TerrainShape::terrainQueryExcluded)||hitDistance>=distance;
    };
    // Distance Fly: Range Out Timer counts seconds outside the 12.1-unit Unalert Range; > 8 -> Idle.
    const auto unalert=[&]{
        rangeOutTimer=distance>12.1f?rangeOutTimer+dt:0;
        if(rangeOutTimer<=8)return false;
        rangeOutTimer=0;home=position;idleWait=0;idleAcceleration={};
        phase=Phase::Idle;animation.play(bank.require("Fly"));return true;
    };
    if(phase!=Phase::Idle&&phase!=Phase::Fire&&delta.x!=0)scaleX=delta.x>0?-1.f:1.f;
    switch(phase){
    case Phase::Idle:{
        // Alert Range New: CircleCollider radius .5 x scale 15.608528 against the hero
        // collider, with CheckCanSeeHero (terrain line of sight).
        constexpr float radius=7.804264f;
        const float nearX=std::clamp(position.x,heroBox.left,heroBox.right)-position.x;
        const float nearY=std::clamp(position.y,heroBox.bottom,heroBox.top)-position.y;
        if(nearX*nearX+nearY*nearY<=radius*radius&&clear()){rangeOutTimer=0;chase();break;}
        // IdleBuzz waitMin .75 waitMax 1 speedMax 1.75 accelerationMax 15 roamingRange 1:
        // acceleration is per callback (/2000), boundary damping, RNG order Y then X.
        constexpr float accelerationMax=15.f;
        const auto boundary=[&](float p,float origin,float& v,float& a){
            if((p<origin-1&&v<0)||(p>origin+1&&v>0)){
                a=(p<origin?accelerationMax:-accelerationMax)/2000;v/=1.125f;idleWait=random.range(.75f,1.f);
            }
        };
        boundary(position.y,home.y,velocity.y,idleAcceleration.y);
        boundary(position.x,home.x,velocity.x,idleAcceleration.x);
        if(idleWait<=0){
            const auto sample=[&](float p,float origin){
                return random.range(p<origin-1?0.f:-accelerationMax,p>origin+1?0.f:accelerationMax)/2000;
            };
            idleAcceleration.y=sample(position.y,home.y);
            idleAcceleration.x=sample(position.x,home.x);
            idleWait=random.range(.75f,1.f);
        }
        if(idleWait>0)idleWait-=dt;
        velocity.x=std::clamp(velocity.x+idleAcceleration.x,-1.75f,1.75f);
        velocity.y=std::clamp(velocity.y+idleAcceleration.y,-1.75f,1.75f);
        if(std::abs(velocity.x)>.01f)scaleX=velocity.x>0?-1.f:1.f;
        break;
    }
    case Phase::Chase:
        distanceFly(7,4);
        if(unalert())break;
        // Raycast: farther than 14 or blocked -> another Distance Fly wait; clear -> Fly Back.
        if(timer<=0){
            if(distance<=14&&clear())enter(Phase::Back,"Fly",.5f);
            else chase();
        }
        break;
    case Phase::Back:
        distanceFly(8.25f,4);
        if(timer<=0){enter(Phase::Fire,"Fire Long",0);fired=spread=false;}
        break;
    case Phase::Fire:
        // Fire Anticipate flies at distance 9 until the Fire Long trigger frame; Fire and
        // Fire Dribble have no movement action, so the velocity is kept.
        if(!fired)distanceFly(9,2);
        if(!fired&&animation.frame>=9){
            shotAngle=std::atan2(delta.y,delta.x);
            shoot(bank,shotAngle,15);
            fired=true;
        }else if(fired&&!spread){
            // FlingObjectsFromGlobalPool: Range(3, 5+1) objects; per object speed, then angle.
            const unsigned count=std::min(5u,unsigned(random.range(3,6)));
            for(unsigned index=0;index<count;++index){
                const float speed=random.range(10,20);
                shoot(bank,shotAngle+random.range(-.4363323f,.4363323f),speed);
            }
            spread=true;
        }
        if(animation.complete)chase();
        break;
    case Phase::Dead:break;
    }
    const Rect body=boxBounds({}, {-.609375f,-.65625f,.484375f,.578125f},scaleX);
    moveBox(position,velocity,body,dt,terrain);
    object.bounds=boxBounds(position,body);
    if(overlaps(object.bounds,heroBox))hero.takeDamage(1,position.x<hero.position.x?-1:1);
}
bool SceneEnemies::FungusFlyer::bind(CombatObject& object,const AnimationBank& bank,Vec2 spawn,float scale){
    for(const char* clip:{"Float","Spit Antic","Spit","Spit Recover","Effect","Death","Gas"})
        if(!bank.find(clip))return false;
    *this={};
    position=home=target=spawn;
    presentation.reset(spawn);
    scaleX=scale<0?-1.f:1.f;
    object={};object.hasHealth=true;object.health.hp=15;object.name="Fungus Flyer";
    object.bounds=boxBounds(position,{-1.765f,-1.44f,1.765f,2.f});
    animation.play(bank.require("Float"));
    return true;
}
void SceneEnemies::FungusFlyer::step(CombatObject& object,const AnimationBank& bank,EnemyRandom random,
                                    float dt,Hero& hero,const World& terrain){
    const Rect heroBox=bodyBounds(hero.position);
    for(auto& puff:gas){
        if(!puff.active)continue;
        puff.age+=dt;
        if(puff.age>=1.15f){puff={};continue;}
        PresentationPosition::Step history(puff.presentation,puff.position);
        // Gas Projectile: Idle (scale 1 + 2/s) for 0.75 s, then Stop keeps its velocity and collider
        // until recycled at 1.15 s with the scale frozen at 2.5 (radius 1.25).
        const float speed=std::hypot(puff.velocity.x,puff.velocity.y);
        if(speed>0){
            const Vec2 direction{puff.velocity.x/speed,puff.velocity.y/speed};
            float hit=0;
            const bool blocked=rayTerrain(puff.position,direction,speed*dt,terrain,hit,
                                          TerrainShape::terrainQueryExcluded);
            const float travel=blocked?hit:speed*dt;
            puff.position.x+=direction.x*travel;puff.position.y+=direction.y*travel;
        }
        const float dx=puff.position.x-std::clamp(puff.position.x,heroBox.left,heroBox.right);
        const float dy=puff.position.y-std::clamp(puff.position.y,heroBox.bottom,heroBox.top);
        const float radius=.5f+std::min(puff.age,.75f);
        if(dx*dx+dy*dy<=radius*radius)hero.takeDamage(1,puff.position.x<hero.position.x?1:-1);
    }
    if(phase!=Phase::Sleep)animation.advance(dt);
    effect.advance(dt);
    if(object.health.dead)return;
    PresentationPosition::Step history(presentation,position);
    timer-=dt;
    const auto enter=[&](Phase next,const char* clip,float duration=0){
        phase=next;timer=duration;animation.play(bank.require(clip),0,true);
    };
    const auto roam=[&](float speed){
        const Vec2 offset{target.x-position.x,target.y-position.y};
        const float distance=std::hypot(offset.x,offset.y);
        if(distance<.15f)target={home.x+random.range(-3,3),home.y+random.range(-1,1)};
        const float ratio=distance>0?std::min(speed,distance/dt)/distance:0;
        velocity.x+=std::clamp(offset.x*ratio-velocity.x,-20*dt,20*dt);
        velocity.y+=std::clamp(offset.y*ratio-velocity.y,-20*dt,20*dt);
    };
    // Wake Region (56 x 32, on the flyer): enter -> WAKE, exit -> global SLEEP (kinematic, still).
    const bool awake=overlaps(boxBounds(position,{-28,-16,28,16}),heroBox);
    if(phase!=Phase::Sleep&&!awake){phase=Phase::Sleep;velocity={};effect={};}
    switch(phase){
    case Phase::Sleep:
        if(awake)enter(Phase::Idle,"Float");
        break;
    case Phase::Idle:{
        const Rect attack{-.05581f,-8.80893f,3.66158f,-.64966f};
        const bool right=overlaps(boxBounds(position,attack),heroBox);
        const bool left=overlaps(boxBounds(position,attack,-1),heroBox);
        if(right||left){
            scaleX=right&&(!left||hero.position.x>=position.x)?-1.f:1.f;
            enter(Phase::Antic,"Spit Antic");
        }else roam(4);
        break;
    }
    case Phase::Antic:
        if(animation.complete){
            enter(Phase::Spit,"Spit",1);
            nextGas=0;
            effect.play(bank.require("Effect"),0,true);
        }
        break;
    case Phase::Spit:
        // AddForce2d (0,1) every frame, mass 1.
        velocity.y+=1*dt;
        nextGas-=dt;
        while(nextGas<-.00001f){
            nextGas+=.05f;
            for(auto& puff:gas)if(!puff.active){
                const float angle=(scaleX<0?random.range(278,292):random.range(248,262))*.017453293f;
                const float speed=random.range(10,11);
                puff.position={position.x-.46f*scaleX,position.y-2.71f};
                puff.presentation.reset(puff.position);
                puff.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
                puff.active=true;
                break;
            }
        }
        if(timer<=.00001f){enter(Phase::Recover,"Spit Recover");effect={};}
        break;
    case Phase::Recover:
        // Spit CD: SetScale X 1.
        if(animation.complete){enter(Phase::Cooldown,"Float",1);scaleX=1;}
        break;
    case Phase::Cooldown:
        roam(2.5f);
        if(timer<=0)enter(Phase::Idle,"Float");
        break;
    case Phase::Dead:break;
    }
    // Terrain Block (0,0.28) 3.53 x 3.44 collides; the root box (-0.048,0.260) 2.331 x 2.836 hurts.
    const Rect body{-1.765f,-1.44f,1.765f,2.f};
    if(phase!=Phase::Sleep)moveBox(position,velocity,body,dt,terrain);
    object.bounds=boxBounds(position,{-1.2138922f,-1.1582252f,1.1172038f,1.6781452f},scaleX);
    if(overlaps(object.bounds,heroBox))hero.takeDamage(1,position.x<hero.position.x?1:-1);
}
bool SceneEnemies::Jellyfish::bind(CombatObject& object,const AnimationBank& bank,SceneEnemyKind variant,
                                 Vec2 spawn,float scale,EnemyRandom random){
    const bool adult=variant==SceneEnemyKind::Jellyfish;
    const Clip* idle=bank.find(adult?"Idle":"Baby Idle");
    if(!idle||(adult&&(!bank.find("Jump")||!bank.find("Death")||
                       !bank.find("Lil Startle")||!bank.find("Lil Fly"))))return false;
    *this={};kind=variant;home=position=spawn;
    scaleX=scale;
    if(adult)position.y+=random.range(0,3);
    else{
        // Jellyfish Baby Randomise: random start frame, then FlipScale (always).
        phase=Phase::Idle;timer=random.range(.75f,1.f);
        scaleX=-scaleX;
    }
    presentation.reset(position);
    animation.play(idle,adult?0:std::min(unsigned(idle->count)-1,unsigned(random.range(0,float(idle->count)))));
    object={};object.hasHealth=true;object.health.hp=1;object.name="Jellyfish";
    object.colliderEnabled=variant!=SceneEnemyKind::JellyfishInert;
    object.bounds=boxBounds(position,body(),scaleX);
    return true;
}
Rect SceneEnemies::Jellyfish::body()const{
    return kind==SceneEnemyKind::Jellyfish?Rect{-.66555f,1.202845f,.59461f,2.533955f}:
                                           Rect{-.51481f,-.60384f,.49589f,.47516f};
}
void SceneEnemies::Jellyfish::die(CombatObject& object,const AnimationBank& bank,EnemyRandom random){
    object.colliderEnabled=false;
    if(kind!=SceneEnemyKind::Jellyfish){phase=Phase::Dead;animation={};return;}
    phase=Phase::Corpse;timer=2;
    animation.play(bank.require("Death"),0,true);
    float angle=10,speed=35;
    switch(object.health.lastDirection){
    case Strike::Right: break;
    case Strike::Left: angle=170;scaleX=-scaleX;break;
    case Strike::Up: angle=random.range(75,105);speed*=1.3f;break;
    case Strike::Down: angle=270;break;
    }
    angle*=.01745329252f;
    velocity={speed*std::cos(angle),speed*std::sin(angle)};
}
void SceneEnemies::Jellyfish::step(CombatObject& object,const AnimationBank& bank,EnemyRandom random,
                                 float dt,Hero& hero,const World& terrain){
    if(phase==Phase::Dead)return;
    if(phase==Phase::Corpse){
        PresentationPosition::Step history(presentation,position);
        animation.advance(dt);
        velocity.y-=30*dt;
        const Contacts contact=moveBox(position,velocity,boxBounds({}, {-.9f,1.04f,.7f,2.64f},scaleX),dt,terrain);
        if(contact.ground||contact.roof||contact.left||contact.right){
            phase=Phase::Startle;velocity={};
            animation.play(bank.require("Lil Startle"),0,true);
            object.health={};object.health.hp=1;object.colliderEnabled=true;
            object.bounds=boxBounds(position,{-.5f,-.5f,.5f,.5f},scaleX);
        }else if((timer-=dt)<=0){phase=Phase::Dead;animation={};}
        return;
    }
    if(phase==Phase::Startle||phase==Phase::Chase){
        if(object.health.dead)return;
        PresentationPosition::Step history(presentation,position);
        animation.advance(dt);
        if(phase==Phase::Startle){
            if(!animation.complete)return;
            phase=Phase::Chase;animation.play(bank.require("Lil Fly"));
        }
        const Vec2 delta{hero.position.x-position.x,hero.position.y-position.y};
        const float distance=std::hypot(delta.x,delta.y);
        if(distance>0){velocity.x+=90*dt*delta.x/distance;velocity.y+=90*dt*delta.y/distance;}
        const float speed=std::hypot(velocity.x,velocity.y);
        if(speed>80){velocity.x*=80/speed;velocity.y*=80/speed;}
        const Rect bounds{-.5f,-.5f,.5f,.5f};
        const Contacts contact=moveBox(position,velocity,bounds,dt,terrain);
        object.bounds=boxBounds(position,bounds);
        const bool touchedHero=overlaps(object.bounds,bodyBounds(hero.position));
        if(touchedHero)hero.takeDamage(2,position.x<hero.position.x?1:-1);
        if(touchedHero||contact.ground||contact.roof||contact.left||contact.right){
            phase=Phase::Dead;object.colliderEnabled=false;animation={};
        }
        return;
    }
    if(object.health.dead)return;
    PresentationPosition::Step history(presentation,position);
    if(kind==SceneEnemyKind::Jellyfish){
        const float dx=hero.position.x-position.x,dy=hero.position.y-position.y;
        const float distanceSquared=dx*dx+dy*dy;
        if(phase==Phase::Sleep&&distanceSquared<31*31){phase=Phase::Idle;animation.play(bank.require("Idle"));}
        if(phase==Phase::Idle&&position.y<home.y){
            if(distanceSquared>32*32){phase=Phase::Sleep;velocity={};}
            else{
                // Check Dir: X Min is overwritten to Start X + 0.5 and X Max is Start X, so X < Start X
                // + 0.5 jumps right (0..1), otherwise left (-1..0). X and Y Veloc apply together at the
                // Jump clip's trigger (Shoot).
                direction.x=position.x<home.x+.5f?random.range(0,1):random.range(-1,0);
                phase=Phase::Jump;
                animation.play(bank.require("Jump"),0,true);
            }
        }
        struct JumpEvent { Jellyfish& actor; EnemyRandom random; } event{*this,random};
        animation.advance(dt,bank,[](void* context,const Clip&,std::uint32_t,const AnimationEvent&){
            auto& jump=*static_cast<JumpEvent*>(context);
            if(jump.actor.phase==Phase::Jump)jump.actor.velocity={jump.actor.direction.x,jump.random.range(7.7f,8.3f)};
        },&event);
        if(phase==Phase::Jump&&animation.complete){phase=Phase::Idle;animation.play(bank.require("Idle"));}
        if(phase!=Phase::Sleep){
            velocity.y-=60.f*.03f*dt;
            const float damping=1/(1+1.1f*dt);
            velocity.x*=damping;velocity.y*=damping;
            position.x+=velocity.x*dt;position.y+=velocity.y*dt;
        }
    }else{
        animation.advance(dt);
        timer-=dt;
        if(timer<=0){
            const Vec2 target{home.x+random.range(-1,1),home.y+random.range(-1,1)};
            const Vec2 offset{target.x-position.x,target.y-position.y};
            const float distance=std::hypot(offset.x,offset.y);
            direction=distance>0?Vec2{offset.x/distance,offset.y/distance}:Vec2{};
            timer=random.range(.75f,1.f);
        }
        velocity.x+=std::clamp(1.5f*direction.x-velocity.x,-10*dt,10*dt);
        velocity.y+=std::clamp(1.5f*direction.y-velocity.y,-10*dt,10*dt);
        if(kind==SceneEnemyKind::JellyfishBaby)
            moveBox(position,velocity,{-.6346f,-1.09384f,.59368f,.68706f},dt,terrain);
        else{position.x+=velocity.x*dt;position.y+=velocity.y*dt;}
    }
    object.bounds=boxBounds(position,body(),scaleX);
    const Rect heroBox=bodyBounds(hero.position);
    const bool tentacles=kind==SceneEnemyKind::Jellyfish&&
        overlaps(boxBounds(position,{-.40540f,-1.65592f,.33445f,.76016f},scaleX),heroBox);
    if(object.colliderEnabled&&(tentacles||overlaps(object.bounds,heroBox)))
        hero.takeDamage(1,position.x<hero.position.x?1:-1);
}
bool SceneEnemies::FungoonBaby::bind(CombatObject& object,const AnimationBank& bank,Vec2 spawn,float scale){
    const Clip* idle=bank.find("Idle");
    if(!idle||!bank.find("TurnToIdle")||!bank.find("Death"))return false;
    *this={};home=position=spawn;scaleX=scale;
    presentation.reset(position);
    animation.play(idle);
    object={};object.hasHealth=true;object.health.hp=10;object.name="Fungoon Baby";
    object.colliderEnabled=true;object.bounds=boxBounds(position,body(),scaleX);
    return true;
}
Rect SceneEnemies::FungoonBaby::body()const{
    return {-.71154414f,-.70876513f,.49588786f,.47515307f};
}
void SceneEnemies::FungoonBaby::step(CombatObject& object,const AnimationBank& bank,EnemyRandom random,
                                    float dt,Hero& hero,const World& terrain){
    if(phase==Phase::Dead){
        animation.advance(dt);
        if(animation.complete)animation={};
        return;
    }
    if(object.health.dead)return;
    PresentationPosition::Step history(presentation,position);
    const Rect heroBox=bodyBounds(hero.position);
    // Wake Region 56 x 32 is a child of the moving baby.
    const bool awake=overlaps(boxBounds(position,{-28,-16,28,16}),heroBox);
    if(awake){
        if(phase==Phase::Sleep){phase=Phase::Awake;timer=0;}
        if((timer-=dt)<=0){
            timer=random.range(.75f,1.f);
            acceleration={random.range(-20,20),random.range(-20,20)};
        }
        const auto buzz=[dt](float speed,float acceleration,float offset,float limit){
            if(std::abs(offset)>limit){
                if(speed*offset>0)speed*=std::pow(1.f/1.125f,dt/.02f);
                acceleration=offset>0?-20.f:20.f;
            }
            return std::clamp(speed+acceleration*(dt/(2000.f*.02f)),-2.5f,2.5f);
        };
        velocity.x=buzz(velocity.x,acceleration.x,position.x-home.x,3.f);
        velocity.y=buzz(velocity.y,acceleration.y,position.y-home.y,1.f);
        // Terrain Block (-0.099,-0.013) 1.674 x 1.899 collides (the root box only hurts).
        const Contacts contacts=moveBox(position,velocity,boxBounds({},{-.93625f,-.96267f,.73795f,.93623f},scaleX),dt,terrain);
        const float size=std::abs(scaleX);
        turnPause=std::max(0.f,turnPause-dt);
        if(contacts.left!=contacts.right){
            // Bump L / Bump R: absolute SetScale and AddForce2d -+20 for one step.
            scaleX=contacts.right?-size:size;
            velocity.x+=contacts.right?-20.f*dt:20.f*dt;
            animation.play(bank.require("TurnToIdle"),0,true);
        }else if(turnPause<=0&&velocity.x!=0&&(velocity.x>0)!=(scaleX>0)){
            // FaceDirection (sprite faces right) with TurnToIdle and 0.5 s between turns.
            scaleX=velocity.x>0?size:-size;turnPause=.5f;
            animation.play(bank.require("TurnToIdle"),0,true);
        }
        animation.advance(dt);
        if(animation.complete)animation.play(bank.require("Idle"));
    }else if(phase==Phase::Awake){
        phase=Phase::Sleep;velocity={};
    }
    object.bounds=boxBounds(position,body(),scaleX);
    if(object.colliderEnabled&&overlaps(object.bounds,heroBox))
        hero.takeDamage(1,position.x<hero.position.x?1:-1);
}
void SceneEnemies::fixedUpdate(float dt,Hero& hero,const World& terrain,bool frozen){
    if(!frozen)for(unsigned i=0;i<count_;++i){const auto& s=slots_[i];
        if(!s.active)continue;
        if(s.kind==SceneEnemyKind::ElderBaldur)elder_.fixedUpdate(dt,hero,terrain);
        else if(s.kind==SceneEnemyKind::ZombieGuard)guard_.step(objects_[i],banks_[8],random_,dt,hero,terrain);
        else if(s.kind==SceneEnemyKind::Spitter)spitters_[s.actor].step(objects_[i],banks_[9],random_,dt,hero,terrain);
        else if(s.kind==SceneEnemyKind::FungusFlyer)flyers_[s.actor].step(objects_[i],banks_[10],random_,dt,hero,terrain);
        else if(isJellyfish(s.kind))jellyfish_[s.actor].step(objects_[i],banks_[11],random_,dt,hero,terrain);
        else if(s.kind==SceneEnemyKind::FungoonBaby)fungoonBabies_[s.actor].step(objects_[i],banks_[12],random_,dt,hero,terrain);
        else if(s.kind==SceneEnemyKind::Crawler){auto& a=crawlers_[s.actor];a.fixedUpdate(dt,terrain);a.afterPhysics(terrain);a.contactHero(hero);}
        else if(s.kind<=SceneEnemyKind::Hatchling){auto& a=a_[s.actor];a.fixedUpdate(dt,hero,terrain);a.contactHero(hero);}
        else if(usesEnemyB(s.kind)){auto& a=b_[s.actor];a.fixedUpdate(dt,hero.position,terrain);a.contactHero(hero);}
    }
    if(!frozen)c_.fixedUpdate(dt,hero,terrain);
    if(!frozen&&boss_&&boss_->active()){
        const auto bossWorld=world(terrain,*boss_);boss_->fixedUpdate(dt,hero,&bossWorld);
    }
    if(!frozen&&watchers.active())watchers.fixedUpdate(dt,hero,world(terrain,*boss_));
    if(!frozen&&mantis.active())mantis.fixedUpdate(dt,hero);
    if(!frozen&&brokenVessel.active())brokenVessel.fixedUpdate(dt,hero,world(terrain,*boss_));
    combat_.fixedUpdate(hero);
}
void SceneEnemies::update(float dt,Hero& hero,const World& terrain,bool frozen,Vec2 camera){
    if(!frozen)audioSeconds_+=dt;
    if(!frozen)battle.step(dt,hero.position,[&](unsigned owner)->int{
        for(unsigned i=0;i<count_;++i)if(slots_[i].active&&slots_[i].owner==owner){
            const auto& slot=slots_[i];
            return std::max(0,slot.kind>=SceneEnemyKind::Bully&&slot.kind<=SceneEnemyKind::FlyingSentry?
                c_.objects[slot.actor].health.hp:objects_[i].health.hp);
        }
        if(life_&&life_->dead(scene_,owner))return 0;
        for(const auto& spawn:spawns_)if(spawn.owner==owner&&spawn.saveActive&&battle.staged(owner)&&
            sceneEnemyKind(spawn.prefab)!=SceneEnemyKind::Unsupported)return 1;
        return -1;
    },[&](const SceneBattleData::Spawn& spawn){
        if(life_&&life_->dead(scene_,spawn.owner))return true;
        for(unsigned i=0;i<count_;++i)if(slots_[i].active&&slots_[i].owner==spawn.owner)return true;
        for(unsigned i=0;i<spawns_.size();++i)if(spawns_[i].owner==spawn.owner){
            if(spawn.reposition)spawns_[i].position=spawn.position;
            if(!activateSpawn(i,terrain))return false;
            return combat_.bind(objects_.data(),combatObjectCount(),nails_);
        }
        return false;
    });
    combat_.update(hero,dt);
    if(mantis.active()){
        if(!frozen)mantis.update(dt);
        mantisDefeated_=mantis.victoryEarned();
    }
    if(brokenVessel.active()){if(!frozen)brokenVessel.update(dt);brokenProgress_=brokenVessel.progress();}
    if(watchers.active()){
        if(!frozen)watchers.update(dt);
        watcherProgress_=watchers.progress();
    }
    for(unsigned i=0;i<count_;++i){const auto& s=slots_[i];
        if(!s.active)continue;
        if(s.kind==SceneEnemyKind::ElderBaldur){
            if(!frozen)elder_.update(dt);
            if(elder_.takeDeath()){
                killed(s,elder_.position,elder_.scaleX);
                if(sceneBools_)sceneBools_->set(SceneBoolSave::key(scene_,"Battle Scene"));
                freeze_=true;
            }
        }else if(s.kind==SceneEnemyKind::ZombieGuard){
            if(objects_[i].health.dead&&guard_.phase!=ZombieGuard::Phase::Dead){
                guard_.phase=ZombieGuard::Phase::Dead;objects_[i].colliderEnabled=false;
                killed(s,guard_.position,guard_.scaleX);freeze_=true;
            }
        }else if(s.kind==SceneEnemyKind::Spitter){
            auto& actor=spitters_[s.actor];
            if(objects_[i].health.dead&&actor.phase!=Spitter::Phase::Dead){
                actor.phase=Spitter::Phase::Dead;objects_[i].colliderEnabled=false;
                actor.animation.play(banks_[9].require("Death Air"),0,true);
                killed(s,actor.position,actor.scaleX);freeze_=true;
            }
        }else if(s.kind==SceneEnemyKind::FungusFlyer){
            auto& actor=flyers_[s.actor];
            if(objects_[i].health.dead&&actor.phase!=FungusFlyer::Phase::Dead){
                actor.phase=FungusFlyer::Phase::Dead;objects_[i].colliderEnabled=false;
                actor.animation.play(banks_[10].require("Death"),0,true);actor.effect={};
                killed(s,actor.position,actor.scaleX);freeze_=true;
            }
        }else if(s.kind==SceneEnemyKind::FungoonBaby){
            auto& actor=fungoonBabies_[s.actor];
            if(objects_[i].health.dead&&actor.phase!=FungoonBaby::Phase::Dead){
                actor.phase=FungoonBaby::Phase::Dead;objects_[i].colliderEnabled=false;
                actor.animation.play(banks_[12].require("Death"),0,true);
                killed(s,actor.position,actor.scaleX);freeze_=true;
            }
        }else if(isJellyfish(s.kind)){
            auto& actor=jellyfish_[s.actor];
            if(objects_[i].health.dead){
                switch(actor.phase){
                case Jellyfish::Phase::Sleep: case Jellyfish::Phase::Idle: case Jellyfish::Phase::Jump:
                    killed(s,actor.position,actor.scaleX);actor.die(objects_[i],banks_[11],random_);freeze_=true;break;
                case Jellyfish::Phase::Startle: case Jellyfish::Phase::Chase:
                    actor.phase=Jellyfish::Phase::Dead;actor.animation={};objects_[i].colliderEnabled=false;
                    freeze_=true;break;
                default: break;
                }
            }
        }else if(s.kind==SceneEnemyKind::Crawler){auto& a=crawlers_[s.actor];if(!frozen)a.update(dt,dt,terrain);
            if(a.killFreezeRequested){killed(s,a.position,a.scaleX);freeze_=true;a.killFreezeRequested=false;}}
        else if(s.kind<=SceneEnemyKind::Hatchling){auto& a=a_[s.actor];if(!frozen)a.update(dt,hero,terrain);
            if(a.killFreezeRequested)killed(s,a.geoPosition,a.scaleX,-a.geoRotation);
            a.geoReleased=0;freeze_=freeze_||a.killFreezeRequested;a.killFreezeRequested=false;}
        else if(usesEnemyB(s.kind)){auto& a=b_[s.actor];a.takeSmallGeo();
            if(!frozen)a.updateWalker(dt,camera,audioDepths_[i].value(audioSeconds_),random_,terrain);
            if(a.killFreezeRequested)killed(s,a.geoPosition,a.scaleX);
            freeze_=freeze_||a.killFreezeRequested;a.killFreezeRequested=false;}
    }
    c_.update(frozen?0:dt,hero,terrain);for(unsigned i=0;i<c_.dropCount;++i){const auto& d=c_.drops[i];
        if(d.kind==EnemyCKind::BroodingMawlek){geo_+=d.count;continue;}
        const auto kind=d.kind==EnemyCKind::HuskBully?SceneEnemyKind::Bully:
            d.kind==EnemyCKind::Baldur?SceneEnemyKind::Baldur:
            d.kind==EnemyCKind::AspidMother?SceneEnemyKind::Mother:
            d.kind==EnemyCKind::MushroomBrawler?SceneEnemyKind::Brawler:
            d.kind==EnemyCKind::Sentry?SceneEnemyKind::Sentry:
            d.kind==EnemyCKind::HeavySentry?SceneEnemyKind::HeavySentry:
            d.kind==EnemyCKind::FlyingSentry?SceneEnemyKind::FlyingSentry:SceneEnemyKind::Unsupported;
        bool found=false;
        for(unsigned j=0;j<count_;++j)if(slots_[j].active&&slots_[j].kind==kind&&slots_[j].actor==d.actor){
            killed(slots_[j],d.position,c_.actors[d.actor].scaleX);found=true;break;}
        if(!found&&d.kind==EnemyCKind::Hatchling)rewards_.push(EnemyRewardKind::Hatchling,d.position);
    }c_.dropCount=0;
    if(boss_&&boss_->active()){
        if(!frozen)boss_->update(dt);
        flags_|=boss_->savedFlags();
        Vec2 origin;
        if(boss_->takeGruzGeo(origin))rewards_.push({0,0,2},origin);
    }
    if(mawlek_<c_.capacity){
        const auto& a=c_.actors[mawlek_];
        // Mawlek Control Wake sends START to the Battle Scene (Alert Range New entered).
        if(a.alive()&&c_.mawlek.body!=0)mawlekStarted_=true;
        if(!a.alive())flags_|=TutorialSave::mawlekFlag;
    }
}
void SceneEnemies::activateMovementAudio(unsigned i){
    const auto& s=slots_[i];
    const bool husk=s.kind>=SceneEnemyKind::Wandering&&s.kind<=SceneEnemyKind::Leaping;
    const bool flying=s.kind==SceneEnemyKind::Vengefly||s.kind==SceneEnemyKind::Hatchling;
    if(s.kind!=SceneEnemyKind::Crawler&&s.kind!=SceneEnemyKind::Tiktik&&s.kind!=SceneEnemyKind::Gruzzer&&!husk&&!flying){audioSerials_[i]=0;return;}
    // All 32 route placements resolved through their complete Transform ancestry.
    // Crossroads_04's seven Fly roots start at world Z=.01; all others at zero.
    float initial=s.kind==SceneEnemyKind::Gruzzer&&!std::strcmp(scene_,"Crossroads_04")?.01f:0;
    // Complete Transform ancestry of the eleven route Crawler placements.
    if(s.kind==SceneEnemyKind::Crawler){
        if(!std::strcmp(scene_,"Crossroads_03")||!std::strcmp(scene_,"Crossroads_07"))
            initial=.008999999612569809f;
        else if(!std::strcmp(scene_,"Tutorial_01"))
            initial=s.owner==200||s.owner==1490?.009999999776482582f:.008999999612569809f;
    }
    // All 23 placed Vengeflies and the native Hatcher Baby prefab start at Z=0.
    // Full ancestry of all 39 husk placements; preserve serialized float offsets.
    if(s.kind==SceneEnemyKind::Wandering){
        if(!std::strcmp(scene_,"Ruins1_28")||!std::strcmp(scene_,"Deepnest_03")||
           !std::strcmp(scene_,"Deepnest_34")||!std::strcmp(scene_,"Crossroads_21")||
           !std::strcmp(scene_,"Crossroads_ShamanTemple")||
           (!std::strcmp(scene_,"Crossroads_01")&&s.owner==975))initial=.0010000000474974513f;
        else if((!std::strcmp(scene_,"Crossroads_01")&&s.owner==1029)||
                (!std::strcmp(scene_,"Crossroads_04")&&s.owner==970))initial=.0009999275207519531f;
        else if(!std::strcmp(scene_,"Crossroads_05")&&s.owner==792)initial=.00099945068359375f;
    }
    if(!++nextAudioSerial_)++nextAudioSerial_;
    audioSerials_[i]=nextAudioSerial_;
    audioDepths_[i].activate(initial,.006f,audioSeconds_,s.owner^nextAudioSerial_);
}
SceneEnemyMovementAudio SceneEnemies::movementAudio(unsigned i)const{
    // Encounter actors include births without a SceneSpawn/Slot. Enumerate the
    // native actor pool directly so reused slots retain independent lifetimes.
    if(i>=capacity){
        if(i>=movementCapacity)return {};
        const auto& a=c_.actors[i-capacity];
        if(!a.alive()||(a.kind!=EnemyCKind::Hatchling&&a.kind!=EnemyCKind::AspidMother))return {};
        const bool mother=a.kind==EnemyCKind::AspidMother;
        return {a.audioSerial,mother?EnemyClip::Hatcher:EnemyClip::Hatchling,a.position,
            a.audioDepth.value(a.audioSeconds),mother&&a.motherAlerted?1.2000000476837158f:1.f};
    }
    if(i>=count_||!slots_[i].active||!audioSerials_[i])return {};
    const auto& s=slots_[i];
    if(s.kind==SceneEnemyKind::Crawler){
        const auto& actor=crawlers_[s.actor];
        if(!actor.alive())return {};
        const auto profile=!std::strcmp(scene_,"Crossroads_01")&&s.owner==967?
            EnemyLoopProfile::CrawlerCrossroads01:EnemyLoopProfile::Default;
        return {audioSerials_[i],EnemyClip::Crawler,actor.position,
            audioDepths_[i].value(audioSeconds_),1,0,profile};
    }
    if(s.kind>=SceneEnemyKind::Wandering&&s.kind<=SceneEnemyKind::Leaping){
        const auto& actor=b_[s.actor];
        // Walker.BeginWalking plays; BeginStopped stops. BeginTurning keeps phase.
        // The attack FSM stops Walker before Anticipate and starts it in Reset.
        if(!actor.alive()||(actor.state!=EnemyBState::Patrol&&actor.state!=EnemyBState::Turn))return {};
        float pitch=.9973541498184204f;
        if(actor.anticipationSerial){
            // AudioPlayRandom overwrites SetAudioPitch and retains that source pitch.
            // Unity's global RNG sequence is not reproduced; use audio-only entropy.
            const auto random=(s.owner^audioSerials_[i]^actor.anticipationSerial)*2654435761u;
            pitch=s.kind==SceneEnemyKind::Leaping?1.f:
                .8500000238418579f+float(random&0xffffffu)/16777215.f*(1.149999976158142f-.8500000238418579f);
        }
        return {audioSerials_[i],EnemyClip::HuskFootstep,actor.position,audioDepths_[i].value(audioSeconds_),pitch,
            actor.walkingPlaySerial()};
    }
    const auto& actor=a_[s.actor];
    if(!actor.alive())return {};
    if(s.kind==SceneEnemyKind::Vengefly)
        return {audioSerials_[i],EnemyClip::Buzzer,actor.position,audioDepths_[i].value(audioSeconds_),
            actor.audioState(0,{}).chasePitch?1.25f:1.f};
    if(s.kind==SceneEnemyKind::Hatchling)
        return {audioSerials_[i],EnemyClip::Hatchling,actor.position,audioDepths_[i].value(audioSeconds_)};
    return {audioSerials_[i],s.kind==SceneEnemyKind::Tiktik?EnemyClip::Tiktik:EnemyClip::Gruzzer,
        actor.position,audioDepths_[i].value(audioSeconds_)};
}
void SceneEnemies::killed(const Slot& s,Vec2 position,float scaleX,float rotation){
    // A persistent payout requires a recorded death, including at the ledger limit.
    if(life_&&!life_->killed(scene_,s.owner,s.reward))return;
    rewards_.push(s.reward,position,scaleX,s.scaleY,rotation);
}
void SceneEnemies::capture(SceneEnemyRenderState& out,PresentationTime time)const{
    out.count=0;
    out.mantisTitleAlpha=mantis.titleAlpha();
    out.mantisGateOffset=mantis.gateOffset();
    out.mantisHeroAnimation=mantis.heroAnimation();
    out.locked=locked();out.watcherBattle=watchers.active()||mantis.active()||brokenVessel.active();out.arena=mantis.locked()?MantisLords::arena():watchers.locked()?watchers.arena():mawlekArena_;
    out.c.capture(c_,time);
    for(unsigned index=0;index<c_.capacity;++index)out.cOwners[index]=c_.actors[index].cageOwner;
    for(unsigned i=0;i<count_;++i){const auto& s=slots_[i];if(!s.active)continue;SceneEnemyDraw d;d.bank=s.bank;d.scaleY=s.scaleY;d.owner=s.owner;
        if(s.kind==SceneEnemyKind::ElderBaldur){if(!elder_.alive())continue;
            d.animation=elder_.animation;d.position=elder_.position;d.scaleX=elder_.artScale();d.depth=.01f;}
        else if(s.kind==SceneEnemyKind::ZombieGuard){
            if(guard_.phase==ZombieGuard::Phase::Dead)continue;
            d.animation=guard_.animation;d.position=guard_.presentation.sample(guard_.position,time);d.scaleX=guard_.scaleX;
        }
        else if(s.kind==SceneEnemyKind::Spitter){
            const auto& actor=spitters_[s.actor];
            for(const auto& shot:actor.shots)if(shot.animation.clip&&!shot.animation.complete&&out.count+1<out.actors.size()){
                SceneEnemyDraw effect;effect.bank=s.bank;effect.animation=shot.animation;
                effect.position=shot.presentation.sample(shot.position,time);
                effect.rotation=std::atan2(shot.velocity.y,shot.velocity.x);
                out.actors[out.count++]=effect;
            }
            if(actor.phase==Spitter::Phase::Dead&&actor.animation.complete)continue;
            d.animation=actor.animation;d.position=actor.presentation.sample(actor.position,time);d.scaleX=actor.scaleX;
        }
        else if(s.kind==SceneEnemyKind::FungusFlyer){
            const auto& actor=flyers_[s.actor];
            for(const auto& puff:actor.gas)if(puff.active&&puff.age<.45f&&out.count+1<out.actors.size()){
                SceneEnemyDraw smoke;smoke.bank=s.bank;
                smoke.animation.play(banks_[10].require("Gas"));
                smoke.position=puff.presentation.sample(puff.position,time);
                smoke.scaleX=smoke.scaleY=.65f+1.85f*puff.age;
                out.actors[out.count++]=smoke;
            }
            if(actor.phase==FungusFlyer::Phase::Dead&&actor.animation.complete)continue;
            d.animation=actor.animation;d.position=actor.presentation.sample(actor.position,time);d.scaleX=actor.scaleX;
            if(actor.effect.clip&&!actor.effect.complete&&out.count+1<out.actors.size()){
                SceneEnemyDraw effect;effect.bank=s.bank;effect.animation=actor.effect;effect.scaleX=actor.scaleX;
                effect.position={d.position.x-.823061f*actor.scaleX,d.position.y-3.81357f};
                out.actors[out.count++]=effect;
            }
        }
        else if(isJellyfish(s.kind)){
            const auto& actor=jellyfish_[s.actor];
            if(!actor.animation.clip)continue;
            d.animation=actor.animation;d.position=actor.presentation.sample(actor.position,time);d.scaleX=actor.scaleX;
        }
        else if(s.kind==SceneEnemyKind::FungoonBaby){
            const auto& actor=fungoonBabies_[s.actor];
            if(!actor.animation.clip)continue;
            d.animation=actor.animation;d.position=actor.presentation.sample(actor.position,time);d.scaleX=actor.scaleX;
        }
        else if(s.kind==SceneEnemyKind::Crawler){const auto& a=crawlers_[s.actor];if(!a.visible())continue;
            d.animation=a.animation;d.position=a.renderPosition(time);d.scaleX=a.alive()?a.artScale():a.corpseScaleX;}
        else if(s.kind<=SceneEnemyKind::Hatchling){const auto& a=a_[s.actor];if(!a.visible())continue;
            d.animation=a.animation();d.position=a.renderPosition(time);d.scaleX=a.artScale();d.rotation=a.rotation;}
        else if(usesEnemyB(s.kind)){const auto& a=b_[s.actor];if(!a.visible())continue;
            d.animation=a.animation;d.position=a.renderPosition(time);d.scaleX=a.artScale();
            if(s.kind==SceneEnemyKind::Goam)d.rotation=s.goamRotation;}
        else{
            if(out.c.actors[s.actor].visible&&c_.actors[s.actor].audioSerial==s.cSerial)
                out.cOwners[s.actor]=s.owner;
            continue;
        }
        out.actors[out.count++]=d;
    }
    if(guard_.phase!=ZombieGuard::Phase::Dead&&guard_.swipe.clip&&!guard_.swipe.complete&&out.count<out.actors.size()){
        SceneEnemyDraw effect;
        effect.bank=9;effect.animation=guard_.swipe;effect.scaleX=guard_.scaleX;
        effect.position=guard_.presentation.sample(guard_.position,time);
        out.actors[out.count++]=effect;
    }
    if(guard_.phase!=ZombieGuard::Phase::Dead)for(unsigned index=0;index<guard_.spurts.size();++index){
        for(const auto& spurt:guard_.spurts[index]){
            if(!spurt.animation.clip||out.count==out.actors.size())continue;
            SceneEnemyDraw effect;
            effect.bank=9;effect.animation=spurt.animation;effect.position=spurt.position;
            effect.scaleX=index==0?-1.f:1.f;
            out.actors[out.count++]=effect;
        }
    }
    if(brokenVessel.visible()){
        SceneEnemyDraw d;d.bank=7;d.position=brokenVessel.renderPosition(time);d.scaleX=brokenVessel.artScale();d.animation=brokenVessel.animation;
        out.actors[out.count++]=d;
    }
    if(brokenVessel.active()){
        const auto append=[&](const auto& pool,bool balloons){for(const auto& a:pool)if(a.active()){
            SceneEnemyDraw d;d.bank=7;d.position=a.renderPosition(time);d.scaleX=d.scaleY=a.scale;d.animation=a.animation;
            if(balloons)d.scaleX=a.artScale();
            out.actors[out.count++]=d;
        }};
        append(brokenVessel.projectiles,false);append(brokenVessel.balloons,true);
        for(const auto& a:brokenVessel.scuttlers)if(a.active()&&out.count<out.actors.size()){
            SceneEnemyDraw d;d.bank=7;d.position=a.renderPosition(time);d.scaleY=a.scale;
            d.scaleX=a.artScale();d.animation=a.animation;
            if(a.state==5){
                d.scaleX=a.facing*a.scale*broken_vessel_data::splashScaleX;
                d.scaleY=a.scale*broken_vessel_data::splashScaleY;
                d.rotation=a.rotation+a.facing*broken_vessel_data::splashRotation;
            }
            out.actors[out.count++]=d;
        }
    }
    if(elderPlacement_){
        for(const auto& b:elder_.bullets)if(b.active){
            SceneEnemyDraw d;d.bank=8;d.position=b.presentation.sample(b.position,time);d.animation=b.animation;
            d.scaleX=d.scaleY=b.scale;d.rotation=b.rotation;out.actors[out.count++]=d;
        }
        const auto& r=elder_.roller;
        if(r.phase!=ElderBaldur::RollerPhase::Gone){
            SceneEnemyDraw d;d.bank=8;d.position=r.presentation.sample(r.position,time);d.animation=r.animation;
            d.scaleX=r.artScale();out.actors[out.count++]=d;
        }
    }
    if(watchers.active())for(const auto& k:watchers.knights)if(k.alive()){
        SceneEnemyDraw d;d.bank=7;d.position=k.renderPosition(time);d.scaleX=k.artScale();d.animation=k.animation;
        out.actors[out.count++]=d;
    }
    if(mantis.active()){
        // level180 Floor GO10/23 extend independently after Hero Check clears.
        for(unsigned side=0;side<2;++side){
            if(const auto* floorClip=mantis.floorClip(side)){
                SceneEnemyDraw d;d.bank=7;
                d.position={side?40.9687214f:18.9812794f,4.9160786f};
                d.scaleX=side?.99211586f:-.99211586f;
                // level180 floor depth -0.06 puts it over the spike TileMap.
                d.foreground=true;
                d.animation.play(banks_[6].require(floorClip));
                d.animation.advance(mantis.floorAnimationAge(side));
                if(d.animation.clip)out.actors[out.count++]=d;
            }
        }
        if(!mantis.completed()){
            // level179 GO975 / Transform3841, including the parent transform.
            SceneEnemyDraw d;d.bank=7;d.position={9.674f,11.001f};
            d.scaleX=1.3929787f;d.scaleY=1.3929788f;
            d.animation.play(banks_[6].require("Gate/Closed"));
            if(d.animation.clip)out.actors[out.count++]=d;
        }
        for(const auto& k:mantis.lords){
            if(!k.visible())continue;
            SceneEnemyDraw d;d.bank=7;d.position=k.presentation.sample(k.position,time);d.scaleX=k.artScale();d.animation=k.animation;
            d.depth=k.renderDepth();
            out.actors[out.count++]=d;
        }
        for(const auto& p:mantis.lances)if(p.active){
            SceneEnemyDraw d;d.bank=7;d.position=p.presentation.sample(p.position,time);d.animation=p.animation;
            d.scaleX=d.scaleY=1.5f; // level180 Shot Mantis Lord Transform139.
            out.actors[out.count++]=d;
        }
    }
}
const SceneSpawn* SceneEnemies::bossSpawn()const{
    for(const auto& e:spawns_)if(enabled(e)&&sceneEnemyKind(e.prefab)>=SceneEnemyKind::GruzMother)return &e;
    return nullptr;
}
Rect SceneEnemies::arenaFor(const SceneSpawn& e,const World& terrain)const{
    if(bossRoom_.authored)return bossRoom_.arena;
    float floor=0;const Vec2 origin{e.position.x,e.position.y+.5f};
    if(!rayTerrain(origin,{0,-1},100,terrain,floor))floor=2.5f;
    const float bottom=origin.y-floor;
    float left=18,right=18;rayTerrain({e.position.x,bottom+2},{-1,0},18,terrain,left);
    rayTerrain({e.position.x,bottom+2},{1,0},18,terrain,right);
    return {e.position.x-std::max(6.f,left),bottom,e.position.x+std::max(6.f,right),bottom+13};
}
World SceneEnemies::world(const World& terrain,const CrossroadsBoss& boss){
    World result=terrain;
    if(brokenRoom_){
        using namespace broken_vessel_data;
        solids_.clear();if(terrain.count)solids_.insert(solids_.end(),terrain.solids,terrain.solids+terrain.count);
        solids_.push_back({BrokenVesselEncounter::gate()});
        result.solids=solids_.data();result.count=terrain.count+(brokenVessel.locked()?1:0);
        if(terrain.shapeCount){
            std::copy(terrain.shapes,terrain.shapes+terrain.shapeCount,lockShapes_.begin());
            for(unsigned i=0;i<terrain.shapeCount;++i){
                auto& shape=lockShapes_[i];
                if(contains(gateColliders,shape.id))shape.enabled=false;
                if(contains(doorColliders,shape.id)&&brokenVessel.exitOpen())shape.enabled=false;
            }
            auto& shape=lockShapes_[terrain.shapeCount];shape.bounds=BrokenVesselEncounter::gate();shape.enabled=brokenVessel.locked();
            const auto r=shape.bounds;auto* v=lockVertices_.data()+shape.first;
            v[0]={r.left,r.bottom};v[1]={r.right,r.bottom};v[2]={r.right,r.top};v[3]={r.left,r.top};
            result.shapes=lockShapes_.data();result.shapeCount=terrain.shapeCount+1;result.vertices=lockVertices_.data();
        }
        return result;
    }
    const bool lock=boss.locked()&&boss.phase!=BossPhase::Dormant;
    const auto block=boss.terrainBlock();
    if(!bossRoom_.authored&&!lock&&!locked()&&!block.enabled){
        if((!battle.empty()||elderPlacement_)&&terrain.shapeCount){
            std::copy(terrain.shapes,terrain.shapes+terrain.shapeCount,lockShapes_.begin());
            for(unsigned i=0;i<terrain.shapeCount;++i)if(battle.owns(lockShapes_[i].id))lockShapes_[i].enabled=battle.closed();
            if(elderDefeated()){
                const auto collider=elderPlacement(scene_).collider;
                for(unsigned i=0;i<terrain.shapeCount;++i)if(lockShapes_[i].id==collider)lockShapes_[i].enabled=false;
            }
            result.shapes=lockShapes_.data();
        }
        return result;
    }
    const auto r=mantis.locked()?MantisLords::arena():lock?boss.arena():mawlekArena_;
    solids_.clear();if(terrain.count)solids_.insert(solids_.end(),terrain.solids,terrain.solids+terrain.count);
    solids_.push_back({{r.left-2,r.bottom-30,r.left,r.top+10}});
    solids_.push_back({{r.right,r.bottom-30,r.right+2,r.top+10}});
    if(watchers.locked())for(unsigned i=0;i<2;++i)solids_[terrain.count+i].bounds=WatcherEncounter::gate(i);
    else if(bossRoom_.authored)for(unsigned i=0;i<bossRoom_.gates.size();++i)solids_[terrain.count+i].bounds=bossRoom_.gates[i].bounds;
    result.count=solids_.size();
    if(bossRoom_.authored&&!watchers.locked())result.count=terrain.count+(lock?bossRoom_.gates.size():0);
    if(block.enabled){
        // Keep active solids contiguous even in authored rooms with fewer gates.
        if(result.count<solids_.size())solids_[result.count]={block.bounds};
        else solids_.push_back({block.bounds});
        ++result.count;
    }
    result.solids=solids_.data();
    // Contour worlds do not consume legacy AABB solids. Extend their geometry
    // too, retaining the original terrain and the contact solver settings.
    if(terrain.shapeCount){
        // Keep runtime terrain activation, then apply only source encounter objects.
        std::copy(terrain.shapes,terrain.shapes+terrain.shapeCount,lockShapes_.begin());
        for(unsigned i=0;i<terrain.shapeCount;++i)if(battle.owns(lockShapes_[i].id))lockShapes_[i].enabled=battle.closed();
        if(bossRoom_.authored)for(unsigned i=0;i<terrain.shapeCount;++i){
            auto& s=lockShapes_[i];
            for(const auto& g:bossRoom_.gates)if(s.id==g.collider)s.enabled=false;
            if(boss.floorCollapsed()&&std::find(bossRoom_.floorColliders.begin(),bossRoom_.floorColliders.end(),s.id)!=bossRoom_.floorColliders.end())s.enabled=false;
        }
        for(unsigned i=0;i<3;++i){
            auto& s=lockShapes_[terrain.shapeCount+i];
            if(i==2){
                const auto id=s.id,first=s.first;s=block;s.id=id;s.first=first;
            }else{
                s.bounds=solids_[terrain.count+i].bounds;s.enabled=true;
                if(bossRoom_.authored&&!watchers.locked()){
                    s.enabled=lock&&i<bossRoom_.gates.size();
                    if(i<bossRoom_.gates.size())s.bounds=bossRoom_.gates[i].bounds;
                }
            }
            auto* v=lockVertices_.data()+s.first;const auto b=s.bounds;
            v[0]={b.left,b.bottom};v[1]={b.right,b.bottom};v[2]={b.right,b.top};v[3]={b.left,b.top};
        }
        result.shapes=lockShapes_.data();result.shapeCount=terrain.shapeCount+2+unsigned(block.enabled);result.vertices=lockVertices_.data();
    }
    return result;
}
bool SceneEnemies::elderDefeated()const{
    if(!elderPlacement_)return false;
    return (elder_.bound()&&!elder_.alive())||(life_&&life_->dead(scene_,elderPlacement(scene_).owner))||
        (sceneBools_&&sceneBools_->get(SceneBoolSave::key(scene_,"Battle Scene")));
}
bool SceneEnemies::hides(unsigned owner,const CrossroadsBoss& boss)const{
    // Mantis additive opening floors and descending battle cages.
    if(mantis.active()&&(owner&0xff000000u)==0x81000000u){
        const unsigned source=owner&0x00ffffffu;
        const bool openingFloor=source==7||source==8||source==19||source==20;
        return openingFloor?mantis.floorRetracted(source>=19?1:0):!mantis.sceneryVisible();
    }
    if(brokenRoom_){
        using namespace broken_vessel_data;
        return contains(bossOwners,owner)||(contains(gateOwners,owner)&&!brokenVessel.locked())||
               (contains(doorOwners,owner)&&brokenVessel.exitOpen());
    }
    if(elderPlacement_){
        const auto placement=elderPlacement(scene_);
        if((owner==placement.owner||owner==placement.terrainOwner)&&elderDefeated())return true;
    }
    if(!watchers.locked()&&battle.hides(owner))return true;
    if(!bossRoom_.authored)return false;
    if(boss.phase==BossPhase::Dormant||!boss.locked())for(const auto& g:bossRoom_.gates)
        if(std::find(g.owners.begin(),g.owners.end(),owner)!=g.owners.end())return true;
    if(std::find(bossRoom_.brokenFloorOwners.begin(),bossRoom_.brokenFloorOwners.end(),owner)!=
       bossRoom_.brokenFloorOwners.end())return !boss.floorCollapsed();
    return boss.floorCollapsed()&&std::find(bossRoom_.floorOwners.begin(),bossRoom_.floorOwners.end(),owner)!=bossRoom_.floorOwners.end();
}
}
