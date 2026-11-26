#pragma once
#include "build_config.hpp"
#if !HK3DS_DIAGNOSTICS
#include "release_observers.hpp"
#else
// Opt-in acceptance observer: hardware state, never submission counters.
#include "hero.hpp"
#include "audio_gameplay_state.hpp"
#include <3ds.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <new>

namespace hk3ds {
class AudioEvidence {
    struct Configuration {
        std::atomic<unsigned> sequence{0},clip{~0u},frames{0},generation{0},loop{0};
        std::atomic<float> rate{0},gain{0};
    };
    struct Row {
        unsigned frame,tick,position,frames,generation,millis;
        float rate,gain;
        unsigned short clip;
        unsigned char channel,flags;
    };
    struct Game {
        unsigned frame,tick,actor,flags,health,attack,slash,decor,plant,epoch,millis;
        float decorPitch,plantPitch;
    };
    static constexpr unsigned capacity=131072,gameCapacity=12000;
    static_assert(sizeof(Row)==36,"bounded audio observer storage");
    std::unique_ptr<Row[]> rows_;
    std::unique_ptr<Game[]> game_;
    // State changes are deduplicated per owner; moving loops remain per frame.
    static constexpr unsigned stateCapacity=65536;
    std::unique_ptr<AudioGameplayState[]> states_;
    std::array<AudioGameplayState,256> lastStates_{};
    unsigned stateCount_=0;
    std::array<Configuration,24> configurations_{};
    std::array<unsigned,24> previousGeneration_{};
    std::array<bool,24> previousPlaying_{};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> recording_{false};
    std::atomic<unsigned> dropped_{0};
    LightLock observationLock_{};
    unsigned count_=0,gameCount_=0,frame_=0,tick_=0;
    u64 began_=0;
    unsigned outputStopMillis_=0;
    bool outputStopped_=false;
public:
    // Hero IDs 0..11, enemy IDs 100+EnemyClip, wind IDs 200..202.
    void begin(bool force=false) {
        auto* flag=std::fopen("sdmc:/3ds/HK3DS/audio-acceptance.flag","rb");
        if(!force&&!flag)return;
        if(flag)std::fclose(flag);
        LightLock_Init(&observationLock_);
        rows_.reset(new(std::nothrow) Row[capacity]);
        game_.reset(new(std::nothrow) Game[gameCapacity]);
        states_.reset(new(std::nothrow) AudioGameplayState[stateCapacity]);
        outputStopped_=false;outputStopMillis_=0;
        enabled_=true;
        began_=osGetTime();
        if(!rows_||!game_||!states_)++dropped_;
        recording_=true;
    }
    bool enabled()const{return enabled_.load();}
    void startHardwareSampling() {
        if(enabled())ndspSetCallback([](void* data){static_cast<AudioEvidence*>(data)->sampleVoices(true);},this);
    }
    void stopHardwareSampling() {
        ndspSetCallback(nullptr,nullptr);
        if(enabled()){LightLock_Lock(&observationLock_);LightLock_Unlock(&observationLock_);}
    }
    void freeze() {
        recording_=false;
        if(enabled()){LightLock_Lock(&observationLock_);LightLock_Unlock(&observationLock_);}
    }
    // Application lifecycle, independent of NDSP submissions/cursor feedback.
    void outputStopped() {
        if(enabled()&&!recording_&&!outputStopped_){
            outputStopMillis_=static_cast<unsigned>(osGetTime()-began_);outputStopped_=true;
        }
    }
    bool writeOutputStop(FILE* f)const {
        if(!enabled())return true;
        return outputStopped_&&std::fprintf(f,"# audio-lifecycle-v1 stop_millis=%u\n",outputStopMillis_)>0;
    }
    void beforeUpdate(unsigned frame,unsigned tick) {
        if(!recording_)return;
        LightLock_Lock(&observationLock_);frame_=frame;tick_=tick;LightLock_Unlock(&observationLock_);
        // Preserve terminal feedback before update replaces/stops a generation.
        sampleVoices(true);
    }
    void configure(unsigned channel,unsigned clip,unsigned frames,float rate,float gain,bool loop,bool restart) {
        if(!recording_||channel>=24)return;
        LightLock_Lock(&observationLock_);
        auto& c=configurations_[channel];++c.sequence;
        c.clip=clip;c.frames=frames;c.rate=rate;c.gain=gain;c.loop=loop;
        if(restart)++c.generation;
        ++c.sequence;
        LightLock_Unlock(&observationLock_);
    }
    // Timestamp gameplay before dispatch can perform stream I/O or block on DSP.
    // Hardware feedback keeps its own later clock in afterUpdate/callback.
    void sampleGameplay(unsigned frame,unsigned tick,const Hero& h,unsigned decor,float decorPitch,
                unsigned plant,float plantPitch,unsigned epoch) {
        if(!recording_||!rows_||!game_)return;
        const unsigned millis=static_cast<unsigned>(osGetTime()-began_);
        if(gameCount_<gameCapacity)game_[gameCount_++]={frame,tick,unsigned(h.actor),
            unsigned(h.contacts.ground)|(unsigned(h.jumping)<<1)|(unsigned(h.dashing)<<2)|(unsigned(h.controlEnabled)<<3),
            unsigned(h.vitals.health),h.nail.serial,h.nail.wall?9u:h.nail.direction==Strike::Up?7u:
            h.nail.direction==Strike::Down?8u:h.nail.alternate?6u:5u,decor,plant,epoch,millis,decorPitch,plantPitch};
        else ++dropped_;
    }
    void afterUpdate() { sampleVoices(false); }
private:
    void sampleVoices(bool detailedOnly) {
        if(!recording_||!rows_)return;
        LightLock_Lock(&observationLock_);
        if(!recording_||!frame_){LightLock_Unlock(&observationLock_);return;}
        const unsigned millis=static_cast<unsigned>(osGetTime()-began_);
        for(unsigned channel=0;channel<24;++channel) {
            const auto& c=configurations_[channel];
            const unsigned clip=c.clip.load(),generation=c.generation.load(),frames=c.frames.load(),loop=c.loop.load();
            const float rate=c.rate.load(),gain=c.gain.load();
            if(clip==~0u)continue;
            const bool playing=ndspChnIsPlaying(channel);
            const auto position=ndspChnGetSamplePos(channel);
            // Loops retain render-frame coverage; one-shots also retain every
            // DSP update, including clips/stops shorter than a rendered frame.
            if(detailedOnly&&loop&&playing==previousPlaying_[channel]&&generation==previousGeneration_[channel])continue;
            if(playing||previousPlaying_[channel]||generation!=previousGeneration_[channel]) {
                if(count_<capacity)rows_[count_++]={frame_,tick_,position,frames,generation,millis,rate,gain,
                    static_cast<unsigned short>(clip),static_cast<unsigned char>(channel),
                    static_cast<unsigned char>(unsigned(playing)|(loop<<1))};
                else ++dropped_;
            }
            previousPlaying_[channel]=playing;previousGeneration_[channel]=generation;
        }
        LightLock_Unlock(&observationLock_);
    }
public:
    // Comment records travel in the existing stages artifact, including through
    // already-installed HK3DS-Net loaders. Ordinary stage CSV readers ignore them.
    void sampleScene(unsigned frame,const TutorialSession& t) {
        if(!recording_||!states_)return;
        unsigned slot=0;
        captureAudioGameplay(frame,t,[&](const AudioGameplayState& state){
            if(slot>=lastStates_.size()){++dropped_;return;}
            auto& last=lastStates_[slot++];
            const bool same=last.kind==state.kind&&last.owner==state.owner&&
                last.a==state.a&&last.b==state.b&&last.c==state.c&&last.d==state.d&&last.e==state.e&&
                last.x==state.x&&last.y==state.y&&last.z==state.z&&last.w==state.w;
            if(last.frame&&same)return;
            last=state;
            if(stateCount_<stateCapacity)states_[stateCount_++]=state;else ++dropped_;
        });
    }
    bool write(FILE* f)const {
        if(!enabled())return true;
        bool ok=std::fprintf(f,"# audio-v1 rows=%u games=%u dropped=%u observer_bytes=%u lifecycle=1 clock=dispatch-start\n",count_,gameCount_,dropped_.load(),
                             unsigned(sizeof(Row)*capacity+sizeof(Game)*gameCapacity+sizeof(AudioGameplayState)*stateCapacity))>0;
        ok&=std::fprintf(f,"# audio-state-v1 rows=%u dropped=%u\n",stateCount_,dropped_.load())>0;
        for(unsigned i=0;i<stateCount_;++i)ok&=states_[i].write(f);
        for(unsigned i=0;i<gameCount_;++i){const auto& g=game_[i];
            ok&=std::fprintf(f,"# audio-game %u,%u,%u,%u,%u,%u,%u,%u,%.9g,%u,%.9g,%u,%u\n",
                g.frame,g.tick,g.actor,g.flags,g.health,g.attack,g.slash,g.decor,g.decorPitch,g.plant,g.plantPitch,g.epoch,g.millis)>0;}
        for(unsigned i=0;i<count_;++i){const auto& r=rows_[i];
            ok&=std::fprintf(f,"# audio-voice %u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%u,%u,%u\n",
                r.frame,r.tick,unsigned(r.channel),unsigned(r.clip),r.generation,unsigned(r.flags&1),r.position,
                r.rate,r.gain,r.frames,unsigned((r.flags>>1)&1),r.millis)>0;}
        ok&=std::fprintf(f,"# audio-complete rows=%u games=%u\n",count_,gameCount_)>0;
        return ok&&dropped_==0&&gameCount_>0&&count_>0;
    }
};
inline AudioEvidence audioEvidence;
}

#endif
