#include "uumuu_graphics.hpp"
#include <cstdio>
#include <limits>

namespace hk3ds {
bool UumuuGraphics::load(const char* root,std::size_t futureBytes){
    release();
    if(!root||futureBytes>std::numeric_limits<std::size_t>::max()-uumuuQuirrelTextureBytes-uumuuGateTextureBytes-uumuuEffectsTextureBytes)return false;
    char path[256];
    const auto readBank=[&](AnimationBank& bank,const char* name){
        const auto size=std::snprintf(path,sizeof(path),"%s/%s-art.bin",root,name);
        return size>0&&size<int(sizeof(path))&&bank.load(path);
    };
    if(!readBank(body_,"uumuu-body")||!readBank(quirrel_,"uumuu-quirrel")||!readBank(gates_,"uumuu-gate")||!readBank(effects_,"uumuu-effects")||
       !bodyRenderer_.loadGuarded(root,body_,"uumuu-body-page-",uumuuBodyPages,uumuuBodyPageCount,
                                  futureBytes+uumuuQuirrelTextureBytes+uumuuGateTextureBytes+uumuuEffectsTextureBytes)||
       !quirrelRenderer_.loadGuarded(root,quirrel_,"uumuu-quirrel-page-",uumuuQuirrelPages,uumuuQuirrelPageCount,
                                     futureBytes+uumuuGateTextureBytes+uumuuEffectsTextureBytes)||
       !gateRenderer_.loadGuarded(root,gates_,"uumuu-gate-page-",uumuuGatePages,uumuuGatePageCount,futureBytes+uumuuEffectsTextureBytes)||
       !effectsRenderer_.loadGuarded(root,effects_,"uumuu-effects-page-",uumuuEffectsPages,uumuuEffectsPageCount,futureBytes)){
        release();return false;
    }
    return true;
}
void UumuuGraphics::release(){
    bodyRenderer_.release();quirrelRenderer_.release();gateRenderer_.release();effectsRenderer_.release();body_={};quirrel_={};gates_={};effects_={};
}
}
