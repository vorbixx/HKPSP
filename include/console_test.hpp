#pragma once
#include "build_config.hpp"
#include "console_wire.hpp"
#include "console_launch.hpp"
#include "diagnostics.hpp"
#include <citro2d.h>
#include <array>
#include <vector>

namespace hk3ds {
// Explicit argv-only adapter. Manual play mounts the upload without a test session.
class ConsoleTestSession {
    console::Job job_{};
    bool active_=false,playing_=false,finished_=false;
    u64 started_=0;
    const char* reason_="initialization_failed";
    char save_[192]{},replay_[192]{};
    std::vector<std::array<char,32>> scenes_;
public:
    bool requested=false;
    ConsoleTestSession(int argc,char** argv) {
        const console::LaunchSelection selection(argc,argv);
        requested=selection.requested();
        if(!requested)return;
        if(!diagnosticsEnabled&&selection.testing())return;
        if(!selection.id||!console::loadJob(selection.id,job_))return;
        playing_=selection.playing();active_=selection.testing();
        if(playing_)return;
        job_.path(save_,sizeof(save_),"tutorial.sav");
        job_.path(replay_,sizeof(replay_),"input-replay.bin");
        started_=svcGetSystemTick();
    }
    ~ConsoleTestSession() {
        if(active_&&!finished_)finish(false,false,false);
        if(requested)console::launch(console::loader);
    }
    bool active() const {return diagnosticsEnabled&&active_;}
    void sceneEntered(const char* scene) {
        if(!active()||scenes_.size()>=64)return;
        scenes_.push_back({});
        std::snprintf(scenes_.back().data(),scenes_.back().size(),"%s",scene);
    }
    bool playing() const {return playing_;}
    Result initRomfs() const {
        if((!active_&&!playing_)||!job_.assets.size){
            const Result embedded=romfsInit();
#if HK3DS_DIAGNOSTICS
            if(R_FAILED(embedded)){
                Handle file=0;
                const Result opened=FSUSER_OpenFileDirectly(&file,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),
                    fsMakePath(PATH_ASCII,"/3ds/HK3DS/diagnostic.romfs"),FS_OPEN_READ,0);
                if(R_FAILED(opened))return opened;
                return romfsMountFromFile(file,0,"romfs");
            }
#endif
            return embedded;
        }
        char path[192];job_.assets.path(path,sizeof(path));Handle file=0;
        Result result=FSUSER_OpenFileDirectly(&file,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),
            fsMakePath(PATH_ASCII,path+5),FS_OPEN_READ,0);
        if(R_FAILED(result))return result;
        u64 size=0;result=FSFILE_GetSize(file,&size);
        if(R_FAILED(result)||size!=job_.assets.size){FSFILE_Close(file);return -1;}
        // libctru takes ownership, including closing the handle on mount failure.
        return romfsMountFromFile(file,0,"romfs");
    }
    bool verified() const {
        if(!active_&&!playing_)return false;
        char binary[192];job_.path(binary,sizeof(binary),"game.3dsx");
        return console::hashFile(binary,job_.binarySize,job_.binaryHash)&&
            (playing_||console::hashFile(replay_,job_.replaySize,job_.replayHash));
    }
    const char* savePath() const {return save_;}
    const char* replayPath() const {return replay_;}
    void path(char* out,std::size_t size,const char* name) const {job_.path(out,size,name);}
    void stop(const char* reason){reason_=reason;}
    bool shouldStop(unsigned frame,unsigned tick,unsigned total) {
        if(!active_)return false;
        if(total&&tick>=total){reason_="replay_complete";return true;}
        if(frame>=job_.maxFrames){reason_="frame_limit";return true;}
        if(double(svcGetSystemTick()-started_)/SYSCLOCK_ARM11>=job_.maxSeconds){reason_="time_limit";return true;}
        return false;
    }
    bool capture(C3D_RenderTarget* target) {
        if constexpr(!diagnosticsEnabled)return false;
        if(!active_)return false;
        C3D_FrameSync();
        u16 width=0,height=0;auto* pixels=gfxGetFramebuffer(GFX_TOP,GFX_LEFT,&width,&height);
        if(!pixels||width!=240||height!=400)return false;
        auto& fb=target->frameBuf;
        C3D_SyncDisplayTransfer(static_cast<u32*>(fb.colorBuf),GX_BUFFER_DIM(fb.width,fb.height),
            reinterpret_cast<u32*>(pixels),GX_BUFFER_DIM(width,height),target->transferFlags);
        GSPGPU_InvalidateDataCache(pixels,240*400*3);
        char path[192];job_.path(path,sizeof(path),"frame.ppm");auto* f=std::fopen(path,"wb");if(!f)return false;
        bool ok=std::fprintf(f,"P6\n400 240\n255\n")>0;
        unsigned char row[400*3];
        for(unsigned y=0;y<240;++y){
            for(unsigned x=0;x<400;++x){const auto* p=pixels+(x*240+239-y)*3;
                row[x*3]=p[2];row[x*3+1]=p[1];row[x*3+2]=p[0];}
            ok&=std::fwrite(row,1,sizeof(row),f)==sizeof(row);
        }
        if(std::fclose(f))ok=false;
        return ok;
    }
    void finish(bool metrics,bool stages,bool frame) {
        if constexpr(!diagnosticsEnabled)return;
        if(!active_||finished_)return;
        finished_=true;
        char path[192];job_.path(path,sizeof(path),"snapshot.bin");auto* f=std::fopen(path,"wb");
        unsigned char bytes[sizeof(DiagnosticSnapshot)];
        const auto* source=reinterpret_cast<const volatile unsigned char*>(&hk3ds_diagnostics);
        for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=source[i];
        bool snapshot=f&&std::fwrite("HKEX0001",1,8,f)==8&&std::fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes);
        if(f&&std::fclose(f))snapshot=false;
        char binaryHash[65],replayHash[65],assetHash[65]{};console::hex(job_.binaryHash,binaryHash,32);console::hex(job_.replayHash,replayHash,32);
        if(job_.assets.size)console::hex(job_.assets.hash,assetHash,32);
        const bool complete=!std::strcmp(reason_,"replay_complete")||!std::strcmp(reason_,"replay_start");
        const bool passed=complete&&snapshot&&metrics&&stages&&frame&&hk3ds_diagnostics.frame>0&&
            hk3ds_diagnostics.visualLoaded&&hk3ds_diagnostics.visualFailures==0&&hk3ds_diagnostics.audioReady&&hk3ds_diagnostics.audioError==0;
        job_.path(path,sizeof(path),"result.tmp");f=std::fopen(path,"wb");if(!f)return;
        bool ok=std::fprintf(f,"{\"protocol\":1,\"id\":\"%s\",\"binary_sha256\":\"%s\",\"replay_sha256\":\"%s\",\"asset_sha256\":\"%s\",\"passed\":%s,\"reason\":\"%s\",\"metrics\":%s,\"stages\":%s,\"frame\":%s,\"snapshot\":%s,\"frames\":%lu,\"replay_tick\":%lu,\"replay_total\":%lu,\"scenes\":[",
            job_.id,binaryHash,replayHash,assetHash,passed?"true":"false",reason_,metrics?"true":"false",stages?"true":"false",frame?"true":"false",snapshot?"true":"false",
            static_cast<unsigned long>(hk3ds_diagnostics.frame),static_cast<unsigned long>(hk3ds_diagnostics.replayTick),static_cast<unsigned long>(hk3ds_diagnostics.replayTotal))>0;
        for(std::size_t i=0;i<scenes_.size();++i)ok&=std::fprintf(f,"%s\"%s\"",i?",":"",scenes_[i].data())>0;
        ok&=std::fprintf(f,"]}\n")>0;
        if(std::fclose(f))ok=false;
        char final[192];job_.path(final,sizeof(final),"result.json");if(ok)std::rename(path,final);
    }
};
}
