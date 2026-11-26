#pragma once
#include "menu_state.hpp"
#include "draw_list.hpp"
#include "input_replay.hpp"
#include "ui.hpp"
#include <string>

namespace hkpsp {
class Platform;
struct MenuRuntime {
    MenuState state;
    GameplayMenuGuard guard;
    unsigned presented=0;
    unsigned controlSteps=0;
    hk3ds::InputReplay replay;
    UiInputRouter router;
    void loadInput(const std::string& path);
    void loadSettings(const std::string& slotPath);
    void saveSettings();
    UiInput pollUi(Platform& platform);
    bool available(unsigned frames)const{return !frames||presented<frames;}
private:
    std::string settingsPath_;
};
// Keep slot one compatible with M1; use the reference slot-2..4 names beside it.
inline std::string menuSlotPath(const std::string& first,unsigned slot){
    if(first.empty()||slot>=hk3ds::saveSlotCount)return {};
    if(!slot)return first;
    const auto slash=first.find_last_of("/\\");
    return (slash==std::string::npos?std::string{}:first.substr(0,slash+1))+
        "slot-"+std::to_string(slot+1)+".sav";
}
// Black overlay over the scene (and, for the pause screen, the HUD). The pause value 0.8 is measured from
// the original (build/psp/compare/ui/pause-before.png, 2026-11-19); quit-to-title keeps 0.7.
inline constexpr float pauseDimAlpha=.8f;
void appendMenuDim(DrawList& list,float alpha=.7f);
void appendPauseMenu(DrawList&,const hk3ds::PauseMenu&,unsigned, hk3ds::ui::Canvas);
bool presentTitle(Platform& platform,MenuRuntime& menus,unsigned frames);
}
