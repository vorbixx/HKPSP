#pragma once
#include "area_title_data.hpp"
#include "physics.hpp"
#include "saved_player_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hk3ds {
// Tutorial_01's AreaTitleController always uses VisitPause for this sub-area.
// Its KINGSPASS delegate suppresses the title until visitedCrossroads is true.
class AreaTitle {
    friend struct SimulationStateHash;
    enum class Entry { Legacy, DirtmouthTrigger, CrossroadsTrigger, GreenpathPlaced };
public:
    static constexpr unsigned visitedCrossroadsFlag=1024;
    void reset(bool visitedCrossroads,bool heroInPosition=true)
    {enabled_=visitedCrossroads;positioned_=heroInPosition;age_=0;region_=0;display_=true;large_=false;visitPending_=false;pendingArea_=-1;entry_=Entry::Legacy;}
    void heroInPosition() {if(entry_==Entry::Legacy||entry_==Entry::GreenpathPlaced)positioned_=true;}
    void observeEntry(Rect body)
    {
        if(positioned_||!display_||(entry_!=Entry::DirtmouthTrigger&&entry_!=Entry::CrossroadsTrigger))return;
        const Rect trigger=entry_==Entry::DirtmouthTrigger?Rect{97.62f,-2.45f,170.22f,39.53f}:
            Rect{48.48f,9.62f,55.92f,15.08f};
        positioned_=body.left<trigger.right&&body.right>trigger.left&&body.bottom<trigger.top&&body.top>trigger.bottom;
    }
    bool waitsForContact() const
    {return !positioned_&&display_&&(entry_==Entry::DirtmouthTrigger||entry_==Entry::CrossroadsTrigger);}
    // Mega Jellyfish/Control Start activates the shared title with Visited=true
    // and Area Event=MEGA_JELLY (EN_Titles: MEGA_JELLY_MAIN).
    void resetUumuu(){reset(true);region_=8;}
    void resetScene(bool crossroads,bool greenpath=false){reset(true);region_=greenpath?3:crossroads?2:1;}
    void resetScene(const char* scene,const SavedPlayerData* progress=nullptr){
        // The guardian and mask scenes must not inherit the default town title.
        if(!std::strcmp(scene,"Dream_Guardian_Monomon")||!std::strcmp(scene,"Cutscene_Boss_Door")){
            reset(false);return;
        }
        reset(true);
        // This room emits SLY without a Dirtmouth entry title, retaining visits.
        // Mines_33 also has no AreaTitleController; retain visit tracking.
        display_=std::strcmp(scene,"Fungus2_15")!=0&&std::strcmp(scene,"Room_ruinhouse")!=0&&std::strcmp(scene,"Mines_33")!=0&&std::strncmp(scene,"Deepnest_",9)!=0&&
            std::strcmp(scene,"Dream_Nailcollection")!=0&&std::strcmp(scene,"RestingGrounds_07")!=0;
        region_=!std::strncmp(scene,"Waterways_",10)?6:
            !std::strncmp(scene,"Abyss_",6)?7:
            !std::strcmp(scene,"Fungus3_archive")||!std::strcmp(scene,"Fungus3_archive_02")?10:
            !std::strncmp(scene,"Fungus3_",8)?9:
            !std::strncmp(scene,"Fungus1_",8)?3:
            !std::strcmp(scene,"Crossroads_50")?4:
            !std::strncmp(scene,"RestingGrounds_",15)?5:
            !std::strncmp(scene,"Crossroads_",10)?2:
            // 2026-11-21 (user report: "DIRTMOUTH" in Ruins2_03): rooms without a mapped area used to fall back to
            // Dirtmouth (1). Only Town is Dirtmouth; other unmapped rooms show no region title until their area
            // is mapped, and never write currentArea.
            !std::strcmp(scene,"Town")?1:0;
        if(!region_)display_=false;  // area 0 is King's Pass in the PSP title art; show nothing instead
        if(progress){
            const bool basinRoute=region_==6||region_==7;
            const bool entryRegion=region_==2||region_==3||!std::strcmp(scene,"Town");
            if(basinRoute||entryRegion)display_=display_&&progress->get("currentArea")!=int(areaId());
            large_=display_&&(basinRoute||entryRegion)&&!progress->get(visitedField());
            pendingArea_=!std::strncmp(scene,"Ruins",5)&&progress->get("currentArea")==7?6:-1;
            // Blue Lake is a subarea; unrelated room names must not overwrite currentArea.
            visitPending_=pendingArea_>=0||(display_&&region_!=0&&region_<=7&&region_!=4&&(region_!=1||!std::strcmp(scene,"Town")));
            entry_=!std::strcmp(scene,"Town")?Entry::DirtmouthTrigger:
                !std::strcmp(scene,"Crossroads_01")?Entry::CrossroadsTrigger:
                !std::strcmp(scene,"Fungus1_01")?Entry::GreenpathPlaced:Entry::Legacy;
            positioned_=entry_==Entry::Legacy;
        }
    }
    bool large() const {return large_;}
    bool originalRegionLayout() const {return entry_!=Entry::Legacy;}
    static constexpr float fleursUp=.6f, titlesDown=5.35f, largeFinish=titlesDown+2.f;
    float fleurTime() const
    {return enabled_&&display_&&positioned_&&originalRegionLayout()&&large_&&age_<delay()+largeFinish?age_-delay():-1.f;}
    bool firstWaterwaysVisit() const {return region_==6&&large_;}
    bool commitVisit(SavedPlayerData& progress) {return step(0,&progress);}
    unsigned artPart() const {return large_?(region_==6?1:region_==7?2:0):0;}
    const char* visitedField() const {return region_==1?"visitedDirtmouth":region_==2?"visitedCrossroads":
        region_==3?"visitedGreenpath":region_==6?"visitedWaterways":"visitedAbyss";}
    unsigned areaId() const {return region_==6?7:region_==7?13:region_==5?12:region_==4?0:region_;}
    const char* regionLabel() const {return region_==1?"DIRTMOUTH":region_==2?"FORGOTTEN CROSSROADS":
        region_==3?"GREENPATH":region_==4?"BLUE LAKE":region_==5?"RESTING GROUNDS":
        region_==6?"ROYAL WATERWAYS":region_==7?"ANCIENT BASIN":region_==8?"UUMUU":
        region_==9?"FOG CANYON":region_==10?"TEACHER'S ARCHIVES":nullptr;}
    bool step(float seconds,SavedPlayerData* progress=nullptr)
    {
        if(!enabled_||!positioned_||!std::isfinite(seconds)||seconds<0)return false;
        age_=std::min(age_+seconds,originalRegionLayout()&&large_?delay()+largeFinish:endTime());
        if(!visitPending_||!progress||age_<delay())return false;
        auto next=*progress;
        if(!next.set("currentArea",pendingArea_>=0?unsigned(pendingArea_):areaId())||(large_&&!next.set(visitedField(),1)))return false;
        *progress=next;visitPending_=false;return true;
    }
    float alpha() const
    {
        if(!enabled_||!display_||!positioned_||age_<delay())return 0;
        const float t=age_-delay();
        // Original large branch: Fleurs Up waits 4.75s, then Titles Up .6s.
        const float appear=(artPart()||(originalRegionLayout()&&large_))?titlesDown:areatitledata::appear;
        return t<appear?std::min(1.f,t/areatitledata::fadeIn):
            std::max(0.f,1.f-(t-appear)/areatitledata::fadeOut);
    }
    bool visitedCrossroads() const {return enabled_;}
    float seconds() const {return age_;}
    static constexpr float endTime()
    {return areatitledata::entryDelay+areatitledata::appear+areatitledata::disappear;}
private:
    float delay() const {return entry_==Entry::DirtmouthTrigger&&large_?0.f:areatitledata::entryDelay;}
    float age_=0;
    bool enabled_=false,positioned_=false,display_=true,large_=false,visitPending_=false;
    int pendingArea_=-1;
    unsigned region_=0;
    Entry entry_=Entry::Legacy;
};
}
