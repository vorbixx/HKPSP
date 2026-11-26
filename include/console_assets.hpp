#pragma once
#include "console_wire.hpp"
#include <sys/stat.h>

namespace hk3ds::console {
// Single-threaded loader store. Only a complete, SHA-256-verified image is published.
class AssetStore {
    const char* base_;
    Assets verified_{};
    // A record is published only after hashing the complete immutable image.
    // This trusts SD contents between loader sessions, just as ready() does
    // within a session. Size/record mismatches take the full SHA-256 path.
    bool recorded(const Assets& asset,const char* path) const {
        char record[208];std::snprintf(record,sizeof(record),"%s.verified",path);
        auto* file=std::fopen(record,"rb");if(!file)return false;
        unsigned char bytes[assetSize];Assets stored;
        const bool ok=std::fread(bytes,1,sizeof(bytes),file)==sizeof(bytes)&&
            std::fgetc(file)==EOF&&stored.decode(bytes,sizeof(bytes))&&
            stored.size==asset.size&&!std::memcmp(stored.hash,asset.hash,32);
        std::fclose(file);return ok;
    }
    void remember(const Assets& asset,const char* path) const {
        char record[208],temporary[216];
        std::snprintf(record,sizeof(record),"%s.verified",path);
        std::snprintf(temporary,sizeof(temporary),"%s.part",record);
        unsigned char bytes[assetSize]{};std::memcpy(bytes,"HKAS0001",8);
        std::memcpy(bytes+8,asset.hash,32);write32(bytes+40,asset.size);
        auto* file=std::fopen(temporary,"wb");if(!file)return;
        bool ok=std::fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes);
        if(std::fclose(file))ok=false;
        if(ok){std::remove(record);ok=std::rename(temporary,record)==0;}
        if(!ok)std::remove(temporary);
    }
public:
    explicit AssetStore(const char* base=assetRoot):base_(base){}
    bool has(const Assets& asset) {
        verified_={};
        if(!asset.valid())return false;
        char path[192];asset.path(path,sizeof(path),base_);struct stat info{};
        if(stat(path,&info)||std::uint64_t(info.st_size)!=asset.size)return false;
        if(!recorded(asset,path)){
            if(!hashFile(path,asset.size,asset.hash))return false;
            remember(asset,path);
        }
        verified_=asset;return true;
    }
    bool ready(const Assets& asset) {
        char path[192];asset.path(path,sizeof(path),base_);struct stat info{};
        // No other request mutates a committed image between verification and launch.
        if(asset.valid()&&asset.size==verified_.size&&!std::memcmp(asset.hash,verified_.hash,32)&&
           !stat(path,&info)&&std::uint64_t(info.st_size)==asset.size)return true;
        return has(asset);
    }
    template<class Receive> bool upload(const Assets& asset,Receive receive) {
        verified_={};if(!asset.valid())return false;
        char path[192],temporary[200];asset.path(path,sizeof(path),base_);
        std::snprintf(temporary,sizeof(temporary),"%s.part",path);
        auto* file=std::fopen(temporary,"wb");if(!file)return false;
        unsigned left=asset.size;unsigned char buffer[8192],digest[32];Sha256 hash;bool ok=true;
        while(left&&ok){const unsigned n=left<sizeof(buffer)?left:sizeof(buffer);
            ok=receive(buffer,n);
            if(ok&&left==asset.size)ok=read32(buffer)==40; // devkitPro raw RomFS, not arbitrary files.
            if(ok){hash.add(buffer,n);ok=std::fwrite(buffer,1,n,file)==n;left-=n;}}
        if(std::fclose(file))ok=false;
        hash.finish(digest);ok=ok&&!left&&!std::memcmp(digest,asset.hash,32);
        if(ok){
            char record[208];std::snprintf(record,sizeof(record),"%s.verified",path);
            std::remove(record);
        }
        if(ok&&std::rename(temporary,path)) {
            // FAT and Windows may refuse rename over an existing (possibly corrupt) image.
            ok=std::remove(path)==0&&std::rename(temporary,path)==0;
        }
        if(!ok){std::remove(temporary);return false;}
        remember(asset,path);verified_=asset;return true;
    }
};
}
