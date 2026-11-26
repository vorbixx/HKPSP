#pragma once
#include "animation.hpp"
#include "cartography.hpp"
#include "hero.hpp"
#include "shop_menu.hpp"
#include <array>

namespace hk3ds {
class Cartographer {
    friend struct SimulationStateHash;
public:
    float artScale() const;
    static bool hasPlacement(const char* scene);
    enum class Dialogue { None, Meet, Greet, Prompt, Refuse, NotEnough, Bought, Card, Iselda, Received, Again };
    CartographySave progress{};
    AnimationPlayer animation;
    Vec2 position{};
    Vec2 cardPosition{};
    Dialogue dialogue=Dialogue::None;
    bool present=false,card=false,shop=false,prompt=false,yes=true;
    unsigned voiceSerial=0;
    void bind(const AnimationBank* bank){bank_=bank;}
    void enter(const char* scene,bool mapperShopOpen=false);
    bool update(float seconds,const Input& input,Hero& hero,unsigned& geo);
    bool locked() const {return dialogue!=Dialogue::None;}
    bool humming() const {return present&&!locked();}
    const char* text() const;
    bool hasNextPage() const;
    unsigned price() const;
    // Iselda's purchase step is the shared shop menu: her only stock is the Crossroads map.
    ShopMenuItem shopItem() const;
    ShopMenuView shopMenu(const ShopMenuItem& item,unsigned geo) const;
private:
    const AnimationBank* bank_=nullptr;
    unsigned page_=0;
    unsigned area_=0;
    bool verticalHeld_=false;
    // Conversation Control3889: local to this room instance, not PlayerData.
    bool spoken_=false;
    void talk(Dialogue next);
    void close();
};
}
