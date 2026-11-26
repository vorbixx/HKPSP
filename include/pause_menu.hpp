#pragma once
#include "menu_navigation.hpp"
#include "system_settings.hpp"
namespace hk3ds {
class PauseMenu {
public:
    enum class Profile { Nintendo3DS, PlayStationPortable };
    enum class Screen { Closed, Pause, Options, Game, Audio, Video, Controls, Keyboard, MainMenu };
    enum class Action { None, SettingsChanged, QuitToMenu, ExitGame };
    enum GameRow : unsigned { Language, CameraShake, HudAppearance, BackerCredits, ResetGame, GameBack, GameRowCount };
    Screen screen=Screen::Closed;
    MenuNavigation rows;
    SystemSettings settings;
    bool saveFailed=false;
    explicit PauseMenu(Profile profile=Profile::Nintendo3DS):profile_(profile){}
    bool active()const{return screen!=Screen::Closed;}
    bool atMainMenu()const{return screen==Screen::MainMenu||optionsFromMain_;}
    bool adjusting()const{return screen==Screen::Audio||screen==Screen::Video;}
    const char* label(unsigned row)const {
        static constexpr const char* options[]={"Game","Audio","Video","Controller","Back"};
        static constexpr const char* pspOptions[]={"Game","Audio","Controller","Back"};
        static constexpr const char* audio[]={"Music Volume","Sound Volume","Back"};
        static constexpr const char* pspAudio[]={"Master Volume","Sound Volume","Music Volume","Reset Defaults","Back"};
        static constexpr const char* video[]={"Graphics","Back"};
        static constexpr const char* game[]={"Language","Back"};
        static constexpr const char* pspGame[]={"Language","Camera Shake","HUD Appearance","Backer Credits","Reset Defaults","Back"};
        if(row>=count())return "";
        if(screen==Screen::Options)return profile_==Profile::PlayStationPortable?pspOptions[row]:options[row];
        if(screen==Screen::Audio)return profile_==Profile::PlayStationPortable?pspAudio[row]:audio[row];
        if(screen==Screen::Video)return video[row];
        if(screen==Screen::Game)return profile_==Profile::PlayStationPortable?pspGame[row]:game[row];
        if(screen==Screen::Controls||screen==Screen::Keyboard)return "Back";
        return row==0?"Continue":row==1?"Options":atMainMenu()?"Quit Game":"Quit to Menu";
    }
    const char* description()const {
        const unsigned row=rows.selected;
        if(screen==Screen::Options){
            if(profile_==Profile::PlayStationPortable){
                static constexpr const char* help[]={"Language and game settings.","Adjust music and sound volume.","View the PSP button controls.","Return to the previous menu."};
                return row<4?help[row]:"";
            }
            static constexpr const char* help[]={"Language and game settings.","Adjust music and sound volume.","Choose the graphics profile.","View the 3DS button controls.","Return to the previous menu."};
            return row<5?help[row]:"";
        }
        if(screen==Screen::Audio&&row<2)return row?"Set the volume of sound effects.":"Set the volume of music.";
        if(screen==Screen::Video&&row==0)return "Graphics changes apply next session.";
        if(screen==Screen::Game&&row==0)return "English is the available language.";
        if(screen==Screen::Controls)return profile_==Profile::PlayStationPortable?"Use the PSP buttons, analog nub or D-pad to explore Hallownest.":"Use the buttons and Circle Pad to explore Hallownest.";
        if(screen==Screen::Keyboard)return "Use the 3DS controls. Keyboard input is unavailable.";
        if(screen!=Screen::Pause&&screen!=Screen::MainMenu)return "Return to Options.";
        return row==0?"Return to your journey.":row==1?"Adjust the game settings.":"Leave the current game.";
    }
    unsigned count()const {
        switch(screen){
        case Screen::Options:return profile_==Profile::PlayStationPortable?4:5;
        case Screen::Game:return profile_==Profile::PlayStationPortable?unsigned(GameRowCount):2u;
        case Screen::Video:return 2;
        case Screen::Audio:return profile_==Profile::PlayStationPortable?5:3;
        case Screen::Controls:case Screen::Keyboard:return 1;
        default:return 3;
        }
    }
    unsigned firstRow()const{return 0;}
    bool rowVisible(unsigned row)const{return screen!=Screen::Options||(row>=firstRow()&&row<firstRow()+5);}
    float rowY(unsigned row)const {
        return screen==Screen::Controls||screen==Screen::Keyboard?195.f:
            screen==Screen::Options?64.f+(int(row)-int(firstRow()))*28.f:
            screen==Screen::Game||adjusting()?82.f+row*34.f:68.f+row*40.f;
    }
    void openOptions(bool fromMain=false){optionsFromMain_=fromMain;open(Screen::Options);}
    unsigned pspAudioVolume(unsigned row)const {
        return row==0?settings.masterVolume:row==1?settings.soundVolume:settings.musicVolume;
    }
    bool pspHudHidden()const{return pspHudHidden_;}
    void restorePspHud(){pspHudHidden_=false;}
    const char* pspGameValue(unsigned row)const {
        constexpr const char* shake[]={"On","Reduced","Off"};
        switch(row){
        case Language:return "English";
        case CameraShake:return shake[unsigned(settings.cameraShake)];
        case HudAppearance:return pspHudHidden_?"Off":settings.hudAppearance==SystemSettings::HudAppearance::Large?"Large":"Small";
        case BackerCredits:return settings.backerCredits?"On":"Off";
        default:return "";
        }
    }
    // Native 320x240 lower LCD coordinates, shared with the renderer.
    Action tap(int x,int y) {
        if(!active()||x<20||x>=300||y<0||y>=240)return Action::None;
        for(unsigned i=0;i<count();++i)if(rowVisible(i)&&y>=rowY(i)-16&&y<rowY(i)+10){
            rows.selected=i;
            const bool slider=(screen==Screen::Audio&&i<2)||(screen==Screen::Video&&i==0);
            return update(0,slider?(x<160?-1:1):0,true,false,false);
        }
        return Action::None;
    }
    Action update(int vertical,int horizontal,bool confirm,bool back,bool start) {
        if(screen==Screen::Closed){if(start)open(Screen::Pause);return Action::None;}
        if(start&&!atMainMenu()){open(Screen::Closed);return Action::None;}
        if(back){
            unsigned category=0;
            switch(screen){
            case Screen::Game:category=0;break;
            case Screen::Audio:category=1;break;
            case Screen::Video:category=2;break;
            case Screen::Controls:category=profile_==Profile::PlayStationPortable?2:3;break;
            case Screen::Keyboard:category=4;break;
            case Screen::Options:{const bool main=optionsFromMain_;optionsFromMain_=false;open(main?Screen::MainMenu:Screen::Pause);rows.selected=1;return Action::None;}
            case Screen::Pause:open(Screen::Closed);return Action::None;
            default:return Action::None;
            }
            open(Screen::Options);rows.selected=category;return Action::None;
        }
        rows.move(vertical,count());
        if(screen==Screen::Options){
            if(confirm){
                if(profile_==Profile::PlayStationPortable){
                    constexpr Screen pages[]={Screen::Game,Screen::Audio,Screen::Controls};
                    if(rows.selected<3)open(pages[rows.selected]);
                    else return update(0,0,false,true,false);
                    return Action::None;
                }
                constexpr Screen pages[]={Screen::Game,Screen::Audio,Screen::Video,Screen::Controls};
                if(rows.selected<4)open(pages[rows.selected]);
                else return update(0,0,false,true,false);
            }
        }else if(screen==Screen::Audio){
            if(profile_==Profile::PlayStationPortable){
                if(horizontal&&rows.selected<3){
                    auto& value=rows.selected==0?settings.masterVolume:rows.selected==1?settings.soundVolume:settings.musicVolume;
                    const int next=int(value)+(horizontal<0?-1:1);
                    if(next>=0&&next<=10){value=static_cast<unsigned char>(next);return Action::SettingsChanged;}
                }
                if(confirm&&rows.selected==3&&(settings.masterVolume!=10||settings.musicVolume!=10||settings.soundVolume!=10)){
                    settings.masterVolume=settings.musicVolume=settings.soundVolume=10;
                    return Action::SettingsChanged;
                }
                if(confirm&&rows.selected==4)return update(0,0,false,true,false);
                return Action::None;
            }
            if(horizontal&&rows.selected<2){
                auto& value=rows.selected?settings.soundVolume:settings.musicVolume;
                const int next=int(value)+(horizontal<0?-1:1);
                if(next>=0&&next<=10){value=static_cast<unsigned char>(next);return Action::SettingsChanged;}
            }
            if(confirm&&rows.selected==2)return update(0,0,false,true,false);
        }else if(screen==Screen::Video){
            if(rows.selected==0&&(horizontal||confirm)){
                const int delta=horizontal<0?-1:1;
                settings.graphics=static_cast<GraphicsMode>((int(settings.graphics)+delta+3)%3);
                return Action::SettingsChanged;
            }
            if(confirm&&rows.selected==1)return update(0,0,false,true,false);
        }else if(screen==Screen::Game){
            if(profile_==Profile::PlayStationPortable)return updatePspGame(horizontal,confirm);
            if(confirm&&rows.selected==1)return update(0,0,false,true,false);
        }else if(screen==Screen::Controls||screen==Screen::Keyboard){
            if(confirm)return update(0,0,false,true,false);
        }else if(confirm){
            if(rows.selected==0)open(Screen::Closed);
            else if(rows.selected==1)openOptions(screen==Screen::MainMenu);
            else if(screen==Screen::Pause){open(Screen::MainMenu);return Action::QuitToMenu;}
            else return Action::ExitGame;
        }
        return Action::None;
    }
private:
    Profile profile_;
    bool optionsFromMain_=false;
    bool pspHudHidden_=false;
    void open(Screen next){screen=next;rows.selected=0;}
    Action updatePspGame(int horizontal,bool confirm){
        using Shake=SystemSettings::CameraShake;
        using Hud=SystemSettings::HudAppearance;
        const unsigned delta=horizontal<0?2:1;
        switch(rows.selected){
        case CameraShake:
            if(!horizontal)break;
            settings.cameraShake=static_cast<Shake>((unsigned(settings.cameraShake)+delta)%3);
            return Action::SettingsChanged;
        case HudAppearance:
            if(!horizontal)break;
            {
                const unsigned next=((pspHudHidden_?2:unsigned(settings.hudAppearance))+delta)%3;
                pspHudHidden_=next==2;
                if(pspHudHidden_)break;
                settings.hudAppearance=static_cast<Hud>(next);
                return Action::SettingsChanged;
            }
        case BackerCredits:
            if(!horizontal)break;
            settings.backerCredits=!settings.backerCredits;
            return Action::SettingsChanged;
        case ResetGame:
            if(!confirm)break;
            if(!pspHudHidden_&&settings.cameraShake==Shake::On&&settings.hudAppearance==Hud::Small&&!settings.backerCredits)break;
            restorePspHud();settings.cameraShake=Shake::On;settings.hudAppearance=Hud::Small;settings.backerCredits=false;
            return Action::SettingsChanged;
        case GameBack:
            if(confirm)return update(0,0,false,true,false);
            break;
        }
        return Action::None;
    }
};
}
