#include "audio_scene.hpp"
#include "tutorial.hpp"
#include "hero_tuning.hpp"
#include "audio_gameplay_state.hpp"
#include <stdexcept>

namespace hkpsp {
using namespace hk3ds;
static_assert(audioTensionVoice==ndspChannelCount&&audioCaveNoisesVoice==ndspChannelCount+1&&
              audioRainIndoorVoice==ndspChannelCount+2&&audioRainOutdoorVoice==ndspChannelCount+3&&
              audioActionVoice==ndspChannelCount+4&&audioSurfaceWindVoice==ndspChannelCount+5&&
              audioSurfaceGateVoice==ndspChannelCount+6&&audioVoiceCount==ndspChannelCount+7&&audioChannelLayoutValid(),
              "Preserve shared voices and reserve PSP music and atmosphere voices");
namespace {
constexpr unsigned enemyId(EnemyClip clip){return 32+unsigned(clip);}
constexpr unsigned uumuuOwner=1024;
constexpr unsigned uumuuFirst=unsigned(EnemyClip::UumuuZapAntic);
static_assert(unsigned(EnemyClip::UumuuExplode)-uumuuFirst==unsigned(UumuuSound::Explode));
// Same branches as src/audio_player.cpp:292-297.
FightAudio fightAudio(const CrossroadsBoss& boss){
    if(boss.kind!=CrossroadsBossKind::FalseKnight)return FightAudio::None;
    if(boss.phase==BossPhase::Defeated)return FightAudio::Defeated;
    return boss.phase==BossPhase::Dormant?FightAudio::None:FightAudio::FalseKnight;
}
}
AudioScene::AudioScene(Platform& platform,bool diagnostic):device_(makeAudio(platform.audioOutput(),diagnostic)){
    for(unsigned i=0;i<12;++i)device_->load(i,platform.path("audio-h"+std::to_string(i)+".pcm"));
#define AUDIO_ASSET(key,name) device_->load(enemyId(EnemyClip::key),platform.path("audio-" #key ".pcm"));
#include "audio_assets.inc"
#undef AUDIO_ASSET
    room_.load(platform,*device_);
    for(unsigned i=5;i<=9;++i){const auto& c=device_->clip(i);
        if(double(c.frames)/(audioRate*c.pitch)>=2*tuning::quickAttackCooldown)throw std::runtime_error("Audio slash reservation invalid");
    }
    // Stream voices never consume effect tails. Derive hero/decor needs from
    // admitted metadata; keep every shared enemy voice capable of long cues.
    std::uint32_t tails=0;
    for(unsigned i=0;i<hk3ds::AudioVoices::VoiceCount;++i)tails|=1u<<audioVoiceChannel(i);
    for(unsigned i=0;i<12;++i)if(device_->clip(i).tail()){
        if(i>=5&&i<=9)tails|=(1u<<slashFirstChannel)|(1u<<(slashFirstChannel+1));
        else tails|=1u<<((i==1||i==4)?footstepChannel:i);
    }
    device_->tailVoices(tails);
    device_->start();
    if(diagnostic){
        gameplay_.reset(std::fopen((platform.audioOutput()+"/audio-gameplay.csv").c_str(),"w"));
        states_.reset(std::fopen((platform.audioOutput()+"/audio-states.log").c_str(),"w"));
        if(!gameplay_||!states_)throw std::runtime_error("Audio gameplay evidence open");
        // PSP stdio otherwise allocates a large filesystem-sized buffer per log.
        // Keep diagnostics buffered without charging those implicit buffers to the game heap.
        evidenceBuffers_=std::make_unique<EvidenceBuffers>();
        if(std::setvbuf(gameplay_.get(),(*evidenceBuffers_)[0].data(),_IOFBF,(*evidenceBuffers_)[0].size())||
           std::setvbuf(states_.get(),(*evidenceBuffers_)[1].data(),_IOFBF,(*evidenceBuffers_)[1].size()))
            throw std::runtime_error("Audio evidence buffering");
        std::fprintf(gameplay_.get(),"frame,actor,audioGround,audioJumping,audioDash,heroControl,hp,audioAttack,audioSlash,decorBreaks,audioDecorPitch,audioPlantSerial,audioPlantPitch,audioEpoch\n");
    }
}
void AudioScene::capture(const TutorialSession& t,unsigned frame){
    if(!gameplay_)return;
    // Reuse the 3DS expectation clock: one-based 60 Hz ticks. PSP presents
    // committed gameplay every two ticks; no synthetic intermediate states.
    const unsigned tick=2*frame+1;const auto& h=t.simulation.hero;
    const unsigned slash=h.nail.wall?9:h.nail.direction==Strike::Up?7:h.nail.direction==Strike::Down?8:h.nail.alternate?6:5;
    if(std::fprintf(gameplay_.get(),"%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%.9g,%u,%.9g,%u\n",
        tick,unsigned(h.actor),unsigned(h.contacts.ground),unsigned(h.jumping),unsigned(h.dashing),unsigned(h.controlEnabled),
        h.vitals.health,unsigned(h.nail.serial),slash,t.decorEffects.decorSoundSerial(),double(t.decorEffects.soundPitch()),
        t.decorEffects.plantSoundSerial(),double(t.decorEffects.plantSoundPitch()),t.audioEpoch)<0)
        throw std::runtime_error("Audio gameplay evidence write");
    captureAudioGameplay(tick,t,[&](const AudioGameplayState& state){
        if(!state.write(states_.get()))throw std::runtime_error("Audio source evidence write");
        ++stateRows_;
    });
}
void AudioScene::finish(){
    device_->finish();
    if(states_){
        if(std::fprintf(states_.get(),"# audio-state-v1 rows=%u dropped=0\n",stateRows_)<0||
           std::fflush(states_.get())||std::fflush(gameplay_.get()))throw std::runtime_error("Audio evidence incomplete");
        states_.reset();gameplay_.reset();
    }
}
// Mirror src/audio_player.cpp:557-611: shared source slots, hero edge detection.
bool AudioScene::footstepPlaying(unsigned frame)const{
    // Frozen original sfx-softLanding.wav: 8197/44100 s; sfx-footStepsRun.wav:
    // 97600/48000 s. Resampled PCM lengths are playback storage, not source
    // lifetimes (65067/32000 would postpone the 61-frame running restart).
    const auto elapsed=std::uint64_t(frame-footstepFrame_);
    if(footstep_.clip()==1)return elapsed*44100<8197u*30;
    if(footstep_.clip()==4)return elapsed*48000<97600u*30;
    return false;
}
void AudioScene::play(unsigned clip,unsigned frame,float pitch){
    unsigned voice=clip;
    const auto& c=device_->clip(clip);
    if(clip==1||clip==4){
        if(pitch!=1||c.pitch!=1)throw std::runtime_error("Original footstep pitch changed");
        if(!footstep_.select(clip,footstepPlaying(frame)))throw std::runtime_error("Audio footstep collision");
        footstepFrame_=frame;voice=footstepChannel;
    }
    if(clip>=5&&clip<=9){
        const auto slot=slashes_.select(clip,{{device_->active(5),device_->active(6)}});
        if(slot==SlashAudioSlots::Invalid)throw std::runtime_error("Audio slash collision");
        voice=slashFirstChannel+slot;
    }
    device_->play(voice,clip,false,centeredAudioGain(c.channels),pitch);
}
void AudioScene::hero(const Hero& h,unsigned frame){
    if(observed_){
        const bool running=h.actor==Actor::Running&&!h.dashing&&h.controlEnabled;
        const bool landed=h.contacts.ground&&!ground_;
        if(footstep_.shouldStop(running,landed)){device_->stop(footstepChannel);footstep_.reset();}
        if(h.jumping&&!jumping_)play(0,frame);
        if(landed)play(h.actor==Actor::HardLanding?2:1,frame);
        if(h.vitals.health<health_&&h.actor!=Actor::Dead)play(3,frame);
        if(h.nail.serial!=nail_)play(h.nail.wall?9:h.nail.direction==Strike::Up?7:h.nail.direction==Strike::Down?8:h.nail.alternate?6:5,frame);
        if(running&&!footstepPlaying(frame))play(4,frame);
    }
    ground_=h.contacts.ground;jumping_=h.jumping;health_=h.vitals.health;nail_=h.nail.serial;observed_=true;
}
void AudioScene::worldObjects(const TutorialSession& t){
    auto& pool=events_.voices();
    const auto& camera=t.flow.camera;
    events_.observeTownLever(t.townLift.hitSerial(),t.audioEpoch);
    if(t.townLift.bound()){
        const auto position=t.townLift.soundPosition();
        pool.updateOwnerMix(316,townLeverAudioMix(position.x-camera.x,position.y-camera.y,t.townLift.soundDepth()+38.1f));
    }
    cityCrest_.observe(t.cityCrest.sounds(),t.cityCrest.bound(),t.audioEpoch,pool);
    if(t.cityCrest.bound()){
        const auto& position=t.cityCrest.data().soundPosition;
        cityCrest_.mix(pool,position[0]-camera.x,position[1]-camera.y,position[2]+38.1f,t.cityCrest.y());
    }
    shamanPillar_.observe(t.shamanPillar.sounds(),t.shamanPillar.bound(),t.audioEpoch,pool);
    if(t.shamanPillar.bound()){
        for(unsigned index=0;index<3;++index){
            const auto& position=t.shamanPillar.bank().draws[index?1:0].values;
            pool.updateOwnerMix(ShamanPillarAudio::owner+index,
                townLeverAudioMix(position[2]-camera.x,position[5]-camera.y,position[6]+38.1f));
        }
    }
    stagLever_.observe(t.stagLever.sounds(),t.stagLever.bound(),t.audioEpoch,pool);
    stagLever_.mix(pool,t.stagLever,{camera.x,camera.y});
    const auto& stag=t.resting;
    if(stag.bellSerial>stagCues_[0])pool.playAtPitch(EnemyClip::StagBell,207,1);
    if(stag.arrivalSerial>stagCues_[1]||stag.departSerial>stagCues_[2])pool.playAtPitch(EnemyClip::StagArrive,208,1);
    if(stag.voiceSerial>stagCues_[3])pool.playAtPitch(EnemyClip::StagVoice,209,1);
    stagCues_={stag.bellSerial,stag.arrivalSerial,stag.departSerial,stag.voiceSerial};
    using Toll=CityLiftToll::Phase;
    using Lift=CityLiftLowering::Phase;
    using Travel=CityLiftTransport::Phase;
    using Jiji=JijiKeyDoor::Phase;
    const auto toll=t.cityToll.phase();
    const auto lift=t.cityLift.phase();
    const auto travel=t.cityTransport.phase();
    const auto jiji=t.jijiDoor.phase();
    if(toll==Toll::PaidWait&&toll!=cityTollPhase_)
        pool.playAtPitch(EnemyClip::CityTollActivate,320,1);
    const auto& routeToll=t.routeTolls;
    if((routeToll.kind==2||routeToll.kind==3)&&routeToll.purchaseSerial>routeTollPurchase_)
        pool.playAtPitch(EnemyClip::CityTollActivate,320,1);
    routeTollPurchase_=routeToll.purchaseSerial;
    if(lift!=cityLiftPhase_&&lift==Lift::Delay)
        pool.playAtPitch(EnemyClip::CityLiftActivate,321,1);
    if(travel!=cityTransportPhase_&&(travel==Travel::Departing||travel==Travel::Arriving))
        pool.playAtPitch(EnemyClip::CityLiftActivate,321,1);
    if(travel!=cityTransportPhase_&&travel==Travel::Closing)
        pool.playAtPitch(EnemyClip::CityLiftClose,322,1);
    if((lift!=cityLiftPhase_&&lift==Lift::Landing)||(travel!=cityTransportPhase_&&travel==Travel::Landing))
        pool.playAtPitch(EnemyClip::CityLiftArrive,323,1);
    if((cityLiftPhase_==Lift::Landing&&lift==Lift::Open)||
       (cityTransportPhase_==Travel::Landing&&travel==Travel::Idle))
        pool.playAtPitch(EnemyClip::CityLiftOpen,324,1);
    if(jiji!=jijiPhase_){
        jijiDirtPlayed_=false;
        if(jiji==Jiji::Activate)pool.playAtPitch(EnemyClip::JijiKeyTurn,325,1);
        if(jiji==Jiji::Antic)pool.playAtPitch(EnemyClip::JijiOpenAntic,326,1);
        if(jiji==Jiji::SlamEnd)pool.playAtPitch(EnemyClip::JijiOpenSlamEnd,327,1);
    }
    if(jiji==Jiji::SlamEnd&&!jijiDirtPlayed_&&t.jijiDoor.slamEndElapsed()>=.65f){
        pool.playAtPitch(EnemyClip::PlatformDirtFall1,328,1);
        jijiDirtPlayed_=true;
    }
    cityTollPhase_=toll;cityLiftPhase_=lift;cityTransportPhase_=travel;jijiPhase_=jiji;
}
// Completion precedes
// dispatch, and the shared AudioVoices implementation owns every steal decision.
void AudioScene::enemies(const TutorialSession& t,unsigned frame){
    auto& pool=events_.voices();
    if(epoch_!=t.audioEpoch){
        for(unsigned i=0;i<generations_.size();++i)device_->stop(audioVoiceChannel(i));
        generations_={};events_.reset();grass_={};contacts_={};tinks_={};movement_={};movementPlays_={};abilityFlash_=0;epoch_=t.audioEpoch;
        cityTollPhase_={};cityLiftPhase_={};cityTransportPhase_={};jijiPhase_={};
        jijiDirtPlayed_=false;
        routeTollPurchase_=0;
        bossHit_=bossImpact_=bossPhase_=0;
        uumuuSerial_=0;
    }
    for(unsigned i=0;i<generations_.size();++i){const auto& logical=pool.voices()[i];
        if(generations_[i]&&logical.active&&!logical.loop&&!device_->active(audioVoiceChannel(i)))pool.finish(i,generations_[i]);
    }
    events_.observeFury(t.furyChest.openSerial(),t.furyChest.pickupSerial(),
        t.furyChest.pickup()==FuryPickupPhase::Idle||t.furyChest.pickup()==FuryPickupPhase::Lowering,t.audioEpoch,t.furyChest.tinkSerial());
    const auto& shiny=t.furyChest.draws()[unsigned(FuryPart::Shiny)];const auto pos=t.furyChest.shinyPosition();
    pool.updateLoopMix(37,shinyLoopMix(pos.x-t.flow.camera.x,pos.y-t.flow.camera.y,shiny.depth+38.1f));
    events_.observeTablets(t.roomAmbient,t.audioEpoch,t.flow.camera.x,t.flow.camera.y);
    events_.observeInitialFall(t.initialFall,t.audioEpoch);
    events_.observeSecrets(t.secretCues,t.audioEpoch);
    events_.observeRoomMasks(t.roomMasks.discoveries(),t.audioEpoch);
    events_.observeFloor(t.floorHits,t.audioEpoch);
    events_.observeCollapse(t.flow.crumbling(),t.flow.progress.collapsed,t.audioEpoch);
    events_.observeGreatDoor(t.greatDoor,t.audioEpoch);
    worldObjects(t);
    const auto& boss=t.crossroadsBoss;
    if(boss.active()){
        const bool knight=boss.kind==CrossroadsBossKind::FalseKnight;
        if(boss.hitSerial!=bossHit_)
            pool.play(knight?(boss.phase==BossPhase::Stagger?EnemyClip::KnightHead:EnemyClip::KnightArmour):EnemyClip::EnemyDamage,200,boss.hitSerial);
        if(boss.impactSerial!=bossImpact_)
            pool.play(knight?EnemyClip::KnightSlam:EnemyClip::GreatDoorHit1,201,boss.impactSerial);
        if(boss.phaseSerial!=bossPhase_){
            auto cue=EnemyClip::Count;
            if(boss.phase==BossPhase::Dying)cue=knight?EnemyClip::KnightDeath:EnemyClip::EnemyDeathSword;
            else if(knight){
                switch(boss.phase){
                case BossPhase::Jump:cue=EnemyClip::KnightJump;break;
                case BossPhase::Stagger:cue=EnemyClip::KnightStagger;break;
                case BossPhase::Rage:cue=EnemyClip::KnightRage;break;
                case BossPhase::Slam:cue=EnemyClip::KnightSwing;break;
                default:break;
                }
            }
            if(cue!=EnemyClip::Count)pool.play(cue,202,boss.phaseSerial);
        }
        bossHit_=boss.hitSerial;bossImpact_=boss.impactSerial;bossPhase_=boss.phaseSerial;
    }else bossHit_=bossImpact_=bossPhase_=0;
    if(t.uumuu.active()){
        const auto& history=t.uumuu.audio();
        if(history.serial<uumuuSerial_)uumuuSerial_=0;
        if(history.serial-uumuuSerial_>UumuuAudioEvents::capacity)
            uumuuSerial_=history.serial-UumuuAudioEvents::capacity;
        while(uumuuSerial_!=history.serial){
            const auto serial=++uumuuSerial_;
            const auto& event=history.events[serial%UumuuAudioEvents::capacity];
            if(event.serial!=serial)continue;
            const auto clip=static_cast<EnemyClip>(uumuuFirst+unsigned(event.sound));
            const unsigned owner=uumuuOwner+serial%UumuuAudioEvents::capacity;
            pool.playAtPitch(clip,owner,event.pitch);
            pool.updateOwnerMix(owner,linearSpatialMix(event.position.x-t.flow.camera.x,
                event.position.y-t.flow.camera.y,38.1f,39,50,event.volume));
        }
    }else if(uumuuSerial_){
        for(unsigned offset=0;offset<UumuuAudioEvents::capacity;++offset)pool.stopOwner(uumuuOwner+offset);
        uumuuSerial_=0;
    }
    events_.observeSceneWalls(t.sceneWalls.cues(),t.audioEpoch);
    pool.updateOwnerMix(33,linearSpatialMix(190.48000015f-t.flow.camera.x,68.42000008f-t.flow.camera.y,38.71f,39,50));
    const auto cocoon=t.healthCocoon.visual();
    events_.observeCocoon(t.healthCocoon.moveSerial(),t.healthCocoon.breakSerial(),t.audioEpoch,
        cocoon.position.x-t.flow.camera.x,cocoon.position.y-t.flow.camera.y,cocoon.depth);
    events_.observeScuttlers(t.healthScuttlers.bounceSerial(),t.healthScuttlers.deathSerial(),t.audioEpoch);
    for(unsigned i=0;i<t.stalactiteCount;++i)events_.observeStalactite(t.stalactites[i],i,t.audioEpoch,t.flow.camera.x,t.flow.camera.y);
    events_.observeShade(t.shade,t.audioEpoch);
    pool.updateLoopMix(39,shadeLoopMix(t.shade.position.x-t.flow.camera.x,t.shade.position.y-t.flow.camera.y));
    events_.observeHeroDeath(t.death.active(),t.audioEpoch);
    events_.observeHeroDash(t.simulation.hero.dashing,t.audioEpoch);
    if(spirit_!=t.simulation.hero.spellSerial){spirit_=t.simulation.hero.spellSerial;pool.playAtPitch(EnemyClip::SpiritCast,205,1);}
    if(abilityFlash_!=t.abilityPickup.flashSerial){
        abilityFlash_=t.abilityPickup.flashSerial;pool.playAtPitch(EnemyClip::ShinyPickup,206,1);
    }
    events_.observeConditional(t.conditionalEvents,t.audioEpoch);
    events_.beginActorFrame();
    for(unsigned i=0;i<t.crawlerCount;++i){const auto& a=t.crawlers[i];events_.observe({i+1,a.active(),a.alive(),true,false,a.injurySerial,a.killSerial,0,0,
        a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y,t.crawlerAudioDepth(i)});}
    for(unsigned i=0;i<t.buzzerCount;++i){const auto& a=t.buzzers[i];
        const bool chase=a.state==BuzzerState::ChaseInSight||a.state==BuzzerState::ChaseOutOfSight;
        events_.observe({17+i,true,a.alive(),false,chase,a.injurySerial,a.killSerial,a.startleSerial,a.corpseSplatSerial,
            a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y,t.buzzerAudioDepth(i)});}
    events_.endActorFrame(true);
    for(unsigned i=0;i<movement_.size();++i){
        const auto source=t.sceneEnemies.movementAudio(i);
        const unsigned owner=512+i;
        if(source.serial!=movement_[i]||source.playSerial!=movementPlays_[i]){
            pool.stopOwner(owner);movement_[i]=source.serial;movementPlays_[i]=source.playSerial;
        }
        if(source.serial){
            // Idempotent for an admitted owner; retry if the bounded pool was full.
            pool.startLoop(source.clip,owner,0);
            pool.updatePitch(owner,source.pitch);
            pool.updateLoopMix(owner,enemyLoopMix(source.clip,
                source.position.x-t.flow.camera.x,source.position.y-t.flow.camera.y,source.depth,source.profile));
        }
    }
    t.grass.dispatchAudio(pool,grass_,t.flow.camera);t.contactEffects.dispatchAudio(pool,contacts_,t.flow.camera);
    t.nailInteractions.dispatchAudio(pool,tinks_,t.flow.camera);
    for(unsigned i=0;i<generations_.size();++i){const auto& logical=pool.voices()[i];const auto voice=audioVoiceChannel(i);
        if(!logical.active){if(generations_[i])device_->stop(voice);generations_[i]=0;continue;}
        if(logical.sourceId)throw std::runtime_error("Tutorial custom audio not admitted");
        if(generations_[i]!=logical.generation){
            device_->play(voice,enemyId(logical.clip),logical.loop,logical.gain,logical.pitch,logical.pan);
            generations_[i]=logical.generation;
            const auto owner=logical.owner;
            if(states_&&owner>=uumuuOwner&&owner<uumuuOwner+UumuuAudioEvents::capacity&&
                std::fprintf(states_.get(),"# uumuu-audio frame=%u epoch=%u clip=%u owner=%u gain=%.9g pitch=%.9g pan=%.9g\n",
                    frame,epoch_,unsigned(logical.clip),unsigned(owner),logical.gain,logical.pitch,logical.pan)<0)
                throw std::runtime_error("Uumuu audio evidence write");
            if(states_&&owner>=200&&owner<=202&&
                std::fprintf(states_.get(),"# boss-audio frame=%u epoch=%u clip=%u owner=%u gain=%.9g\n",
                    frame,t.audioEpoch,unsigned(logical.clip),unsigned(owner),double(logical.gain))<0)
                throw std::runtime_error("Boss audio evidence write");
            if(states_&&(CityCrestAudio::owns(owner)||(owner>=ShamanPillarAudio::owner&&owner<ShamanPillarAudio::owner+3)||
                owner==StagLeverAudio::owner||owner==StagLeverAudio::owner+1||owner==316||(owner>=207&&owner<=209)||
                (owner>=320&&owner<=328))&&
                std::fprintf(states_.get(),"# world-audio frame=%u epoch=%u clip=%u owner=%u loop=%u gain=%.9g\n",
                    frame,t.audioEpoch,unsigned(logical.clip),unsigned(owner),unsigned(logical.loop),double(logical.gain))<0)
                throw std::runtime_error("World audio evidence write");
        }
        else device_->volumePan(voice,logical.gain,logical.pan,logical.pitch);
    }
}
// main.cpp:1425,1452,1966,2375-2386: load, scene selection, volumes, then
// hero/decor/plant/enemy observations once per presented committed game frame.
void AudioScene::update(const TutorialSession& game,unsigned frame){
    capture(game,frame);
    hero(game.simulation.hero,frame);
    const auto decor=game.decorEffects.decorSoundSerial(),plant=game.decorEffects.plantSoundSerial();
    if(decor>decor_)play(10,frame,game.decorEffects.soundPitch());
    if(!decor&&decor_)device_->stop(10);
    decor_=decor;
    if(plant>plant_)play(11,frame,game.decorEffects.plantSoundPitch());
    if(!plant&&plant_)device_->stop(11);
    plant_=plant;
    room_.update(*device_,game.sceneName(),game.audioEpoch,game.flow.transition?game.flow.transition->scene:nullptr,
                 bodyBounds(game.simulation.hero.position),fightAudio(game.crossroadsBoss));
    enemies(game,frame);device_->frame(frame);
}
}
