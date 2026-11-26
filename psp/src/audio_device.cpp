#include "psp_audio.hpp"
#include "audio.hpp"
#include "perf.hpp"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#ifdef __PSP__
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#endif

namespace hkpsp {
namespace {
unsigned micros(){
#ifdef __PSP__
    return sceKernelGetSystemTimeLow();
#else
    return unsigned(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
constexpr unsigned ringFrames=16384,ioBlock=8192,outputFrames=512,queueSize=128;
// One I/O worker owns these aligned transfer bytes; they are never published.
// Static storage. Ring 16384 frames (0.5 s), block 8192: halves 15 ambience rings (-960 KB heap, heap-trim).
#ifdef HKPSP_PERF
alignas(64) unsigned char audioTransfer[262144+64];
#else
alignas(64) unsigned char audioTransfer[ioBlock*4+audioSectorBytes];
#endif
#ifdef __PSP__
#ifdef HKPSP_PERF
struct IoCost {unsigned wall=0,cpu=0;bool error=false;};
class IoTimer {
    SceKernelThreadRunStatus before_{};int thread_,result_;unsigned started_;
public:
    IoTimer():thread_(sceKernelGetThreadId()){
        before_.size=sizeof(before_);result_=sceKernelReferThreadRunStatus(thread_,&before_);started_=micros();
    }
    IoCost stop(){
        const unsigned wall=micros()-started_;
        SceKernelThreadRunStatus after{};after.size=sizeof(after);
        const bool error=sceKernelReferThreadRunStatus(thread_,&after)<0||result_<0;
        return {wall,error?0:after.runClocks.low-before_.runClocks.low,error};
    }
};
// Raw SDK measurements use a separate handle, before either audio worker or
// frame timing starts. Setup seeks are reported separately from measured seeks.
void rawAudioProbe(const char* path,const std::string& output){
    FILE* csv=std::fopen((output+"/io-probe.csv").c_str(),"wb");
    if(!csv)throw std::runtime_error("Audio raw probe output");
    std::fprintf(csv,"offset,buffer_offset,bytes,with_seek,open_calls,open_us,open_cpu_us,setup_seek_us,setup_seek_cpu_us,seek_calls,seek_us,seek_cpu_us,read_calls,read_bytes,read_us,read_cpu_us,close_us,close_cpu_us,cpu_probe_errors,io_errors\n");
    for(unsigned offset:{0u,32u,512u})for(unsigned buffer:{0u,8u})
    for(unsigned bytes:{8192u,65536u,262144u})for(unsigned seek:{0u,1u}){
        unsigned errors=0,probeErrors=0,seeks=0,seekUs=0,seekCpu=0,reads=0,readBytes=0,readUs=0,readCpu=0;
        IoTimer opening;const int file=sceIoOpen(path,PSP_O_RDONLY,0);const auto opened=opening.stop();
        probeErrors+=opened.error;errors+=file<0;
        IoCost setup{},closed{};
        if(file>=0){
            IoTimer positioning;const int pos=sceIoLseek32(file,int(offset),PSP_SEEK_SET);setup=positioning.stop();
            probeErrors+=setup.error;errors+=pos!=int(offset);
            for(unsigned i=0;i<4;++i){
                if(seek){
                    IoTimer positioningRead;
                    const int result=sceIoLseek32(file,int(offset+i*bytes),PSP_SEEK_SET);
                    const auto cost=positioningRead.stop();
                    ++seeks;seekUs+=cost.wall;seekCpu+=cost.cpu;probeErrors+=cost.error;errors+=result!=int(offset+i*bytes);
                }
                IoTimer reading;const int result=sceIoRead(file,audioTransfer+buffer,bytes);const auto cost=reading.stop();
                ++reads;readBytes+=result>0?unsigned(result):0;readUs+=cost.wall;readCpu+=cost.cpu;
                probeErrors+=cost.error;errors+=result!=int(bytes);
            }
            IoTimer closing;const int result=sceIoClose(file);closed=closing.stop();probeErrors+=closed.error;errors+=result<0;
        }
        std::fprintf(csv,"%u,%u,%u,%u,1,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            offset,buffer,bytes,seek,opened.wall,opened.cpu,setup.wall,setup.cpu,seeks,seekUs,seekCpu,
            reads,readBytes,readUs,readCpu,closed.wall,closed.cpu,probeErrors,errors);
    }
    const bool failed=std::ferror(csv)!=0;const int result=std::fclose(csv);
    if(failed||result)throw std::runtime_error("Audio raw probe write");
}
#endif
enum class IoOperation {Open,Close,Seek,Read};
template<class Call> int audioCall(IoOperation operation,Call call){
    HKPSP_PROFILE(IoTimer timer;)
    const int result=call();
    HKPSP_PROFILE(const auto cost=timer.stop();
        if(operation==IoOperation::Read)perf::audioRead(result>0?unsigned(result):0,cost.wall,cost.cpu,cost.error);
        else {
            constexpr perf::Column columns[]={perf::AudioOpens,perf::AudioCloses,perf::AudioSeeks};
            perf::audioOperation(columns[unsigned(operation)],cost.wall,cost.cpu,cost.error);
        })
    (void)operation;return result;
}
#endif
// Handles are admitted before worker startup and retained for the room. Only
// failed reads close their own handle; interleaved clips cannot reopen each other.
class AudioFile {
    struct Entry {
        const AudioClip* clip=nullptr;unsigned position=0,used=0;
#ifdef __PSP__
        int file=-1;
#else
        FILE* file=nullptr;
#endif
    };
    std::array<Entry,audioClipCount> files_{};
    const AudioClip* previous_=nullptr;unsigned tick_=0;
    // Production clips share one HPK1 handle, opened at admission. Keep the
    // eight-file LRU bound for standalone loose-HPA1 tools and synthetic banks.
    static constexpr unsigned maxOpen=8;
    unsigned openCount_=0;
    bool isOpen(const Entry& entry)const{
#ifdef __PSP__
        return entry.file>=0;
#else
        return entry.file!=nullptr;
#endif
    }
#ifdef __PSP__
    int event_=-1;
#endif
    void close(Entry& entry){
        if(isOpen(entry))--openCount_;
#ifdef __PSP__
        if(entry.file>=0)audioCall(IoOperation::Close,[&]{return sceIoClose(entry.file);});
        entry.file=-1;
#else
        if(entry.file)std::fclose(entry.file);
        entry.file=nullptr;
#endif
        entry.position=0;
    }
    Entry* admit(const AudioClip& clip){
        Entry* empty=nullptr;
        for(auto& entry:files_){if(entry.clip&&entry.clip->path==clip.path)return &entry;if(!entry.clip&&!empty)empty=&entry;}
        if(empty)empty->clip=&clip;
        return empty;
    }
    bool open(Entry& entry){
        entry.used=++tick_;
        if(isOpen(entry))return true;
        if(!isOpen(entry)){
            unsigned count=0;Entry* oldest=nullptr;
            for(auto& e:files_)if(isOpen(e)){++count;if(!oldest||e.used<oldest->used)oldest=&e;}
            if(count>=maxOpen&&oldest)close(*oldest);
        }
#ifdef __PSP__
        if(entry.file<0)entry.file=audioCall(IoOperation::Open,[&]{return sceIoOpen(entry.clip->path.c_str(),PSP_O_RDONLY,0);});
#else
        if(!entry.file){entry.file=std::fopen(entry.clip->path.c_str(),"rb");if(entry.file)std::setvbuf(entry.file,nullptr,_IONBF,0);}
#endif
        if(!isOpen(entry))return false;
        peakOpenFiles=std::max(peakOpenFiles,++openCount_);
        if(gameFrame.load(std::memory_order_relaxed)>5)++opensAfterFrame5;
        return true;
    }
public:
    std::atomic<unsigned> gameFrame{0};
    unsigned peakOpenFiles=0,opensAfterFrame5=0;
    // Last failed I/O for the worker failure report (run 6 combat: "-1" alone
    // could not tell open/seek/read or the SCE error apart).
    std::atomic<int> failResult{0};std::atomic<unsigned> failOperation{0},failBytes{0},failOffset{0},failFrame{0};
    void noteFailure(unsigned operation,int result,unsigned bytes,unsigned offset){
        failOperation.store(operation);failResult.store(result);failBytes.store(bytes);failOffset.store(offset);
        failFrame.store(gameFrame.load(std::memory_order_relaxed));
    }
    std::string failure()const{
        static const char* names[]={"none","admit","open","seek","read"};
        return std::string(" (last I/O failure: ")+names[std::min(failOperation.load(),4u)]+" result="+
            std::to_string(failResult.load())+" bytes="+std::to_string(failBytes.load())+" offset="+
            std::to_string(failOffset.load())+" frame="+std::to_string(failFrame.load())+")";
    }
    ~AudioFile(){close();
#ifdef __PSP__
        if(event_>=0)sceKernelDeleteEventFlag(event_);
#endif
    }
    AudioFile()=default;
    AudioFile(const AudioFile&)=delete;
    AudioFile& operator=(const AudioFile&)=delete;
    bool prepare(const AudioClip& clip){auto* entry=admit(clip);return entry&&open(*entry);}
    void close(){for(auto& entry:files_)close(entry);previous_=nullptr;}
    void wake(){
#ifdef __PSP__
        if(event_>=0)sceKernelSetEventFlag(event_,1);
#endif
    }
#ifdef __PSP__
    void start(){
        event_=sceKernelCreateEventFlag("HKPSP audio refill",0,1,nullptr);
        if(event_<0)throw std::runtime_error("Audio refill event");
    }
    int wait(){return sceKernelWaitEventFlag(event_,1,PSP_EVENT_WAITCLEAR,nullptr,nullptr);}
#endif
    bool read(const AudioClip& clip,unsigned frame,std::int16_t* pcm,unsigned frames,
              unsigned firstFrames=~0u,std::int16_t* wrapped=nullptr){
        auto* entry=admit(clip);
        if(!entry||frames>ioBlock){noteFailure(1,0,frames,0);return false;}
        if(!open(*entry)){
#ifdef __PSP__
            noteFailure(2,entry->file,0,0);
#else
            noteFailure(2,-1,0,0);
#endif
            return false;
        }
        if(previous_!=&clip){HKPSP_PROFILE(perf::audioAdd(perf::AudioClipChanges);)previous_=&clip;}
        const unsigned stride=clip.channels*2,offset=clip.pcmOffset+frame*stride;
        const unsigned aligned=offset/audioSectorBytes*audioSectorBytes,prefix=offset-aligned;
        const unsigned bytes=(prefix+frames*stride+audioSectorBytes-1)/audioSectorBytes*audioSectorBytes;
        bool ok=true;
        if(entry->position!=aligned){
#ifdef __PSP__
            const int sought=audioCall(IoOperation::Seek,[&]{return sceIoLseek32(entry->file,int(aligned),PSP_SEEK_SET);});
            ok=sought==int(aligned);
            if(!ok)noteFailure(3,sought,0,aligned);
#else
            ok=std::fseek(entry->file,long(aligned),SEEK_SET)==0;
#endif
            if(ok)entry->position=aligned;
        }
        if(ok){
#ifdef __PSP__
            const int count=audioCall(IoOperation::Read,[&]{return sceIoRead(entry->file,audioTransfer,bytes);});
#else
            HKPSP_PROFILE(const unsigned started=micros();)
            const int count=int(std::fread(audioTransfer,1,bytes,entry->file));
            HKPSP_PROFILE(perf::audioRead(count>0?unsigned(count):0,micros()-started,0);)
#endif
            if(count==int(bytes)){
                entry->position+=bytes;
                const unsigned first=std::min(frames,firstFrames);
                std::memcpy(pcm,audioTransfer+prefix,first*stride);
                if(first<frames)std::memcpy(wrapped,audioTransfer+prefix+first*stride,(frames-first)*stride);
                return true;
            }
            noteFailure(4,count,bytes,aligned);
        }
        close(*entry);return false;
    }
};
#include "audio_file_stream.inc"
// Pages are immutable from release-publication until the mixer releases them.
// Only the mixer retires pages; only I/O writes them. A stale I/O request can
// publish an unwanted page, but can never replace a page being sampled.
class TailStream final:public AudioTail {
    struct Page {
        std::atomic<bool> ready{false};unsigned key=~0u;
        std::array<std::int16_t,audioTailPageFrames*2> pcm{};
    };
    const std::array<AudioClip,audioClipCount>& clips_;
    std::array<Page,audioTailPages> pages_{};
    std::atomic<unsigned> wanted_{~0u};unsigned clip_=~0u;
    AudioFile& file_;
public:
    std::atomic<bool> failed{false};unsigned underruns=0,reopens=0;
    TailStream(const std::array<AudioClip,audioClipCount>& clips,AudioFile& file):clips_(clips),file_(file){}
    void prepare(unsigned clip,unsigned frame)override{
        clip_=clip;
        const unsigned page=frame>audioHeadFrames?(frame-audioHeadFrames)/audioTailPageFrames:0;
        const unsigned key=clip*1024+page;
        bool changed=wanted_.load(std::memory_order_relaxed)!=key;
        wanted_.store(key,std::memory_order_release);
        for(auto& p:pages_)if(p.ready.load(std::memory_order_acquire)&&
            (p.key<key||p.key>=key+audioTailPages)){p.ready.store(false,std::memory_order_release);changed=true;}
        HKPSP_PROFILE(unsigned ready=0;
            for(const auto& p:pages_)if(p.ready.load(std::memory_order_acquire)&&p.key>=key&&p.key<key+audioTailPages)++ready;
            perf::audioLow(perf::AudioTailLow,ready);)
        // Signal after retirement: a worker preempting before then could find
        // no free page. Repeated prepares also retire a late stale publication.
        if(changed)file_.wake();
#ifndef __PSP__
        for(unsigned i=0;i<audioTailPages;++i)pump();
#endif
    }
    bool pump(){
        const unsigned wanted=wanted_.load(std::memory_order_acquire);
        if(wanted==~0u||failed.load())return false;
        const unsigned clip=wanted/1024;const auto& c=clips_[clip];
        for(unsigned key=wanted;key<wanted+audioTailPages;++key){
            const unsigned frame=audioHeadFrames+(key%1024)*audioTailPageFrames;
            if(frame>=c.frames)return false;
            bool found=false;Page* empty=nullptr;
            for(auto& p:pages_){
                if(p.ready.load(std::memory_order_acquire)){if(p.key==key)found=true;}
                else empty=&p;
            }
            if(found)continue;
            if(!empty)return false;
            const unsigned n=std::min(audioTailPageFrames,c.frames-frame);
            bool ok=false;
            for(unsigned attempt=0;attempt<3;++attempt){
                if(file_.read(c,frame,empty->pcm.data(),n)){ok=true;break;}
                ++reopens;
            }
            if(!ok){failed.store(true);return false;}
            empty->key=key;empty->ready.store(true,std::memory_order_release);
            return true; // Round-robin across voices before doing more file work.
        }
        return false;
    }
    const std::int16_t* page(unsigned page)override{
        const unsigned key=clip_*1024+page;
        for(auto& p:pages_)if(p.ready.load(std::memory_order_acquire)&&p.key==key)return p.pcm.data();
        ++underruns;HKPSP_PROFILE(perf::audioUnderrun(true);)return nullptr;
    }
};
#ifndef __PSP__
void le32(FILE* f,unsigned x){for(unsigned i=0;i<4;++i)std::fputc(int((x>>(8*i))&255),f);}
void wavHeader(FILE* f,unsigned frames){
    std::rewind(f);std::fwrite("RIFF",1,4,f);le32(f,36+frames*4);std::fwrite("WAVEfmt ",1,8,f);
    le32(f,16);le32(f,0x20001);le32(f,audioRate);le32(f,audioRate*4);le32(f,0x100004);
    std::fwrite("data",1,4,f);le32(f,frames*4);
}
#endif
struct Packet {unsigned frame=0;AudioState state;};
struct MixCost {unsigned frames,us;};
// Mixer produces; the log thread (PSP) or the main thread (host) writes diagnostics.
// Never retain a whole tape.
struct CostLog {
    static constexpr unsigned capacity=4096;
    std::array<MixCost,capacity> rows{};
    std::atomic<unsigned> head{0},tail{0};
    unsigned logged=0;
    bool push(MixCost row){
        const unsigned h=head.load(std::memory_order_relaxed),t=tail.load(std::memory_order_acquire);
        if(h-t>=capacity)return false;
        rows[h%capacity]=row;head.store(h+1,std::memory_order_release);return true;
    }
    void drain(FILE* file){
        unsigned t=tail.load(std::memory_order_relaxed);
        const unsigned h=head.load(std::memory_order_acquire);
        while(t!=h){const auto row=rows[t%capacity];
            std::fprintf(file,"%u,%u,%u\n",logged++,row.frames,row.us);
            tail.store(++t,std::memory_order_release);
        }
    }
};
class Device final:public AudioDevice {
    std::array<AudioClip,audioClipCount> clips_{};
    AudioFile input_;
    std::array<std::unique_ptr<FileStream>,audioClipCount> streams_{};
    std::array<AudioStream*,audioClipCount> readers_{};
    std::array<std::unique_ptr<TailStream>,audioVoiceCount> tails_{};
    std::array<AudioTail*,audioVoiceCount> tailReaders_{};
    std::uint32_t tailVoices_=(1u<<audioVoiceCount)-1;
    AudioMixer mixer_{clips_};AudioState state_{};
    std::bitset<audioClipCount> musicClips_;
    unsigned musicVolume_=100,soundVolume_=100;
    std::array<std::uint64_t,audioVoiceCount> position_{};
    unsigned generation_=0,submitted_=0,applied_=0,totalSamples_=0,queuePeak_=0;
    unsigned long long mixUs_=0;unsigned mixBlocks_=0;
    std::string output_;bool diagnostic_=false,started_=false,finished_=false;
    std::unique_ptr<CostLog> costs_;
    FILE* wav_=nullptr,*voiceLog_=nullptr,*costLog_=nullptr;
#ifdef __PSP__
    std::array<Packet,queueSize> queue_{};
    std::atomic<unsigned> logged_{0};
    std::atomic<unsigned> head_{0},tail_{0};
    std::atomic<bool> quit_{false},logQuit_{false};std::atomic<int> error_{0};
    int mixThread_=-1,ioThread_=-1,logThread_=-1;bool reserved_=false;
    alignas(64) std::int16_t buffers_[2][outputFrames*2]{};
    static int mixEntry(SceSize,void* data){return (*static_cast<Device**>(data))->mixLoop();}
    static int ioEntry(SceSize,void* data){return (*static_cast<Device**>(data))->ioLoop();}
    static int logEntry(SceSize,void* data){return (*static_cast<Device**>(data))->logLoop();}
    // 2026-11-17 [M]: writing both logs from the game thread every frame (2b44d539) stalled
    // Device::frame for 17-23 ms in 6 of 600 combat frames on the PSP-3000 (run 8; run 7, with
    // the logs written at the end, had none). This thread runs below the game and audio I/O
    // threads, i.e. while the game waits; the queue overflow check still bounds the lag.
    int logLoop(){
        for(;;){
            const bool last=logQuit_.load(std::memory_order_acquire);
            drainPackets();
            if(std::ferror(voiceLog_)||std::ferror(costLog_)){error_.store(-3);return 0;}
            if(last)return 0;
            sceKernelDelayThread(4000);
        }
    }
    void drainPackets(){
        // tail publishes packets only after apply. Drain before reusing any slot.
        const unsigned t=tail_.load(std::memory_order_acquire);
        unsigned l=logged_.load(std::memory_order_relaxed);
        while(l!=t){logPacket(queue_[l%queueSize]);logged_.store(++l,std::memory_order_release);}
        costs_->drain(costLog_);
    }
    int ioLoop(){
        while(!quit_.load(std::memory_order_acquire)){
            const int result=input_.wait();
            if(result<0){error_.store(result);break;}
            if(quit_.load(std::memory_order_acquire))break;
            HKPSP_PROFILE(perf::audioWake();)
            bool work;
            do{
                work=false;
                for(auto& stream:streams_)if(stream){work=stream->pump()||work;if(stream->failed)error_.store(-1);}
                for(auto& tail:tails_)if(tail){work=tail->pump()||work;if(tail->failed)error_.store(-1);}
            }while(work&&!quit_.load(std::memory_order_acquire));
        }return 0;
    }
    int mixLoop(){
        unsigned buffer=0;
        while(!quit_.load(std::memory_order_acquire)||tail_.load()!=head_.load()){
            unsigned t=tail_.load(std::memory_order_relaxed),h=head_.load(std::memory_order_acquire);
            while(t!=h){const auto& packet=queue_[t%queueSize];mixer_.apply(packet.state);
                ++applied_;++t;tail_.store(t,std::memory_order_release);
            }
            if(!applied_){sceKernelDelayThread(1000);continue;}
            render(buffers_[buffer],outputFrames);
            const int result=sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX,buffers_[buffer]);
            if(result<0){error_.store(result);break;}
            buffer^=1;
        }return 0;
    }
    void join(){
        quit_.store(true,std::memory_order_release);
        input_.wake();
        for(int* id:{&mixThread_,&ioThread_})if(*id>=0){sceKernelWaitThreadEnd(*id,nullptr);sceKernelDeleteThread(*id);*id=-1;}
        // The mixer has applied every packet: the log thread's last pass drains them all.
        if(logThread_>=0){
            logQuit_.store(true,std::memory_order_release);
            sceKernelWaitThreadEnd(logThread_,nullptr);sceKernelDeleteThread(logThread_);logThread_=-1;
        }
        if(reserved_){
            // Both DMA buffers must have finished before their storage is freed.
            sceKernelDelayThread(2*outputFrames*1000000/audioRate);
            const int result=sceAudioSRCChRelease();if(result<0)error_.store(result);
            reserved_=false;
        }
    }
#endif
    void render(std::int16_t* pcm,unsigned frames){
        const unsigned before=micros();mixer_.render(pcm,frames,readers_.data(),tailReaders_.data());
        const unsigned us=micros()-before;mixUs_+=us;++mixBlocks_;totalSamples_+=frames;
        if(diagnostic_&&!costs_->push({frames,us})){
#ifdef __PSP__
            error_.store(-2);
#else
            throw std::runtime_error("Audio diagnostic cost overflow");
#endif
        }
    }
    void logPacket(const Packet& p){
        for(unsigned v=0;v<audioVoiceCount;++v){const auto& s=p.state[v];
            std::fprintf(voiceLog_,"%u,%u,%u,%u,%u,%d,%d,%u\n",p.frame,v,s.clip,s.generation,s.step,s.left,s.right,unsigned(s.loop));
        }
    }
    void drainDiagnostics(){
        if(!diagnostic_)return;
#ifdef __PSP__
        drainPackets();
#else
        costs_->drain(costLog_);
#endif
        if(std::ferror(voiceLog_)||std::ferror(costLog_))throw std::runtime_error("Audio diagnostic write");
    }
    void closeLog(FILE*& file){
        if(!file)return;
        const int result=std::fclose(file);file=nullptr;
        if(result)throw std::runtime_error("Audio diagnostic close");
    }
    FILE* open(const char* name){
        const auto path=output_+"/"+name;FILE* f=std::fopen(path.c_str(),"wb");
        if(!f)throw std::runtime_error("Audio output: "+path);
        return f;
    }
    void check(){
#ifdef __PSP__
        if(error_.load())throw std::runtime_error("Audio worker failure: "+std::to_string(error_.load())+input_.failure());
#endif
        for(const auto& stream:streams_)if(stream&&stream->failed)throw std::runtime_error("Audio stream failure");
        for(const auto& tail:tails_)if(tail&&tail->failed)throw std::runtime_error("Audio tail failure");
    }
public:
    Device(std::string output,bool diagnostic):output_(std::move(output)),diagnostic_(diagnostic){
        HKPSP_PROFILE(perf::preloadBytes=0;)
#ifdef __PSP__
        // Long page-I/O stress uses play-mode memory behavior: retain counters,
        // not minutes of voice packets/cost samples. Both audio workers stay on.
        SceIoStat stress{};
        if(sceIoGetstat("ms0:/PSP/GAME/HKPSP/data/page-upload-stress.cfg",&stress)>=0)diagnostic_=false;
#endif
    }
    ~Device()override{
#ifdef __PSP__
        join();
#endif
        if(wav_)std::fclose(wav_);
        if(voiceLog_)std::fclose(voiceLog_);
        if(costLog_)std::fclose(costLog_);
    }
    void load(unsigned id,const std::string& path,bool stream)override{
        if(started_||id>=clips_.size()||clips_[id].frames)throw std::runtime_error("Audio load state");
        clips_[id].load(path,stream);
        HKPSP_PROFILE(perf::preloadBytes+=clips_[id].frames*clips_[id].channels*2;)
        if((stream||clips_[id].tail())&&!input_.prepare(clips_[id]))throw std::runtime_error("Audio retained open: "+path);
        if(stream){streams_[id]=std::make_unique<FileStream>(clips_[id],input_);readers_[id]=streams_[id].get();}
    }
    const AudioClip& clip(unsigned id)const override{return clips_.at(id);}
    void play(unsigned voice,unsigned id,bool loop,float gain,float pitch,float pan)override{
        if(voice>=state_.size()||id>=clips_.size()||!clips_[id].frames)throw std::runtime_error("Unadmitted audio voice/clip");
        if(clips_[id].tail()&&!(tailVoices_&(1u<<voice)))throw std::runtime_error("Unreserved audio tail voice");
        if(clips_[id].stream&&(!loop||pitch!=1))throw std::runtime_error("Ambience streams require looping unity pitch");
        state_[voice]={};state_[voice].clip=id;state_[voice].loop=loop;state_[voice].generation=++generation_;position_[voice]=0;
        volumePan(voice,gain,pan,pitch);
    }
    void stop(unsigned voice)override{state_.at(voice).clip=~0u;}
    void volumePan(unsigned voice,float gain,float pan,float pitch)override{
        if(!std::isfinite(gain)||gain<0||gain>64||!std::isfinite(pan)||std::abs(pan)>1||!std::isfinite(pitch)||pitch<=0||pitch>8)
            throw std::runtime_error("Invalid audio mix");
        auto& s=state_.at(voice);
        if(s.clip!=~0u){gain*=clips_[s.clip].gain;pitch*=clips_[s.clip].pitch;}
        if(!std::isfinite(gain)||gain>64||!std::isfinite(pitch)||pitch<=0||pitch>8)
            throw std::runtime_error("Invalid clip audio mix");
        s.left=int(std::lround(gain*(1-pan)*hk3ds::tutorialAudioMasterGain*65536));
        s.right=int(std::lround(gain*(1+pan)*hk3ds::tutorialAudioMasterGain*65536));s.step=unsigned(std::lround(pitch*65536));
    }
    bool active(unsigned voice)const override{return state_.at(voice).clip!=~0u;}
    void tailVoices(std::uint32_t mask)override{
        if(started_||mask>>audioVoiceCount)throw std::runtime_error("Audio tail reservation state");
        tailVoices_=mask;
    }
    void start()override{
        if(started_)throw std::runtime_error("Audio already started");
        // A clean no-read reference must retain every admitted PCM sample: current admission
        // (20,675,968 B on the Tutorial bank) exceeds the heap cap, so that config is omitted and
        // perf::preloadBytes is reported in the manifest. Never throw here: small test banks fit
        // (the audio latency fixture terminated on this check after the run-5 merge).
#if defined(__PSP__) && defined(HKPSP_PERF)
        static bool probed=false;
        if(!probed)for(const auto& clip:clips_)if(clip.frames*clip.channels*2>=4*262144+512){
            rawAudioProbe(clip.path.c_str(),output_);probed=true;break;
        }
#endif
        if(std::any_of(clips_.begin(),clips_.end(),[](const AudioClip& c){return c.tail();}))
            for(unsigned i=0;i<audioVoiceCount;++i)if(tailVoices_&(1u<<i)){
                tails_[i]=std::make_unique<TailStream>(clips_,input_);tailReaders_[i]=tails_[i].get();
            }
        if(diagnostic_){
            costs_=std::make_unique<CostLog>();
            voiceLog_=open("audio-voices.csv");std::fprintf(voiceLog_,"frame,voice,clip,generation,step,left,right,loop\n");
            costLog_=open("audio-cost.csv");std::fprintf(costLog_,"block,frames,mix_us\n");
        }
        started_=true;
#ifdef __PSP__
        const int result=sceAudioSRCChReserve(outputFrames,audioRate,2);
        if(result<0)throw std::runtime_error("Audio SRC reserve: "+std::to_string(result));
        reserved_=true;
        input_.start();
        // Run 5: 0x22 preserved calm/combat audio while allowing main (0x20)
        // to finish its work before refill. Mixer (0x18) still has first call.
        mixThread_=sceKernelCreateThread("HKPSP mixer",mixEntry,0x18,65536,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,nullptr);
        const unsigned ioPriority=
#ifdef HKPSP_PERF
            cpu::config().ioPriority;
#else
            0x22;
#endif
        ioThread_=sceKernelCreateThread("HKPSP audio I/O",ioEntry,ioPriority,32768,PSP_THREAD_ATTR_USER,nullptr);
        if(mixThread_<0||ioThread_<0)throw std::runtime_error("Audio thread creation");
        HKPSP_PROFILE(perf::mixerThread=mixThread_;perf::ioThread=ioThread_;)
        Device* self=this;
        if(sceKernelStartThread(ioThread_,sizeof(self),&self)<0||sceKernelStartThread(mixThread_,sizeof(self),&self)<0)
            throw std::runtime_error("Audio thread start");
        if(diagnostic_){
            logThread_=sceKernelCreateThread("HKPSP audio log",logEntry,0x30,32768,PSP_THREAD_ATTR_USER,nullptr);
            if(logThread_<0||sceKernelStartThread(logThread_,sizeof(self),&self)<0)throw std::runtime_error("Audio log thread");
        }
#else
        wav_=open("audio.wav");wavHeader(wav_,0);
#endif
    }
    void musicClip(unsigned clip)override{musicClips_.set(clip);}
    void volumes(unsigned music,unsigned sound,unsigned master)override{
        master=std::min(master,10u);
        musicVolume_=master*std::min(music,10u);soundVolume_=master*std::min(sound,10u);
    }
    void frame(unsigned frame)override{
        if(!started_||finished_||frame!=submitted_)throw std::runtime_error("Audio frame sequence");
        input_.gameFrame.store(frame,std::memory_order_relaxed);
        check();
#ifndef __PSP__
        drainDiagnostics();
#endif
        Packet packet{frame,state_};
        if(musicVolume_!=100||soundVolume_!=100){
            for(auto& voice:packet.state){
                if(voice.clip>=audioClipCount)continue;
                const int volume=musicClips_[voice.clip]?musicVolume_:soundVolume_;
                voice.left=(voice.left*volume+50)/100;
                voice.right=(voice.right*volume+50)/100;
            }
        }
        const unsigned n=unsigned((std::uint64_t(frame+1)*audioRate)/30-(std::uint64_t(frame)*audioRate)/30);
#ifdef __PSP__
        const unsigned h=head_.load(std::memory_order_relaxed),t=tail_.load(std::memory_order_acquire);
        if(h-(diagnostic_?logged_.load(std::memory_order_acquire):t)>=queueSize)throw std::runtime_error("Audio command overflow");
        queue_[h%queueSize]=packet;head_.store(h+1,std::memory_order_release);queuePeak_=std::max(queuePeak_,h+1-t);
#else
        std::array<std::int16_t,2134> pcm{};mixer_.apply(packet.state);render(pcm.data(),n);++applied_;
        if(diagnostic_){logPacket(packet);drainDiagnostics();}
        if(std::fwrite(pcm.data(),4,n,wav_)!=n)throw std::runtime_error("Audio WAV write");
#endif
        for(unsigned i=0;i<state_.size();++i){auto& s=state_[i];if(s.clip==~0u)continue;
            const auto& c=clips_[s.clip];if(c.stream)continue;
            position_[i]+=std::uint64_t(n)*s.step;const auto end=std::uint64_t(c.frames)<<16;
            if(s.loop)position_[i]%=end;else if(position_[i]>=end)stop(i);
        }++submitted_;
    }
    void finish()override{
        if(finished_)return;
        finished_=true;
#ifdef __PSP__
        join();
#else
        wavHeader(wav_,totalSamples_);if(std::fclose(wav_)){wav_=nullptr;throw std::runtime_error("Audio WAV close");}wav_=nullptr;
#endif
        input_.close();
        check();
        if(applied_!=submitted_)throw std::runtime_error("Audio unapplied commands");
        drainDiagnostics();closeLog(voiceLog_);closeLog(costLog_);
        unsigned underruns=0,reopens=0;for(auto& s:streams_)if(s){underruns+=s->underruns;reopens+=s->reopens;}
        const unsigned streamUnderruns=underruns;
        for(auto& s:tails_)if(s){underruns+=s->underruns;reopens+=s->reopens;}
        unsigned resident=0,tailBytes=0,ambienceBytes=0,longClips=0;
        for(const auto& c:clips_){resident+=unsigned(c.pcm.size()*2);if(c.tail())++longClips;}
        for(const auto& t:tails_)if(t)tailBytes+=audioTailPages*audioTailPageFrames*2*2;
        unsigned prefixBytes=0;
        for(const auto& s:streams_)if(s){ambienceBytes+=(ringFrames+audioHeadFrames)*2*2;prefixBytes+=audioHeadFrames*2*2;}
        FILE* f=open("audio-summary.txt");std::fprintf(f,"rate=%u\nvoices=%u\nsubmitted=%u\napplied=%u\nrendered_samples=%u\nblocks=%u\nmix_us=%llu\nmix_us_per_game_frame=%.3f\npeak=%u\nclipped=%u\nunderruns=%u\nreopens=%u\nqueue_peak=%u\n",
            audioRate,audioVoiceCount,submitted_,applied_,totalSamples_,mixBlocks_,mixUs_,submitted_?double(mixUs_)/submitted_:0.,mixer_.peak,mixer_.clipped,underruns,reopens,queuePeak_);
        std::fprintf(f,"stream_underruns=%u\ntail_underruns=%u\n",streamUnderruns,underruns-streamUnderruns);
        std::fprintf(f,"resident_pcm_bytes=%u\nlong_clips=%u\ntail_buffer_bytes=%u\nambience_buffer_bytes=%u\n",resident,longClips,tailBytes,ambienceBytes);
        std::fprintf(f,"stream_prefix_bytes=%u\n",prefixBytes);
        std::fprintf(f,"audio_peak_open_files=%u\naudio_opens_after_frame5=%u\n",input_.peakOpenFiles,input_.opensAfterFrame5);
        std::fclose(f);
    }
};
}
std::unique_ptr<AudioDevice> makeAudio(const std::string& output,bool diagnostic){return std::make_unique<Device>(output,diagnostic);}
}
