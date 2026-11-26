#pragma once
#include "uumuu_room.hpp"
#include "monomon_room.hpp"
#include "tutorial.hpp"
#include <utility>

namespace hk3ds {
struct CrawlerRenderState {
    AnimationPlayer animation;
    float scaleX=1,corpseScaleX=1;
    void capture(const Crawler& source,PresentationTime time){
        animation=source.animation;scaleX=source.artScale();corpseScaleX=source.corpseScaleX;
        alive_=source.alive();visible_=source.visible();bounds_=source.bounds();pose_=source.renderPosition(time);
    }
    bool alive() const{return alive_;}
    bool visible() const{return visible_;}
    Rect bounds() const{return bounds_;}
    Vec2 renderPosition(PresentationTime) const{return pose_;}
private:
    bool alive_=false,visible_=false;
    Rect bounds_{};
    Vec2 pose_{};
};
struct BuzzerRenderState {
    AnimationPlayer animation;
    Vec2 position{};
    float scaleX=1;
    void capture(const Buzzer& source,PresentationTime time){
        animation=source.animation;position=source.position;scaleX=source.artScale();
        alive_=source.alive();bounds_=source.bounds();pose_=source.renderPosition(time);
    }
    bool alive() const{return alive_;}
    Rect bounds() const{return bounds_;}
    Vec2 renderPosition(PresentationTime) const{return pose_;}
private:
    bool alive_=false;
    Rect bounds_{};
    Vec2 pose_{};
};
struct HeroParticleRenderState {
    const Clip* clip=nullptr;
    Vec2 position{};
    float size=1,opacity=1,angle=0;
};
struct BossEffectDraw {
    Vec2 position{};
    Vec2 scale{};
    float opacity=0,angle=0,progress=0;
    unsigned emitter=0;
};
struct BossWaveSpurtDraw {
    Vec2 position{};
    float elapsed=0;
    unsigned phase=0;
};
// Keep collision data out of both pipeline slots; retain the bounded authored
// wave sprites because they are presentation state, not debug geometry.
struct BossProjectileDraw {
    Vec2 position{};
    AnimationPlayer animation;
    float life=0,barrelScale=1,barrelAngle=0;
    bool ground=false;
    std::array<BossWaveSpurtDraw,8> waveSpurts{};
};
struct BossRenderState {
    CrossroadsBossKind kind=CrossroadsBossKind::None;
    BossPhase phase=BossPhase::Dormant;
    Rect arena{};
    Vec2 position{},headPosition{};
    AnimationPlayer animation,headAnimation;
    float scaleX=1,headScaleX=1,hitFlash=0,titleAlpha=0;
    bool visible=false,locked=false,collapsed=false,showHead=false;
    std::array<BossMinion,CrossroadsBoss::minionCapacity> minions{};
    std::array<BossProjectileDraw,CrossroadsBoss::projectileCapacity> projectiles{};
    std::vector<BossEffectDraw> effects;
    void capture(const CrossroadsBoss& source,PresentationTime time){
        kind=source.kind;phase=source.phase;arena=source.arena();position=source.renderPosition(time);
        headPosition=source.headPosition(position);showHead=source.headVisible();
        titleAlpha=source.titleAlpha();
        animation=source.animation;headAnimation=source.headAnimation;scaleX=source.scaleX;hitFlash=source.hitFlash;
        if(source.active()){
            scaleX=source.artScale();headScaleX=source.artScale(true);
        }
        visible=source.visible();locked=source.locked();collapsed=source.floorCollapsed();
        minions=source.minions;
        for(unsigned i=0;i<minions.size();++i)minions[i].position=source.minions[i].renderPosition(time);
        for(unsigned i=0;i<projectiles.size();++i){
            const auto& p=source.projectiles[i];
            auto& draw=projectiles[i];
            draw={p.renderPosition(time),p.animation,p.life,p.barrelScale,p.barrelAngle,p.ground};
            for(unsigned spurt=0;spurt<draw.waveSpurts.size();++spurt)
                draw.waveSpurts[spurt]={p.waveSpurts[spurt].position,p.waveSpurts[spurt].elapsed,p.waveSpurts[spurt].phase};
        }
        effects.clear();
        if(!source.effectParticles().empty())effects.reserve(CrossroadsBoss::effectCapacity);
        for(const auto& p:source.effectParticles())if(p.life>0){
            const auto draw=source.effectBillboard(p);const auto position=p.renderPosition(time);
            effects.push_back({{position.x+draw.offset.x,position.y+draw.offset.y},draw.scale,
                source.effectOpacity(p),draw.rotation,p.age/p.life,p.emitter});
        }
    }
    bool active()const{return kind!=CrossroadsBossKind::None;}
};
struct RoomRenderFlow {
    RoomPhase phase=RoomPhase::Waking;
    Vec2 camera{};
    AnimationPlayer presentation;
    float fade=0;
    RoomGate prompt{};
    DoorPromptPresentation doorPromptPresentation;
    bool promptVisible=false;
    float transitionAlpha=0;
    float deathFade() const{return fade;}
    const RoomGate* doorPrompt() const{return promptVisible?&prompt:nullptr;}
    float transitionFade() const{return transitionAlpha;}
};
struct DeathRenderState {
    Vec2 origin{};
    float flashRotation=0;
    bool facingRight=false;
    AnimationPlayer body,flash,flashRays;
    std::vector<HeroParticleRenderState> draws;
    std::array<float,2> opacity{},scale{};
    bool isActive=false,isBodyVisible=false;
    bool active() const{return isActive;}
    bool bodyVisible() const{return isBodyVisible;}
    float flashOpacity(bool rays=false) const{return opacity[rays];}
    float flashScale(bool rays=false) const{return scale[rays];}
    const auto& particles() const{return draws;}
    void capture(const HeroDeathPresentation&,PresentationTime);
};
struct ScuttlerRenderFlash {
    std::array<float,2> strength{};
    float screenFlashStrength(unsigned i=0) const{return strength[i];}
};

// Render-only copies: never advance/bind these components. Their animation
// pointers refer only to immutable banks that outlive both snapshot slots.
// Combat actors are evaluated above, avoiding their live CombatObject aliases.
struct TutorialRenderState {
    Simulation simulation;
    RoomRenderFlow flow;
    DeathRenderState death;
    std::vector<HeroParticleRenderState> heroParticles;
    std::vector<HeroParticleRenderState> focusParticles;
    std::array<FocusEffectDraw,FocusEffects::maxSprites> focusDraws;
    HeroDamageFlash damageFlash;
    GreatDoor greatDoor;
    std::vector<GeoVisualDraw> geoDraws;
    CollectibleProgress inventory{};
    ScuttlerRenderFlash healthScuttlers;
    FuryChest furyChest;
    CharmInventory charmInventory;
    CharmsPage charmsPage;
    bool charmsEditable=false;
    unsigned charmGeo=0;
    ConditionalEvents conditionalEvents;
    Cartographer cartographer;
    SlyShop slyShop;
    ArrowPrompt arrowPrompt;
    TollMenu tollMenu;
    AbilityPickup abilityPickup;
    HollowShade shade;
    Resting resting;
    NailInteractions nailInteractions;
    AreaTitle areaTitle;
    OpeningCredits credits;
    DarknessState darkness;
    float heroLightScale=3,focusFlash=0;
    TutorialPrompts prompts;
    LoreInspection inspection;
    WorldQuestDialogue worldQuestDialogue;
    TollArtState tollArt;
    AnimationPlayer worldQuestAnimation;
    Vec2 worldQuestPosition{},worldQuestScale{1,1};
    bool worldQuestVisible=false;
    float worldQuestTitleAlpha=0;
    std::array<CrawlerRenderState,16> crawlers;
    std::array<BuzzerRenderState,RoomEntityBank::maxBuzzers> buzzers;
    EnemyCRenderState enemyC;
    SceneEnemyRenderState sceneEnemies;
    UumuuRoomDraw uumuu;
    MonomonRoomDraw monomon;
    bool sceneActive=false;
    char sceneName[32]="Tutorial_01";
    GreenpathRenderState greenpath;
    BossRenderState boss;
    unsigned crawlerCount=0,buzzerCount=0;
    RoomVisualBank roomVisuals,decorVisuals,ambientVisuals,worldSwitchVisuals;
    Vec2 decorShake{};
    bool testIsolateDecor=false;
    void initialize(const TutorialSession&,const RoomVisualBank*);
    void capture(const TutorialSession&,const RoomVisualBank*,bool cullRoom=false);
    PresentationTime presentationTime() const{return time_;}
    Vec2 heroRenderPosition() const{return heroPose_;}
    float levelSeconds() const{return seconds_;}
    float visualAlpha(unsigned owner) const;
    std::size_t memoryBytes() const;
    // Each published slot owns its geometry; hidden diagnostics allocate nothing.
    static constexpr unsigned diagnosticCapacity=2*(3*CombatScene::capacity+EnemyCEncounter::capacity)+16+RoomEntityBank::maxBuzzers;
    std::vector<Rect> diagnosticBoxes;
    unsigned diagnosticBoxCount=0;
    void captureDiagnosticBounds(const TutorialSession&);
    void captureWorld(const World&);
    World world() const;
private:
    void captureAlpha(const TutorialSession&);
    PresentationTime time_{};
    Vec2 heroPose_{};
    float seconds_=0;
    std::vector<std::pair<unsigned,float>> alpha_;
    std::vector<Solid> solids_;
    std::vector<TerrainShape> shapes_;
    std::vector<Vec2> vertices_;
};
}
