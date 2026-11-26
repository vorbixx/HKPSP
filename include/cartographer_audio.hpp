#pragma once
#include "cartographer.hpp"
#include "audio_channels.hpp"
#include <3ds.h>
#include <array>
#include <atomic>
#include <cstdio>

namespace hk3ds {
struct CityGate;
class CartographerAudio {
public:
    CartographerAudio(){LightLock_Init(&lock_);}
    void update(const Cartographer&,Vec2 listener,unsigned volume);
    void updateGate(const CityGate&,unsigned volume);
    void release();
    void pause(bool paused);
private:
    static constexpr unsigned channel=tabletHumChannel;
    static constexpr unsigned samples=4096;
    std::array<ndspWaveBuf,2> buffers_{};
    // Worker-private staging keeps an obsolete read out of the live DSP queue.
    std::array<std::int16_t,samples> staging_{};
    std::int16_t* pcm_=nullptr;
    FILE* file_=nullptr;
    unsigned active_=0,serial_=0,remaining_=0,length_=0,rate_=0;
    long start_=0;
    LightLock lock_;
    Thread thread_=nullptr;
    std::atomic<bool> quit_{false};
    unsigned generation_=0,serviced_=0,next_=0;
    bool paused_=false,starting_=false;
    decltype(&std::fopen) open_=std::fopen;
    decltype(&std::fread) read_=std::fread;
    decltype(&std::fseek) seek_=std::fseek;
    decltype(&std::fclose) close_=std::fclose;
    bool request(unsigned kind);
    bool openFile(const char* path);
    void closeFile();
    void mix(float gain);
    void stop();
    void service();
    static void entry(void* self){static_cast<CartographerAudio*>(self)->run();}
    void run();
    friend struct CartographerAudioTest;
};
}
