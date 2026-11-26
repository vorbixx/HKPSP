#pragma once
#include "uumuu_room.hpp"
#include "monomon_room.hpp"
#include "cartographer.hpp"
#include "route_tolls.hpp"
#include "city_gate.hpp"
#include "enemy_c.hpp"
#include "nail_interactions.hpp"
#include "world_switch_room.hpp"
#include "world_town_lift.hpp"
#include "world_city_lift.hpp"
#include "world_city_transport.hpp"
#include "world_city_crest_room.hpp"
#include "world_city_crest_effects.hpp"
#include "world_jiji_door.hpp"
#include "world_waterways_room.hpp"
#include "world_city_toll.hpp"
#include "world_stag_lever.hpp"
#include "world_shaman_pillar.hpp"
#include "world_mantis_runtime.hpp"
#include "world_switch_save.hpp"
#include "arrow_prompt.hpp"
#include "door_prompt_presentation.hpp"
#include "room.hpp"
#include "scene_lifecycle.hpp"
#include "hero_death.hpp"
#include "hero_effects.hpp"
#include "focus_effects.hpp"
#include "great_door.hpp"
#include "save.hpp"
#include "world_quests.hpp"
#include "sly_shop.hpp"
#include "fury_chest.hpp"
#include "plant.hpp"
#include "room_dynamics.hpp"
#include "scene_walls.hpp"
#include "archive_shortcut.hpp"
#include "grass_scene.hpp"
#include "lighting_math.hpp"
#include "ambient_particles.hpp"
#include "geo_visual.hpp"
#include "health_cocoon.hpp"
#include "health_cocoon_fx.hpp"
#include "health_scuttler.hpp"
#include "tutorial_prompts.hpp"
#include "opening_credits.hpp"
#include "initial_fall.hpp"
#include "lore_tablets.hpp"
#include "contact_effects.hpp"
#include "stalactite_visual.hpp"
#include "shade.hpp"
#include "conditional_events.hpp"
#include "room_ambient.hpp"
#include "world_transport_save.hpp"
#include "area_title.hpp"
#include "crossroads_boss.hpp"
#include "scene_enemies.hpp"
#include "ability_pickup.hpp"
#include "greenpath_enemy.hpp"
#include "world_stag_toll.hpp"
#include <array>
namespace hk3ds
{
// A semantic input to visualAlpha. Missing factors have value 1; terminal rules
// also retain identity inputs to preserve membership/precedence on a rebind.
// source identifies a controller/rule slot, preserving overlapping owner rules.
struct VisualAlphaInput {unsigned owner=0,source=0;float value=1;};
class TutorialSession
{
    friend struct SimulationStateHash;
public:
    TutorialPrompts prompts;
    WorldSwitchRuntime worldSwitch;
    WorldSwitchVisuals worldSwitchVisuals;
    TownMineLift townLift;
    JijiDoorRuntime jijiDoor;
    CityCrestGate cityCrest;
    CityCrestDust cityCrestDust;
    WaterwaysRoom waterways;
    CityLiftToll cityToll;
    StagStationToll stagToll;
    // The Crossroads_47 toll took hero control; it returns when the toll reaches Open (see updateWorldQuest).
    bool stagTollHoldsControl=false;
    StagStationLever stagLever;
    ShamanPillar shamanPillar;
    MantisGates mantisGates;
    bool bindSavedMantisGates(const char* directory);
    bool bindSavedShamanPillar(const char* directory);
    ShamanPillarHooks shamanPillarSaveHooks();
    CityLiftLowering cityLift;
    CityLiftTransport cityTransport;
    // presentation: art already bound by the caller (PSP PSA1 table); null loads the HK3DS sidecars.
    bool bindSavedCityToll(const char* directory,const CityLiftArtAssets* presentation=nullptr);
    bool bindSavedStagStation(const char* directory);
    bool bindSavedStagLever(const char* directory);
    StagBellHooks stagLeverSaveHooks();
    bool bindSavedWaterways(const char* directory);
    bool bindSavedWaterways(const WaterwaysRoomData& data);
    bool bindSavedCityCrest(const char* directory);
    bool bindSavedCityCrest(const CityCrestRoomData& data);
    bool bindSavedJijiDoor(const char* directory);
    bool bindSavedJijiDoor(const JijiDoorRoomData& data);
    const RoomVisualBank& worldActorBank()const{return cityCrestDust.ready()?cityCrestDust.visuals:shamanPillar.bound()?shamanPillar.bank():mantisGates.bound()?mantisGates.bank():stagLever.bound()?stagLever.bank():stagToll.hasArt()?stagToll.bank():townLift.bound()?townLift.bank():waterways.bound()?waterways.bank():cityLift.bound()?cityLift.bank():worldSwitchVisuals.bank();}
    bool bindSavedTownLift(const TownLiftRoomData&,const AnimationBank&);

    // Data/animation/identity storage must outlive this scene binding. The save
    // owner supplies callbacks; an absent schema cannot admit an unlocked gate.
    bool bindWorldSwitch(const WorldSwitchRoomData&,const AnimationBank&,SceneBoolHooks);
    bool bindSavedWorldSwitch(const WorldSwitchRoomData& data,const AnimationBank& art){
        return bindWorldSwitch(data,art,worldSceneBoolSaveHooks());
    }
    AbilityPickup abilityPickup;
    AreaTitle areaTitle;
    OpeningCredits credits;
    InitialFallPresentation initialFall;
    LoreTabletBank loreBank;
    LoreInspection inspection;
    bool bindLore(const char* path) {if(!loreBank.load(path))return false;inspection.bind(loreBank);return true;}
    bool bind(const RoomConfig& rules,TerrainBank& terrain,const AnimationBank& heroBank,
              const AnimationBank& crawlerBank,const CrawlerConfig& crawlerConfig,
              const NailBank& nails,const RoomEntityBank& entities,EnemyRandom random,const GreatDoorConfig* greatDoor=nullptr,const GeoConfig* geoConfig=nullptr,const AnimationBank* buzzerBank=nullptr,const DecorBank* decorBank=nullptr,float viewportAspect=400.0f/240.0f,bool visitedDirtmouth=false,const PlantBank* plantBank=nullptr,const ShadeConfig* shadeConfig=nullptr,const AnimationBank* shadeBank=nullptr);
    void setViewportAspect(float aspect);
    void bindLighting(const LightingConfig* config){lightingConfig_=config;darkness.reset(config,hasLantern());heroLight.reset(config);}
    DarknessState darkness;
    HeroLightState heroLight;
    bool restart(bool retry=false);
    void enterScene(const RoomConfig& rules,TerrainBank& terrain,const char* scene,const char* gate);
    bool sceneActive() const {return sceneActive_;}
    const char* sceneName() const {return sceneActive_?sceneSave_.scene:"Tutorial_01";}
    void rememberTutorialGate(const char* gate);
    // A fixed view override also moves the emitters, never existing world motes.
    void advance(double seconds,InputLatch& input,const Vec2* emitterCamera=nullptr);
    // Only the explicitly enabled test-command adapter writes these controls.
    // A frozen Rigidbody2D can still deliver trigger callbacks in source fixtures.
    bool testPinnedHeroTriggers=false;
    void setFrameCaptureActive(bool active,bool pinHero=false)
    {
        // The source fixture disables HeroController/Rigidbody2D, while scene
        // effects and FSM timers keep advancing at the requested fixed steps.
        // Canonicalize once per capture epoch; later fixed steps keep their phase.
        if(active&&!testFrameCaptureActive_){accumulator_=0;simulation.frameTime.remainder=0;}
        testFrameCaptureActive_=active;testHeroPinned_=active&&pinHero;
    }
    bool testFreezeEnemies=false,testInvulnerable=false;
    CrossroadsBoss crossroadsBoss;
    SceneEnemies sceneEnemies;
    SceneWalls sceneWalls;
    RoomMasks roomMasks;
    ArchiveShortcut archiveShortcut;
    WatcherProgress watcherProgress{}; // Typed save-owner hook; no private save-bit allocation.
    UumuuProgress uumuuProgress{};
    UumuuRoom uumuu;
    MonomonRoom monomon;
    EnemyLifeState enemyLife;
    char sceneAssetRoot_[256]{};
    bool bindSceneEnemies(const SceneBank& scene,const char* root,const SavedPlayerData* dreamArrival=nullptr,
                          bool loadLegacyCityGate=true){
        std::snprintf(sceneAssetRoot_,sizeof(sceneAssetRoot_),"%s",root);
        if(!std::strcmp(sceneName(),"Room_shop")&&!slyShop.loadRegion(root))return false;
        const bool tollPaid=!std::strcmp(sceneName(),"Crossroads_49b")?playerData_.get("cityLift1")!=0:
                            collectibles.saved().contains(!std::strcmp(sceneName(),"Fungus1_31")?158001811:272000380);
        if(!routeTolls.load(root,sceneName(),*terrain_,tollPaid,
                            !std::strcmp(sceneName(),"Crossroads_49b"),collectibles.saved().contains(158000686)))return false;
        if(loadLegacyCityGate&&!cityGate.load(root,sceneName(),*terrain_,playerData_.get("openedCityGate")!=0,playerData_.get("cityGateClosed")!=0))return false;
        greenpath.setLifecycle(&enemyLife,sceneName(),scene.enemies);
        return sceneEnemies.bind(scene,root,*nails_,*crawlerBank_,crawlerConfig_,random_,terrain_->world(),sceneSave_.flags,crossroadsBoss,
                                 watcherProgress,sceneSave_.conditional.nailSmithUpgrades,sceneName(),&enemyLife,&activation_,&sceneSave_.sceneBools,playerData_.get("defeatedMantisLords")!=0,BrokenVesselProgress::read(playerData_),playerData_.get("openedCityGate")!=0)&&
               uumuu.enter(root,sceneName(),*nails_,random_,uumuuProgress)&&
               monomon.enter(root,sceneName(),*nails_,dreamArrival?*dreamArrival:playerData_)&&bindSceneBreakables(root);
    }
    void bindBossArt(const AnimationBank* bank){bossBank_=bank;if(crossroadsBoss.active())crossroadsBoss.bindArt(bank);}
    bool startBossArena(unsigned kind);
    bool startGreenpathArena(unsigned kind);
    unsigned testDecorLayers=7;
    bool testIsolateDecor=false;
    bool testBreakDecor(unsigned owner,AttackType type=AttackType::Nail);
    unsigned testDecorBreaks() const;
    void presentPlants();
    bool ambientEnabled() const{return !sceneActive_||sceneAmbient_;}
    void presentAmbient(){if(ambientEnabled())ambientParticles.present(presentationTime());}
    void setSceneAmbient(bool enabled,unsigned zone=5,const char* config=nullptr){
        bool loaded=ambientParticles.ready();
        if(enabled){
            // A room-specific bank is loaded and bound by the scene loader before
            // this flag is published.  Keep that bank; only select a resident
            // zone when there is no already-loaded bank.  Explicit fixtures still
            // replace the bank so their source path remains authoritative.
            if(config)loaded=ambientParticles.load(config);
            else if(!loaded)loaded=ambientParticles.selectZone(zone);
        }else loaded=false;
        sceneAmbient_=enabled&&loaded;ambientParticles.reset();
        if(sceneAmbient_&&ambientParticles.ready())ambientParticles.prewarm({flow.camera.x,flow.camera.y});
    }
    // Captures compare the committed frame with Unity's non-interpolated bodies.
    // Normal rendering retains the shared delayed display clock.
    PresentationTime presentationTime() const {return testFrameCaptureActive_?PresentationTime{1}:simulation.presentationTime(accumulator_);}
    float levelSeconds() const {return float(levelSeconds_+accumulator_*simulation.clock.scale);}
    Vec2 heroRenderPosition() const {return simulation.heroRenderPosition(accumulator_);}
    Vec2 cameraRenderPosition() const {return testFrameCaptureActive_?flow.camera:flow.renderCamera(accumulator_);}
    bool visible(unsigned owner) const;
    float visualAlpha(unsigned owner) const;
    void captureVisualAlphaInputs(const std::vector<unsigned>& owners,std::vector<VisualAlphaInput>& output) const;
    // Exact original PlayerData names; progression adapters can grant flags here.
    bool setPlayerData(const char* field,std::int32_t value);
    bool setPlayerData(std::uint32_t field,std::int32_t value);
    const SavedPlayerData& playerData() const {return playerData_;}
    // Transport controllers publish through session activation and revision.
    LiftPlayerBoolHooks worldLiftSaveHooks();
    SceneBoolHooks worldSceneBoolSaveHooks();
    JijiKeyDoorSaveHooks worldKeyDoorSaveHooks();
    TollSaveHooks worldTollSaveHooks();
    bool hasLantern() const {return playerData_.get("hasLantern")!=0;}
    TutorialSave saved() const;
    bool restore(const TutorialSave& save);
    bool restoreSceneEntry(const TutorialSave& save,const SceneHeroState& entryState,const char* gate);
    // Empty-slot placeholders must not turn New Game into a grounded Continue.
    bool restoreContinue(const char* path,bool newGame,TutorialSave& save)
    {return !newGame&&save.load(path)&&restore(save);}
    // Room scripts call this when an original ability reward is collected.
    bool collectAbility(AbilityReward reward);
    bool completeWorldQuest(WorldQuestEvent);
    bool canTalkWorldQuest() const;
    bool worldQuestInteraction(WorldQuestInteraction);
    // The shop actor uses the same source Idle as rescued Sly, with its shop transform.
    bool slyShopVisible() const {return !std::strcmp(sceneName(),"Room_shop")&&playerData_.get("slyRescued");}
    bool shopMenuOpen() const {return slyShop.open||routeTolls.menu.open||cityGate.menu.open;}
    AnimationPlayer slyShopAnimation;
    WorldQuests worldQuests;
    WorldQuests tiso;
    bool talkingToTiso=false;
    bool lemmShopSelected=false;
    WorldQuests& dialogueQuest(){return talkingToTiso?tiso:worldQuests;}
    const WorldQuests& dialogueQuest() const{return talkingToTiso?tiso:worldQuests;}
    WorldQuestDialogue worldQuestDialogue;
    // Diagnostic fixture uses the ordinary transition loader, without grants.
    bool testWorldQuestRoom() {
        if(!worldQuests.definition().sourceId)return false;
        requestRestTransition(worldQuests.definition().scene,"left1");return true;
    }
    bool hasAbility(AbilityReward reward) const;
    bool testSceneLeverState(unsigned owner,bool activated,bool solid,bool visible)const {
        if(!sceneActive_||!terrain_)return false;
        bool found=false;
        for(const auto& lever:terrain_->levers)if(lever.owner==owner){
            found=true;
            if(lever.shape>=terrain_->shapes.size()||lever.state.activated!=activated||
               sceneSave_.sceneBools.get(lever.key)!=activated||
               lever.state.colliderEnabled()!=solid||terrain_->shapes[lever.shape].enabled!=solid||
               lever.state.visible()!=visible)return false;
        }
        return found;
    }
    unsigned saveRevision=0,crawlerJournalKills=0,buzzerJournalKills=0,geo=0;
    Resting resting;
    Collectibles collectibles;
    unsigned secretFlags=0;
    std::array<unsigned,2> secretCues{};
    // Audio adapters clear detached voices and serial snapshots on every room rebuild.
    unsigned audioEpoch=0;
    Simulation simulation;
    RoomFlow flow;
    DoorPromptPresentation doorPromptPresentation;
    ArrowPrompt arrowPrompt; // top-screen Listen/Rest/Shop/Inspect prompt, fed by gameplayPrompt()
    ArrowPromptRequest gameplayPrompt() const;
    HeroDeathPresentation death;
    HeroEffects heroEffects;
    FocusEffects focusEffects;
    CombatScene combat;
    EnemyCEncounter enemyC;
    GreenpathEncounter greenpath;
    GreatDoor greatDoor;
    RoomPlants plants;
    RoomDynamics roomDynamics;
    RoomBreakables roomBreakables;
    RoomGrass grass;
    DecorEffects decorEffects;
    RoomContactEffects contactEffects;
    NailInteractions nailInteractions;
    AmbientParticles ambientParticles;
    GeoBank coins;
    GeoVisuals geoVisuals;
    StalactiteVisuals stalactiteVisuals;
    FuryChest furyChest;
    CharmInventory charmInventory;
    Cartographer cartographer;
    SlyShop slyShop;
    CityGate cityGate;
    bool openCityGate();
    void updateCityGate(const Input&);
    RouteTolls routeTolls;
    bool buyRouteToll();
    void updateRouteToll(const Input& input);
    bool slyStockVisible(unsigned index) const;
    bool buySlyItem(unsigned index);
    void updateSlyShop(const Input& input);
    bool updateCartographer(float,const Input&);
    CharmsPage charmsPage;
    bool canEquipCharms() const { const auto& h=simulation.hero;return resting.sitting&&flow.phase==RoomPhase::Playing&&!inspection.locked()&&!worldQuestDialogue.locked()&&h.actor!=Actor::Dead&&h.actor!=Actor::Damage&&!furyChest.collecting(); }
    void syncRestCharms(bool fromRest=false);
    bool toggleCharm() { if(!canEquipCharms())return false;const bool changed=charmInventory.toggle(charmsPage.selected,charmsPage.overcharmAttempts);if(changed){syncRestCharms();charmInventory.apply(simulation.hero);++saveRevision;}return changed; }
    bool buyCharmNotch() {const bool changed=charmInventory.buyNotch(geo,sceneName());if(changed)++saveRevision;return changed;}
    bool charmStockAvailable() const {
        if(!std::strcmp(sceneName(),"Room_shop"))return playerData_.get("slyRescued")!=0;
        if(!std::strcmp(sceneName(),"Room_mapper"))return playerData_.get("openedMapperShop")!=0;
        return true;
    }
    bool buyCharm();
    HealthCocoon healthCocoon;
    HealthCocoonFx healthCocoonFx;
    HealthScuttlers healthScuttlers;
    HollowShade shade;
    ConditionalEvents conditionalEvents;
    RoomAmbient roomAmbient;
    HealthCocoon::BreakEffect healthCocoonBreakEffect=nullptr;
    void* healthCocoonEffectContext=nullptr;
    std::array<JournalProgress,2> journal{};
    unsigned rejectedDrops=0;
    unsigned floorHits=0;
    bool floorBroken=false;
    std::array<Crawler,16> crawlers;
    std::array<BreakableDoor,16> doors;
    std::array<BreakableDecor,DecorBank::capacity> decor;
    std::array<BreakablePlant,4> plantBreaks;
    std::array<Buzzer,RoomEntityBank::maxBuzzers> buzzers;
    double enemyAudioSeconds()const{return levelSeconds_;}
    float crawlerAudioDepth(unsigned i)const{return crawlers[i].active()?crawlerSourceDepth_[i].value(levelSeconds_):rules_->crawlers[i].audioInitialDepth;}
    double crawlerAudioActivatedAt(unsigned i)const{return crawlerSourceDepth_[i].activatedAt();}
    float buzzerAudioDepth(unsigned i)const{return buzzerSourceDepth_[i].value(levelSeconds_);}
    std::array<Stalactite,RoomEntityBank::maxStalactites> stalactites;
    std::array<CombatObject,CombatScene::capacity> objects;
    unsigned crawlerCount=0,doorCount=0,buzzerCount=0,stalactiteCount=0;
private:
    std::array<EnemySourceDepth,16> crawlerSourceDepth_{};
    std::array<EnemySourceDepth,RoomEntityBank::maxBuzzers> buzzerSourceDepth_{};
    bool bindSceneBreakables(const char* root);
    static void roomBreakableHit(void*,const RoomBreakableConfig&,unsigned,const NailHit&);
    bool reloadWorldTransport();
    bool worldQuestCallbackAllowed() const;
    bool commitWorldQuest(WorldQuestEvent,bool beginTalk);
    void updateWorldQuest(float seconds);
    void resetWorldQuest();
    bool sceneActive_=false;
    bool sceneAmbient_=false;
    TutorialSave sceneSave_{};
    SavedPlayerData playerData_{};
    bool commitPlayerData(const SavedPlayerData& next);
    SceneActivationState activation_;
    void applySceneActivation();
    bool cloakPickupUnlocked() const;
    void enterAbilityPickup();
    RoomGate restTransition_{};
    void bindSceneShade();
    void placeBenchRespawn();
    void updateResting(const Input&);
    void requestRestTransition(const char* scene,const char* entry);
    const LightingConfig* lightingConfig_=nullptr;
    std::uint32_t lightDamageSerial_=0;
    void updateLighting();
    const RoomConfig* rules_=nullptr;
    TerrainBank* terrain_=nullptr;
    const AnimationBank* heroBank_=nullptr,*crawlerBank_=nullptr,*buzzerBank_=nullptr;
    const NailBank* nails_=nullptr;
    const RoomEntityBank* entities_=nullptr;
    CrawlerConfig crawlerConfig_;
    EnemyRandom random_;
    float viewportAspect_=400.0f/240.0f;
    bool visitedDirtmouth_=false;
    WaterwaysArtAssets waterwaysArt_;
    CityLiftArtAssets cityLiftArt_;
    unsigned cityLeverAttack_=0;
    void cityLiftPhysics(float seconds);
    const AnimationBank* bossBank_=nullptr;
    std::uint32_t bossFlags_=0;
    bool activateCrawler(unsigned index,double audioSeconds);
    void activateContacts();
    static void activationPhysics(void* context);
    static void presentationPhysics(void* context);
    static void greenpathPhysics(void* context);
    static void decorHit(void*,unsigned,const NailHit&);
    static void plantHit(void*,unsigned,const NailHit&);
    float plantSeconds_=0;
    double levelSeconds_=0;
    const GreatDoorConfig* greatDoorConfig_=nullptr;
    const GeoConfig* geoConfig_=nullptr;
    EnemyRewardQueue<EnemyCEncounter::capacity> enemyCRewards_;
    std::uint32_t enemyLifeRevision_=0;
    void syncEnemyLifeSave();
    const DecorBank* decorBank_=nullptr;
    const PlantBank* plantBank_=nullptr;
    const ShadeConfig* shadeConfig_=nullptr;
    const AnimationBank* shadeBank_=nullptr;
    bool shadePresent_=false,deathRecorded_=false;
    unsigned shadeGeo_=0,shadeHealth_=0;
    Vec2 shadeSpawn_{};
    RoomGate greatDoorExit_;
    DoorConfig floorConfig_;
    unsigned floorIndex_=0;
    Rect floorRange_{};
    float floorWait_=0;
    std::array<float,16> doorFade_{};
    // Negative time means the player has not entered a reveal trigger yet.
    std::array<float,16> revealFade_{};
    enum class RemaskState { Pause, Idle, FadeOut, FadeIn };
    struct RemaskFade
    {
        float alpha=0,start=0,elapsed=0,duration=0,target=0;
        double pause=0;
        bool inside=false;
        bool enabled=true;
        RemaskState state=RemaskState::FadeIn;
    };
    std::array<RemaskFade,RoomConfig::maxRemasks> remaskFade_{};
    void resetRemasks();
    void updateRemasks();
    std::array<float,2> secretActivate_{};
    std::array<bool,2> secretReload_{};
    std::array<bool,16> crawlerRecorded_{};
    std::array<std::uint16_t,128> messageContacts_{};
    std::array<bool,RoomEntityBank::maxBuzzers> buzzerRecorded_{};
    static void floorHit(void*,const NailHit&,HitOutcome);
    static void combatTrace(void*,const CombatTrace&);
    double accumulator_=0;
    bool testFrameCaptureActive_=false,testHeroPinned_=false;
    unsigned objectCount_=0;
    unsigned abilityFlags_=0;
    void applyAbilities();
    void updateAbilityPickup(float dt,const Input& input);
    void enterDreamNail();
    void updateDreamNail(float dt,const Input& input);
    float dreamPlatformAlpha(unsigned owner) const;
    void captureDreamPlatformAlpha(unsigned source,std::vector<VisualAlphaInput>& output) const;
    float dreamPlatformAge_=-1;
    bool dreamPlatformsRepeat_=false;
    void updateWaterways(float dt,const Input& input);
    void updateAreaTitle(float seconds);
    bool waterwaysHides(unsigned owner) const;
    bool waterwaysUpHeld_=false;
    float waterwaysPumpDelay_=-1;
};
}
