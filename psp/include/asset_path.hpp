#pragma once
#include <string>

namespace hkpsp {
// Same logical grammar as stage-data.ps1: a flat file or one room namespace.
inline bool validAssetName(const std::string& name){
    const auto word=[](const std::string& value){
        if(value.empty())return false;
        for(const char c:value)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                                   (c>='0'&&c<='9')||c=='_'||c=='-'))return false;
        return true;
    };
    std::string leaf=name;
    const auto slash=name.find('/');
    if(slash!=std::string::npos){
        const auto prefix=name.substr(0,slash);
        const auto second=name.find('/',slash+1);
        if((prefix!="rooms"&&prefix!="scenes")||second==std::string::npos||
           !word(name.substr(slash+1,second-slash-1)))return false;
        leaf=name.substr(second+1);
    }
    const auto dot=leaf.find('.');
    if(dot==std::string::npos||!word(leaf.substr(0,dot))||dot+1==leaf.size())return false;
    // Compound extensions such as "hud-visual.psp.bin" (2026-11-18: the HUD fixture loads it through
    // Platform::path and failed); single dots between alphanumeric parts only, so "hero..bin" stays invalid.
    for(std::size_t i=dot+1;i<leaf.size();++i){
        const char c=leaf[i];
        if(c=='.'&&leaf[i-1]!='.'&&i+1<leaf.size())continue;
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')))return false;
    }
    return true;
}
}
