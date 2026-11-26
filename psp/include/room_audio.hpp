#pragma once
#include "wind_audio.hpp"
#include "audio_channels.hpp"
#include "psp_audio.hpp"
#include "platform.hpp"
#include "physics.hpp"

namespace hkpsp {
// Boss music state from the 3DS updateEnemies rule (src/audio_player.cpp:292-297).
enum class FightAudio : unsigned { None, FalseKnight, Defeated };
// Area streams are primed before the device starts; frame commands do no I/O.
class RoomAudio {
public:
    void load(Platform& platform,AudioDevice& device);
    void update(AudioDevice& device,const char* scene,unsigned epoch,const char* destination=nullptr,
                hk3ds::Rect hero={},FightAudio fight=FightAudio::None);
private:
    enum class Snapshot { Normal, SubArea, Silent };
    void advanceMusicSnapshot();
    void transitionMusicSnapshot(Snapshot snapshot,unsigned frames);
    void requestMusic(hk3ds::SceneAudioId cue,unsigned delay);
    void updateMusic(AudioDevice& device);
    void updateAtmosphere(AudioDevice& device,hk3ds::SceneAudioId area,unsigned epoch,unsigned room,bool caveMuted);
    // resources mixer3805 starts at None3884: all four groups -80 dB.
    std::array<float,4> atmosDb_{{-80,-80,-80,-80}},atmosStartDb_{{-80,-80,-80,-80}};
    unsigned atmosFrame_=15,atmosEpoch_=~0u,atmosRoom_=0;
    bool atmosMuted_=false;
    hk3ds::SceneAudioId atmosArea_=hk3ds::SceneAudioId::Silent;
    std::array<float,2> musicDb_{{-60,-60}},snapshotStartDb_{{-60,-60}};
    unsigned snapshotFrame_=0,snapshotFrames_=0,snapshotDelayFrames_=0;
    unsigned snapshotRoom_=0,snapshotEpoch_=~0u;
    Snapshot snapshot_=Snapshot::Silent;
    bool inTownMusicRegion_=false;
    hk3ds::WindCue cue_;
    hk3ds::SceneAudioId area_=hk3ds::SceneAudioId::Silent;
    hk3ds::SceneAudioId requestedMusic_=hk3ds::SceneAudioId::Silent;
    hk3ds::SceneAudioId activeMusic_=hk3ds::SceneAudioId::Silent;
    unsigned musicDelayFrames_=0;
    bool musicPending_=false;
    std::array<unsigned,hk3ds::WindCue::ChannelCount> playing_{{~0u,~0u,~0u}};
};
}
