#include "audio_player.hpp"
#include "world_transport_audio.hpp"
#include "audio.hpp"
#include "tutorial.hpp"
#include "enemy_audio_probe.hpp"
#include "render_memory.hpp"
#include "graphics_profile.hpp"
#include "hero_tuning.hpp"
#include "audio_evidence.hpp"
#include <malloc.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#if HK3DS_DIAGNOSTICS
extern "C" { volatile hk3ds::EnemyAudioProbe hk3ds_enemy_audio{}; }
#endif
namespace hk3ds
{
namespace {
unsigned ordinaryHeadroom(){return envGetHeapSize()-static_cast<unsigned>(mallinfo().uordblks);}
}
bool AudioPlayer::load(const char* directory)
{
    release();if(!graphicsProfile().audio)return false;
    error_=ndspInit();if(R_FAILED(error_))return false;initialized_=true;
    ndspSetMasterVol(tutorialAudioMasterGain);
    char settings[256];std::snprintf(settings,sizeof(settings),"%s/sfx-settings.bin",directory);
    FILE* metadata=std::fopen(settings,"rb");float values[24]{};values[20]=values[21]=values[22]=values[23]=1;
    bool ok=metadata&&std::fread(values,4,20,metadata)==20&&std::fgetc(metadata)==EOF;
    if(metadata)std::fclose(metadata);
    for(float v:values)ok &= std::isfinite(v)&&v>=0&&v<=4;
    if(!ok){error_=-1;release();return false;}
    const char* names[]={"jump","softLanding","hardLanding","takeHit","footStepsRun","normalSlash","alternateSlash","upSlash","downSlash","wallSlash","decor","plant"};
    for(unsigned i=0;i<sounds_.size();++i)
    {
        char path[256];std::snprintf(path,sizeof(path),"%s/sfx-%s.wav",directory,names[i]);PcmSound pcm;
        if(!pcm.load(path)){if(i>=10)continue;error_=-2;release();return false;}
        auto& s=sounds_[i];s.data=linearAlloc(pcm.samples.size()*2);if(!s.data){error_=-3;release();return false;}
        std::memcpy(s.data,pcm.samples.data(),pcm.samples.size()*2);DSP_FlushDataCache(s.data,pcm.samples.size()*2);
        s.frames=pcm.samples.size()/pcm.channels;s.channels=pcm.channels;s.rate=pcm.rate;s.volume=values[i*2];s.pitch=values[i*2+1];
        // Preserve every slash tail even at Quick Slash cadence. Refuse a changed
        // bank that would invalidate the two-slot hardware reservation.
        if(i>=5&&i<=9&&(s.pitch<=0||s.frames/static_cast<float>(s.rate)/s.pitch>=2*tuning::quickAttackCooldown)){
            error_=-24;release();return false;
        }
        ndspChnReset(i);ndspChnSetInterp(i,NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(i,pcm.channels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetRate(i,pcm.rate);
    }
    audioEvidence.startHardwareSampling();
    return true;
}
bool AudioPlayer::loadEnemies(const char* directory)
{
    std::snprintf(soundDirectory_,sizeof(soundDirectory_),"%s",directory);
    if(!graphicsProfile().audio)return false;
    static_assert(audioChannelLayoutValid(),"reserved NDSP channels");
    if(!initialized_||enemiesReady_)return enemiesReady_;
    auto& p=hk3ds_enemy_audio;
    unsigned residentBytes=0;const auto linearBefore=linearSpaceFree();
    if constexpr(diagnosticsEnabled){++p.sequence;p.ready=0;p.residentBytes=0;p.tabletHumChannel=tabletHumChannel;}
    if constexpr(diagnosticsEnabled){p.linearBefore=linearSpaceFree();p.ordinaryBefore=ordinaryHeadroom();}
    const char* names[]={"crawler","buzzer_fly","enemy_damage","enemy_death_sword",
        "buzzer_startle_01","buzzer_startle_02","buzzer_startle_03","splat_01","Splat_02","Splat_03",
        "grass_move_6","grass_move_2","grass_move_5","grass_move_1","grass_move_3","grass_move_4",
        "props_grass_stud_move_1","props_grass_stud_move_4","props_grass_stud_move_5","props_grass_stud_move_3","props_grass_stud_move_2",
        "grass_cut_4","grass_cut_2","grass_cut_3","grass_cut_5",
        "breakable_wall_hit_1","breakable_wall_hit_2","breakable_wall_death",
        "health_cocoon_move","health_cocoon_break",
        "stalactite_start","stalactite_hit","stalactite_break",
        "hollow_shade_startle","hollow_shade_fly","hollow_shade_sword","boss_explode_clean","hero_void_scream_spell",
        "chest_open","barrel_death_2","shiny_item_loop","shiny_item_pickup","tablet_hum","tablet_wake",
        "water_drip_1","water_drip_2","water_drip_3","water_drip_4",
        "platform_dirt_fall_1","platform_dirt_fall_2","platform_dirt_fall_3","health_bug_wall_hit","tablet_inspect",
        "Mr_Mush_talk_02","mr_mush_takeoff","mage_knight_teleport","flamebearer_dash_prepare","flamebearer_dash_dash","flamebearer_teleport_out","flamebearer_death_and_absorb","flamebearer_shoot","flamebearer_fire_spiral_prepare","flamebearer_fire_spiral_cast","accordion_sting_04","nightmare_lantern_powering_up","nightmare_lantern_flame_burst",
        "hero_death_v2","hero_damage","hero_death_extra_details",
        "false_knight_land_1st_time","pigeon_fly_away_3","pigeon_fly_away_2","S75_Opening_Sting-08","sword_hit_reject","break_wall_after_tutorial_area","hero_dash","secret_discovered_temp","hero_fireball","stag-arrive","stag-voice","stag-bell","barrel_death_1","switch_gate_switch"};
    static_assert(sizeof(names)/sizeof(*names)==static_cast<unsigned>(EnemyClip::UumuuZapAntic)+2,"resident voice clip table");
    for(unsigned i=static_cast<unsigned>(EnemyClip::KnightArmour);i<static_cast<unsigned>(EnemyClip::Count);++i){
        enemySounds_[i].channels=2;enemySounds_[i].rate=44100;
    }
    // Reserve mandatory room allocations first; admit only this measured native PCM bank.
    const auto bankCap=graphicsProfile().enemyAudioBytes;
    const bool bounded=bankCap<enemyAudioBankCapBytes;
    bool ok=linearBefore>=bankCap+enemyAudioLinearReserveBytes;
    if(!ok)error_=-4;
    for(unsigned index=0;ok&&index<sizeof(names)/sizeof(*names);++index){
        const unsigned i=index==static_cast<unsigned>(EnemyClip::UumuuZapAntic)?static_cast<unsigned>(EnemyClip::FloorHit):index==static_cast<unsigned>(EnemyClip::UumuuZapAntic)+1?static_cast<unsigned>(EnemyClip::TownLeverHit):index;
        const bool grass=i>=static_cast<unsigned>(EnemyClip::GrassMove1)&&i<static_cast<unsigned>(EnemyClip::GreatDoorHit1);
        char path[256];std::snprintf(path,sizeof(path),"%s/sfx-%s-%s.wav",directory,grass?"grass":"enemy",names[index]);
        if(streamedEnemyClip(static_cast<EnemyClip>(i))){
            enemySounds_[i].channels=2;enemySounds_[i].rate=44100;continue;
        }
        PcmSound pcm;
        if(!pcm.load(path)){if(i==static_cast<unsigned>(EnemyClip::SpiritCast))continue;error_=-5;ok=false;break;}
        const auto bytes=pcm.samples.size()*sizeof(std::int16_t);
        if(!audioClipFits(bytes,residentBytes,bankCap)){if(bounded)continue;error_=-4;ok=false;break;}
        if(linearSpaceFree()<bytes+enemyAudioLinearReserveBytes){error_=-4;ok=false;break;}
        auto& s=enemySounds_[i];s.data=linearAlloc(bytes);
        if(!s.data){error_=-6;ok=false;break;}
        std::memcpy(s.data,pcm.samples.data(),bytes);DSP_FlushDataCache(s.data,bytes);
        s.frames=pcm.samples.size()/pcm.channels;s.channels=pcm.channels;s.rate=pcm.rate;
        residentBytes+=bytes;
    }
    if(!ok){for(auto& s:enemySounds_){if(s.data)linearFree(s.data);s={};}residentBytes=0;}
    enemiesReady_=ok;if constexpr(diagnosticsEnabled){p.residentBytes=residentBytes;p.ready=ok;p.error=error_;}
    if constexpr(diagnosticsEnabled){p.linearAfter=linearSpaceFree();p.ordinaryAfter=ordinaryHeadroom();}
    if constexpr(diagnosticsEnabled){++p.sequence;p.sequenceEnd=p.sequence;}
    if(!ok||!winds_.load(directory))return false;
    // Every observation of this streamed clip uses the same source length.
    // Per-frame mix updates must not overwrite its metadata with zero.
    auto& opening=enemySounds_[static_cast<unsigned>(EnemyClip::OpeningSting)];
    opening.frames=winds_.oneShotFrames(0);
    if(!opening.frames){error_=-24;if constexpr(diagnosticsEnabled){p.ready=0;p.error=error_;}return false;}
    return true;
}
void AudioPlayer::updateUumuuAudio(const TutorialSession& t)
{
    constexpr unsigned first=static_cast<unsigned>(EnemyClip::UumuuZapAntic);
    constexpr unsigned end=static_cast<unsigned>(EnemyClip::KnightArmour);
    auto& pool=enemyEvents_.voices();
    if(!t.uumuu.active()){
        // Stop every descriptor borrowing the scene bank before freeing its PCM.
        if(uumuuAudioLoaded_){
            for(unsigned i=0;i<enemyVoices_.size();++i){
                const auto& v=pool.voices()[i];
                if(v.active&&static_cast<unsigned>(v.clip)>=first&&static_cast<unsigned>(v.clip)<end){
                    ndspChnReset(audioVoiceChannel(i));enemyVoices_[i]={};pool.finish(i,v.generation);
                }
            }
            for(unsigned i=first;i<end;++i){auto& s=enemySounds_[i];if(s.data)linearFree(s.data);s={};}
        }
        uumuuAudioLoaded_=false;uumuuAudioSerial_=0;return;
    }
    if(!uumuuAudioLoaded_){
        constexpr const char* names[]={"zap_bug_pt1","zap_bug_pt2","zap_bug","Quirrel_battle_intro",
            "uumuu_helper_enter","uumuu_helper_exit","uumuu_helper_slash","uumuu_wounded",
            "Quirrel_battle_shout_01","Quirrel_battle_shout_02","Quirrel_battle_dialogue_01",
            "Quirrel_battle_dialogue_02","Quirrel_battle_dialogue_03","boss_final_hit","boss_gushing","boss_explode"};
        static_assert(sizeof(names)/sizeof(*names)==end-first,"Uumuu PCM bank");
        std::size_t bytes=0;bool ok=true;
        for(unsigned i=0;ok&&i<sizeof(names)/sizeof(*names);++i){
            char path[512];std::snprintf(path,sizeof(path),"%s/sfx-uumuu-%s.wav",soundDirectory_,names[i]);
            PcmSound pcm;if(!pcm.load(path)){ok=false;break;}
            const auto size=pcm.samples.size()*sizeof(std::int16_t);
            if(size>uumuuAudioCapBytes-bytes||linearSpaceFree()<size+enemyAudioLinearReserveBytes){ok=false;break;}
            auto& s=enemySounds_[first+i];s.data=linearAlloc(size);
            if(!s.data){ok=false;break;}
            std::memcpy(s.data,pcm.samples.data(),size);DSP_FlushDataCache(s.data,size);
            s.frames=pcm.samples.size()/pcm.channels;s.channels=pcm.channels;s.rate=pcm.rate;bytes+=size;
        }
        if(!ok){
            for(unsigned i=first;i<end;++i){auto& s=enemySounds_[i];if(s.data)linearFree(s.data);s={};}
            error_=-27;return;
        }
        uumuuAudioLoaded_=true;
    }
    const auto& events=t.uumuu.audio();
    if(events.serial<uumuuAudioSerial_)uumuuAudioSerial_=0;
    if(events.serial-uumuuAudioSerial_>UumuuAudioEvents::capacity)uumuuAudioSerial_=events.serial-UumuuAudioEvents::capacity;
    while(uumuuAudioSerial_!=events.serial){
        const auto serial=++uumuuAudioSerial_;const auto& e=events.events[serial%UumuuAudioEvents::capacity];
        if(e.serial!=serial)continue;
        const auto clip=static_cast<EnemyClip>(first+static_cast<unsigned>(e.sound));
        const unsigned owner=300+serial%UumuuAudioEvents::capacity;
        pool.playAtPitch(clip,owner,e.pitch);
        // Approximation: linear rolloff between the original source distances.
        pool.updateOwnerMix(owner,linearSpatialMix(e.position.x-t.flow.camera.x,e.position.y-t.flow.camera.y,38.1f,39,50,e.volume));
    }
}
void AudioPlayer::stopStreamedVoice(unsigned voice)
{
    for(unsigned slot=0;slot<streamedVoices_.size();++slot)if(streamedVoices_[slot]==voice){
        winds_.stopOneShot(slot);streamedVoices_[slot]=~0u;
    }
}
void AudioPlayer::updateCityCrestAudio(const TutorialSession& t)
{
    constexpr unsigned first=static_cast<unsigned>(EnemyClip::CityCrestOpen);
    constexpr unsigned end=static_cast<unsigned>(EnemyClip::CityCrestBump)+1;
    auto& pool=enemyEvents_.voices();
    if(!t.cityCrest.bound()){
        cityCrestAudio_.observe({},false,t.audioEpoch,pool);
        if(cityCrestAudioLoaded_){
            // Clear NDSP descriptors before releasing borrowed scene PCM.
            for(unsigned i=0;i<enemyVoices_.size();++i){
                const auto clip=static_cast<unsigned>(pool.voices()[i].clip);
                if(clip>=first&&clip<end){ndspChnReset(audioVoiceChannel(i));enemyVoices_[i]={};}
            }
            for(unsigned i=first;i<end;++i){auto& s=enemySounds_[i];if(s.data)linearFree(s.data);s={};}
        }
        cityCrestAudioLoaded_=cityCrestAudioAttempted_=false;return;
    }
    if(!cityCrestAudioAttempted_){
        cityCrestAudioAttempted_=true;
        constexpr const char* names[]={"open","move","bump"};
        std::size_t resident=0,bytes=0;bool ok=true;
        for(const auto& s:enemySounds_)if(s.data)resident+=std::size_t(s.frames)*s.channels*2;
        for(unsigned i=0;ok&&i<3;++i){
            char path[512];std::snprintf(path,sizeof(path),"%s/scenes/Fungus2_21/city-crest-%s.wav",soundDirectory_,names[i]);
            PcmSound pcm;if(!pcm.load(path)||pcm.channels!=2||pcm.rate!=44100){ok=false;break;}
            const auto size=pcm.samples.size()*sizeof(std::int16_t);
            if(!cityCrestAudioFits(size,bytes,resident,graphicsProfile().enemyAudioBytes,linearSpaceFree())){ok=false;break;}
            auto& s=enemySounds_[first+i];s.data=linearAlloc(size);if(!s.data){ok=false;break;}
            std::memcpy(s.data,pcm.samples.data(),size);DSP_FlushDataCache(s.data,size);
            s.frames=pcm.samples.size()/pcm.channels;s.channels=pcm.channels;s.rate=pcm.rate;bytes+=size;
        }
        if(!ok){
            for(unsigned i=first;i<end;++i){auto& s=enemySounds_[i];if(s.data)linearFree(s.data);s={};}
            // Preserve Old's bounded admission; New must load the measured bank.
            if(graphicsProfile().enemyAudioBytes>=enemyAudioBankCapBytes)error_=-28;
            return;
        }
        cityCrestAudioLoaded_=true;
    }
    if(!cityCrestAudioLoaded_)return;
    cityCrestAudio_.observe(t.cityCrest.sounds(),true,t.audioEpoch,pool);
    const auto& p=t.cityCrest.data().soundPosition;
    cityCrestAudio_.mix(pool,p[0]-t.flow.camera.x,p[1]-t.flow.camera.y,p[2]+38.1f,t.cityCrest.y());
}
void AudioPlayer::updateShamanPillarAudio(const TutorialSession& t)
{
    constexpr unsigned first=static_cast<unsigned>(EnemyClip::ShamanPillarShake),end=first+2;
    auto& pool=enemyEvents_.voices();
    if(!t.shamanPillar.bound()){
        shamanPillarAudio_.observe({},false,t.audioEpoch,pool);
        if(shamanAudioLoaded_){
            for(unsigned i=0;i<enemyVoices_.size();++i){const auto clip=static_cast<unsigned>(pool.voices()[i].clip);
                if(clip>=first&&clip<end){ndspChnReset(audioVoiceChannel(i));enemyVoices_[i]={};}}
            for(unsigned i=first;i<end;++i){auto& sound=enemySounds_[i];if(sound.data)linearFree(sound.data);sound={};}
        }
        shamanAudioLoaded_=shamanAudioAttempted_=false;return;
    }
    if(!shamanAudioAttempted_){
        shamanAudioAttempted_=true;std::size_t resident=0,bytes=0;bool ok=true;
        for(const auto& sound:enemySounds_)if(sound.data)resident+=std::size_t(sound.frames)*sound.channels*2;
        constexpr const char* names[]{"shake","finish"};constexpr unsigned channels[]{2,1};
        for(unsigned i=0;ok&&i<2;++i){
            char path[512];std::snprintf(path,sizeof(path),"%s/scenes/Crossroads_06/shaman-pillar-%s.wav",soundDirectory_,names[i]);
            PcmSound pcm;if(!pcm.load(path)||pcm.channels!=channels[i]||pcm.rate!=44100){ok=false;break;}
            const auto size=pcm.samples.size()*sizeof(std::int16_t);
            if(!shamanPillarAudioFits(size,bytes,resident,graphicsProfile().enemyAudioBytes,linearSpaceFree())){ok=false;break;}
            auto& sound=enemySounds_[first+i];sound.data=linearAlloc(size);if(!sound.data){ok=false;break;}
            std::memcpy(sound.data,pcm.samples.data(),size);DSP_FlushDataCache(sound.data,size);
            sound.frames=pcm.samples.size()/pcm.channels;sound.channels=pcm.channels;sound.rate=pcm.rate;bytes+=size;
        }
        if(!ok){
            for(unsigned i=first;i<end;++i){auto& sound=enemySounds_[i];if(sound.data)linearFree(sound.data);sound={};}
            if(graphicsProfile().enemyAudioBytes>=enemyAudioBankCapBytes)error_=-28;
        }else shamanAudioLoaded_=true;
    }
    // The switch reuses resident PCM even when Old cannot admit the room bank.
    shamanPillarAudio_.observe(t.shamanPillar.sounds(),true,t.audioEpoch,pool);
    for(unsigned i=0;i<3;++i){const auto& pos=t.shamanPillar.bank().draws[i?1:0].values;
        pool.updateOwnerMix(ShamanPillarAudio::owner+i,townLeverAudioMix(pos[2]-t.flow.camera.x,pos[5]-t.flow.camera.y,pos[6]+38.1f));}
}
void AudioPlayer::updateStagLeverAudio(const TutorialSession& t)
{
    constexpr auto index=static_cast<unsigned>(EnemyClip::StagBellRise);
    auto& pool=enemyEvents_.voices();auto& sound=enemySounds_[index];
    if(!t.stagLever.bound()){
        stagLeverAudio_.observe({},false,t.audioEpoch,pool);
        if(stagAudioLoaded_){
            for(unsigned i=0;i<enemyVoices_.size();++i)if(pool.voices()[i].clip==EnemyClip::StagBellRise){
                ndspChnReset(audioVoiceChannel(i));enemyVoices_[i]={};}
            if(sound.data)linearFree(sound.data);
            sound={};
        }
        stagAudioLoaded_=stagAudioAttempted_=false;return;
    }
    if(!stagAudioAttempted_){
        stagAudioAttempted_=true;char path[512];
        std::snprintf(path,sizeof(path),"%s/scenes/RestingGrounds_09/stag-bell-rise.wav",soundDirectory_);
        PcmSound pcm;std::size_t resident=0;
        for(const auto& s:enemySounds_)if(s.data)resident+=std::size_t(s.frames)*s.channels*2;
        bool ok=pcm.load(path)&&pcm.channels==1&&pcm.rate==44100;
        const auto bytes=pcm.samples.size()*sizeof(std::int16_t);
        ok=ok&&stagFeedbackAudioFits(bytes,resident,graphicsProfile().enemyAudioBytes,linearSpaceFree());
        if(ok){sound.data=linearAlloc(bytes);ok=sound.data!=nullptr;}
        if(!ok){if(graphicsProfile().enemyAudioBytes>=enemyAudioBankCapBytes)error_=-29;return;}
        std::memcpy(sound.data,pcm.samples.data(),bytes);DSP_FlushDataCache(sound.data,bytes);
        sound.frames=pcm.samples.size();sound.channels=1;sound.rate=pcm.rate;stagAudioLoaded_=true;
    }
    if(!stagAudioLoaded_)return;
    stagLeverAudio_.observe(t.stagLever.sounds(),true,t.audioEpoch,pool);
    stagLeverAudio_.mix(pool,t.stagLever,{t.flow.camera.x,t.flow.camera.y});
}
void AudioPlayer::updateEnemies(const TutorialSession& t,const ActorAudioState* arena)
{
    if(!initialized_)return;
    // Scene loading must not fade or rewind an unchanged original cue.
    const bool continuing=t.flow.transition&&sceneAudioKeepsPhase(sceneAudio_,sceneAudioId(t.flow.transition->scene));
    const auto& boss=t.crossroadsBoss;
    const bool knight=boss.kind==CrossroadsBossKind::FalseKnight;
    const auto cue=knight&&boss.phase!=BossPhase::Dormant&&boss.phase!=BossPhase::Defeated?SceneAudioId::FalseKnight:
        knight&&boss.phase==BossPhase::Defeated?SceneAudioId::Silent:sceneAudio_;
    winds_.scene(t.audioEpoch,!t.flow.transition||continuing,cue);
    if(!enemiesReady_)return;
    auto& p=hk3ds_enemy_audio;if constexpr(diagnosticsEnabled){++p.sequence;}
    auto& pool=enemyEvents_.voices();
    if(boss.active()){
        if(boss.hitSerial!=bossHitSerial_)pool.play(knight?(boss.phase==BossPhase::Stagger?EnemyClip::KnightHead:EnemyClip::KnightArmour):EnemyClip::EnemyDamage,200,boss.hitSerial);
        if(boss.impactSerial!=bossImpactSerial_)pool.play(knight?EnemyClip::KnightSlam:EnemyClip::GreatDoorHit1,201,boss.impactSerial);
        if(boss.phaseSerial!=bossPhaseSerial_){
            if(boss.phase==BossPhase::Dying)pool.play(knight?EnemyClip::KnightDeath:EnemyClip::EnemyDeathSword,202,boss.phaseSerial);
            else if(knight){
                const auto sound=boss.phase==BossPhase::Jump?EnemyClip::KnightJump:boss.phase==BossPhase::Stagger?EnemyClip::KnightStagger:
                    boss.phase==BossPhase::Rage?EnemyClip::KnightRage:boss.phase==BossPhase::Slam?EnemyClip::KnightSwing:EnemyClip::Count;
                if(sound!=EnemyClip::Count)pool.play(sound,202,boss.phaseSerial);
            }
        }
        bossHitSerial_=boss.hitSerial;bossImpactSerial_=boss.impactSerial;bossPhaseSerial_=boss.phaseSerial;
    }else bossHitSerial_=bossImpactSerial_=bossPhaseSerial_=0;
    // Hornet uses the resident enemy bank until a dedicated boss bank is
    // admitted.  Keep the event mapping deterministic so attacks, staggers,
    // and the escape still produce the same audible cues on hardware.
    const GreenpathActor* hornet=nullptr;
    for(const auto& actor:t.greenpath.actors)if(actor.kind==GreenpathKind::Hornet&&actor.visible()){hornet=&actor;break;}
    if(hornet&&hornet->phase!=GreenpathPhase::Dormant){
        if(hornet->hits!=hornetHitSerial_){
            pool.play(EnemyClip::EnemyDamage,210,hornet->hits);
            hornetHitSerial_=hornet->hits;
        }
        const auto serial=t.greenpath.phaseSerial();
        if(serial!=hornetPhaseSerial_){
            EnemyClip cue=EnemyClip::Count;
            switch(hornet->phase){
                case GreenpathPhase::Antic: cue=EnemyClip::EnemyDamage; break;
                case GreenpathPhase::Jump: cue=EnemyClip::KnightJump; break;
                case GreenpathPhase::Throw: cue=EnemyClip::EnemyDamage; break;
                case GreenpathPhase::Sphere: cue=EnemyClip::EnemyDamage; break;
                case GreenpathPhase::Evade: cue=EnemyClip::KnightJump; break;
                case GreenpathPhase::Counter: cue=EnemyClip::EnemyDamage; break;
                case GreenpathPhase::Dead:
                case GreenpathPhase::Escape: cue=EnemyClip::EnemyDeathSword; break;
                default: break;
            }
            if(cue!=EnemyClip::Count)pool.play(cue,211,serial);
            hornetPhaseSerial_=serial;
        }
    }else hornetHitSerial_=hornetPhaseSerial_=0;
    if(enemyEpoch_!=t.audioEpoch){
        uumuuAudioSerial_=0;
        for(unsigned slot=0;slot<streamedVoices_.size();++slot){winds_.stopOneShot(slot);streamedVoices_[slot]=~0u;}
        for(unsigned i=0;i<enemyVoices_.size();++i){ndspChnReset(audioVoiceChannel(i));enemyVoices_[i]={};}
        enemyEvents_.reset();grassSoundSerial_={};contactSoundSerial_={};tinkSoundSerial_={};
        hornetHitSerial_=hornetPhaseSerial_=0;abilityFlash_=0;enemyEpoch_=t.audioEpoch;
    }
    // Completion is read before dispatch, so newly freed slots are available this frame.
    for(unsigned i=0;i<enemyVoices_.size();++i){
        auto& backend=enemyVoices_[i];const auto& logical=pool.voices()[i];
        if(!backend.generation||!logical.active)continue;
        const auto clip=static_cast<unsigned>(logical.clip);
        unsigned status=backend.buffer.status;
        for(unsigned slot=0;slot<streamedVoices_.size();++slot)if(i==streamedVoices_[slot])status=winds_.oneShotStatus(slot);
        if(!backend.heard&&(status==NDSP_WBUF_PLAYING||status==NDSP_WBUF_DONE)){
            if(clip<10)if constexpr(diagnosticsEnabled){++p.playing[clip];}
            if(logical.clip==EnemyClip::TabletWake)if constexpr(diagnosticsEnabled){++p.tabletWakePlaying;}
            backend.heard=true;
        }
        if(logical.clip==EnemyClip::Buzzer&&logical.pitch==1.25f&&status==NDSP_WBUF_PLAYING)if constexpr(diagnosticsEnabled){++p.chasePlaying;}
        if(logical.clip==EnemyClip::TabletHum&&logical.gain>0&&status==NDSP_WBUF_PLAYING)if constexpr(diagnosticsEnabled){++p.tabletHumAudibleFrames;}
        if(!logical.loop&&status==NDSP_WBUF_DONE){if(clip<10)if constexpr(diagnosticsEnabled){++p.completed[clip];}pool.finish(i,backend.generation);}
    }
    enemyEvents_.observeFury(t.furyChest.openSerial(),t.furyChest.pickupSerial(),
        t.furyChest.pickup()==FuryPickupPhase::Idle||t.furyChest.pickup()==FuryPickupPhase::Lowering,t.audioEpoch,t.furyChest.tinkSerial());
    const auto& shiny=t.furyChest.draws()[static_cast<unsigned>(FuryPart::Shiny)];
    const auto shinyPosition=t.furyChest.shinyPosition();
    pool.updateLoopMix(37,shinyLoopMix(shinyPosition.x-t.flow.camera.x,shinyPosition.y-t.flow.camera.y,shiny.depth+38.1f));
    enemyEvents_.observeTablets(t.roomAmbient,t.audioEpoch,t.flow.camera.x,t.flow.camera.y);
    enemyEvents_.observeInitialFall(t.initialFall,t.audioEpoch);
    enemyEvents_.observeSecrets(t.secretCues,t.audioEpoch);
    enemyEvents_.observeRoomMasks(t.roomMasks.discoveries(),t.audioEpoch);
    enemyEvents_.observeFloor(t.floorHits,t.audioEpoch);
    enemyEvents_.observeCollapse(t.flow.crumbling(),t.flow.progress.collapsed,t.audioEpoch);
    enemyEvents_.observeGreatDoor(t.greatDoor,t.audioEpoch);
    enemyEvents_.observeTownLever(t.townLift.hitSerial(),t.audioEpoch);
    updateCityCrestAudio(t);
    updateStagLeverAudio(t);
    updateShamanPillarAudio(t);
    if(t.townLift.bound()){
        const auto p=t.townLift.soundPosition();
        enemyEvents_.voices().updateOwnerMix(316,townLeverAudioMix(
            p.x-t.flow.camera.x,p.y-t.flow.camera.y,t.townLift.soundDepth()+38.1f));
    }
    enemyEvents_.observeSceneWalls(t.sceneWalls.cues(),t.audioEpoch);
    // Source level6:8417: linear 3D, min/max 39/50, volume 1.
    // Transform 5674 through 5677/6759 is (190.48,68.42,.61).
    enemyEvents_.voices().updateOwnerMix(33,linearSpatialMix(
        190.48000015f-t.flow.camera.x,68.42000008f-t.flow.camera.y,38.71f,39,50));
    const auto cocoon=t.healthCocoon.visual();
    enemyEvents_.observeCocoon(t.healthCocoon.moveSerial(),t.healthCocoon.breakSerial(),t.audioEpoch,
        cocoon.position.x-t.flow.camera.x,cocoon.position.y-t.flow.camera.y,cocoon.depth);
    enemyEvents_.observeScuttlers(t.healthScuttlers.bounceSerial(),t.healthScuttlers.deathSerial(),t.audioEpoch);
    for(unsigned i=0;i<t.stalactiteCount;++i)enemyEvents_.observeStalactite(t.stalactites[i],i,t.audioEpoch,t.flow.camera.x,t.flow.camera.y);
    enemyEvents_.observeShade(t.shade,t.audioEpoch);
    pool.updateLoopMix(39,shadeLoopMix(t.shade.position.x-t.flow.camera.x,t.shade.position.y-t.flow.camera.y));
    enemyEvents_.observeHeroDeath(t.death.active(),t.audioEpoch);
    enemyEvents_.observeHeroDash(t.simulation.hero.dashing,t.audioEpoch);
    if(spiritSerial_!=t.simulation.hero.spellSerial){
        spiritSerial_=t.simulation.hero.spellSerial;pool.playAtPitch(EnemyClip::SpiritCast,205,1);
    }
    const auto& stag=t.resting;
    if(stagBell_!=stag.bellSerial){stagBell_=stag.bellSerial;pool.playAtPitch(EnemyClip::StagBell,207,1);}
    if(stagArrival_!=stag.arrivalSerial||stagDepart_!=stag.departSerial){stagArrival_=stag.arrivalSerial;stagDepart_=stag.departSerial;pool.playAtPitch(EnemyClip::StagArrive,208,1);}
    if(stagVoice_!=stag.voiceSerial){stagVoice_=stag.voiceSerial;pool.playAtPitch(EnemyClip::StagVoice,209,1);}
    // Preserve the existing other pickup paths; only Shiny Control has moved
    // its sound from the panel edge to the authored flash event.
    const bool abilityPanel=t.abilityPickup.phase==AbilityPickup::Phase::Item;
    if(abilityFlash_!=t.abilityPickup.flashSerial||
       (!t.abilityPickup.shinySequence()&&abilityPanel&&!abilityPanel_)){
        abilityFlash_=t.abilityPickup.flashSerial;pool.playAtPitch(EnemyClip::ShinyPickup,206,1);
    }
    abilityPanel_=abilityPanel;
    enemyEvents_.observeConditional(t.conditionalEvents,t.audioEpoch);
    enemyEvents_.beginActorFrame();
    for(unsigned i=0;i<t.crawlerCount;++i){const auto& a=t.crawlers[i];
        enemyEvents_.observe({i+1,a.active(),a.alive(),true,false,a.injurySerial,a.killSerial,0,0,
            a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y,t.crawlerAudioDepth(i)});}
    for(unsigned i=0;i<t.buzzerCount;++i){const auto& a=t.buzzers[i];
        const bool chase=a.state==BuzzerState::ChaseInSight||a.state==BuzzerState::ChaseOutOfSight;
        enemyEvents_.observe({17+i,true,a.alive(),false,chase,a.injurySerial,a.killSerial,a.startleSerial,a.corpseSplatSerial,
            a.position.x-t.flow.camera.x,a.position.y-t.flow.camera.y,t.buzzerAudioDepth(i)});}
    if(arena)enemyEvents_.observe(*arena);
    enemyEvents_.endActorFrame(true);
    // C actors share the admitted injury/death PCM without consuming loop slots.
    if(enemyCInjury_>t.enemyC.injurySerial)enemyCInjury_=0;
    if(enemyCKill_>t.enemyC.killSerial)enemyCKill_=0;
    for(unsigned n=0;enemyCInjury_<t.enemyC.injurySerial&&n<12;++n)
        pool.dispatch(EnemySoundEvent::Injury,80,++enemyCInjury_*2654435761u);
    for(unsigned n=0;enemyCKill_<t.enemyC.killSerial&&n<12;++n)
        pool.dispatch(EnemySoundEvent::Kill,80,++enemyCKill_*2246822519u);
    t.grass.dispatchAudio(pool,grassSoundSerial_,t.flow.camera);
    t.contactEffects.dispatchAudio(pool,contactSoundSerial_,t.flow.camera);
    t.nailInteractions.dispatchAudio(pool,tinkSoundSerial_,t.flow.camera);
    updateUumuuAudio(t);
    updateRoomBreakables(t);
    unsigned active=0;
    for(unsigned i=0;i<enemyVoices_.size();++i){
        auto& backend=enemyVoices_[i];const auto& logical=pool.voices()[i];const unsigned channel=audioVoiceChannel(i);
        if(!logical.active){if(backend.generation){
            stopStreamedVoice(i);
            ndspChnReset(channel);if(i<AudioVoices::LoopVoiceCount)if constexpr(diagnosticsEnabled){++p.stoppedLoops;}
        }backend={};continue;}
        const auto clip=static_cast<unsigned>(logical.clip);const auto& s=logical.sourceId?roomBreakSounds_[i]:enemySounds_[clip];
        if(!s.data&&!streamedEnemyClip(logical.clip)){
            stopStreamedVoice(i);ndspChnReset(channel);backend={};pool.finish(i,logical.generation);continue;
        }
        ++active;
        if(backend.generation!=logical.generation){
            stopStreamedVoice(i);
            ndspChnReset(channel);backend={};backend.generation=logical.generation;
            backend.buffer.data_vaddr=s.data;backend.buffer.nsamples=s.frames;backend.buffer.looping=logical.loop;
            ndspChnSetInterp(channel,NDSP_INTERP_POLYPHASE);
            ndspChnSetFormat(channel,s.channels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
            mix(channel,logical.gain*(1-logical.pan),logical.gain*(1+logical.pan));
            ndspChnSetRate(channel,s.rate*logical.pitch);backend.pitch=logical.pitch;
            if(streamedEnemyClip(logical.clip)){
                // Discovery has its own queue so it can overlap the opening sting.
                const auto sound=logical.clip>=EnemyClip::KnightArmour?
                    static_cast<WindAudioPlayer::OneShot>(static_cast<unsigned>(WindAudioPlayer::OneShot::KnightArmour)+clip-static_cast<unsigned>(EnemyClip::KnightArmour)):
                    logical.clip==EnemyClip::CollapseAfterTutorial?WindAudioPlayer::OneShot::Collapse:logical.clip==EnemyClip::SecretDiscovered?WindAudioPlayer::OneShot::Secret:
                    logical.clip==EnemyClip::HeroDeathDetails?WindAudioPlayer::OneShot::HeroDeathDetails:WindAudioPlayer::OneShot::Opening;
                const unsigned slot=WindAudioPlayer::streamSlot(sound);
                if(streamedVoices_[slot]!=~0u&&streamedVoices_[slot]!=i){
                    pool.finish(streamedVoices_[slot],enemyVoices_[streamedVoices_[slot]].generation);
                }
                if(winds_.startOneShot(channel,sound)){
                    streamedVoices_[slot]=i;enemySounds_[clip].frames=winds_.oneShotFrames(slot);
                }
                else error_=winds_.error();
            }else ndspChnWaveBufAdd(channel,&backend.buffer);
            audioEvidence.configure(channel,logical.sourceId?logical.sourceId:100+clip,s.frames,s.rate*logical.pitch,logical.gain,logical.loop,true);
            if(clip<10)if constexpr(diagnosticsEnabled){++p.submitted[clip];}
            if(logical.clip==EnemyClip::TabletHum)if constexpr(diagnosticsEnabled){++p.tabletHumSubmitted;}
            ++played;
        }else if(backend.pitch!=logical.pitch){ndspChnSetRate(channel,s.rate*logical.pitch);backend.pitch=logical.pitch;}
        if(backend.gain!=logical.gain||backend.pan!=logical.pan){
            mix(channel,logical.gain*(1-logical.pan),logical.gain*(1+logical.pan));
            backend.gain=logical.gain;backend.pan=logical.pan;
        }
        audioEvidence.configure(channel,logical.sourceId?logical.sourceId:100+clip,s.frames,s.rate*logical.pitch,logical.gain,logical.loop,false);
    }
    if constexpr(diagnosticsEnabled){p.active=active;if(active>p.peak)p.peak=active;}
    for(unsigned i=0;i<10;++i)if constexpr(diagnosticsEnabled){p.dispatched[i]=pool.counters()[i].dispatched;}
    if constexpr(diagnosticsEnabled){++p.sequence;p.sequenceEnd=p.sequence;}
}
void AudioPlayer::updateDecor(unsigned serial,float pitch)
{
    if(serial>decorSerial_){
        ndspChnSetFormat(10,sounds_[10].channels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
        play(10,1,pitch);
    }
    if(serial==0&&decorSerial_&&initialized_)ndspChnReset(10);
    decorSerial_=serial;
}
void AudioPlayer::updateRoomBreakables(const TutorialSession& tutorial)
{
    if(!initialized_)return;
    if(roomBreakEpoch_!=tutorial.audioEpoch){
        // updateEnemies resets every real channel before this epoch cleanup.
        for(auto& sound:roomBreakSounds_){if(sound.data)linearFree(sound.data);sound={};}
        roomBreakFiles_={};roomBreakSerial_=0;roomBreakEpoch_=tutorial.audioEpoch;
    }
    const auto& room=tutorial.roomBreakables;
    const auto serial=room.soundSerial();
    if(serial<roomBreakSerial_)roomBreakSerial_=0;
    if(serial-roomBreakSerial_>254)roomBreakSerial_=serial-254;
    auto& pool=enemyEvents_.voices();
    while(roomBreakSerial_<serial){
        const auto& event=room.soundEvent(++roomBreakSerial_);
        if(!event.choice)continue;
        const auto voice=pool.playCustom(roomBreakClipId(event.choice->file.data()),
            0x80000000u|roomBreakSerial_,event.gain,event.pitch);
        if(voice==AudioVoices::VoiceCount)continue;
        auto& sound=roomBreakSounds_[voice];
        if(roomBreakFiles_[voice]!=event.choice->file){
            stopStreamedVoice(voice);ndspChnReset(audioVoiceChannel(voice));
            if(sound.data)linearFree(sound.data);
            sound={};roomBreakFiles_[voice]={};
            char path[384];std::snprintf(path,sizeof(path),"%s/scenes/%s/%s",
                soundDirectory_[0]?soundDirectory_:"romfs:",tutorial.sceneName(),event.choice->file.data());
            auto fail=[&](){error_=-31;pool.finish(voice,pool.voices()[voice].generation);};
            auto* file=std::fopen(path,"rb");if(!file){fail();continue;}
            std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);std::fclose(file);
            if(size<=44||size>1024*1024+44){fail();continue;}
            PcmSound pcm;if(!pcm.load(path)){fail();continue;}
            sound.data=linearAlloc(pcm.samples.size()*2);
            if(!sound.data){fail();error_=-3;continue;}
            std::memcpy(sound.data,pcm.samples.data(),pcm.samples.size()*2);
            DSP_FlushDataCache(sound.data,pcm.samples.size()*2);
            sound.frames=unsigned(pcm.samples.size()/pcm.channels);sound.channels=pcm.channels;sound.rate=pcm.rate;
            roomBreakFiles_[voice]=event.choice->file;
        }
        pool.voices()[voice].gain=centeredAudioGain(sound.channels,event.gain);
    }
}
void AudioPlayer::updatePlant(unsigned serial,float pitch)
{
    if(serial>plantSerial_)play(11,1,pitch);
    if(serial==0&&plantSerial_&&initialized_)ndspChnReset(11);
    plantSerial_=serial;
}
void AudioPlayer::mix(unsigned channel,float left,float right)
{
    mixes_[channel]={{left,right}};
    float values[12]{};values[0]=left*soundVolume_;values[1]=right*soundVolume_;
    ndspChnSetMix(channel,values);
}
void AudioPlayer::volumes(unsigned music,unsigned sound)
{
    music=music>10?10:music;sound=sound>10?10:sound;
    soundVolume_=sound*.1f;winds_.volumes(music,sound);
    if(initialized_)for(unsigned i=0;i<ndspChannelCount;++i)
        if(i<windFirstChannel||i>=windFirstChannel+windChannelCount)
            mix(i,mixes_[i][0],mixes_[i][1]);
}
void AudioPlayer::pause(bool paused)
{
    if(paused_==paused)return;
    paused_=paused;
    if(initialized_)for(unsigned i=0;i<ndspChannelCount;++i)
        if(i<windFirstChannel||i>=windFirstChannel+windChannelCount)ndspChnSetPaused(i,paused);
}
void AudioPlayer::play(unsigned i,float volume,float pitch)
{
    if(!initialized_||i>=sounds_.size())return;
    auto& s=sounds_[i];if(!s.data)return;
    unsigned channel=i;
    if(i==1||i==4){
        if(!footstepSlot_.select(i,footstepSlot_.playing(sourceTime_))){error_=-26;return;}
        footstepSlot_.started(sourceTime_,double(s.frames)/(s.rate*pitch*s.pitch));
        channel=footstepChannel;
    }
    if(i>=5&&i<=9){
        std::array<bool,slashChannelCount> busy{};
        for(unsigned n=0;n<slashChannelCount;++n){const auto clip=slashSlots_.clip(n);
            if(clip!=SlashAudioSlots::Invalid){const auto state=sounds_[clip].buffer.status;
                busy[n]=state==NDSP_WBUF_QUEUED||state==NDSP_WBUF_PLAYING;}}
        const auto slot=slashSlots_.select(i,busy);
        if(slot==SlashAudioSlots::Invalid){error_=-25;return;}
        channel=slashFirstChannel+slot;
    }
    // Azahar's partial reset (WaveBufClear) leaves the current PCM buffer alive.
    // A replacement must discard it as well as the queue; restore all settings.
    ndspChnReset(channel);s.buffer={};s.buffer.data_vaddr=s.data;s.buffer.nsamples=s.frames;
    ndspChnSetInterp(channel,NDSP_INTERP_POLYPHASE);
    ndspChnSetFormat(channel,s.channels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
    const float gain=centeredAudioGain(s.channels,volume*s.volume);
    mix(channel,gain,gain);ndspChnSetRate(channel,s.rate*pitch*s.pitch);
    ndspChnWaveBufAdd(channel,&s.buffer);++played;
    audioEvidence.configure(channel,i,s.frames,s.rate*pitch*s.pitch,gain,false,true);
}
void AudioPlayer::update(const Hero& h)
{
    if(!initialized_)return;
    sourceTime_=double(osGetTime())/1000;
    if(observed_)
    {
        const bool running=h.actor==Actor::Running&&!h.dashing&&h.controlEnabled;
        const bool landed=h.contacts.ground&&!ground_;
        // Stop before a landing can claim the shared channel. A reset does not
        // synchronously clear the old wave-buffer status on every DSP backend.
        if(footstepSlot_.shouldStop(running,landed)){
            ndspChnReset(footstepChannel);footstepSlot_.reset();
        }
        if(h.jumping&&!jumping_)play(0);
        if(landed)play(h.actor==Actor::HardLanding?2:1);
        if(h.vitals.health<health_&&h.actor!=Actor::Dead)play(3);
        if(h.nail.serial!=serial_)
        {
            const unsigned slash=h.nail.wall?9:h.nail.direction==Strike::Up?7:h.nail.direction==Strike::Down?8:h.nail.alternate?6:5;
            play(slash);
        }
        // HeroAudioController gates footsteps on !softLanding.isPlaying; its
        // Update stops footsteps whenever not running, before the next landing.
        if(running&&!footstepSlot_.playing(sourceTime_))play(4);
    }
    ground_=h.contacts.ground;jumping_=h.jumping;health_=h.vitals.health;serial_=h.nail.serial;observed_=true;
}
void AudioPlayer::release()
{
    cityCrestAudio_={};cityCrestAudioLoaded_=cityCrestAudioAttempted_=false;
    if(initialized_){audioEvidence.outputStopped();audioEvidence.stopHardwareSampling();}
    winds_.release();
    streamedVoices_={{~0u,~0u}};
    if(initialized_)for(unsigned i=0;i<enemyVoices_.size();++i)ndspChnReset(audioVoiceChannel(i));
    for(auto& s:enemySounds_){if(s.data)linearFree(s.data);s={};}
    uumuuAudioLoaded_=false;uumuuAudioSerial_=0;
    enemyVoices_={};enemyEvents_.reset();grassSoundSerial_={};contactSoundSerial_={};enemiesReady_=false;enemyEpoch_=0;
    if(initialized_)for(unsigned i=0;i<sounds_.size();++i)ndspChnReset(i);
    for(auto& s:sounds_){if(s.data)linearFree(s.data);s={};}
    for(auto& sound:roomBreakSounds_){if(sound.data)linearFree(sound.data);sound={};}
    roomBreakFiles_={};roomBreakSerial_=0;roomBreakEpoch_=~0u;
    if(initialized_)ndspExit();
    initialized_=observed_=false;
    slashSlots_.reset();
    footstepSlot_.reset();
    decorSerial_=0;
    plantSerial_=0;
}
}
