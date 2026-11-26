#pragma once
#include "physics.hpp"
#include "tutorial_prompts.hpp"
#include <array>

namespace hk3ds {
struct LoreTablet {
    unsigned owner=0;
    Rect range{};
    unsigned pageCount=0;
    // HKL1 carries at most two source pages. Native displays may repaginate
    // their resident copy without shrinking the original font.
    static constexpr unsigned sourcePages=2;
    std::array<std::array<char,768>,8> pages{};
};
struct LoreTabletBank {
    std::array<LoreTablet,3> tablets{};
    bool load(const char* path);
};
enum class InspectionPhase { Idle, Opening, Reading, Down, TurnBack, Cooldown };
struct InspectionInput {
    Rect hero{};
    bool canInspect=false,up=false,down=false,continuePressed=false,interrupted=false;
};
class LoreInspection {
    friend struct SimulationStateHash;
public:
    void bind(const LoreTabletBank& bank) {bank_=&bank;reset();}
    void reset();
    void step(float seconds,const InspectionInput&,TutorialPrompts&);
    bool locked() const {return phase_!=InspectionPhase::Idle&&phase_!=InspectionPhase::Cooldown;}
    bool prompt() const {return nearby_>=0&&phase_==InspectionPhase::Idle;}
    unsigned promptOwner() const {return prompt()?bank_->tablets[nearby_].owner:0;}
    InspectionPhase phase() const {return phase_;}
    unsigned owner() const {return bank_&&active_>=0?bank_->tablets[active_].owner:0;}
    unsigned serial() const {return serial_;}
    unsigned revision() const {return revision_;}
    unsigned page() const {return page_;}
    unsigned visibleCharacters() const {return static_cast<unsigned>(characters_);}
    bool pageComplete() const;
    bool hasNextPage() const {return bank_&&active_>=0&&page_+1<bank_->tablets[active_].pageCount;}
    const char* text() const;
    float alpha() const;
private:
    const LoreTabletBank* bank_=nullptr;
    InspectionPhase phase_=InspectionPhase::Idle;
    int nearby_=-1,active_=-1;
    unsigned page_=0,serial_=0,revision_=0;
    float age_=0,time_=0,characters_=0,speed_=65;
    void down(TutorialPrompts&);
};
}
