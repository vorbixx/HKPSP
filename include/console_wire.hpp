#pragma once
// Shared, bounded wire format. No platform dependencies or native struct dumps.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace hk3ds::console {
constexpr const char* root="sdmc:/3ds/HK3DS/nettest";
constexpr const char* loader="sdmc:/3ds/HK3DS-Net/HK3DS-Net.3dsx";
constexpr unsigned port=17491, jobSize=120, requestSize=48, responseSize=16;
constexpr unsigned assetSize=44, assetJobSize=156;
// Size limits: these two lines are their only definition. tools/console_wire.py parses this header for the
// PC tools (console_net.py keeps no copy), and wireLimits below carries the values inside every binary that
// includes it. 2026-11-11: the limit went 256 MiB -> 1 GiB in the loader and the PC tool, a game built from the
// old header rejected its own 387 MB job, and the only symptom was "console_test_verification_failed".
#define HK3DS_WIRE_MAX_BINARY 1073741824
#define HK3DS_WIRE_MAX_REPLAY 393228
#define HK3DS_WIRE_TEXT_(value) #value
#define HK3DS_WIRE_TEXT(value) HK3DS_WIRE_TEXT_(value)
constexpr unsigned maxBinary=HK3DS_WIRE_MAX_BINARY, maxReplay=HK3DS_WIRE_MAX_REPLAY;
static_assert(maxBinary==1024u*1024u*1024u&&maxReplay==12+32768*12,"1 GiB; replay header plus 32768 entries");
// The compiled limits as text, found by console_wire.py in a 3DSX/ELF before anything is uploaded.
// Job::decode references it with a volatile store, which keeps it through -Wl,--gc-sections in every binary
// that can decode a job (devkitARM GCC 16.1 ignores [[gnu::retain]]; tried 2026-11-12).
inline const char wireLimits[]="HK3DS-WIRE1 maxBinary=" HK3DS_WIRE_TEXT(HK3DS_WIRE_MAX_BINARY)
    " maxReplay=" HK3DS_WIRE_TEXT(HK3DS_WIRE_MAX_REPLAY) " ;";
constexpr const char* assetRoot="sdmc:/3ds/HK3DS/nettest/assets";
enum Operation : unsigned { Status=1, Upload=2, Run=3, Fetch=4, HasAssets=5, UploadAssets=6, FetchLogs=7 };
enum Artifact : unsigned { ResultJson=0, Snapshot=1, Metrics=2, Frame=3, Stages=4 };
inline const char* artifactName(unsigned id) {
    constexpr const char* names[]={"result.json","snapshot.bin","metrics.csv","frame.ppm","stages.csv"};
    return id<5?names[id]:nullptr;
}
inline std::uint32_t read32(const unsigned char* p) {
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
inline void write32(unsigned char* p,std::uint32_t n) {
    for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));
}
inline bool validId(const char* id) {
    for(unsigned i=0;i<32;++i)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
inline void hex(const unsigned char* bytes,char* out,unsigned size) {
    const char* digits="0123456789abcdef";
    for(unsigned i=0;i<size;++i){out[i*2]=digits[bytes[i]>>4];out[i*2+1]=digits[bytes[i]&15];}
    out[size*2]=0;
}
struct Assets {
    unsigned char hash[32]{};
    unsigned size=0;
    bool valid() const {return size>=40&&size<=maxBinary;}
    bool decode(const unsigned char* b,unsigned length) {
        if(length!=assetSize||std::memcmp(b,"HKAS0001",8))return false;
        std::memcpy(hash,b+8,32);size=read32(b+40);return valid();
    }
    void path(char* out,std::size_t length,const char* base=assetRoot) const {
        char digest[65];hex(hash,digest,32);std::snprintf(out,length,"%s/%s.romfs",base,digest);
    }
};
struct Job {
    char id[33]{};
    unsigned char binaryHash[32]{},replayHash[32]{};
    unsigned binarySize=0,replaySize=0,maxFrames=0,maxSeconds=0;
    Assets assets{};
    bool decode(const unsigned char* b,unsigned length=jobSize) {
        const char* volatile limits=wireLimits;(void)limits;  // keeps the marker linked; see wireLimits
        const bool external=length==assetJobSize&&!std::memcmp(b,"HKJB0002",8);
        if(!(external||(length==jobSize&&!std::memcmp(b,"HKJB0001",8)))||
           !validId(reinterpret_cast<const char*>(b+8)))return false;
        assets={};
        if(external){std::memcpy(assets.hash,b+120,32);assets.size=read32(b+152);if(!assets.valid())return false;}
        std::memcpy(id,b+8,32);id[32]=0;
        std::memcpy(binaryHash,b+40,32);std::memcpy(replayHash,b+72,32);
        binarySize=read32(b+104);replaySize=read32(b+108);
        maxFrames=read32(b+112);maxSeconds=read32(b+116);
        return binarySize>=4&&binarySize<=maxBinary&&replaySize>=24&&replaySize<=maxReplay&&
            maxFrames>=1&&maxFrames<=216000&&maxSeconds>=1&&maxSeconds<=3600;
    }
    void path(char* out,std::size_t size,const char* name,const char* base=root) const {
        std::snprintf(out,size,"%s/%s/%s",base,id,name);
    }
};
// Streaming SHA-256; network transfers never allocate a whole build in RAM.
class Sha256 {
    std::uint32_t state_[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    unsigned char block_[64]{};unsigned used_=0;std::uint64_t bytes_=0;
    static std::uint32_t rotate(std::uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
    void compress() {
        constexpr std::uint32_t k[]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for(unsigned i=0;i<16;++i)w[i]=(std::uint32_t(block_[4*i])<<24)|(std::uint32_t(block_[4*i+1])<<16)|(std::uint32_t(block_[4*i+2])<<8)|block_[4*i+3];
        for(unsigned i=16;i<64;++i){auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));}
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for(unsigned i=0;i<64;++i){auto t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            auto u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
public:
    void add(const unsigned char* p,std::size_t n) {
        bytes_+=n;
        while(n--){block_[used_++]=*p++;if(used_==64){compress();used_=0;}}
    }
    void finish(unsigned char* out) {
        const auto bits=bytes_*8;
        unsigned char byte=0x80;add(&byte,1);byte=0;
        while(used_!=56)add(&byte,1);
        unsigned char tail[8];for(unsigned i=0;i<8;++i)tail[i]=static_cast<unsigned char>(bits>>(56-8*i));
        add(tail,8);
        for(unsigned i=0;i<32;++i)out[i]=static_cast<unsigned char>(state_[i/4]>>(24-8*(i%4)));
    }
};
inline bool hashFile(const char* path,unsigned expectedSize,const unsigned char* expected) {
    auto* f=std::fopen(path,"rb");if(!f)return false;
    Sha256 hash;unsigned char buffer[8192],digest[32];std::size_t count;std::uint64_t total=0;
    while((count=std::fread(buffer,1,sizeof(buffer),f))){
        total+=count;if(total>expectedSize){std::fclose(f);return false;}
        hash.add(buffer,count);
    }
    bool ok=!std::ferror(f)&&total==expectedSize;std::fclose(f);hash.finish(digest);
    return ok&&!std::memcmp(digest,expected,32);
}
inline bool loadJob(const char* id,Job& job,const char* base=root) {
    if(!validId(id))return false;
    std::memcpy(job.id,id,32);job.id[32]=0;
    char path[192];job.path(path,sizeof(path),"job.bin",base);auto* f=std::fopen(path,"rb");if(!f)return false;
    unsigned char bytes[assetJobSize];const auto count=std::fread(bytes,1,sizeof(bytes),f);
    const bool ok=!std::ferror(f)&&std::fgetc(f)==EOF;
    std::fclose(f);return ok&&job.decode(bytes,unsigned(count))&&!std::memcmp(job.id,id,32);
}
}
