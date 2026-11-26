#pragma once
// Read-only access to diagnostic text files in the fixed game directory.
#include "console_wire.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <initializer_list>
#ifdef _WIN32
#include <filesystem>
#endif

namespace hk3ds::console {
constexpr const char* logRoot="sdmc:/3ds/HK3DS";
constexpr unsigned maxLogName=96, maxLogFiles=128, maxLogManifest=32768;
constexpr unsigned maxLogBytes=8*1024*1024;
inline bool logName(const char* name) {
    const auto n=std::strlen(name);
    if(n<5||n>=maxLogName||name[0]=='.'||std::strstr(name,"..")||std::strcmp(name+n-4,".txt"))return false;
    for(unsigned i=0;i<n;++i) {
        const char c=name[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return false;
    }
    return true;
}
inline bool saveMetadataName(const char* name) {
    for(const char* allowed:{"tutorial.sav","slot-2.sav","slot-3.sav","slot-4.sav","system.sav"})
        if(!std::strcmp(name,allowed))return true;
    return false;
}
// MinGW lacks lstat; inspect links before stat in the Windows host fixture.
inline int logStat(const char* path,struct stat* info) {
#ifdef _WIN32
    std::error_code error;
    const auto type=std::filesystem::symlink_status(path,error);
    if(error)return -1;
    if(std::filesystem::is_symlink(type)){std::memset(info,0,sizeof(*info));return 0;}
    return stat(path,info);
#else
    return lstat(path,info);
#endif
}
struct LogStore {
    const char* directory=logRoot;
    bool path(char* out,unsigned capacity,const char* name) const {
        const int n=std::snprintf(out,capacity,"%s/%s",directory,name);
        return n>=0&&unsigned(n)<capacity;
    }
    // False means a complete bounded inventory could not be made. Never truncate silently.
    bool manifest(char* out,unsigned capacity,unsigned& length) const {
        if(capacity<32)return false;
        auto* dir=opendir(directory);if(!dir)return false;
        length=unsigned(std::snprintf(out,capacity,"{\"protocol\":1,\"files\":["));
        unsigned count=0;bool ok=true;
        while(ok) {
            errno=0;auto* entry=readdir(dir);
            if(!entry){if(errno)ok=false;break;}
            const bool log=logName(entry->d_name);
            if(!log&&!saveMetadataName(entry->d_name))continue;
            char full[256];struct stat info{};
            if(!path(full,sizeof(full),entry->d_name)||logStat(full,&info)||info.st_size<0){ok=false;break;}
            if(!S_ISREG(info.st_mode))continue;
            if(count==maxLogFiles){ok=false;break;}
            const int n=std::snprintf(out+length,capacity-length,
                "%s{\"name\":\"%s\",\"kind\":\"%s\",\"size\":%llu,\"mtime\":%lld}",
                count?",":"",entry->d_name,log?"log":"save",
                static_cast<unsigned long long>(info.st_size),static_cast<long long>(info.st_mtime));
            if(n<0||unsigned(n)>=capacity-length){ok=false;break;}
            length+=unsigned(n);++count;
        }
        closedir(dir);
        if(!ok||length+3>capacity)return false;
        std::memcpy(out+length,"]}",3);length+=2;return true;
    }
    // Caller owns the returned read-only handle. Saves and nonregular files are refused.
    std::FILE* open(const char* name,unsigned& length) const {
        if(!logName(name))return nullptr;
        char full[256];struct stat info{};
        if(!path(full,sizeof(full),name)||logStat(full,&info)||!S_ISREG(info.st_mode)||
           info.st_size<0||static_cast<unsigned long long>(info.st_size)>maxLogBytes)return nullptr;
        auto* file=std::fopen(full,"rb");if(!file)return nullptr;
        const bool seek=std::fseek(file,0,SEEK_END)==0;
        const long actual=std::ftell(file);std::rewind(file);
        if(!seek||actual<0||actual!=info.st_size){std::fclose(file);return nullptr;}
        length=unsigned(actual);return file;
    }
};
}
