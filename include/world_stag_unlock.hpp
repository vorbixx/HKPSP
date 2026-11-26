#pragma once
#include <cstdint>
#include <limits>

namespace hk3ds {
// Stag Bell Yes writes these together (including the free Resting Grounds
// lever). The save owner commits the staged ledger and publishes its revision.
template<class Store> bool stageStagStationUnlock(Store& store,const char* flag)
{
    if(!flag||!*flag)return false;
    if(store.get(flag)!=0)return true;
    const auto count=store.get("stationsOpened");
    if(count<0||count==std::numeric_limits<std::int32_t>::max())return false;
    auto next=store;
    if(!next.set(flag,1)||!next.set("stationsOpened",count+1))return false;
    store=next;return true;
}
}
