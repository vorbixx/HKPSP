#pragma once
#include "console_wire.hpp"

namespace hk3ds::console {
// Versioned argument also identifies binaries with the normal-play launch hook.
inline constexpr char playArgument[]="--console-play-v1";
enum class LaunchMode { Normal, Test, Play, Invalid };
struct LaunchSelection {
    LaunchMode mode=LaunchMode::Normal;
    const char* id=nullptr;
    LaunchSelection(int argc,const char* const* argv) {
        if(argc<2)return;
        const bool test=!std::strcmp(argv[1],"--console-test");
        const bool play=!std::strcmp(argv[1],playArgument);
        if(!test&&!play)return;
        mode=LaunchMode::Invalid;
        if(argc!=3||std::strlen(argv[2])!=32||!validId(argv[2]))return;
        mode=play?LaunchMode::Play:LaunchMode::Test;id=argv[2];
    }
    bool requested()const{return mode!=LaunchMode::Normal;}
    bool testing()const{return mode==LaunchMode::Test;}
    bool playing()const{return mode==LaunchMode::Play;}
};
struct LaunchArguments {
    unsigned char bytes[1024]{};
    bool set(const char* path,const char* id,LaunchMode mode) {
        if(!path||std::strlen(path)>768||mode==LaunchMode::Invalid)return false;
        const bool job=mode==LaunchMode::Test||mode==LaunchMode::Play;
        if(job&&(!id||std::strlen(id)!=32||!validId(id)))return false;
        if(!job&&id)return false;
        unsigned offset=4;
        auto append=[&](const char* value){const auto n=std::strlen(value)+1;std::memcpy(bytes+offset,value,n);offset+=unsigned(n);};
        write32(bytes,job?3:1);append(path);
        if(job){append(mode==LaunchMode::Play?playArgument:"--console-test");append(id);}
        return true;
    }
};
}
