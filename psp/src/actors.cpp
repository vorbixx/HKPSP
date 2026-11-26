#include "actors.hpp"
#include "../include/platform.hpp"
#include <stdexcept>

namespace hkpsp {
namespace {
void checked(bool ok,const char* table){if(!ok)throw std::runtime_error(std::string("Actor binding failed: ")+table);}
void draw(DrawList& list,const ActorAtlas& atlas,const hk3ds::AnimationPlayer& player,hk3ds::Vec2 camera,
          hk3ds::Vec2 at,hk3ds::Vec2 scale={1,1},float depth=.004f,float alpha=1,float rotation=0,
          hk3ds::RoomMaterial material=hk3ds::RoomMaterial::Normal,float flash=0,float tint=1){
    ActorPose pose;pose.position=at;pose.scale=scale;pose.depth=depth;pose.alpha=alpha;
    pose.rotation=rotation;pose.material=material;pose.flash=flash;pose.tint={tint,tint,tint};
    appendActor(list,atlas.bank,player,pose,camera,atlas.page);
}
void geoDraw(DrawList& list,const ActorAtlas& atlas,const hk3ds::GeoVisualDraw& d,hk3ds::Vec2 camera,
             hk3ds::Vec2 position,float alpha=1,float rotation=0){
    if(d.visible)draw(list,atlas,d.player,camera,position,d.scale,d.depth,alpha,rotation+d.rotation);
}
}
void ActorArt::load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages){
    const auto atlas=[&](ActorAtlas& a,const char* table,const char* prefix){
        a=pages.load(platform,textures,table,prefix);
    };
    atlas(crawler,"crawler-anim.bin","crawler");atlas(buzzer,"buzzer-anim.bin","buzzer");
    atlas(shade,"shade-anim.bin","shade");atlas(geo,"geo-visual.bin","geo");
    atlas(door,"great-door-visual.bin","great-door");atlas(fury,"fury-visual.bin","fury");
    atlas(events,"events-anim.bin","events");
    atlas(decor,"decor-fx-visual.bin","decor-fx");
}
void ActorArt::bindTerrain(Platform& p,hk3ds::TutorialSession& g,hk3ds::TerrainBank& terrain){
    // main.cpp:1388-1391: install Fury terrain BEFORE TutorialSession stores World views.
    checked(g.furyChest.bind(fury.bank,p.path("fury-chest.bin").c_str()),"fury-chest.bin");
    checked(g.furyChest.bindTerrain(terrain),"Fury terrain");
    checked(g.nailInteractions.load(p.path("nail-interactions.bin").c_str(),fury.bank),"nail-interactions.bin");
}
void ActorArt::bindEffects(Platform& p,hk3ds::TutorialSession& g,const hk3ds::AnimationBank& hero,const hk3ds::GrassBank& grass){
    // main.cpp:1492-1527: same config bindings; original HKR metadata supplies
    // templates while the PSP atlas replaces only texel rectangles/pages.
    hk3ds::GeoRockConfig rocks;checked(rocks.load(p.path("geo-rocks.bin").c_str()),"geo-rocks.bin");
    checked(g.geoVisuals.bind(geo.bank,rocks),"Geo visuals");
    checked(g.healthCocoon.bind(geo.bank,p.path("health-cocoon.bin").c_str()),"health-cocoon.bin");
    checked(g.healthCocoonFx.bind(geo.bank,"Spatter Blue","Spawn","Cocoon Cap"),"Cocoon FX");
    checked(g.healthCocoonFx.configure(g.healthCocoon.fxConfig()),"Cocoon FX config");
    checked(g.healthScuttlers.bind(geo.bank),"Scuttlers");
    g.healthCocoonBreakEffect=[](void* raw,hk3ds::Vec2 origin,hk3ds::Strike direction,hk3ds::Vec2 nail){
        auto& room=*static_cast<hk3ds::TutorialSession*>(raw);
        room.healthCocoonFx.breakAt(origin,room.healthCocoon.visual().depth,nail);
        room.decorEffects.healthCocoonBreak(origin,direction);
    };g.healthCocoonEffectContext=&g;
    checked(g.decorEffects.load(p.path("decor-effects.bin").c_str(),p.path("decor-fx-source.bin").c_str()),"decor-effects.bin");
    g.decorEffects.visuals.atlas=decor.bank;g.decorEffects.bindGrass(grass);
    checked(g.contactEffects.load(p.path("contact-effects.bin").c_str(),g.decorEffects),"contact-effects.bin");
    checked(g.heroEffects.load(p.path("hero-effects.bin").c_str(),hero),"hero-effects.bin");
    checked(g.focusEffects.load(p.path("focus-effects.bin").c_str(),hero),"focus-effects.bin");
    checked(g.decorEffects.loadStalactites(p.path("stalactite-fx.bin").c_str()),"stalactite-fx.bin");
    checked(g.decorEffects.loadInitialFall(p.path("initial-fall-fx.bin").c_str()),"initial-fall-fx.bin");
    checked(g.decorEffects.loadOwners(p.path("owner-effects.bin").c_str()),"owner-effects.bin");
    checked(g.stalactiteVisuals.load(p.path("stalactite-visuals.bin").c_str(),decor.bank),"stalactite-visuals.bin");
}
void appendDoor(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    // main.cpp:462-480: authored door geometry, including parent pose, at z=.61. King's Pass only
    // (tutorial_render_state.cpp:130 clears the door in scenes).
    if(g.sceneActive()||!g.greatDoor.visible())return;
    hk3ds::AnimationPlayer a;a.play(art.door.bank.find(g.greatDoor.spriteName()));
    draw(list,art.door,a,camera,{0,0},{1,1},.61f);
}
void appendGeo(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    // main.cpp:481-505; tutorial_render_state.cpp:154-170: presented positions.
    const auto time=g.presentationTime();
    const auto geo=[&](const hk3ds::GeoVisualDraw& d,hk3ds::Vec2 p){geoDraw(list,art.geo,d,camera,p);};
    // tutorial_render_state.cpp:124-128: a scene draws only its Geo coins. Rocks, chips, the health cocoon and its
    // scuttlers are King's Pass objects: enterScene -> geoVisuals.reset() rebuilds the King's Pass rocks visible at
    // their King's Pass coordinates (user 2026-11-22: misplaced objects in Town and other rooms).
    if(g.sceneActive()){
        for(unsigned i=0;i<hk3ds::GeoBank::capacity;++i)if(g.coins.coins()[i].active)geo(g.geoVisuals.coin(i),g.coins.coins()[i].renderPosition(time));
        return;
    }
    for(unsigned i=0;i<g.geoVisuals.rockCount();++i)if(!g.collectibles.hides(g.geoVisuals.rockOwner(i))){const auto& d=g.geoVisuals.rock(i).draw;geo(d,d.position);}
    for(unsigned i=0;i<hk3ds::GeoBank::capacity;++i)if(g.coins.coins()[i].active)geo(g.geoVisuals.coin(i),g.coins.coins()[i].renderPosition(time));
    for(const auto& chip:g.geoVisuals.chips())if(chip.draw.visible)geo(chip.draw,chip.renderPosition(time));
    geo(g.healthCocoon.visual(),g.healthCocoon.visual().position);geo(g.healthCocoon.splatVisual(),g.healthCocoon.splatVisual().position);
    for(const auto& d:g.healthCocoonFx.draws(time))geo(d,d.position);
    for(unsigned i=0;i<hk3ds::HealthScuttlers::capacity;++i){const auto& a=g.healthScuttlers.actors()[i];geo(a.draw,a.renderPosition(time));
        for(const auto& d:g.healthScuttlers.effects(i).draws(time))geo(d,d.position);}
    for(const auto& d:g.healthScuttlers.draws(time))geo(d,d.position);
}
void appendFury(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera,bool front){
    const auto& pickup=g.abilityPickup;
    if(front&&pickup.kind==hk3ds::AbilityPickup::Kind::CityCrest&&pickup.visible()){
        hk3ds::AnimationPlayer shiny;shiny.play(art.fury.bank.require("Fury Shiny"));
        shiny.advance(pickup.seconds);
        draw(list,art.fury,shiny,camera,{pickup.position.x,pickup.position.y+.5f},{1.5f,1.5f},0);
    }
    if(g.sceneActive())return;
    // main.cpp:506-515,905-907. Prompts/tutorial UI deliberately omitted.
    const auto& chest=g.furyChest;
    const auto part=[&](hk3ds::FuryPart p,float alpha=1){const auto d=chest.draw(p,g.presentationTime());geoDraw(list,art.fury,d,camera,d.position,alpha);};
    if(front){part(hk3ds::FuryPart::Front);for(const auto& d:g.nailInteractions.tinks())geoDraw(list,art.fury,d,camera,d.position);return;}
    for(auto p:{hk3ds::FuryPart::Back,hk3ds::FuryPart::Chest,hk3ds::FuryPart::Glow,hk3ds::FuryPart::Shiny,hk3ds::FuryPart::Burst,hk3ds::FuryPart::Wave})
        part(p,p==hk3ds::FuryPart::Wave?chest.waveOpacity():1);
    for(const auto& p:chest.particles())geoDraw(list,art.fury,p.draw,camera,p.draw.position,chest.config().emitters[p.emitter].v[23]);
    const auto& fx=chest.effects();geoDraw(list,art.fury,fx[0],camera,fx[0].position);
    geoDraw(list,art.fury,fx[1],camera,fx[1].position,chest.flashOpacity());
    geoDraw(list,art.fury,fx[2],camera,fx[2].position,1,chest.tinkRotation()*.01745329252f);
}
void appendConditional(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    // Shared main.cpp conditional actor pass; all activation/animation is shared.
    if(g.sceneActive())return;
    const auto& e=g.conditionalEvents;const auto time=g.presentationTime();
    const auto actor=[&](const hk3ds::AnimationPlayer& a,hk3ds::Vec2 at,float scale=1.f){
        draw(list,art.events,a,camera,at,{scale,scale});
    };
    if(e.mushroomVisible())actor(e.mushroomAnimation,e.mushroomRenderPosition(time),e.mushroomArtScale());
    if(e.grimmkinVisible())actor(e.grimmkinAnimation,e.grimmkinRenderPosition(time),e.grimmkinArtScale());
    if(e.flameVisible())actor(e.flameAnimation,{146.49000046f,8.6700008f});
    for(const auto& p:e.projectiles)if(p.active)actor(p.animation,p.renderPosition(time),p.scale());
    for(const auto& p:e.particles)if(p.active)
        draw(list,art.events,p.animation,camera,p.renderPosition(time),{p.size,p.size},.004f,p.alpha());
    if(e.sourceActiveCount())for(const auto& p:e.sourceParticles())if(p.active){
        hk3ds::AnimationPlayer frame;frame.play(p.clip);
        if(p.clip)frame.frame=std::min<unsigned>(p.clip->count-1,unsigned(p.age/p.life*p.clip->count));
        const float size=e.sourceParticleSize(p);
        draw(list,art.events,frame,camera,p.renderPosition(time),{size,size},.004f,e.sourceParticleOpacity(p),p.angle);
    }
    if(e.flashAnimation.clip&&!e.flashAnimation.complete){
        const auto hero=g.heroRenderPosition();actor(e.flashAnimation,{hero.x,hero.y+1.5f});
    }
}
namespace {
void reportEnemyDraw(std::FILE* survey,const char* scene,unsigned owner,unsigned bank,bool bound,
                     std::size_t quads,hk3ds::Vec2 position,const char* path){
    if(std::fprintf(survey,"%s,%u,%u,%u,%zu,%.6g,%.6g,%s\n",scene,owner,bank,unsigned(bound),quads,
                    double(position.x),double(position.y),path)<0)
        throw std::runtime_error("Enemy survey report failed");
}
}
void appendCrawlers(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera,std::FILE* survey){
    // main.cpp:516-522. Shared simulation owns state->clip selection (enemy.cpp).
    for(unsigned slot=0;slot<g.crawlerCount;++slot){const auto& actor=g.crawlers[slot];if(actor.visible()){
        const auto position=actor.renderPosition(g.presentationTime());
        const auto first=survey?list.quads.size():0;
        draw(list,art.crawler,actor.animation,camera,position,{actor.alive()?actor.artScale():actor.corpseScaleX,1});
        if(survey)reportEnemyDraw(survey,g.sceneName(),slot,0,art.crawler.bank.pageCount!=0,
                                  list.quads.size()-first,position,"tutorial-crawler");
    }}
}
// Rooms-namespace logical names select the PSA1 render tables staged by tools/psp/room_stage.py.
// Sim bank 1..6 = crossroads registry (Tiktik, Gruzzer, Vengefly, Aspid), then Enemy B, then Enemy C.
namespace {
constexpr const char* sceneEnemyStems[SceneEnemyArt::slots]={"","tiktik","gruzzer","vengefly","aspid-hatchling","enemy-b","enemy-c","","elder-baldur","zombie-guard","spitter","fungus-flyer","jellyfish","fungoon-baby","fung-crawler"};
}
void SceneEnemyArt::clear(){
    for(auto& bank:banks)bank=ActorAtlas{};
    ready.fill(false);
}
void SceneEnemyArt::load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,const hk3ds::SceneEnemies& enemies){
    // Per scene: only the banks this scene's simulation binds, so the page pool and heap grow per room.
    clear();
    if(!state)state=std::make_unique<hk3ds::SceneEnemyRenderState>();
    for(unsigned i=1;i<slots;++i){
        // Slot 7 is the room's encounter bank: Broken Vessel, Watcher Knights or Mantis Lords (one per room).
        const bool encounter=i==7;
        if(encounter ? !(enemies.mantis.active()||enemies.brokenVessel.active()||enemies.watchers.active())
                     : !enemies.usesBank(i))continue;
        const std::string stem=encounter?enemies.encounterArtStem():sceneEnemyStems[i];
        const std::string table=stem+(i==7||i==8?"-art":"-anim");
        banks[i]=pages.load(platform,textures,"rooms/scene-enemy/"+table+".bin","rooms/scene-enemy/"+stem);
        ready[i]=true;
    }
}
void appendSceneEnemies(DrawList& list,SceneEnemyArt& art,const ActorAtlas& crawler,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera,std::FILE* survey){
    // Simulation state comes from the shared capture; only texel rectangles come from the PSA1 bank.
    if(!art.state)return;
    auto& state=*art.state;
    g.sceneEnemies.capture(state,g.presentationTime());
    const auto append=[&](unsigned bank,unsigned owner,const hk3ds::AnimationPlayer& animation,
                          hk3ds::Vec2 position,hk3ds::Vec2 scale,float depth,float rotation,float alpha,float flash){
        const bool bound=bank==0||(bank<SceneEnemyArt::slots&&art.ready[bank]);
        const auto first=survey?list.quads.size():0;
        if(bound&&animation.clip){
            const auto& atlas=bank==0?crawler:art.banks[bank];
            const auto* clip=atlas.bank.find(animation.clip->name);
            if(!clip)throw std::runtime_error(std::string("Scene enemy clip missing: ")+animation.clip->name);
            hk3ds::AnimationPlayer player=animation;player.clip=clip;
            draw(list,atlas,player,camera,position,scale,depth,alpha,rotation,hk3ds::RoomMaterial::Normal,flash);
        }
        if(survey)reportEnemyDraw(survey,g.sceneName(),owner,bank,bound,list.quads.size()-first,position,"scene");
    };
    for(unsigned i=0;i<state.count;++i){
        const auto& actor=state.actors[i];
        append(actor.bank,actor.owner,actor.animation,actor.position,{actor.scaleX,actor.scaleY},actor.depth,actor.rotation,1,0);
    }
    // Brooding Mawlek arms and head sit behind the body; the Dummy draws in front.
    const auto mawlekParts=[&](bool front){
        for(unsigned i=0;i<state.c.partCount;++i){const auto& part=state.c.parts[i];
            if(part.front==front)append(6,state.c.mawlekActor<state.cOwners.size()?state.cOwners[state.c.mawlekActor]:0,
                                        part.animation,part.position,{part.scaleX,part.scaleY},.004f,0,1,0);}
    };
    mawlekParts(false);
    for(unsigned i=0;i<state.c.actors.size();++i){
        const auto& actor=state.c.actors[i];
        if(actor.visible)append(6,state.cOwners[i],actor.animation,actor.position,{actor.scaleX,actor.scaleY},.004f,0,
                                actor.alpha,std::min(1.f,actor.flash/.12f));
    }
    mawlekParts(true);
    for(unsigned i=0;i<state.c.shotCount;++i){
        const auto& shot=state.c.shots[i];
        append(6,0,shot.animation,shot.position,{shot.scale,shot.scale},.004f,0,1,0);
    }
    // Mushroom Brawler Vomit Globs (enemy-c bank, glob/Idle and glob/Land).
    for(unsigned i=0;i<state.c.globCount;++i){
        const auto& glob=state.c.globs[i];
        append(6,0,glob.animation,glob.position,{glob.scale,glob.scale},.004f,0,1,0);
    }
}
void GreenpathArt::clear(hk3ds::GreenpathEncounter& encounter){
    encounter.bind(nullptr,nullptr);
    atlas={};
}
void GreenpathArt::load(Platform& platform,std::vector<Texture>& textures,AnimationPages& pages,
                        hk3ds::GreenpathEncounter& encounter,const hk3ds::NailBank& nails,
                        const std::string& metadata,std::uint32_t flags){
    const auto loadScene=[&]{checked(encounter.loadScene(metadata.c_str(),flags),"Greenpath scene");};
    loadScene();
    for(const auto& actor:encounter.actors)if(actor.visible()){
        atlas=pages.load(platform,textures,"rooms/scene-enemy/greenpath-art.bin","rooms/scene-enemy/greenpath");
        checked(encounter.bind(&atlas.bank,&nails),"Greenpath art and nails");
        loadScene();
        break;
    }
}
void appendGreenpath(DrawList& list,const GreenpathArt& art,const hk3ds::TutorialSession& game,hk3ds::Vec2 camera,std::FILE* survey){
    const auto& atlas=art.atlas;
    if(!atlas.bank.pageCount)return;
    hk3ds::GreenpathRenderState state;
    state.capture(game.greenpath,game.presentationTime());
    const auto effect=[&](const char* name,float age){
        hk3ds::AnimationPlayer player;player.play(atlas.bank.require(name));player.advance(age);return player;
    };
    if(state.sphereVisible){
        const float grow=.8f+.7f*std::min(1.f,state.sphereAge/.25f);
        draw(list,atlas,effect("Hornet/SphereBall",state.sphereAge),camera,state.spherePosition,{grow,grow});
        const auto flash=effect("Hornet/SphereFlash",state.sphereAge);
        if(flash.clip&&!flash.complete)
            draw(list,atlas,flash,camera,{state.spherePosition.x,state.spherePosition.y-.25f},{1.3849015f,1.05f});
    }
    for(unsigned slot=0;slot<state.actors.size();++slot){const auto& actor=state.actors[slot];if(actor.visible){
        const auto first=survey?list.quads.size():0;
        draw(list,atlas,actor.animation,camera,actor.position,{actor.facing,actor.scaleY},.004f,actor.alpha,actor.rotation);
        if(survey)reportEnemyDraw(survey,game.sceneName(),game.greenpath.owner(slot),0,true,
                                  list.quads.size()-first,actor.position,"greenpath");
    }}
    if(state.needleVisible)draw(list,atlas,effect("Needle/Idle",state.needleAge),camera,state.needle);
    for(unsigned i=0;i<state.ballCount;++i){
        const auto& ball=state.balls[i];
        draw(list,atlas,effect("MossKnight/Spit",0),camera,ball.position,{ball.scale,ball.scale},.004f,1,ball.rotation);
    }
}
void appendBuzzers(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera,std::FILE* survey){
    // Use the shared native actor presentation convention.
    for(unsigned slot=0;slot<g.buzzerCount;++slot){const auto& actor=g.buzzers[slot];if(actor.alive()){
        const auto position=actor.renderPosition(g.presentationTime());
        const auto first=survey?list.quads.size():0;
        draw(list,art.buzzer,actor.animation,camera,position,{actor.artScale(),1});
        if(survey)reportEnemyDraw(survey,g.sceneName(),slot,0,art.buzzer.bank.pageCount!=0,
                                  list.quads.size()-first,position,"tutorial-buzzer");
    }}
}
void appendShade(DrawList& list,const ActorArt& art,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    // main.cpp:578-628: body/corpse, local motes, recovery, hits, glow and slash.
    const auto& s=g.shade;if(!s.visible())return;
    const auto time=g.presentationTime();const auto p=s.renderPosition(time);const float facing=s.alive()?s.artScale():1;
    const auto named=[&](const char* name,hk3ds::Vec2 at,hk3ds::Vec2 scale,float alpha=1,float angle=0,float age=0){
        hk3ds::AnimationPlayer a;a.play(art.shade.bank.require(name));a.advance(age);draw(list,art.shade,a,camera,at,scale,.004f,alpha,angle);};
    for(const auto& mote:s.particles())if(mote.active()){const auto local=mote.renderPosition(time);named("Shade Particle",{p.x+local.x*facing,p.y+local.y},{mote.size(),mote.size()});}
    if(s.beamVisible())named("Recovery Beam",{p.x,p.y-.03f},{-1.2682843f,1.2682844f},.3019608f);
    if(s.state!=hk3ds::ShadeState::RecoveryDissipate){
        draw(list,art.shade,s.animation,camera,p,{facing,1},.004f,1,0,hk3ds::RoomMaterial::Normal,0,s.alive()?s.hitNeutral():1);
        if(s.deathSprite.clip)draw(list,art.shade,s.deathSprite,camera,p);
    }
    for(const auto& q:s.hitQuads())if(q.hitActive())named(q.hitClip(),q.renderPosition(time),{q.hitScaleX(),q.hitScaleY()},q.hitOpacity(),q.hitRotation(),q.age);
    if(s.glowAnimation.clip&&!s.glowAnimation.complete)draw(list,art.shade,s.glowAnimation,camera,p,{3,3},.004f,1,0,hk3ds::RoomMaterial::Additive);
    if(s.state==hk3ds::ShadeState::Slash||s.state==hk3ds::ShadeState::SlashEnd)
        draw(list,art.shade,s.slashAnimation,camera,{p.x-.04862976f*s.scaleX,p.y-.0116996765f},{1.2700316f*s.scaleX,1});
}
// main.cpp:540-555: the Crossroads_47 station stand, bell and smoke and the riding stag all use the stag bank.
// Stag positions are world coordinates; the riding stag is offset by the arrival path while it smokes or departs.
void appendStagStation(DrawList& list,const hk3ds::AnimationBank& bank,const hk3ds::TutorialSession& g,
                       hk3ds::PresentationTime time,hk3ds::Vec2 camera){
    if(!bank.pageCount)return;
    const auto& rest=g.resting;
    if(rest.station.present){
        const auto& c=rest.station;
        if(c.standScale.x!=0){ActorPose p;p.position=c.toll;p.scale=c.standScale;appendActor(list,bank,rest.standAnimation,p,camera,0);}
        if(c.bellScale.x!=0&&rest.stationOpen()&&rest.stagPhase!=hk3ds::StagPhase::Opening){
            ActorPose p;p.position=c.bell;p.scale=c.bellScale;appendActor(list,bank,rest.bellAnimation,p,camera,0);}
        if(rest.smokeVisible()){ActorPose p;p.position=c.dust;p.scale=c.scale;appendActor(list,bank,rest.smokeAnimation,p,camera,0);}
    }
    if(rest.stagVisible()){
        ActorPose p;p.position=rest.presentedStag(time);
        if(rest.smokeVisible()||rest.stagPhase==hk3ds::StagPhase::Departing){
            p.position.x+=rest.station.arrival.x-rest.station.position.x;
            p.position.y+=rest.station.arrival.y-rest.station.position.y;
        }
        p.scale=rest.station.scale;
        appendActor(list,bank,rest.stagAnimation,p,camera,0);
    }
}
void appendSlash(DrawList& list,const hk3ds::AnimationBank& bank,unsigned page,const hk3ds::TutorialSession& g,
                 const hk3ds::NailBank& nails,hk3ds::Vec2 position,hk3ds::Vec2 camera){
    // main.cpp:864 / renderer.cpp:123-134: transforms belong to each nail variant.
    const auto& h=g.simulation.hero;const auto& animator=g.simulation.animator;
    if(!nails.ready()||animator.slash.complete||!h.nail.active)return;
    const auto& shape=nails.shapes[animator.slashVariant];const float facing=h.facingRight?-1.f:1.f;
    ActorPose p;p.position={position.x+shape.offset.x*facing,position.y+shape.offset.y};
    p.scale={shape.scale.x*facing*h.nail.rangeScale,shape.scale.y*h.nail.rangeScale};
    appendActor(list,bank,animator.slash,p,camera,page);
}
void appendFocus(DrawList& list,const hk3ds::AnimationBank& bank,unsigned page,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera,hk3ds::FocusDrawPass pass){
    // main.cpp:813-833,904; snapshot rotates White Flash R before Heal Anim.
    const auto& draws=g.focusEffects.draws();const auto hero=g.heroRenderPosition();
    for(unsigned i=0;i<draws.size();++i){const auto& d=draws[(i+draws.size()-1)%draws.size()];if(!d.active||d.pass!=pass)continue;
        ActorPose p;p.position=d.position;if(d.followHero){p.position.x+=hero.x-g.simulation.hero.position.x;p.position.y+=hero.y-g.simulation.hero.position.y;}
        p.scale=d.scale;p.alpha=d.alpha;p.material=d.screenBlend?hk3ds::RoomMaterial::Screen:hk3ds::RoomMaterial::Normal;
        // convert_focus_effects.py:54-61 already premultiplies these texels.
        if(d.screenBlend)p.blend=Blend::ScreenPremult;
        appendActor(list,bank,d.animation,p,camera,page);
    }
}
void appendHeroEffects(DrawList& list,const hk3ds::AnimationBank& bank,unsigned page,const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    // main.cpp:829-864; death particle positions/sizes from the shared presenter.
    const auto particle=[&](const hk3ds::Clip* clip,hk3ds::Vec2 at,float size,float alpha,float angle){
        hk3ds::AnimationPlayer a;a.clip=clip;ActorPose p;p.position=at;p.scale={size,size};p.alpha=alpha;p.rotation=angle;appendActor(list,bank,a,p,camera,page);
    };
    const auto time=g.presentationTime();
    for(const auto& p:g.focusEffects.particles())if(p.active)particle(p.clip,p.history.sample(p.position,time),p.size,hk3ds::FocusEffects::opacity(p),0);
    for(const auto& p:g.heroEffects.particles())if(p.active)particle(p.clip,p.renderPosition(time),g.heroEffects.drawSize(p),g.heroEffects.opacity(p),p.angle);
    const auto& flash=g.heroEffects.damageFlash();if(flash.active){
        hk3ds::AnimationPlayer a;a.clip=flash.clip;ActorPose p;p.position=flash.position;p.scale=flash.scale;p.alpha=flash.alpha;p.rotation=flash.angle;appendActor(list,bank,a,p,camera,page);
    }
    const auto& death=g.death;if(!death.active())return;
    for(const auto& p:death.particles())if(p.active)particle(p.clip,p.renderPosition(time),p.drawSize(),1,p.angle);
    const float facing=death.facingRight?-1.f:1.f;
    if(death.flashOpacity()>0){
        ActorPose p;p.position={death.origin.x,death.origin.y-.5f};p.scale={death.flashScale()*facing,death.flashScale()};p.alpha=death.flashOpacity();appendActor(list,bank,death.flash,p,camera,page);
        p.position.x-=.21f*facing;p.scale={death.flashScale(true)*facing,death.flashScale(true)};p.alpha=death.flashOpacity(true);p.rotation=death.flashRotation;appendActor(list,bank,death.flashRays,p,camera,page);
    }
    if(death.bodyVisible()){ActorPose p;p.position=death.origin;p.scale.x=facing;appendActor(list,bank,death.body,p,camera,page);}
}
void appendSpiritProjectile(DrawList& list,const hk3ds::AnimationBank& bank,unsigned page,
                            const hk3ds::SpiritProjectile& spirit,hk3ds::PresentationTime time,hk3ds::Vec2 camera){
    // main.cpp:873-885: the spirit flies at worldPpu (the ActorPose default depth .004 gives
    // focal/38.104), mirrored by direction and scaled by renderScale. Its alpha is opacity().
    if(!spirit.active())return;
    // Bank guard: the shipped spirit bank always has these clips; a missing clip draws nothing.
    const auto* clip=bank.find(spirit.animation());
    if(!clip)return;
    hk3ds::AnimationPlayer player;player.play(clip);player.advance(spirit.age);
    const float d=float(spirit.direction);const auto scale=spirit.renderScale();
    ActorPose pose;pose.position=spirit.renderPosition(time);pose.scale={d*scale.x,scale.y};pose.alpha=spirit.opacity();
    appendActor(list,bank,player,pose,camera,page);
}
void appendSpirits(DrawList& list,const hk3ds::AnimationBank& bank,unsigned page,
                   const hk3ds::TutorialSession& g,hk3ds::Vec2 camera){
    const auto time=g.presentationTime();
    for(const auto& spirit:g.simulation.spirits.actors)appendSpiritProjectile(list,bank,page,spirit,time,camera);
}
void appendDecor(hk3ds::TutorialSession& g){
    // main.cpp:2093-2097. These RoomDraw transforms join the existing room merge.
    g.decorEffects.present(g.presentationTime());
    g.stalactiteVisuals.present(g.stalactites.data(),g.stalactiteCount,g.presentationTime(),g.decorEffects.visuals);
}
}
