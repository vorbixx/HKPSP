#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace hkpsp {
// High-water scratch storage: growth never copies or duplicates existing blocks.
// Power-of-two blocks keep indexed access to a shift, mask and pointer lookup.
template<class T,std::size_t BlockElements> class ChunkedScratch {
    static_assert(BlockElements && !(BlockElements&(BlockElements-1)));
    std::vector<std::unique_ptr<T[]>> blocks_;
    std::size_t size_=0;
public:
    std::size_t size()const{return size_;}
    void grow(std::size_t count){
        if(count<=size_)return;
        const auto blocks=(count-1)/BlockElements+1;
        while(blocks_.size()<blocks)blocks_.push_back(std::make_unique<T[]>(BlockElements));
        size_=count;
    }
    T& operator[](std::size_t index){return blocks_[index/BlockElements][index%BlockElements];}
    const T& operator[](std::size_t index)const{return blocks_[index/BlockElements][index%BlockElements];}
    void append(const T* source,std::size_t count){
        if(count>std::numeric_limits<std::size_t>::max()-size_)
            throw std::length_error("Scratch size overflow");
        auto offset=size_;grow(size_+count);
        while(count){
            const auto part=std::min(count,BlockElements-offset%BlockElements);
            std::copy_n(source,part,&(*this)[offset]);
            source+=part;offset+=part;count-=part;
        }
    }
};
}
