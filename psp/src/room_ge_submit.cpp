// Host fixtures compile the adapter body against a recording SDK.
#ifdef __PSP__
#include "room_ge.hpp"
#include "perf.hpp"
#include <pspgu.h>
#include <pspkernel.h>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
void prepareRoomGe(const RoomGeBuffers& buffers){
    if(buffers.vertices.empty())return;
    sceKernelDcacheWritebackRange(buffers.vertices.data(),buffers.vertices.size()*sizeof(RoomGePosition));
    const auto& indices=roomGeIndices();
    sceKernelDcacheWritebackRange(indices.data(),sizeof(indices));
}
void setRoomGeProjection(const RoomGeMatrix& projection){
    alignas(16) ScePspFMatrix4 matrix;
    static_assert(sizeof(matrix)==sizeof(projection),"GU matrix layout");
    std::memcpy(&matrix,projection.data(),sizeof(matrix));
    HKPSP_PROFILE(const unsigned before=sceGuCheckList();)
    sceGuSetMatrix(GU_PROJECTION,&matrix);
    HKPSP_PROFILE(
        const auto bytes=sceGuCheckList()-before;
        perf::current[perf::GuStateCommands]+=bytes/4;
        ++perf::current[perf::RoomGeMatrixLoads];perf::current[perf::RoomGeMatrixBytes]+=bytes;
    )
}
void drawRoomGe(const RoomGeBuffers& buffers,const RoomGeRun& run,std::uint32_t tint){
    if(run.count==0||run.count>roomGeMaxQuads||run.firstVertex%4||
       run.count>buffers.vertices.size()/4||run.firstVertex>buffers.vertices.size()-run.count*4)
        throw std::out_of_range("Invalid GE room run");
    HKPSP_PROFILE(const unsigned before=sceGuCheckList();)
    sceGuColor(tint);
    HKPSP_PROFILE(perf::current[perf::GuStateCommands]+=(sceGuCheckList()-before)/4;)
    sceGuDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_VERTEX_32BITF|GU_INDEX_16BIT|GU_TRANSFORM_3D,
                   run.count*6,roomGeIndices().data(),buffers.vertices.data()+run.firstVertex);
    HKPSP_PROFILE(
        perf::current[perf::RoomGeDrawBytes]+=sceGuCheckList()-before;
        ++cpu::drawCalls;perf::current[perf::Triangles]+=run.count*2;
        ++perf::current[perf::RoomRunsDrawn];perf::current[perf::RoomGeVertices]+=run.count*4;
    )
}
} // namespace hkpsp
#endif
