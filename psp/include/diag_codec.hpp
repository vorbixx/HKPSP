#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hkpsp::diag {
inline constexpr unsigned history=90;
inline constexpr unsigned width=480,height=272;
inline constexpr std::size_t poolBytes=26*1024*1024;
inline constexpr std::size_t encodedBound=width*height*4+22;
// QOI RGB, lossless for all displayed color bits. Input is PSP RGBA8888;
// alpha and stride padding are not displayed and are deliberately excluded.
std::size_t encodeRgb(const std::uint32_t* rgba,unsigned w,unsigned h,unsigned stride,
                      unsigned char* out,std::size_t capacity);
struct EncodedFrame {std::size_t offset=0,size=0;unsigned slot=0;};
// Caller owns storage, allowing PSP kernel partition memory outside libc's heap.
// Capacity failure leaves all records intact; the caller must report it.
class Ring {
    unsigned char* storage;
    std::size_t capacity,used=0,next=0;
    unsigned head=0,count=0;
    std::array<EncodedFrame,history> records{};
public:
    Ring(unsigned char* data,std::size_t bytes):storage(data),capacity(bytes){}
    unsigned push(const unsigned char* data,std::size_t bytes);
    unsigned size()const{return count;}
    std::size_t bytes()const{return used;}
    const EncodedFrame& oldest(unsigned n)const;
    const unsigned char* first(const EncodedFrame& r)const{return storage+r.offset;}
    std::size_t firstSize(const EncodedFrame& r)const;
    const unsigned char* second()const{return storage;}
};
}
