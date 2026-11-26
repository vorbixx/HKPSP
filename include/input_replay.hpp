#pragma once
#include "input.hpp"
#include "diagnostic_game_flags.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

namespace hk3ds
{
inline constexpr std::uint32_t diagnosticCombo = 256u | 512u | 4u; // R + L + SELECT.
inline constexpr std::uint32_t quickCastHid = 32768u;
inline constexpr bool diagnosticComboHeld(std::uint32_t keys) { return (keys & diagnosticCombo) == diagnosticCombo; }
struct HidUiState {std::uint32_t previousKeys=0,suppressed=0;bool shop=false;};

// libctru HID bit layout. Both live HID and local replay use this same adapter.
inline void sampleHid(InputLatch& latch, std::uint32_t keys, int dx, int dy,bool shopButtons=false,HidUiState* context=nullptr)
{
    if(diagnosticComboHeld(keys))keys &= ~diagnosticCombo;
    if(context){
        if(context->shop!=shopButtons)context->suppressed|=context->previousKeys&keys&3u;
        context->previousKeys=keys;context->shop=shopButtons;context->suppressed&=keys;
        keys&=~context->suppressed;
    }
    float x=dx/156.0f,y=dy/156.0f;
    if(keys&48)x=float(bool(keys&16))-float(bool(keys&32));
    if(keys&192)y=float(bool(keys&64))-float(bool(keys&128));
    std::uint16_t held=0;
    if(keys&2)held|=Jump;
    if(keys&256)held|=Dash;
    if(keys&1024)held|=Attack;
    if(keys&2048)held|=Focus;
    if(keys&128)held|=Down;
    if(keys&512)held|=Dream; // L: Dream Nail for prepared late-game saves.
    if(keys&quickCastHid)held|=Spell; // ZR: quick cast; Y remains tap spell / hold Focus.
    if(keys&1)held|=SuperDash; // A: Crystal Heart charge/release, also on Old hardware.
    if(shopButtons){held&=Down;if(keys&1)held|=Jump;if(keys&2)held|=Attack;}
    latch.sample(x,y,held);
}

// Explicit local test input, never a game-state recording. HKIR0001 stores a
// LE segment count followed by (u32 ticks,u32 HID,i16 circleX,i16 circleY).
// One record tick is one InputLatch consume, including paused/transition ticks.
class InputReplay
{
public:
    // Match tools/plan_convert.py; run-length tapes reserve whole-map settle time.
    static constexpr std::uint32_t maxControlTicks=4u*60*60*60;
    bool load(const char* path)
    {
        *this={};
        auto* f=std::fopen(path,"rb");if(!f)return false;
        const bool ok=read([&](void* out,std::size_t size){return std::fread(out,1,size,f)==size;},
                           [&](){return std::fgetc(f)==EOF;});
        std::fclose(f);return ok;
    }
    bool loadBytes(const unsigned char* data,std::size_t size)
    {
        std::size_t offset=0;
        return read([&](void* out,std::size_t count){
            if(!data||count>size-offset)return false;
            std::memcpy(out,data+offset,count);offset+=count;return true;
        },[&](){return offset==size;});
    }
private:
    template<class Read,class End> bool read(Read readBytes,End atEnd)
    {
        *this={};
        char magic[8];unsigned char bytes[12];
        bool ok=readBytes(magic,8);
        const bool benchmark=ok&&!std::memcmp(magic,"HKIR0002",8);
        ok=ok&&(benchmark||!std::memcmp(magic,"HKIR0001",8))&&readBytes(bytes,4);
        const auto count=ok?u32(bytes):0;
        ok=ok&&count>0&&count<=32768;
        // v2 adds a fixed fresh-start fixture: room (0 Tutorial, 2 Crossroads),
        // RNG seed (currently 1), and the complete diagnostic game mask.
        // Controller records retain exactly the v1 InputLatch semantics.
        if(ok&&benchmark){
            ok=readBytes(bytes,12);
            if(ok){
                benchmarkRoom_=int(u32(bytes));benchmarkMask_=int(u32(bytes+8));
                ok=(benchmarkRoom_==0||benchmarkRoom_==2)&&u32(bytes+4)==1&&u32(bytes+8)<=255;
            }
        }
        for(std::uint32_t i=0;ok&&i<count;++i)
        {
            ok=readBytes(bytes,12);if(!ok)break;
            Segment s{u32(bytes),u32(bytes+4),i16(bytes+8),i16(bytes+10)};
            // Gameplay controls plus the complete diagnostic chord. Standalone
            // or partial SELECT remains out-of-band and is never replayed.
            ok=s.ticks>0&&s.ticks<=maxControlTicks-total_&&!(s.keys&~(3579u|512u|4u|quickCastHid))&&
                (!(s.keys&4u)||diagnosticComboHeld(s.keys))&&
                s.x>=-156&&s.x<=156&&s.y>=-156&&s.y<=156;
            if(ok){segments_.push_back(s);total_+=s.ticks;}
        }
        ok=ok&&atEnd();
        // Legacy SD loaders validate HKIR0001 only. Two reserved
        // controller markers carry an envelope (signed-byte mask, -156 keeps defaults)
        // which the game removes before
        // replay tick zero. The loader validates these as ordinary records.
        if(ok&&!benchmark&&segments_.size()>=2){
            const auto marker=[](const Segment& s,int x,int y){
                return s.ticks==1&&s.keys==3578&&s.x==x&&s.y==y;
            };
            if(marker(segments_[0],-156,156)&&marker(segments_[1],156,-156)){
                ok=segments_.size()>3;
                if(ok){
                    const auto& meta=segments_[2];
                    ok=meta.ticks==1&&(meta.keys==0||meta.keys==2)&&((meta.x>=-128&&meta.x<=127)||meta.x==-156)&&meta.y==1;
                    if(ok){benchmarkRoom_=int(meta.keys);benchmarkMask_=meta.x==-156?-1:(meta.x<0?meta.x+256:meta.x);
                        total_-=3;segments_.erase(segments_.begin(),segments_.begin()+3);}
                }
            }
        }
        if(!ok){*this={};return false;}
        remaining_=segments_[0].ticks;return true;
    }
public:
    bool exhausted() const {return active()&&tick_>=total_;}
    void sample(InputLatch& input,bool shopButtons=false)
    {
        if(index_>=segments_.size()){keys_=0;sampleHid(input,0,0,0,shopButtons,&uiState_);return;}
        const auto& s=segments_[index_];keys_=s.keys;
        sampleHid(input,s.keys,s.x,s.y,shopButtons,&uiState_);++tick_;
        if(!--remaining_&&++index_<segments_.size())remaining_=segments_[index_].ticks;
    }
    bool active() const { return !segments_.empty(); }
    std::uint32_t keys() const { return keys_; }
    std::uint32_t tick() const { return tick_; }
    std::uint32_t total() const { return total_; }
    int benchmarkRoom() const { return benchmarkRoom_; }
    int benchmarkMask() const { return benchmarkMask_; }
private:
    HidUiState uiState_{};
    struct Segment {std::uint32_t ticks,keys;int x,y;};
    std::vector<Segment> segments_;
    std::size_t index_=0;
    std::uint32_t remaining_=0,total_=0,tick_=0,keys_=0;
    int benchmarkRoom_=-1,benchmarkMask_=benchmarkUseSdDefaults;
    static std::uint32_t u32(const unsigned char* p)
    {return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
    static int i16(const unsigned char* p)
    {const unsigned n=p[0]|(unsigned(p[1])<<8);return n<32768?int(n):int(n)-65536;}
};
}
