#pragma once
#include <cstddef>
#include <array>
#include <cstring>
#include "menu_navigation.hpp"

namespace hk3ds {
inline constexpr unsigned saveSlotCount=4;
enum class TitlePage { Title, Slots, Clear, Options, Achievements, Extras };
enum class TitleAction { None, Start, Clear, Quit };
struct TitleMenuState {
    MenuNavigation navigation;
    TitlePage page=TitlePage::Title;
    unsigned selectedSlot=0;
    // Empty slots create a placeholder record, but must still launch the
    // authored opening instead of the Continue restore path.
    bool newGame=false;
    TitleAction input(bool previous,bool next,bool accept,bool back,bool clear,
                      const std::array<bool,saveSlotCount>& occupied,bool profileBackRow=false) {
        navigation.move(int(next)-int(previous),page==TitlePage::Slots?saveSlotCount+unsigned(profileBackRow):page==TitlePage::Title?5:2);
        const bool realSlot=page==TitlePage::Slots&&navigation.selected<saveSlotCount;
        if(back||(page==TitlePage::Slots&&!realSlot&&accept)){
            if(page==TitlePage::Clear){page=TitlePage::Slots;navigation.selected=selectedSlot;}
            else {const unsigned selected=page==TitlePage::Achievements?2:page==TitlePage::Extras?3:0;
                page=TitlePage::Title;navigation.selected=selected;}
        }else if(realSlot&&clear&&occupied[navigation.selected]){
            selectedSlot=navigation.selected;page=TitlePage::Clear;navigation.selected=0;
        }else if(accept){
            if(page==TitlePage::Title){
                if(navigation.selected==4)return TitleAction::Quit;
                constexpr TitlePage pages[]={TitlePage::Slots,TitlePage::Options,TitlePage::Achievements,TitlePage::Extras};
                page=pages[navigation.selected];navigation.selected=0;
            }else if(page==TitlePage::Slots){selectedSlot=navigation.selected;newGame=!occupied[selectedSlot];return TitleAction::Start;}
            else if(page==TitlePage::Clear){
                const bool confirmed=navigation.selected==1;
                page=TitlePage::Slots;navigation.selected=selectedSlot;
                if(confirmed)return TitleAction::Clear;
            }else if(page==TitlePage::Options){page=TitlePage::Title;navigation.selected=1;}
        }
        return TitleAction::None;
    }
};
// Slot one keeps the original single-save filename for automatic migration.
inline const char* saveSlotPath(unsigned slot) {
    constexpr const char* paths[]={"sdmc:/3ds/HK3DS/tutorial.sav","sdmc:/3ds/HK3DS/slot-2.sav",
                                 "sdmc:/3ds/HK3DS/slot-3.sav","sdmc:/3ds/HK3DS/slot-4.sav"};
    return slot<saveSlotCount?paths[slot]:nullptr;
}
inline const char* saveAreaName(const char* scene) {
    // Only areas currently supported by the port. Scene identifiers stay in v5.
    if(!scene||!scene[0])return "Unknown Area";
    if(scene[0]=='T'&&scene[1]=='o')return "Dirtmouth";
    if(scene[0]=='T'&&scene[1]=='u')return "King's Pass";
    if(!std::strncmp(scene,"Fungus1_",8))return "Greenpath";
    return "Forgotten Crossroads";
}
}
