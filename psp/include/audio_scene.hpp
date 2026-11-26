#pragma once
#include "world_city_crest_audio.hpp"
#include "world_shaman_audio.hpp"
#include "world_stag_audio.hpp"
#include "world_city_toll.hpp"
#include "world_city_lift.hpp"
#include "world_city_transport.hpp"
#include "world_key_doors.hpp"
#include "platform.hpp"
#include "audio_channels.hpp"
#include "wind_audio.hpp"
#include "room_audio.hpp"
#include "scene_enemies.hpp"
#include <cstdio>

namespace hk3ds {class TutorialSession;struct Hero;}
namespace hkpsp {
class AudioScene {
public:
    AudioScene(Platform& platform,bool diagnostic);
    void volumes(unsigned music,unsigned sound,unsigned master){device_->volumes(music,sound,master);}
    void update(const hk3ds::TutorialSession& game,unsigned frame);
    void finish();
private:
    // Outlive the FILEs, including constructor failure and unfinished captures.
    using EvidenceBuffers=std::array<std::array<char,4096>,2>;
    static_assert(sizeof(EvidenceBuffers)<=8192,"Audio evidence buffer budget");
    std::unique_ptr<EvidenceBuffers> evidenceBuffers_;
    using Log=std::unique_ptr<FILE,decltype(&std::fclose)>;
    Log gameplay_{nullptr,&std::fclose},states_{nullptr,&std::fclose};
    unsigned stateRows_=0;
    void capture(const hk3ds::TutorialSession& game,unsigned frame);
    std::unique_ptr<AudioDevice> device_;
    hk3ds::EnemyAudioEvents events_;
    hk3ds::CityCrestAudio cityCrest_;
    hk3ds::ShamanPillarAudio shamanPillar_;
    hk3ds::StagLeverAudio stagLever_;
    std::array<unsigned,4> stagCues_{};
    hk3ds::CityLiftToll::Phase cityTollPhase_{};
    hk3ds::CityLiftLowering::Phase cityLiftPhase_{};
    hk3ds::CityLiftTransport::Phase cityTransportPhase_{};
    hk3ds::JijiKeyDoor::Phase jijiPhase_{};
    bool jijiDirtPlayed_=false;
    unsigned routeTollPurchase_=0;
    unsigned bossHit_=0,bossImpact_=0,bossPhase_=0;
    std::uint32_t uumuuSerial_=0;
    RoomAudio room_;
    hk3ds::FootstepAudioSlot footstep_;
    unsigned footstepFrame_=0;
    hk3ds::SlashAudioSlots slashes_;
    std::array<unsigned,hk3ds::AudioVoices::VoiceCount> generations_{};
    std::array<std::array<unsigned,3>,41> grass_{};
    std::array<unsigned,27> contacts_{};
    std::array<unsigned,2> tinks_{};
    std::array<std::uint32_t,hk3ds::SceneEnemies::movementCapacity> movement_{};
    std::array<std::uint32_t,hk3ds::SceneEnemies::movementCapacity> movementPlays_{};
    bool observed_=false,ground_=false,jumping_=false;
    unsigned nail_=0,decor_=0,plant_=0,epoch_=0,spirit_=0,abilityFlash_=0;
    int health_=5;
    bool footstepPlaying(unsigned frame)const;
    void hero(const hk3ds::Hero& hero,unsigned frame);
    void play(unsigned clip,unsigned frame,float pitch=1);
    void worldObjects(const hk3ds::TutorialSession& game);
    void enemies(const hk3ds::TutorialSession& game,unsigned frame);
};
}
