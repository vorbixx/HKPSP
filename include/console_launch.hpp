#pragma once
#include <3ds.h>
#include <cstring>
#include "console_launch_mode.hpp"

namespace hk3ds::console {
// Rosalina hb:ldr IPC 2/3, as documented by devkitPro/3ds-hbmenu
// source/loaders/rosalina.c. Does not replace boot.3dsx or install a title.
inline bool launch(const char* path,const char* id=nullptr,LaunchMode mode=LaunchMode::Test) {
    LaunchArguments args;
    if(!args.set(path,id,id?mode:LaunchMode::Normal))return false;
    Handle handle=0;if(R_FAILED(svcConnectToPort(&handle,"hb:ldr")))return false;
    const char* target=!std::strncmp(path,"sdmc:/",6)?path+5:path;
    auto* ipc=getThreadCommandBuffer();ipc[0]=IPC_MakeHeader(3,0,2);
    ipc[1]=IPC_Desc_StaticBuffer(sizeof(args.bytes),1);ipc[2]=reinterpret_cast<u32>(args.bytes);
    Result result=svcSendSyncRequest(handle);if(R_SUCCEEDED(result))result=ipc[1];
    if(R_SUCCEEDED(result)) {
        ipc[0]=IPC_MakeHeader(2,0,2);ipc[1]=IPC_Desc_StaticBuffer(std::strlen(target)+1,0);ipc[2]=reinterpret_cast<u32>(target);
        result=svcSendSyncRequest(handle);if(R_SUCCEEDED(result))result=ipc[1];
    }
    svcCloseHandle(handle);return R_SUCCEEDED(result);
}
}
