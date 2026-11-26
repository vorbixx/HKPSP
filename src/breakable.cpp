#include "breakable.hpp"
#include "performance_sections.hpp"
#include "render_memory.hpp"
#include "room_entities.hpp"
#include "grass.hpp"
#include "performance_sections.hpp"
#include "decor_terrain.hpp"
#include "decor_body.hpp"
#include "decor_motion.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <optional>
namespace hk3ds
{
void BreakableState::emit(BreakableAction action)
{ if(notify_)notify_(context_,action,state_); }
void BreakableState::staticParts()
{
    state_.whole=false;state_.remnants=true;
    emit(BreakableAction::StaticParts);
    if(hitReceiver_)emit(BreakableAction::Hit);
    state_.collider=false;
}
void BreakableState::awake(BreakableActivation initial,bool savedBroken,bool hasHitReceiver,
                          bool forwardBreak,void* context,Notify notify)
{
    state_=initial;state_.broken=savedBroken;state_.interactive=true;
    hitReceiver_=hasHitReceiver;forwardBreak_=forwardBreak;context_=context;notify_=notify;started_=false;
    if(savedBroken)staticParts();
}
void BreakableState::start(float z,float foreground,float background,bool hasBoxCollider)
{
    if(started_)return;
    started_=true;
    if(z<foreground || z>background)
    {
        state_.interactive=false;
        if(hasBoxCollider)state_.collider=false;
        return;
    }
    state_.interactive=true;state_.remnants=false;
}
bool BreakableState::breakNow()
{
    if(!state_.interactive || state_.broken)return false;
    staticParts();
    state_.debris=true;emit(BreakableAction::Debris);
    if(hitReceiver_)emit(BreakableAction::Hit);
    if(forwardBreak_)emit(BreakableAction::ForwardBreak);
    emit(BreakableAction::CameraShake);
    state_.collider=false;state_.broken=true;
    return true;
}
bool DoorConfig::load(const char* path)
{
    auto* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4];unsigned header[4];DoorConfig next;
    bool ok=std::fread(magic,1,4,file)==4 && std::memcmp(magic,"HKD1",4)==0 &&
        std::fread(header,4,4,file)==4;
    if(ok)
    {
        next.trigger=header[0];next.count=header[1];next.layer=header[2];
        ok=next.trigger && next.count && next.count<=capacity && next.layer<32 && header[3]==0;
    }
    if(ok)ok=std::fread(next.disabledShapes.data(),4,next.count,file)==next.count;
    if(ok)ok=std::fgetc(file)==EOF;
    std::fclose(file);
    bool containsTrigger=false;
    for(unsigned i=0;ok && i<next.count;++i)
    {
        ok=next.disabledShapes[i]!=0;containsTrigger |= next.disabledShapes[i]==next.trigger;
        for(unsigned j=0;j<i;++j)ok &= next.disabledShapes[i]!=next.disabledShapes[j];
    }
    if(!ok || !containsTrigger)return false;
    *this=next;return true;
}
bool BreakableDoor::bind(const DoorConfig& config,TerrainBank& terrain,CombatObject& object,bool savedBroken)
{
    const TerrainShape* trigger=nullptr;
    if(!config.count || config.count>DoorConfig::capacity || config.layer>=32)return false;
    for(unsigned i=0;i<config.count;++i)
    {
        const TerrainShape* shape=nullptr;
        for(const auto& candidate:terrain.shapes)if(candidate.id==config.disabledShapes[i])shape=&candidate;
        if(!shape)return false;
        if(shape->id==config.trigger)trigger=shape;
    }
    if(!trigger || !(trigger->flags&1))return false;
    *this={};config_=config;terrain_=&terrain;object_=&object;
    object={};object.name="Tute Door 5";object.layer=config.layer;object.bounds=trigger->bounds;
    object.nonBouncer=true;object.receiver=this;object.onHit=receive;
    for(unsigned i=0;i<config.count;++i)terrain.setEnabled(config.disabledShapes[i],true);
    state_.awake({},false,false,false);
    // Door eligibility was resolved by the room converter; no scene depth is
    // inferred here. The neutral interval selects the existing active branch.
    state_.start(0,0,0);
    if(savedBroken)breakDoor();
    return true;
}
void BreakableDoor::receive(void* self,const NailHit&,HitOutcome)
{static_cast<BreakableDoor*>(self)->breakDoor();}
void BreakableDoor::breakDoor()
{
    if(!terrain_ || !state_.breakNow())return;
    // Both the root trigger and colliders in wholeParts stop participating now.
    for(unsigned i=0;i<config_.count;++i)terrain_->setEnabled(config_.disabledShapes[i],false);
    object_->colliderEnabled=false;
}
bool validDecor(const DecorConfig& c)
{
    float previous=0;
    for(unsigned i=0;i<c.vertices.size();++i)
    {
        const auto a=c.vertices[i],b=c.vertices[(i+1)%4],d=c.vertices[(i+2)%4];
        if(!std::isfinite(a.x)||!std::isfinite(a.y)||a.x<c.bounds.left||a.x>c.bounds.right||
           a.y<c.bounds.bottom||a.y>c.bounds.top)return false;
        const float cross=(b.x-a.x)*(d.y-b.y)-(b.y-a.y)*(d.x-b.x);
        if(!std::isfinite(cross)||cross==0||(i && (cross>0)!=(previous>0)))return false;
        previous=cross;
    }
    return c.whole && c.remnant && c.whole!=c.remnant && c.layer<32 &&
        std::isfinite(c.bounds.left) && std::isfinite(c.bounds.right) &&
        std::isfinite(c.bounds.bottom) && std::isfinite(c.bounds.top) &&
        c.bounds.left<c.bounds.right && c.bounds.bottom<c.bounds.top;
}
bool DecorBank::load(const char* path)
{
    auto* file=std::fopen(path,"rb");if(!file)return false;
    DecorBank next;char magic[4];
    bool ok=std::fread(magic,1,4,file)==4 && !std::memcmp(magic,"HKB1",4) &&
        std::fread(&next.count,4,1,file)==1 && next.count && next.count<=capacity;
    for(unsigned i=0;ok && i<next.count;++i)
    {
        auto& c=next.items[i];unsigned ids[3];float points[8];
        ok=std::fread(ids,4,3,file)==3 && std::fread(points,4,8,file)==8;
        if(!ok)break;
        c.whole=ids[0];c.remnant=ids[1];c.layer=ids[2];
        c.bounds={points[0],points[1],points[0],points[1]};
        for(unsigned j=0;j<4;++j)
        {
            c.vertices[j]={points[j*2],points[j*2+1]};
            c.bounds.left=std::min(c.bounds.left,points[j*2]);c.bounds.right=std::max(c.bounds.right,points[j*2]);
            c.bounds.bottom=std::min(c.bounds.bottom,points[j*2+1]);c.bounds.top=std::max(c.bounds.top,points[j*2+1]);
        }
        ok=validDecor(c);
        for(unsigned j=0;ok && j<i;++j)
        {
            const auto& p=next.items[j];
            ok=c.whole!=p.whole && c.whole!=p.remnant && c.remnant!=p.whole && c.remnant!=p.remnant;
        }
    }
    if(ok)ok=std::fgetc(file)==EOF;
    std::fclose(file);if(!ok)return false;
    *this=next;return true;
}
bool BreakableDecor::bind(const DecorConfig& config,CombatObject& object,void* context,HitEffect effect)
{
    if(!validDecor(config))return false;
    config_=config;object_=&object;object={};context_=context;effect_=effect;
    object.name="Breakable decor";object.layer=config.layer;object.bounds=config.bounds;
    object.hitPolygon=&config_.vertices;
    object.receiver=this;object.onHit=receive;
    state_.awake({},false,false,false);state_.start(0,0,0);
    return true;
}
float BreakableDecor::alpha(unsigned owner) const
{
    if(owner==config_.whole)return state_.activation().whole?1.0f:0.0f;
    if(owner==config_.remnant)return state_.activation().remnants?1.0f:0.0f;
    return 1;
}
void BreakableDecor::receive(void* context,const NailHit& hit,HitOutcome)
{
    auto& self=*static_cast<BreakableDecor*>(context);
    if(self.state_.breakNow()){
        self.object_->colliderEnabled=false;
        if(self.effect_)self.effect_(self.context_,self.config_.whole,hit);
    }
}

namespace {
bool validRoomDebris(const RoomDebrisConfig& config)
{
    if(config.flags>7||config.owners.empty()||config.owners.size()>512)return false;
    for(float v:config.physics)if(!std::isfinite(v)||std::abs(v)>1e6f)return false;
    for(float v:config.hull)if(!std::isfinite(v)||std::abs(v)>1e6f)return false;
    for(float v:config.finish)if(!std::isfinite(v)||v<0||v>1e6f)return false;
    const auto& b=config.physics;const auto& h=config.hull;
    if(b[3]>b[4]||b[6]<0||b[7]<0||b[8]<=0||b[9]<=0||b[10]<0||b[11]<0||b[12]<0||b[12]>1||b[13]<0||b[14]<=0||b[15]<=0||b[15]>.1f)return false;
    if(h[0]!=0&&(h[0]<3||h[0]>7||h[0]!=std::floor(h[0])))return false;
    if(h[0]){
        float area=0;
        for(unsigned i=0;i<unsigned(h[0]);++i){const auto j=(i+1)%unsigned(h[0]);area+=h[3+2*i]*h[4+2*j]-h[4+2*i]*h[3+2*j];}
        if(std::abs(area)<1e-6f)return false;
    }
    return std::all_of(config.owners.begin(),config.owners.end(),[](unsigned id){return id>0;});
}
bool validRoomLaunch(const std::array<float,16>& launch)
{
    for(float value:launch)if(!std::isfinite(value)||std::abs(value)>100000)return false;
    for(unsigned i=0;i<16;i+=4)if(launch[i]>launch[i+1]||launch[i+2]<0||launch[i+2]>launch[i+3])return false;
    return true;
}
}
static bool validRoomAudio(const RoomBreakableConfig& item)
{
    if(item.audio.size()>2||(!item.audioAuthored&&!item.audio.empty()))return false;
    for(const auto& cue:item.audio){
        if(cue.choices.empty()||cue.choices.size()>32||!std::isfinite(cue.gain)||cue.gain<0||cue.gain>4||
           !std::isfinite(cue.low)||!std::isfinite(cue.high)||cue.low<=0||cue.high<cue.low||cue.high>4)return false;
        double total=0;
        for(const auto& choice:cue.choices){
            if(!std::isfinite(choice.weight)||choice.weight<=0)return false;
            total+=choice.weight;
            const auto end=std::find(choice.file.begin(),choice.file.end(),'\0');
            if(end==choice.file.end())return false;
            if(choice.file[0]){
                const auto length=end-choice.file.begin();
                if(length<13||std::strncmp(choice.file.data(),"room-sfx-",9)||std::strcmp(choice.file.data()+length-4,".wav"))return false;
                for(auto i=choice.file.begin();i!=end;++i)
                    if(!((*i>='a'&&*i<='z')||(*i>='A'&&*i<='Z')||(*i>='0'&&*i<='9')||*i=='-'||*i=='_'||*i=='.'))return false;
            }
        }
        if(!std::isfinite(total)||total>1e9)return false;
    }
    return true;
}
bool RoomBreakableBank::load(const char* path)
{
    auto* file=std::fopen(path,"rb");if(!file)return false;
    RoomBreakableBank next;char magic[4]{};unsigned count=0,totalTriangles=0,totalBodies=0,totalOwners=0,totalChoices=0;
    const bool headerRead=std::fread(magic,1,4,file)==4;
    const bool audio=headerRead&&std::memcmp(magic,"HKB8",4)==0;
    const bool effectDepth=audio||(headerRead&&std::memcmp(magic,"HKB7",4)==0);
    const bool edges=effectDepth||(headerRead&&std::memcmp(magic,"HKB6",4)==0);
    const bool bodies=edges||(headerRead&&std::memcmp(magic,"HKB5",4)==0);
    const bool circles=bodies||(headerRead&&std::memcmp(magic,"HKB4",4)==0);
    const bool pieces=circles||(headerRead&&std::memcmp(magic,"HKB3",4)==0);
    bool ok=headerRead&&(pieces||std::memcmp(magic,"HKB2",4)==0)&&
        std::fread(&count,4,1,file)==1&&count<CombatScene::capacity;
    if(ok)next.items.resize(count);
    for(auto& item:next.items){
        unsigned header[12]{};float values[23]{};
        const unsigned headers=edges?12:bodies?11:circles?9:pieces?8:7, floats=effectDepth?23:bodies?22:pieces?6:14;
        ok=ok&&std::fread(header,4,headers,file)==headers&&std::fread(values,4,floats,file)==floats;
        if(!ok||!header[0]||header[1]>=32||header[2]>1||header[3]>512||header[4]>512||header[5]>512||header[6]>31){ok=false;break;}
        for(float value:values)if(!std::isfinite(value))ok=false;
        if(!ok)break;
        item.owner=header[0];item.layer=header[1];item.debris=header[2];item.effects=header[6];item.effectZ=values[22];
        const unsigned pose=pieces?0:8;
        if(!pieces)for(unsigned i=0;i<4;++i)item.vertices[i]={values[i*2],values[i*2+1]};
        item.origin={values[pose],values[pose+1]};item.depth=values[pose+2];item.effect={values[pose+3],values[pose+4]};item.angle=values[pose+5];
        if(bodies){
            if(header[9]>512||(totalBodies+=header[9])>512||header[10]>1){ok=false;break;}
            item.impulse=header[10];std::copy(values+6,values+22,item.launch.begin());
            if(!validRoomLaunch(item.launch)){ok=false;break;}
        }
        if(pieces){
            if(header[7]>256||header[8]>256||header[11]>256||header[7]+header[8]+header[11]==0||
               header[7]+header[8]+header[11]>256||(totalTriangles+=header[7]+header[8]+header[11])>2048){ok=false;break;}
            item.triangles.resize(header[7]);
            for(auto& triangle:item.triangles){
                float points[6];if(std::fread(points,4,6,file)!=6){ok=false;break;}
                for(unsigned i=0;i<3;++i)triangle[i]={points[2*i],points[2*i+1]};
                if(!validHitTriangle(triangle))ok=false;
            }
            if(!ok)break;
        }
        if(circles){
            item.circles.resize(header[8]);
            for(auto& circle:item.circles){
                float values[3];if(std::fread(values,4,3,file)!=3){ok=false;break;}
                circle={{values[0],values[1]},values[2]};
                if(!validHitCircle(circle))ok=false;
            }
            if(!ok)break;
        }
        item.segments.resize(header[11]);
        for(auto& segment:item.segments){
            float points[4];if(std::fread(points,4,4,file)!=4){ok=false;break;}
            segment={Vec2{points[0],points[1]},Vec2{points[2],points[3]}};
            if(!validHitSegment(segment))ok=false;
        }
        if(!ok)break;
        auto read=[&](std::vector<unsigned>& ids,unsigned n){ids.resize(n);return std::fread(ids.data(),4,n,file)==n;};
        ok=read(item.whole,header[3])&&read(item.remnants,header[4])&&read(item.colliders,header[5]);
        if(!ok||item.whole.empty()){ok=false;break;}
        item.rigidDebris.resize(header[9]);
        for(auto& body:item.rigidDebris){
            unsigned row[2]{};
            ok=std::fread(row,4,2,file)==2&&row[1]>0&&row[1]<=512&&(totalOwners+=row[1])<=2048;
            if(!ok)break;
            body.flags=row[0];
            ok=std::fread(body.physics.data(),4,16,file)==16&&std::fread(body.hull.data(),4,17,file)==17&&
                std::fread(body.finish.data(),4,2,file)==2&&read(body.owners,row[1]);
            if(!ok||!validRoomDebris(body)){ok=false;break;}
        }
        if(audio){
            unsigned authored=0,cues=0;
            ok=ok&&std::fread(&authored,4,1,file)==1&&authored<=1&&std::fread(&cues,4,1,file)==1&&cues<=2;
            if(!ok)break;
            item.audioAuthored=authored;item.audio.resize(cues);
            for(auto& cue:item.audio){
                unsigned choices=0;float values[3]{};
                ok=ok&&std::fread(&choices,4,1,file)==1&&choices&&choices<=32&&
                    (totalChoices+=choices)<=512&&std::fread(values,4,3,file)==3;
                if(!ok)break;
                cue.gain=values[0];cue.low=values[1];cue.high=values[2];cue.choices.resize(choices);
                for(auto& choice:cue.choices)
                    ok=ok&&std::fread(choice.file.data(),1,80,file)==80&&std::fread(&choice.weight,4,1,file)==1;
            }
            if(!ok||!validRoomAudio(item)){ok=false;break;}
        }
        for(const auto& previous:next.items)if(&previous==&item)break;else if(previous.owner==item.owner)ok=false;
        if(!ok)break;
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(ok)*this=std::move(next);
    return ok;
}
bool RoomBreakables::bind(const RoomBreakableBank& bank,TerrainBank& terrain,CombatObject* objects,
                         unsigned capacity,void* context,Effect effect)
{
    if(bank.items.size()>capacity||(!objects&&!bank.items.empty()))return false;
    unsigned triangles=0,bodies=0,owners=0,audioChoices=0;
    std::vector<unsigned> debrisIds;
    for(const auto& config:bank.items){
        if(!validRoomAudio(config))return false;
        for(const auto& cue:config.audio)if((audioChoices+=unsigned(cue.choices.size()))>512)return false;
        if(!validRoomLaunch(config.launch)||(bodies+=unsigned(config.rigidDebris.size()))>512)return false;
        for(const auto& body:config.rigidDebris){
            if(!validRoomDebris(body)||(owners+=unsigned(body.owners.size()))>2048)return false;
            debrisIds.insert(debrisIds.end(),body.owners.begin(),body.owners.end());
        }
        const auto pieces=config.triangles.size()+config.circles.size()+config.segments.size();
        if(pieces>256||(triangles+=unsigned(pieces))>2048)return false;
        for(const auto& triangle:config.triangles)if(!validHitTriangle(triangle))return false;
        for(const auto& circle:config.circles)if(!validHitCircle(circle))return false;
        for(const auto& segment:config.segments)if(!validHitSegment(segment))return false;
    }
    std::sort(debrisIds.begin(),debrisIds.end());
    if(std::adjacent_find(debrisIds.begin(),debrisIds.end())!=debrisIds.end())return false;
    for(const auto& config:bank.items)for(auto id:config.colliders)
        if(std::none_of(terrain.shapes.begin(),terrain.shapes.end(),[&](const TerrainShape& shape){return shape.id==id;}))return false;
    // A retry reuses this bank and terrain; restore only its authored colliders.
    if(&bank==&bank_&&terrain_==&terrain)for(const auto& config:bank.items)for(auto id:config.colliders)
        terrain.setEnabled(id,true);
    // Copy first: restart can pass our own bank. Resize before storing callbacks.
    auto next=bank;clear();bank_=std::move(next);terrain_=&terrain;context_=context;effect_=effect;
    actors_.resize(bank_.items.size());visibility_.clear();bodies_.reserve(bodies);debrisOwners_.reserve(owners);random_=1;
    for(unsigned i=0;i<actors_.size();++i){
        auto& actor=actors_[i];const auto& config=bank_.items[i];auto& object=objects[i];
        actor.room=this;actor.index=i;actor.object=&object;actor.state.awake({},false,false,false);actor.state.start(0,0,0);
        object={};object.name="Room breakable";object.layer=config.layer;
        const auto first=!config.triangles.empty()?config.triangles[0][0]:
            !config.circles.empty()?config.circles[0].center:!config.segments.empty()?config.segments[0][0]:config.vertices[0];
        object.bounds={first.x,first.y,first.x,first.y};
        const auto extend=[&](Vec2 p){object.bounds.left=std::min(object.bounds.left,p.x);object.bounds.right=std::max(object.bounds.right,p.x);
            object.bounds.bottom=std::min(object.bounds.bottom,p.y);object.bounds.top=std::max(object.bounds.top,p.y);};
        if(config.triangles.empty()&&config.circles.empty()&&config.segments.empty()){
            object.hitPolygon=&config.vertices;for(const auto& p:config.vertices)extend(p);
        }else{
            object.hitTriangles=config.triangles.empty()?nullptr:config.triangles.data();object.hitTriangleCount=unsigned(config.triangles.size());
            for(const auto& triangle:config.triangles)for(const auto& p:triangle)extend(p);
            object.hitCircles=config.circles.empty()?nullptr:config.circles.data();object.hitCircleCount=unsigned(config.circles.size());
            object.hitSegments=config.segments.empty()?nullptr:config.segments.data();object.hitSegmentCount=unsigned(config.segments.size());
            for(const auto& segment:config.segments)for(const auto& p:segment)extend(p);
            for(const auto& circle:config.circles){
                extend({circle.center.x-circle.radius,circle.center.y-circle.radius});
                extend({circle.center.x+circle.radius,circle.center.y+circle.radius});
            }
        }
        object.receiver=&actor;object.onHit=receive;object.receivesAreaHits=true;
        for(auto owner:config.whole)visibility_.push_back({owner,i,false});
        for(auto owner:config.remnants)visibility_.push_back({owner,i,true});
        for(const auto& piece:config.rigidDebris){
            Body body;body.config=&piece;body.actor=i;body.position={piece.physics[0],piece.physics[1]};body.history.reset(body.position);body.rotationHistory.reset({});
            for(auto owner:piece.owners)debrisOwners_.push_back({owner,unsigned(bodies_.size())});
            bodies_.push_back(body);
        }
    }
    std::sort(visibility_.begin(),visibility_.end(),[](const Visibility& a,const Visibility& b){return a.owner<b.owner;});
    std::sort(debrisOwners_.begin(),debrisOwners_.end(),[](const DebrisOwner& a,const DebrisOwner& b){return a.owner<b.owner;});
    return true;
}
void RoomBreakables::receive(void* context,const NailHit& hit,HitOutcome)
{
    auto& actor=*static_cast<Actor*>(context);auto& room=*actor.room;
    const auto& config=room.bank_.items[actor.index];
    // BreakableObject uses mass-dependent impulses and accepts Nail/Spell only.
    if(config.impulse&&hit.type==AttackType::Generic)return;
    if(!actor.state.breakNow())return;
    actor.object->colliderEnabled=false;
    for(auto& shape:room.terrain_->shapes)
        if(std::find(config.colliders.begin(),config.colliders.end(),shape.id)!=config.colliders.end())shape.enabled=false;
    const unsigned direction=hit.direction==Strike::Right?0:hit.direction==Strike::Up?1:hit.direction==Strike::Left?2:3;
    const unsigned offset=4*direction;
    for(auto& body:room.bodies_)if(body.actor==actor.index){
        const auto& physics=body.config->physics;body.active=true;
        const float angle=room.random(config.launch[offset],config.launch[offset+1])*.01745329252f;
        const float impact=std::isfinite(hit.multiplier)?std::max(0.f,hit.multiplier):1;
        const float speed=room.random(config.launch[offset+2],config.launch[offset+3])*impact/(config.impulse?physics[8]:1);
        if(body.config->flags&1)body.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
        body.rotation=(direction==0?config.angle:direction==2?-config.angle:0)*.01745329252f;
        if(physics[3]!=physics[4])body.rotation+=room.random(physics[3],physics[4]);
        else body.rotation+=physics[3];
        body.rotationPose={body.rotation,0};body.rotationHistory.reset(body.rotationPose);
    }
    actor.audioSeedBefore=room.audioSeed_;
    for(const auto& cue:config.audio){
        const auto random=[&](float low,float high){
            room.audioSeed_=room.audioSeed_*1664525u+1013904223u;
            return low+(high-low)*float(room.audioSeed_>>8)*(1.f/16777216.f);
        };
        float total=0;for(const auto& choice:cue.choices)total+=choice.weight;
        float value=random(0,total);const RoomBreakSoundChoice* selected=&cue.choices.back();
        for(const auto& choice:cue.choices){if(value<choice.weight){selected=&choice;break;}value-=choice.weight;}
        const float pitch=random(cue.low,cue.high);
        if(selected->file[0]){
            auto& event=room.soundEvents_[room.soundSerial_%room.soundEvents_.size()];
            event={selected,config.owner,cue.gain,pitch};++room.soundSerial_;
        }
    }
    if(room.effect_)room.effect_(room.context_,config,actor.index,hit);
}
float RoomBreakables::alpha(unsigned owner)const
{
    const auto debris=std::lower_bound(debrisOwners_.begin(),debrisOwners_.end(),owner,
        [](const DebrisOwner& item,unsigned value){return item.owner<value;});
    if(debris!=debrisOwners_.end()&&debris->owner==owner)return bodies_[debris->body].active?1:0;
    const auto found=std::lower_bound(visibility_.begin(),visibility_.end(),owner,
        [](const Visibility& item,unsigned value){return item.owner<value;});
    if(found==visibility_.end()||found->owner!=owner)return 1;
    return actors_[found->index].state.activation().broken==found->remnant?1:0;
}
bool RoomBreakables::isDebris(unsigned owner)const
{
    const auto found=std::lower_bound(debrisOwners_.begin(),debrisOwners_.end(),owner,
        [](const DebrisOwner& item,unsigned value){return item.owner<value;});
    return found!=debrisOwners_.end()&&found->owner==owner;
}
float RoomBreakables::random(float lo,float hi)
{random_=1664525u*random_+1013904223u;return lo+(hi-lo)*float(random_>>8)/16777216.f;}
void RoomBreakables::fixedUpdate(float seconds,const World& world)
{
    if(!std::isfinite(seconds)||seconds<=0||seconds>.1f)return;
    for(auto& body:bodies_)if(body.active){
        PresentationPosition::Step history(body.history,body.position),rotationHistory(body.rotationHistory,body.rotationPose);
        const auto& config=*body.config;const auto& physics=config.physics;
        const float previous=body.age;body.age+=seconds;
        if(config.flags&1){
            if(previous+1e-6f<2*physics[15]&&body.age+1e-6f>=2*physics[15])body.spin+=body.velocity.x*physics[5]*physics[15]/physics[9];
            if(config.hull[0])decorBody::step(body.position,body.velocity,body.rotation,body.spin,body.sleep,body.floor,
                seconds,physics,config.hull,-60.f*physics[10],world,[&](float lo,float hi){return random(lo,hi);});
            else{
                body.velocity.y-=60.f*physics[10]*seconds;
                body.velocity.x/=1+physics[7]*seconds;body.velocity.y/=1+physics[7]*seconds;
                body.spin/=1+physics[6]*seconds;body.rotation+=body.spin*seconds;
                body.position.x+=body.velocity.x*seconds;body.position.y+=body.velocity.y*seconds;
            }
        }
        body.rotationPose.x=body.rotation;
    }
}
void RoomBreakables::visualUpdate(float seconds,Vec2 camera,float aspect)
{
    if(!std::isfinite(seconds)||seconds<=0)return;
    for(auto& body:bodies_)if(body.active&&(body.config->flags&2)){
        const auto& config=*body.config;
        // FinishingRigidBody.Update accumulates only sleeping time, then keeps
        // shrinking even if the body wakes. A zero wait still requires sleep.
        if(!body.shrinking&&body.sleep>=.5f){
            body.quiet+=seconds;
            if(body.quiet>config.finish[0]){body.shrinking=true;body.quiet=0;}
        }
        if(body.shrinking){
            body.quiet+=seconds;
            body.shrink=config.finish[1]>0?std::max(0.f,1-body.quiet/config.finish[1]):0;
            if(body.quiet>config.finish[1]){body.active=false;continue;}
        }
        if(!(config.flags&4)&&std::isfinite(aspect)&&aspect>0&&
           std::isfinite(camera.x)&&std::isfinite(camera.y)){
            // approximation: collider radius bounds the source root renderer;
            // ten scaled port Updates stand in for ten invisible Unity Updates.
            const float radius=config.physics[14]*body.shrink;
            const float halfHeight=120*(38.1f+config.physics[2])/roomFocalLength(240*aspect,240);
            const bool visible=halfHeight>0&&
                std::abs(body.position.x-camera.x)<=halfHeight*aspect+radius&&
                std::abs(body.position.y-camera.y)<=halfHeight+radius;
            body.invisibleFrames=visible?0:body.invisibleFrames+1;
            if(body.invisibleFrames>=10)body.active=false;
        }
    }
}
void RoomBreakables::transform(RoomDraw& draw,PresentationTime time)const
{
    const auto found=std::lower_bound(debrisOwners_.begin(),debrisOwners_.end(),draw.owner,
        [](const DebrisOwner& item,unsigned value){return item.owner<value;});
    if(found==debrisOwners_.end()||found->owner!=draw.owner)return;
    const auto& body=bodies_[found->body];if(!body.active)return;
    const auto position=body.history.sample(body.position,time);const auto& physics=body.config->physics;
    const float rotation=body.rotationHistory.sample(body.rotationPose,time).x;
    auto& v=draw.values;const float c=std::cos(rotation)*body.shrink,s=std::sin(rotation)*body.shrink;
    const float a=v[0],b=v[1],x=v[2]-physics[0],d=v[3],e=v[4],y=v[5]-physics[1];
    v[0]=c*a-s*d;v[1]=c*b-s*e;v[2]=position.x+c*x-s*y;
    v[3]=s*a+c*d;v[4]=s*b+c*e;v[5]=position.y+s*x+c*y;v[13]*=body.shrink;v[14]*=body.shrink;
}
bool RoomBreakables::testBreak(unsigned owner,const NailHit& hit)
{
    for(auto& actor:actors_)if(bank_.items[actor.index].owner==owner){
        if(actor.state.activation().broken)return false;
        receive(&actor,hit,HitOutcome::Damaged);return true;
    }
    return false;
}
unsigned RoomBreakables::brokenCount()const
{unsigned count=0;for(const auto& actor:actors_)count+=actor.state.activation().broken;return count;}

namespace
{
float fxSample(const std::array<float,60>& values,unsigned offset,float t)
{
    const float f=std::clamp(t,0.0f,1.0f)*7;
    const unsigned i=std::min(6u,unsigned(f));
    return values[offset+i]+(values[offset+i+1]-values[offset+i])*(f-i);
}
}
bool DecorEffects::load(const char* config,const char* artwork)
{
    ownerCount_=0;ownerShapes_={};ownerVisual_={};ownerSorting_={};
    ready_=false;doorReady_=false;grassProfileCount_=0;grassEmitter_=0xffffffffu;
    doorRockEmitters_={0xffffffffu,0xffffffffu};reset();
    if(!visuals.load(artwork))return false;
    if(visuals.draws.size()!=3)return false;
    dustSorting_={visuals.draws.front().values[11],visuals.draws.front().values[12]};
    for(unsigned i=0;i<flashSorting_.size();++i)
        flashSorting_[i]={visuals.draws[i+1].values[11],visuals.draws[i+1].values[12]};
    auto* file=std::fopen(config,"rb");if(!file)return false;
    char magic[4]{};unsigned header[4]{};
    bool ok=std::fread(magic,1,4,file)==4&&std::memcmp(magic,"HFX7",4)==0&&std::fread(header,4,4,file)==4;
    ok=ok&&header[0]>0&&header[0]<=emitters_.size()&&header[1]>0&&header[1]<=profiles_.size();
    if(!ok){std::fclose(file);return false;}
    emitterCount_=header[0];roomEmitterBase_=emitterCount_;roomEffectsBound_=false;roomProfiles_.clear();roomFlings_.clear();roomForces_.clear();roomEmissions_.clear();roomDraws_.clear();
    profileCount_=header[1];dust_={header[2],header[3]};
    for(unsigned i=0;ok&&i<emitterCount_;++i){
        auto& e=emitters_[i];e={};unsigned fields[3]{};
        ok=std::fread(fields,4,3,file)==3&&std::fread(e.v.data(),4,e.v.size(),file)==e.v.size()&&std::fread(&e.rate,4,1,file)==1&&
           std::fread(e.cone.data(),4,e.cone.size(),file)==e.cone.size()&&
           std::fread(e.bases.data(),4,e.bases.size(),file)==e.bases.size()&&
           std::fread(e.axes.data(),4,e.axes.size(),file)==e.axes.size();
        e.sprite=fields[0];e.tiles=fields[1];e.count=fields[2];
        ok=ok&&e.tiles>0&&e.sprite<visuals.atlas.sprites.size()&&e.tiles<=visuals.atlas.sprites.size()-e.sprite&&e.count<=capacity;
        for(float v:e.v)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.cone)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.bases)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.axes)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        ok=ok&&e.v[18]>=0&&e.v[18]<=1&&e.v[19]>=0&&e.v[58]>=0&&e.v[58]<=1&&e.v[59]>=0;
        ok=ok&&e.cone[0]>=0;
        ok=ok&&std::isfinite(e.rate)&&e.rate>=0&&e.rate<=1e6f&&e.v[6]>=0&&e.v[7]>0&&e.v[8]>0&&e.v[11]>0&&e.v[12]>0;
    }
    unsigned bound=0;
    for(unsigned i=0;ok&&i<profileCount_;++i){
        auto& p=profiles_[i];unsigned fields[3]{};
        ok=std::fread(fields,4,3,file)==3&&std::fread(p.v.data(),4,6,file)==6;
        p.owner=fields[0];p.emitters={fields[1],fields[2]};
        for(unsigned e:p.emitters){if(e==0xffffffffu)continue;ok=ok&&e<emitterCount_;if(ok)bound+=emitters_[e].count;}
        for(float v:p.v)ok=ok&&std::isfinite(v)&&std::abs(v)<100000;
        for(unsigned j=0;j<i;++j)ok=ok&&profiles_[j].owner!=p.owner;
    }
    // Approximation source: owner 129's room breakable has the closest exported
    // small-rock particle art in this pool. Great Door prefab 4328 is instead a
    // MeshRenderer/Rigidbody prefab, which the 2D decor renderer cannot draw.
    for(unsigned i=0;i<profileCount_;++i)if(profiles_[i].owner==129){
        doorRockEmitters_=profiles_[i].emitters;break;
    }
    for(auto& f:flashes_){
        unsigned fields[2]{};ok=ok&&std::fread(fields,4,2,file)==2&&std::fread(&f.fps,4,1,file)==1;
        f.sprite=fields[0];f.count=fields[1];
        ok=ok&&f.count>0&&std::isfinite(f.fps)&&f.fps>0&&f.sprite<visuals.atlas.sprites.size()&&f.count<=visuals.atlas.sprites.size()-f.sprite;
    }
    ok=ok&&std::fread(nailTransform_.data(),4,4,file)==4;
    for(float v:nailTransform_)ok=ok&&std::isfinite(v);
    ok=ok&&nailTransform_[0]>0&&nailTransform_[1]>=nailTransform_[0]&&nailTransform_[1]<=16&&
        nailTransform_[2]>=0&&nailTransform_[3]>nailTransform_[2]&&nailTransform_[3]<=360&&
        std::floor(nailTransform_[2])==nailTransform_[2]&&std::floor(nailTransform_[3])==nailTransform_[3];
    ok=ok&&std::fread(audioPitch_.data(),4,2,file)==2&&
        std::isfinite(audioPitch_[0])&&std::isfinite(audioPitch_[1])&&audioPitch_[0]>0&&audioPitch_[1]>=audioPitch_[0]&&audioPitch_[1]<=4&&
        dust_[0]<emitterCount_&&dust_[1]<emitterCount_;
    if(ok){
        char tag[4]{};const int first=std::fgetc(file);
        if(first==EOF)grassProfileCount_=0;
        else{
            tag[0]=char(first);
            ok=std::fread(tag+1,1,3,file)==3&&std::memcmp(tag,"GRS1",4)==0;
            unsigned fields[2]{};
            ok=ok&&std::fread(fields,4,2,file)==2&&std::fread(&grassAnimationFps_,4,1,file)==1;
            grassEmitter_=fields[0];grassParticleCount_=fields[1];grassProfileCount_=0;
            ok=ok&&grassEmitter_<emitterCount_&&grassParticleCount_>0&&grassParticleCount_<=emitters_[grassEmitter_].count&&
                std::isfinite(grassAnimationFps_)&&grassAnimationFps_>=0;
            if(ok)bound+=(grassParticleCount_+1)*39;
            const int next=std::fgetc(file);
            if(next!=EOF){
                tag[0]=char(next);
                ok=ok&&std::fread(tag+1,1,3,file)==3&&!std::memcmp(tag,"DMX2",4)&&
                    std::fread(doorBodies_.data(),4,32,file)==32&&std::fread(doorAlpha_.data(),4,32,file)==32&&
                    std::fread(doorSize_.data(),4,32,file)==32&&std::fread(doorDelays_.data(),4,3,file)==3&&
                    std::fread(doorHulls_.data(),4,34,file)==34;
                for(const auto& b:doorBodies_){
                    for(float v:b)ok=ok&&std::isfinite(v);
                    ok=ok&&b[3]>0&&b[6]>=0&&b[7]>=0&&b[8]>0&&b[9]>0&&b[10]>=0&&
                        b[11]>=0&&b[12]>=0&&b[12]<=1&&b[13]>=0&&b[14]>0&&b[15]>0&&b[15]<=.1f;
                }
                for(const auto& a:doorAlpha_)for(unsigned i=0;i<8;++i)
                    ok=ok&&std::isfinite(a[2*i])&&std::isfinite(a[2*i+1])&&a[2*i]>=0&&a[2*i]<=1&&
                        a[2*i+1]>=0&&a[2*i+1]<=1&&(i%4==0||a[2*i]>a[2*i-2]);
                for(const auto& a:doorSize_)for(float v:a)ok=ok&&std::isfinite(v)&&v>=0;
                for(float delay:doorDelays_)ok=ok&&std::isfinite(delay)&&delay>=0;
                for(const auto& h:doorHulls_){
                    ok=ok&&h[0]>=3&&h[0]<=7&&std::floor(h[0])==h[0];
                    for(float v:h)ok=ok&&std::isfinite(v);
                }
                ok=ok&&std::fread(tag,1,4,file)==4&&!std::memcmp(tag,"DFX1",4)&&
                    std::fread(doorEmitters_.data(),4,7,file)==7&&
                    std::fread(doorSorting_.data(),4,6,file)==6&&std::fgetc(file)==EOF;
                for(unsigned id:doorEmitters_)ok=ok&&id<emitterCount_;
                for(const auto& pair:doorSorting_)for(float v:pair)ok=ok&&std::isfinite(v);
                if(ok){doorReady_=true;bound+=1152;}
            }
        }
    }
    std::fclose(file);
    if(ok)bound+=profileCount_*(std::max(emitters_[dust_[0]].count,emitters_[dust_[1]].count)+2);
    if(!ok||bound>capacity)return false;
    visuals.draws.clear();visuals.cameraBounds.clear();visuals.draws.reserve(capacity+9+stalactiteSpriteCapacity);
    ready_=true;return true;
}
bool DecorEffects::loadStalactites(const char* path)
{
    if(!ready_)return false;
    FILE*f=std::fopen(path,"rb");if(!f)return false;char tag[4]{};
    std::array<unsigned,5> ids{};std::array<std::array<float,3>,5> offsets{};
    Flash thunk;std::array<float,2> sorting{};
    bool ok=std::fread(tag,1,4,f)==4&&!std::memcmp(tag,"STF1",4)&&std::fread(ids.data(),4,5,f)==5;
    for(auto&v:offsets)ok=ok&&std::fread(v.data(),4,3,f)==3;
    ok=ok&&std::fread(&thunk.sprite,4,1,f)==1&&std::fread(&thunk.count,4,1,f)==1&&
        std::fread(&thunk.fps,4,1,f)==1&&std::fread(sorting.data(),4,2,f)==2;
    for(unsigned id:ids)ok=ok&&id<emitterCount_;
    for(const auto&v:offsets)for(float x:v)ok=ok&&std::isfinite(x)&&std::abs(x)<100000;
    for(float x:sorting)ok=ok&&std::isfinite(x);
    ok=ok&&thunk.count&&thunk.sprite<visuals.atlas.sprites.size()&&thunk.count<=visuals.atlas.sprites.size()-thunk.sprite&&
        std::isfinite(thunk.fps)&&thunk.fps>0&&std::fgetc(f)==EOF;
    std::fclose(f);if(!ok)return false;
    stalactiteEmitters_=ids;stalactiteOffsets_=offsets;stalactiteThunk_=thunk;stalactiteThunkSorting_=sorting;
    stalactiteReady_=true;return true;
}
void DecorEffects::stalactiteEmission(unsigned kind,Vec2 origin,float rotation)
{
    const auto&offset=stalactiteOffsets_[kind];const float c=std::cos(rotation),s=std::sin(rotation);
    origin={origin.x+c*offset[0]-s*offset[1],origin.y+s*offset[0]+c*offset[1]};
    const unsigned id=stalactiteEmitters_[kind];const auto&e=emitters_[id];
    for(auto&emission:geoEmissions_)if(!emission.seed){
        emission={origin,rotation,0,0,id,0,seed_,offset[2]};
        for(unsigned n=0;n<e.count*12;++n)random(0,1);
        ++activeEmissions_;break;
    }
    visualActive_=true;
}
bool DecorEffects::loadInitialFall(const char* path)
{
    initialFallReady_=false;if(!ready_)return false;
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    const auto read=[f](void* data,std::size_t bytes){return std::fread(data,1,bytes,f)==bytes;};
    char tag[4]{};
    bool ok=read(tag,4)&&!std::memcmp(tag,"IFX1",4)&&read(initialEmitters_.data(),sizeof(initialEmitters_))&&
        read(initialOffsets_.data(),sizeof(initialOffsets_))&&read(initialSorting_.data(),sizeof(initialSorting_))&&
        read(initialForceX_.data(),sizeof(initialForceX_))&&read(&flockCycles_,4)&&read(&initialLines_.sprite,4)&&
        read(&initialLines_.count,4)&&read(&initialLines_.fps,4)&&read(initialLinesOffset_.data(),sizeof(initialLinesOffset_));
    for(auto id:initialEmitters_)ok &= id<emitterCount_;
    for(const auto& v:initialOffsets_)for(float x:v)ok &= std::isfinite(x)&&std::abs(x)<100000;
    for(const auto& v:initialSorting_)for(float x:v)ok &= std::isfinite(x);
    for(const auto& v:initialForceX_)for(float x:v)ok &= std::isfinite(x)&&std::abs(x)<100000;
    for(float x:initialLinesOffset_)ok &= std::isfinite(x)&&std::abs(x)<100000;
    ok &= std::isfinite(flockCycles_)&&flockCycles_>0&&flockCycles_<100&&initialLines_.count&&
        std::isfinite(initialLines_.fps)&&initialLines_.fps>0&&initialLines_.sprite<visuals.atlas.sprites.size()&&
        initialLines_.count<=visuals.atlas.sprites.size()-initialLines_.sprite&&std::fgetc(f)==EOF;
    std::fclose(f);initialFallReady_=ok;return ok;
}
void DecorEffects::initialFall(bool flock,Vec2 hero,const World& world)
{
    if(!ready_||!initialFallReady_||!std::isfinite(hero.x)||!std::isfinite(hero.y))return;

    for(unsigned i=flock?2:0;i<(flock?3u:2u);++i){
        const auto& offset=initialOffsets_[i];const auto id=initialEmitters_[i];const auto& e=emitters_[id];
        const Vec2 origin=flock?Vec2{offset[0],offset[1]}:Vec2{hero.x+offset[0],hero.y+offset[1]};
        for(auto& emission:geoEmissions_)if(!emission.seed){
            emission={origin,0,0,0,id,0,seed_,offset[2]};
            for(unsigned n=0;n<e.count*12;++n)random(0,1);
            ++activeEmissions_;break;
        }
    }
    if(!flock){
        // Approximation: source Small Shrinker rigidbodies reuse the shared rock
        // art/terrain model; original 2..3 per side, angles and speeds retained.
        for(unsigned side=0;side<2;++side){
            const auto id=doorRockEmitters_[0];if(id>=emitterCount_)continue;
            const auto first=count_;const unsigned amount=unsigned(random(2,4));
            const Vec2 origin=decorTerrain::separate({hero.x,hero.y-.9f},.5f*emitters_[id].v[59]+.01f,world);
            emit(world,id,origin,0,0,false,amount);
            for(unsigned n=first;n<count_;++n){auto& p=particles_[n];
                const float angle=random(side?40.f:95.f,side?85.f:140.f)*.01745329252f,speed=random(12,15);
                p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};p.z=.01f;p.history.reset(p.position);
            }
        }
        if(count_<capacity){auto& p=particles_[count_++];p={};p.flash=4;p.age=.001f;
            p.life=initialLines_.count/initialLines_.fps;p.z=initialLinesOffset_[2];
            p.position={hero.x+initialLinesOffset_[0],hero.y+initialLinesOffset_[1]};p.history.reset(p.position);
        }
        // Approximation: reuse the existing bounded shake for BigShake/AverageShake.
        shake_=.12f;
    }
    visualActive_=true;
}
void DecorEffects::stalactiteCue(StalactiteCue cue,Vec2 origin,float degrees,Strike direction,const World&world)
{
    if(!ready_||!stalactiteReady_)return;
    const float angle=degrees*.017453292519943295f;
    if(cue==StalactiteCue::Start)stalactiteEmission(0,origin,angle);
    else if(cue==StalactiteCue::Fall)stalactiteEmission(1,origin,angle);
    else if(cue==StalactiteCue::Land){
        // Source land effects spawn at a downward layer-8 ray hit, not the
        // root pivot above the floor. Query the real enabled room geometry.
        float distance=0;auto terrain=world;terrain.queriesHitTriggers=false;
        if(rayTerrain(origin,{0,-1},10,terrain,distance))origin.y-=distance;
        stalactiteEmission(3,origin,0);
        if(count_<capacity){auto&p=particles_[count_++];p={};p.flash=3;p.freshFlash=true;
            p.age=.001f;p.life=float(stalactiteThunk_.count)/stalactiteThunk_.fps;p.position=origin;p.history.reset(origin);}
    }else if(cue==StalactiteCue::Break){
        stalactiteEmission(4,origin,angle);geoRockHit(origin,0,direction,true);
    }else if(cue==StalactiteCue::Hit||cue==StalactiteCue::UpBreak){
        if(cue==StalactiteCue::UpBreak){
            const unsigned dust=dust_[0];const auto& emitter=emitters_[dust];
            for(auto& emission:geoEmissions_)if(!emission.seed){
                emission={origin,0,0,0,dust,0,seed_,.1f};
                for(unsigned i=0;i<emitter.count*12;++i)random(0,1);
                ++activeEmissions_;break;
            }
            // approximation: source 4328 tk2d/Rigidbody rubble uses the shared
            // small-rock particle art and terrain contacts. C# count/speed are retained.
            const unsigned id=doorRockEmitters_[0],first=count_;
            if(id<emitterCount_)emit(world,id,origin,0,0,false,unsigned(random(10,13)));
            for(unsigned i=first;i<count_;++i){auto&p=particles_[i];const float a=random(0,6.283185307f),speed=float(unsigned(random(15,20)));
                p.position=decorTerrain::separate(origin,.5f*emitters_[id].v[59]*p.size+.01f,world);
                p.velocity={std::cos(a)*speed,std::sin(a)*speed};p.history.reset(p.position);}
        }
        nailStrike(origin,0);
    }
}
void DecorEffects::stalactiteTrail(Vec2 origin,float degrees,float seconds,float&clock,const World&world)
{
    if(!ready_||!stalactiteReady_||seconds<=0)return;
    const unsigned id=stalactiteEmitters_[2];const auto&e=emitters_[id];
    clock+=seconds*e.rate;const unsigned amount=unsigned(clock);clock-=float(amount);
    const float a=degrees*.017453292519943295f,c=std::cos(a),s=std::sin(a);const auto&o=stalactiteOffsets_[2];
    const unsigned first=count_;emit(world,id,{origin.x+c*o[0]-s*o[1],origin.y+s*o[0]+c*o[1]},a,0,false,amount);
    for(unsigned i=first;i<count_;++i)particles_[i].z=o[2];
    visualActive_=true;
}
void DecorEffects::bindGrass(const GrassBank& bank)
{
    grassProfileCount_=0;
    if(!ready_||grassEmitter_>=emitterCount_||bank.count>grassProfiles_.size())return;
    for(unsigned i=0;i<bank.count;++i){
        const auto& source=bank.bushes[i];if(!source.interactive())continue;
        auto& profile=grassProfiles_[grassProfileCount_++];profile.owner=source.owner;profile.count=grassParticleCount_;
        const auto& m=source.stubValues;
        const float localX=source.particleOffset[0],localY=source.particleOffset[1];
        profile.origin={m[0]*localX+m[1]*localY+m[2],
                        m[3]*localX+m[4]*localY+m[5]};
        profile.z=m[6]+m[13]*localX+m[14]*localY+source.particleOffset[2];
    }
}
void DecorEffects::reset(unsigned seed)
{
    for(auto& emission:roomEmissions_)emission={};
    roomDraws_.clear();
    count_=0;sounds_=0;plantSounds_=0;broken_={};roomBroken_={};emissions_={};geoEmissions_={};platformEmissions_={};doorEmissions_={};ownerBroken_={};ownerEmissions_={};activeEmissions_=0;visualActive_=false;shake_=0;seed_=seed?seed:1;pitch_=plantPitch_=1;viewAspect_=0;visuals.draws.clear();
}
bool DecorEffects::loadOwners(const char* path)
{
    ownerCount_=0;ownerShapes_={};ownerVisual_={};ownerSorting_={};
    ownerParticleScale_.fill({1,1});ownerForceX_={};
    if(!ready_)return false;
    auto* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4]{};unsigned count=0;
    bool ok=std::fread(magic,1,4,file)==4&&(std::memcmp(magic,"OWF1",4)==0||std::memcmp(magic,"OWF2",4)==0)&&
        std::fread(&count,4,1,file)==1&&count==ownerProfiles_.size();
    std::array<bool,emitterCapacity> used{};unsigned total=0;
    for(unsigned i=0;ok&&i<count;++i){
        auto& p=ownerProfiles_[i];unsigned row[8]{};
        ok=std::fread(row,4,8,file)==8;p.owner=row[0];p.count=row[1];
        ok=ok&&p.owner>0&&p.count>0&&p.count<=p.emitters.size();
        for(unsigned j=0;ok&&j<p.count;++j){
            const auto id=row[j+2];ok=id<emitterCount_&&!used[id];
            if(ok){p.emitters[j]=id;used[id]=true;++total;}
        }
        for(unsigned j=0;j<i;++j)ok=ok&&p.owner!=ownerProfiles_[j].owner;
    }
    unsigned metadata=0;ok=ok&&std::fread(&metadata,4,1,file)==1&&metadata==total&&metadata<=ownerEmissions_.size();
    for(unsigned i=0;ok&&i<metadata;++i){
        unsigned row[3]{};float sorting[2]{};
        ok=std::fread(row,4,3,file)==3&&std::fread(sorting,4,2,file)==2;
        ok=ok&&row[0]<emitterCount_&&used[row[0]]&&(row[1]==5||row[1]==10||row[1]==12)&&row[2]<=1;
        Vec2 size{1,1};
        if(ok&&magic[3]=='2')ok=std::fread(&size,sizeof(size),1,file)==1&&
            std::isfinite(size.x)&&std::isfinite(size.y)&&size.x>0&&size.y>0;
        Vec2 forceX{};
        if(ok&&magic[3]=='2')ok=std::fread(&forceX,sizeof(forceX),1,file)==1&&
            std::isfinite(forceX.x)&&std::isfinite(forceX.y);
        if(ok){ownerParticleScale_[row[0]]=size;ownerForceX_[row[0]]=forceX;}
        if(ok){used[row[0]]=false;ownerShapes_[row[0]]=row[1];ownerVisual_[row[0]]=row[2]!=0;
            ownerSorting_[row[0]]={sorting[0],sorting[1]};
            for(float v:sorting)ok=ok&&std::isfinite(v);}
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(!ok){ownerShapes_={};ownerVisual_={};return false;}
    ownerCount_=count;ownerBroken_={};return true;
}
bool DecorEffects::ownerBreak(unsigned owner,Strike direction,const World&)
{
    unsigned index=0;for(;index<ownerCount_&&ownerProfiles_[index].owner!=owner;++index){}
    if(index==ownerCount_||ownerBroken_[index])return false;
    ownerBroken_[index]=true;
    if(owner!=318&&owner!=2421&&owner!=2422&&owner!=2423){
        // All seven Tute doors use the same source decor table (1059/clip 92).
        ++sounds_;pitch_=random(audioPitch_[0],audioPitch_[1]);shake_=.12f;
    }
    const auto& profile=ownerProfiles_[index];
    for(unsigned i=0;i<profile.count;++i){
        auto free=std::find_if(ownerEmissions_.begin(),ownerEmissions_.end(),[](const auto& e){return !e.seed;});
        if(free==ownerEmissions_.end())return false;
        const auto id=profile.emitters[i];const auto& e=emitters_[id];
        *free={};free->emitter=id;free->origin={e.v[0],e.v[1]};free->z=e.v[2];
        free->seed=seed_?seed_:1;random(0,1);free->owned=true;
        free->basis=direction==Strike::Left?1:direction==Strike::Right?2:0;
        ++activeEmissions_;
    }
    return true;
}
void DecorEffects::platformContact(const std::array<unsigned,3>& ids,Vec2 origin,Vec2 size,
                                  float dustRate,Vec2 streamOffset,bool rocks,const World&)
{
    if(!ready_||!std::isfinite(dustRate)||dustRate<=0||size.x<=0||size.y<=0)return;
    platformEmitters_=ids;
    for(unsigned part=0;part<ids.size();++part){
        if(ids[part]>=emitterCount_||(part==2&&!rocks))continue;
        const auto& e=emitters_[ids[part]];
        for(auto& emission:platformEmissions_)if(!emission.seed){
            emission={};emission.emitter=ids[part];emission.origin=origin;
            if(part==1){emission.origin.x+=streamOffset.x;emission.origin.y+=streamOffset.y;}
            emission.z=part==2?.003f:part==1?-.08f:-.1f;
            emission.scale={size.x,part==1?1.0f:size.y};
            emission.rate=part==0?dustRate:random(e.axes[8],e.axes[9]);emission.box=true;
            // The shared fixed pool bounds simultaneous source emitters. Admission
            // keeps native lifetime/rates; it never extends a particle's lifetime.
            emission.cap=std::min(e.count,unsigned(std::ceil(emission.rate*e.v[6]-1e-5f)));
            if(emission.rate<=0||!emission.cap)break;
            seed_=seed_*1664525u+1013904223u;emission.seed=seed_?seed_:1;++activeEmissions_;break;
        }
    }
}
float DecorEffects::random(float low,float high)
{
    // approximation: a private RNG avoids changing gameplay's random stream.
    seed_^=seed_<<13;seed_^=seed_>>17;seed_^=seed_<<5;
    return low+(high-low)*(float(seed_&0xffffff)/16777216.0f);
}
void DecorEffects::emit(const World& world,unsigned index,Vec2 origin,float angle,float floor,bool placed,unsigned amount,unsigned basis,Vec2 offset)
{
    if(index>=emitterCount_)return;
    const auto& e=emitters_[index];const auto& v=e.v;
    std::optional<decorTerrain::FixtureQuery> birthQuery;
    for(unsigned i=0;i<amount&&count_<capacity;++i){
        auto& p=particles_[count_++];p={};p.emitter=index;
        const float sample=random(-std::abs(v[4])*.5f,std::abs(v[4])*.5f);
        const float a=angle*(v[4]<0?-1:1)+v[3]+sample,r=random(0,v[5]);
        p.position=placed?Vec2{v[0]+offset.x,v[1]+offset.y}:origin;
        p.position.x+=std::cos(a)*r;p.position.y+=std::sin(a)*r;
        const float speed=random(v[9],v[10]);p.velocity={std::cos(a)*speed,std::sin(a)*speed};
        if(placed){
            // Shape uses the original local arc, transformed after C# rotation.
            // Approximation: radial RNG remains uniform; velocity uses the same
            // post-hit transformed local direction, including reflected parents.
            const float theta=sample+std::abs(v[4])*.5f;
            const float radius=v[5]>0?r/v[5]:0;
            const float x=std::cos(theta)*radius,y=std::sin(theta)*radius;
            const unsigned b=basis*4;
            p.position={v[0]+x*e.bases[b]+y*e.bases[b+2],v[1]+x*e.bases[b+1]+y*e.bases[b+3]};
            const unsigned k=basis*6;const auto& m=e.axes;
            const float dx=std::cos(theta)*m[k]+std::sin(theta)*m[k+3];
            const float dy=std::cos(theta)*m[k+1]+std::sin(theta)*m[k+4];
            const float dz=std::cos(theta)*m[k+2]+std::sin(theta)*m[k+5];
            const float norm=std::sqrt(dx*dx+dy*dy+dz*dz);
            if(norm>0){p.velocity={dx*speed/norm,dy*speed/norm};p.velocityZ=dz*speed/norm;}
        }
        if(ownerShapes_[index]==5||ownerShapes_[index]==12){
            const float x=random(-.5f,.5f),y=random(-.5f,.5f),z=random(-.5f,.5f);
            const unsigned b=basis*6,d=basis*3;
            p.position={v[0]+offset.x+x*e.axes[b]+y*e.axes[b+2]+z*e.axes[b+4],
                        v[1]+offset.y+x*e.axes[b+1]+y*e.axes[b+3]+z*e.axes[b+5]};
            p.velocity={speed*e.bases[d],speed*e.bases[d+1]};p.velocityZ=speed*e.bases[d+2];
        }else if(e.cone[0]>0){
            // Reuse the same two random draws as the arc, keeping other FX RNG.
            // Approximation: uniform disk, projected cone; subsequent Z is omitted.
            const auto& q=e.cone;
            const float radius=std::sqrt(std::clamp(v[5]>0?r/v[5]:0,0.0f,1.0f));
            const float azimuth=a-angle*(v[4]<0?-1:1)-v[3];
            const float x=std::cos(azimuth)*radius,y=std::sin(azimuth)*radius;
            const float dx=x*q[0],dy=y*q[0],inv=1/std::sqrt(1+dx*dx+dy*dy);
            const float c=std::cos(angle),s=std::sin(angle);
            const Vec2 offset{x*q[1]+y*q[3],x*q[2]+y*q[4]};
            const Vec2 velocity{(dx*q[5]+dy*q[7]+q[9])*speed*inv,
                                (dx*q[6]+dy*q[8]+q[10])*speed*inv};
            p.position={origin.x+c*offset.x-s*offset.y,origin.y+s*offset.x+c*offset.y};
            p.velocity={c*velocity.x-s*velocity.y,s*velocity.x+c*velocity.y};
        }
        p.life=random(v[7],v[8]);p.size=random(v[11],v[12]);p.rotation=random(v[13],v[14]);
        if(ownerShapes_[index]){p.scaleX=ownerParticleScale_[index].x;p.scaleY=ownerParticleScale_[index].y;
            p.forceX=random(ownerForceX_[index].x,ownerForceX_[index].y);}
        p.gravity=v[15];
        p.force=random(v[16],v[17]);p.floor=floor;p.z=placed?v[2]:0;
        if(initialFallReady_)for(unsigned j=0;j<3;++j)if(index==initialEmitters_[j])
            p.forceX=random(initialForceX_[j][0],initialForceX_[j][1]);
        p.sprite=index==grassEmitter_&&grassAnimationFps_>0?e.sprite:e.sprite+std::min(e.tiles-1,unsigned(random(0,float(e.tiles))));
        p.moduleVelocity={random(v[50],v[51]),random(v[52],v[53])};
        p.forceZ=random(v[54],v[55]);p.moduleZ=random(v[56],v[57]);
        if(index>=roomEmitterBase_&&roomEffectsBound_&&roomForces_[index-roomEmitterBase_].active){
            p.forceX=random(0,1);p.force=random(0,1);p.forceZ=random(0,1);
        }
        // The smoke emitters have no rigidbody spin; reuse that scalar for the
        // stable per-particle TwoGradients blend without enlarging the pool.
        if(doorReady_&&(index==doorEmitters_[4]||index==doorEmitters_[5])){
            p.spin=random(0,1);p.floor=random(0,1); // Independent alpha/size blends; smoke has no collision floor.
        }
        const bool body=doorReady_&&(index==doorEmitters_[2]||index==doorEmitters_[3]);
        if(v[59]>0&&!body){
            const float radius=.5f*v[59]*p.size*v[22]+.01f;
            // Nearby births share ordered fixtures, without changing RNG/birth order.
            // Each separation pass falls back if penetration leaves this envelope.
            if(!birthQuery&&amount>1&&count_<capacity)
                birthQuery.emplace(world,decorTerrain::area(p.position,p.position,radius+2.0f));
            p.position=decorTerrain::separate(p.position,radius,world,birthQuery?&*birthQuery:nullptr);
        }
        if(placed){p.position.x+=e.shapeOrigins[basis*3];p.position.y+=e.shapeOrigins[basis*3+1];p.z+=e.shapeOrigins[basis*3+2];}
        p.history.reset(p.position);
    }
}
bool DecorEffects::hit(unsigned owner,const NailHit& hit,Vec2 source,const World& world,bool plant,
                       const std::array<float,2>* debrisZ)
{
    if(!ready_)return false;
    unsigned index=0;for(;index<profileCount_&&profiles_[index].owner!=owner;++index){}
    observeSimulationWork(SimulationWork::DecorTests,std::min(index+1,profileCount_));
    if(index==profileCount_||broken_[index])return false;
    broken_[index]=true;
    return hitProfile(profiles_[index],index,hit,source,world,plant,debrisZ);
}
void DecorEffects::clearRoomEffects()
{
    reset();
    for(unsigned i=roomEmitterBase_;i<emitterCount_;++i){ownerShapes_[i]=0;ownerVisual_[i]=false;ownerSorting_[i]={};}
    emitterCount_=roomEmitterBase_;roomProfiles_.clear();roomFlings_.clear();roomForces_.clear();roomEmissions_.clear();roomDraws_.clear();
    roomEffectsBound_=false;
}
bool DecorEffects::loadRoomEffects(const char* path,unsigned sprites)
{
    if(!ready_||!roomEmitterBase_)return false;
    auto* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4]{};unsigned header[2]{};
    const auto format=[&](const char* version){return !std::memcmp(magic,version,4);};
    bool ok=std::fread(magic,1,4,file)==4&&(format("RBF2")||format("RBF3")||format("RBF4")||format("RBF5")||format("RBF6"))&&
        std::fread(header,4,2,file)==2&&header[0]<=roomEmitterCapacity&&header[1]<=127&&
        roomEmitterBase_+header[0]<=emitters_.size();
    if(!ok){std::fclose(file);return false;}
    const bool materials=format("RBF6"),spells=materials||format("RBF5"),flings=spells||format("RBF4");
    std::vector<RoomFling> fling(header[0]);
    std::vector<RoomForce> forces(header[0]);
    std::vector<DecorEmitter> next(header[0]);std::vector<unsigned> shapes(header[0]);
    std::vector<std::array<float,2>> sorting(header[0]);std::vector<bool> visual(header[0]);unsigned bound=0;
    for(unsigned i=0;ok&&i<header[0];++i){
        auto& e=next[i];unsigned row[4]{};
        ok=std::fread(row,4,4,file)==4&&std::fread(e.v.data(),4,e.v.size(),file)==e.v.size()&&
            std::fread(&e.rate,4,1,file)==1&&std::fread(e.cone.data(),4,e.cone.size(),file)==e.cone.size()&&
            std::fread(e.bases.data(),4,e.bases.size(),file)==e.bases.size()&&
            std::fread(e.axes.data(),4,e.axes.size(),file)==e.axes.size()&&
            std::fread(e.shapeOrigins.data(),4,e.shapeOrigins.size(),file)==e.shapeOrigins.size()&&
            std::fread(sorting[i].data(),4,2,file)==2;
        auto& burst=fling[i];
        if(flings)ok=ok&&std::fread(&burst.minimum,4,1,file)==1&&std::fread(burst.v.data(),4,5,file)==5;
        if(materials){
            auto& force=forces[i];
            ok=ok&&std::fread(&force.active,4,1,file)==1&&force.active<=1&&
                std::fread(force.curves.data(),4,force.curves.size(),file)==force.curves.size();
            for(float value:force.curves)ok=ok&&std::isfinite(value)&&std::abs(value)<=1e6f;
        }
        for(float value:burst.v)ok=ok&&std::isfinite(value)&&value>=0&&value<=1e6f;
        const bool spatter=flings&&(row[3]&255u)==13;
        const bool spell=spells&&(row[3]&255u)==14;
        ok=ok&&(spatter?(burst.minimum<=row[2]&&row[2]<=256&&burst.v[2]>0&&burst.v[2]<=240&&
            burst.v[3]>0&&burst.v[4]>=burst.v[3]&&row[1]==7):burst.minimum==0);
        e.sprite=row[0];e.tiles=row[1];e.count=row[2];shapes[i]=row[3]&255u;visual[i]=(row[3]&256u)!=0;
        const unsigned material=row[3]>>9;
        e.material=RoomMaterial(material);
        ok=ok&&e.tiles>0&&e.sprite<sprites&&e.tiles<=sprites-e.sprite&&e.count<=capacity-bound&&
            (materials?(material==0||material==1||material==4):!(row[3]&~511u))&&
            (shapes[i]==5||shapes[i]==10||shapes[i]==12||spatter||spell)&&
            (!spell||(e.tiles==1&&e.count==1&&visual[i]&&e.rate==0&&e.v[7]<=60&&e.v[9]<=e.v[10]));
        for(float v:e.v)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.cone)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.bases)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.axes)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:e.shapeOrigins)ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        for(float v:sorting[i])ok=ok&&std::isfinite(v)&&std::abs(v)<=1e6f;
        ok=ok&&std::isfinite(e.rate)&&e.rate>=0&&e.rate<=1e6f&&e.v[6]>=0&&e.v[7]>0&&e.v[8]>=e.v[7]&&
            e.v[11]>0&&e.v[12]>=e.v[11]&&e.v[18]>=0&&e.v[18]<=1&&e.v[19]>=0&&
            e.v[58]>=0&&e.v[58]<=1&&e.v[59]>=0&&e.cone[0]>=0;
        if(ok)bound+=e.count;
    }
    const bool weighted=flings||!std::memcmp(magic,"RBF3",4);
    std::vector<RoomEffectProfile> profiles(header[1]);std::vector<unsigned> owners(header[0],~0u);
    unsigned choices=0,references=0;
    for(unsigned i=0;ok&&i<header[1];++i){
        unsigned row[4]{};const unsigned fields=spells?4:weighted?3:2;
        ok=std::fread(row,4,fields,file)==fields&&row[0]&&row[1]<=header[0]&&row[2]<=1024-choices;
        auto& profile=profiles[i];profile.owner=row[0];
        for(unsigned j=0;j<i;++j)ok=ok&&profiles[j].owner!=profile.owner;
        if(!ok)break;
        const auto readIds=[&](std::vector<unsigned>& ids,unsigned count,bool spellIds=false){
            if(count>header[0]||count>1024-references){ok=false;return;}
            references+=count;ids.resize(count);
            for(unsigned j=0;ok&&j<count;++j){
                unsigned local=0;ok=std::fread(&local,4,1,file)==1&&local<header[0];
                if(!ok)break;
                const unsigned id=roomEmitterBase_+local;
                ok=((shapes[local]==14)==spellIds)&&(owners[local]==~0u||owners[local]==i)&&std::find(ids.begin(),ids.begin()+j,id)==ids.begin()+j;
                if(ok){owners[local]=i;ids[j]=id;}
            }
        };
        readIds(profile.emitters,row[1]);choices+=row[2];profile.choices.resize(row[2]);
        float total=0;
        for(auto& choice:profile.choices){
            unsigned count=0;
            ok=ok&&std::fread(&choice.weight,4,1,file)==1&&std::fread(&count,4,1,file)==1&&
                std::isfinite(choice.weight)&&choice.weight>=0;
            if(!ok)break;
            total+=choice.weight;readIds(choice.emitters,count);
        }
        ok=ok&&std::isfinite(total)&&(profile.choices.empty()||total>0);
        if(spells){
            readIds(profile.spells,row[3],true);
        }
    }
    ok=ok&&std::all_of(owners.begin(),owners.end(),[](unsigned v){return v!=~0u;})&&std::fgetc(file)==EOF;
    std::fclose(file);if(!ok)return false;
    clearRoomEffects();
    for(unsigned i=0;i<header[0];++i){
        const unsigned id=roomEmitterBase_+i;emitters_[id]=next[i];
        ownerShapes_[id]=shapes[i];ownerVisual_[id]=visual[i];ownerSorting_[id]=sorting[i];
    }
    emitterCount_=roomEmitterBase_+header[0];roomProfiles_=std::move(profiles);roomFlings_=std::move(fling);roomForces_=std::move(forces);
    roomEmissions_.resize(header[0]);roomDraws_.reserve(bound);roomEffectsBound_=true;
    return true;
}
bool DecorEffects::roomHit(const RoomBreakableConfig& config,unsigned slot,const NailHit& hit,Vec2 source,const World& world)
{
    if(!ready_||!profileCount_||slot>=127||roomBroken_[slot])return false;
    // Legacy packages without a room bank retain the Tutorial debris approximation.
    roomBroken_[slot]=true;
    auto profile=profiles_[0];const Vec2 offset{config.origin.x-profile.v[0],config.origin.y-profile.v[1]};
    profile.v={config.origin.x,config.origin.y,config.depth,config.effect.x,config.effect.y,config.angle};
    const auto room=std::find_if(roomProfiles_.begin(),roomProfiles_.end(),[&](const auto& p){return p.owner==config.owner;});
    if(roomEffectsBound_){
        profile.emitters.fill(emitterCount_);
        const auto schedule=[&](unsigned id){
            const auto& e=emitters_[id];
            if(ownerShapes_[id]==13){
                const auto& burst=roomFlings_[id-roomEmitterBase_];
                const unsigned amount=std::min(e.count,burst.minimum+unsigned(random(0,float(e.count-burst.minimum+1))));
                const unsigned first=count_;
                emit(world,id,config.origin,0,-1,false,amount);
                for(unsigned j=first;j<count_;++j){auto& p=particles_[j];
                    p.position.x+=random(-burst.v[0],burst.v[0]);p.position.y+=random(-burst.v[1],burst.v[1]);
                    p.z=config.depth;p.sprite=e.sprite;p.history.reset(p.position);
                }
                return;
            }
            if(!e.count||e.rate<=0||e.v[6]<=0)return;
            auto free=std::find_if(roomEmissions_.begin(),roomEmissions_.end(),[](const auto& e){return !e.seed;});
            if(free==roomEmissions_.end())return;
            *free={};free->emitter=id;free->origin={e.v[0],e.v[1]};free->z=e.v[2];
            free->seed=seed_?seed_:1;random(0,1);free->owned=true;
            free->basis=hit.direction==Strike::Left?1:hit.direction==Strike::Right?2:0;
            ++activeEmissions_;
        };
        if(room!=roomProfiles_.end()){
            if(hit.type==AttackType::Spell)for(unsigned id:room->spells){
                if(count_>=capacity)break;
                const auto& e=emitters_[id];auto& p=particles_[count_++];p={};
                p.flash=5;p.emitter=id;p.sprite=e.sprite;p.life=e.v[7];p.freshFlash=true;
                p.position=config.origin;p.z=e.v[2];p.rotation=random(e.v[9],e.v[10]);
                p.history.reset(p.position);
            }
            for(unsigned id:room->emitters)schedule(id);
            if(!room->choices.empty()){
                float total=0;for(const auto& choice:room->choices)total+=choice.weight;
                const float roll=random(0,total);float cumulative=0;
                const RoomEffectChoice* selected=nullptr;
                for(const auto& choice:room->choices){
                    if(choice.weight<=0)continue;
                    selected=&choice;cumulative+=choice.weight;if(roll<cumulative)break;
                }
                if(selected)for(unsigned id:selected->emitters)
                    if(std::find(room->emitters.begin(),room->emitters.end(),id)==room->emitters.end())schedule(id);
            }
        }
    }else if(!config.debris)profile.emitters.fill(emitterCount_);
    return hitProfile(profile,profileCapacity+slot,hit,source,world,false,nullptr,offset,
                      config.audioAuthored?(config.effects&~16u):config.effects,config.effectZ,config.impulse);
}
bool DecorEffects::hitProfile(const DecorEffectProfile& profile,unsigned index,const NailHit& hit,Vec2 source,
                              const World& world,bool plant,const std::array<float,2>* debrisZ,Vec2 placementOffset,unsigned effects,
                              float effectZ,bool objectEffects)
{
    // Both source components use their spell prefab in place of nail flashes.
    // Room-specific spell artwork is emitted by roomHit from the room bank.
    if(hit.type==AttackType::Spell)effects&=~12u;
    const auto& v=profile.v;
    const Vec2 origin{v[0],v[1]};
    // Retained as a diagnostic only; contacts query the live room contours.
    auto queryWorld=world;queryWorld.queriesHitTriggers=false;
    float groundDistance=0;
    const float floor=rayTerrain(origin,{0,-1},1000,queryWorld,groundDistance)?origin.y-groundDistance:-1e6f;
    const float pi=3.14159265358979323846f;
    const float angle=hit.direction==Strike::Left?pi:hit.direction==Strike::Up?pi*.5f:hit.direction==Strike::Down?-pi*.5f:0;
    const float offset=(hit.direction==Strike::Left?-v[5]:hit.direction==Strike::Right?v[5]:0)*pi/180;
    const unsigned basis=hit.direction==Strike::Left?1:hit.direction==Strike::Right?2:0;
    for(unsigned part=0;part<profile.emitters.size();++part){
        const unsigned e=profile.emitters[part],first=count_;
        if(e>=emitterCount_)continue;
        emit(world,e,origin,offset,floor,true,emitters_[e].count,basis,placementOffset);
        for(unsigned i=first;i<count_;++i)particles_[i].freshDebris=!plant;
        // SetZRandom.OnEnable assigns world position.z before particle birth.
        if(debrisZ)for(unsigned i=first;i<count_;++i)particles_[i].z=(*debrisZ)[part];
    }
    const unsigned dust=dust_[hit.direction==Strike::Down?1:0];
    const auto& emitter=emitters_[dust];
    auto& emission=emissions_[index];
    emission={{origin.x+v[3],origin.y+v[4]},angle,floor,0,dust,0,seed_};
    // Shared scenery keeps the prefab depth; room actors supply their world depth.
    emission.z=index>=profileCapacity?v[2]+effectZ:0;
    // Reserve twelve random draws per particle. Delayed emission
    // replays that span without changing debris, flashes, pitch or later hits.
    const bool hasDust=(effects&(hit.direction==Strike::Down?2u:1u))!=0;
    if(hasDust)for(unsigned i=0;i<emitter.count*12;++i)random(0,1);
    if(hasDust&&emitter.count&&emitter.rate>0&&emitter.v[6]>0)++activeEmissions_;
    else emission.seed=0;
    for(unsigned i=0;i<2&&count_<capacity;++i){
        if(!(effects&(4u<<i)))continue;
        auto& p=particles_[count_++];p={};p.flash=i+1;p.life=float(flashes_[i].count)/flashes_[i].fps;
        p.age=.001f;p.freshFlash=true; // observed tk2d Play phase, seconds
        p.position=i?Vec2{(origin.x+source.x)*.5f,(origin.y+source.y)*.5f}:origin;
        p.z=i?v[2]*.5f:v[2];
        p.rotation=i&&!objectEffects?angle+random(-20,20)*pi/180:0;
        if(objectEffects)p.scaleX=source.x>origin.x?-1:1;
        p.history.reset(p.position);
    }
    visualActive_=true;
    if(effects&16)++sounds_;
    // Plant Hit uses rope_cut at volume 1, Random.Range(.9,1.1), from the
    // verified plant-hit-dependencies export; decor keeps its authored clip.
    if(plant){++plantSounds_;plantPitch_=random(.9f,1.1f);}
    else pitch_=random(audioPitch_[0],audioPitch_[1]);
    shake_=.12f;
    return true;
}
void DecorEffects::nailStrike(Vec2 origin,float depth)
{
    if(count_>=capacity)return;
    visualActive_=true;
    auto& p=particles_[count_++];p={};p.flash=2;p.freshFlash=true;
    p.life=float(flashes_[1].count)/flashes_[1].fps;p.age=.001f;
    p.position=origin;p.z=depth;p.history.reset(origin);
    // Strike Nail R's Effect state replaces caller scale/rotation. The Sword
    // artwork already includes 1.5x; undo that bake before applying SetScale.
    p.size=random(nailTransform_[0],nailTransform_[1])/1.5f;
    p.rotation=std::floor(random(nailTransform_[2],nailTransform_[3]))*(3.14159265358979323846f/180);
}
void DecorEffects::healthCocoonBreak(Vec2 origin,Strike direction)
{
    if(!ready_)return;
    // HealthCocoon.OnTriggerEnter2D spawns Slash Impact R and Strike Nail R
    // at effectOrigin=(0,.8), then overwrites both clone transforms with 1.5x.
    constexpr float pi=3.14159265358979323846f;
    const float low=direction==Strike::Left?70:direction==Strike::Down?250:340;
    const float angle=random(low,low+40)*pi/180;
    for(unsigned i=0;i<2&&count_<capacity;++i){
        auto& p=particles_[count_++];p={};p.flash=i+1;
        p.life=float(flashes_[i].count)/flashes_[i].fps;p.age=.001f;p.freshFlash=true;
        p.position={origin.x,origin.y+.8f};p.history.reset(p.position);
        p.rotation=angle;p.scaleX=direction==Strike::Left?-1:1;
        // Slash Impact art bakes scenery's .6x1; Sword art bakes 1.5x1.5.
        if(!i){p.scaleX*=1.5f/.6f;p.scaleY=1.5f;}
    }
    visualActive_=true;
}

void DecorEffects::nailHit(Vec2 origin,float depth)
{
    if(!ready_||!std::isfinite(origin.x)||!std::isfinite(origin.y)||!std::isfinite(depth))return;
    nailStrike(origin,depth);
}
void DecorEffects::enemyNailHit(Vec2 origin,Strike direction,float slashScale)
{
    if(!ready_||!std::isfinite(origin.x)||!std::isfinite(origin.y)||!std::isfinite(slashScale)||slashScale<=0)return;
    nailStrike(origin,0);
    if(count_>=capacity)return;
    // HealthManager.Hit leaves Strike Nail R's activation transform intact,
    // then assigns Slash Impact 1.5x. Health Scuttler uses .9x instead.
    auto& p=particles_[count_++];p={};p.flash=1;p.freshFlash=true;
    p.age=.001f;p.life=float(flashes_[0].count)/flashes_[0].fps;
    p.position=origin;p.history.reset(origin);
    const float low=direction==Strike::Up?70:direction==Strike::Down?250:340;
    p.rotation=random(low,low+40)*(3.14159265358979323846f/180);
    p.scaleX=(direction==Strike::Left?-slashScale:slashScale)/.6f;p.scaleY=slashScale;
}

void DecorEffects::geoRockHit(Vec2 origin,float depth,Strike direction,bool broken)
{
    if(!ready_||!std::isfinite(origin.x)||!std::isfinite(origin.y)||!std::isfinite(depth))return;
    nailStrike(origin,depth);
    constexpr float pi=3.14159265358979323846f;
    // The FSM routes both UP and DOWN to Hit Down.
    const float angle=broken||direction==Strike::Down||direction==Strike::Up?-pi*.5f:
        direction==Strike::Left?pi:0;
    // Hit Left/Right/Down wait Recoil Time (.1s), then spawn regular dust.
    // Destroy bypasses that wait and spawns regular + down dust at Self.z+.1.
    for(unsigned variant=0;variant<(broken?2u:1u);++variant){
        const unsigned index=dust_[variant];const auto& emitter=emitters_[index];
        if(!emitter.count||emitter.rate<=0||emitter.v[6]<=0)continue;
        for(auto& emission:geoEmissions_)if(!emission.seed){
            emission={origin,angle,0,broken?0.0f:-.1f,index,0,seed_,depth+.1f};
            for(unsigned i=0;i<emitter.count*12;++i)random(0,1);
            ++activeEmissions_;break;
        }
    }
    visualActive_=true;
}
void DecorEffects::doorEmission(unsigned kind,Vec2 origin,float depth)
{
    if(!doorReady_||kind>=doorEmitters_.size())return;
    for(auto& emission:doorEmissions_)if(!emission.seed){
        emission={};emission.origin=origin;emission.z=depth;emission.emitter=doorEmitters_[kind];
        if(kind>=4)emission.elapsed=-doorDelays_[kind-4];
        emission.box=kind!=5;seed_=seed_*1664525u+1013904223u;
        emission.seed=seed_?seed_:1;++activeEmissions_;return;
    }
}
void DecorEffects::floorBreak(Vec2 self,const World& world)
{
    if(!ready_||!doorReady_)return;
    // FSM12191 Break: 3 + 8 pooled bodies, variation (2,.5), zero fling speed.
    // Approximation: reuse the original small-rock artwork for the three boulders.
    for(unsigned i=0;i<11&&count_<capacity;++i){
        const Vec2 origin{self.x+random(-2,2),self.y+random(-.5f,.5f)};
        const unsigned first=count_;emit(world,doorEmitters_[0],origin,0,0,false,1);
        if(count_==first)continue;
        auto& p=particles_[first];p.z=.019f+random(0,.009f);p.velocity={};
        p.position=decorTerrain::separate(origin,.5f*emitters_[p.emitter].v[59]*p.size+.01f,world);
        p.history.reset(p.position);
    }
    shake_=.12f;visualActive_=true;
}
void DecorEffects::greatDoorHit(Vec2 self,Vec2 damager,float depth,Strike direction,const World& world)
{
    if(!ready_||!doorReady_||!std::isfinite(self.x)||!std::isfinite(self.y)||
       !std::isfinite(damager.x)||!std::isfinite(damager.y)||!std::isfinite(depth))return;
    shake_=.12f;
    constexpr float pi=3.14159265358979323846f;
    for(unsigned kind=0;kind<2;++kind){
        const unsigned amount=unsigned(random(kind?10:3,kind?13:6));
        for(unsigned n=0;n<amount&&count_<capacity;++n){
            const Vec2 origin=kind?Vec2{self.x+random(-.5f,.5f),self.y+random(-4.5f,4.5f)}:Vec2{damager.x+1,damager.y};
            const unsigned first=count_;emit(world,doorEmitters_[kind],origin,0,0,false,1);
            if(count_==first)continue;
            auto& p=particles_[first];const float angle=random(100,140)*pi/180,speed=random(15,kind?25:20);
            p.position=decorTerrain::separate(origin,.5f*emitters_[p.emitter].v[59]*p.size+.01f,world);
            p.z=.019f+random(0,.009f);if(random(0,1)<1.0f/3)p.z-=.05f;
            p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
            // Approximation: one rigidbody torque is represented by a bounded spin.
            p.spin=-p.velocity.x*pi/180;p.history.reset(p.position);
        }
    }
    doorEmission(4,{self.x-.5f,self.y},depth-1);
    doorEmission(5,{damager.x+1,damager.y},depth);
    if(count_<capacity){
        auto& p=particles_[count_++];p={};p.flash=2;
        p.life=float(flashes_[1].count)/flashes_[1].fps;p.age=.001f;p.freshFlash=true;
        p.position={damager.x+1,damager.y};p.z=depth;
        p.rotation=direction==Strike::Left?pi:direction==Strike::Up?pi*.5f:direction==Strike::Down?-pi*.5f:0;
        p.history.reset(p.position);
    }
    visualActive_=true;
}
void DecorEffects::greatDoorBreak(Vec2 self,float depth,GreatDoorBreakStage stage,const World& world)
{
    if(!ready_||!doorReady_||!std::isfinite(self.x)||!std::isfinite(self.y)||!std::isfinite(depth))return;
    shake_=.12f;
    if(stage==GreatDoorBreakStage::Final)return;
    doorEmission(6,{185,75},.1f);
    constexpr float pi=3.14159265358979323846f;
    if(stage==GreatDoorBreakStage::Break1){
        for(unsigned n=0;n<12&&count_<capacity;++n){
            const Vec2 origin{self.x+random(-.5f,.5f),self.y+random(-4.5f,4.5f)};
            const unsigned first=count_;emit(world,doorEmitters_[1],origin,0,0,false,1);
            if(count_==first)continue;
            auto& p=particles_[first];const float angle=random(100,140)*pi/180,speed=random(25,30);
            p.position=decorTerrain::separate(origin,.5f*emitters_[p.emitter].v[59]*p.size+.01f,world);
            p.z=.019f+random(0,.009f);if(random(0,1)<1.0f/3)p.z-=.05f;
            p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};p.spin=-p.velocity.x*pi/180;p.history.reset(p.position);
        }
    }else{
        for(unsigned part=0;part<2&&count_<capacity;++part){
            const auto& body=doorBodies_[part];
            const unsigned first=count_;const Vec2 origin{self.x+body[0],self.y+body[1]};
            emit(world,doorEmitters_[2+part],origin,0,0,false,1);
            if(count_==first)continue;
            auto& p=particles_[first];p.z=depth+body[2];
            p.velocity={body[3]*std::cos(body[4]),body[3]*std::sin(body[4])};
            p.rotation=std::floor(random(0,360))*pi/180;
            p.freshFlash=true; // Rigidbody age starts after birth; ParticleSystem ages include the birth render.
            p.position=decorBody::separate(p.position,p.rotation,doorHulls_[part],body[14],world);
            p.history.reset(p.position);
        }
    }
    visualActive_=true;
}
void DecorEffects::persistentWall(Vec2 self,float depth,unsigned facing,bool broken,const World& world)
{
    if(!ready_||!doorReady_)return;

    if(broken)shake_=.12f; // approximation: existing bounded shake for source AverageShake.
    constexpr float pi=3.14159265358979323846f;
    // Original rock sprites and rigidbody profiles. Approximation: share the
    // Great Door rock lifetime and dust profiles until wall emitters are exported.
    const unsigned amount=broken?18u:unsigned(random(8,11));
    for(unsigned n=0;n<amount&&count_<capacity;++n){
        const auto kind=broken&&n%2?1u:0u;
        const unsigned first=count_;emit(world,doorEmitters_[kind],self,0,0,false,1);
        if(count_==first)continue;
        auto& p=particles_[first];
        float angle=random(facing==0||facing==2?40:70,facing==0||facing==2?80:110)*pi/180;
        const float speed=random(15,20);
        const float x=std::cos(angle)*speed*(facing==2?-1:1);
        const float y=std::sin(angle)*speed*(facing==3?-1:1);
        p.position=decorTerrain::separate(self,.5f*emitters_[p.emitter].v[59]*p.size+.01f,world);
        p.z=depth-.02f;p.velocity={x,y};p.spin=-x*pi/180;p.history.reset(p.position);
    }
    if(broken){doorEmission(4,self,depth-.1f);doorEmission(5,self,depth-.1f);}
    visualActive_=true;
}
void DecorEffects::grassCut(unsigned owner,Vec2 impactOrigin,float depth,Strike direction,const World& world)
{
    if(!ready_||grassEmitter_>=emitterCount_||!std::isfinite(impactOrigin.x)||!std::isfinite(impactOrigin.y)||!std::isfinite(depth))return;
    unsigned profile=0;for(;profile<grassProfileCount_&&grassProfiles_[profile].owner!=owner;++profile){}
    if(profile==grassProfileCount_)return;

    const auto& grass=grassProfiles_[profile];
    const unsigned first=count_;
    emit(world,grassEmitter_,grass.origin,0,0,false,grass.count);
    for(unsigned i=first;i<count_;++i)particles_[i].z=grass.z;
    // Source starts Slash Impact before mutating the prefab scale, so clone
    // scale is uncertain. Relative to the converter's 1.5x art bake, .4x/.667y
    // approximates the authored .6x/1y assignment.
    if(count_<capacity){
        auto& p=particles_[count_++];p={};p.flash=1;p.life=float(flashes_[0].count)/flashes_[0].fps;p.age=.001f;p.freshFlash=true;
        p.position=impactOrigin;p.z=depth;p.history.reset(impactOrigin);
        const float side=grass.origin.x-impactOrigin.x;
        // Mathf.Sign(0) is +1: centered hits still produce a visible flash.
        p.scaleX=side>=0?-.4f:.4f;p.scaleY=2.0f/3;
    }
    visualActive_=true;
    (void)direction;
}
void DecorEffects::fixedUpdate(float seconds,const World& world)
{
    if(!std::isfinite(seconds)||seconds<=0)return;
    shake_=std::max(0.0f,shake_-seconds);
    if(!doorReady_)return;
    for(unsigned i=0;i<count_;++i){
        auto& p=particles_[i];
        const int part=p.emitter==doorEmitters_[2]?0:p.emitter==doorEmitters_[3]?1:-1;
        if(part<0||p.flash)continue;
        const auto& body=doorBodies_[unsigned(part)];
        PresentationPosition::Step history(p.history,p.position);
        // offscreen is unused by persistent shards; it stores elapsed fixed time.
        const float previous=p.offscreen;p.offscreen+=seconds;
        if(previous+1e-6f<2*body[15]&&p.offscreen+1e-6f>=2*body[15])p.spin+=p.velocity.x*body[5]*body[15]/body[9];
        decorBody::step(p.position,p.velocity,p.rotation,p.spin,p.sleep,p.floor,seconds,
                       body,doorHulls_[unsigned(part)],-9.81f*p.gravity,world,
                       [&](float lo,float hi){return random(lo,hi);});
    }
}
void DecorEffects::visualUpdate(float seconds,const World& world)
{
    if(!visualActive_&&!activeEmissions_)return;
    update(seconds,true,world);
    if(std::isfinite(seconds)&&seconds>0){
        visualActive_=count_!=0;
        for(unsigned i=0;i<count_;++i){
            particles_[i].freshFlash=false;
        }
    }
}
void DecorEffects::update(float seconds,bool visual,const World& world)
{
    if(!ready_||!std::isfinite(seconds)||seconds<=0||(count_==0&&shake_==0&&activeEmissions_==0))return;
    // Equal variable substeps match the measured scheduler set. Fixed Unity
    // Simulate accumulation and arbitrary dt are not claimed to be exact.
    if(seconds>.03f){
        const unsigned steps=unsigned(std::ceil(seconds/.03f));
        for(unsigned i=0;i<steps;++i)update(seconds/steps,visual,world);
        return;
    }
    const auto advanceEmission=[&](DustEmission& emission){
        if(!emission.seed)return;
        const auto& e=emitters_[emission.emitter];
        const float rate=emission.rate>0?emission.rate:e.rate;
        const unsigned cap=emission.cap?emission.cap:e.count;
        const float before=emission.elapsed;
        emission.elapsed=std::min(e.v[6],before+seconds);
        // Measured dust births are spaced at 1/rate from Play, including the
        // fractional age and motion remaining in this step (not a new burst).
        // The cap bounds total births, without refilling deaths (notably down dust).
        // 1e-5 particles only absorbs floating-point rounding at rate boundaries.
        const unsigned due=unsigned(std::clamp(std::floor(double(rate)*emission.elapsed+1e-5),0.0,double(cap)));
        const unsigned savedSeed=seed_;seed_=emission.seed;
        for(unsigned birth=emission.emitted;birth<due;++birth){
            const unsigned first=count_;
            emit(world,emission.emitter,emission.origin,emission.angle,emission.floor,emission.owned,1,emission.basis);
            if(count_>first){auto& p=particles_[first];p.age=before-float(birth+1)/rate;p.z=emission.z+(emission.owned?e.shapeOrigins[emission.basis*3+2]:0);
                if(emission.box){
                    const float x=random(-.5f,.5f),y=random(-.5f,.5f),z=random(-.5f,.5f);
                    p.position={emission.origin.x+(e.v[0]+x*e.bases[0]+y*e.bases[2]+z*e.bases[4])*emission.scale.x,
                                emission.origin.y+(e.v[1]+x*e.bases[1]+y*e.bases[3]+z*e.bases[5])*emission.scale.y};
                    if(!doorReady_||(emission.emitter!=doorEmitters_[4]&&emission.emitter!=doorEmitters_[6]))
                        p.gravity=random(e.axes[6],e.axes[7]);
                }
                p.history.reset(p.position);}
        }
        emission.seed=seed_;seed_=savedSeed;emission.emitted=due;
        if(due==cap||emission.elapsed>=e.v[6]){emission.seed=0;--activeEmissions_;}
    };
    if(visual){
        observeSimulationWork(SimulationWork::DecorEmissionSlots,emissions_.size()+geoEmissions_.size()+
            platformEmissions_.size()+ownerEmissions_.size()+doorEmissions_.size()+roomEmissions_.size());
        for(auto& emission:emissions_)advanceEmission(emission);
        for(auto& emission:geoEmissions_)advanceEmission(emission);
        for(auto& emission:platformEmissions_)advanceEmission(emission);
        for(auto& emission:ownerEmissions_)advanceEmission(emission);
        for(auto& emission:doorEmissions_)advanceEmission(emission);
        for(auto& emission:roomEmissions_)advanceEmission(emission);
    }
    std::array<float,emitterCapacity> retention{};std::array<bool,emitterCapacity> computed{};
    std::array<unsigned,emitterCapacity> birthIndex{};
    for(unsigned i=0;i<count_;){
        observeSimulationWork(SimulationWork::DecorParticles);
        auto& p=particles_[i];
        if(p.freshFlash){++i;continue;}
        if(p.freshDebris){
            const auto& e=emitters_[p.emitter];
            // Native rate-over-time at a full pool retains the newest births:
            // offset0 ages are dt-floor(dt*rate)/rate + i/rate (5000/s).
            // Keep the already reserved RNG/particle order and simulate only
            // that fractional first-frame age, then the full render clock.
            if(e.rate>0&&seconds*e.rate>=e.count){
                const float remainder=seconds-std::floor(seconds*e.rate)/e.rate;
                p.age=-seconds+remainder+float(birthIndex[p.emitter]++)/e.rate;
            }
            p.freshDebris=false;
        }
        const float step=std::clamp(seconds+p.age,0.0f,seconds);
        p.age=std::max(0.0f,p.age+seconds);
        if(p.age>=p.life){p=particles_[--count_];continue;}
        if(doorReady_&&!p.flash&&(p.emitter==doorEmitters_[2]||p.emitter==doorEmitters_[3])){++i;continue;}
        if(!p.flash&&ownerShapes_[p.emitter]==13&&p.floor>=0){
            const auto& burst=roomFlings_[p.emitter-roomEmitterBase_];
            p.sprite=emitters_[p.emitter].sprite+std::min(emitters_[p.emitter].tiles-1,unsigned((p.age-p.floor)*burst.v[2]));
        }
        {
            PresentationPosition::Step presentationStep(p.history,p.position);
            if(!p.flash&&step>0&&!(ownerShapes_[p.emitter]==13&&p.floor>=0)){
                const auto& v=emitters_[p.emitter].v;
                if(!computed[p.emitter]){retention[p.emitter]=decorRetention(v[18],seconds);computed[p.emitter]=true;}
                const Vec2 before=p.position;
                // CircleCollider2D follows the largest axis of the flight squash.
                const float collisionScale=ownerShapes_[p.emitter]==13?std::max(std::abs(p.scaleX),std::abs(p.scaleY)):1;
                const float radius=.5f*v[59]*p.size*collisionScale*fxSample(v,22,(p.age-step)/p.life);
                std::array<float,3> position{p.position.x,p.position.y,p.z};
                std::array<float,3> base{p.velocity.x,p.velocity.y,p.velocityZ};
                // SpatterOrange is a Rigidbody2D; ParticleSystems use Physics.gravity.
                const float gravity=ownerShapes_[p.emitter]==13?-60.f:-9.81f;
                std::array<float,3> force{p.forceX,p.force,p.forceZ};
                if(p.emitter>=roomEmitterBase_&&roomEffectsBound_){
                    const auto& source=roomForces_[p.emitter-roomEmitterBase_];
                    if(source.active){
                        const float coordinate=std::clamp((p.age-step)/p.life,0.f,1.f)*7;
                        const unsigned k=std::min(6u,unsigned(coordinate));const float u=coordinate-k;
                        for(unsigned axis=0;axis<3;++axis){
                            const unsigned lo=axis*16+k,hi=lo+8;
                            const float a=source.curves[lo]+u*(source.curves[lo+1]-source.curves[lo]);
                            const float b=source.curves[hi]+u*(source.curves[hi+1]-source.curves[hi]);
                            force[axis]=a+force[axis]*(b-a);
                        }
                    }
                }
                const auto total=decorMotionStep(position,base,{p.moduleVelocity.x,p.moduleVelocity.y,p.moduleZ},
                    {force[0]*step,force[1]*step,force[2]*step},{0,(gravity*p.gravity)*step,0},step,v[19],step==seconds?retention[p.emitter]:decorRetention(v[18],step));
                p.position={position[0],position[1]};p.z=position[2];
                p.velocity={base[0],base[1]};p.velocityZ=base[2];
                Vec2 contactPosition=before,contactVelocity{total[0],total[1]};
                const bool contact=v[59]>0&&decorTerrain::move(contactPosition,p.position,contactVelocity,radius+.01f,
                    v[20],1-v[58],world);
                if(v[59]>0)p.position=contactPosition;
                if(contact){
                    p.velocity={contactVelocity.x-p.moduleVelocity.x,contactVelocity.y-p.moduleVelocity.y};
                    p.velocityZ=total[2]*(1-v[58])-p.moduleZ;
                    // Z displacement uses post-contact speed, as in measured contacts.
                    p.z=position[2]+(p.velocityZ+p.moduleZ-total[2])*step;
                    const float x=p.velocity.x+p.moduleVelocity.x,y=p.velocity.y+p.moduleVelocity.y,z=p.velocityZ+p.moduleZ;
                    if(std::sqrt(x*x+y*y+z*z)<v[21])p.age=p.life;
                }
                if(ownerShapes_[p.emitter]==13){
                    const auto& burst=roomFlings_[p.emitter-roomEmitterBase_];
                    if(p.floor<0){
                        const float speed=std::hypot(p.velocity.x,p.velocity.y);
                        p.rotation=std::atan2(p.velocity.y,p.velocity.x);
                        p.scaleX=std::max(.6f,1-speed*.014f);p.scaleY=std::min(1.75f,1+speed*.014f);
                        if(contact||p.age>3){
                            // Approximation: contact orientation uses the solver response,
                            // rather than Unity's normal-specific Euler branches.
                            if(contact)p.rotation=std::atan2(total[1]-contactVelocity.y,total[0]-contactVelocity.x);
                            p.floor=p.age;p.life=p.age+float(emitters_[p.emitter].tiles)/burst.v[2];
                            p.size=random(burst.v[3],burst.v[4]);p.scaleX=p.scaleY=1;
                            p.velocity={};p.velocityZ=0;p.gravity=0;
                        }
                    }
                }
                if(doorReady_&&(p.emitter==doorEmitters_[0]||p.emitter==doorEmitters_[1]||
                               p.emitter==doorEmitters_[2]||p.emitter==doorEmitters_[3])){
                    const bool small=p.emitter==doorEmitters_[0]||p.emitter==doorEmitters_[1];
                    // Approximation: room contacts and low speed stand in for Rigidbody sleep.
                    // No owner-height floor: suspension requires real terrain underneath.
                    Vec2 supportPosition=p.position,supportVelocity{0,-1};
                    const bool quiet=contact&&std::hypot(p.velocity.x,p.velocity.y)<1;
                    const bool asleep=quiet&&decorTerrain::move(supportPosition,{p.position.x,p.position.y-.1f},
                        supportVelocity,radius+.01f,0,1,world);
                    if(asleep){p.velocity={};p.spin=0;if(small)p.sleep+=step;}
                    if(small&&p.sleep>=8){
                        p.scaleX=p.scaleY=std::max(0.0f,9-p.sleep);
                        if(p.sleep>=9)p.age=p.life;
                    }
                    if(small&&viewAspect_>0){
                        const auto& sprite=visuals.atlas.sprites[p.sprite];
                        const float radius=std::hypot(std::max(std::abs(sprite.left),std::abs(sprite.right)),
                            std::max(std::abs(sprite.bottom),std::abs(sprite.top)))*p.size;
                        const float halfHeight=120*(38.1f+p.z)/roomFocalLength(240*viewAspect_,240);
                        const bool visible=std::abs(p.position.x-viewCamera_.x)<=halfHeight*viewAspect_+radius&&
                                           std::abs(p.position.y-viewCamera_.y)<=halfHeight+radius;
                        p.offscreen=visible?0:p.offscreen+seconds;
                        // Approximation: ten 60 Hz render periods stand in for ten invisible Updates.
                        if(p.offscreen>=10.0f/60)p.age=p.life;
                    }
                    p.rotation+=p.spin*step;
                    if(small)p.spin/=1+.05f*step;
                }
                p.rotation+=fxSample(v,38,p.age/p.life)*step;
            }
        }
        ++i;
    }

}
void DecorEffects::present(PresentationTime,unsigned layers)
{
    PerformanceScope timing(PerformanceSection::DecorPresent);
    visuals.draws.clear();roomDraws_.clear();
    for(unsigned i=0;i<count_;++i){
        // Unity ParticleSystems advance in their birth render frame, including
        // debris. They must not inherit the hero's delayed fixed-step phase.
        const auto& p=particles_[i];const auto pose=p.renderPosition(PresentationTime{1});
        if(!(layers&(p.flash?4u:visualParticle(p)?2u:1u)))continue;
        RoomDraw draw;float size=p.size,alpha=1;std::array<float,4> color{1,1,1,1};
        if(p.flash==5){draw.sprite=p.sprite;color={emitters_[p.emitter].v[46],emitters_[p.emitter].v[47],emitters_[p.emitter].v[48],emitters_[p.emitter].v[49]};}
        else if(p.flash){const auto& f=p.flash==4?initialLines_:p.flash==3?stalactiteThunk_:flashes_[p.flash-1];draw.sprite=f.sprite+std::min(f.count-1,unsigned(p.age*f.fps));}
        else{
            const auto& e=emitters_[p.emitter];const auto& v=e.v;
            draw.sprite=p.emitter==grassEmitter_&&grassAnimationFps_>0?e.sprite+std::min(e.tiles-1,unsigned(p.age*grassAnimationFps_)):p.sprite;
            if(initialFallReady_&&p.emitter==initialEmitters_[2])
                draw.sprite=e.sprite+std::min(e.tiles-1,unsigned(std::fmod(p.age/p.life*flockCycles_,1.0f)*e.tiles));
            if(p.emitter==platformEmitters_[1])draw.sprite=e.sprite+std::min(e.tiles-1,unsigned(p.age/p.life*e.tiles));
            size*=fxSample(v,22,p.age/p.life);alpha=fxSample(v,30,p.age/p.life);
            if(doorReady_&&(p.emitter==doorEmitters_[4]||p.emitter==doorEmitters_[5])){
                const auto& keys=doorAlpha_[p.emitter==doorEmitters_[5]?1:0];
                const float t=p.age/p.life;
                auto sample=[&](unsigned base){
                    for(unsigned k=1;k<4;++k)if(t<=keys[base+2*k]){
                        const unsigned a=base+2*(k-1),b=a+2;
                        const float u=std::clamp((t-keys[a])/(keys[b]-keys[a]),0.0f,1.0f);
                        return keys[a+1]+u*(keys[b+1]-keys[a+1]);
                    }
                    return keys[base+7];
                };
                const float low=sample(0);alpha=low+p.spin*(sample(8)-low);
                const auto& sizes=doorSize_[p.emitter==doorEmitters_[5]?1:0];
                const float coordinate=std::clamp(t,0.f,1.f)*7;
                const unsigned a=std::min(6u,unsigned(coordinate));const float u=coordinate-a;
                const float small=sizes[a]+u*(sizes[a+1]-sizes[a]);
                const float large=sizes[8+a]+u*(sizes[9+a]-sizes[8+a]);
                size=p.size*(small+p.floor*(large-small));
            }
            for(unsigned c=0;c<4;++c)color[c]=std::clamp(v[46+c],0.0f,1.0f);
            if(doorReady_&&(p.emitter==doorEmitters_[0]||p.emitter==doorEmitters_[1])&&p.z<0)
                color[0]=color[1]=color[2]=0;
        }
        const float c=std::cos(p.rotation)*size,s=std::sin(p.rotation)*size;
        draw.values={c*p.scaleX,-s*p.scaleY,pose.x,s*p.scaleX,c*p.scaleY,pose.y,p.z,color[0],color[1],color[2],color[3]*std::clamp(alpha,0.0f,1.0f),0,0,0,0,0};
        if(p.flash){
            const auto sorting=p.flash==5?ownerSorting_[p.emitter]:p.flash==4?std::array<float,2>{0,0}:p.flash==3?stalactiteThunkSorting_:flashSorting_[p.flash-1];
            draw.values[11]=sorting[0];draw.values[12]=sorting[1];
        }else if(visualParticle(p)){
            draw.values[11]=dustSorting_[0];draw.values[12]=dustSorting_[1];
        }
        if(doorReady_&&!p.flash)for(unsigned kind=4;kind<7;++kind)if(p.emitter==doorEmitters_[kind]){
            draw.values[11]=doorSorting_[kind-4][0];draw.values[12]=doorSorting_[kind-4][1];
        }
        if(!p.flash&&initialFallReady_)for(unsigned j=0;j<3;++j)if(p.emitter==initialEmitters_[j]){
            draw.values[11]=initialSorting_[j][0];draw.values[12]=initialSorting_[j][1];
        }
        if(!p.flash&&ownerShapes_[p.emitter]){
            draw.values[11]=ownerSorting_[p.emitter][0];draw.values[12]=ownerSorting_[p.emitter][1];
            draw.values[15]=float(emitters_[p.emitter].material);
        }
        if((!p.flash||p.flash==5)&&p.emitter>=roomEmitterBase_)roomDraws_.push_back(draw);
        else visuals.draws.push_back(draw);
    }
}
Vec2 DecorEffects::shake() const
{
    // approximation: EnemyKillShake's FSM magnitude/duration are unknown.
    const float amplitude=.1f*(shake_/.12f);
    return {std::sin(shake_*180)*amplitude,std::cos(shake_*137)*amplitude};
}
}
