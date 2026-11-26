#pragma once
#include "ui_input.hpp"
#include "pause_menu.hpp"
#include "menu.hpp"

namespace hkpsp {
// Presentation state only: save I/O, simulation and drawing belong to their callers.
class MenuState {
public:
    enum class Screen { Game, Pause, Title };
    enum class Action { None, SettingsChanged, QuitToTitle, StartGame, ClearSlot, ExitGame };
    Screen screen;
    hk3ds::PauseMenu pause{hk3ds::PauseMenu::Profile::PlayStationPortable};
    hk3ds::TitleMenuState title;
    explicit MenuState(Screen initial=Screen::Title):screen(initial),simulate_(initial==Screen::Game){}
    bool simulate()const{return simulate_;}
    Action update(const UiInput& input,const std::array<bool,hk3ds::saveSlotCount>& occupied={}){
        const bool wasGame=screen==Screen::Game;
        const auto action=route(input,occupied);
        // Opening and closing a menu both consume the whole presentation frame.
        simulate_=wasGame&&screen==Screen::Game;
        return action;
    }
private:
    bool simulate_=false;
    Action route(const UiInput& input,const std::array<bool,hk3ds::saveSlotCount>& occupied){
        const bool confirm=input.pressed&UiCircle,back=input.pressed&UiCross,start=input.pressed&UiStart;
        if(screen==Screen::Game){
            if(start){pause.update(0,0,false,false,true);screen=Screen::Pause;}
            return Action::None;
        }
        if(screen==Screen::Pause||title.page==hk3ds::TitlePage::Options){
            const auto action=pause.update(input.vertical(),input.horizontal(),confirm,back,start);
            if(action==hk3ds::PauseMenu::Action::QuitToMenu){
                pause.screen=hk3ds::PauseMenu::Screen::Closed;
                pause.restorePspHud();
                title={};screen=Screen::Title;return Action::QuitToTitle;
            }
            if(screen==Screen::Pause&&!pause.active())screen=Screen::Game;
            if(screen==Screen::Title&&pause.screen==hk3ds::PauseMenu::Screen::MainMenu){
                pause.screen=hk3ds::PauseMenu::Screen::Closed;
                title.page=hk3ds::TitlePage::Title;title.navigation.selected=1;
            }
            return action==hk3ds::PauseMenu::Action::SettingsChanged?Action::SettingsChanged:Action::None;
        }
        const int navigation=title.page==hk3ds::TitlePage::Clear?-input.horizontal():input.vertical();
        const auto action=title.input(navigation<0,navigation>0,confirm,back,
                                      input.pressed&UiTriangle,occupied,true);
        if(title.page==hk3ds::TitlePage::Options)pause.openOptions(true);
        if(action==hk3ds::TitleAction::Start){screen=Screen::Game;return Action::StartGame;}
        if(action==hk3ds::TitleAction::Clear)return Action::ClearSlot;
        if(action==hk3ds::TitleAction::Quit)return Action::ExitGame;
        return Action::None;
    }
};
}
