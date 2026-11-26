#include "room_audio.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
namespace {
struct Stream {unsigned id;const char* file;const char* source;};
constexpr Stream streams[]={
#define ROOM_AUDIO_STREAM(id,file,source) {id,file,source},
#include "room_audio_streams.inc"
#undef ROOM_AUDIO_STREAM
};
unsigned clip(const char* source){
    if(!source)return ~0u;
    for(const auto& stream:streams)if(!std::strcmp(source,stream.source))return stream.id;
    throw std::runtime_error("Scene audio source needs a PSP stream");
}
bool hasRain(hk3ds::SceneAudioId area){
    using hk3ds::SceneAudioId;
    return area==SceneAudioId::Tutorial||area==SceneAudioId::Dirtmouth||
           area==SceneAudioId::Crossroads||area==SceneAudioId::ShamanTemple;
}
bool hasCaveSurface(hk3ds::SceneAudioId area){return hasRain(area);}
unsigned snapshotRoom(const char* scene){
    if(!scene)return 0;
    if(!std::strcmp(scene,"Crossroads_01"))return 1;
    if(!std::strcmp(scene,"Crossroads_07"))return 7;
    if(!std::strcmp(scene,"Crossroads_33"))return 33;
    if(!std::strcmp(scene,"Crossroads_08"))return 8;
    return 0;
}
bool inTownMusicRegion(hk3ds::Rect hero){
    constexpr float x=130.99813842773438f,y=22.13051986694336f;
    constexpr float halfWidth=70.80499267578125f/2,halfHeight=32.49988555908203f/2;
    return hero.left<x+halfWidth&&hero.right>x-halfWidth&&
           hero.bottom<y+halfHeight&&hero.top>y-halfHeight;
}
}
void RoomAudio::updateAtmosphere(AudioDevice& device,hk3ds::SceneAudioId area,unsigned epoch,unsigned room,bool caveMuted){
    // Stable original source identities permit both cues to overlap. One
    // streaming cursor per clip; none are duplicated in the shared slots.
    constexpr unsigned voices[]={hk3ds::windFirstChannel,audioCaveNoisesVoice,
                                 audioSurfaceWindVoice,audioSurfaceGateVoice};
    constexpr unsigned clips[]={240,241,242,243};
    const bool surface=atmosArea_==hk3ds::SceneAudioId::Dirtmouth;
    // 3DS Silent cue (wind_audio Silent profile: no sources) mutes the Cave wind too.
    // The mute reuses this 0.5 s overlap ramp; the Cave voices stop when it completes.
    const auto target=[&](unsigned i){return i<2?(surface||atmosMuted_?-80.f:0.f):
        (surface?(i==2?0.f:-2.2889833450317383f):-80.f);};
    if(atmosFrame_<15){
        const float t=float(++atmosFrame_)/15;
        for(unsigned i=0;i<4;++i){
            // at Cave3852: Cave sqrt / Surface smoothstep. at Surface3890:
            // Cave linear / Surface sqrt. Same dB convention as music.
            const float curve=(i<2?!surface:surface)?std::sqrt(t):
                (i<2?t:t*t*(3-2*t));
            atmosDb_[i]=atmosStartDb_[i]+(target(i)-atmosStartDb_[i])*curve;
        }
    }
    if(!hasCaveSurface(area)){
        if(hasCaveSurface(atmosArea_))for(unsigned voice:voices)device.stop(voice);
        atmosArea_=area;return;
    }
    if(area!=atmosArea_||epoch!=atmosEpoch_||room!=atmosRoom_||caveMuted!=atmosMuted_){
        atmosStartDb_=atmosDb_;atmosFrame_=0;
        atmosArea_=area;atmosEpoch_=epoch;atmosRoom_=room;atmosMuted_=caveMuted;
    }
    const bool incomingSurface=area==hk3ds::SceneAudioId::Dirtmouth;
    for(unsigned i=0;i<4;++i){
        // Muted Cave voices keep playing through the ramp, then stop like other disabled sources.
        const bool enabled=(i>=2)==incomingSurface&&(i>=2||!caveMuted);
        if(!enabled&&atmosFrame_==15){
            if(device.active(voices[i]))device.stop(voices[i]);
            continue;
        }
        const float gain=std::pow(10.f,atmosDb_[i]/20.f);
        if(device.active(voices[i]))device.volumePan(voices[i],gain,0);
        else if(enabled)device.play(voices[i],clips[i],true,gain);
    }
}
void RoomAudio::transitionMusicSnapshot(Snapshot snapshot,unsigned frames){
    snapshotStartDb_=musicDb_;snapshotFrame_=0;snapshotFrames_=frames;
    snapshot_=snapshot;
}
void RoomAudio::advanceMusicSnapshot(){
    if(snapshotFrame_<snapshotFrames_){
        const float t=float(++snapshotFrame_)/snapshotFrames_;
        // resources.assets mixer3809: Normal3855 / Sub Area3865 / Silent3896.
        // Unity 2017.4 curves: 2=Squared, 3=SquareRoot. Main is bass;
        // Sub is melody. Interpolate mixer dB, then convert to source gain.
        const bool silent=snapshot_==Snapshot::Silent,subArea=snapshot_==Snapshot::SubArea;
        const float subCurve=silent?t*t:std::sqrt(t);
        const float mainCurve=subArea?t*t:subCurve;
        const float mainTarget=silent?-60.f:subArea?-59.60676193237305f:0.f;
        const float subTarget=silent?-60.f:subArea?.025578511878848076f:0.f;
        musicDb_[0]=snapshotStartDb_[0]+(mainTarget-snapshotStartDb_[0])*mainCurve;
        musicDb_[1]=snapshotStartDb_[1]+(subTarget-snapshotStartDb_[1])*subCurve;
    }
    // Waiting to apply a snapshot does not freeze the previous mixer fade.
    if(snapshotDelayFrames_&&!--snapshotDelayFrames_)transitionMusicSnapshot(Snapshot::Normal,150);
}
void RoomAudio::requestMusic(hk3ds::SceneAudioId cue,unsigned delay){
    if(requestedMusic_==cue)return;
    requestedMusic_=cue;musicDelayFrames_=delay;musicPending_=true;
}
void RoomAudio::updateMusic(AudioDevice& device){
    using namespace hk3ds;
    const bool replace=musicPending_&&!musicDelayFrames_;
    const auto oldMask=sceneAudioProfile(activeMusic_).musicMask;
    if(replace){activeMusic_=requestedMusic_;musicPending_=false;}
    if(musicDelayFrames_)--musicDelayFrames_;
    const auto& profile=sceneAudioProfile(sceneAudioKeepsPhase(activeMusic_,area_)?area_:activeMusic_);
    for(unsigned index=0;index<profile.files.size();++index){
        const unsigned voice=windFirstChannel+index;
        if(!(profile.musicMask&(1u<<index))){if(replace&&(oldMask&(1u<<index)))device.stop(voice);continue;}
        const bool crossroads=activeMusic_==SceneAudioId::Crossroads;
        const bool dirtmouth=activeMusic_==SceneAudioId::Dirtmouth;
        const float gain=crossroads||dirtmouth?std::pow(10.f,musicDb_[crossroads?index-1:0]/20.f):profile.gains[index];
        if(replace||!device.active(voice))device.play(voice,clip(profile.files[index]),true,gain);
        else device.volumePan(voice,gain,0);
    }
    // Action is phase-locked to Main by equal loop length and pitch;
    // Tension loops independently. Both carry fixed -60 dB in the headers.
    for(const auto voice:{audioTensionVoice,audioActionVoice}){
        if(activeMusic_==SceneAudioId::Crossroads){
            if(replace||!device.active(voice))device.play(voice,voice==audioTensionVoice?247:250,true,1.f);
        }else if(device.active(voice))device.stop(voice);
    }
}
void RoomAudio::load(Platform& platform,AudioDevice& device){
    for(const auto& stream:streams){
        device.load(stream.id,platform.path(stream.file),true);
        if(std::strncmp(stream.source,"music-",6)==0)device.musicClip(stream.id);
    }
}
void RoomAudio::update(AudioDevice& device,const char* scene,unsigned epoch,const char* /*destination*/,hk3ds::Rect hero,
                       FightAudio fight){
    using namespace hk3ds;
    const auto area=sceneAudioId(scene);
    const auto& profile=sceneAudioProfile(area);
    std::array<unsigned,WindCue::ChannelCount> clips{};
    // Reject an unsupported source before issuing any partial scene commands.
    for(unsigned i=0;i<clips.size();++i)clips[i]=clip(profile.files[i]);
    advanceMusicSnapshot();
    const auto room=snapshotRoom(scene);
    const bool entered=area!=area_||room!=snapshotRoom_||epoch!=snapshotEpoch_;
    if(room&&entered){
        // SceneManager reapplies snapshots on entry even when the MusicCue
        // is unchanged. Its pending wait is independent of the cue's wait.
        snapshotDelayFrames_=room==1?30u:0u;
        if(!snapshotDelayFrames_)transitionMusicSnapshot(room==33?Snapshot::SubArea:Snapshot::Normal,
                                                         room==33?180u:60u);
        requestMusic(area,room==1?30u:0u);
    }else if(area==SceneAudioId::Tutorial&&entered){
        snapshotDelayFrames_=0;
        transitionMusicSnapshot(Snapshot::Silent,60);
    }else if(area==SceneAudioId::Dirtmouth){
        // level7:4080 calls TransitionTo directly, not the SceneManager's
        // delayed snapshot API; its enter and exit share cue s7:639 (Main).
        if(entered)inTownMusicRegion_=false;
        const bool inside=inTownMusicRegion(hero);
        if(inside!=inTownMusicRegion_){
            transitionMusicSnapshot(inside?Snapshot::Normal:Snapshot::Silent,
                                    inside?(requestedMusic_==area?90u:30u):180u);
            requestMusic(area,0);
            inTownMusicRegion_=inside;
        }
    }else if(entered){
        if(!sceneAudioKeepsPhase(requestedMusic_,area))requestMusic(area,0);
        if(area==SceneAudioId::Crossroads){
            snapshotDelayFrames_=0;musicDb_={};snapshotFrame_=snapshotFrames_=0;
        }
    }
    // The fight owns the music cue while active; Defeated silences it until the
    // room changes. Dormant keeps the area cue (the 3DS rule's default branch).
    if(fight==FightAudio::FalseKnight)requestMusic(SceneAudioId::FalseKnight,0);
    else if(fight==FightAudio::Defeated)requestMusic(SceneAudioId::Silent,0);
    snapshotRoom_=room;snapshotEpoch_=epoch;
    area_=area;
    // Ordinary departures have no AudioManager calls. Apply the next cue
    // only when its SceneManager starts, independently of the screen fade.
    // Account for time since the previous command before starting a new fade.
    // The scene's first packet is at t=0, not one frame into the envelope.
    cue_.advance(1.f/30);cue_.scene(epoch,true,area);
    for(unsigned i=0;i<playing_.size();++i){
        const auto& channel=cue_.channels()[i];const auto voice=windFirstChannel+i;
        if(hasCaveSurface(area)||(profile.musicMask&(1u<<i))){
            if(playing_[i]!=~0u){device.stop(voice);playing_[i]=~0u;}
            continue;
        }
        if(channel.playing){
            const float gain=channel.gain*profile.gains[i];
            if(playing_[i]!=clips[i]||!device.active(voice))
                device.play(voice,clips[i],true,gain);
            else device.volumePan(voice,gain,0);
            playing_[i]=clips[i];
        }else if(playing_[i]!=~0u){device.stop(voice);playing_[i]=~0u;}
    }
    updateMusic(device);
    updateAtmosphere(device,area,epoch,room,fight==FightAudio::Defeated);
    // Cave and Surface enable sources 5/6, both at -60.40684127807617 dB.
    // Gain lives in each clip header. Keep their independent phase even when
    // the music cue changes (Tutorial -> Town -> Crossroads); no file I/O here.
    // Use one stable cue identity because both rain sources remain enabled.
    const bool rain=hasRain(area);
    for(unsigned i=0;i<2;++i){
        const unsigned voice=audioRainIndoorVoice+i;
        if(rain){
            if(!device.active(voice))device.play(voice,248+i,true,1.f);
            else device.volumePan(voice,1.f,0);
        }else if(device.active(voice))device.stop(voice);
    }
}
}
