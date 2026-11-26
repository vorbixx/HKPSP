#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hk3ds {
struct DreamDialogueBank {
    using Lines=std::array<std::array<char,96>,4>;
    struct Page {std::uint32_t group;Lines lines;};
    std::array<Page,32> pages{};
    unsigned count=0;
    unsigned first(unsigned group) const {
        for(unsigned i=0;i<count;++i)if(pages[i].group==group)return i;
        return count;
    }
    bool load(const char* path){
        count=0;
        auto* file=std::fopen(path,"rb");if(!file)return false;
        char magic[4];std::uint32_t size=0;
        bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"HKDN",4)==0&&
            std::fread(&size,4,1,file)==1&&size>=12&&size<=pages.size();
        unsigned groups=0;
        for(unsigned i=0;ok&&i<size;++i){
            auto& page=pages[i];
            ok=std::fread(&page.group,4,1,file)==1&&page.group<12&&(!i||page.group>=pages[i-1].group);
            if(!ok)break;
            groups|=1u<<page.group;
            for(auto& row:page.lines){
                ok=std::fread(row.data(),1,row.size(),file)==row.size()&&std::memchr(row.data(),0,row.size());
                if(!ok)break;
            }
        }
        ok=ok&&groups==0xfffu&&std::fgetc(file)==EOF;
        std::fclose(file);if(ok)count=size;return ok;
    }
};
}
