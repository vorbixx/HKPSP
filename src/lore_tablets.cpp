#include "lore_tablets.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
bool LoreTabletBank::load(const char* path)
{
    auto* file=std::fopen(path,"rb");if(!file)return false;
    LoreTabletBank next;
    char magic[4];bool ok=std::fread(magic,1,4,file)==4&&!std::memcmp(magic,"HKL1",4);
    for(auto& t:next.tablets){
        ok=ok&&std::fread(&t.owner,4,1,file)==1&&std::fread(&t.range,4,4,file)==4&&std::fread(&t.pageCount,4,1,file)==1;
        ok=ok&&t.owner&&t.pageCount>=1&&t.pageCount<=LoreTablet::sourcePages&&
            std::isfinite(t.range.left)&&std::isfinite(t.range.bottom)&&std::isfinite(t.range.right)&&std::isfinite(t.range.top)&&
            t.range.left<t.range.right&&t.range.bottom<t.range.top;
        if(!ok)break;
        for(unsigned page=0;page<t.pageCount;++page){
            unsigned size=0;
            ok=std::fread(&size,4,1,file)==1&&size>0&&size<t.pages[page].size();
            if(!ok)break;
            ok=std::fread(t.pages[page].data(),1,size,file)==size;
            for(unsigned i=0;i<size&&ok;++i)ok=t.pages[page][i]=='\n'||(t.pages[page][i]>=32&&t.pages[page][i]<=126);
        }
        if(!ok)break;
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(ok)*this=next;
    return ok;
}
void LoreInspection::reset()
{
    phase_=InspectionPhase::Idle;nearby_=active_=-1;page_=serial_=0;
    age_=time_=characters_=0;speed_=65;++revision_;
}
const char* LoreInspection::text() const
{return bank_&&active_>=0?bank_->tablets[active_].pages[page_].data():"";}
bool LoreInspection::pageComplete() const {return visibleCharacters()>=std::strlen(text());}
float LoreInspection::alpha() const
{
    if(phase_==InspectionPhase::Opening)return std::min(1.f,time_/.5f);
    if(phase_==InspectionPhase::Reading)return 1;
    if(phase_==InspectionPhase::Down)return std::max(0.f,1-time_/.3f);
    return 0;
}
void LoreInspection::down(TutorialPrompts& prompts)
{phase_=InspectionPhase::Down;time_=0;prompts.endTabletFocus();++revision_;}
void LoreInspection::step(float dt,const InspectionInput& input,TutorialPrompts& prompts)
{
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    age_+=dt;time_+=dt;
    const auto oldNearby=nearby_;nearby_=-1;
    for(unsigned i=0;i<bank_->tablets.size();++i){
        const auto r=bank_->tablets[i].range,h=input.hero;
        if(h.left<r.right&&h.right>r.left&&h.bottom<r.top&&h.top>r.bottom)nearby_=int(i);
    }
    if(nearby_!=oldNearby)++revision_;
    if(input.interrupted&&locked()){down(prompts);phase_=InspectionPhase::Cooldown;return;}
    switch(phase_){
    case InspectionPhase::Idle:
        if(age_+.00001f>=.5f&&nearby_>=0&&input.canInspect&&input.up){
            active_=nearby_;page_=0;characters_=0;speed_=65;
            phase_=InspectionPhase::Opening;time_=0;++serial_;++revision_;prompts.lorePromptUp();
        }
        break;
    case InspectionPhase::Opening:
        ++revision_;
        if(time_+.00001f>=.85f){
            phase_=InspectionPhase::Reading;time_=0;
            if(owner()==1579)prompts.beginTabletFocus();
        }
        break;
    case InspectionPhase::Reading:
        {
        const auto previousVisible=visibleCharacters();const auto previousPage=page_;
        if(input.down){down(prompts);break;}
        if(input.continuePressed){
            if(!pageComplete())speed_=200;
            else if(page_+1<bank_->tablets[active_].pageCount){++page_;characters_=0;speed_=65;}
            else {down(prompts);break;}
        }
        characters_=std::min(float(std::strlen(text())),characters_+dt*speed_);
        if(previousVisible!=visibleCharacters()||previousPage!=page_)++revision_;
        break;
        }
    case InspectionPhase::Down:
        ++revision_;
        if(time_+.00001f>=.5f){phase_=InspectionPhase::TurnBack;time_=0;}
        break;
    case InspectionPhase::TurnBack:
        // Original TurnFromBG: two frames at 12 fps; regain control on completion.
        if(time_+.00001f>=2.f/12){phase_=InspectionPhase::Cooldown;time_=0;++revision_;}
        break;
    case InspectionPhase::Cooldown:
        if(time_+.00001f>=1){phase_=InspectionPhase::Idle;time_=0;++revision_;}
        break;
    }
}
}
