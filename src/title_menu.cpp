#include "build_config.hpp"
#include "text_capture.hpp"
#include "menu.hpp"
#include "menu_renderer.hpp"
#include "save.hpp"
#include "pause_menu.hpp"
#include "hud_renderer.hpp"
#include "diagnostic_overlay.hpp"
#include "input_replay.hpp"
#include <3ds.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern "C" volatile std::uint32_t hk3ds_test_shutdown;
extern "C" { volatile std::uint32_t hk3ds_test_menu_frame=0; }

namespace hk3ds {
namespace {
// Menu owns NDSP only before gameplay AudioPlayer is constructed/loaded.
class TitleMusic {
    FILE* file_=nullptr;
    void* memory_=nullptr;
    bool initialized_=false;
    std::array<ndspWaveBuf,2> buffers_{};
    static constexpr unsigned bytes=32768;
public:
    void volume(unsigned value){if(initialized_)ndspSetMasterVol(.7f*value/10.f);}
    void load(){
        file_=std::fopen("romfs:/title-music.wav","rb");if(!file_)return;
        unsigned char header[44]{};
        if(std::fread(header,1,44,file_)!=44||std::memcmp(header,"RIFF",4)||std::memcmp(header+8,"WAVE",4))return;
        if(R_FAILED(ndspInit()))return;
        initialized_=true;
        // Approximation: title mixer gain; the native menu bus was not measured.
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);ndspSetMasterVol(.7f);
        ndspChnReset(0);ndspChnSetInterp(0,NDSP_INTERP_LINEAR);ndspChnSetRate(0,22050);
        ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16);
        float mix[12]{};mix[0]=mix[1]=1;ndspChnSetMix(0,mix);
        memory_=linearAlloc(bytes*2);if(!memory_)return;
        for(unsigned i=0;i<2;++i)buffers_[i].data_vaddr=static_cast<char*>(memory_)+i*bytes;
        update();
    }
    void update(){
        if(!memory_)return;
        for(auto& buffer:buffers_)if(buffer.status==NDSP_WBUF_FREE||buffer.status==NDSP_WBUF_DONE){
            auto* data=static_cast<unsigned char*>(const_cast<void*>(buffer.data_vaddr));
            const auto read=std::fread(data,1,bytes,file_);
            if(read<bytes){
                std::fseek(file_,44,SEEK_SET);
                const auto extra=std::fread(data+read,1,bytes-read,file_);
                if(read+extra!=bytes){std::memset(data+read+extra,0,bytes-read-extra);}
            }
            DSP_FlushDataCache(data,bytes);buffer.nsamples=bytes/4;
            ndspChnWaveBufAdd(0,&buffer);
        }
    }
    ~TitleMusic(){
        if(initialized_)ndspChnWaveBufClear(0);
        if(memory_)linearFree(memory_);
        if(initialized_)ndspExit();
        if(file_)std::fclose(file_);
    }
};
using Page=TitlePage;
bool eraseSlot(unsigned slot){
    // Include recovery and in-flight names: an erased slot must never resurrect.
    return TutorialSave::clear(saveSlotPath(slot));
}
}
int runTitleMenu(C3D_RenderTarget* top,C3D_RenderTarget* bottom,HudRenderer& hud,bool* newGame){
    if(newGame)*newGame=false;
    mkdir("sdmc:/3ds",0777);mkdir("sdmc:/3ds/HK3DS",0777);
    MenuRenderer renderer;renderer.load();TitleMusic music;music.load();
    std::array<TutorialSave,saveSlotCount> saves{};
    std::array<bool,saveSlotCount> exists{};
    const auto refresh=[&](){for(unsigned i=0;i<saveSlotCount;++i)exists[i]=saves[i].load(saveSlotPath(i));};
    refresh();TitleMenuState state;
    PauseMenu options;
    constexpr const char* settingsPath="sdmc:/3ds/HK3DS/system.sav";
    options.settings.load(settingsPath);music.volume(options.settings.musicVolume);
    auto& navigation=state.navigation;auto& page=state.page;auto& selectedSlot=state.selectedSlot;
    const char* error=nullptr;
    const auto start=svcGetSystemTick();int result=-1;
    if constexpr(diagnosticsEnabled)std::printf("\x1b[2J\x1b[HHOLLOW KNIGHT\n\nD-Pad / Circle Pad: select\nA: confirm    B: back\n\nChoose Start Game to select a save.\n");
    while(aptMainLoop()){
        if constexpr(diagnosticsEnabled)if(hk3ds_test_shutdown==0x484b4558u)break;
        hidScanInput();const auto keys=hidKeysDown();music.update();
        // The title screen is the first state after launch: the chord must work here too.
        {const double now=double(svcGetSystemTick())/SYSCLOCK_ARM11;
        if(hk3ds::diagnosticChordPoll(hk3ds::consoleOverlay,hk3ds::diagnosticComboHeld(hidKeysHeld()),now))hk3ds::toggleDiagnosticLogging(now);}
        if(keys&(KEY_A|KEY_B|KEY_X))error=nullptr;
        TitleAction action=TitleAction::None;
        if(page==Page::Options){
            touchPosition touch{};if(keys&KEY_TOUCH)hidTouchRead(&touch);
            const auto changed=(keys&KEY_TOUCH)?options.tap(touch.px,touch.py):options.update(int(bool(keys&(KEY_DOWN|KEY_CPAD_DOWN)))-int(bool(keys&(KEY_UP|KEY_CPAD_UP))),
                int(bool(keys&(KEY_RIGHT|KEY_CPAD_RIGHT)))-int(bool(keys&(KEY_LEFT|KEY_CPAD_LEFT))),keys&KEY_A,keys&KEY_B,false);
            if(changed==PauseMenu::Action::SettingsChanged){
                options.saveFailed=!options.settings.write(settingsPath);music.volume(options.settings.musicVolume);
            }
            if(options.screen==PauseMenu::Screen::MainMenu||options.screen==PauseMenu::Screen::Pause||!options.active()){
                page=Page::Title;navigation.selected=1;
            }
        }else {
            action=state.input(keys&(KEY_UP|KEY_CPAD_UP),keys&(KEY_DOWN|KEY_CPAD_DOWN),
                               keys&KEY_A,keys&KEY_B,keys&KEY_X,exists);
            if(page==Page::Options){options.openOptions(true);}
        }
        if(action==TitleAction::Quit)break;
        if(action==TitleAction::Start){
            if(exists[selectedSlot]||TutorialSave{}.write(saveSlotPath(selectedSlot))){
                if(newGame)*newGame=state.newGame;
                result=static_cast<int>(selectedSlot);break;
            }
            error="Could not write save. Check SD card.";
        }else if(action==TitleAction::Clear){
            if(!eraseSlot(selectedSlot))error="Could not clear save.";
            refresh();
        }
        const auto seconds=static_cast<float>(svcGetSystemTick()-start)/SYSCLOCK_ARM11;
        renderer.beginText();C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top,C2D_Color32(5,9,18,255));C2D_SceneBegin(top);
        renderer.backdrop(seconds,true,true);
        if(page==Page::Options)hud.drawMenuDetail(options);
        else {
            const char* name="HOLLOW KNIGHT";const char* description="";char value[96]{};
            if(page==Page::Title){
                static constexpr const char* labels[]={"Start Game","Options","Achievements","Extras","Quit Game"};
                static constexpr const char* help[]={"Begin a journey or continue a saved profile.","Adjust settings and view the controls.","View your achievements.","Additional Hollow Knight content.","Close the game."};
                name=labels[navigation.selected];description=help[navigation.selected];
            }else if(page==Page::Slots||page==Page::Clear){
                const auto slot=page==Page::Slots?navigation.selected:selectedSlot;
                name=exists[slot]?saveAreaName(saves[slot].scene):"New Game";
                std::snprintf(value,sizeof(value),"Profile %u%s",slot+1,exists[slot]?"":" - Empty");
                if(exists[slot]){
                    std::snprintf(value,sizeof(value),"Profile %u   %lu Geo   %u Masks",slot+1,static_cast<unsigned long>(saves[slot].geo),5+unsigned(saves[slot].collectibles.maskShards/4));
                    description=page==Page::Clear?"Clear all saved progress in this profile?":"Continue your journey in Hallownest.";
                }else description="Begin a new journey into Hallownest.";
            }else {name=page==Page::Achievements?"Achievements":"Extras";description=page==Page::Achievements?"Achievements are not available.":"Extras are not available.";}
            ui::detail(hud.uiCanvas(),name,value,description,{8,80,384,152},false);
        }
        textcapture::select(1,true);
        hud.drawBottomPanel(bottom);
        const auto c=hud.uiCanvas();
        if(page==Page::Options)hud.drawMenu(bottom,options);
        else {
            ui::title(c,page==Page::Title?"HOLLOW KNIGHT":page==Page::Slots?"Select Profile":page==Page::Achievements?"Achievements":page==Page::Extras?"Extras":"Clear this profile?",{8,8,304,40});
            if(page==Page::Title){
                const char* labels[]={"Start Game","Options","Achievements","Extras","Quit Game"};
                for(unsigned i=0;i<5;++i)ui::listRow(c,{16,56+i*28.f,288,28},labels[i],nullptr,navigation.selected==i);
            }else if(page==Page::Slots){
                for(unsigned i=0;i<saveSlotCount;++i){
                    char label[96],value[24];
                    std::snprintf(label,sizeof(label),"%u. %s",i+1,exists[i]?saveAreaName(saves[i].scene):"New Game");
                    std::snprintf(value,sizeof(value),"%lu Geo",static_cast<unsigned long>(saves[i].geo));
                    ui::listRow(c,{16,56+i*36.f,288,32},label,exists[i]?value:nullptr,navigation.selected==i);
                }
            }else if(page==Page::Achievements||page==Page::Extras){
                ui::wrappedText(c,page==Page::Achievements?"Achievements are not available.":"Extras are not available.",{20,68,280,80});
            }else if(page==Page::Clear){
                ui::wrappedText(c,"All progress in this slot will be lost.",{20,64,280,48});
                ui::listRow(c,{16,128,288,32},"Cancel",nullptr,navigation.selected==0);
                ui::listRow(c,{16,168,288,32},"Clear Save",nullptr,navigation.selected==1);
            }
            if(error)ui::wrappedText(c,error,{20,188,280,20});
            const ui::Action actions[]={{"A",page==Page::Slots?"Start":"Select"},{"B","Back"},{"X","Clear"}};
            if(page==Page::Achievements||page==Page::Extras){const ui::Action back[]={ {"B","Back"} };ui::promptBar(c,back,1);}
            else ui::promptBar(c,actions,page==Page::Slots?3:2);
        }
        hk3ds::drawDiagnosticConfirmation(top,double(svcGetSystemTick())/SYSCLOCK_ARM11);
        C3D_FrameEnd(0);
        textcapture::publish();
        if constexpr(diagnosticsEnabled)++hk3ds_test_menu_frame;
    }
    C3D_FrameSync();renderer.release();
    if constexpr(diagnosticsEnabled)if(hk3ds_test_shutdown==0x484b4558u)hk3ds_test_menu_frame|=0x80000000u;
    return result;
}
}
