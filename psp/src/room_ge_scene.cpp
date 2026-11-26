#include "room_ge.hpp"
#include "perf.hpp"
#include "look_grade.hpp"

namespace hkpsp {
RoomGeScene::~RoomGeScene(){
    if(DrawList::geScene==this){DrawList::geTarget=nullptr;DrawList::geScene=nullptr;DrawList::geQuads=0;}
}
void RoomGeScene::clear(DrawList& list,const hk3ds::RoomVisualBank& bank){
    if(DrawList::geScene&&DrawList::geScene!=this)throw std::logic_error("Another GE scene is bound");
    room=&bank;spans.clear();commands.clear();scratch.commands.clear();scratch.cpu.quads.clear();
    DrawList::geTarget=&list;DrawList::geScene=this;DrawList::geQuads=0;
}
void RoomGeScene::append(DrawList& list,const RoomGePlan& plan){
    if(list.roomGe()!=this||!room)throw std::logic_error("Unbound GE scene");
    RoomGeSpan span{&plan,static_cast<unsigned>(list.quads.size()),0,
                    static_cast<unsigned>(commands.size()),0};
    list.quads.insert(list.quads.end(),scratch.cpu.quads.begin(),scratch.cpu.quads.end());
    for(auto command:scratch.commands){
        if(command.source&roomGeCpuBit){
            const auto id=command.source&~roomGeCpuBit;
            if(id>=scratch.cpu.quads.size())throw std::out_of_range("Invalid GE scene CPU slot");
            command.source=roomGeCpuBit|(span.firstCpu+id);
        }else ++DrawList::geQuads;
        commands.push_back(command);
    }
    span.lastCpu=static_cast<unsigned>(list.quads.size());
#ifdef HKPSP_PERF
    // Mixed-mode CPU fallbacks have the same room counters as appendRoom.
    if(span.firstCpu<span.lastCpu)list.roomRanges.push_back({span.firstCpu,span.lastCpu});
#endif
    span.lastCommand=static_cast<unsigned>(commands.size());spans.push_back(span);
}
std::size_t RoomGeScene::bytes()const{
    return spans.capacity()*sizeof(RoomGeSpan)+commands.capacity()*sizeof(RoomGeCommand)+
        scratch.cpu.quads.capacity()*sizeof(Quad)+scratch.commands.capacity()*sizeof(RoomGeCommand)+
        scratch.entries.capacity()*sizeof(unsigned)+scratch.exactKeys.capacity()*sizeof(std::array<float,2>);
}
RoomGeSubmission::RoomGeSubmission(const DrawList& source):list(source){
    unsigned cpu=0,command=0;count=static_cast<unsigned>(list.quads.size());
    if(const auto* scene=list.roomGe()){
        if(!scene->room)throw std::logic_error("GE submission has no room");
        for(const auto& span:scene->spans){
            if(!span.plan||span.firstCpu<cpu||span.lastCpu<span.firstCpu||span.lastCpu>list.quads.size()||
               span.firstCommand!=command||span.lastCommand<span.firstCommand||span.lastCommand>scene->commands.size()||
               span.lastCommand-span.firstCommand<span.lastCpu-span.firstCpu)
                throw std::logic_error("Invalid GE submission span");
            count+=span.lastCommand-span.firstCommand-(span.lastCpu-span.firstCpu);
            cpu=span.lastCpu;command=span.lastCommand;
        }
        if(command!=scene->commands.size()||count!=list.nextOrder())
            throw std::logic_error("GE submission lost commands");
    }
}
RoomGeItem RoomGeSubmission::at(unsigned index)const{
    if(index>=count)throw std::out_of_range("Invalid GE submission index");
    unsigned cpu=0;
    if(const auto* scene=list.roomGe())for(const auto& span:scene->spans){
        HKPSP_PROFILE(++perf::current[perf::RoomGeSpanVisits];)
        const unsigned gap=span.firstCpu-cpu;
        if(index<gap)return {cpu+index,span.firstCpu};
        index-=gap;
        const unsigned commands=span.lastCommand-span.firstCommand;
        if(index<commands){
            const auto& c=scene->commands[span.firstCommand+index];
            if(c.source&roomGeCpuBit){
                const unsigned id=c.source&~roomGeCpuBit;
                if(id<span.firstCpu||id>=span.lastCpu)throw std::logic_error("Invalid GE submission CPU slot");
                return {id,id+1,c.order};
            }
            const auto& entry=span.plan->buffers.ordered.at(c.source);
            if(entry.geometry&roomGeCpuBit)throw std::logic_error("CPU cache submitted as world geometry");
            const auto& run=span.plan->buffers.runs.at(entry.geometry);
            if(run.count!=1)throw std::logic_error("Unproved GE submission batch ordering");
            return {~0u,0,c.order,span.plan,&run,&scene->room->draws.at(entry.draw)};
        }
        index-=commands;cpu=span.lastCpu;
    }
    return {cpu+index,static_cast<unsigned>(list.quads.size())};
}
unsigned RoomGeSubmission::page(unsigned index)const{
    const auto item=at(index);
    if(item.world())return list.roomGe()->room->atlas.sprites.at(item.draw->sprite).page;
    const auto& q=list.quads.at(item.cpu);
    if(!samplesTexture(q.blend))return ~0u;
    if(q.page==~0u)throw std::out_of_range("Invalid texture page");
    return q.page;
}
Quad RoomGeSubmission::state(const RoomGeItem& item)const{
    if(!item.world())throw std::invalid_argument("World state requested for a CPU draw");
    const auto& sprite=list.roomGe()->room->atlas.sprites.at(item.draw->sprite);
    const auto& v=item.draw->values;Quad q;
    q.page=sprite.page;q.clut=sprite.clut;q.material=hk3ds::roomMaterial(*item.draw);
    q.rgba={v[7],v[8],v[9],v[10]};q.order=item.order;q.blend=Blend::Room;
    if(q.material==hk3ds::RoomMaterial::LinearDodge)
        throw std::logic_error("Screen-bounds material submitted as world geometry");
    return q;
}
#ifndef __PSP__
void expandRoomGe(const DrawList& source,DrawList& destination){
    if(&source==&destination)throw std::invalid_argument("GE expansion aliases its source");
    if(destination.roomGe())throw std::invalid_argument("GE expansion overwrites a bound list");
    destination=source;
    if(!source.roomGe())return;
    const auto& scene=*source.roomGe();
    if(!scene.room)throw std::logic_error("GE scene has no room");
    destination.quads.clear();destination.quads.reserve(source.nextOrder());
    unsigned cpu=0,command=0,ge=0;
    const hk3ds::Vec2 camera{source.info.cameraX,source.info.cameraY};
    for(const auto& span:scene.spans){
        if(!span.plan||span.firstCpu<cpu||span.lastCpu<span.firstCpu||span.lastCpu>source.quads.size()||
           span.firstCommand!=command||span.lastCommand<span.firstCommand||span.lastCommand>scene.commands.size())
            throw std::logic_error("Invalid ordered GE span");
        destination.quads.insert(destination.quads.end(),source.quads.begin()+cpu,source.quads.begin()+span.firstCpu);
        unsigned nextCpu=span.firstCpu;
        for(;command<span.lastCommand;++command){
            const auto& c=scene.commands[command];
            if(c.source&roomGeCpuBit){
                const auto id=c.source&~roomGeCpuBit;
                if(id!=nextCpu++||id>=span.lastCpu)throw std::logic_error("Nonsequential GE CPU slot");
                destination.quads.push_back(source.quads[id]);
            }else{
                const auto& plan=*span.plan;const auto& e=plan.buffers.ordered.at(c.source);
                if(e.geometry&roomGeCpuBit)throw std::logic_error("CPU cache submitted as GE geometry");
                const auto& run=plan.buffers.runs.at(e.geometry);
                if(run.count!=1)throw std::logic_error("Unproved GE scene batch ordering");
                const auto& draw=scene.room->draws.at(e.draw);RoomQuadCache cache;Quad q;
                if(!cacheRoomQuad(scene.room->atlas.sprites.at(draw.sprite),draw,plan.background,scene.room->blurSplitDepth,cache)||
                   !projectRoomQuad(cache,camera,plan.focal,q))throw std::logic_error("GE scene admission changed");
                q.order=c.order;destination.quads.push_back(q);++ge;
            }
        }
        if(nextCpu!=span.lastCpu)throw std::logic_error("GE span lost CPU payloads");
        cpu=span.lastCpu;
    }
    if(command!=scene.commands.size()||ge!=DrawList::geQuads)throw std::logic_error("GE scene lost commands");
    destination.quads.insert(destination.quads.end(),source.quads.begin()+cpu,source.quads.end());
}
#endif
}
