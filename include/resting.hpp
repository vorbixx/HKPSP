#pragma once
#include "hero.hpp"
#include "animation.hpp"
#include "presentation.hpp"
#include "saved_player_data.hpp"
#include <cstdint>
#include <vector>
namespace hk3ds {
// Station IDs are stable save bits, independent of the resident room order.
struct StagStation {const char* scene;const char* label;const char* flag;int originalId;};
inline constexpr StagStation stagStations[]={{"Room_Town_Stag_Station","Dirtmouth","openedTown",0},{"Crossroads_47","Forgotten Crossroads","openedCrossroads",1},{"Fungus1_16_alt","Greenpath","openedGreenpath",2},{"RestingGrounds_09","Resting Grounds","openedRestingGrounds",7},{"Fungus2_02","Queen's Station","openedFungalWastes",3},{"Fungus3_40","Queen's Gardens","openedRoyalGardens",4},{"Ruins1_29","City Storerooms","openedRuins1",5},{"Ruins2_08","King's Station","openedRuins2",6},{"Deepnest_09","Deepnest","openedDeepnest",8},{"Cliffs_03","Stag Nest","openedTown",9},{"Abyss_22","Hidden Station","openedHiddenStation",10}};
inline constexpr unsigned stagStationCount=sizeof(stagStations)/sizeof(stagStations[0]);
static_assert(stagStationCount>0 && stagStationCount<32, "Station mask must fit in 32 bits");
// Optional HKS sidecar: original station placement and purchase price.
struct StagConfig {
    bool present=false;
    unsigned station=0,price=0;
    Vec2 position{},scale{1,1},bell{},toll{},bellScale{1,1},standScale{1,1},arrival{},dust{};
    float startX=0;
};
enum class StagPhase { Hidden, Opening, Ringing, Arriving, Idle, Toll, Poor, Dialogue, Menu, Departing, Riding, StepOff };
struct RestPoint {Vec2 position{};unsigned kind=0,station=0;}; // 0 bench, 1 bell
inline constexpr unsigned legacyCharmIds[]={0,0,0,0,14,4,7,0,20,0,18,0,6};
struct RestSave {
    char benchScene[32]{},shadeScene[32]="Tutorial_01";
    float benchX=0,benchY=0;
    std::uint32_t stations=0,ownedCharms=0,equippedCharms=0;
    bool valid() const;
};
class Resting {
    friend struct SimulationStateHash;
public:
    float stagArtScale() const {return animationFacingScale("stag/stag",-station.scale.x,std::abs(station.scale.x));}
    RestSave saved{};
    unsigned availableStations=15;
    bool sitting=false,travelMenu=false,charmMenu=false,prompt=false;
    unsigned selection=0;
    int nearby=-1;
    StagConfig station{};
    StagPhase stagPhase=StagPhase::Hidden;
    AnimationPlayer stagAnimation,bellAnimation,standAnimation,smokeAnimation;
    Vec2 stagPosition{};
    PresentationPosition stagHistory;
    unsigned bellSerial=0,arrivalSerial=0,departSerial=0,voiceSerial=0;
    bool tollPrompt=false,bellPrompt=false,talkPrompt=false,yes=true;
    void bindStag(const AnimationBank* bank){stagBank_=bank;}
    void enterStation(const StagConfig&,bool arrival,Hero&,SavedPlayerData&);
    bool stagVisible() const;
    bool smokeVisible() const {return stagPhase==StagPhase::Arriving||stagPhase==StagPhase::Riding;}
    bool stationOpen() const {return station.present&&(saved.stations&(1u<<station.station));}
    unsigned destinations() const {return saved.stations&availableStations&~(station.present?1u<<station.station:0u);}
    const char* stationText() const;
    bool stationHasNextPage() const {return dialoguePage_+1<dialogueCount();}
    Vec2 presentedStag(PresentationTime time) const {return stagHistory.sample(stagPosition,time);}
    // Returns true when a persistent event changed. Travel is consumed by the loader.
    bool update(const Input&,Hero&,const std::vector<RestPoint>&,const char* scene,float seconds=1.f/60,unsigned* geo=nullptr,SavedPlayerData* playerData=nullptr,unsigned blockedStations=0);
    int takeTravel(){const int result=travel_;travel_=-1;return result;}
    void leave(){sitting=travelMenu=charmMenu=prompt=false;nearby=-1;travel_=-1;vertical_=0;standPosition_={};station={};stagPhase=StagPhase::Hidden;stagAnimation={};tollPrompt=bellPrompt=talkPrompt=false;}
    void applyCharms(Hero&) const;
    const char* charmLabel() const;
private:
    const AnimationBank* stagBank_=nullptr;
    float stagClock_=0;
    unsigned dialoguePage_=0;
    bool firstMeeting_=true;
    unsigned dialogueCount() const;
    int destination_=-1,stationVertical_=0;
    void stagState(StagPhase);
    bool updateStation(const Input&,Hero&,float,unsigned*,SavedPlayerData*);
    Vec2 standPosition_{};
    int vertical_=0,travel_=-1;
};
}
