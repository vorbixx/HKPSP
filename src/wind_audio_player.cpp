#include "wind_audio_player.hpp"
#include "audio_evidence.hpp"
#include <cstdio>
#include <algorithm>
#if HK3DS_DIAGNOSTICS
extern "C" { volatile hk3ds::WindAudioProbe hk3ds_wind_audio{}; }
#endif
#if HK3DS_DIAGNOSTICS
extern "C" { volatile hk3ds::OpeningStreamProbe hk3ds_opening_stream{}; }
#endif
namespace hk3ds {
namespace {
bool optionalOneShot(unsigned index)
{
    // The first three clips are used by Tutorial_01 and the map transition
    // path. False Knight clips are a separately converted package and may be
    // absent from a presentation build; a present but malformed file remains
    // an admission error.
    return index >= static_cast<unsigned>(WindAudioPlayer::OneShot::KnightArmour)&&
        index <= static_cast<unsigned>(WindAudioPlayer::OneShot::KnightLand);
}
bool filePresent(const char* path)
{
    auto* file=std::fopen(path,"rb");
    if(!file)return false;
    std::fclose(file);return true;
}
}
static_assert(WindCue::ChannelCount==windChannelCount,"reserved wind channels");
static_assert(WindAudioPlayer::FirstChannel+WindCue::ChannelCount<=ndspChannelCount,"NDSP channel capacity");
static_assert(windStreamStartCacheBytes==WindCue::ChannelCount*windStreamChunkBytes,"one cached initial chunk per wind");
static_assert(windStreamBufferBytes==WindCue::ChannelCount*WindAudioPlayer::BufferCount*windStreamChunkBytes,"wind allocation");
static_assert(openingStreamBufferBytes==WindAudioPlayer::BufferCount*windStreamChunkBytes,"opening stream allocation");
static_assert(WindAudioPlayer::streamSlot(WindAudioPlayer::OneShot::Opening)!=WindAudioPlayer::streamSlot(WindAudioPlayer::OneShot::Secret),"discovery must not cut off the opening sting");
static_assert(secretStreamBufferBytes==openingStreamBufferBytes,"equal one-shot queue partitions");
static_assert(sizeof(WindAudioProbe)==124,"wind probe v4 wire size");
static_assert(sizeof(OpeningStreamProbe)==24,"opening stream probe wire size");

bool WindAudioPlayer::load(const char* directory)
{
    release();error_=0;command_=0;quit_=false;
    std::snprintf(sceneDirectory_,sizeof(sceneDirectory_),"%s",directory);
    if constexpr(diagnosticsEnabled)*const_cast<WindAudioProbe*>(&hk3ds_wind_audio)=WindAudioProbe{};
    if constexpr(diagnosticsEnabled)*const_cast<OpeningStreamProbe*>(&hk3ds_opening_stream)=OpeningStreamProbe{};
    // Immutable prefixes and open handles cover every streamed gameplay cue.
    // Cached prefixes use ordinary heap; linear queues/channels are unchanged.
    oneShotStartCache_.reset(new(std::nothrow) unsigned char[oneShotStartCacheBytes]);
    if(!oneShotStartCache_){error_=-21;release();return false;}
    constexpr const char* files[]={"S75_Opening_Sting-08","break_wall_after_tutorial_area","secret_discovered_temp",
        "false_knight_damage_armour","false_knight_head_damage_2","false_knight_jump","false_knight_strike_ground",
        "Fknight_daze","FKnight_Rage","FKnight_death","false_knight_swing","Fknight_flump_01","hero_death_extra_details"};
    static_assert(sizeof(files)/sizeof(*files)==static_cast<unsigned>(OneShot::Count),"one-shot clip table");
    static_assert(oneShotStartCacheBytes==static_cast<unsigned>(OneShot::Count)*openingStreamBufferBytes,"one-shot cache size");
    for(unsigned i=0;i<oneShotPcm_.size();++i){
        char path[384];std::snprintf(path,sizeof(path),"%s/sfx-enemy-%s.wav",directory,files[i]);
        auto& pcm=oneShotPcm_[i];
        if(!pcm.openWave(path)){
            if(optionalOneShot(i)&&!filePresent(path))continue;
            error_=-24;release();return false;
        }
        if(!pcm.cachePrefix(oneShotStartCache_.get()+i*openingStreamBufferBytes,
            std::min(openingStreamBufferBytes,static_cast<std::size_t>(pcm.frames())*4))){
            error_=-24;release();return false;
        }
    }
    oneShots_[0].frames=oneShotPcm_[0].frames();oneShots_[1].frames=oneShotPcm_[2].frames();
    constexpr auto streamBytes=windStreamBufferBytes+openingStreamBufferBytes+secretStreamBufferBytes;
    if(linearSpaceFree()<streamBytes+enemyAudioLinearReserveBytes){error_=-21;release();return false;}
    memory_=linearAlloc(streamBytes);
    if(!memory_){error_=-21;release();return false;}
    for(unsigned i=0;i<streams_.size();++i)for(unsigned j=0;j<BufferCount;++j)
        streams_[i].buffers[j].data_vaddr=static_cast<char*>(memory_)+(i*BufferCount+j)*windStreamChunkBytes;
    startCache_.reset(new(std::nothrow) unsigned char[windStreamStartCacheBytes]);
    if(!startCache_){error_=-21;release();return false;}
    // Use the same admission path as room changes: an unused queue needs no file.
    if(!selectScene(SceneAudioId::Tutorial)){release();return false;}
    if constexpr(diagnosticsEnabled){hk3ds_opening_stream.bufferBytes=openingStreamBufferBytes+secretStreamBufferBytes;}
    // Same application core, higher priority than rendering; asleep except for PCM
    // copies/refills. No Vorbis decoder, render locks or hot-path allocations.
    s32 priority=0;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    acceptingOneShots_=true;
    thread_=threadCreate(entry,this,windStreamStackBytes,priority-1,-2,false);
    if(!thread_){error_=-22;release();return false;}
    return true;
}
void WindAudioPlayer::stop(unsigned i)
{
    auto& s=streams_[i];ndspChnReset(FirstChannel+i);
    s.active=false;s.next=0;
    for(auto& b:s.buffers){auto* data=b.data_vaddr;b={};b.data_vaddr=data;}
}
bool WindAudioPlayer::fill(unsigned i,unsigned j)
{
    auto& s=streams_[i];auto& b=s.buffers[j];
    auto* destination=static_cast<char*>(memory_)+(i*BufferCount+j)*windStreamChunkBytes;
    const bool read=s.pcm.read(destination,windStreamChunkBytes);publishIo(i);
    if(!read){error_=-23;return false;}
    DSP_FlushDataCache(b.data_vaddr,windStreamChunkBytes);
    b.nsamples=windStreamChunkBytes/4;b.looping=false;
    ndspChnWaveBufAdd(FirstChannel+i,&b);if constexpr(diagnosticsEnabled){++hk3ds_wind_audio.submitted[i];}return true;
}
bool WindAudioPlayer::selectScene(SceneAudioId id)
{
    const auto& profile=sceneAudioProfile(id);
    for(unsigned i=0;i<streams_.size();++i){
        auto& s=streams_[i];s.pcm.close();s.reportedFailures=s.reportedRecoveries=0;
        if(!(profile.mask&(1u<<i)))continue;
        if(!s.pcm.openScene(sceneDirectory_,id,i)||
           !s.pcm.cachePrefix(startCache_.get()+i*windStreamChunkBytes,windStreamChunkBytes)){
            error_=-20;return false;
        }
    }
    return true;
}
void WindAudioPlayer::run()
{
    auto& p=hk3ds_wind_audio;unsigned previous=~0u;bool failed=false;
    auto last=osGetTime();const auto began=last;
    while(!quit_.load()){
        const auto tick=diagnosticsEnabled?svcGetSystemTick():0;const auto now=osGetTime();
        if constexpr(diagnosticsEnabled){++p.sequence;p.ready=1;p.bufferBytes=windStreamBufferBytes;}
        const auto command=command_.load();
        const auto id=sceneAudioCommandId(command);
        if(previous==~0u||!sceneAudioKeepsPhase(sceneAudioCommandId(previous),id)){
            for(unsigned i=0;i<streams_.size();++i)stop(i);
            failed=!selectScene(id);
            cue_.reset();if constexpr(diagnosticsEnabled){++p.resets;}
        }
        // Advance the previous transition before applying a new cue.
        cue_.advance(static_cast<float>(now-last)/1000.0f);last=now;
        cue_.scene(sceneAudioCommandEpoch(command),(command&1)!=0,id);previous=command;
        if constexpr(diagnosticsEnabled){p.activeMask=0;}
        std::array<bool,WindCue::ChannelCount> starting{};
        for(unsigned i=0;i<streams_.size()&&!failed;++i){
            auto& s=streams_[i];const auto& logical=cue_.channels()[i];const auto channel=FirstChannel+i;
            if(!logical.playing){if(s.active)stop(i);continue;}
            const auto levels=volumes_.load();
            const bool music=(sceneAudioProfile(id).musicMask&(1u<<i))!=0;
            const float volume=float(music?levels>>8:levels&255u)*.1f;
            const float gain=logical.gain*sceneAudioProfile(id).gains[i]*volume;
            float mix[12]{};mix[0]=mix[1]=gain;
            audioEvidence.configure(channel,200+i,windStreamChunkBytes/4,s.pcm.sampleRate(),gain,true,!s.active);
            if(!s.active){
                stop(i);const bool rewound=s.pcm.rewind();publishIo(i);
                if(!rewound){error_=-23;failed=true;break;}
                ndspChnReset(channel);ndspChnSetInterp(channel,NDSP_INTERP_POLYPHASE);
                ndspChnSetFormat(channel,NDSP_FORMAT_STEREO_PCM16);ndspChnSetRate(channel,s.pcm.sampleRate());
                ndspChnSetMix(channel,mix);
                // The cached first chunk starts every stem without file I/O.
                // Fill the remaining 186 ms only after all stems have started.
                ndspChnSetPaused(channel,true);
                if(!fill(i,0)){failed=true;break;}
                ndspChnSetPaused(channel,false);s.active=true;starting[i]=true;
            }else{
                bool allDone=true;
                for(auto& b:s.buffers){
                    allDone &= b.status==NDSP_WBUF_DONE;
                    if(b.status==NDSP_WBUF_PLAYING)if constexpr(diagnosticsEnabled){++p.playing[i];}
                }
                if(allDone)if constexpr(diagnosticsEnabled){++p.underruns;}
                // Requeue in playback order even if multiple buffers completed.
                for(unsigned count=0;count<BufferCount&&s.buffers[s.next].status==NDSP_WBUF_DONE;++count){
                    if(!fill(i,s.next)){failed=true;break;}s.next=(s.next+1)%BufferCount;
                }
                ndspChnSetMix(channel,mix);
            }
            if constexpr(diagnosticsEnabled){p.wraps[i]=s.pcm.wraps();p.activeMask|=1u<<i;}
        }
        for(unsigned i=0;i<streams_.size()&&!failed;++i)if(starting[i])
            for(unsigned j=1;j<BufferCount;++j)if(!fill(i,j)){failed=true;break;}
        serviceOneShot();
        const auto micros=static_cast<unsigned>(((diagnosticsEnabled?svcGetSystemTick():0)-tick)*1000000/SYSCLOCK_ARM11);
        if constexpr(diagnosticsEnabled){p.serviceMicros+=micros;if(micros>p.maxServiceMicros)p.maxServiceMicros=micros;}
        if constexpr(diagnosticsEnabled){p.elapsedMillis=now-began;p.error=error_.load();}
        if(failed){if constexpr(diagnosticsEnabled){p.ready=0;p.activeMask=0;}for(unsigned i=0;i<streams_.size();++i)stop(i);}
        if constexpr(diagnosticsEnabled){++p.sequence;p.sequenceEnd=p.sequence;}
        if(failed)break;
        svcSleepThread(5000000);
    }
    cancelOneShots();
    for(unsigned i=0;i<streams_.size();++i)stop(i);
    if constexpr(diagnosticsEnabled){++p.sequence;p.stopped=1;p.activeMask=0;p.error=error_.load();++p.sequence;p.sequenceEnd=p.sequence;}
}
void WindAudioPlayer::publishIo(unsigned channel)
{
    if constexpr(!diagnosticsEnabled)return;
    auto& s=streams_[channel];auto& p=hk3ds_wind_audio;
    const auto failures=s.pcm.ioFailures(),recoveries=s.pcm.recoveries();
    if constexpr(diagnosticsEnabled){p.recoveries+=recoveries-s.reportedRecoveries;}s.reportedRecoveries=recoveries;
    if(failures==s.reportedFailures)return;
    if constexpr(diagnosticsEnabled){p.ioFailures+=failures-s.reportedFailures;}s.reportedFailures=failures;
    const auto& f=s.pcm.failure();
    if constexpr(diagnosticsEnabled){p.failureStream=channel;p.failureOperation=f.operation;p.failurePosition=f.position;}
    if constexpr(diagnosticsEnabled){p.failureRequested=f.requested;p.failureReceived=f.received;p.failureErrno=f.error;p.failureFlags=f.flags;}
}
void WindAudioPlayer::release()
{
    quit_=true;cancelOneShots();
    if(thread_){threadJoin(thread_,U64_MAX);threadFree(thread_);thread_=nullptr;}
    for(auto& pcm:oneShotPcm_)pcm.close();
    for(auto& shot:oneShots_)shot.frames=0;
    for(auto& s:streams_){s.pcm.close();s.buffers={};s.active=false;s.next=0;s.reportedFailures=s.reportedRecoveries=0;}
    startCache_.reset();oneShotStartCache_.reset();
    if(memory_){linearFree(memory_);memory_=nullptr;}cue_.reset();
    if constexpr(diagnosticsEnabled){hk3ds_wind_audio.error=error_.load();}
}
bool WindAudioPlayer::fillOneShot(unsigned slot,unsigned index,unsigned generation,WindPcmStream& pcm)
{
    if(!pcm.remainingFrames())return true;
    auto* destination=static_cast<char*>(memory_)+windStreamBufferBytes+slot*openingStreamBufferBytes+index*windStreamChunkBytes;
    unsigned frames=0;
    // Only this worker writes PCM. Start/stop can invalidate this read but never
    // queue its memory. No file I/O may hold the gameplay-facing lock.
    const bool ok=pcm.readOnce(destination,windStreamChunkBytes,frames);
    LightLock_Lock(&oneShotLock_);auto& shot=oneShots_[slot];
    if(shot.generation!=generation){LightLock_Unlock(&oneShotLock_);return false;}
    if(!ok){
        error_=-25;if constexpr(diagnosticsEnabled){hk3ds_opening_stream.error=-25;}
        stopOneShotLocked(slot);shot.status=NDSP_WBUF_DONE;
        LightLock_Unlock(&oneShotLock_);return false;
    }
    auto& b=shot.buffers[index];
    b={};b.data_vaddr=destination;b.nsamples=frames;
    DSP_FlushDataCache(destination,frames*4);
    ndspChnWaveBufAdd(shot.channel,&b);if constexpr(diagnosticsEnabled){hk3ds_opening_stream.submittedFrames+=frames;}
    LightLock_Unlock(&oneShotLock_);return true;
}
bool WindAudioPlayer::startOneShot(unsigned channel,OneShot clip)
{
    if(channel>=ndspChannelCount||static_cast<unsigned>(clip)>=oneShotPcm_.size())return false;
    if(!oneShotPcm_[static_cast<unsigned>(clip)].frames())return false;
    LightLock_Lock(&oneShotLock_);
    const bool ok=acceptingOneShots_&&!quit_.load();
    if(ok){
        const unsigned slot=streamSlot(clip);stopOneShotLocked(slot);auto& shot=oneShots_[slot];
        shot.channel=channel;shot.clip=clip;shot.starting=true;
        shot.frames=oneShotPcm_[static_cast<unsigned>(clip)].frames();
        shot.status=NDSP_WBUF_QUEUED;
        ndspChnSetPaused(channel,true);
    }
    LightLock_Unlock(&oneShotLock_);
    return ok;
}
void WindAudioPlayer::stopOneShotLocked(unsigned slot)
{
    auto& shot=oneShots_[slot];++shot.generation;shot.starting=false;shot.next=0;
    if(shot.channel<ndspChannelCount){ndspChnWaveBufClear(shot.channel);ndspChnSetPaused(shot.channel,false);}
    shot.channel=ndspChannelCount;shot.buffers={};shot.status=NDSP_WBUF_FREE;
}
void WindAudioPlayer::stopOneShot(unsigned slot)
{
    if(slot>=oneShots_.size())return;
    LightLock_Lock(&oneShotLock_);stopOneShotLocked(slot);
    LightLock_Unlock(&oneShotLock_);
}
void WindAudioPlayer::cancelOneShots()
{
    LightLock_Lock(&oneShotLock_);acceptingOneShots_=false;
    for(unsigned slot=0;slot<oneShots_.size();++slot){stopOneShotLocked(slot);oneShots_[slot].status=NDSP_WBUF_DONE;}
    LightLock_Unlock(&oneShotLock_);
}
void WindAudioPlayer::serviceOneShot()
{
    for(unsigned slot=0;slot<oneShots_.size();++slot){
        LightLock_Lock(&oneShotLock_);
        auto& shot=oneShots_[slot];
        const auto generation=shot.generation;
        const auto clip=shot.clip;const bool starting=shot.starting;shot.starting=false;
        const bool active=shot.channel<ndspChannelCount&&shot.status!=NDSP_WBUF_DONE;
        LightLock_Unlock(&oneShotLock_);
        if(!active)continue;
        auto& pcm=oneShotPcm_[static_cast<unsigned>(clip)];
        if(starting){
            const bool rewound=pcm.rewind(); // Prefix rewind is normally memory-only.
            if(!rewound){
                LightLock_Lock(&oneShotLock_);
                if(shot.generation==generation){
                    error_=-25;if constexpr(diagnosticsEnabled){hk3ds_opening_stream.error=-25;}
                    stopOneShotLocked(slot);shot.status=NDSP_WBUF_DONE;
                }
                LightLock_Unlock(&oneShotLock_);continue;
            }
            bool ok=true;
            for(unsigned i=0;ok&&i<BufferCount;++i)ok=fillOneShot(slot,i,generation,pcm);
            LightLock_Lock(&oneShotLock_);
            if(ok&&shot.generation==generation){
                ndspChnSetPaused(shot.channel,false);
                if constexpr(diagnosticsEnabled){++hk3ds_opening_stream.plays;}
            }
            LightLock_Unlock(&oneShotLock_);continue;
        }
        LightLock_Lock(&oneShotLock_);
        if(shot.generation==generation){
            bool pending=false,playing=false;
            for(const auto& b:shot.buffers){
                pending|=b.status==NDSP_WBUF_QUEUED||b.status==NDSP_WBUF_PLAYING;
                playing|=b.status==NDSP_WBUF_PLAYING;
            }
            if(playing)shot.status=NDSP_WBUF_PLAYING;
            if(!pending){
                if(pcm.remainingFrames()){if constexpr(diagnosticsEnabled){++hk3ds_opening_stream.underruns;}}
                else{shot.status=NDSP_WBUF_DONE;if constexpr(diagnosticsEnabled){++hk3ds_opening_stream.completed;}}
            }
        }
        LightLock_Unlock(&oneShotLock_);
        for(unsigned count=0;count<BufferCount&&pcm.remainingFrames();++count){
            LightLock_Lock(&oneShotLock_);const auto next=shot.next;
            const bool refill=shot.generation==generation&&shot.buffers[next].status==NDSP_WBUF_DONE;
            LightLock_Unlock(&oneShotLock_);
            if(!refill||!fillOneShot(slot,next,generation,pcm))break;
            LightLock_Lock(&oneShotLock_);
            if(shot.generation==generation)shot.next=(next+1)%BufferCount;
            LightLock_Unlock(&oneShotLock_);
        }
    }
}
}
