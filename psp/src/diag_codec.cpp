#include "diag_codec.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hkpsp::diag {
std::size_t encodeRgb(const std::uint32_t* rgba,unsigned w,unsigned h,unsigned stride,
                      unsigned char* out,std::size_t capacity){
    if(!rgba||!out||!w||!h||stride<w||capacity<22||
       std::size_t(w)*h>(capacity-22)/4)throw std::runtime_error("Diagnostic encoder capacity");
    std::size_t n=0;
    const auto byte=[&](unsigned v){out[n++]=static_cast<unsigned char>(v);};
    const auto big=[&](unsigned v){byte(v>>24);byte(v>>16);byte(v>>8);byte(v);};
    byte('q');byte('o');byte('i');byte('f');big(w);big(h);byte(3);byte(0);
    // The implicit QOI alpha is 255; RGB chunks never change it.
    std::array<std::uint32_t,64> cache{};
    std::uint32_t previous=0xff000000u;unsigned run=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const auto pixel=rgba[std::size_t(y)*stride+x]|0xff000000u;
        if(pixel==previous){
            if(++run==62||(y+1==h&&x+1==w)){byte(0xc0|(run-1));run=0;}
            continue;
        }
        if(run){byte(0xc0|(run-1));run=0;}
        const int r=pixel&255,g=(pixel>>8)&255,b=(pixel>>16)&255;
        const unsigned hash=(r*3+g*5+b*7+255*11)%64;
        if(cache[hash]==pixel)byte(hash);
        else{
            cache[hash]=pixel;
            const int dr=r-int(previous&255),dg=g-int((previous>>8)&255),db=b-int((previous>>16)&255);
            if(dr>=-2&&dr<=1&&dg>=-2&&dg<=1&&db>=-2&&db<=1)
                byte(0x40|((dr+2)<<4)|((dg+2)<<2)|(db+2));
            else if(dg>=-32&&dg<=31&&dr-dg>=-8&&dr-dg<=7&&db-dg>=-8&&db-dg<=7){
                byte(0x80|(dg+32));byte(((dr-dg+8)<<4)|(db-dg+8));
            }else{byte(0xfe);byte(r);byte(g);byte(b);}
        }
        previous=pixel;
    }
    for(unsigned i=0;i<7;++i)byte(0);
    byte(1);return n;
}
unsigned Ring::push(const unsigned char* data,std::size_t bytes){
    if(!data||!bytes||bytes>capacity||used-(count==history?records[head].size:0)>capacity-bytes)
        throw std::runtime_error("Diagnostic 90-frame ring capacity exceeded");
    if(count==history)used-=records[head].size;
    const auto first=std::min(bytes,capacity-next);
    std::memcpy(storage+next,data,first);std::memcpy(storage,data+first,bytes-first);
    const unsigned slot=head;records[head]={next,bytes,slot};
    next=(next+bytes)%capacity;used+=bytes;head=(head+1)%history;
    count=std::min(count+1,history);return slot;
}
const EncodedFrame& Ring::oldest(unsigned n)const{
    if(n>=count)throw std::out_of_range("Diagnostic frame index");
    return records[(head+history-count+n)%history];
}
std::size_t Ring::firstSize(const EncodedFrame& r)const{return std::min(r.size,capacity-r.offset);}
}
