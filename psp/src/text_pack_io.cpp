#include "text_pack_io.hpp"

#include "data_format.hpp"
#include "../include/platform.hpp"
#include "text_pack.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace hkpsp {
namespace {

constexpr const char* rebuild=
    "Run tools/psp/build-steam.ps1 again and copy the new data folder to the memory stick.";

bool readAll(const std::string& path,std::vector<char>& bytes) {
    const std::unique_ptr<std::FILE,int(*)(std::FILE*)> file(std::fopen(path.c_str(),"rb"),std::fclose);
    if(!file)return false;
    char buffer[4096];
    for(std::size_t n;(n=std::fread(buffer,1,sizeof buffer,file.get()))>0;)bytes.insert(bytes.end(),buffer,buffer+n);
    return !std::ferror(file.get());
}

}

std::string dataFormatError(const std::string& formatFile) {
    std::vector<char> bytes;
    if(!readAll(formatFile,bytes))return std::string("The game data is from an older HKPSP version (no format.txt). ")+rebuild;
    unsigned value=0;bool digits=false;
    for(char c:bytes){if(c<'0'||c>'9')break;value=value*10+unsigned(c-'0');digits=true;}
    if(!digits)return std::string("The game data has an unreadable format.txt. ")+rebuild;
    if(value!=hkpsp::dataFormat)
        return "The game data has format "+std::to_string(value)+", this EBOOT needs format "+
               std::to_string(hkpsp::dataFormat)+". "+rebuild;
    return {};
}

void loadGameText(const Platform& platform) {
    if(hk3ds::text::loaded())return;
    if(const auto error=dataFormatError(platform.path("format.txt"));!error.empty())throw std::runtime_error(error);
    std::vector<char> bytes;
    if(!readAll(platform.path("text.bin"),bytes)||!hk3ds::text::load(std::move(bytes)))
        throw std::runtime_error(std::string("The game text (text.bin) is missing or damaged. ")+rebuild);
}

}
