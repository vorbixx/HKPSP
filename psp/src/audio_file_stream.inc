// Included after AudioFile inside audio_device.cpp's private namespace.
// One producer (I/O), one consumer (mixer). Restart publishes a request only
// after the consumer has left the ring; a retained prefix covers the rewind.
class FileStream final:public AudioStream {
    const AudioClip& clip_;AudioFile& file_;unsigned position_=0,remaining_=ringFrames;
    std::array<std::int16_t,ringFrames*2> ring_{};
    std::array<std::int16_t,audioHeadFrames*2> prefix_{};
    const unsigned prefixFrames_;
    unsigned prefixRead_,serviced_=0;
    bool consumed_=false;
    std::atomic<unsigned> read_{0},write_{0},requested_{0},ready_{0};
public:
    std::atomic<bool> failed{false};unsigned underruns=0,reopens=0;
    FileStream(const AudioClip& clip,AudioFile& file):clip_(clip),file_(file),
        prefixFrames_(std::min(audioHeadFrames,clip.frames)),prefixRead_(prefixFrames_){
        while(pump()){}
        if(failed)throw std::runtime_error("Audio stream prime");
        std::copy_n(ring_.data(),prefixFrames_*2,prefix_.data());
    }
    void restart()override{
        if(!consumed_)return; // Startup ring is already primed from sample zero.
        prefixRead_=0;
        if(prefixFrames_==clip_.frames)return; // Entire short loop is retained.
        // The I/O worker owns both counters only during this handoff. read()
        // cannot touch the ring again until ready_ acknowledges this request.
        requested_.fetch_add(1,std::memory_order_release);
        file_.wake();
    }
    bool pump(){
        if(failed)return false;
        const unsigned request=requested_.load(std::memory_order_acquire);
        if(request!=serviced_){
            read_.store(0,std::memory_order_relaxed);write_.store(0,std::memory_order_relaxed);
            position_=prefixFrames_%clip_.frames;remaining_=ringFrames;serviced_=request;
        }
        const unsigned w=write_.load(std::memory_order_relaxed),r=read_.load(std::memory_order_acquire);
        // Freeze the burst target. Mixer progress during a read must not make
        // this worker chase a full ring with another tiny read on every pass.
        if(!remaining_){
            if(w-r>ringFrames-ioBlock)return false;
            remaining_=(ringFrames-(w-r))/ioBlock*ioBlock;
        }
        const unsigned n=std::min({ioBlock,remaining_,clip_.frames-position_});
        bool ok=false;
        for(unsigned attempt=0;attempt<3;++attempt){
            if(file_.read(clip_,position_,ring_.data()+(w%ringFrames)*2,n,ringFrames-w%ringFrames,ring_.data())){ok=true;break;}
            ++reopens;
        }
        if(!ok){failed=true;return false;}
        position_+=n;
        if(position_==clip_.frames)position_=0;
        write_.store(w+n,std::memory_order_release);
        remaining_-=n;
        ready_.store(serviced_,std::memory_order_release);
        return true;
    }
    unsigned read(std::int16_t* pcm,unsigned frames)override{
        consumed_=true;
        if(prefixFrames_==clip_.frames){
            unsigned copied=0;
            while(copied<frames){
                if(prefixRead_==prefixFrames_)prefixRead_=0;
                const unsigned n=std::min(frames-copied,prefixFrames_-prefixRead_);
                std::memcpy(pcm+copied*2,prefix_.data()+prefixRead_*2,n*4);
                prefixRead_+=n;copied+=n;
            }
            return copied;
        }
        unsigned copied=std::min(frames,prefixFrames_-prefixRead_);
        std::memcpy(pcm,prefix_.data()+prefixRead_*2,copied*4);prefixRead_+=copied;
        if(copied==frames)return copied;
#ifndef __PSP__
        while((ready_.load()!=requested_.load()||write_.load()-read_.load()<frames-copied)&&!failed)pump();
#endif
        if(ready_.load(std::memory_order_acquire)!=requested_.load(std::memory_order_relaxed)){
            ++underruns;HKPSP_PROFILE(perf::audioUnderrun(false);)return copied;
        }
        const unsigned r=read_.load(std::memory_order_relaxed),w=write_.load(std::memory_order_acquire);
        const unsigned n=std::min(frames-copied,w-r);
        if(n<frames-copied){++underruns;HKPSP_PROFILE(perf::audioUnderrun(false);)}
        const unsigned first=std::min(n,ringFrames-r%ringFrames);
        std::memcpy(pcm+copied*2,ring_.data()+(r%ringFrames)*2,first*4);
        if(first<n)std::memcpy(pcm+(copied+first)*2,ring_.data(),(n-first)*4);
        read_.store(r+n,std::memory_order_release);
        HKPSP_PROFILE(perf::audioLow(perf::AudioRingLow,w-r-n);)
        if(w-r>ringFrames-ioBlock&&w-r-n<=ringFrames-ioBlock)file_.wake();
        return copied+n;
    }
};
