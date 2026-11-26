#include "tutorial_render_state.hpp"
#include "graphics_profile.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace hk3ds {
namespace {
std::size_t bankBytes(const RoomVisualBank& bank){
    return (bank.scaleCurve?sizeof(RoomScaleAnimation)+bank.scaleCurve->keys.capacity()*sizeof(RoomScaleKey)+
        bank.scaleCurve->owners.capacity()*sizeof(std::uint32_t):0)+bank.draws.capacity()*sizeof(RoomDraw)+bank.cameraBounds.capacity()*sizeof(RoomCameraBounds)+
        bank.atlas.sprites.capacity()*sizeof(SpriteFrame)+bank.atlas.clips.capacity()*sizeof(Clip)+
        bank.atlas.frames.capacity()*sizeof(std::uint32_t);
}
void copyDraws(RoomVisualBank& target,const RoomVisualBank& source,const Vec2* camera,
               const RoomBreakables* breakables=nullptr,const TownMineLift* lift=nullptr,PresentationTime time={},const JijiDoorRuntime* jiji=nullptr,const CityLiftLowering* city=nullptr){
    target.scaleCurve=source.scaleCurve;
    if((!lift||!lift->bound())&&(!jiji||!jiji->bound())&&(!city||!city->bound())&&(!camera||source.cameraBounds.size()!=source.draws.size())){
        target.draws=source.draws;target.cameraBounds=source.cameraBounds;return;
    }
    target.draws.clear();target.cameraBounds.clear();
    for(std::size_t i=0;i<source.draws.size();++i){
        RoomDraw draw=source.draws[i];
        if(lift)lift->presentDraw(draw,time);
        if(jiji)jiji->presentDraw(draw,time);
        if(city)city->presentDraw(draw,time);
        const bool moved=(lift&&lift->moves(draw.owner))||(jiji&&jiji->bound())||(city&&city->moves(draw.owner));
        const auto bounds=moved||source.cameraBounds.size()!=source.draws.size()
            ?roomCameraBounds(source.atlas.sprites[draw.sprite],draw):source.cameraBounds[i];
        if(!camera||bounds.contains(camera->x,camera->y)||(breakables&&breakables->isDebris(draw.owner))){
            target.draws.push_back(draw);target.cameraBounds.push_back(bounds);
        }
    }
}

template<class T> void clearRenderCopy(T& target){
    // Large event snapshots must not create temporaries on the ARM stack.
    static const T empty{};
    target=empty;
}
}
void TutorialRenderState::initialize(const TutorialSession& source,const RoomVisualBank* room){
    if(room)roomVisuals=*room;
    decorVisuals=source.decorEffects.visuals;
    ambientVisuals=source.ambientParticles.visuals;
    worldSwitchVisuals=source.worldActorBank();
    worldSwitchVisuals.atlas.sprites.reserve(3);
    const auto worldDrawCapacity=source.cityCrestDust.ready()?CityCrestDust::drawCapacity:source.shamanPillar.bound()?ShamanPillarDust::drawCapacity:source.mantisGates.bound()?MantisGates::drawCapacity:source.stagLever.bound()?4+StagBellDust::capacity:3;
    worldSwitchVisuals.draws.reserve(worldDrawCapacity);
    worldSwitchVisuals.cameraBounds.reserve(worldDrawCapacity);
    // Copy live records only. Mirroring spare simulation-pool capacity in
    // every snapshot can exhaust the pipeline budget before startup.
    alpha_.reserve(512);
    heroParticles.reserve(HeroEffects::capacity);
    focusParticles.reserve(FocusEffects::capacity);
    death.draws.reserve(HeroDeathPresentation::capacity);
}
void TutorialRenderState::captureDiagnosticBounds(const TutorialSession& s){
    std::array<Rect,diagnosticCapacity> boxes;
    diagnosticBoxCount=0;
    const auto append=[&](const CombatScene& combat){
        diagnosticBoxCount+=combat.diagnosticBounds(boxes.data()+diagnosticBoxCount,
            boxes.size()-diagnosticBoxCount);
    };
    append(s.combat);append(s.sceneEnemies.combat());append(s.greenpath.combat());
    diagnosticBoxCount+=copyDiagnosticBounds(s.enemyC.objects.data(),s.enemyC.objects.size(),
        boxes.data()+diagnosticBoxCount,boxes.size()-diagnosticBoxCount);
    const auto body=[&](Rect b){if(diagnosticBoxCount<boxes.size())boxes[diagnosticBoxCount++]=b;};
    for(unsigned i=0;i<s.crawlerCount;++i)if(s.crawlers[i].alive())body(s.crawlers[i].bounds());
    for(unsigned i=0;i<s.buzzerCount;++i)if(s.buzzers[i].alive())body(s.buzzers[i].bounds());
    diagnosticBoxes.assign(boxes.data(),boxes.data()+diagnosticBoxCount);
}
void TutorialRenderState::capture(const TutorialSession& s,const RoomVisualBank* room,bool cullRoom){
    std::snprintf(sceneName,sizeof(sceneName),"%s",s.sceneName());
    inventory=s.collectibles.saved();
    slyShop=s.slyShop;
    arrowPrompt=s.arrowPrompt;
    tollMenu=s.cityGate.menu.open?s.cityGate.menu:s.routeTolls.menu;tollArt=s.routeTolls.art;
    for(unsigned i=0;i<tollArt.count;++i)tollArt.poses[i].scale.x=s.routeTolls.artScale(i);
    time_=s.presentationTime();heroPose_=s.heroRenderPosition();seconds_=s.levelSeconds();
    simulation=s.simulation;flow.phase=s.flow.phase;flow.camera=s.cameraRenderPosition();
    const auto* prompt=s.flow.doorPrompt();
    flow.promptVisible=prompt!=nullptr;flow.prompt=prompt?*prompt:RoomGate{};
    flow.doorPromptPresentation=s.doorPromptPresentation;
    flow.doorPromptPresentation.target=nullptr;
    flow.transitionAlpha=std::max(s.flow.transitionFade(),s.cityTransport.fade());
    sceneActive=s.sceneActive();
    resting=s.resting;
    charmInventory=s.charmInventory;charmsPage=s.charmsPage;charmsEditable=s.canEquipCharms();charmGeo=s.geo;
    abilityPickup=s.abilityPickup;
    worldQuestAnimation=s.worldQuests.animation();worldQuestPosition=s.worldQuests.definition().position;
    worldQuestScale=s.worldQuests.definition().scale;worldQuestVisible=s.worldQuests.present();
    if(worldQuestVisible)worldQuestScale.x=s.worldQuests.artScale();
    if(s.slyShopVisible()){
        worldQuestAnimation=s.slyShopAnimation;worldQuestPosition={slyShopX,slyShopY};
        worldQuestScale={s.slyShop.artScale(),1};worldQuestVisible=true;
    }
    worldQuestTitleAlpha=s.worldQuests.titleAlpha();
    cartographer=s.cartographer;
    heroLightScale=s.heroLight.scale();
    // Switch animation rewrites these three sprite slots. Copy them as well as
    // the draws so an in-flight render never observes the next simulation pose.
    const auto& switchBank=s.worldActorBank();
    worldSwitchVisuals.atlas.pageCount=switchBank.atlas.pageCount;
    worldSwitchVisuals.atlas.sprites=switchBank.atlas.sprites;
    copyDraws(worldSwitchVisuals,switchBank,cullRoom?&flow.camera:nullptr,nullptr,&s.townLift,time_,nullptr,&s.cityLift);
    focusFlash=s.focusEffects.healFlash();
    focusDraws=s.focusEffects.draws();focusParticles.clear();
    // Layer 13: White Flash R order 5 precedes Heal Anim order 22; ring is layer 15.
    std::rotate(focusDraws.begin(),focusDraws.end()-1,focusDraws.end());
    for(auto& draw:focusDraws)if(draw.active&&draw.followHero){
        draw.position.x+=heroPose_.x-s.simulation.hero.position.x;
        draw.position.y+=heroPose_.y-s.simulation.hero.position.y;
    }
    if(s.focusEffects.activeCount())for(const auto& particle:s.focusEffects.particles())if(particle.active)
        focusParticles.push_back({particle.clip,particle.history.sample(particle.position,time_),particle.size,FocusEffects::opacity(particle),0});
    const Vec2* admission=cullRoom?&flow.camera:nullptr;
    if(graphicsProfile().ambientParticles&&s.ambientEnabled())copyDraws(ambientVisuals,s.ambientParticles.visuals,admission);
    else {ambientVisuals.draws.clear();ambientVisuals.cameraBounds.clear();}
    heroParticles.clear();damageFlash=s.heroEffects.damageFlash();
    for(const auto& item:s.heroEffects.particles())if(item.active)
        heroParticles.push_back({item.clip,item.renderPosition(time_),s.heroEffects.drawSize(item),s.heroEffects.opacity(item),item.angle});
    if(s.sceneActive()){
        greenpath.capture(s.greenpath,time_);
        flow.presentation={};flow.fade=0;death.isActive=death.isBodyVisible=false;geoDraws.clear();
        for(unsigned i=0;i<GeoBank::capacity;++i)if(s.coins.coins()[i].active){
            const auto& draw=s.geoVisuals.coin(i);
            if(draw.visible){geoDraws.push_back(draw);geoDraws.back().position=s.coins.coins()[i].renderPosition(time_);}
        }
        clearRenderCopy(greatDoor);clearRenderCopy(furyChest);clearRenderCopy(conditionalEvents);
        shade=s.shade;clearRenderCopy(nailInteractions);boss.capture(s.crossroadsBoss,time_);
        if(boss.phase==BossPhase::Dormant)boss.locked=false;
        s.uumuu.capture(uumuu,time_);s.monomon.capture(monomon);s.sceneEnemies.capture(sceneEnemies,time_);enemyC=sceneEnemies.c;
        areaTitle=s.areaTitle;clearRenderCopy(credits);darkness=s.darkness;clearRenderCopy(prompts);clearRenderCopy(inspection);
        worldQuestDialogue=s.worldQuestDialogue;
        crawlerCount=buzzerCount=0;clearRenderCopy(healthScuttlers);decorShake=s.decorEffects.shake();
        if(room){
            // Reserve particle presentation with the room, before a break can emit.
            roomVisuals.draws.reserve(room->draws.size()+s.decorEffects.roomDraws().capacity());
            copyDraws(roomVisuals,*room,cullRoom?&flow.camera:nullptr,&s.roomBreakables,&s.townLift,time_,&s.jijiDoor,&s.cityLift);
        }
        for(auto& draw:roomVisuals.draws){
            s.roomBreakables.transform(draw,time_);
            s.monomon.present(draw);s.waterways.presentDraw(draw);s.cityCrest.presentDraw(draw,time_);
            if(s.collectibles.hides(draw.owner)||s.worldQuests.hides(draw.owner))draw.values[10]=0;
            for(const auto& replacement:s.worldActorBank().draws)
                if(draw.owner==replacement.owner){draw.values[10]=0;break;}
        }
        roomVisuals.draws.insert(roomVisuals.draws.end(),s.decorEffects.roomDraws().begin(),s.decorEffects.roomDraws().end());
        copyDraws(decorVisuals,s.decorEffects.visuals,cullRoom?&flow.camera:nullptr);
        captureAlpha(s);
        return;
    }
    flow.presentation=s.flow.presentation;flow.fade=s.flow.deathFade();death.capture(s.death,time_);greatDoor=s.greatDoor;
    sceneEnemies.count=0;uumuu={};monomon={};
    geoDraws.clear();
    const auto geo=[&](const GeoVisualDraw& draw,Vec2 position){if(draw.visible){geoDraws.push_back(draw);geoDraws.back().position=position;}};
    for(unsigned i=0;i<s.geoVisuals.rockCount();++i){const auto& draw=s.geoVisuals.rock(i).draw;
        if(!s.collectibles.hides(s.geoVisuals.rockOwner(i)))geo(draw,draw.position);}
    for(unsigned i=0;i<GeoBank::capacity;++i)if(s.coins.coins()[i].active)geo(s.geoVisuals.coin(i),s.coins.coins()[i].renderPosition(time_));
    for(const auto& chip:s.geoVisuals.chips())if(chip.draw.visible)geo(chip.draw,chip.renderPosition(time_));
    geo(s.healthCocoon.visual(),s.healthCocoon.visual().position);geo(s.healthCocoon.splatVisual(),s.healthCocoon.splatVisual().position);
    for(const auto& draw:s.healthCocoonFx.draws(time_))geo(draw,draw.position);
    for(unsigned i=0;i<HealthScuttlers::capacity;++i){
        const auto& actor=s.healthScuttlers.actors()[i];geo(actor.draw,actor.renderPosition(time_));
        for(const auto& draw:s.healthScuttlers.effects(i).draws(time_))geo(draw,draw.position);
        healthScuttlers.strength[i]=s.healthScuttlers.screenFlashStrength(i);
    }
    for(const auto& draw:s.healthScuttlers.draws(time_))geo(draw,draw.position);
    furyChest=s.furyChest;conditionalEvents=s.conditionalEvents;
    shade=s.shade;nailInteractions=s.nailInteractions;areaTitle=s.areaTitle;credits=s.credits;darkness=s.darkness;
    prompts=s.prompts;inspection=s.inspection;worldQuestDialogue=s.worldQuestDialogue;
    crawlerCount=s.crawlerCount;buzzerCount=s.buzzerCount;testIsolateDecor=s.testIsolateDecor;
    enemyC.capture(s.enemyC,time_);greenpath.capture(s.greenpath,time_);
    for(unsigned i=0;i<crawlerCount;++i)crawlers[i].capture(s.crawlers[i],time_);
    for(unsigned i=0;i<buzzerCount;++i)buzzers[i].capture(s.buzzers[i],time_);
    boss.capture(s.crossroadsBoss,time_);
    if(boss.active()){crawlerCount=0;buzzerCount=0;}
    decorShake=s.decorEffects.shake();
    Vec2 camera=flow.camera;
    const auto chestShake=furyChest.shake();
    camera.x+=decorShake.x+chestShake.x;camera.y+=decorShake.y+chestShake.y;
    admission=cullRoom?&camera:nullptr;
    if(room)copyDraws(roomVisuals,*room,admission);
    copyDraws(decorVisuals,s.decorEffects.visuals,admission);
    captureAlpha(s);
}
void TutorialRenderState::captureAlpha(const TutorialSession& s){
    alpha_.clear();
    // Only non-identity visibility needs storage. Preserve lookup semantics for
    // duplicated source owners across front/background and effect draw banks.
    unsigned lastOwner=0;float lastAlpha=1;bool cached=false;
    for(const auto* bank:{&roomVisuals,&decorVisuals,&ambientVisuals})for(const auto& draw:bank->draws){
        // Particle runs commonly share an owner. Source state stays immutable
        // throughout this capture, so repeated visibility queries are identical.
        if(!cached||lastOwner!=draw.owner){lastOwner=draw.owner;lastAlpha=s.visualAlpha(draw.owner);cached=true;}
        const float alpha=lastAlpha;
        if(alpha!=1)alpha_.emplace_back(draw.owner,alpha);
    }
    // The interactive shiny is presented by AbilityPickup; keep the corpse.
    if(s.sceneActive()&&abilityPickup.kind==AbilityPickup::Kind::Cloak)alpha_.emplace_back(522,0);
    std::sort(alpha_.begin(),alpha_.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    alpha_.erase(std::unique(alpha_.begin(),alpha_.end(),[](const auto& a,const auto& b){return a.first==b.first;}),alpha_.end());
}
float TutorialRenderState::visualAlpha(unsigned owner) const{
    const auto it=std::lower_bound(alpha_.begin(),alpha_.end(),owner,[](const auto& item,unsigned key){return item.first<key;});
    return it!=alpha_.end()&&it->first==owner?it->second:1;
}
std::size_t TutorialRenderState::memoryBytes() const{
    return sizeof(*this)+bankBytes(roomVisuals)+bankBytes(decorVisuals)+bankBytes(ambientVisuals)+bankBytes(worldSwitchVisuals)+alpha_.capacity()*sizeof(alpha_[0])+
        solids_.capacity()*sizeof(Solid)+shapes_.capacity()*sizeof(TerrainShape)+vertices_.capacity()*sizeof(Vec2)+
        diagnosticBoxes.capacity()*sizeof(Rect)+
        (heroParticles.capacity()+focusParticles.capacity()+death.draws.capacity())*sizeof(HeroParticleRenderState)+geoDraws.capacity()*sizeof(GeoVisualDraw)+
        flow.doorPromptPresentation.fading.capacity()*sizeof(DoorPromptPresentation::Fading);
}
void DeathRenderState::capture(const HeroDeathPresentation& source,PresentationTime time){
    origin=source.origin;flashRotation=source.flashRotation;facingRight=source.facingRight;
    body=source.body;flash=source.flash;flashRays=source.flashRays;
    isActive=source.active();isBodyVisible=source.bodyVisible();
    for(unsigned i=0;i<2;++i){opacity[i]=source.flashOpacity(i);scale[i]=source.flashScale(i);}
    draws.clear();
    for(const auto& item:source.particles())if(item.active)
        draws.push_back({item.clip,item.renderPosition(time),item.drawSize(),1,item.angle});
}
void TutorialRenderState::captureWorld(const World& source){
    if(source.count)solids_.assign(source.solids,source.solids+source.count);else solids_.clear();
    if(source.shapeCount)shapes_.assign(source.shapes,source.shapes+source.shapeCount);else shapes_.clear();
    std::size_t vertices=0;
    for(const auto& shape:shapes_)vertices=std::max<std::size_t>(vertices,shape.first+shape.count);
    if(vertices)vertices_.assign(source.vertices,source.vertices+vertices);else vertices_.clear();
}
World TutorialRenderState::world() const{
    World result;
    result.solids=solids_.data();result.count=solids_.size();
    result.shapes=shapes_.data();result.shapeCount=shapes_.size();result.vertices=vertices_.data();
    return result;
}
}
