#include "room_ge_trace.hpp"
#include "room_ge.hpp"
#include <stdexcept>

namespace hkpsp {
void RoomGeTrace::append(const DrawList& list,unsigned first,unsigned last){
    const RoomGeSubmission submission(list);
    if(first!=draws||last<first||last>submission.size())
        throw std::logic_error("Invalid room trace submission range");
    const auto add=[&](const auto& value){
        const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(unsigned i=0;i<sizeof(value);++i){hash^=bytes[i];hash*=1099511628211ull;}
    };
    for(unsigned i=first;i<last;++i){
        const auto item=submission.at(i);
        Quad state;
        if(item.world()){state=submission.state(item);++world;}
        const auto& q=item.world()?state:list.quads.at(item.cpu);
        add(q.page);add(q.clut);add(q.material);add(q.blend);
        for(const auto value:q.rgba)add(value);
        add(q.whiteFlash);add(q.order);
    }
    draws=last;
}
}
