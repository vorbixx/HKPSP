#include "monomon_movie.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace hk3ds {
bool MonomonMovie::start(const char* path){
    release();file_=std::fopen(path,"rb");if(!file_)return false;
    std::uint32_t header[5]{};
    if(std::fread(header,sizeof(header),1,file_)!=1||std::memcmp(header,"HKM1",4)||
       header[1]!=200||header[2]!=112||header[3]!=348||header[4]!=30){release();return false;}
    for(unsigned i=0;i<2;++i){
        allocated_[i]=C3D_TexInit(&textures_[i],256,128,GPU_RGB565);
        if(!allocated_[i]){release();return false;}
        C3D_TexSetFilter(&textures_[i],GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&textures_[i],GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    }
    region_={200,112,0,1,200.f/256,1-112.f/128};
    return true;
}
bool MonomonMovie::draw(double dt){
    if(!file_||!std::isfinite(dt)||dt<0)return false;
    const unsigned frame=std::min(347u,unsigned(seconds_*30));
    if(frame!=last_){
        if(std::fseek(file_,20+long(frame*sizeof(pixels_)),SEEK_SET)||
           std::fread(pixels_,sizeof(pixels_),1,file_)!=1)return false;
        slot_^=1;auto* target=static_cast<std::uint16_t*>(textures_[slot_].data);
        // PICA texture tiles use Morton order inside each 8x8 tile.
        for(unsigned y=0;y<112;++y)for(unsigned x=0;x<200;++x){
            const unsigned a=x&7,b=y&7;
            const unsigned morton=(a&1)|((b&1)<<1)|((a&2)<<1)|((b&2)<<2)|((a&4)<<2)|((b&4)<<3);
            target[((y>>3)*32+(x>>3))*64+morton]=pixels_[y*200+x];
        }
        C3D_TexFlush(&textures_[slot_]);last_=frame;
    }
    C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,255));
    C2D_DrawImageAt({&textures_[slot_],&region_},0,8,0,nullptr,2,2);
    seconds_+=std::min(dt,.25);
    if(frame==347&&seconds_>=348.0/30)complete_=true;
    return true;
}
void MonomonMovie::release(){
    if(file_)std::fclose(file_);
    file_=nullptr;
    for(unsigned i=0;i<2;++i)if(allocated_[i]){C3D_TexDelete(&textures_[i]);allocated_[i]=false;}
    seconds_=0;last_=~0u;slot_=0;complete_=false;
}
}
