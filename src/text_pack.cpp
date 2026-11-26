#include "text_pack.hpp"

#include <cstring>
#include <utility>

namespace hk3ds::text {
namespace {

// Layout (little endian): "HKTX", version, count, then count sorted pairs of
// (key offset, text offset) from the start of the pack; NUL-terminated strings.
constexpr char magic[4]={'H','K','T','X'};
constexpr std::uint32_t version=1;

struct Pack {
    std::vector<char> bytes;
    std::uint32_t count=0;
    std::uint32_t missing=0;
};
Pack pack;

std::uint32_t word(const std::vector<char>& b,std::size_t at) {
    return std::uint32_t(std::uint8_t(b[at]))|std::uint32_t(std::uint8_t(b[at+1]))<<8|
           std::uint32_t(std::uint8_t(b[at+2]))<<16|std::uint32_t(std::uint8_t(b[at+3]))<<24;
}

bool terminated(const std::vector<char>& b,std::uint32_t offset) {
    return offset<b.size()&&std::memchr(b.data()+offset,0,b.size()-offset)!=nullptr;
}

const char* at(std::uint32_t offset) { return pack.bytes.data()+offset; }
const char* keyOf(std::uint32_t i) { return at(word(pack.bytes,12+i*8)); }
const char* textOf(std::uint32_t i) { return at(word(pack.bytes,16+i*8)); }

}

bool load(std::vector<char> bytes) {
    pack={};
    if(bytes.size()<12||std::memcmp(bytes.data(),magic,4)||word(bytes,4)!=version)return false;
    const std::uint32_t n=word(bytes,8);
    if(n>(bytes.size()-12)/8)return false;
    const char* previous=nullptr;
    for(std::uint32_t i=0;i<n;++i) {
        const std::uint32_t key=word(bytes,12+i*8),text=word(bytes,16+i*8);
        if(!terminated(bytes,key)||!terminated(bytes,text))return false;
        // Binary search needs strictly increasing keys.
        if(previous&&std::strcmp(previous,bytes.data()+key)>=0)return false;
        previous=bytes.data()+key;
    }
    pack.bytes=std::move(bytes);
    pack.count=n;
    return true;
}

bool loaded() { return pack.count!=0; }
std::uint32_t count() { return pack.count; }
std::uint32_t missing() { return pack.missing; }

const char* find(const char* key) {
    if(!key)return nullptr;
    std::uint32_t low=0,high=pack.count;
    while(low<high) {
        const std::uint32_t mid=low+(high-low)/2;
        const int order=std::strcmp(keyOf(mid),key);
        if(!order)return textOf(mid);
        if(order<0)low=mid+1;else high=mid;
    }
    return nullptr;
}

bool isKey(const char* text) { return text&&text[0]==keyPrefix&&text[1]; }

const char* resolve(const char* text) {
    if(!isKey(text))return text;
    if(const char* found=find(text+1))return found;
    ++pack.missing;
    return text;
}

}
