#pragma once
#include "console_assets.hpp"
#include "console_launch_mode.hpp"
#include <dirent.h>
#include <algorithm>
#include <vector>
#include <ctime>

namespace hk3ds::console {
// job.bin is committed last by Upload. Results/started.flag never affect recency.
class BuildStore {
    const char* base_;
    static bool supportsPlay(const char* path) {
        auto* file=std::fopen(path,"rb");if(!file)return false;
        constexpr unsigned overlap=sizeof(playArgument)-1;
        unsigned char bytes[8192+overlap];unsigned kept=0;bool found=false;
        while(!found){
            const auto n=std::fread(bytes+kept,1,8192,file);if(!n)break;
            const auto total=kept+n;
            for(unsigned i=0;i+sizeof(playArgument)<=total;++i)
                if(!std::memcmp(bytes+i,playArgument,sizeof(playArgument))){found=true;break;}
            kept=unsigned(std::min<std::size_t>(total,overlap));
            std::memmove(bytes,bytes+total-kept,kept);
        }
        std::fclose(file);return found;
    }
public:
    explicit BuildStore(const char* base=root):base_(base){}
    bool playable(const Job& job,AssetStore& assets)const {
        char path[192];job.path(path,sizeof(path),"game.3dsx",base_);
        return supportsPlay(path)&&hashFile(path,job.binarySize,job.binaryHash)&&
            (!job.assets.size||assets.ready(job.assets));
    }
    bool remember(const Job& job)const {
        char path[192],temporary[200];std::snprintf(path,sizeof(path),"%s/latest.id",base_);
        std::snprintf(temporary,sizeof(temporary),"%s.part",path);
        auto* file=std::fopen(temporary,"wb");if(!file)return false;
        bool ok=std::fwrite(job.id,1,32,file)==32;if(std::fclose(file))ok=false;
        if(ok&&std::rename(temporary,path))ok=std::remove(path)==0&&std::rename(temporary,path)==0;
        if(!ok)std::remove(temporary);
        return ok;
    }
    bool latest(Job& selected,AssetStore& assets)const {
        selected={};char preferred[33]{},path[192];
        std::snprintf(path,sizeof(path),"%s/latest.id",base_);
        if(auto* file=std::fopen(path,"rb")){
            if(std::fread(preferred,1,32,file)!=32||std::fgetc(file)!=EOF)preferred[0]=0;
            std::fclose(file);
        }
        struct Candidate {char id[33];std::time_t modified;bool preferred;};
        std::vector<Candidate> candidates;
        auto* directory=opendir(base_);if(!directory)return false;
        while(auto* entry=readdir(directory)){
            if(std::strlen(entry->d_name)!=32||!validId(entry->d_name))continue;
            std::snprintf(path,sizeof(path),"%s/%.32s/job.bin",base_,entry->d_name);struct stat info{};
            if(stat(path,&info)||!S_ISREG(info.st_mode))continue;
            Candidate candidate{};std::memcpy(candidate.id,entry->d_name,32);candidate.modified=info.st_mtime;
            candidate.preferred=!std::strcmp(candidate.id,preferred);candidates.push_back(candidate);
        }
        closedir(directory);
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){
            if(a.preferred!=b.preferred)return a.preferred;
            if(a.modified!=b.modified)return a.modified>b.modified;
            return std::strcmp(a.id,b.id)>0;
        });
        for(const auto& candidate:candidates){
            Job job;
            if(loadJob(candidate.id,job,base_)&&playable(job,assets)){selected=job;return true;}
        }
        return false;
    }
};
}
