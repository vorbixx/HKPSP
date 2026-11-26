#include "sector_cache.hpp"
#include "sector_prefetch.hpp"
#include <algorithm>
#include <bitset>
#include <cstring>

namespace hk3ds {
bool roomPageRanges(const AnimationBank& atlas,unsigned page,std::vector<RoomPageRange>& ranges)
{
    ranges.clear();
    if(page>=atlas.pageCount)return false;
    std::bitset<128*128> tiles;
    for(const auto& s:atlas.sprites){
        if(s.page>=atlas.pageCount || s.x>=1024 || s.y>=1024 || !s.width || !s.height ||
           s.width>1024-s.x || s.height>1024-s.y)return false;
        if(s.page!=page)continue;
        const unsigned left=(s.x?s.x-1:0)/8;
        const unsigned right=(std::min<std::uint32_t>(1024,s.x+s.width+1)+7)/8;
        const unsigned top=(s.y?s.y-1:0)/8;
        const unsigned bottom=(std::min<std::uint32_t>(1024,s.y+s.height+1)+7)/8;
        // tex3ds stores rows of 8x8 Morton tiles in PNG top-to-bottom order.
        // A whole RGBA4 tile is contiguous (128 bytes); no texel swizzle needed.
        for(unsigned y=top;y<bottom;++y)for(unsigned x=left;x<right;++x)
            tiles.set(y*128+x);
    }
    for(unsigned i=0;i<tiles.size();){
        if(!tiles[i]){++i;continue;}
        const auto start=i;
        while(i<tiles.size()&&tiles[i])++i;
        ranges.push_back({start*128u,(i-start)*128u});
    }
    return true;
}
bool decodeRoomPage(const std::vector<std::uint8_t>& file,std::vector<std::uint8_t>& output,unsigned width)
{
    unsigned char header[]={1,0,63,4,0,0,4,0,4,0,0,0,4,0,4,0,0,17,0,0,32};
    if((width!=512&&width!=1024)||file.size()<sizeof(header))return false;
    if(width==512){header[2]=54;header[6]=header[8]=2;header[20]=8;}
    if(file[3]!=0&&file[3]!=4)return false;
    if(file[3]==0){header[3]=0;header[20]*=2;}
    if(file[17]!=0&&file[17]!=17)return false;
    header[17]=file[17];
    if(file.size()<sizeof(header)||std::memcmp(file.data(),header,sizeof(header)))return false;
    const std::size_t size=std::size_t(width)*width*(file[3]==0?4:2);
    if(!header[17]){
        if(file.size()!=sizeof(header)+size)return false;
        std::vector<std::uint8_t> next(file.begin()+sizeof(header),file.end());
        output=std::move(next);return true;
    }
    std::vector<std::uint8_t> next(size);
    std::size_t at=sizeof(header),out=0;
    while(out<size){
        if(at>=file.size())return false;
        const unsigned flags=file[at++];
        for(unsigned bit=128;bit&&out<size;bit>>=1){
            if(at>=file.size())return false;
            if(!(flags&bit)){next[out++]=file[at++];continue;}
            unsigned a=file[at++],length=a>>4,distance;
            if(length<=1){
                if(at+2+(length==1)>file.size())return false;
                const unsigned b=file[at++],c=file[at++];
                if(length==0){length=((a&15)<<4)+(b>>4)+17;distance=((b&15)<<8)+c+1;}
                else {const unsigned d=file[at++];length=((a&15)<<12)+(b<<4)+(c>>4)+273;distance=((c&15)<<8)+d+1;}
            }else{
                if(at>=file.size())return false;
                length+=1;distance=((a&15)<<8)+file[at++]+1;
            }
            if(distance>out||length>size-out)return false;
            // Seed one repetition without overlap, then double the known prefix.
            // LZ matches may overlap; memcpy is only used on disjoint ranges.
            const auto start=out;
            auto copied=std::min(length,distance);
            std::memcpy(next.data()+start,next.data()+start-distance,copied);
            while(copied<length){
                const auto chunk=std::min(copied,length-copied);
                std::memcpy(next.data()+start+copied,next.data()+start,chunk);
                copied+=chunk;
            }
            out+=length;
        }
    }
    // tex3ds may align the compressed stream with up to three zero bytes.
    if(file.size()-at>3)return false;
    while(at<file.size())if(file[at++])return false;
    output=std::move(next);return true;
}
std::size_t SectorData::bytes() const
{
    std::size_t total=sizeof(SectorData)+visuals.draws.capacity()*sizeof(RoomDraw)+
        visuals.cameraBounds.capacity()*sizeof(RoomCameraBounds)+
        visuals.atlas.sprites.capacity()*sizeof(SpriteFrame)+visuals.atlas.clips.capacity()*sizeof(Clip)+
        visuals.atlas.frames.capacity()*sizeof(std::uint32_t)+pages.capacity()*sizeof(pages[0]);
    for(const auto& page:pages)total+=page.capacity();
    return total;
}
bool SectorCache::contains(int x,int y) const
{
    for(const auto& entry:entries_)if(entry->x==x&&entry->y==y)return true;
    return false;
}
SectorData* SectorCache::find(int x,int y)
{
    for(auto it=entries_.begin();it!=entries_.end();++it)if((*it)->x==x&&(*it)->y==y){
        auto* result=it->get();entries_.splice(entries_.end(),entries_,it);return result;
    }
    return nullptr;
}
bool SectorCache::reserve(std::size_t bytes,const SectorTarget* keep,std::size_t count)
{
    if(bytes>budget_){if(observer)observer(1,-1,-1,bytes,observerContext);return false;}
    const auto evictable=[&](const auto& entry){
        if(entry.get()==active_)return false;
        for(std::size_t i=0;i<count;++i)if(entry->x==keep[i].x&&entry->y==keep[i].y)return false;
        return true;
    };
    auto retained=used_;
    for(const auto& entry:entries_)if(evictable(entry))retained-=entry->bytes();
    if(retained>budget_-bytes){if(observer)observer(2,-1,-1,bytes,observerContext);return false;}
    while(used_>budget_-bytes){
        auto it=std::find_if(entries_.begin(),entries_.end(),evictable);
        if(observer)observer(3,(*it)->x,(*it)->y,(*it)->bytes(),observerContext);
        used_-=(*it)->bytes();entries_.erase(it);
    }
    return true;
}
SectorData* SectorCache::insert(std::unique_ptr<SectorData> entry)
{
    if(!entry||!reserve(entry->bytes()))return nullptr;
    auto* result=entry.get();used_+=entry->bytes();entries_.push_back(std::move(entry));return result;
}
}
