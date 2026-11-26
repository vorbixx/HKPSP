#pragma once
#include "hero.hpp"
#include "audio.hpp"
#include "world_city_crest_audio.hpp"
#include "world_stag_audio.hpp"
#include "world_shaman_audio.hpp"
#include "audio_channels.hpp"
#include "wind_audio_player.hpp"
#include <3ds.h>
#include <array>
#include <cstdint>
namespace hk3ds
{
class TutorialSession;
class AudioPlayer
{
public:
    bool load(const char* directory);
    bool loadEnemies(const char* directory);
    void updateEnemies(const TutorialSession& tutorial,const ActorAudioState* arena=nullptr);
    void update(const Hero& hero);
    void volumes(unsigned music,unsigned sound);
    void pause(bool paused);
    void updateDecor(unsigned serial,float pitch);
    void updateRoomBreakables(const TutorialSession&);
    void updatePlant(unsigned serial,float pitch);
    // Call on room entry. Unchanged original cues retain phase across room
    // epochs; unknown names select silence.
    void sceneAudio(SceneAudioId id){sceneAudio_=id;}
    void sceneAudio(const char* scene){sceneAudio(sceneAudioId(scene));}
    void release();
    bool ready()const{return initialized_&&enemiesReady_&&winds_.ready();}
    // Native Result for ndspInit; -1/-2/-3 identify metadata/PCM/linear allocation.
    std::int32_t error()const{return error_?error_:winds_.error();}
    unsigned played=0;
private:
    void mix(unsigned channel,float left,float right);
    std::array<std::array<float,2>,ndspChannelCount> mixes_{};
    float soundVolume_=1;
    bool paused_=false;
    struct Sound {void* data=nullptr;unsigned frames=0,channels=0,rate=0;float volume=1,pitch=1;ndspWaveBuf buffer{};};
    std::array<Sound,12> sounds_{};
    // Native PCM is shared by voices; a voice owns only its NDSP wave descriptor.
    std::array<Sound,static_cast<unsigned>(EnemyClip::Count)> enemySounds_{};
    struct Voice { ndspWaveBuf buffer{};std::uint32_t generation=0;float pitch=0,gain=-1,pan=0;bool heard=false; };
    std::array<Voice,AudioVoices::VoiceCount> enemyVoices_{};
    EnemyAudioEvents enemyEvents_;
    std::uint32_t spiritSerial_=0;
    unsigned stagBell_=0,stagArrival_=0,stagDepart_=0,stagVoice_=0;
    unsigned abilityFlash_=0;
    bool abilityPanel_=false;
    SlashAudioSlots slashSlots_;
    FootstepAudioSlot footstepSlot_;
    WindAudioPlayer winds_;
    SceneAudioId sceneAudio_=SceneAudioId::Tutorial;
    unsigned enemyEpoch_=0;
    unsigned bossHitSerial_=0,bossImpactSerial_=0,bossPhaseSerial_=0;
    unsigned hornetHitSerial_=0,hornetPhaseSerial_=0;
    std::array<unsigned,2> streamedVoices_{{~0u,~0u}};
    void stopStreamedVoice(unsigned voice);
    void updateUumuuAudio(const TutorialSession&);
    void updateCityCrestAudio(const TutorialSession&);
    CityCrestAudio cityCrestAudio_;
    bool cityCrestAudioLoaded_=false,cityCrestAudioAttempted_=false;
    void updateStagLeverAudio(const TutorialSession&);
    StagLeverAudio stagLeverAudio_;
    void updateShamanPillarAudio(const TutorialSession&);
    ShamanPillarAudio shamanPillarAudio_;
    bool shamanAudioLoaded_=false,shamanAudioAttempted_=false;
    bool stagAudioLoaded_=false,stagAudioAttempted_=false;
    bool uumuuAudioLoaded_=false;
    unsigned uumuuAudioSerial_=0;
    char soundDirectory_[256]{};
    std::array<std::array<unsigned,3>,41> grassSoundSerial_{};
    std::array<unsigned,27> contactSoundSerial_{};
    std::array<unsigned,2> tinkSoundSerial_{};
    unsigned enemyCInjury_=0,enemyCKill_=0;
    bool enemiesReady_=false;
    bool initialized_=false,observed_=false,ground_=false,jumping_=false;
    unsigned serial_=0;int health_=5;
    unsigned decorSerial_=0;
    std::array<Sound,AudioVoices::VoiceCount> roomBreakSounds_{};
    std::array<std::array<char,80>,AudioVoices::VoiceCount> roomBreakFiles_{};
    unsigned roomBreakSerial_=0,roomBreakEpoch_=~0u;
    unsigned plantSerial_=0;
    double sourceTime_=0;
    std::int32_t error_=0;
    void play(unsigned index,float volume=1,float pitch=1);
};
}
