#pragma once
#include "animation.hpp"
#include "sly_shop_data.hpp"
#include "input.hpp"
#include "physics.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>
namespace hk3ds {
// Presentation only; TutorialSession owns the purchase transaction and save.
struct SlyShop {
    float artScale()const{return animationFacingScale("sly-shop/sly-shop-actor",-1);}
    Rect region{};
    bool open=false,prompt=false,confirm=false,yes=true;
    int heldY=0,heldX=0;
    unsigned selected=0,count=0;
    mutable unsigned textPage=0,textPages=1;
    void setVisibleTextPages(unsigned n) const {textPages=n?n:1;if(textPage>=textPages)textPage=textPages-1;}
    std::array<unsigned,8> stock{};
    const char* message="";
    void reset(){const auto savedRegion=region;*this={};region=savedRegion;}
    bool loadRegion(const char* root){
        char path[384];std::snprintf(path,sizeof(path),"%s/sly-shop.bin",root);
        auto* f=std::fopen(path,"rb");if(!f)return false;
        char magic[4];float v[4];
        const bool ok=std::fread(magic,1,4,f)==4&&std::memcmp(magic,"SSP1",4)==0&&std::fread(v,4,4,f)==4;
        std::fclose(f);
        if(!ok)return false;
        for(float n:v)if(!std::isfinite(n))return false;
        if(v[0]>=v[2]||v[1]>=v[3])return false;
        region={v[0],v[1],v[2],v[3]};return true;
    }
    unsigned item() const {return count?stock[selected<count?selected:0]:unsigned(slyItems.size());}
};
}
