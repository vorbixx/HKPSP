#include "waterways_pump.hpp"
#include "tutorial.hpp"
#include "world_stag_unlock.hpp"
#include "npc_shaman.hpp"
#include "graphics_profile.hpp"
#include "map_view.hpp"
#include "performance_sections.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds
{
bool TutorialSession::bindSavedCityCrest(const char* directory)
{
    CityCrestRoomData data;return data.load(directory)&&bindSavedCityCrest(data)&&cityCrestDust.load(directory);
}
bool TutorialSession::bindSavedCityCrest(const CityCrestRoomData& data)
{
    if(!terrain_||std::strcmp(sceneName(),"Fungus2_21"))return false;
    const CityCrestSaveHooks hooks{this,
        [](void* p){auto& s=*static_cast<TutorialSession*>(p);return !s.playerData_.get("openedCityGate")&&
            (s.playerData_.get("hasCityKey")||(s.bossFlags_&TutorialSave::cityCrestFlag));},
        [](void* p){return static_cast<TutorialSession*>(p)->playerData_.get("openedCityGate")!=0;},
        [](void* p){return static_cast<TutorialSession*>(p)->playerData_.get("cityGateClosed")!=0;},
        [](void* p){
            auto& s=*static_cast<TutorialSession*>(p);
            if(s.playerData_.get("openedCityGate")||(!s.playerData_.get("hasCityKey")&&!(s.bossFlags_&TutorialSave::cityCrestFlag)))return false;
            auto next=s.playerData_;
            return next.set("hasCityKey",0)&&next.set("openedCityGate",1)&&s.commitPlayerData(next);
        },
        [](void* p){
            auto& s=*static_cast<TutorialSession*>(p);if(s.playerData_.get("cityGateClosed"))return false;
            auto next=s.playerData_;
            if(!next.set("cityGateClosed",1)||!next.set("mapZone",int(s.cityCrest.data().mapZone))||!s.commitPlayerData(next))return false;
            const auto point=s.cityCrest.data().respawn;
            std::snprintf(s.resting.saved.benchScene,sizeof(s.resting.saved.benchScene),"%s",s.sceneName());
            s.resting.saved.benchX=point.x;s.resting.saved.benchY=point.y;return true;
        }};
    if(!cityCrest.bind(data,hooks,*terrain_,simulation.hero))return false;
    if(!std::strcmp(sceneSave_.gate,"bench")&&resting.saved.benchX==data.respawn.x&&resting.saved.benchY==data.respawn.y)
        simulation.hero.facingRight=true;
    return true;
}
bool TutorialSession::bindSavedCityToll(const char* directory,const CityLiftArtAssets* presentation)
{
    CityTollData data;CityLiftData lift;CityTransportData travel;
    const bool lower=!std::strcmp(sceneName(),"Crossroads_49b");
    if(!rules_||!terrain_||(!lower&&std::strcmp(sceneName(),"Crossroads_49"))||!lift.load(directory)||
       !travel.load(directory))return false;
    if(presentation)cityLiftArt_=*presentation;
    else if(!cityLiftArt_.load(directory))return false;
    const auto save=worldTollSaveHooks();
    if(lower&&(!data.load(directory)||!cityToll.bind(data,save)))return false;
    if(!cityLift.bind(lift,*terrain_,save.opened(save.context,"cityLift1"),simulation.hero)||!cityLift.bindArt(cityLiftArt_)||
       !cityTransport.bind(travel)){
        cityToll.clear();cityLift.clear();cityTransport.clear();return false;}
    cityLeverAttack_=0;
    if(playerData_.get("liftArrival")){
        // Consume the arrival marker only after the in-memory transport and
        // lift are ready. A failed bind must leave the retry marker intact.
        auto staged=playerData_;
        if(!staged.set(SavedPlayerData::key("liftArrival"),0)||!cityTransport.arrive())return false;
        const float previousLiftY=cityLift.y();
        auto& h=simulation.hero;
        if(!cityLift.travelY(cityTransport.y(),h,false)){
            cityTransport.cancelArrival();
            return false;
        }
        cityLift.travelDoors(true);
        if(!commitPlayerData(staged)){
            cityLift.travelDoors(false);cityLift.travelY(previousLiftY,h,false);cityTransport.cancelArrival();
            return false;
        }
        h.position={travel.x()+travel.heroOffset().x,cityTransport.y()+travel.heroOffset().y};
        h.velocity={};h.gravityEnabled=h.controlEnabled=false;h.presentation.reset(h.position);
    }
    return true;
}
bool TutorialSession::bindSavedStagStation(const char* directory)
{
    if(!rules_||!terrain_||std::strcmp(sceneName(),"Crossroads_47"))return false;
    if(!stagToll.bind(worldTollSaveHooks(),&resting.saved)||!stagToll.loadArt(directory)){
        stagToll.clear();return false;
    }
    return true;
}
StagBellHooks TutorialSession::stagLeverSaveHooks()
{
    return {this,
        [](void* p){return static_cast<TutorialSession*>(p)->playerData_.get("openedRestingGrounds")!=0;},
        [](void* p){
            auto& session=*static_cast<TutorialSession*>(p);
            if(session.playerData_.get("openedRestingGrounds"))return true;
            auto next=session.playerData_;
            return stageStagStationUnlock(next,"openedRestingGrounds")&&session.commitPlayerData(next);
        }};
}
bool TutorialSession::bindSavedStagLever(const char* directory)
{
    return rules_&&terrain_&&!std::strcmp(sceneName(),StagStationLever::scene)&&
        stagLever.load(directory,worldSceneBoolSaveHooks(),stagLeverSaveHooks(),&resting.saved);
}
ShamanPillarHooks TutorialSession::shamanPillarSaveHooks()
{
    return {this,
        [](void* p,unsigned which){
            const auto& s=*static_cast<TutorialSession*>(p);
            if(which==2)return s.playerData_.get("crossroadsInfected")!=0;
            return s.sceneSave_.sceneBools.get(SceneBoolSave::key(ShamanPillar::scene,which?"Raising Pillar":"Gate Switch"))||
                (!which&&s.playerData_.get("shamanPillar")!=0);
        },
        [](void* p,unsigned which){
            auto& s=*static_cast<TutorialSession*>(p);
            auto scenes=s.sceneSave_.sceneBools;auto player=s.playerData_;
            if(!stageShamanPillarSave(scenes,player,which)||(!which&&!s.commitPlayerData(player)))return false;
            s.sceneSave_.sceneBools=scenes;++s.saveRevision;return true;
        }};
}
bool TutorialSession::bindSavedShamanPillar(const char* directory)
{
    return terrain_&&rules_&&!std::strcmp(sceneName(),ShamanPillar::scene)&&
        shamanPillar.load(directory,*terrain_,shamanPillarSaveHooks());
}
bool TutorialSession::bindSavedMantisGates(const char* directory)
{
    return terrain_&&rules_&&!std::strcmp(sceneName(),MantisGates::scene)&&
        mantisGates.load(directory,*terrain_,worldSceneBoolSaveHooks());
}
void TutorialSession::cityLiftPhysics(float seconds)
{
    auto& h=simulation.hero;cityLift.fixedUpdate(seconds,h);
    const bool was=cityTransport.locked();cityTransport.update(seconds);
    if(was||cityTransport.locked()){
        cityLift.travelY(cityTransport.y(),h,true);
        if(cityTransport.locked()){h.gravityEnabled=false;h.controlEnabled=false;h.velocity={};}
        else {cityLift.travelDoors(false);h.gravityEnabled=true;}
    }
}
bool TutorialSession::bindWorldSwitch(const WorldSwitchRoomData& data,const AnimationBank& artwork,SceneBoolHooks save)
{
    if(!terrain_||!data.valid()||std::strcmp(data.scene,sceneName()))return false;
    WorldSwitchVisuals visuals;
    if(!visuals.prepare(data,artwork)||!worldSwitch.bind(data.binding(),save,*terrain_,artwork))return false;
    worldSwitchVisuals=std::move(visuals);worldSwitchVisuals.update(worldSwitch);return true;
}
bool TutorialSession::bindSavedTownLift(const TownLiftRoomData& data,const AnimationBank& art)
{
    return terrain_&&sceneActive_&&!std::strcmp(data.scene,sceneName())&&
        townLift.bind(data,art,*terrain_,worldLiftSaveHooks(),
            worldSceneBoolSaveHooks(),simulation.hero.position.y);
}
bool TutorialSession::bindSavedWaterways(const char* directory)
{
    WaterwaysRoomData data;
    return data.load(directory)&&waterwaysArt_.load(directory)&&bindSavedWaterways(data)&&waterways.bindArt(waterwaysArt_);
}
bool TutorialSession::bindSavedWaterways(const WaterwaysRoomData& data)
{
    return terrain_&&rules_&&!std::strcmp(sceneName(),"Ruins1_05b")&&waterways.bind(data,worldKeyDoorSaveHooks(),*terrain_);
}
bool TutorialSession::bindSavedJijiDoor(const char* directory)
{
    JijiDoorRoomData data;
    return data.load(directory)&&bindSavedJijiDoor(data);
}
bool TutorialSession::bindSavedJijiDoor(const JijiDoorRoomData& data)
{
    if(!rules_||std::strcmp(sceneName(),"Town")||!data.valid())return false;
    bool gateFound=false;
    for(const auto& gate:rules_->gates)if(!std::strcmp(gate.name,data.gate))gateFound=true;
    if(!gateFound||!jijiDoor.bind(data,worldKeyDoorSaveHooks()))return false;
    flow.setGateFilter(this,[](const void* context,unsigned index){
        const auto& session=*static_cast<const TutorialSession*>(context);
        return session.rules_&&index<session.rules_->gates.size()&&session.jijiDoor.gateEnabled(session.rules_->gates[index]);
    });
    return true;
}
static_assert(TutorialSave::hornetFlag==GreenpathEncounter::hornetDefeatedFlag,"Hornet save identity");
constexpr unsigned furySaveFlags=FuryChest::openedFlag|FuryChest::gotCharm6Flag|FuryChest::hasCharmFlag;
constexpr unsigned promptSaveFlags=TutorialPrompts::seenFocusFlag|TutorialPrompts::focusPromptedFlag|TutorialPrompts::interactFlag;
static_assert((furySaveFlags&promptSaveFlags)==0,"Fury and tutorial saves must be independent");
static_assert(((7u|furySaveFlags|promptSaveFlags)&OpeningCredits::playedFlag)==0,"opening credits save is independent");
static_assert(((7u|furySaveFlags|promptSaveFlags|OpeningCredits::playedFlag)&AreaTitle::visitedCrossroadsFlag)==0,"area and room save flags must be independent");
static_assert((7u|furySaveFlags|promptSaveFlags|OpeningCredits::playedFlag|AreaTitle::visitedCrossroadsFlag|TutorialSave::secretFlags|TutorialSave::bossFlags|TutorialSave::abilityFlags)==TutorialSave::validFlags,"save flag admission matches all producers");
static_assert(((7u|furySaveFlags|promptSaveFlags|OpeningCredits::playedFlag|AreaTitle::visitedCrossroadsFlag|TutorialSave::secretFlags)&TutorialSave::bossFlags)==0,"boss save flags must be independent");
static_assert((TutorialSave::bossFlags&TutorialSave::abilityFlags)==0,"boss defeats and abilities must save independently");
bool TutorialSession::bind(const RoomConfig& rules,TerrainBank& terrain,const AnimationBank& heroBank,
                           const AnimationBank& crawlerBank,const CrawlerConfig& crawlerConfig,
                           const NailBank& nails,const RoomEntityBank& entities,EnemyRandom random,const GreatDoorConfig* greatDoorConfig,const GeoConfig* geoConfig,const AnimationBank* buzzerBank,const DecorBank* decorBank,float viewportAspect,bool visitedDirtmouth,const PlantBank* plantBank,const ShadeConfig* shadeConfig,const AnimationBank* shadeBank)
{
    worldSwitch.clear();worldSwitchVisuals.clear();shamanPillar.clear();mantisGates.clear();townLift.clear();jijiDoor.clear();cityCrest.clear();cityCrestDust.clear();waterways.clear();cityToll.clear();stagToll.clear();stagLever.clear();cityLift.clear();cityTransport.clear();flow.setGateFilter(nullptr,nullptr);
    if(!enemyLife.bindPersistent(sceneSave_.sceneBools))return false;
    sceneActive_=false;roomBreakables.clear();sceneWalls.clear();roomMasks.clear();archiveShortcut={};flow.setCameraLockFilter(nullptr,nullptr);
    abilityPickup={};
    if(!std::isfinite(viewportAspect)||viewportAspect<=0)return false;
    if(rules.crawlers.size()>crawlers.size()||rules.doors.size()>doors.size()||rules.reveals.size()>revealFade_.size()||rules.remasks.size()>remaskFade_.size()||rules.enemyMessages.size()>messageContacts_.size()||rules.secrets.size()>secretCues.size())return false;
    for(const auto& source:rules.crawlers){auto config=crawlerConfig;config.colliderCount=source.count;
        if(!Crawler::canBind(config,crawlerBank,random,source.scale))return false;}
    rules_=&rules;terrain_=&terrain;heroBank_=&heroBank;crawlerBank_=&crawlerBank;
    nails_=&nails;entities_=&entities;crawlerConfig_=crawlerConfig;random_=random;greatDoorConfig_=greatDoorConfig;
    greenpath.setRandom(random);
    geoConfig_=geoConfig;buzzerBank_=buzzerBank;decorBank_=decorBank;
    plantBank_=plantBank;
    shadeConfig_=shadeConfig;shadeBank_=shadeBank;
    viewportAspect_=viewportAspect;visitedDirtmouth_=visitedDirtmouth;
    if(decorBank_ && decorBank_->count>decor.size())return false;
    if(plantBank_ && plantBank_->count>plantBreaks.size())return false;
    journal={};journal[1].remaining=45;
    floorConfig_=rules.breakFloor;floorRange_=rules.breakFloorRange;
    if(greatDoorConfig_)
    {
        if(std::strlen(greatDoorConfig_->transition.scene)>=sizeof(greatDoorExit_.scene)||
           std::strlen(greatDoorConfig_->transition.gate)>=sizeof(greatDoorExit_.entry))return false;
        std::snprintf(greatDoorExit_.name,sizeof(greatDoorExit_.name),"Great Door");
        std::memcpy(greatDoorExit_.scene,greatDoorConfig_->transition.scene,std::strlen(greatDoorConfig_->transition.scene)+1);
        std::memcpy(greatDoorExit_.entry,greatDoorConfig_->transition.gate,std::strlen(greatDoorConfig_->transition.gate)+1);
        greatDoorExit_.delay=greatDoorConfig_->transition.entryDelay;
    }
    return restart();
}
void TutorialSession::applySceneActivation()
{
    if(!rules_)return;
    // These original PlayerData fields have dedicated versioned save storage.
    // Feed their authoritative values to the same predicates as ordinary PD.
    const auto& conditional=conditionalEvents.saved();
    const SavedPlayerData::Entry overrides[]={
        {SavedPlayerData::key("falseKnightDefeated"),((bossFlags_|sceneSave_.flags)&TutorialSave::falseKnightFlag)!=0||playerData_.get("falseKnightDefeated")!=0},
        {SavedPlayerData::key("hornet1Defeated"),(bossFlags_&TutorialSave::hornetFlag)!=0||playerData_.get("hornet1Defeated")!=0},
        {SavedPlayerData::key("hasCityKey"),(playerData_.get("hasCityKey")!=0||
            (bossFlags_&TutorialSave::cityCrestFlag)!=0)&&!playerData_.get("openedCityGate")},
        {SavedPlayerData::key("hasDash"),hasAbility(AbilityReward::MothwingCloak)},
        {SavedPlayerData::key("hasWalljump"),hasAbility(AbilityReward::MantisClaw)},
        {SavedPlayerData::key("hasDoubleJump"),hasAbility(AbilityReward::MonarchWings)},
        {SavedPlayerData::key("hasSuperDash"),hasAbility(AbilityReward::CrystalHeart)},
        {SavedPlayerData::key("hasAcidArmour"),hasAbility(AbilityReward::IsmasTear)},
        {SavedPlayerData::key("hasShadowDash"),hasAbility(AbilityReward::ShadeCloak)},
        {SavedPlayerData::key("hasDreamNail"),hasAbility(AbilityReward::DreamNail)},
        {SavedPlayerData::key("fireballLevel"),hasAbility(AbilityReward::ShadeSoul)?2:hasAbility(AbilityReward::VengefulSpirit)?1:0},
        {SavedPlayerData::key("mrMushroomState"),static_cast<std::int32_t>(conditional.mrMushroomState)},
        {SavedPlayerData::key("grimmChildLevel"),static_cast<std::int32_t>(conditional.grimmChildLevel)},
        {SavedPlayerData::key("flamesCollected"),static_cast<std::int32_t>(conditional.flamesCollected)},
        {SavedPlayerData::key("flamesRequired"),static_cast<std::int32_t>(conditional.flamesRequired)},
        {SavedPlayerData::key("nailSmithUpgrades"),static_cast<std::int32_t>(conditional.nailSmithUpgrades)}};
    activation_.reset(rules_->activation,playerData_,overrides,sizeof(overrides)/sizeof(overrides[0]),&sceneSave_.sceneBools);
    if(terrain_)for(const auto& target:rules_->activation.targets)
        if(target.kind==SceneActivationTarget::Collider)
            terrain_->setEnabled(target.id,activation_.enabled(target.kind,target.id));
    flow.setActivation(&activation_);
    if(terrain_&&!std::strcmp(sceneName(),"Ruins1_05b"))
        terrain_->setEnabled(1807,playerData_.get("openedWaterwaysManhole")==0);
    if(terrain_&&!std::strcmp(sceneName(),shamanquest::scene))
        terrain_->setEnabled(shamanquest::boneGateCollider,playerData_.get("shaman")<shamanquest::gateOpenedState);
    sceneWalls.applyCollision();
    cityLift.applyCollision();cityCrest.applyCollision();
    if(terrain_)terrain_->applyLeverCollision();
    if(terrain_)archiveShortcut.applyCollision(*terrain_);
    // Mines_31 mine_break_bridge Control6543: SUPERDASH BLAST disables its
    // BoxCollider2D4354; an owned Heart destroys the bridge on scene entry.
    if(terrain_&&!std::strcmp(sceneName(),"Mines_31")&&
       (hasAbility(AbilityReward::CrystalHeart)||abilityPickup.heartBlasted))
        terrain_->setEnabled(4354,false);
    abilityPickup.available=abilityPickup.kind!=AbilityPickup::Kind::Cloak||cloakPickupUnlocked();
    if(abilityPickup.kind==AbilityPickup::Kind::SimpleKey)
        abilityPickup.collected=sceneSave_.sceneBools.get(SceneBoolSave::key("Ruins1_17","level97-222"));
    if(abilityPickup.kind==AbilityPickup::Kind::CityCrest){
        abilityPickup.available=((bossFlags_|sceneSave_.flags)&TutorialSave::falseKnightFlag)||playerData_.get("falseKnightDefeated");
        abilityPickup.collected=(bossFlags_&TutorialSave::cityCrestFlag)||playerData_.get("hasCityKey")||playerData_.get("openedCityGate");
    }
}
bool TutorialSession::cloakPickupUnlocked() const
{
    // Fungus1_04, level132-5165: the cloak group opens after Hornet.
    return ((bossFlags_&TutorialSave::hornetFlag)!=0||playerData_.get("hornet1Defeated")!=0)&&
           !sceneSave_.sceneBools.get(SceneBoolSave::key("Fungus1_04","Shiny Item"));
}
void TutorialSession::enterAbilityPickup()
{
    abilityPickup.enter(sceneName(),hasAbility(AbilityReward::VengefulSpirit),
                        hasAbility(AbilityReward::MothwingCloak),collectibles.saved());
    abilityPickup.available=abilityPickup.kind!=AbilityPickup::Kind::Cloak||cloakPickupUnlocked();
    if(abilityPickup.kind==AbilityPickup::Kind::SimpleKey)
        abilityPickup.collected=sceneSave_.sceneBools.get(SceneBoolSave::key("Ruins1_17","level97-222"));
    if(abilityPickup.kind==AbilityPickup::Kind::CityCrest){
        abilityPickup.available=((bossFlags_|sceneSave_.flags)&TutorialSave::falseKnightFlag)||playerData_.get("falseKnightDefeated");
        abilityPickup.collected=(bossFlags_&TutorialSave::cityCrestFlag)||playerData_.get("hasCityKey")||playerData_.get("openedCityGate");
    }
    enterDreamNail();
}
bool TutorialSession::setPlayerData(const char* field,std::int32_t value)
{
    if(!field||!field[0])return false;
    return setPlayerData(SavedPlayerData::key(field),value);
}
bool TutorialSession::slyStockVisible(unsigned index) const
{
    if(index>=slyItems.size()||std::strcmp(sceneName(),"Room_shop")||!playerData_.get("slyRescued"))return false;
    const auto& item=slyItems[index];
    if(playerData_.get(item.flag)||(item.prerequisite[0]&&!playerData_.get(item.prerequisite)))return false;
    if(!std::strcmp(item.flag,"gotCharm_1")&&charmInventory.has(1))return false;
    if(!std::strcmp(item.flag,"gotCharm_4")&&charmInventory.has(4))return false;
    return true;
}
bool TutorialSession::buySlyItem(unsigned index)
{
    if(!slyStockVisible(index))return false;
    const auto& item=slyItems[index];
    if(geo<item.price)return false;
    auto nextData=playerData_;
    auto nextProgress=collectibles.saved();
    if(!nextData.set(item.flag,1))return false;
    switch(item.reward){
    case 0:case 2:break;
    case 1:++nextProgress.maskShards;break;
    case 3:++nextProgress.vesselFragments;break;
    case 10:{const auto keys=nextData.get("simpleKeys");
        if(keys<0||keys>=999||!nextData.set("simpleKeys",keys+1))return false;
        break;}
    case 11:++nextProgress.rancidEggs;break;
    default:return false;
    }
    if(!nextProgress.valid())return false;
    const auto previousData=playerData_;
    playerData_=nextData;applySceneActivation();
    if(terrain_&&!sceneEnemies.updateActivation(activation_,terrain_->world())){
        playerData_=previousData;applySceneActivation();
        sceneEnemies.updateActivation(activation_,terrain_->world());return false;
    }
    collectibles.restore(nextProgress,simulation.hero.vitals);
    if(!std::strcmp(item.flag,"gotCharm_1"))charmInventory.owned|=charmBit(1);
    if(!std::strcmp(item.flag,"gotCharm_4"))charmInventory.owned|=charmBit(4);
    geo-=item.price;++saveRevision;return true;
}
bool TutorialSession::buyCharm()
{
    if(!charmStockAvailable())return false;
    if(charmsPage.selected>=charmDefinitions.size())return false;
    const auto id=charmDefinitions[charmsPage.selected].id;
    if(id==1||id==4)return buySlyItem(id==1?4:5);
    const bool changed=charmInventory.buy(charmsPage.selected,geo,sceneName());
    if(changed)++saveRevision;
    return changed;
}
void TutorialSession::updateSlyShop(const Input& input)
{
    auto& shop=slyShop;auto& hero=simulation.hero;
    if(std::strcmp(sceneName(),"Room_shop")||!playerData_.get("slyRescued")){shop.reset();return;}
    shop.count=0;
    for(unsigned i=0;i<slyItems.size();++i)if(slyStockVisible(i))shop.stock[shop.count++]=i;
    if(shop.selected>=shop.count)shop.selected=0;
    const int y=vertical(input.y),x=horizontal(input.x);
    const bool interact=(y&&y!=shop.heldY)||input.isPressed(Down);
    // Original Sly Shop owner 13 and Shop Region owner 114, level17.
    shop.prompt=!shop.open&&boxSensorOverlap(bodyBounds(hero.position),shop.region)&&hero.contacts.ground;
    if(shop.open){
        hero.velocity={};
        if(input.isPressed(Attack)||input.isPressed(Dash)){
            if(shop.confirm)shop.confirm=false;
            else {shop.open=false;hero.inputEnabled=hero.controlEnabled=true;}
        }else if(shop.confirm){
            if(y&&y!=shop.heldY)shop.yes=y>0;
            else if(x&&x!=shop.heldX)shop.yes=x<0;
            if(input.isPressed(Jump)){
                if(shop.yes)shop.message=buySlyItem(shop.item())?"Purchased":"Not enough Geo or inventory full";
                shop.confirm=false;
            }
        }else{
            if(y&&y!=shop.heldY&&shop.count){shop.selected=unsigned((int(shop.selected)-y+int(shop.count))%int(shop.count));shop.message="";shop.textPage=0;shop.textPages=1;}
            if(x&&x!=shop.heldX){if(x>0&&shop.textPage+1<shop.textPages)++shop.textPage;else if(x<0&&shop.textPage)--shop.textPage;}
            if(input.isPressed(Jump)&&shop.count&&slyItems[shop.item()].price<=geo){
                shop.confirm=true;shop.yes=true;shop.message="";
            }
        }
    }else if(shop.prompt&&interact){
        shop.open=true;shop.prompt=false;shop.message="";shop.confirm=false;
        hero.velocity={};hero.inputEnabled=hero.controlEnabled=false;
    }
    shop.heldY=y;shop.heldX=x;
}
bool TutorialSession::openCityGate()
{
    if(!cityGate.active||cityGate.opened||playerData_.get("openedCityGate")||
       (!playerData_.get("hasCityKey")&&!(bossFlags_&TutorialSave::cityCrestFlag)))return false;
    auto next=playerData_;
    if(!next.set("hasCityKey",0)||!next.set("openedCityGate",1))return false;
    playerData_=next;bossFlags_&=~TutorialSave::cityCrestFlag;sceneSave_.flags&=~TutorialSave::cityCrestFlag;
    greenpath.removeSavedFlags(TutorialSave::cityCrestFlag);
    cityGate.activate();applySceneActivation();++saveRevision;return true;
}
void TutorialSession::updateCityGate(const Input& input)
{
    auto& gate=cityGate;auto& menu=gate.menu;auto& hero=simulation.hero;
    if(!gate.active)return;
    const int y=vertical(input.y),x=horizontal(input.x);
    menu.prompt=!gate.opened&&!menu.open&&hero.contacts.ground&&boxSensorOverlap(bodyBounds(hero.position),gate.inspect);
    if(menu.open){
        hero.velocity={};
        if(x&&x!=menu.heldX)menu.yes=x<0;
        bool close=input.isPressed(Attack)||input.isPressed(Dash);
        if(input.isPressed(Jump))close=menu.refusal||!menu.yes||openCityGate();
        if(close){menu.open=false;hero.inputEnabled=hero.controlEnabled=gate.phase!=CityGate::Phase::Insert;}
    }else if(menu.prompt&&((y&&y!=menu.heldY)||input.isPressed(Down))){
        menu.open=true;menu.crest=true;
        menu.refusal=!playerData_.get("hasCityKey")&&!(bossFlags_&TutorialSave::cityCrestFlag);menu.yes=true;
        menu.question=menu.refusal?gate.noKey:gate.withKey;hero.velocity={};hero.inputEnabled=hero.controlEnabled=false;
    }
    menu.heldX=x;menu.heldY=y;
}
bool TutorialSession::buyRouteToll()
{
    auto& toll=routeTolls;
    if(!toll.active()||!toll.awaitingPayment()||geo<toll.price||
       (toll.kind==2&&!playerData_.get("hasLantern")))return false;
    if(toll.kind==1){
        if(!playerData_.set("cityLift1",1))return false;
    }else if(!collectibles.recordSourceEvent(toll.paymentSource()))return false;
    geo-=toll.price;toll.activate(*terrain_);++saveRevision;return true;
}
void TutorialSession::updateRouteToll(const Input& input)
{
    auto& toll=routeTolls;auto& menu=toll.menu;auto& hero=simulation.hero;
    if(!toll.active()||(toll.kind==1&&cityToll.bound()))return;
    // Original Call Lever FSM847: nail damage + Hero Check -> Lift Move/MOVE.
    if(nails_&&toll.departureHit(hero,*nails_)&&setPlayerData("liftArrival",1)){
        requestRestTransition("Crossroads_49","elev_entrance");
        std::snprintf(restTransition_.name,sizeof(restTransition_.name),"elev_entrance");
        hero.controlEnabled=false;return;
    }
    const int y=vertical(input.y),x=horizontal(input.x);
    toll.selectMachine(bodyBounds(hero.position));
    menu.prompt=toll.awaitingPayment()&&!menu.open&&hero.contacts.ground&&toll.inRange(bodyBounds(hero.position))&&
                (toll.kind!=2||playerData_.get("hasLantern"));
    if(menu.open){
        hero.velocity={};
        if(x&&x!=menu.heldX)menu.yes=x<0;
        bool close=input.isPressed(Attack)||input.isPressed(Dash);
        if(input.isPressed(Jump)){
            close=!menu.yes||buyRouteToll();
            if(!close)menu.message="Not enough Geo or inventory full";
        }
        if(close){menu.open=false;hero.inputEnabled=hero.controlEnabled=true;}
    }else if(menu.prompt&&((y&&y!=menu.heldY)||input.isPressed(Down))){
        menu.open=true;menu.yes=true;menu.message="";hero.velocity={};hero.inputEnabled=hero.controlEnabled=false;
    }
    menu.heldX=x;menu.heldY=y;
}
bool TutorialSession::updateCartographer(float seconds,const Input& input)
{
    const bool owned=cartographer.progress.ownsCrossroads();
    const bool changed=cartographer.update(seconds,input,simulation.hero,geo);
    if(!owned&&cartographer.progress.ownsCrossroads()&&!playerData_.get("openedMapperShop"))
        return setPlayerData("openedMapperShop",1);
    if(changed)++saveRevision;
    return changed;
}
bool TutorialSession::setPlayerData(std::uint32_t field,std::int32_t value)
{
    if(!field)return false;
    if(playerData_.get(field)==value)return true;
    auto next=playerData_;
    return next.set(field,value)&&commitPlayerData(next);
}
bool TutorialSession::commitPlayerData(const SavedPlayerData& next)
{
    if(!next.valid())return false;
    if(next==playerData_)return true;
    const auto previousPlayerData=playerData_;
    playerData_=next;
    applySceneActivation();
    if(terrain_&&!sceneEnemies.updateActivation(activation_,terrain_->world())){
        playerData_=previousPlayerData;applySceneActivation();
        sceneEnemies.updateActivation(activation_,terrain_->world());
        return false;
    }
    if(next.get(SavedPlayerData::key("openedMapperShop"))!=previousPlayerData.get(SavedPlayerData::key("openedMapperShop"))){
        const bool wasLocked=cartographer.locked();
        cartographer.enter(sceneName(),next.get("openedMapperShop")!=0);
        if(wasLocked)simulation.hero.inputEnabled=simulation.hero.controlEnabled=true;
    }
    ++saveRevision;return true;
}
SceneBoolHooks TutorialSession::worldSceneBoolSaveHooks()
{
    return {this,
        [](void* p,const char* scene,const char* id){
            auto hooks=WorldSwitchSavedState<TutorialSave>::hooks(static_cast<TutorialSession*>(p)->sceneSave_);
            return hooks.read&&hooks.read(hooks.context,scene,id);
        },
        [](void* p,const char* scene,const char* id,bool value){
            auto& session=*static_cast<TutorialSession*>(p);
            auto hooks=WorldSwitchSavedState<TutorialSave>::hooks(session.sceneSave_);
            if(!hooks.read||!hooks.write)return false;
            const bool before=hooks.read(hooks.context,scene,id);
            if(!hooks.write(hooks.context,scene,id,value))return false;
            // Publish the durable lever hit immediately, including the .75s
            // before the lift's separate PlayerData unlock can be committed.
            if(before!=value)++session.saveRevision;
            return true;
        }};
}
LiftPlayerBoolHooks TutorialSession::worldLiftSaveHooks()
{
    return {this,
        [](void* p,const char* key){
            return static_cast<TutorialSession*>(p)->playerData_.get(key)!=0;
        },
        [](void* p,const char* key,bool value){
            auto& session=*static_cast<TutorialSession*>(p);
            auto next=session.playerData_;
            auto hooks=hk3ds::mineExitLiftSaveHooks(next);
            return hooks.write&&hooks.write(hooks.context,key,value)&&
                session.commitPlayerData(next);
        }};
}
JijiKeyDoorSaveHooks TutorialSession::worldKeyDoorSaveHooks()
{
    return {this,
        [](void* p,const char* key)->int{
            return static_cast<TutorialSession*>(p)->playerData_.get(key);
        },
        [](void* p,const char* key){
            return static_cast<TutorialSession*>(p)->playerData_.get(key)!=0;
        },
        [](void* p,const char* key,int delta,const char* flag){
            auto& session=*static_cast<TutorialSession*>(p);
            auto next=session.playerData_;
            auto hooks=hk3ds::jijiKeyDoorSaveHooks(next);
            return hooks.commitUnlock&&hooks.commitUnlock(hooks.context,key,delta,flag)&&
                session.commitPlayerData(next);
        }};
}
TollSaveHooks TutorialSession::worldTollSaveHooks()
{
    return {this,
        [](void* p,const char* key){return static_cast<TutorialSession*>(p)->playerData_.get(key)!=0;},
        [](void* p){return static_cast<TutorialSession*>(p)->geo;},
        [](void* p,unsigned price,const char* flag){
            auto& s=*static_cast<TutorialSession*>(p);
            if(!price||price>s.geo||!flag||!*flag||s.playerData_.get(flag))return false;
            auto next=s.playerData_;
            // FSM1077 increments stationsOpened in the same YES transaction.
            // Keep the bounded PlayerData copy atomic so a full ledger cannot
            // charge Geo or publish only half of the station unlock.
            if(!std::strcmp(flag,"openedCrossroads")){
                if(!stageStagStationUnlock(next,flag))return false;
            } else if(!next.set(flag,1))return false;
            if(!s.commitPlayerData(next))return false;
            s.geo-=price;return true;
        }};
}
bool TutorialSession::restart(bool retry)
{
    if(!rules_)return false;
    if(!retry){worldSwitch.clear();worldSwitchVisuals.clear();shamanPillar.clear();mantisGates.clear();townLift.clear();jijiDoor.clear();cityCrest.clear();cityCrestDust.clear();waterways.clear();cityToll.clear();stagToll.clear();stagLever.clear();cityLift.clear();cityTransport.clear();flow.setGateFilter(nullptr,nullptr);}
    slyShop.reset();
    doorPromptPresentation.reset();arrowPrompt.reset();
    enemyCRewards_.clear();
    if(!retry){enemyLife.clear();sceneSave_.sceneBools={};}
    if(!retry)playerData_={};
    if(!retry)collectibles.restore({},simulation.hero.vitals);
    resetWorldQuest();
    focusEffects.reset();
    if(!retry){abilityFlags_=0;watcherProgress={};uumuuProgress={};conditionalEvents.setDreamNailOwned(false);resting.saved={};}
    if(!retry)cartographer.progress={};
    cartographer.progress.visitRoom(mapRoomIndex(sceneName()));
    cartographer.enter(sceneName(),playerData_.get("openedMapperShop")!=0);
    decorEffects.reset();
    if(sceneActive_){
        resetRemasks();
        for(unsigned i=0;i<rules_->remasks.size();++i)
            if(rules_->remasks[i].startupHeroPositioned)remaskFade_[i].pause=0;
        sceneWalls.restore(playerData_);
        archiveShortcut.bind(sceneName(),*terrain_,playerData_.get(SavedPlayerData::key(archive_shortcut_data::playerData))!=0);
        if(geoConfig_&&!coins.bind(*geoConfig_,random_))return false;
        geoVisuals.reset();
        applySceneActivation();
        enterAbilityPickup();
        flow.begin(*rules_,simulation,terrain_->world(),false,areaTitle.visitedCrossroads());flow.phase=RoomPhase::Playing;
        flow.finishedEnteringScene();
        flow.enteredThrough(sceneSave_.gate);
        // Retry uses the same door FindGroundPoint step as first arrival.
        // Otherwise an authored marker inside a floor starts below its top.
        flow.groundDoorArrival(simulation.hero,terrain_->world());
        placeBenchRespawn();
        sceneWalls.enter(simulation.hero.position.x);
        sceneEnemies.restoreWatcherProgress(watcherProgress);
        sceneEnemies.restoreBrokenVesselProgress(BrokenVesselProgress::read(playerData_));
        // Restore/new-game activation must refresh the cached spawn flags too.
        if(!sceneEnemies.updateActivation(activation_,terrain_->world())||
           !sceneEnemies.restart(terrain_->world(),sceneSave_.flags))return false;
        if(!greenpath.restartScene(sceneSave_.flags))return false;
        if(!uumuu.enter(sceneAssetRoot_,sceneName(),*nails_,random_,uumuuProgress))return false;
        if(!monomon.enter(sceneAssetRoot_,sceneName(),*nails_,playerData_))return false;
        if(!roomBreakables.bind(roomBreakables.bank(),*terrain_,objects.data()+1,unsigned(objects.size()-1),this,roomBreakableHit)||
           !combat.bind(objects.data(),1+roomBreakables.count(),nails_))return false;
        decorEffects.reset();
        if(crossroadsBoss.active())crossroadsBoss.bindArt(bossBank_);
        collectibles.restore(collectibles.saved(),simulation.hero.vitals);
        resetWorldQuest();
        applyAbilities();
        if(retry){
            simulation.hero.vitals.maxSoul=shadePresent_?66:99;
            resting.applyCharms(simulation.hero);charmInventory.apply(simulation.hero);
        }
        simulation.hero.inputEnabled=simulation.hero.controlEnabled=simulation.hero.gravityEnabled=true;
        if(retry&&!reloadWorldTransport())return false;
        accumulator_=0;++audioEpoch;return true;
    }
    if(!retry){charmInventory={};shadePresent_=false;shadeGeo_=shadeHealth_=0;shadeSpawn_={};}
    deathRecorded_=false;
    shade.clear();
    conditionalEvents.reset(retry?conditionalEvents.saved():ConditionalSave{});
    ++audioEpoch;
    prompts.reset(retry?prompts.savedFlags():0);
    areaTitle.reset(retry&&areaTitle.visitedCrossroads());
    if(!retry)credits={};
    credits.begin();
    initialFall.reset();
    inspection.reset();
    decorEffects.reset();
    contactEffects.reset();
    nailInteractions.reset();
    ambientParticles.reset();
    geoVisuals.reset();
    stalactiteVisuals.reset();
    healthCocoon.reset();
    healthCocoonFx.reset();
    healthScuttlers.reset();
    roomAmbient.reset();
    darkness.reset(lightingConfig_,hasLantern());
    heroLight.reset(lightingConfig_);
    lightDamageSerial_=0;
    if(geoConfig_&&!coins.bind(*geoConfig_,random_))return false;
    for(auto& shape:terrain_->shapes)shape.enabled=true;
    furyChest.reset(retry);
    enemyC.clear();
    crawlerCount=rules_->crawlers.size();doorCount=rules_->doors.size();
    buzzerCount=entities_->buzzerCount;stalactiteCount=entities_->stalactiteCount;
    const bool savedFloor=retry&&floorBroken;
    floorBroken=savedFloor;floorHits=savedFloor?3:0;floorWait_=0;
    const unsigned decorOffset=crawlerCount+doorCount+buzzerCount+stalactiteCount+(greatDoorConfig_?1:0);
    const unsigned plantOffset=decorOffset+(decorBank_?decorBank_->count:0);
    floorIndex_=plantOffset+(plantBank_?plantBank_->count:0);
    objectCount_=floorIndex_+(floorConfig_.count?1:0)+(shadeConfig_?1:0);
    if(objectCount_>objects.size())return false;
    const auto world=terrain_->world();objects={};
    crawlerRecorded_={};buzzerRecorded_={};crawlers={};crawlerSourceDepth_={};messageContacts_={};
    crossroadsBoss.reset();if(!retry)bossFlags_=0;greenpath.clear(bossFlags_);
    for(unsigned i=0;i<crawlerCount;++i)
    {
        const auto& source=rules_->crawlers[i];objects[i].colliderEnabled=false;
        if(source.disableIfVisitedDirtmouth&&visitedDirtmouth_)continue;
        if(source.initialActive||(source.ultrawideThreshold>0&&viewportAspect_-1.7777778f>=source.ultrawideThreshold))
            if(!activateCrawler(i,0))return false;
    }
    for(unsigned i=0;i<doorCount;++i)
        if(!doors[i].bind(rules_->doors[i].collision,*terrain_,objects[crawlerCount+i],
                         retry&&(flow.progress.broken&(1u<<i))&&rules_->doors[i].persistent))return false;
    for(unsigned i=0;i<doorCount;++i)doorFade_[i]=doors[i].broken()?rules_->doors[i].fadeSeconds:0;
    revealFade_.fill(-1);
    if(!retry)secretFlags=0;
    secretCues={};
    secretActivate_.fill(-1);
    secretReload_.fill(false);
    for(unsigned i=0;i<rules_->secrets.size();++i)
        if(secretFlags&rules_->secrets[i].saveFlag){secretActivate_[i]=0;secretReload_[i]=true;}
    resetRemasks();
    for(unsigned i=0;i<buzzerCount;++i){
        if(!buzzers[i].bind(entities_->buzzers[i],objects[crawlerCount+doorCount+i],random_,buzzerBank_))return false;
        // level6 8401/8436/8444: initial world Z=0, SetZ z=.006, delay=.5.
        // restart resets the scaled room clock below, including on retry.
        buzzerSourceDepth_[i].activate(0,.006f,0,(audioEpoch<<5)^(17+i));
    }
    for(unsigned i=0;i<stalactiteCount;++i)
        if(!stalactites[i].bind(entities_->stalactites[i],objects[crawlerCount+doorCount+buzzerCount+i]))return false;
    if(greatDoorConfig_&&!greatDoor.bind(*greatDoorConfig_,*terrain_,objects[crawlerCount+doorCount+buzzerCount+stalactiteCount]))return false;
    if(!sceneActive_&&decorBank_)for(unsigned i=0;i<decorBank_->count;++i)
        if(!decor[i].bind(decorBank_->items[i],objects[decorOffset+i],this,decorHit))return false;
    if(plantBank_)for(unsigned i=0;i<plantBank_->count;++i)
        if(!plantBreaks[i].bind(plantBank_->plants[i],objects[plantOffset+i],random_,this,plantHit))return false;
    if(floorConfig_.count)
    {
        auto& object=objects[floorIndex_];object.name="Break Floor 1";object.layer=floorConfig_.layer;
        const TerrainShape* shape=nullptr;for(const auto& s:terrain_->shapes)if(s.id==floorConfig_.trigger)shape=&s;
        if(!shape)return false;
        object.bounds=shape->bounds;object.receiver=this;object.onHit=floorHit;object.colliderEnabled=!floorBroken;
        if(floorBroken)for(unsigned i=0;i<floorConfig_.count;++i)terrain_->setEnabled(floorConfig_.disabledShapes[i],false);
    }
    if(shadeConfig_){auto& object=objects[objectCount_-1];object.colliderEnabled=false;
        if(shadePresent_&&!std::strcmp(resting.saved.shadeScene,"Tutorial_01"))shade.bind(*shadeConfig_,shadeBank_,object,shadeSpawn_,int(shadeHealth_)*simulation.hero.nailDamage,random_);}
    if(!combat.bind(objects.data(),objectCount_,nails_))return false;
    combat.traceContext=this;combat.trace=combatTrace;
    if(!retry)flow={};
    // Room setup restores every terrain shape and flow reset drops the
    // activation pointer. Reapply both before begin() can ground a retry
    // against save-gated collision shapes.
    applySceneActivation();
    flow.begin(*rules_,simulation,world,retry,areaTitle.visitedCrossroads());
    // Started-on-scene / respawn paths notify GameManager before hero recovery.
    flow.finishedEnteringScene();
    death.reset();heroEffects.reset();focusEffects.reset();accumulator_=0;
    collectibles.restore(collectibles.saved(),simulation.hero.vitals);
    applyAbilities();
    if(retry){resting.applyCharms(simulation.hero);charmInventory.apply(simulation.hero);}
    // New-game placement precedes these FSM listeners. WaitForHeroInPosition
    // finishes without sending its event when the hero is already positioned,
    // leaving the concurrent authored Wait to finish Pause. Keep the existing
    // loaded-room respawn placement delivery.
    if(retry)for(unsigned i=0;i<rules_->remasks.size();++i)
        if(rules_->remasks[i].startupHeroPositioned)remaskFade_[i].pause=0;
    simulation.hero.vitals.maxSoul=shadePresent_?66:99;
    if(ambientParticles.ready())ambientParticles.prewarm({flow.camera.x,flow.camera.y});
    plants.restart();grass.restart();roomDynamics.restart();plantSeconds_=0;levelSeconds_=0;
    return !retry||reloadWorldTransport();
}
bool TutorialSession::reloadWorldTransport()
{
    if(!worldSwitch.reload()||!shamanPillar.reload(shamanPillarSaveHooks())||!mantisGates.reload()||!stagToll.reload(worldTollSaveHooks(),&resting.saved)||
       !stagLever.reload(worldSceneBoolSaveHooks(),stagLeverSaveHooks(),&resting.saved)||
       !townLift.reload(worldLiftSaveHooks(),worldSceneBoolSaveHooks(),simulation.hero)||
       !jijiDoor.reload(worldKeyDoorSaveHooks())||!cityCrest.reload(simulation.hero)||!waterways.reload(worldKeyDoorSaveHooks())||
       !cityToll.reload(worldTollSaveHooks())||
       !cityLift.reload(playerData_.get("cityLift1")!=0,simulation.hero))return false;
    if(cityTransport.bound()){const auto data=cityTransport.data();cityTransport.bind(data);cityLeverAttack_=0;}
    worldSwitchVisuals.update(worldSwitch);cityCrestDust.reset(cityCrest.sounds(),1,true);return true;
}
bool TutorialSession::activateCrawler(unsigned i,double audioSeconds)
{
    const auto& source=rules_->crawlers[i];
    if(crawlers[i].active()||(source.disableIfVisitedDirtmouth&&visitedDirtmouth_))return true;
    auto config=crawlerConfig_;config.colliderCount=source.count;
    if(!crawlers[i].bind(config,*crawlerBank_,objects[i],source.spawn,terrain_->world(),random_,source.scale))return false;
    if(source.audioInitialDepth!=0||source.audioMinimumDepth!=0)
        crawlerSourceDepth_[i].activate(source.audioInitialDepth,source.audioMinimumDepth,audioSeconds,(audioEpoch<<5)^(1+i));
    return true;
}
void TutorialSession::setViewportAspect(float aspect)
{
    if(!std::isfinite(aspect)||aspect<=0)return;
    viewportAspect_=aspect;
    if(!rules_)return;
    for(unsigned i=0;i<crawlerCount;++i)
        if(rules_->crawlers[i].ultrawideThreshold>0&&aspect-1.7777778f>=rules_->crawlers[i].ultrawideThreshold)activateCrawler(i,levelSeconds_);
}
bool TutorialSession::startGreenpathArena(unsigned kind)
{
    if(kind>9)return false;
    if(kind==9){greenpath.clear(bossFlags_);return true;}
    if(kind==8){if(!greenpath.beginHornet({28,0,52,13},bossFlags_))return false;greenpath.fixture=true;
        auto& h=simulation.hero;h.position={33,1.4f};h.velocity={};h.presentation.reset(h.position);
        h.controlEnabled=h.inputEnabled=h.gravityEnabled=true;flow.phase=RoomPhase::Playing;flow.transition=nullptr;flow.camera={40,6};
    }else{greenpath.clear(bossFlags_);const auto h=simulation.hero.position;
        const auto k=static_cast<GreenpathKind>(kind);
        const float y=greenpathProfile(k).gravity?bodyBounds(h).bottom-greenpathProfile(k).body.bottom:h.y+2;
        return greenpath.spawn(k,{h.x+4,y},-1);}
    return true;
}
bool TutorialSession::startBossArena(unsigned kind)
{
    if(kind<1||kind>2||!nails_||objectCount_+CrossroadsBoss::objectCapacity>objects.size())return false;
    // Standalone arena adapter until Crossroads room spawn data is integrated.
    // It owns only the appended combat slots; Tutorial object indices stay stable.
    for(unsigned i=0;i<objectCount_;++i)objects[i].colliderEnabled=false;
    if(!crossroadsBoss.bind(kind==1?CrossroadsBossKind::GruzMother:CrossroadsBossKind::FalseKnight,
                           {28,0,52,13},objects.data()+objectCount_,CrossroadsBoss::objectCapacity,bossBank_,bossFlags_))return false;
    if(!combat.bind(objects.data(),objectCount_+CrossroadsBoss::objectCapacity,nails_))return false;
    testFreezeEnemies=true;flow.phase=RoomPhase::Playing;flow.transition=nullptr;
    flow.presentation={};
    auto& h=simulation.hero;h.position={33,1.4f};h.velocity={};h.damageVelocity={};h.contacts={};
    h.presentation.reset(h.position);h.controlEnabled=true;h.gravityEnabled=true;
    simulation.animator.overrideClip=nullptr;flow.camera={40,6};
    return true;
}
void TutorialSession::activateContacts()
{
    const auto body=bodyBounds(simulation.hero.position);
    for(unsigned i=0;i<crawlerCount;++i)
        if(rules_->crawlers[i].contactActivation&&boxSensorOverlap(body,rules_->crawlers[i].activationTrigger))activateCrawler(i,levelSeconds_);
}
void TutorialSession::decorHit(void* context,unsigned owner,const NailHit& hit)
{
    auto& session=*static_cast<TutorialSession*>(context);
    session.decorEffects.hit(owner,hit,session.simulation.hero.position,session.terrain_->world());
}
void TutorialSession::plantHit(void* context,unsigned owner,const NailHit& hit)
{
    auto& session=*static_cast<TutorialSession*>(context);
    if(!session.plantBank_)return;
    for(unsigned i=0;i<session.plantBank_->count;++i)if(session.plantBank_->plants[i].owner==owner){
        const auto& poses=session.plantBreaks[i].state().debris();
        const std::array<float,2> z{poses[0].z,poses[1].z};
        session.decorEffects.hit(owner,hit,session.simulation.hero.position,session.terrain_->world(),true,&z);
        break;
    }
}
void TutorialSession::greenpathPhysics(void* context)
{
    auto& s=*static_cast<TutorialSession*>(context);
    const auto world=s.greenpath.fixture?s.greenpath.arenaWorld():s.terrain_->world();
    s.greenpath.fixedUpdate(float(tuning::physicsStep),s.simulation.hero,world);
}
void TutorialSession::presentationPhysics(void* context)
{
    auto& session=*static_cast<TutorialSession*>(context);
    greenpathPhysics(context);
    if(!session.testFreezeEnemies)session.enemyC.fixedUpdate(float(tuning::physicsStep),session.simulation.hero,session.terrain_->world());
    // SceneEnemies owns its boss physics callback. The isolated fixture uses
    // this callback directly; dispatching both advances room bosses twice.
    if(!session.sceneActive_&&session.crossroadsBoss.active())session.crossroadsBoss.fixedUpdate(float(tuning::physicsStep),session.simulation.hero);
    session.decorEffects.fixedUpdate(float(tuning::physicsStep),session.terrain_->world());
    session.healthCocoonFx.fixedUpdate(float(tuning::physicsStep),session.terrain_->world());
    session.death.fixedUpdate(float(tuning::physicsStep),session.terrain_->world());
    session.shade.presentationFixedUpdate(float(tuning::physicsStep));
    session.healthScuttlers.fixedUpdate(float(tuning::physicsStep),session.terrain_->world());
    session.contactEffects.fixedUpdate(float(tuning::physicsStep),session.terrain_->world());
    session.roomDynamics.fixedUpdate(float(tuning::physicsStep),session.terrain_->world(),
        boxBounds(session.simulation.hero.position,session.rules_->heroHurtbox),
        session.objects.data(),session.crawlerCount+session.doorCount+session.buzzerCount+session.stalactiteCount);
    session.roomAmbient.fixedUpdate(float(tuning::physicsStep),session.simulation.hero.position);
    if(!session.testFreezeEnemies)activationPhysics(context);
}
void TutorialSession::activationPhysics(void* context)
{
    auto& session=*static_cast<TutorialSession*>(context);
    // OnTriggerStay sends once per rigidbody per zone per fixed step, even if
    // both crawler colliders overlap. State acceptance belongs to the receiver.
    for(unsigned z=0;z<session.rules_->enemyMessages.size();++z)
    {
        const auto& zone=session.rules_->enemyMessages[z];
        std::uint16_t contacts=0;
        for(unsigned i=0;i<session.crawlerCount;++i)
            if(zone.event<2&&session.crawlers[i].alive()&&
               (zone.layers&(1u<<session.objects[i].layer))&&
               boxSensorOverlap(zone.trigger,session.crawlers[i].bounds()))
            {
                contacts|=static_cast<std::uint16_t>(1u<<i);
                // Enter has no sender callback; Stay begins on the next step.
                if(session.messageContacts_[z]&(1u<<i))
                    session.crawlers[i].command(zone.event==1,session.terrain_->world());
            }
        session.messageContacts_[z]=contacts;
    }
    // Activation runs in a collider callback. The new Rigidbody joins sensor
    // pairs on the next physics step; it cannot Enter a message zone in this
    // callback batch, nor receive that zone's Stay on the following step.
    if(!session.testHeroPinned_)session.activateContacts();
}
bool TutorialSession::testBreakDecor(unsigned owner,AttackType type)
{
    if(sceneActive_){
        if(type==AttackType::Nail)for(unsigned i=0;i<terrain_->walls.size();++i)
            if(terrain_->walls[i].root==owner)return sceneWalls.wallBreaker(i);
        NailHit hit{simulation.hero.facingRight?Strike::Right:Strike::Left,5};hit.type=type;
        return roomBreakables.testBreak(owner,hit);
    }
    if(type!=AttackType::Nail)return false;
    if(owner==2421&&floorConfig_.count){
        const unsigned before=floorHits;
        floorHit(this,{Strike::Down,5},HitOutcome::Ignored);
        return floorHits!=before;
    }
    const unsigned offset=crawlerCount+doorCount+buzzerCount+stalactiteCount+(greatDoorConfig_?1:0);
    if(!sceneActive_&&decorBank_)for(unsigned i=0;i<decorBank_->count;++i)if(decorBank_->items[i].whole==owner)
    {
        if(decor[i].broken())return false;
        auto& object=objects[offset+i];
        object.onHit(object.receiver,{simulation.hero.facingRight?Strike::Right:Strike::Left,5},HitOutcome::Damaged);
        return decor[i].broken();
    }
    if(plantBank_)for(unsigned i=0;i<plantBank_->count;++i)if(plantBank_->plants[i].owner==owner)
    {
        if(plantBreaks[i].broken())return false;
        auto& object=objects[offset+(decorBank_?decorBank_->count:0)+i];
        object.onHit(object.receiver,{simulation.hero.facingRight?Strike::Right:Strike::Left,5},HitOutcome::Damaged);
        return plantBreaks[i].broken();
    }
    return grass.cut(owner,simulation.hero,decorEffects,terrain_->world());
}
unsigned TutorialSession::testDecorBreaks() const
{
    if(sceneActive_){
        unsigned count=roomBreakables.brokenCount();
        for(const auto& wall:terrain_->walls)count+=sceneWalls.alpha(wall.root)==0;
        return count;
    }
    unsigned count=grass.cuts();
    if(!sceneActive_&&decorBank_)for(unsigned i=0;i<decorBank_->count;++i)count+=decor[i].broken();
    if(plantBank_)for(unsigned i=0;i<plantBank_->count;++i)count+=plantBreaks[i].broken();
    if(!sceneActive_&&floorConfig_.count&&floorBroken)++count;
    return count;
}
void TutorialSession::updateLighting()
{
    // Shared by Tutorial and loaded rooms. StartRecoil's damage event is
    // separate from hazard death, which must not start the HeroLight tween.
    const bool lightDamage=simulation.hero.lightDamageSerial!=lightDamageSerial_;
    lightDamageSerial_=simulation.hero.lightDamageSerial;
    heroLight.step(simulation.scaledSeconds,lightDamage,simulation.hero.actor==Actor::Dead);
    darkness.step(bodyBounds(simulation.hero.position),float(tuning::controlStep)*simulation.clock.scale,lightDamage,hasLantern());
}
void TutorialSession::advance(double seconds,InputLatch& input,const Vec2* emitterCamera)
{
    SimulationScope sessionTiming(PerformanceSection::SimSession);
    // HealthManager checks equipped and unbroken Greed before its lethal-hit payout.
    // Set this before combat: Greenpath queues from the hit callback itself.
    const bool greed=charmInventory.wears(24)&&!playerData_.get(SavedPlayerData::key("brokenCharm_24"));
    sceneEnemies.setGreed(greed);greenpath.setGreed(greed);enemyCRewards_.setGreed(greed);

    if(!rules_||!std::isfinite(seconds)||charmsPage.open)return;
    if(worldQuests.talking()&&!worldQuestCallbackAllowed()){
        worldQuestInteraction(WorldQuestInteraction::Cancel);
    }
    if(furyChest.gotCharm6())charmInventory.owned|=charmBit(6);
    syncRestCharms();
    charmInventory.apply(simulation.hero);
    if(sceneActive_){
        accumulator_+=std::clamp(seconds,0.0,tuning::controlStep*5);
        for(unsigned n=0;n<5&&accumulator_+1e-12>=tuning::controlStep;++n){
            sessionTiming.next(PerformanceSection::SimSession);
            accumulator_-=tuning::controlStep;levelSeconds_+=tuning::controlStep;updateAreaTitle(float(tuning::controlStep));
            roomMasks.advance(float(tuning::controlStep)*simulation.clock.scale);
            // Run on every room tick so dialog waits also fade the previous prompt.
            arrowPrompt.update(float(tuning::controlStep)*simulation.clock.scale,gameplayPrompt());
            const auto updateAmbient=[&](float seconds){
                if(ambientEnabled()){
                    PerformanceScope timing(PerformanceSection::AmbientUpdate);
                    ambientParticles.update(seconds,emitterCamera?*emitterCamera:Vec2{flow.camera.x,flow.camera.y});
                }
            };
            // Normal ticks update below using scaled time; waits skip that path.
            const bool waiting=flow.transition||monomon.locked()||cartographer.locked()||
                (abilityPickup.locked()&&!abilityPickup.authoredSequence());
            if(waiting&&graphicsProfile().ambientParticles)updateAmbient(float(tuning::controlStep));
            if(flow.transition){flow.advanceTransition(float(tuning::controlStep),simulation);input.consume();continue;}
            if(monomon.locked()){
                // CameraFade continues during the control-locked mask scene.
                flow.advanceArrivalFade(float(tuning::controlStep)*simulation.clock.scale);
                if(monomon.update(float(tuning::controlStep),simulation.hero,input.consume(),playerData_,hasAbility(AbilityReward::DreamNail)))++saveRevision;
                if(monomon.guardian()){
                    // Source Dream Focus keeps the Focus presentation running while
                    // control is locked. Keep its soul/healing rules out of this path.
                    auto presentation=simulation.hero;
                    const auto phase=monomon.dreamer().phase();
                    presentation.focus=phase>=MonomonPhase::SuckPause&&phase<MonomonPhase::Ending?
                        FocusPhase::Draining:FocusPhase::None;
                    simulation.animator.update(presentation,*heroBank_,float(tuning::controlStep),nails_);
                    {PerformanceScope timing(PerformanceSection::SimParticles);focusEffects.update(float(tuning::controlStep),presentation);}
                }
                if(const auto* gate=monomon.transition()){flow.transition=gate;flow.phase=RoomPhase::Transition;}
                continue;
            }
            const bool inserting=cityGate.phase==CityGate::Phase::Insert;
            if(cityGate.step(float(tuning::controlStep),*terrain_,simulation.hero)){
                setPlayerData("cityGateClosed",1);
                std::snprintf(resting.saved.benchScene,32,"Fungus2_21");
                resting.saved.benchX=cityGate.respawn.x;resting.saved.benchY=cityGate.respawn.y;
                ++saveRevision;
            }
            if(cityGate.menu.open){updateCityGate(input.consume());continue;}
            if(inserting){
                if(cityGate.phase!=CityGate::Phase::Insert)simulation.hero.inputEnabled=simulation.hero.controlEnabled=true;
                else continue;
            }
            if(slyShop.open){updateSlyShop(input.consume());continue;}
            if(routeTolls.menu.open){updateRouteToll(input.consume());continue;}
            if(routeTolls.kind==1&&cityToll.bound()&&!routeTolls.paid&&playerData_.get("cityLift1")){
                routeTolls.paid=true;routeTolls.updateArt();
            }
            routeTolls.step(float(tuning::controlStep),*terrain_);
            if(routeTolls.kind==1&&cityToll.bound()&&routeTolls.paid&&!routeTolls.art.visible)
                routeTolls.opened=true;
            if(cartographer.locked()){
                updateCartographer(float(tuning::controlStep),input.consume());
                continue;
            }
            if(abilityPickup.locked()&&!abilityPickup.authoredSequence()){
                updateAbilityPickup(float(tuning::controlStep),input.consume());
                continue;
            }
            if(greenpath.introActive()){
                const auto dialogueInput=input.consume();
                if(dialogueInput.isPressed(Jump)||dialogueInput.isPressed(Attack))greenpath.advanceIntro(simulation.hero);
            }
            simulation.hero.testInvulnerable=testInvulnerable;
            if(worldQuestDialogue.locked()||jijiDoor.controlsHero()||cityCrest.controlsHero()||waterways.controlsHero()||cityToll.controlsHero()||cityLift.controlsHero()||cityTransport.locked()){simulation.hero.controlEnabled=false;simulation.hero.velocity={worldQuestDialogue.velocity(),0};}
            const auto before=saved();
            const auto beforeRoomFlags=sceneSave_.flags;
            const unsigned collected=coins.collectedValue;
            SimulationScope worldTiming(PerformanceSection::SimWorld);
            const auto world=uumuu.world(sceneEnemies.world(terrain_->world(),crossroadsBoss));
            worldTiming.finish();
            RoomActors actors;actors.shade=&shade;actors.contactContext=this;
            actors.geo=geoConfig_?&coins:nullptr;actors.heroBox=rules_->heroHurtbox;
            actors.spellTargets=&sceneEnemies.combat();actors.encounterCombat=monomon.guardian()?&monomon.combat():(uumuu.active()?&uumuu.combat():&greenpath.combat());
            actors.heroSimulated=!testHeroPinned_;
            actors.beforePhysics=[](void* context){auto& s=*static_cast<TutorialSession*>(context);
                if(!s.testHeroPinned_)s.terrain_->fixedMotion(float(tuning::physicsStep),s.simulation.hero,s.nails_);
                // Pinning the capture body leaves autonomous gate clocks running.
                else s.terrain_->fixedLeverMotion(float(tuning::physicsStep),s.simulation.hero);
                if(!s.testHeroPinned_)s.townLift.beforePhysics(float(tuning::physicsStep),s.simulation.hero,s.rules_->heroHurtbox);
                s.cityLiftPhysics(float(tuning::physicsStep));
                // A pinned capture body must not stop the wall's delayed break or mask fade.
                s.sceneWalls.update(float(tuning::physicsStep),s.simulation.hero,s.testHeroPinned_?nullptr:s.nails_);
                s.roomMasks.update(float(tuning::physicsStep),bodyBounds(s.simulation.hero.position),
                    !s.testHeroPinned_||s.testPinnedHeroTriggers);
                s.archiveShortcut.update(float(tuning::physicsStep),s.simulation.hero,s.testHeroPinned_?nullptr:s.nails_,
                    boxBounds(s.simulation.hero.position,s.rules_->heroHurtbox));};
            actors.spellGeometry=[](void* context,Rect swept){static_cast<TutorialSession*>(context)->sceneWalls.spell(swept);};
            actors.afterPhysics=[](void* context){auto& s=*static_cast<TutorialSession*>(context);
                s.roomBreakables.fixedUpdate(float(tuning::physicsStep),s.terrain_->world());
                if(!s.testHeroPinned_)s.rules_->supportWater(s.simulation.hero,&s.activation_);
                s.townLift.afterPhysics(s.simulation.hero);
                greenpathPhysics(context);s.sceneEnemies.fixedUpdate(float(tuning::physicsStep),s.simulation.hero,s.terrain_->world(),s.testFreezeEnemies);s.uumuu.fixedUpdate(float(tuning::physicsStep),s.simulation.hero,s.testFreezeEnemies);
                s.monomon.fixedUpdate(s.simulation.hero);s.decorEffects.fixedUpdate(float(tuning::physicsStep),s.terrain_->world());};
            simulation.updateFrame(tuning::controlStep,input,world,*heroBank_,&combat,nullptr,0,actors);
            sessionTiming.next(PerformanceSection::SimInteractions);
            if(nails_){worldSwitch.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock);if(shamanPillar.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock))decorEffects.enemyKillShake();
            if(mantisGates.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock))decorEffects.enemyKillShake();}
            if(nails_&&stagLever.update(simulation.scaledSeconds,simulation.hero,*nails_))simulation.clock.enemyKill();
            if(stagLever.takeRouteChanged())++saveRevision;
            worldSwitchVisuals.update(worldSwitch);
            if(nails_){
                townLift.update(simulation.scaledSeconds,simulation.hero,rules_->heroHurtbox,*nails_,simulation.clock);
                const auto feedback=townLift.takeLeverEvents();
                for(unsigned i=0;i<feedback.count;++i){
                    if(feedback.actions[i].kind==MineLiftLeverActionKind::ShakeCamera)
                        decorEffects.enemyKillShake();
                    if(feedback.actions[i].kind==MineLiftLeverActionKind::Hit)
                        decorEffects.nailHit(townLift.hitOrigin(),townLift.hitDepth());
                }
            }
            rules_->supportWater(simulation.hero,&activation_);
            // updateLighting below advances Lantern reveals and region tweens
            // once, using accepted hero hits rather than retained combat totals.
            // Settle this physics tick before a shop or dialogue can pause the session.
            const unsigned collectedGeo=coins.collectedValue-collected;
            if(collectedGeo){geo=std::min(9999999u,geo+collectedGeo);++saveRevision;}
            decorEffects.setView({flow.camera.x,flow.camera.y},viewportAspect_);
            {PerformanceScope timing(PerformanceSection::DecorUpdate);decorEffects.visualUpdate(simulation.scaledSeconds,world);
                roomBreakables.visualUpdate(simulation.scaledSeconds,{flow.camera.x,flow.camera.y},viewportAspect_);}
            if(sceneWalls.takeChanged()){++saveRevision;applySceneActivation();}
            if(roomMasks.takeChanged())++saveRevision;
            if(sceneActive_&&terrain_&&terrain_->takeLeverChanged()){++saveRevision;applySceneActivation();}
            if(const auto field=sceneWalls.takePlayerData())setPlayerData(field,1);
            // The source lever has no Dung Defender flag test: access is physical.
            if(!std::strcmp(sceneName(),"Waterways_05")&&!playerData_.get("waterwaysAcidDrained")&&
               nails_&&waterwaysPumpHit(simulation.hero,*nails_))
                setPlayerData("waterwaysAcidDrained",1);
            if(archiveShortcut.ready()&&setPlayerData(archive_shortcut_data::playerData,1))archiveShortcut.open(*terrain_);
            updateLighting();
            {PerformanceScope timing(PerformanceSection::SimParticles);focusEffects.update(simulation.scaledSeconds,simulation.hero);}
            {PerformanceScope timing(PerformanceSection::SimEnemies);greenpath.update(simulation.scaledSeconds,simulation.hero);}
            {PerformanceScope timing(PerformanceSection::SimParticles);heroEffects.update(simulation.scaledSeconds,simulation.hero);}
            updateWorldQuest(simulation.scaledSeconds);
        cityCrestDust.update(simulation.scaledSeconds,cityCrest.sounds(),cityCrest.y());
            updateSlyShop(simulation.hero.input);
            updateRouteToll(simulation.hero.input);
            updateCityGate(simulation.hero.input);
            if(cityGate.menu.open||cityGate.phase==CityGate::Phase::Insert)continue;
            if(routeTolls.menu.open)continue;
            if(slyShop.open)continue;
            updateCartographer(float(tuning::controlStep),simulation.hero.input);
            if(cartographer.locked()){updateAmbient(simulation.scaledSeconds);continue;}
            geo=std::min(9999999u,geo+greenpath.geoReleased);greenpath.geoReleased=0;
            if((bossFlags_|greenpath.savedFlags())!=bossFlags_){
                ++saveRevision;bossFlags_|=greenpath.savedFlags();applySceneActivation();
            }
            {PerformanceScope timing(PerformanceSection::SimEnemies);sceneEnemies.update(simulation.scaledSeconds,simulation.hero,world,testFreezeEnemies,flow.camera);}
            if(sceneEnemies.mantis.victoryEarned())setPlayerData("defeatedMantisLords",1);
            if(sceneEnemies.brokenVessel.active()){
                const auto progress=sceneEnemies.brokenVessel.progress();
                const auto previous=BrokenVesselProgress::read(playerData_);
                if(progress.encountered!=previous.encountered||progress.killed!=previous.killed||progress.corniferLeft!=previous.corniferLeft){
                    if(progress.write(playerData_)){++saveRevision;applySceneActivation();}
                }
            }
            const auto previousUumuu=uumuuProgress;
            {PerformanceScope timing(PerformanceSection::SimEnemies);uumuu.update(simulation.scaledSeconds,simulation.hero,testFreezeEnemies);}
            if(uumuu.takeTitleRequest())areaTitle.resetUumuu();
            if(monomon.update(simulation.scaledSeconds,simulation.hero,simulation.hero.input,playerData_,hasAbility(AbilityReward::DreamNail)))++saveRevision;
            if(previousUumuu.encounteredMegaJelly!=uumuuProgress.encounteredMegaJelly||previousUumuu.defeatedMegaJelly!=uumuuProgress.defeatedMegaJelly)++saveRevision;
            if(sceneEnemies.watchers.active()){
                const auto progress=sceneEnemies.watchers.progress();
                if(progress.battleActivated!=watcherProgress.battleActivated||progress.watcherChandelier!=watcherProgress.watcherChandelier)++saveRevision;
                watcherProgress=progress;
            }
            if(geoConfig_){sceneEnemies.releaseRewards(coins);greenpath.releaseRewards(coins);}
            geo=std::min(9999999u,geo+sceneEnemies.takeGeo());
            if(nails_){PerformanceScope timing(PerformanceSection::SimParticles);geoVisuals.update(simulation.scaledSeconds,coins,simulation.hero,*nails_,nullptr,world);}
            if(sceneEnemies.takeFreeze())simulation.clock.enemyKill();
            sceneSave_.flags|=sceneEnemies.savedFlags()|greenpath.savedFlags();
            if(((beforeRoomFlags^sceneSave_.flags)&TutorialSave::falseKnightFlag)!=0)applySceneActivation();
            if(beforeRoomFlags!=sceneSave_.flags||before.geo!=geo||
               std::memcmp(&before.sceneBools,&sceneSave_.sceneBools,sizeof(SceneBoolSave)))++saveRevision;
            if(nails_&&collectibles.update(simulation.hero,*nails_,geo))++saveRevision;
            // RelinquishControl ignores hero input, while message FSM listeners
            // still consume the original edges. World clocks remain scaled.
            updateAbilityPickup(simulation.scaledSeconds,abilityPickup.authoredSequence()?input.lastConsumed():simulation.hero.input);
            updateWaterways(float(tuning::controlStep),simulation.hero.input);
            // level180 CamLocks/CameraLockArea (2): source max-priority intro lock.
            flow.setEncounterCamera(sceneEnemies.mantis.locked(),MantisLords::cameraLimits(simulation.hero.position));
            // Gate admission disables hero control. Check every encounter lock
            // first so rejecting an exit never leaves the hero immobilized.
            const bool canLeave=!abilityPickup.locked()&&!greenpath.locked()&&!uumuu.locked()&&!sceneEnemies.locked()&&
                (!crossroadsBoss.locked()||crossroadsBoss.phase==BossPhase::Dormant)&&!monomon.locked();
            {PerformanceScope timing(PerformanceSection::SimFlow);flow.update(float(tuning::controlStep),simulation,world,terrain_,simulation.scaledSeconds,canLeave,heroBank_);}
            updateAmbient(simulation.scaledSeconds);
            updateRemasks();
            static const RoomGate challengePrompt=[] {
                RoomGate gate{};
                // Original level180/100; approximation: bubble offset follows the room reference.
                gate.promptPosition={30.198439f,11.3723025f};
                std::strcpy(gate.prompt,"CHALLENGE");return gate;
            }();
            doorPromptPresentation.update(simulation.scaledSeconds,
                sceneEnemies.mantis.challengeAvailable(simulation.hero.position)?&challengePrompt:flow.doorPrompt());
            arrowPrompt.update(simulation.scaledSeconds,gameplayPrompt());
            if((monomon.locked()||uumuu.locked()||sceneEnemies.locked()||(crossroadsBoss.locked()&&crossroadsBoss.phase!=BossPhase::Dormant))&&flow.transition){
                flow.transition=nullptr;flow.phase=RoomPhase::Playing;
            }
            if(const auto* gate=monomon.transition()){flow.transition=gate;flow.phase=RoomPhase::Transition;}
            {PerformanceScope timing(PerformanceSection::SimEnemies);shade.update(simulation.scaledSeconds,simulation.hero,world);}
            if(shadePresent_&&shade.state==ShadeState::Recover){shadePresent_=false;geo=std::min(9999999u,geo+shadeGeo_);shadeGeo_=shadeHealth_=0;
                simulation.hero.vitals.maxSoul=99;++saveRevision;}
            auto& hero=simulation.hero;
            if(hero.position.y<rules_->cameraLimits.bottom-40){hero.actor=Actor::Dead;hero.vitals.health=0;flow.retryRequested=true;}
            if(hero.actor==Actor::Dead&&!deathRecorded_){
                enemyLife.resetSemiPersistent();
                deathRecorded_=true;shadePresent_=true;shadeGeo_=geo;geo=0;shadeHealth_=unsigned(std::max(1,hero.vitals.maxHealth/2));
                // The Shade appears where the hero died (original behaviour). Clamping to the camera
                // bottom put a death on a lower floor above that floor.
                shadeSpawn_=hero.position;
                std::snprintf(resting.saved.shadeScene,32,"%s",sceneName());shade.clear();++saveRevision;
            }
            if(!abilityPickup.locked())updateResting(input.lastConsumed());
            if(flow.retryRequested){
                if(resting.saved.benchScene[0])requestRestTransition(resting.saved.benchScene,"bench");
                else {restart(true);
                    hero.vitals.maxSoul=shadePresent_?66:99;flow.phase=RoomPhase::Playing;deathRecorded_=false;bindSceneShade();}
            }
        }syncEnemyLifeSave();return;
    }
    if(seconds<0||seconds>tuning::controlStep*5)++simulation.overloads;
    accumulator_+=std::clamp(seconds,0.0,tuning::controlStep*5);
    for(unsigned step=0;step<5&&accumulator_+1e-12>=tuning::controlStep;++step)
    {
        sessionTiming.next(PerformanceSection::SimSession);
        accumulator_-=tuning::controlStep;
        levelSeconds_+=tuning::controlStep*simulation.clock.scale;
        credits.advance(float(tuning::controlStep)*simulation.clock.scale);
        if(flow.phase==RoomPhase::Transition){flow.advanceTransition(float(tuning::controlStep),simulation);input.consume();continue;}
        const auto before=saved();
        std::array<bool,16> wasBroken{};
        for(unsigned i=0;i<doorCount;++i)wasBroken[i]=doors[i].broken();
        const unsigned collected=coins.collectedValue,doorHits=greatDoor.hits(),doorStage=greatDoor.stage();
        SimulationScope worldTiming(PerformanceSection::SimWorld);
        auto world=greenpath.fixture?greenpath.arenaWorld():(!sceneActive_&&crossroadsBoss.active()?crossroadsBoss.arenaWorld():terrain_->world());
        worldTiming.finish();
        floorWait_=std::max(0.0f,floorWait_-float(tuning::controlStep)*simulation.hero.scale);
        // Same latched scale consumed by Simulation::frameTime this control tick.
        plantSeconds_+=float(tuning::controlStep)*simulation.clock.scale;
        simulation.hero.testInvulnerable=testInvulnerable;
        simulation.lastDamageSource=0;
        const bool wasCollecting=furyChest.collecting();
        const bool wasInspecting=inspection.locked();
        if(worldQuestDialogue.locked()||jijiDoor.controlsHero()||cityCrest.controlsHero()||waterways.controlsHero()||cityToll.controlsHero()||cityLift.controlsHero()||cityTransport.locked()){simulation.hero.controlEnabled=false;simulation.hero.velocity={worldQuestDialogue.velocity(),0};}
        if(wasInspecting){auto& h=simulation.hero;h.controlEnabled=false;h.velocity={};}
        const float charmX=furyChest.shinyPosition().x;
        const float collectX=charmX+(simulation.hero.facingRight?-.25f:.25f);
        if(wasCollecting){
            auto& h=simulation.hero;h.controlEnabled=false;h.gravityEnabled=false;h.velocity={};
            if(furyChest.pickup()==FuryPickupPhase::Aligning)
                h.velocity.x=std::clamp((collectX-h.position.x)/float(tuning::controlStep),-6.f,6.f);
        }
        const auto previousSoulGain=simulation.hero.vitals.soulGainSerial;
        const auto previousFocus=simulation.hero.focus;
        simulation.updateFrame(tuning::controlStep,input,world,*heroBank_,&combat,crawlers.data(),testFreezeEnemies?0:crawlerCount,
                               {buzzers.data(),testFreezeEnemies?0:buzzerCount,stalactites.data(),testFreezeEnemies?0:stalactiteCount,geoConfig_?&coins:nullptr,rules_->heroHurtbox,this,presentationPhysics,testFreezeEnemies?nullptr:&shade,!testHeroPinned_,&greenpath.combat()});
        sessionTiming.next(PerformanceSection::SimInteractions);
        if(nails_){
            if(worldSwitch.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock))++saveRevision;
            if(shamanPillar.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock))decorEffects.enemyKillShake();
            if(mantisGates.update(simulation.scaledSeconds,simulation.hero,*nails_,simulation.clock))decorEffects.enemyKillShake();
        }
        if(nails_&&stagLever.update(simulation.scaledSeconds,simulation.hero,*nails_))simulation.clock.enemyKill();
        if(stagLever.takeRouteChanged())++saveRevision;
        worldSwitchVisuals.update(worldSwitch);
        if(nails_&&collectibles.update(simulation.hero,*nails_,geo))++saveRevision;
        if(crossroadsBoss.active()){
            {PerformanceScope timing(PerformanceSection::SimEnemies);crossroadsBoss.update(simulation.scaledSeconds);}
            const auto previousBossFlags=bossFlags_;
            bossFlags_=crossroadsBoss.savedFlags();
            if(previousBossFlags!=bossFlags_)applySceneActivation();
            Vec2 origin;
            if(geoConfig_&&crossroadsBoss.takeGruzGeo(origin))coins.spawnReward(0,0,2,origin);
        }
        if(greatDoor.hits()>doorHits)
        {
            // Scene Transform 5674 through parents 5677/6759 (source build 22529139).
            constexpr Vec2 self{190.48000015f,68.42000008f};
            constexpr float depth=.61f;
            // Approximation: use the hero anchor for the FSM's nail Damager
            // transform. The source spawn action adds its own +1 X offset.
            decorEffects.greatDoorHit(self,simulation.hero.position,depth,simulation.hero.nail.direction,world);
            if(greatDoor.stage()>doorStage)
                decorEffects.greatDoorBreak(self,depth,greatDoor.stage()==1?GreatDoorBreakStage::Break1:GreatDoorBreakStage::Break2,world);
            else if(greatDoor.activated())
                decorEffects.greatDoorBreak(self,depth,GreatDoorBreakStage::Final,world);
        }
        for(unsigned i=0;i<doorCount;++i)if(!wasBroken[i]&&doors[i].broken())
            decorEffects.ownerBreak(rules_->doors[i].owner,simulation.hero.nail.direction,world);

        {PerformanceScope timing(PerformanceSection::SimEnemies);greenpath.update(simulation.scaledSeconds,simulation.hero);}
        if((bossFlags_|greenpath.savedFlags())!=bossFlags_){
            ++saveRevision;bossFlags_|=greenpath.savedFlags();applySceneActivation();
        }
        geo=std::min(9999999u,geo+greenpath.geoReleased);greenpath.geoReleased=0;
        if(geoConfig_)greenpath.releaseRewards(coins);
        {PerformanceScope timing(PerformanceSection::SimEnemies);enemyC.update(simulation.scaledSeconds,simulation.hero,world);}
        for(unsigned i=0;i<enemyC.dropCount;++i)if(geoConfig_){const auto& d=enemyC.drops[i];
            if(d.kind==EnemyCKind::BroodingMawlek){coins.spawn(d.count,JournalEnemy::Crawler,d.position);continue;}
            const auto kind=d.kind==EnemyCKind::Hatchling?EnemyRewardKind::Hatchling:
                d.kind<=EnemyCKind::AspidMother?EnemyRewardKind(unsigned(EnemyRewardKind::Bully)+unsigned(d.kind)):EnemyRewardKind::Count;
            enemyCRewards_.push(kind,d.position,enemyC.actors[d.actor].scaleX);
        }
        enemyC.dropCount=0;
        if(geoConfig_)enemyCRewards_.release(coins);
        {PerformanceScope timing(PerformanceSection::SimParticles);heroEffects.update(simulation.scaledSeconds,simulation.hero);}
        {PerformanceScope timing(PerformanceSection::SimParticles);focusEffects.update(simulation.scaledSeconds,simulation.hero);}
        grass.update(simulation.scaledSeconds,simulation.hero,*nails_,decorEffects,world);
        {PerformanceScope timing(PerformanceSection::SimParticles);contactEffects.update(simulation.scaledSeconds,simulation.hero.contacts,decorEffects,world);}
        {PerformanceScope timing(PerformanceSection::SimParticles);stalactiteVisuals.advance(simulation.scaledSeconds,stalactites.data(),stalactiteCount,&decorEffects,world,presentationTime());}
        decorEffects.setView({flow.camera.x,flow.camera.y},viewportAspect_);
        {PerformanceScope timing(PerformanceSection::DecorUpdate);decorEffects.visualUpdate(simulation.scaledSeconds,world);}
        const Vec2 chipCamera=emitterCamera?*emitterCamera:Vec2{flow.camera.x,flow.camera.y};
        {PerformanceScope timing(PerformanceSection::SimParticles);geoVisuals.update(simulation.scaledSeconds,coins,simulation.hero,*nails_,&decorEffects,world,&chipCamera);}
        healthCocoon.update(simulation.scaledSeconds,simulation.hero,*nails_,
                            healthCocoonBreakEffect,healthCocoonEffectContext);
        if(healthCocoon.broken()&&!healthScuttlers.spawned()){
            for(const auto id:healthCocoon.disabledColliders())terrain_->setEnabled(id,false);
            healthScuttlers.spawn(healthCocoon.visual().position,healthCocoon.visual().depth);
        }
        healthScuttlers.update(simulation.scaledSeconds,simulation.hero,*nails_,&decorEffects);
        {
            auto& h=simulation.hero;const auto& c=furyChest.config();
            const auto overlaps=[](Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;};
            const Rect hero=bodyBounds(h.position);
            FuryChestInput request;
            request.nailHit=h.nail.hitbox&&nailOverlaps(h,nails_->shapes[nailVariant(h.nail)],c.nailBounds);
            if(furyChest.phase()==FuryChestPhase::Opened){
                request.nailHit=false;
                for(unsigned side=0;side<2;++side)if(h.nail.hitbox&&nailOverlaps(h,nails_->shapes[nailVariant(h.nail)],c.openedSides[side])){
                    request.nailHit=true;request.openedSide=side;break;
                }
            }
            request.heroPosition=request.nailPosition=h.position;request.direction=h.nail.direction;
            request.heroRange=overlaps(hero,c.heroRange);request.inspectRange=overlaps(hero,furyChest.inspectRange());
            request.grounded=h.contacts.ground;request.inspect=std::abs(h.input.y)>.5f;
            request.button=(h.input.pressed&(Jump|Attack|Focus|Dash))!=0;
            request.damaged=combat.events.tookDamage!=0||h.actor==Actor::Dead;
            request.aligned=std::abs(h.position.x-collectX)<.05f;
            const auto tinks=furyChest.tinkSerial(),opens=furyChest.openSerial();
            furyChest.update(simulation.scaledSeconds,request);
            if(furyChest.tinkSerial()!=tinks&&request.direction!=Strike::Down)h.reactToNailContact(request.direction);
            if(furyChest.openSerial()!=opens){
                decorEffects.enemyKillShake();
                const auto& chest=furyChest.config().parts[static_cast<unsigned>(FuryPart::Chest)];
                decorEffects.nailHit(chest.position,chest.depth);
            }
            if(furyChest.collecting()){
                h.controlEnabled=false;h.gravityEnabled=false;h.velocity={};h.focus=FocusPhase::None;h.nail={};
                const char* clip=furyChest.pickup()==FuryPickupPhase::Aligning?"Run":furyChest.pickup()==FuryPickupPhase::Lowering?"Collect Normal 1":
                    furyChest.pickup()==FuryPickupPhase::Raising?"Collect Normal 3":"Collect Normal 2";
                simulation.animator.overrideClip=heroBank_->find(clip);
                simulation.animator.player.play(simulation.animator.overrideClip);
            }else {simulation.animator.overrideClip=nullptr;
                if(wasCollecting&&!request.damaged){h.controlEnabled=true;h.gravityEnabled=true;}
            }
        }
        {
            auto& h=simulation.hero;
            const unsigned serial=inspection.serial();
            const bool can=h.controlEnabled&&h.inputEnabled&&h.contacts.ground&&!h.nail.active&&!h.dashing&&
                h.actor!=Actor::Dead&&h.actor!=Actor::Damage&&h.actor!=Actor::HardLanding&&!furyChest.collecting();
            inspection.step(simulation.scaledSeconds,{bodyBounds(h.position),can,h.input.y>.6f,h.input.isPressed(Down),
                (h.input.pressed&(Jump|Attack|Focus))!=0,combat.events.tookDamage!=0||h.actor==Actor::Dead},prompts);
            if(inspection.serial()!=serial)roomAmbient.inspect(inspection.owner());
            if(inspection.locked()){
                h.controlEnabled=false;h.velocity={};h.focus=FocusPhase::None;h.nail={};
                h.jumpQueued=h.dashQueued=h.attackQueued=-1;h.lookTime=0;
                const auto* clip=heroBank_->find(inspection.phase()==InspectionPhase::TurnBack?"TurnFromBG":"TurnToBG");
                simulation.animator.overrideClip=clip;
                simulation.animator.player.play(clip);
            }else if(wasInspecting){
                simulation.animator.overrideClip=nullptr;
                if(h.actor!=Actor::Dead)h.controlEnabled=true;
            }
        }
        updateWorldQuest(simulation.scaledSeconds);
        cityCrestDust.update(simulation.scaledSeconds,cityCrest.sounds(),cityCrest.y());
        nailInteractions.update(simulation.scaledSeconds,simulation.hero,*nails_,
            furyChest.prompt()?2550:inspection.promptOwner(),&decorEffects);
        if(geoConfig_)
        {
            geo=std::min(9999999u,geo+coins.collectedValue-collected);
        }
        updateLighting();
        updateAreaTitle(float(tuning::controlStep)*simulation.clock.scale);
        const auto b=bodyBounds(simulation.hero.position);
        const auto& h=simulation.hero;
        // Source reminder FSMs are children of Tute Door 1 and Tute Door 5.
        // Converted door order also defines save bits, not source ownership.
        const auto reminderDoorActive=[&](unsigned owner){
            for(unsigned i=0;i<doorCount;++i)
                if(rules_->doors[i].owner==owner)return !doors[i].broken();
            return false;
        };
        prompts.step(float(tuning::controlStep)*simulation.clock.scale,
            {{b.left,b.bottom,b.right,b.top},h.position.x,h.vitals.health,h.vitals.maxHealth,h.vitals.soul,
             combat.events.tookDamage!=0,h.vitals.soulGainSerial!=previousSoulGain,h.input.isPressed(Attack),
             h.focus==FocusPhase::Draining&&previousFocus!=FocusPhase::Draining,
             reminderDoorActive(2208),reminderDoorActive(3359),visitedDirtmouth_});
        flow.progress.hits+=combat.events.tookDamage;flow.progress.kills+=combat.events.zeroHP;combat.events={};
        for(unsigned i=0;i<doorCount;++i)if(doors[i].broken())flow.progress.broken |= 1u<<i;
        const bool wasCollapsed=flow.progress.collapsed;
        {PerformanceScope timing(PerformanceSection::SimFlow);flow.update(tuning::controlStep,simulation,world,terrain_,simulation.scaledSeconds,
                    !greenpath.locked()&&!crossroadsBoss.active(),heroBank_);}
        doorPromptPresentation.update(simulation.scaledSeconds,flow.doorPrompt());
        arrowPrompt.update(simulation.scaledSeconds,gameplayPrompt());
        if(crossroadsBoss.active()){
            flow.camera={40,crossroadsBoss.floorCollapsed()?0.f:6.f};
            if(simulation.hero.actor!=Actor::Dead){flow.phase=RoomPhase::Playing;flow.transition=nullptr;}
        }
        roomDynamics.observeCrumble(flow.crumbling(),flow.progress.collapsed);
        if(!wasCollapsed&&flow.progress.collapsed)decorEffects.ownerBreak(318,Strike::Down,terrain_->world());
        const auto oldFallCues=initialFall.cues();
        initialFall.update(flow.phase,float(tuning::controlStep)*simulation.clock.scale);
        if(initialFall.cues()[0]!=oldFallCues[0])decorEffects.initialFall(false,simulation.hero.position,world);
        if(initialFall.cues()[1]!=oldFallCues[1])decorEffects.initialFall(true,simulation.hero.position,world);
        if(flow.phase==RoomPhase::Dead){
            if(!death.active()){
                enemyLife.resetSemiPersistent();
                death.begin(simulation.hero,*heroBank_);decorEffects.enemyKillShake();
                // Native Animators/ParticleSystems sample the activation frame
                // before FreezeMoment takes effect. Its physics callback has
                // already run, so give the newly active presentation that sample.
                death.fixedUpdate(simulation.scaledSeconds,world);
            }
            death.update(simulation.scaledSeconds);
        }
        if(shadeConfig_){
            if(!testFreezeEnemies){PerformanceScope timing(PerformanceSection::SimEnemies);shade.update(simulation.scaledSeconds,simulation.hero,world);}
            if(shadePresent_&&shade.state==ShadeState::Recover){
                shadePresent_=false;geo=std::min(9999999u,geo+shadeGeo_);shadeGeo_=shadeHealth_=0;
                simulation.hero.vitals.maxSoul=99;
            }
            if(simulation.hero.actor==Actor::Dead&&!deathRecorded_){
                deathRecorded_=true;shadePresent_=true;shadeGeo_=geo;geo=0;
                shadeHealth_=unsigned(std::max(1,simulation.hero.vitals.maxHealth/2));
                shadeSpawn_=shadeConfig_->spawnPosition(simulation.hero.position);
                std::strcpy(resting.saved.shadeScene,"Tutorial_01");
                simulation.hero.vitals.maxSoul=66;simulation.hero.vitals.soul=simulation.hero.vitals.reserve=0;
                shade.clear();
            }
        }
        if(graphicsProfile().ambientParticles){PerformanceScope timing(PerformanceSection::AmbientUpdate);
            ambientParticles.update(simulation.scaledSeconds,emitterCamera?*emitterCamera:Vec2{flow.camera.x,flow.camera.y});}
        for(unsigned i=0;i<doorCount;++i)if(wasBroken[i])
            doorFade_[i]=std::min(rules_->doors[i].fadeSeconds,doorFade_[i]+float(tuning::controlStep)*simulation.hero.scale);
        for(unsigned i=0;i<rules_->reveals.size();++i)
        {
            const auto& reveal=rules_->reveals[i];auto& elapsed=revealFade_[i];
            if(elapsed>=0)elapsed=std::min(reveal.seconds,elapsed+float(tuning::controlStep)*simulation.hero.scale);
            else if(!testHeroPinned_||testPinnedHeroTriggers)for(const auto trigger:reveal.triggers)
                if(roomOverlap(bodyBounds(simulation.hero.position),trigger)){elapsed=0;break;}
        }
        for(unsigned i=0;i<rules_->secrets.size();++i)
        {
            const auto& secret=rules_->secrets[i];
            if(secretActivate_[i]>=0)secretActivate_[i]=std::min(secretReload_[i]?secret.reloadSeconds:secret.discoverySeconds,
                secretActivate_[i]+float(tuning::controlStep)*simulation.hero.scale);
            if(!(secretFlags&secret.saveFlag)&&(!testHeroPinned_||testPinnedHeroTriggers)&&
               roomOverlap(bodyBounds(simulation.hero.position),secret.trigger))
            {secretFlags|=secret.saveFlag;++secretCues[i];secretActivate_[i]=0;}
        }
        updateRemasks();
        const char* forced=flow.phase==RoomPhase::InitialLand||flow.phase==RoomPhase::InitialFlock?"Super Hard Land":
                           flow.phase==RoomPhase::InitialGetUp?"Collect Normal 3":
                           flow.phase==RoomPhase::Waking?"Wake Up Ground":flow.phase==RoomPhase::HazardWake?"Hazard Respawn":nullptr;
        if(forced){flow.presentation.play(heroBank_->find(forced));flow.presentation.advance(tuning::controlStep*simulation.hero.scale);}
        else flow.presentation={};
        if(greatDoorConfig_)
        {
            greatDoor.update(tuning::controlStep*simulation.hero.scale);
            if(greatDoor.transitionRequested())
            {flow.transition=&greatDoorExit_;flow.phase=RoomPhase::Transition;simulation.hero.controlEnabled=false;}
        }
        const auto mushroomHits=conditionalEvents.mushroomHitSerial;
        const auto grimmkinHits=conditionalEvents.health.hits;
        const bool conditionalWasLocked=conditionalEvents.locked();
        const auto conditionalBefore=conditionalEvents.saved();
        conditionalEvents.update(simulation.scaledSeconds,simulation.hero,*nails_,testFreezeEnemies);
        const auto& conditionalAfter=conditionalEvents.saved();
        if(conditionalBefore.mrMushroomState!=conditionalAfter.mrMushroomState||
           conditionalBefore.flags!=conditionalAfter.flags||
           conditionalBefore.grimmChildLevel!=conditionalAfter.grimmChildLevel||
           conditionalBefore.flamesCollected!=conditionalAfter.flamesCollected||
           conditionalBefore.flamesRequired!=conditionalAfter.flamesRequired||
           conditionalBefore.nailSmithUpgrades!=conditionalAfter.nailSmithUpgrades)applySceneActivation();
        if(conditionalEvents.mushroomHitSerial!=mushroomHits){
            auto origin=conditionalEvents.mushroomPosition;origin.y-=.5f;
            decorEffects.nailHit(origin);
        }
        if(conditionalEvents.health.hits!=grimmkinHits){
            const auto& h=simulation.hero;const auto nail=nailPoint(h,nails_->shapes[nailVariant(h.nail)],{});
            const auto origin=conditionalEvents.grimmkinPosition;
            decorEffects.enemyNailHit({(origin.x+nail.x)*.5f,(origin.y+nail.y)*.5f},h.nail.direction);
        }
        if(conditionalEvents.locked())simulation.animator.overrideClip=heroBank_->find("LookUp");
        else if(conditionalWasLocked)simulation.animator.overrideClip=nullptr;
        if(flow.transition)healthScuttlers.unload(simulation.hero.vitals);
        const auto after=saved();
        if(before.broken!=after.broken||before.flags!=after.flags||before.geo!=after.geo||
           before.crawlerKills!=after.crawlerKills||before.buzzerKills!=after.buzzerKills||
           before.shadePresent!=after.shadePresent||before.shadeGeo!=after.shadeGeo||
           before.shadeX!=after.shadeX||before.shadeY!=after.shadeY||before.shadeHealth!=after.shadeHealth||before.conditional!=after.conditional)++saveRevision;
        if(flow.retryRequested){
            if(resting.saved.benchScene[0])requestRestTransition(resting.saved.benchScene,"bench");
            else restart(true);
            input.clear();break;}
    }
    syncEnemyLifeSave();
}
void TutorialSession::syncEnemyLifeSave()
{
    if(enemyLifeRevision_!=enemyLife.revision()){
        enemyLifeRevision_=enemyLife.revision();++saveRevision;
    }
}
void TutorialSession::presentPlants()
{
    plants.present(plantSeconds_,flow.camera.x,flow.camera.y);
    roomDynamics.present(plantSeconds_,flow.camera.x,flow.camera.y,presentationTime());plantSeconds_=0;
}
TutorialSave TutorialSession::saved() const
{
    SimulationScope timing(PerformanceSection::SimSnapshot);
    if(sceneActive_){auto s=sceneSave_;s.enemyDeaths=enemyLife.snapshot();s.playerData=playerData_;s.cartography=cartographer.progress;s.watcherProgress=watcherProgress;s.uumuuProgress=uumuuProgress;s.geo=geo;s.charms=charmInventory;s.resting=resting.saved;s.collectibles=collectibles.saved();s.conditional=conditionalEvents.saved();s.shadePresent=shadePresent_;s.shadeGeo=shadeGeo_;s.shadeX=shadeSpawn_.x;s.shadeY=shadeSpawn_.y;s.shadeHealth=shadeHealth_;s.flags=(s.flags&~TutorialSave::abilityFlags)|abilityFlags_|bossFlags_|greenpath.savedFlags();if(s.playerData.get("openedCityGate"))s.flags&=~TutorialSave::cityCrestFlag;return s;}
    TutorialSave save{flow.progress.broken,(floorBroken?1u:0u)|(flow.progress.collapsed?2u:0u)|(greatDoor.activated()?4u:0u)|furyChest.flags()|prompts.savedFlags()|credits.savedFlags()|(areaTitle.visitedCrossroads()?AreaTitle::visitedCrossroadsFlag:0u)|secretFlags|bossFlags_,geo,crawlerJournalKills,buzzerJournalKills,shadePresent_?1u:0u,shadeGeo_,shadeSpawn_.x,shadeSpawn_.y,shadeHealth_,conditionalEvents.saved()};
    save.enemyDeaths=enemyLife.snapshot();save.sceneBools=sceneSave_.sceneBools;save.playerData=playerData_;save.cartography=cartographer.progress;save.watcherProgress=watcherProgress;save.uumuuProgress=uumuuProgress;save.charms=charmInventory;save.collectibles=collectibles.saved();save.flags|=abilityFlags_;std::memcpy(save.gate,sceneSave_.gate,32);save.resting=resting.saved;if(save.playerData.get("openedCityGate"))save.flags&=~TutorialSave::cityCrestFlag;return save;
}
void TutorialSession::requestRestTransition(const char* scene,const char* entry){
    restTransition_={};std::snprintf(restTransition_.scene,32,"%s",scene);std::snprintf(restTransition_.entry,32,"%s",entry);
    flow.transition=&restTransition_;flow.phase=RoomPhase::Transition;
}
void TutorialSession::bindSceneShade(){
    shade.clear();objects[0]={};objects[0].colliderEnabled=false;
    if(shadePresent_&&shadeConfig_&&!std::strcmp(resting.saved.shadeScene,sceneName()))
        shade.bind(*shadeConfig_,shadeBank_,objects[0],shadeSpawn_,int(shadeHealth_)*simulation.hero.nailDamage,random_);
    roomBreakables.bind(roomBreakables.bank(),*terrain_,objects.data()+1,unsigned(objects.size()-1),this,roomBreakableHit);
    combat.bind(objects.data(),1+roomBreakables.count(),nails_);
}
bool TutorialSession::bindSceneBreakables(const char* root)
{
    char path[384];std::snprintf(path,sizeof(path),"%s/scenes/%s/breakables.bin",root,sceneName());
    RoomBreakableBank bank;
    // Older packages predate HKB2. New room regeneration always writes a bank,
    // including an empty one; a present but corrupt bank must fail admission.
    if(auto* file=std::fopen(path,"rb")){std::fclose(file);if(!bank.load(path))return false;}
    if(!roomBreakables.bind(bank,*terrain_,objects.data()+1,unsigned(objects.size()-1),this,roomBreakableHit))return false;
    decorEffects.clearRoomEffects();
    std::snprintf(path,sizeof(path),"%s/scenes/%s/room-breakable-fx.bin",root,sceneName());
    if(decorEffects.ready())if(auto* effects=std::fopen(path,"rb")){
        std::fclose(effects);
        char artwork[384];std::snprintf(artwork,sizeof(artwork),"%s/scenes/%s/%svisual.bin",root,sceneName(),graphicsProfile().roomPrefix);
        auto* visual=std::fopen(artwork,"rb");if(!visual)return false;
        char magic[4]{};unsigned header[4]{};
        const bool valid=std::fread(magic,1,4,visual)==4&&!std::memcmp(magic,"HKR",3)&&
            std::fread(header,4,4,visual)==4;
        std::fclose(visual);
        if(!valid||!decorEffects.loadRoomEffects(path,header[1]))return false;
    }
    return combat.bind(objects.data(),1+roomBreakables.count(),nails_);
}
void TutorialSession::roomBreakableHit(void* context,const RoomBreakableConfig& config,unsigned slot,const NailHit& hit)
{
    auto& session=*static_cast<TutorialSession*>(context);
    session.decorEffects.roomHit(config,slot,hit,session.simulation.hero.position,session.terrain_->world());
}
void TutorialSession::updateResting(const Input& command){
    // FSM1077 owns the Crossroads bell until its toll completes. Do not let
    // the generic Resting adapter open the travel menu on the same UP press.
    if(stagToll.bound()&&!std::strcmp(sceneName(),"Crossroads_47")&&stagToll.blocksResting())return;
    const auto equipped=resting.saved.equippedCharms;
    const bool wasSitting=resting.sitting;
    unsigned blockedStations=stagToll.bound()&&!std::strcmp(sceneName(),"Crossroads_47")&&
        stagToll.phase()!=StagStationToll::Phase::Open?1u<<StagStationToll::station:0;
    if(stagLever.blocksResting())blockedStations|=1u<<StagStationLever::station;
    if(resting.update(command,simulation.hero,rules_->restPoints,sceneName(),simulation.scaledSeconds,&geo,&playerData_,blockedStations))++saveRevision;
    if(!wasSitting&&resting.sitting){
        enemyLife.resetSemiPersistent();
        if(cartographer.progress.mapVisited())++saveRevision;
    }
    if(equipped!=resting.saved.equippedCharms){syncRestCharms(true);charmInventory.apply(simulation.hero);}
    // Release only the seated pose; other interactions own their overrides.
    if(resting.sitting){
        // Bench FSM: "Sit" plays once on arrival, then the seated loop "Sit Idle".
        // Older hero packages without these clips keep the standing idle pose.
        const Clip* intro=heroBank_->find("Sit");
        const Clip* seated=heroBank_->find("Sit Idle");
        if(!seated)seated=heroBank_->find("Idle");
        const Clip* current=simulation.animator.overrideClip;
        const bool introPlaying=intro&&(!wasSitting||(current==intro&&!simulation.animator.player.complete));
        const Clip* next=introPlaying?intro:seated;
        simulation.animator.overrideClip=next;
        if(next)simulation.animator.player.play(next,0,next!=current);
        simulation.hero.velocity={};
    }else if(wasSitting||simulation.animator.overrideClip){
        // Stand-up: the bench FSM plays "Get Off" once, then control returns to the hero.
        // Older hero packages without the clip return to the standing pose at once.
        const Clip* getOff=heroBank_->find("Get Off");
        const Clip* current=simulation.animator.overrideClip;
        if(wasSitting&&getOff){simulation.animator.overrideClip=getOff;simulation.animator.player.play(getOff,0,true);}
        else if(current==getOff&&simulation.animator.player.complete)simulation.animator.overrideClip=nullptr;
        else if(wasSitting)simulation.animator.overrideClip=nullptr;
    }
    if(const int station=resting.takeTravel();station>=0)requestRestTransition(stagStations[station].scene,"stag");
}
void TutorialSession::applyAbilities()
{
    simulation.hero.abilities.vengefulSpirit=hasAbility(AbilityReward::VengefulSpirit);
    simulation.hero.abilities.shadeSoul=hasAbility(AbilityReward::ShadeSoul);
    simulation.hero.abilities.dash=hasAbility(AbilityReward::MothwingCloak);
    simulation.hero.abilities.wallJump=hasAbility(AbilityReward::MantisClaw);
    simulation.hero.abilities.doubleJump=hasAbility(AbilityReward::MonarchWings);
    simulation.hero.abilities.superDash=hasAbility(AbilityReward::CrystalHeart);
    simulation.hero.abilities.acidArmour=hasAbility(AbilityReward::IsmasTear);
    simulation.hero.abilities.shadeDash=hasAbility(AbilityReward::ShadeCloak);
    if(abilityPickup.kind!=AbilityPickup::Kind::None&&!abilityPickup.trinketSequence())
        abilityPickup.collected=hasAbility(abilityPickup.reward());
}
void TutorialSession::syncRestCharms(bool fromRest)
{
    auto next=charmInventory;
    if(fromRest){
        for(unsigned i=0;i<13;++i)if(const auto id=legacyCharmIds[i]){
            const auto bit=charmBit(id);
            if(resting.saved.ownedCharms&(1u<<i))next.owned|=bit;
            if(resting.saved.equippedCharms&(1u<<i))next.equipped|=bit;else next.equipped&=~bit;
        }
        if(next.valid())charmInventory=next;
    }
    for(unsigned i=0;i<13;++i)if(const auto id=legacyCharmIds[i]){
        const auto bit=1u<<i;
        resting.saved.ownedCharms=(resting.saved.ownedCharms&~bit)|(charmInventory.has(id)?bit:0);
        resting.saved.equippedCharms=(resting.saved.equippedCharms&~bit)|(charmInventory.wears(id)?bit:0);
    }
}
bool TutorialSession::hasAbility(AbilityReward reward) const
{
    if(reward==AbilityReward::DreamNail)return (conditionalEvents.saved().flags&ConditionalSave::DreamNail)!=0;
    if(const auto* source=traversalReward(reward))return collectibles.saved().contains(source->sourceId);
    if(reward==AbilityReward::VengefulSpirit)return (abilityFlags_&TutorialSave::vengefulSpiritFlag)!=0||hasAbility(AbilityReward::ShadeSoul);
    if(reward==AbilityReward::MothwingCloak)return (abilityFlags_&TutorialSave::mothwingCloakFlag)!=0;
    return false;
}
bool TutorialSession::collectAbility(AbilityReward reward)
{
    if(hasAbility(reward))return false;
    // The original Shiny Item is disabled until Hornet's saved defeat.  Keep
    // the same invariant for direct callers as for the room pickup flow;
    // otherwise a test or scripted caller could grant dash before the fight.
    if(reward==AbilityReward::MothwingCloak&&
       !(bossFlags_&TutorialSave::hornetFlag)&&!playerData_.get("hornet1Defeated"))return false;
    const auto previousPlayerData=playerData_;
    // Shiny Control Dash / Walljump and Mines_31 Prompt also update Cornifer
    // progression and the separate permission flags. Ownership itself stays
    // in the existing ability ledger, exposed through applySceneActivation.
    auto pickupPlayerData=playerData_;
    const char* permission=nullptr;const char* cornifer=nullptr;
    switch(reward){
    case AbilityReward::MothwingCloak:permission="canDash";cornifer="corn_greenpathLeft";break;
    case AbilityReward::MantisClaw:permission="canWallJump";cornifer="corn_fungalWastesLeft";break;
    case AbilityReward::CrystalHeart:cornifer="corn_minesLeft";break;
    default:break;
    }
    if(reward==AbilityReward::CrystalHeart){
        const int bretta=pickupPlayerData.get("brettaState");
        if(bretta==std::numeric_limits<int>::max()||!pickupPlayerData.set("brettaState",bretta+1))return false;
    }
    if((permission&&!pickupPlayerData.set(permission,1))||
       (cornifer&&!pickupPlayerData.set(cornifer,1)))return false;
    const auto refresh=[&](){
        applyAbilities();applySceneActivation();
        return !terrain_||sceneEnemies.updateActivation(activation_,terrain_->world());
    };
    const auto rollbackActivation=[&](){playerData_=previousPlayerData;refresh();};
    if(reward==AbilityReward::DreamNail){
        conditionalEvents.setDreamNailOwned(true);
        if(!refresh()){
            conditionalEvents.setDreamNailOwned(false);rollbackActivation();return false;
        }
        ++saveRevision;return true;
    }
    if(const auto* source=traversalReward(reward)){
        if(!collectibles.remember(source->sourceId))return false;
        playerData_=pickupPlayerData;
        if(!refresh()){
            collectibles.undoRemember(source->sourceId);rollbackActivation();return false;
        }
        if(const auto* checkpoint=source->checkpoint){
            std::snprintf(resting.saved.benchScene,sizeof(resting.saved.benchScene),"%s",source->scene);
            resting.saved.benchX=checkpoint->position.x;resting.saved.benchY=checkpoint->position.y;
        }
        ++saveRevision;return true;
    }
    if(reward!=AbilityReward::VengefulSpirit&&reward!=AbilityReward::MothwingCloak)return false;
    const unsigned flag=reward==AbilityReward::VengefulSpirit?TutorialSave::vengefulSpiritFlag:TutorialSave::mothwingCloakFlag;
    if(abilityFlags_&flag)return false;
    const auto previousSceneBools=sceneSave_.sceneBools;
    // PersistentBoolItem level132-5439 uses this identity when Shiny Item
    // finishes its pickup. Keep exported artwork and the native reward in sync.
    if(reward==AbilityReward::MothwingCloak&&
       !sceneSave_.sceneBools.set(SceneBoolSave::key("Fungus1_04","Shiny Item")))return false;
    if(reward==AbilityReward::MothwingCloak)playerData_=pickupPlayerData;
    if(reward==AbilityReward::VengefulSpirit){
        // Crossroads_ShamanTemple level76-14190 / Set Respawns. Publish both
        // named fields with the ability; a full table must not grant half a reward.
        auto next=playerData_;
        if(!next.set("hasSpell",1)||!next.set("fireballLevel",1))return false;
        playerData_=next;
    }
    abilityFlags_|=flag;
    if(!refresh()){
        sceneSave_.sceneBools=previousSceneBools;abilityFlags_&=~flag;rollbackActivation();
        return false;
    }
    if(reward==AbilityReward::VengefulSpirit&&!std::strcmp(sceneName(),shamanquest::scene)){
        // Fall14190 / Set Respawns sets both markers before SaveGame. Publish
        // only after the inventory/activation transaction has succeeded. A
        // generic ability fixture outside this room is not this source callback.
        std::snprintf(resting.saved.benchScene,sizeof(resting.saved.benchScene),"%s",shamanquest::scene);
        resting.saved.benchX=shamanquest::checkpoint.x;resting.saved.benchY=shamanquest::checkpoint.y;
        flow.hazardMarker=shamanquest::hazard;flow.hazardRight=shamanquest::facingRight;
    }
    ++saveRevision;
    return true;
}
void TutorialSession::resetWorldQuest()
{
    if(worldQuestDialogue.locked())simulation.animator.overrideClip=nullptr;
    const auto* field=WorldQuests::playerDataField(worldQuests.definition().event);
    worldQuests.enter(sceneName(),collectibles.saved(),field&&playerData_.get(field)!=0);
    worldQuestDialogue.reset();
}
bool TutorialSession::worldQuestCallbackAllowed() const
{
    const auto& h=simulation.hero;
    return !std::strcmp(sceneName(),worldQuests.definition().scene)&&
        flow.phase==RoomPhase::Playing&&h.vitals.health>0&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage&&
        // FSM119 checks OUT OF RANGE before Convo Start; its scripted walk and
        // active conversation keep control until finish, damage or cancellation.
        (worldQuestDialogue.locked()||worldQuests.talking()||worldQuests.nearby(bodyBounds(h.position)));
}
void TutorialSession::updateWorldQuest(float seconds)
{
    auto& ui=worldQuestDialogue;auto& h=simulation.hero;
    const auto releaseDoorControl=[&]{
        simulation.animator.overrideClip=nullptr;
        // Door adapters can finish their closing phase before the shared
        // dialogue code samples locked(). Release the control they owned here.
        if(flow.phase==RoomPhase::Playing&&h.vitals.health>0&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage&&
           !inspection.locked()&&!furyChest.collecting()){
            h.focus=FocusPhase::None;h.focusTime=0;
            h.preventCastByDialogueEnd();h.controlEnabled=true;
        }
    };
    if(!std::strcmp(sceneName(),shamanquest::scene)){
        const bool wasLocked=ui.locked(),up=simulation.hero.input.y>.5f;
        const bool opened=playerData_.get("shaman")>=shamanquest::gateOpenedState;
        const bool available=flow.phase==RoomPhase::Playing&&simulation.hero.vitals.health>0&&
            simulation.hero.actor!=Actor::Dead&&simulation.hero.actor!=Actor::Damage;
        if(wasLocked&&!available){ui.reset();releaseDoorControl();return;}
        ui.step(seconds);
        ui.prompt=!ui.locked()&&available&&!opened&&simulation.hero.controlEnabled&&simulation.hero.inputEnabled&&
            simulation.hero.contacts.ground&&!simulation.hero.nail.active&&!simulation.hero.dashing&&
            !inspection.locked()&&!abilityPickup.locked()&&
            sceneSave_.sceneBools.get(SceneBoolSave::key(shamanquest::scene,"Battle Scene"))&&
            overlaps(bodyBounds(simulation.hero.position),shamanquest::listenRange);
        if(ui.prompt&&up&&!ui.upHeld)ui.open();
        ui.upHeld=up;
        if(ui.opened())ui.read(opened?shamanquest::farewellDialogue:shamanquest::defeatedDialogue);
        else if(ui.phase()==WorldQuestDialogue::Phase::Reading&&
                (simulation.hero.input.pressed&(Jump|Attack|Focus|Dash))&&ui.proceed()){
            if(!opened&&setPlayerData("shaman",shamanquest::gateOpenedState))ui.intermission();
            else ui.close();
        }
        if(ui.locked()){
            simulation.hero.controlEnabled=false;simulation.hero.velocity={};
            simulation.hero.facingRight=simulation.hero.position.x<shamanquest::shamanX;
            simulation.hero.focus=FocusPhase::None;simulation.hero.focusTime=0;simulation.hero.nail={};
            simulation.hero.jumpQueued=simulation.hero.dashQueued=simulation.hero.attackQueued=-1;
            simulation.hero.lookTime=0;
        }else if(wasLocked)releaseDoorControl();
        return;
    }
    if(stagToll.bound()&&!std::strcmp(sceneName(),"Crossroads_47")){
        Rect range{};float stationX=0;bool found=false;
        if(rules_)for(const auto& point:rules_->restPoints)if(point.kind==1&&point.station==StagStationToll::station){
            stationX=point.position.x;range=StagStationToll::trigger;found=true;break;
        }
        if(found){
            const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
            const bool owned=stagToll.controlsHero();
            const bool tollControl=stagToll.update(seconds,h,ui,range,stationX,
                flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuests.talking()&&
                !furyChest.collecting()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage,
                turn&&turn->fps>0?float(turn->count)/turn->fps:0);
            stagToll.takeUnlocked();
            if(tollControl){
                stagTollHoldsControl=true;
                h.controlEnabled=false;h.velocity={ui.velocity(),0};
                if(ui.moving())h.facingRight=ui.facingRight();
                const char* clip=ui.phase()==WorldQuestDialogue::Phase::Walking?"Walk":
                    ui.phase()==WorldQuestDialogue::Phase::Turning?"Turn":
                    ui.phase()==WorldQuestDialogue::Phase::Closing?"LookUpEnd":"LookUp";
                if(heroBank_){simulation.animator.overrideClip=heroBank_->find(clip);
                    if(simulation.animator.overrideClip)simulation.animator.player.play(simulation.animator.overrideClip);}
                return;
            }
            if(owned)releaseDoorControl();
            // The shared dialogue lock keeps hero control off for the closing frames after the paid toll.
            // Release once the toll is Open and the dialogue has unlocked, not on the transition frame alone.
            if(stagTollHoldsControl&&!owned&&stagToll.phase()==StagStationToll::Phase::Open&&!ui.locked()){
                stagTollHoldsControl=false;releaseDoorControl();
            }
            if(stagToll.blocksResting()||owned)return;
        }
    }
    if(cityLift.bound()){
        const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
        const bool released=cityLift.takeControlRelease()||cityTransport.takeRelease();
        const bool owned=cityToll.controlsHero()||cityLift.controlsHero()||cityTransport.locked()||released;
        const bool tollControl=cityToll.update(seconds,h,bodyBounds(h.position),ui,
            flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuests.talking()&&
            !furyChest.collecting()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage,
            turn&&turn->fps>0?float(turn->count)/turn->fps:0);
        if(cityToll.phase()==CityLiftToll::Phase::PaidWait)cityLift.lower();
        // Original Call Lever FSM847/1102 checks only Hero Check and the nail. The upper station
        // (Crossroads_49) therefore uses its lever while the cage is still raised and unpaid; the
        // lowered cage needs the toll in Crossroads_49b.
        const bool upperStation=!std::strcmp(sceneName(),"Crossroads_49");
        if(cityTransport.bound()&&(cityLift.phase()==CityLiftLowering::Phase::Open||upperStation)&&!cityTransport.locked()&&
           flow.phase==RoomPhase::Playing&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage&&nails_&&h.nail.active&&h.nail.hitbox&&
           h.nail.serial!=cityLeverAttack_){
            const auto& d=cityTransport.data();Rect lever=d.lever(),check=d.heroCheck();
            const float delta=cityLift.y()-d.initial();lever.bottom+=delta;lever.top+=delta;check.bottom+=delta;check.top+=delta;
            if(nailOverlaps(h,nails_->shapes[nailVariant(h.nail)],lever)){
                cityLeverAttack_=h.nail.serial;cityLift.strikeLever(h.facingRight);
                const auto body=bodyBounds(h.position);
                const bool inside=body.left<check.right&&body.right>check.left&&body.bottom<check.top&&body.top>check.bottom;
                auto pending=playerData_;const bool canSave=pending.set(SavedPlayerData::key("liftArrival"),1);
                // Only the lowered cage (Crossroads_49b) needs the paid flag; the upper departure is unconditional.
                const bool unlocked=canSave&&(upperStation||playerData_.get("cityLift1")!=0);
                if(cityTransport.depart(unlocked,inside,true,cityLift.y())){
                    cityLift.travelDoors(true);h.controlEnabled=h.gravityEnabled=false;h.velocity={};
                }
            }
        }
        if(cityTransport.ready()&&setPlayerData("liftArrival",1)){
            requestRestTransition(cityTransport.data().destination,cityTransport.data().entry);
            std::snprintf(restTransition_.name,sizeof(restTransition_.name),"elev_entrance");
        }
        if(tollControl||cityLift.controlsHero()||cityTransport.locked()){
            h.controlEnabled=false;h.velocity={tollControl?ui.velocity():0,0};
            if(ui.moving())h.facingRight=ui.facingRight();
            if(heroBank_){simulation.animator.overrideClip=heroBank_->find(tollControl?cityToll.heroClip(ui):"Idle");
                if(simulation.animator.overrideClip)simulation.animator.player.play(simulation.animator.overrideClip);}
            return;
        }
        if(owned)releaseDoorControl();
        if(ui.prompt||owned)return;
    }
    if(waterways.bound()){
        const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
        const bool owned=waterways.controlsHero();
        if(waterways.update(seconds,h,bodyBounds(h.position),ui,
            flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuests.talking()&&
            !furyChest.collecting()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage,
            turn&&turn->fps>0?float(turn->count)/turn->fps:0)){
            if(heroBank_){
                const auto* clip=waterways.heroClip(ui);
                simulation.animator.overrideClip=clip?heroBank_->find(clip):nullptr;
                if(simulation.animator.overrideClip)simulation.animator.player.play(simulation.animator.overrideClip);
            }
            return;
        }
        if(owned)releaseDoorControl();
        if(ui.prompt||owned)return;
    }
    if(cityCrest.bound()){
        const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
        const bool owned=cityCrest.controlsHero();
        if(cityCrest.update(seconds,h,ui,
            flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuests.talking()&&
            !furyChest.collecting()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage,
            turn&&turn->fps>0?float(turn->count)/turn->fps:0)){
            if(heroBank_){simulation.animator.overrideClip=heroBank_->find(ui.heroClip());
                if(simulation.animator.overrideClip)simulation.animator.player.play(simulation.animator.overrideClip);}
            return;
        }
        if(owned)releaseDoorControl();
        if(ui.prompt||owned)return;
    }
    if(jijiDoor.bound()){
        const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
        const bool owned=jijiDoor.controlsHero();
        if(jijiDoor.update(seconds,h,bodyBounds(h.position),ui,
            flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuests.talking()&&
            !furyChest.collecting()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage,
            turn&&turn->fps>0?float(turn->count)/turn->fps:0)){
            if(heroBank_){
                const auto* clip=jijiDoor.heroClip(ui);
                simulation.animator.overrideClip=clip?heroBank_->find(clip):nullptr;
                if(simulation.animator.overrideClip)simulation.animator.player.play(simulation.animator.overrideClip);
            }
            return;
        }
        if(owned)releaseDoorControl();
        if(ui.prompt||owned)return;
    }
    ui.promptLabel[0]=0;
    worldQuests.step(seconds);
    if(slyShopVisible())slyShopAnimation.advance(seconds);
    const bool wasLocked=ui.locked(),up=h.input.y>.5f;
    if(flow.phase==RoomPhase::Playing&&h.vitals.health>0&&h.actor!=Actor::Dead&&
       worldQuests.elderbugWalkedBy(bodyBounds(h.position),areaTitle.visitedCrossroads(),playerData_))
        setPlayerData("elderbug",1);
    if(wasLocked&&!worldQuestCallbackAllowed()){
        worldQuestInteraction(WorldQuestInteraction::Cancel);
        return;
    }
    ui.step(seconds);ui.observePosition(h.position.x);
    ui.prompt=canTalkWorldQuest()&&!ui.locked()&&!inspection.locked()&&!furyChest.collecting();
    if(ui.prompt&&up&&!ui.upHeld){
        const auto* turn=heroBank_?heroBank_->find("Turn"):nullptr;
        const float turnSeconds=turn&&turn->fps>0?float(turn->count)/turn->fps:0;
        if(worldQuests.definition().event==WorldQuestEvent::Elderbug)
            ui.approachEither(h.position.x,worldQuests.definition().position.x,2,6,h.facingRight,turnSeconds);
        else ui.approach(h.position.x,worldQuests.definition().position.x,h.facingRight,turnSeconds);
    }
    ui.upHeld=up;
    if(ui.opened()){
        if(worldQuests.talking()||worldQuestInteraction(WorldQuestInteraction::DialogueOpened))ui.read(worldQuests.text());
        else ui.close();
    }else if(ui.phase()==WorldQuestDialogue::Phase::Reading){
        if((h.input.pressed&(Jump|Attack|Focus|Dash))&&ui.proceed()){
            worldQuestInteraction(WorldQuestInteraction::ConversationFinished);
            if(worldQuests.talking()){
                const auto talk=worldQuests.talk();
                if(worldQuests.definition().event==WorldQuestEvent::BrettaRescued&&
                   (talk==WorldQuestTalk::Rescue3||talk==WorldQuestTalk::Rescue4))ui.intermission();
                else ui.read(worldQuests.text());
            }else ui.close();
        }
    }
    if(wasLocked&&!ui.locked())simulation.animator.overrideClip=nullptr;
    if(ui.locked()&&heroBank_){
        simulation.animator.overrideClip=heroBank_->find(ui.heroClip());
        simulation.animator.player.play(simulation.animator.overrideClip);
    }
    if(wasLocked||ui.locked()){
        h.focus=FocusPhase::None;h.focusTime=0;h.nail={};
        h.jumpQueued=h.dashQueued=h.attackQueued=-1;h.lookTime=0;
    }
    if(ui.locked()){
        h.controlEnabled=false;h.velocity={ui.velocity(),0};
        h.facingRight=ui.moving()?ui.facingRight():h.position.x<worldQuests.definition().position.x;
    }else if(wasLocked){h.preventCastByDialogueEnd();h.controlEnabled=true;}
}
bool TutorialSession::canTalkWorldQuest() const
{
    const auto& h=simulation.hero;
    // Original HeroController.CanTalk, evaluated before the NPC takes control.
    return worldQuestCallbackAllowed()&&!worldQuests.talking()&&h.controlEnabled&&h.inputEnabled&&
        h.contacts.ground&&!h.nail.active&&!h.dashing;
}
bool TutorialSession::completeWorldQuest(WorldQuestEvent event)
{
    return worldQuestCallbackAllowed()&&commitWorldQuest(event,false);
}
bool TutorialSession::commitWorldQuest(WorldQuestEvent event,bool beginTalk)
{
    const auto* field=WorldQuests::playerDataField(event);
    if(!field)return false;
    const auto previous=collectibles.saved();
    const auto revision=saveRevision;
    if(!(beginTalk?worldQuests.beginTalk(collectibles):worldQuests.complete(event,collectibles)))return false;
    // A full named table must not consume the event and strand relocation.
    if(!setPlayerData(field,1)){
        collectibles.restore(previous,simulation.hero.vitals);
        if(beginTalk)worldQuests.cancelTalk();
        return false;
    }
    if(previous.count!=collectibles.saved().count||saveRevision!=revision)saveRevision=revision+1;
    return true;
}
void TutorialSession::updateWaterways(float dt,const Input& input)
{
    const bool up=input.y>.5f,pressed=up&&!waterwaysUpHeld_;waterwaysUpHeld_=up;
    auto& h=simulation.hero;
    if(h.actor==Actor::Dead||h.actor==Actor::Damage||!h.controlEnabled)return;
    if(!std::strcmp(sceneName(),"Ruins1_05b")&&pressed&&playerData_.get("simpleKeys")>0&&
       !playerData_.get("openedWaterwaysManhole")&&std::abs(h.position.x-27.85f)<2&&std::abs(h.position.y-6.8f)<3){
        const auto previous=playerData_;
        if(setPlayerData("openedWaterwaysManhole",1)&&setPlayerData("simpleKeys",previous.get("simpleKeys")-1))return;
        playerData_=previous;applySceneActivation();
    }
    if(std::strcmp(sceneName(),"Waterways_05")){waterwaysPumpDelay_=-1;return;}
    if((crossroadsBoss.savedFlags()&TutorialSave::dungDefenderFlag)&&!playerData_.get("defeatedDungDefender"))
        setPlayerData("defeatedDungDefender",1);
    if(waterwaysPumpDelay_>=0){
        waterwaysPumpDelay_=std::max(0.f,waterwaysPumpDelay_-dt);
        if(waterwaysPumpDelay_<=0&&setPlayerData("waterwaysAcidDrained",1))waterwaysPumpDelay_=-1;
    }
    if(!playerData_.get("waterwaysAcidDrained")&&waterwaysPumpDelay_<0&&nails_&&h.nail.hitbox&&
       nailOverlaps(h,nails_->shapes[nailVariant(h.nail)],{101.79819f,13.33745f,103.93827f,17.62463f}))
        waterwaysPumpDelay_=.75f;
    if(pressed&&playerData_.get("defeatedDungDefender")&&!charmInventory.has(10)&&
       overlaps(bodyBounds(h.position),{79.66377f,6.22722f,82.17623f,6.60762f})&&setPlayerData("gotCharm_10",1)){
        charmInventory.owned|=charmBit(10);++saveRevision;
    }
}
bool TutorialSession::waterwaysHides(unsigned owner) const
{
    if(!std::strcmp(sceneName(),"Ruins1_17")){
        // Shiny Item222 hierarchy, level97 transforms (build22529139).
        for(const unsigned child:{222u,223u,220u,218u,219u})
            if(owner==child)return sceneSave_.sceneBools.get(SceneBoolSave::key("Ruins1_17","level97-222"));
    }
    if(!std::strcmp(sceneName(),"Ruins1_05b")&&owner==238)return playerData_.get("openedWaterwaysManhole")!=0;
    if(!std::strcmp(sceneName(),"Waterways_05")&&owner==152)
        return !playerData_.get("defeatedDungDefender")||charmInventory.has(10);
    return false;
}
bool TutorialSession::worldQuestInteraction(WorldQuestInteraction event)
{
    if(event==WorldQuestInteraction::Cancel){
        const bool talking=worldQuests.talking(),locked=worldQuestDialogue.locked();
        const bool upHeld=worldQuestDialogue.upHeld||simulation.hero.input.y>.5f;
        if(locked)simulation.animator.overrideClip=nullptr;
        worldQuests.cancelTalk();worldQuestDialogue.reset();
        worldQuestDialogue.upHeld=upHeld;
        auto& h=simulation.hero;
        if(locked&&h.vitals.health>0&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage){
            h.focus=FocusPhase::None;h.focusTime=0;
            h.preventCastByDialogueEnd();h.controlEnabled=true;
        }
        return talking||locked;
    }
    if(!worldQuestCallbackAllowed()){worldQuests.cancelTalk();return false;}
    if(event==WorldQuestInteraction::ConversationFinished){
        if(!worldQuests.finishDialogue())return false;
        if(worldQuests.talk()==WorldQuestTalk::IntroMain&&!setPlayerData("metElderbug",1))worldQuests.cancelTalk();
        return true;
    }
    if(event!=WorldQuestInteraction::DialogueOpened)return false;
    if(worldQuests.definition().event==WorldQuestEvent::Elderbug){
        const char* flag=nullptr;
        if(!worldQuests.beginElderbug(areaTitle.visitedCrossroads(),playerData_,flag))return false;
        if(flag&&!setPlayerData(flag,1)){worldQuests.cancelTalk();return false;}
        return true;
    }
    return commitWorldQuest(worldQuests.definition().event,true);
}
void TutorialSession::updateAbilityPickup(float dt,const Input& input)
{
    if(abilityPickup.kind==AbilityPickup::Kind::Memorial||abilityPickup.kind==AbilityPickup::Kind::DreamNail){
        updateDreamNail(dt,input);return;
    }
    const bool wasLocked=abilityPickup.locked();
    const bool grant=abilityPickup.update(dt,input,simulation.hero);
    if(terrain_&&abilityPickup.kind==AbilityPickup::Kind::CrystalHeart&&abilityPickup.heartBlasted)
        terrain_->setEnabled(4354,false);
    if(abilityPickup.authoredSequence()&&heroBank_){
        const auto* clip=abilityPickup.heroClip();
        if(clip){simulation.animator.overrideClip=heroBank_->require(clip);
            simulation.animator.player.play(simulation.animator.overrideClip);
            if(abilityPickup.kind==AbilityPickup::Kind::CrystalHeart&&abilityPickup.phase==AbilityPickup::Phase::HeartFall)
                simulation.animator.player.play(simulation.animator.overrideClip,6,true);
            if(!wasLocked)
                simulation.animator.player.advance(dt);
            if(abilityPickup.kind==AbilityPickup::Kind::CrystalHeart)
                simulation.hero.presentation.reset(simulation.hero.position);
        }else if(wasLocked)simulation.animator.overrideClip=nullptr;
    }
    if(!grant)return;
    if(abilityPickup.kind==AbilityPickup::Kind::SimpleKey){
        const auto previousLedger=sceneSave_.sceneBools;
        auto ledger=previousLedger;
        const auto key=SceneBoolSave::key("Ruins1_17","level97-222");
        if(playerData_.get("simpleKeys")<std::numeric_limits<int>::max()&&
           !sceneSave_.sceneBools.get(key)&&ledger.set(key)){
            // Activation refresh reads this identity while publishing the count.
            sceneSave_.sceneBools=ledger;
            if(setPlayerData("simpleKeys",playerData_.get("simpleKeys")+1))return;
            sceneSave_.sceneBools=previousLedger;
        }
        abilityPickup.collected=sceneSave_.sceneBools.get(key);
    }else if(abilityPickup.kind==AbilityPickup::Kind::CityCrest){
        if(setPlayerData("hasCityKey",1))return;
        abilityPickup.collected=false;
    }else if(collectAbility(abilityPickup.reward()))return;
    // Do not show an acquired panel or remove a pickup that could not be saved.
    if(!abilityPickup.trinketSequence())abilityPickup.collected=hasAbility(abilityPickup.reward());
    abilityPickup.phase=AbilityPickup::Phase::Idle;abilityPickup.popupAge=-1;
    simulation.animator.overrideClip=nullptr;
    simulation.hero.inputEnabled=simulation.hero.controlEnabled=simulation.hero.gravityEnabled=true;
}
void TutorialSession::updateAreaTitle(float seconds)
{
    if(areaTitle.waitsForContact()&&(!testHeroPinned_||testPinnedHeroTriggers))
        areaTitle.observeEntry(bodyBounds(simulation.hero.position));
    if(areaTitle.step(seconds,&playerData_)){applySceneActivation();++saveRevision;}
    flow.setVisitedCrossroads(areaTitle.visitedCrossroads());
}
void TutorialSession::rememberTutorialGate(const char* gate){std::snprintf(sceneSave_.gate,32,"%s",gate);}
void TutorialSession::updateRemasks()
{
    for(unsigned i=0;i<rules_->remasks.size();++i)
    {
        const auto& mask=rules_->remasks[i];auto& fade=remaskFade_[i];
        if(!fade.enabled)continue;
        // Older fixtures disable Rigidbody2D. Reveal fixtures instead freeze
        // its constraints and retain callbacks, matching the source player.
        const bool inside=testHeroPinned_&&!testPinnedHeroTriggers?fade.inside:roomMaskOverlap(bodyBounds(simulation.hero.position),mask);
        const bool exited=!inside&&fade.inside;fade.inside=inside;
        const float seconds=float(tuning::controlStep)*simulation.hero.scale;
        if(fade.state==RemaskState::Pause)
        {
            // Trigger callbacks during Pause do not reach Idle's listener.
            fade.pause=std::max(0.0,fade.pause-seconds);
            if(fade.pause>0)continue;
            fade.state=RemaskState::Idle;fade.start=fade.alpha;fade.elapsed=0;
            fade.target=mask.opaqueInside?0.0f:1.0f;fade.duration=mask.startupFade;
        }
        // UNCOVER is OnTriggerStay2D, including a contact made during
        // Pause. Restarting it every step would prevent the tween finishing.
        else if((inside&&fade.state!=RemaskState::FadeOut)||
                (exited&&fade.state==RemaskState::FadeOut))
        {
            fade.state=inside?RemaskState::FadeOut:RemaskState::FadeIn;
            fade.start=fade.alpha;fade.elapsed=0;fade.duration=mask.seconds;
            fade.target=(inside==mask.opaqueInside)?1.0f:0.0f;
        }
        else fade.elapsed=std::min(fade.duration,fade.elapsed+seconds);
        fade.alpha=fade.duration>0?fade.start+(fade.target-fade.start)*(fade.elapsed/fade.duration):fade.target;
    }
}
void TutorialSession::resetRemasks()
{
    remaskFade_={};
    for(unsigned i=0;i<rules_->remasks.size()&&i<remaskFade_.size();++i)
    {
        const auto& mask=rules_->remasks[i];auto& fade=remaskFade_[i];
        fade.enabled=!mask.requiredCharm||charmInventory.wears(mask.requiredCharm);
        fade.target=mask.opaqueInside?0.0f:1.0f;
        fade.alpha=fade.start=mask.startupPause>0?1.0f:fade.target;
        fade.duration=mask.seconds;fade.pause=mask.startupPause;
        if(mask.startupPause>0)fade.state=RemaskState::Pause;
    }
}
void TutorialSession::placeBenchRespawn()
{
    if(std::strcmp(sceneSave_.gate,"bench"))return;
    auto& hero=simulation.hero;
    hero.position={resting.saved.benchX,resting.saved.benchY};
    if(!std::strcmp(sceneName(),shamanquest::scene)&&
       resting.saved.benchX==shamanquest::checkpoint.x&&resting.saved.benchY==shamanquest::checkpoint.y){
        hero.facingRight=shamanquest::facingRight;
        flow.hazardMarker=shamanquest::hazard;flow.hazardRight=shamanquest::facingRight;
    }
    // RestBench saves the seated marker. Its body can overlap the floor;
    // resolve the standing body before the first physics step, as for doors.
    if(const auto* source=traversalReward(sceneName());source&&source->checkpoint&&hasAbility(source->reward)){
        const auto& checkpoint=*source->checkpoint;
        if(resting.saved.benchX==checkpoint.position.x&&resting.saved.benchY==checkpoint.position.y)
            placeTraversalCheckpoint(hero,terrain_->world(),checkpoint);
    }
    flow.groundArrival(hero,terrain_->world());
    hero.vitals.health=hero.vitals.maxHealth;hero.vitals.soul=hero.vitals.reserve=0;
}
void TutorialSession::enterScene(const RoomConfig& rules,TerrainBank& terrain,const char* scene,const char* gate)
{
    // The loader may already have released the departing SceneBank's gates.
    const float entryPause=flow.departureEntryPause();
    roomBreakables.clear();
    worldSwitch.clear();worldSwitchVisuals.clear();shamanPillar.clear();mantisGates.clear();townLift.clear();jijiDoor.clear();cityCrest.clear();cityCrestDust.clear();waterways.clear();cityToll.clear();stagToll.clear();cityLift.clear();cityTransport.clear();flow.setGateFilter(nullptr,nullptr);
    slyShop.reset();
    routeTolls={};cityGate={};
    doorPromptPresentation.reset();arrowPrompt.reset();
    decorEffects.reset();
    const int superDashDirection=simulation.hero.superDash.flying()?simulation.hero.superDash.direction:0;
    const auto keep=saved();const SceneHeroState entryState(simulation.hero);
    if(keep.flags&FuryChest::gotCharm6Flag)resting.saved.ownedCharms|=1u<<12;
    sceneSave_=keep;std::snprintf(sceneSave_.scene,32,"%s",scene);std::snprintf(sceneSave_.gate,32,"%s",gate);
    const bool crossroads=std::strncmp(scene,"Crossroads_",10)==0;
    areaTitle.resetScene(scene,&playerData_);if(crossroads)sceneSave_.flags|=AreaTitle::visitedCrossroadsFlag;
    sceneActive_=true;rules_=&rules;terrain_=&terrain;accumulator_=0;levelSeconds_=0;
    sceneAmbient_=false;ambientParticles.reset();
    resetRemasks();
    heroEffects.reset();focusEffects.reset();
    if(geoConfig_)coins.bind(*geoConfig_,random_);
    geoVisuals.reset();
    uumuu.reset();monomon.reset();sceneEnemies.clear();enemyC.clear();
    enemyCRewards_.clear();
    resting.leave();deathRecorded_=false;
    crossroadsBoss.reset();greenpath.clear(keep.flags);bossFlags_=keep.flags&TutorialSave::bossFlags;darkness.reset(lightingConfig_,hasLantern());heroLight.reset(lightingConfig_);
    sceneWalls.bind(terrain,sceneSave_.sceneBools,playerData_,&decorEffects);
    roomMasks.bind(terrain.masks,&sceneSave_.sceneBools);
    terrain.bindMotionSave(sceneSave_.sceneBools,&playerData_);
    archiveShortcut.bind(sceneSave_.scene,terrain,playerData_.get(SavedPlayerData::key(archive_shortcut_data::playerData))!=0);
    applySceneActivation();
    flow={};
    flow.setCameraLockFilter(&sceneWalls,[](const void* context,unsigned owner){
        return !static_cast<const SceneWalls*>(context)->hidesLock(owner);});
    flow.begin(rules,simulation,terrain.world(),false,areaTitle.visitedCrossroads());flow.setActivation(&activation_);flow.phase=RoomPhase::Playing;flow.enteredThrough(gate);
    flow.groundDoorArrival(simulation.hero,terrain.world());
    // RoomFlow creates a new hero; its reset serial is not another damage hit.
    lightDamageSerial_=simulation.hero.lightDamageSerial;
    entryState.apply(simulation.hero);
    collectibles.restore(keep.collectibles,simulation.hero.vitals);
    resetWorldQuest();
    placeBenchRespawn();
    sceneWalls.enter(simulation.hero.position.x);
    // Entering a loaded room delivers the source HERO IN POSITION event.
    for(unsigned i=0;i<rules_->remasks.size();++i)
        if(rules_->remasks[i].startupHeroPositioned)remaskFade_[i].pause=0;
    simulation.hero.vitals.maxSoul=shadePresent_?66:99;resting.applyCharms(simulation.hero);charmInventory.apply(simulation.hero);bindSceneShade();
    applyAbilities();
    simulation.hero.inputEnabled=simulation.hero.controlEnabled=simulation.hero.gravityEnabled=true;
    if(!flow.beginDoorArrival(simulation,*heroBank_,entryPause)){
        flow.fadeIn();flow.finishedEnteringScene();
    }
    areaTitle.heroInPosition();
    enterAbilityPickup();
    cartographer.progress.visitRoom(mapRoomIndex(scene));
    cartographer.enter(scene,playerData_.get("openedMapperShop")!=0);
    resting.enterStation(rules.stag,!std::strcmp(gate,"stag"),simulation.hero,playerData_);
    if(!std::strncmp(gate,"bot",3))simulation.hero.velocity.y=40;
    // Source ENTER SUPER DASH resumes flight after the old scene's FSM cancels.
    if(superDashDirection)simulation.hero.resumeSuperDash(superDashDirection);
    // Crossroads_47 FSM1077 owns the bell toll before the existing travel menu.
    stagToll.bind(worldTollSaveHooks(),&resting.saved);
    crawlerCount=doorCount=buzzerCount=stalactiteCount=0;++audioEpoch;++saveRevision;
}
bool TutorialSession::restore(const TutorialSave& save)
{
    if(!save.playerData.valid()||!save.sceneBools.valid())return false;
    if(!enemyLife.restoreSnapshot(save.enemyDeaths,save.sceneBools))return false;
    sceneSave_.sceneBools=save.sceneBools;
    if(!enemyLife.bindPersistent(sceneSave_.sceneBools))return false;
    playerData_=save.playerData;
    cartographer.progress=save.cartography;
    resting.saved=save.resting;watcherProgress=save.watcherProgress;uumuuProgress=save.uumuuProgress;
    if(!std::strcmp(save.scene,"Tutorial_01"))rememberTutorialGate(save.gate);
    flow.progress.broken=save.broken;floorBroken=save.flags&1;flow.progress.collapsed=save.flags&2;
    charmInventory=save.charms;if(save.flags&128)charmInventory.owned|=charmBit(6);
    geo=save.geo;crawlerJournalKills=save.crawlerKills;buzzerJournalKills=save.buzzerKills;
    secretFlags=save.flags&TutorialSave::secretFlags;
    bossFlags_=save.flags&TutorialSave::bossFlags;
    if(playerData_.get("openedCityGate")){bossFlags_&=~TutorialSave::cityCrestFlag;sceneSave_.flags&=~TutorialSave::cityCrestFlag;}
    // Older Dung Defender saves used the bit now assigned to False Knight's first plop.
    // The persisted PlayerData event unambiguously recovers a completed Dung encounter.
    if(playerData_.get("defeatedDungDefender"))bossFlags_|=TutorialSave::dungDefenderFlag;
    greenpath.clear(bossFlags_);
    abilityFlags_=save.flags&TutorialSave::abilityFlags;
    shadePresent_=save.shadePresent!=0;shadeGeo_=save.shadeGeo;shadeSpawn_={save.shadeX,save.shadeY};shadeHealth_=save.shadeHealth;
    conditionalEvents.reset(save.conditional);
    prompts.reset(save.flags);
    areaTitle.reset(save.flags&AreaTitle::visitedCrossroadsFlag);
    journal[0]={crawlerJournalKills>0,crawlerJournalKills>0,0};
    journal[1]={buzzerJournalKills>0,buzzerJournalKills>0,buzzerJournalKills>=45?0:45-buzzerJournalKills};
    if(!restart(true))return false;
    collectibles.restore(save.collectibles,simulation.hero.vitals);
    // Promote only the loaded ledger, never the previous slot's transient state.
    const auto event=worldQuests.definition().event;
    const auto* field=WorldQuests::playerDataField(event);
    if(field&&worldQuests.completed(event,collectibles.saved()))setPlayerData(field,1);
    resetWorldQuest();
    applyAbilities();
    enterAbilityPickup();
    credits={};credits.begin((save.flags&OpeningCredits::playedFlag)!=0);
    furyChest.restore(save.flags);
    resting.applyCharms(simulation.hero);
    charmInventory.apply(simulation.hero);
    if((save.flags&4)&&greatDoorConfig_)greatDoor.bind(*greatDoorConfig_,*terrain_,objects[crawlerCount+doorCount+buzzerCount+stalactiteCount],true);
    // Restart precedes restoration of the persistent ability pickup ledger.
    applySceneActivation();
    return sceneEnemies.updateActivation(activation_,terrain_->world());
}
bool TutorialSession::restoreSceneEntry(const TutorialSave& save,const SceneHeroState& entryState,const char* gate)
{
    if(!gate||!restore(save))return false;
    if(std::strcmp(gate,"bench"))entryState.apply(simulation.hero);
    return true;
}
void TutorialSession::floorHit(void* context,const NailHit&,HitOutcome)
{
    auto& self=*static_cast<TutorialSession*>(context);
    if(self.floorBroken||self.floorWait_>0||!roomOverlap(bodyBounds(self.simulation.hero.position),self.floorRange_))return;
    ++self.floorHits;self.floorWait_=self.rules_->breakFloorWait;
    self.decorEffects.ownerBreak(self.floorHits==1?2422:self.floorHits==2?2423:2421,Strike::Down,self.terrain_->world());
    // level6 FSM12191 Hit 1/2: Self (Transform6289 through parent6422)
    // plus SpawnObjectFromGlobalPool position (0,-2,0).
    if(self.floorHits<3)self.decorEffects.nailHit({144.21f,59.01f},-.02f);
    if(self.floorHits>=3)
    {
        self.floorBroken=true;self.objects[self.floorIndex_].colliderEnabled=false;
        for(unsigned i=0;i<self.floorConfig_.count;++i)self.terrain_->setEnabled(self.floorConfig_.disabledShapes[i],false);
        self.decorEffects.floorBreak({144.21f,61.01f},self.terrain_->world());
    }
}
void TutorialSession::combatTrace(void* context,const CombatTrace& trace)
{
    if(trace.phase!=CombatPhase::HealthHit)return;
    auto& self=*static_cast<TutorialSession*>(context);
    if(trace.outcome==HitOutcome::Damaged||trace.outcome==HitOutcome::Killed){
        Vec2 origin{};float offset=0;bool sourceOwner=false;
        if(trace.target<self.crawlerCount){
            origin=self.crawlers[trace.target].position;offset=-.2f;sourceOwner=true;
        }else if(self.shadeConfig_&&self.shadePresent_&&trace.target==self.objectCount_-1){
            origin=self.shade.position;offset=-.5f;sourceOwner=true;
        }else{
            const unsigned first=self.crawlerCount+self.doorCount;
            if(trace.target>=first&&trace.target<first+self.buzzerCount){
                origin=self.buzzers[trace.target-first].position;sourceOwner=true;
            }
        }
        if(sourceOwner){
            // HealthManager.Hit: midpoint of nail collider Transform and Self,
            // then authored effectOrigin (crawler Y=-.2, Shade Y=-.5, buzzer zero).
            const auto& h=self.simulation.hero;
            const auto nail=nailPoint(h,self.nails_->shapes[nailVariant(h.nail)],{});
            origin={(origin.x+nail.x)*.5f,(origin.y+nail.y)*.5f+offset};
            self.decorEffects.enemyNailHit(origin,h.nail.direction);
        }
    }
    if(trace.outcome!=HitOutcome::Killed)return;
    JournalEnemy type;Vec2 position;float scale=1;unsigned count=0;
    if(trace.target<self.crawlerCount)
    {
        const unsigned i=trace.target;if(self.crawlerRecorded_[i])return;
        self.crawlerRecorded_[i]=true;++self.crawlerJournalKills;
        type=JournalEnemy::Crawler;position=self.crawlers[i].position;scale=self.crawlers[i].scaleX;count=self.crawlerConfig_.smallGeo;
    }
    else
    {
        const unsigned first=self.crawlerCount+self.doorCount;
        if(trace.target<first||trace.target>=first+self.buzzerCount)return;
        const unsigned i=trace.target-first;if(self.buzzerRecorded_[i])return;
        self.buzzerRecorded_[i]=true;++self.buzzerJournalKills;
        type=JournalEnemy::Buzzer;position=self.buzzers[i].position;count=self.entities_->buzzers[i].smallGeo;
    }
    if(self.geoConfig_)
    {
        recordJournalKill(type,*self.geoConfig_,self.journal[static_cast<unsigned>(type)],false);
        if(!self.coins.spawn(count,type,position,scale))++self.rejectedDrops;
    }
}
bool TutorialSession::visible(unsigned owner) const
{
    return visualAlpha(owner)>0;
}
void TutorialSession::captureVisualAlphaInputs(const std::vector<unsigned>& owners,std::vector<VisualAlphaInput>& output) const
{
    enum Source : unsigned {
        CityToll, ShamanPillar, MantisGates, RouteTolls, Activation, Monomon, SceneWalls, ArchiveShortcut, RoomMasks, TerrainLevers,
        AbilityPickup, Waterways, SceneEnemies, Collectibles, WorldQuests, Collapse, Floor, ContactEffects, Stalactites, Ambient,
        RoomBreakables, HealthCocoon, Plants, Decor, Secrets, Remasks, Reveals, Doors, DoorFade, DreamPlatforms
    };
    output.clear();
    // Capture controller outputs once per affected owner, rather than re-walking
    // every controller/rule for every room draw. Keep each input separate: a
    // hidden owner must still notice changes to its other dependencies.
    const auto add=[&](unsigned group,unsigned owner,float value,unsigned slot=0,bool terminal=false){
        if(terminal||value!=1.f)output.push_back({owner,(group<<16)|slot,value});
    };
    cityToll.visitAlphaOwners([&](unsigned owner){add(CityToll,owner,cityToll.hides(owner)?0.f:1.f);});
    shamanPillar.visitAlphaOwners([&](unsigned owner){add(ShamanPillar,owner,shamanPillar.hides(owner)?0.f:1.f);});
    mantisGates.visitAlphaOwners([&](unsigned owner){add(MantisGates,owner,mantisGates.hides(owner)?0.f:1.f);});
    for(const auto owner:routeTolls.opened?routeTolls.hidePaid:routeTolls.hideUnpaid)add(RouteTolls,owner,0);
    activation_.visitDisabledVisuals([&](unsigned owner){add(Activation,owner,0);});
    // These scene controllers contain authored special-case owners. Query the
    // room's dense owner set only while the corresponding encounter is active.
    if(monomon.active())for(const auto owner:owners)add(Monomon,owner,monomon.visualAlpha(owner));
    sceneWalls.visitAlphaOwners([&](unsigned owner){add(SceneWalls,owner,sceneWalls.alpha(owner));});
    archiveShortcut.visitHiddenOwners([&](unsigned owner){add(ArchiveShortcut,owner,0);});
    if(!std::strcmp(sceneName(),shamanquest::scene)&&playerData_.get("shaman")>=shamanquest::gateOpenedState)
        add(SceneEnemies,shamanquest::boneGateOwner,0);
    roomMasks.visitAlphaOwners([&](unsigned owner){add(RoomMasks,owner,roomMasks.alpha(owner));});
    if(sceneActive_&&terrain_)for(const auto& lever:terrain_->levers)add(TerrainLevers,lever.gate,terrain_->leverAlpha(lever.gate));
    abilityPickup.visitHiddenOwners([&](unsigned owner){add(AbilityPickup,owner,0);});
    captureDreamPlatformAlpha(DreamPlatforms<<16,output);
    for(const unsigned owner:{222u,223u,220u,218u,219u,238u,152u})if(waterwaysHides(owner))add(Waterways,owner,0);
    if(sceneActive_)for(const auto owner:owners)if(sceneEnemies.hides(owner,crossroadsBoss))add(SceneEnemies,owner,0);
    for(unsigned i=0;i<collectibles.records().size();++i)if(collectibles.collected(i)){
        const auto& record=collectibles.records()[i];add(Collectibles,record.owner,0);
        for(const auto owner:record.visualOwners)add(Collectibles,owner,0);
    }
    const auto& quest=worldQuests.definition();
    if(worldQuests.hides(quest.owner))add(WorldQuests,quest.owner,0);
    for(unsigned i=0;i<quest.ownerCount;++i)if(worldQuests.hides(quest.owners[i]))add(WorldQuests,quest.owners[i],0);
    if(!sceneActive_){
        if(flow.progress.collapsed)roomDynamics.visitCollapseOwners([&](unsigned owner){add(Collapse,owner,0);});
        if(floorBroken&&rules_)for(const auto owner:rules_->breakFloorOwners)add(Floor,owner,0);
        if(contactEffects.ready())for(const auto& drip:contactEffects.bank().drips)add(ContactEffects,drip.owner,0);
        for(unsigned i=0;i<stalactiteVisuals.count();++i)add(Stalactites,stalactiteVisuals.record(i).owner,0);
        if(roomAmbient.ready())for(const auto& tablet:roomAmbient.tablets()){
            add(Ambient,tablet.config.lit,roomAmbient.alpha(tablet.config.lit));
            add(Ambient,tablet.config.light,roomAmbient.alpha(tablet.config.light));
        }
        if(healthCocoon.broken())add(HealthCocoon,971,0);
    }else roomBreakables.visitAlphaOwners([&](unsigned owner){add(RoomBreakables,owner,roomBreakables.alpha(owner)==0?0.f:1.f);});
    if(!rules_)return;
    // Rules below return even at alpha 1. Record their membership and slot as
    // well, so a rebind cannot silently change precedence for overlapping rules.
    if(!sceneActive_&&plantBank_)for(unsigned i=0;i<plantBank_->count;++i){
        const auto& plant=plantBank_->plants[i];
        add(Plants,plant.owner,plantBreaks[i].alpha(plant.owner),i,true);
        add(Plants,plant.collision.remnant,plantBreaks[i].alpha(plant.collision.remnant),i,true);
    }
    if(!sceneActive_&&decorBank_)for(unsigned i=0;i<decorBank_->count;++i){
        const auto& item=decorBank_->items[i];
        for(const auto owner:{item.whole,item.remnant})add(Decor,owner,decor[i].alpha(owner),i,true);
    }
    for(unsigned i=0;i<rules_->secrets.size();++i){
        const auto& secret=rules_->secrets[i];
        add(Secrets,secret.owner,secretActivate_[i]<0?1:std::max(0.f,1-secretActivate_[i]/
            (secretReload_[i]?secret.reloadSeconds:secret.discoverySeconds)),i,true);
    }
    for(unsigned i=0;i<rules_->remasks.size();++i)for(const auto owner:rules_->remasks[i].owners)
        add(Remasks,owner,remaskFade_[i].alpha,i,true);
    for(unsigned i=0;i<rules_->reveals.size();++i){
        const auto& reveal=rules_->reveals[i];
        for(const auto owner:reveal.owners)add(Reveals,owner,revealFade_[i]<0?1:std::max(0.f,1-revealFade_[i]/reveal.seconds),i,true);
    }
    for(unsigned i=0;i<doorCount;++i){
        const auto& door=rules_->doors[i];const bool broken=doors[i].broken();
        add(Doors,door.owner,broken?0.f:1.f,i,true);
        for(const auto owner:door.wholeOwners)add(Doors,owner,broken?0.f:1.f,i,true);
        for(const auto owner:door.fadeOwners)add(DoorFade,owner,broken?std::max(0.f,1-doorFade_[i]/door.fadeSeconds):1,i,true);
    }
}
float TutorialSession::visualAlpha(unsigned owner) const
{
    if(owner==shamanquest::boneGateOwner&&!std::strcmp(sceneName(),shamanquest::scene)&&
       playerData_.get("shaman")>=shamanquest::gateOpenedState)return 0;
    if(cityToll.hides(owner)||shamanPillar.hides(owner)||mantisGates.hides(owner))return 0;
    if(routeTolls.hides(owner))return 0;
    if(!activation_.enabled(SceneActivationTarget::Visual,owner))return 0;
    const float encounterAlpha=monomon.visualAlpha(owner)*sceneWalls.alpha(owner)*archiveShortcut.alpha(owner)*roomMasks.alpha(owner)*
                               (sceneActive_&&terrain_?terrain_->leverAlpha(owner):1.f);
    if(encounterAlpha!=1)return encounterAlpha;
    if(abilityPickup.hides(owner)||waterwaysHides(owner))return 0;
    const float dreamAlpha=dreamPlatformAlpha(owner);
    if(dreamAlpha!=1)return dreamAlpha;
    if(sceneActive_&&sceneEnemies.hides(owner,crossroadsBoss))return 0;
    if(collectibles.hides(owner)||worldQuests.hides(owner))return 0;
    // These banks stay resident from Tutorial; their owner IDs are scene local.
    if(!sceneActive_&&flow.progress.collapsed&&roomDynamics.collapseOwner(owner))return 0;
    if(!sceneActive_&&floorBroken&&rules_&&
       std::find(rules_->breakFloorOwners.begin(),rules_->breakFloorOwners.end(),owner)!=rules_->breakFloorOwners.end())return 0;
    if(!sceneActive_&&contactEffects.owns(owner))return 0;
    if(!sceneActive_&&stalactiteVisuals.hides(owner))return 0;
    const float ambientAlpha=sceneActive_?1:roomAmbient.alpha(owner);
    if(ambientAlpha!=1)return ambientAlpha;
    if(sceneActive_&&roomBreakables.alpha(owner)==0)return 0;
    // HealthCocoon.SetBroken disables this source haze child with the mesh.
    if(!sceneActive_&&owner==971&&healthCocoon.broken())return 0;
    if(!rules_)return 1;
    if(!sceneActive_&&plantBank_)for(unsigned i=0;i<plantBank_->count;++i)
    {
        const auto& plant=plantBank_->plants[i];
        if(owner==plant.owner||owner==plant.collision.remnant)return plantBreaks[i].alpha(owner);
    }
    if(!sceneActive_&&decorBank_)for(unsigned i=0;i<decorBank_->count;++i)
    {
        const auto& c=decorBank_->items[i];
        if(owner==c.whole || owner==c.remnant)return decor[i].alpha(owner);
    }
    for(unsigned i=0;i<rules_->secrets.size();++i)
        if(owner==rules_->secrets[i].owner)
            return secretActivate_[i]<0?1:std::max(0.f,1-secretActivate_[i]/
                (secretReload_[i]?rules_->secrets[i].reloadSeconds:rules_->secrets[i].discoverySeconds));
    for(unsigned i=0;i<rules_->remasks.size();++i)
    {
        const auto& owners=rules_->remasks[i].owners;
        if(std::find(owners.begin(),owners.end(),owner)!=owners.end())return remaskFade_[i].alpha;
    }
    for(unsigned i=0;i<rules_->reveals.size();++i)
    {
        const auto& reveal=rules_->reveals[i];
        if(std::find(reveal.owners.begin(),reveal.owners.end(),owner)!=reveal.owners.end())
            return revealFade_[i]<0?1:std::max(0.0f,1-revealFade_[i]/reveal.seconds);
    }
    for(unsigned i=0;i<doorCount;++i)
    {
        const auto& d=rules_->doors[i];
        if(d.owner==owner||std::find(d.wholeOwners.begin(),d.wholeOwners.end(),owner)!=d.wholeOwners.end())
            return doors[i].broken()?0:1;
        if(std::find(d.fadeOwners.begin(),d.fadeOwners.end(),owner)!=d.fadeOwners.end())
            return doors[i].broken()?std::max(0.0f,1-doorFade_[i]/d.fadeSeconds):1;
    }
    return 1;
}
}

namespace hk3ds {
// Prompts for actions in the room are drawn on the top screen like the original Arrow Prompt;
// nothing is requested while a dialogue, shop, bench menu or pickup already owns the screen.
ArrowPromptRequest TutorialSession::gameplayPrompt() const
{
    const Vec2 at=simulation.hero.position;
    if(slyShop.open||routeTolls.menu.open||cityGate.menu.open||cartographer.locked()||conditionalEvents.dialogue!=EventDialogue::None||
       worldQuestDialogue.locked()||inspection.locked()||abilityPickup.locked()||resting.sitting||
       resting.travelMenu||resting.charmMenu)return {};
    if(slyShop.prompt)return {"SHOP",at};
    if(cityGate.menu.prompt)return {"INSPECT",at};
    if(routeTolls.menu.prompt)return {"INSPECT",at};
    if(resting.prompt){
        if(rules_&&resting.nearby>=0&&unsigned(resting.nearby)<rules_->restPoints.size()&&
           !std::strcmp(sceneName(),"Town")){
            const auto& point=rules_->restPoints[resting.nearby];
            if(!point.kind)
                return {"REST",{point.position.x,point.position.y+3.39f},true};
        }
        return {"REST",at};
    }
    // Stag toll and station prompts: the original Arrow Prompt replaces the port "UP: Inspect" / "UP: Listen" text.
    if(resting.tollPrompt||(worldQuestDialogue.prompt&&(!std::strcmp(worldQuestDialogue.promptLabel,"Inspect")||!std::strcmp(worldQuestDialogue.promptLabel,"INSPECT"))))return {"INSPECT",at};
    if(resting.talkPrompt)return {"LISTEN",at};
    if(conditionalEvents.listenPrompt||worldQuestDialogue.prompt)return {"LISTEN",at};
    if(cartographer.prompt)return {"LISTEN",{36.31f,37.9078f},true};
    if(abilityPickup.near)return {abilityPickup.kind==AbilityPickup::Kind::Spirit?"LISTEN":"INSPECT",at};
    return {};
}
}
