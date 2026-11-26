#include "room_dynamics.hpp"
#include "render_memory.hpp"
#include <cstdio>
#include <cstring>
#include <limits>

namespace hk3ds {
static_assert(sizeof(RoomDynamics)+sizeof(RoomDynamicsBank)<=roomDynamicsStateBudgetBytes,"room motion budget");
namespace {
constexpr float pi=3.14159265358979323846f;
bool rectValid(Rect r){return std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&
    std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;}
Vec2 rotate(Vec2 p,float a){return {std::cos(a)*p.x-std::sin(a)*p.y,std::sin(a)*p.x+std::cos(a)*p.y};}
}
bool RoomDynamicsBank::load(const char* path)
{
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    RoomDynamicsBank next;char magic[4]{};
    auto read=[&](void* p,std::size_t bytes){return std::fread(p,1,bytes,f)==bytes;};
    bool ok=read(magic,4)&&!std::memcmp(magic,"HKD1",4)&&read(&next.spriteCount,4)&&
        read(&next.drawCount,4)&&read(&next.duration,4)&&next.spriteCount>0&&next.spriteCount<=4096&&
        next.drawCount>0&&next.drawCount<=16384&&std::isfinite(next.duration)&&next.duration>0;
    float previous=-1;
    for(auto& k:next.keys){ok=ok&&read(&k.time,4)&&read(&k.sprite,4)&&std::isfinite(k.time)&&
        k.time>previous&&k.time<next.duration&&k.sprite<next.spriteCount;previous=k.time;}
    ok=ok&&next.keys[0].time==0;
    for(auto& s:next.spikes)ok=ok&&read(&s.owner,4)&&read(&s.authored,4)&&s.owner&&s.authored<next.spriteCount;
    for(auto& r:next.rocks){float v[18]{};ok=ok&&read(&r.owner,4)&&read(v,sizeof(v))&&r.owner;
        for(float value:v)ok=ok&&std::isfinite(value);
        r.spawn={v[0],v[1]};r.collider={v[2],v[3],v[4],v[5]};r.trigger={v[6],v[7],v[8],v[9]};
        r.material={v[10],v[11]};r.mass=v[12];r.inertia=v[13];r.gravity=v[14];r.damping=v[15];r.bounce=v[16];r.threshold=v[17];
        ok=ok&&rectValid(r.collider)&&rectValid(r.trigger)&&r.mass>0&&r.inertia>0&&r.gravity<=0&&
            r.material.friction>=0&&r.material.bounce>=0&&r.damping>=0&&r.bounce>=0&&r.bounce<=1&&r.threshold>=0;
    }
    std::array<unsigned,40> owners{};unsigned n=0;
    for(const auto& s:next.spikes)owners[n++]=s.owner;
    for(const auto& r:next.rocks)owners[n++]=r.owner;
    for(unsigned i=0;i<n;++i)for(unsigned j=0;j<i;++j)ok=ok&&owners[i]!=owners[j];
    if(ok&&std::fgetc(f)!=EOF){
        std::fseek(f,-1,SEEK_CUR);char extension[4]{};
        ok=read(extension,4)&&!std::memcmp(extension,"CRB1",4)&&read(&next.crumbleCount,4)&&
            next.crumbleCount>0&&next.crumbleCount<=next.crumble.size();
        for(unsigned i=0;ok&&i<next.crumbleCount;++i){auto& d=next.crumble[i];
            ok=read(&d.draw,4)&&read(&d.owner,4)&&read(d.values.data(),sizeof(d.values))&&
                d.draw<next.drawCount&&d.owner>0;
            for(float v:d.values)ok=ok&&std::isfinite(v);
            for(unsigned j=0;j<n;++j)ok=ok&&d.owner!=owners[j];
            for(unsigned j=0;j<i;++j)ok=ok&&d.draw!=next.crumble[j].draw;
            if(i)ok=ok&&d.owner>=next.crumble[i-1].owner;
        }
    }
    ok=ok&&std::fgetc(f)==EOF&&!std::ferror(f);std::fclose(f);
    if(ok)*this=next;
    return ok;
}
unsigned RoomDynamicsBank::sprite(float seconds)const
{
    const float t=seconds-std::floor(seconds/duration)*duration;
    unsigned k=0;while(k+1<keys.size()&&keys[k+1].time<=t)++k;
    return keys[k].sprite;
}
void SpikeAnimation::advance(float dt,bool nowVisible,float duration)
{
    if(!std::isfinite(dt)||dt<0||duration<=0)return;
    if(visible){evaluated=true;seconds=std::fmod(seconds+dt,duration);}
    visible=nowVisible;
}
void SimpleRock::reset(const SimpleRockConfig& c,EnemyRandom random)
{
    *this={};position=c.spawn;rotation.x=std::floor(random.range(0,360))*pi/180;
    depthJitter=random.range(0,.0009999f);presentation.reset(position);rotationPresentation.reset(rotation);
}
Rect SimpleRock::collider(const SimpleRockConfig& c)const
{
    const auto r=c.collider;
    const Vec2 center=rotate({(r.left+r.right)*.5f,(r.bottom+r.top)*.5f},rotation.x);
    const float hx=(r.right-r.left)*.5f,hy=(r.top-r.bottom)*.5f;
    const float x=std::fabs(std::cos(rotation.x))*hx+std::fabs(std::sin(rotation.x))*hy;
    const float y=std::fabs(std::sin(rotation.x))*hx+std::fabs(std::cos(rotation.x))*hy;
    return {center.x-x,center.y-y,center.x+x,center.y+y};
}
bool SimpleRock::touches(const SimpleRockConfig& c,Rect actor)const
{
    Vec2 center=rotate({(c.trigger.left+c.trigger.right)*.5f,(c.trigger.bottom+c.trigger.top)*.5f},rotation.x);
    center.x+=position.x;center.y+=position.y;
    const float radius=(c.trigger.right-c.trigger.left)*.5f;
    const float dx=center.x-std::clamp(center.x,actor.left,actor.right);
    const float dy=center.y-std::clamp(center.y,actor.bottom,actor.top);
    return dx*dx+dy*dy<=(radius+.02f)*(radius+.02f);
}
void SimpleRock::contact(const SimpleRockConfig& c,unsigned actor,bool overlap,float dt,EnemyRandom random)
{
    if(actor>=64||dt<=0||!std::isfinite(dt))return;
    const auto bit=std::uint64_t(1)<<actor;
    if(overlap&&!(touching&bit)){
        sleeping=false;
        // Approximation: adapt the source OnTriggerEnter(Collider) to the exported 2D detector.
        velocity.x+=random.range(-100,100)*dt/c.mass;
        velocity.y+=random.range(0,40)*dt/c.mass;
        angularVelocity+=random.range(-50,50)*dt/c.inertia;++enters;
    }
    if(overlap)touching|=bit;else touching&=~bit;
}
void SimpleRock::fixedUpdate(const SimpleRockConfig& c,float dt,const World& world,EnemyRandom random)
{
    if(dt<=0||!std::isfinite(dt))return;
    PresentationPosition::Step step(presentation,position),angleStep(rotationPresentation,rotation);
    if(sleeping){
        if(supportShapes==world.shapes&&supportIndex<world.shapeCount&&world.shapes[supportIndex].actorContact())return;
        sleeping=false;
    }
    if(steps==1)angularVelocity+=velocity.x*-7.5f*dt/c.inertia;
    if(steps<2)++steps;
    // ObjectBounce samples its displacement and speed every fourth FixedUpdate.
    if(sampleStep>=3){displacement={position.x-lastSample.x,position.y-lastSample.y};lastSample=position;
        sampleSpeed=std::hypot(velocity.x,velocity.y);sampleStep=0;}else ++sampleStep;
    velocity.y+=c.gravity*dt;
    angularVelocity/=1+c.damping*dt;rotation.x+=angularVelocity*dt;
    // Approximation: rotating boxes use their conservative AABB in the shared terrain solver.
    const auto local=collider(c);
    const auto contacts=moveBox(position,velocity,local,dt,world,c.material);
    std::array<unsigned,16> ids{};unsigned count=0;
    auto enter=[&](unsigned id,Vec2 normal){
        bool known=false;for(unsigned i=0;i<contactCount;++i)known=known||contactIds[i]==id;
        for(unsigned i=0;i<count;++i)if(ids[i]==id)return;
        if(count<ids.size())ids[count++]=id;
        if(known||sampleSpeed<=c.threshold)return;
        const float length=std::hypot(displacement.x,displacement.y);
        if(length>0){const Vec2 d{displacement.x/length,displacement.y/length};
            const float dot=d.x*normal.x+d.y*normal.y;
            const float speed=sampleSpeed*c.bounce*random.range(.8f,1.2f);
            velocity={(d.x-2*dot*normal.x)*speed,(d.y-2*dot*normal.y)*speed};}
        else velocity={};
        ++bounces;
    };
    if(world.shapeCount){for(unsigned i=0;i<contacts.count;++i)enter(contacts.surfaces[i].id,contacts.surfaces[i].normal);}
    else {
        if(contacts.ground)enter(1,{0,1});
        if(contacts.roof)enter(2,{0,-1});
        if(contacts.left)enter(3,{1,0});
        if(contacts.right)enter(4,{-1,0});
    }
    contactCount=count;contactIds=ids;
    if(contacts.ground){
        // Approximation: contact friction damps spin; the port has no angular contact solver.
        const float radius=(local.right-local.left)*.5f;
        const float drag=c.material.friction*c.mass*std::fabs(c.gravity)*radius*dt/c.inertia;
        angularVelocity=std::copysign(std::max(0.0f,std::fabs(angularVelocity)-drag),angularVelocity);
        if(std::hypot(velocity.x,velocity.y)<.001f&&std::fabs(angularVelocity)<.001f){
            // Approximation: sleep only on a known static source fixture; disabling it wakes the body.
            for(unsigned i=0;i<contacts.count&&!sleeping;++i)if(contacts.surfaces[i].normal.y>.5f)
                for(unsigned s=0;s<world.shapeCount;++s)if(world.shapes[s].id==contacts.surfaces[i].id){
                    sleeping=true;supportShapes=world.shapes;supportIndex=s;velocity={};angularVelocity=0;break;}
        }
    }
}
float RoomDynamics::randomUnit(void* context)
{
    auto& seed=*static_cast<unsigned*>(context);seed=seed*1664525u+1013904223u;
    return float(seed>>8)/16777216.0f;
}
bool RoomDynamics::bind(const RoomDynamicsBank& bank,RoomVisualBank& room)
{
    if(bank.spriteCount!=room.atlas.sprites.size()||bank.drawCount!=room.draws.size()||
       room.cameraBounds.size()!=room.draws.size()||!std::isfinite(bank.duration)||bank.duration<=0)return false;
    auto find=[&](unsigned owner,unsigned& target){unsigned count=0;
        for(unsigned i=0;i<room.draws.size();++i)if(room.draws[i].owner==owner){target=i;++count;}
        return count==1;};
    std::array<unsigned,32> spikes;std::array<unsigned,8> rocks;
    for(unsigned i=0;i<spikes.size();++i)if(!find(bank.spikes[i].owner,spikes[i])||
        room.draws[spikes[i]].sprite!=bank.spikes[i].authored)return false;
    for(unsigned i=0;i<rocks.size();++i)if(!find(bank.rocks[i].owner,rocks[i]))return false;
    for(const auto& k:bank.keys)if(k.sprite>=bank.spriteCount)return false;
    if(bank.crumbleCount>bank.crumble.size())return false;
    for(unsigned i=0;i<bank.crumbleCount;++i){const auto& c=bank.crumble[i];
        if(c.draw>=room.draws.size()||room.draws[c.draw].owner!=c.owner)return false;
        for(float v:c.values)if(!std::isfinite(v))return false;
        for(unsigned j=0;j<i;++j)if(c.draw==bank.crumble[j].draw)return false;
        if(i&&c.owner<bank.crumble[i-1].owner)return false;
        for(unsigned j:spikes)if(j==c.draw)return false;
        for(unsigned j:rocks)if(j==c.draw)return false;
    }
    // Approximation: independent visual RNG retains source ranges without changing gameplay draws.
    bank_=&bank;room_=&room;random_={&randomSeed_,randomUnit};spikeDraws_=spikes;rockDraws_=rocks;
    for(unsigned i=0;i<rocks.size();++i)authored_[i]=room.draws[rocks[i]];
    for(unsigned i=0;i<bank.crumbleCount;++i)
        std::copy_n(room.draws[bank.crumble[i].draw].values.begin(),6,crumbleAuthored_[i].begin());
    crumbleApplied_=false;
    restart();return true;
}
void RoomDynamics::restart()
{
    spikes_={};if(!ready())return;
    observeCrumble(false,false);
    for(unsigned i=0;i<rocks_.size();++i){rocks_[i].reset(bank_->rocks[i],random_);room_->draws[rockDraws_[i]]=authored_[i];}
    for(unsigned i=0;i<spikes_.size();++i){auto& draw=room_->draws[spikeDraws_[i]];draw.sprite=bank_->spikes[i].authored;
        room_->cameraBounds[spikeDraws_[i]]=roomCameraBounds(room_->atlas.sprites[draw.sprite],draw);}
    present(0,std::numeric_limits<float>::infinity(),0,{1});
}
void RoomDynamics::observeCrumble(bool crumbling,bool collapsed)
{
    if(!ready())return;
    const bool warning=crumbling&&!collapsed;
    if(warning==crumbleApplied_)return;
    for(unsigned i=0;i<bank_->crumbleCount;++i){const auto& c=bank_->crumble[i];auto& d=room_->draws[c.draw];
        const auto& pose=warning?c.values:crumbleAuthored_[i];
        std::copy(pose.begin(),pose.end(),d.values.begin());
        room_->cameraBounds[c.draw]=roomCameraBounds(room_->atlas.sprites[d.sprite],d);
    }
    crumbleApplied_=warning;
}
bool RoomDynamics::collapseOwner(unsigned owner)const
{
    if(!ready())return false;
    const auto end=bank_->crumble.begin()+bank_->crumbleCount;
    const auto found=std::lower_bound(bank_->crumble.begin(),end,owner,
        [](const CrumbleDrawConfig& d,unsigned id){return d.owner<id;});
    return found!=end&&found->owner==owner;
}
void RoomDynamics::fixedUpdate(float dt,const World& world,Rect hero,const CombatObject* enemies,unsigned count)
{
    if(!ready()||dt<=0||!std::isfinite(dt))return;
    for(unsigned i=0;i<rocks_.size();++i){auto& r=rocks_[i];const auto& c=bank_->rocks[i];
        r.contact(c,0,r.touches(c,hero),dt,random_);
        for(unsigned e=0;e<std::min(count,63u);++e){const auto& actor=enemies[e];
            const bool enabled=actor.layer==11&&actor.colliderEnabled&&(!actor.hasHealth||!actor.health.dead);
            r.contact(c,e+1,enabled&&r.touches(c,actor.bounds),dt,random_);}
        r.fixedUpdate(c,dt,world,random_);
    }
}
void RoomDynamics::present(float dt,float cameraX,float cameraY,PresentationTime time)
{
    if(!ready()||dt<0||!std::isfinite(dt))return;
    for(unsigned i=0;i<spikes_.size();++i){auto& draw=room_->draws[spikeDraws_[i]];
        auto& bounds=room_->cameraBounds[spikeDraws_[i]];auto& state=spikes_[i];
        state.advance(dt,bounds.contains(cameraX,cameraY),bank_->duration);
        const auto sprite=state.evaluated?bank_->sprite(state.seconds):bank_->spikes[i].authored;
        if(draw.sprite!=sprite){draw.sprite=sprite;bounds=roomCameraBounds(room_->atlas.sprites[sprite],draw);}
    }
    for(unsigned i=0;i<rocks_.size();++i){const auto& state=rocks_[i];const auto& base=authored_[i];
        auto& draw=room_->draws[rockDraws_[i]];draw=base;auto& v=draw.values;
        const auto p=state.renderPosition(time);const float delta=state.renderAngle(time)-std::atan2(v[3],v[0]);
        const auto x=rotate({v[0],v[3]},delta),y=rotate({v[1],v[4]},delta);
        v[0]=x.x;v[3]=x.y;v[1]=y.x;v[4]=y.y;v[2]=p.x;v[5]=p.y;v[6]+=state.depthJitter;
        room_->cameraBounds[rockDraws_[i]]=roomCameraBounds(room_->atlas.sprites[draw.sprite],draw);
    }
}
}
