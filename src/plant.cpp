#include "plant.hpp"
#include "render_memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds
{
static_assert(3*sizeof(PlantConfig)+3*sizeof(PlantAnimation)+3*sizeof(unsigned)+64<=passivePlantStateBudgetBytes,
              "Passive plant state exceeds its ordinary RAM allowance");
bool PlantBreakState::bind(const PlantBreakConfig& config,EnemyRandom random,void* context,Notify notify)
{
    if(!std::isfinite(config.angleOffset))return false;
    for(const auto& pose:config.debris)
        if(pose.active||!std::isfinite(pose.z)||!std::isfinite(pose.rotation))return false;
    config_=config;random_=random;context_=context;notify_=notify;bound_=true;reset();return true;
}
void PlantBreakState::reset()
{
    debris_=config_.debris;offset_=config_.angleOffset;
    state_.awake({},false,false,false,this,receive);
    // Eligibility is already a prerequisite of this selected plant contract.
    state_.start(0,0,0);
}
void PlantBreakState::emit(PlantBreakEvent event,unsigned index)
{if(notify_)notify_(context_,event,index,*this);}
void PlantBreakState::receive(void* context,BreakableAction action,const BreakableActivation&)
{
    auto& self=*static_cast<PlantBreakState*>(context);
    if(action==BreakableAction::StaticParts)self.emit(PlantBreakEvent::StaticParts);
    else if(action==BreakableAction::Debris)
    {
        for(unsigned i=0;i<self.debris_.size();++i)
        {
            auto& pose=self.debris_[i];pose.active=true;
            pose.z=self.random_.range(.002f,.006f);
            self.emit(PlantBreakEvent::DebrisActivated,i);
            pose.rotation=std::fmod(pose.rotation+self.offset_,360.f);
            if(pose.rotation<0)pose.rotation+=360;
            self.emit(PlantBreakEvent::DebrisRotated,i);
        }
    }
    else if(action==BreakableAction::CameraShake)self.emit(PlantBreakEvent::CameraShake);
}
bool PlantBreakState::hit(PlantHitDirection direction)
{
    if(!bound_||state_.activation().broken)return false;
    switch(direction)
    {
        case PlantHitDirection::Right:offset_=config_.angleOffset;break;
        case PlantHitDirection::Left:offset_=-config_.angleOffset;break;
        case PlantHitDirection::Up:case PlantHitDirection::Down:offset_=0;break;
        default:return false;
    }
    return state_.breakNow();
}
bool BreakablePlant::bind(const PlantConfig& config,CombatObject& object,EnemyRandom random,
                         void* context,BreakableDecor::HitEffect effect)
{
    if(config.owner!=config.collision.whole||!validDecor(config.collision)||
       !state_.bind(config.breakage,random))return false;
    config_=config;object_=&object;context_=context;effect_=effect;object={};
    object.name="Breakable plant";object.layer=config.collision.layer;
    object.bounds=config.collision.bounds;object.hitPolygon=&config_.collision.vertices;
    object.receiver=this;object.onHit=receive;
    return true;
}
float BreakablePlant::alpha(unsigned owner) const
{
    if(owner==config_.owner)return state_.activation().whole?1.0f:0.0f;
    if(owner==config_.collision.remnant)return state_.activation().remnants?1.0f:0.0f;
    return 1;
}
void BreakablePlant::receive(void* context,const NailHit& hit,HitOutcome)
{
    auto& self=*static_cast<BreakablePlant*>(context);
    const auto direction=hit.direction==Strike::Right?PlantHitDirection::Right:
        hit.direction==Strike::Left?PlantHitDirection::Left:
        hit.direction==Strike::Up?PlantHitDirection::Up:PlantHitDirection::Down;
    if(!self.state_.hit(direction))return;
    self.object_->colliderEnabled=false;
    if(self.effect_)self.effect_(self.context_,self.config_.owner,hit);
}
std::uint32_t PlantClip::sprite(float normalized) const
{
    const float time=(normalized-std::floor(normalized))*duration;
    unsigned key=0;
    while(key+1<count&&keys[key+1].time<=time)++key;
    return keys[key].sprite;
}
bool PlantBank::load(const char* path)
{
    FILE* file=std::fopen(path,"rb");if(!file)return false;
    PlantBank next;char magic[4];std::uint32_t header[5]{};
    bool ok=std::fread(magic,1,4,file)==4;
    const bool passive=ok&&std::memcmp(magic,"HKP3",4)==0;
    const bool breakable=ok&&(passive||std::memcmp(magic,"HKP2",4)==0);
    ok=ok&&(breakable||std::memcmp(magic,"HKP1",4)==0)&&
        std::fread(header,4,5,file)==5&&header[0]>0&&header[0]<=4&&
        header[1]>0&&header[1]<=2&&header[2]>0&&header[2]<=65536&&
        header[3]>0&&header[3]<=65536&&(passive?header[4]>0&&header[4]<=3:header[4]==0);
    if(ok){next.count=header[0];next.passiveCount=header[4];next.clipCount=header[1];next.spriteCount=header[2];next.drawCount=header[3];}
    for(unsigned i=0;ok&&i<next.clipCount;++i)
    {
        auto& clip=next.clips[i];std::uint32_t count=0;
        ok=std::fread(&clip.duration,4,1,file)==1&&std::isfinite(clip.duration)&&clip.duration>0&&
            std::fread(&count,4,1,file)==1&&count>0&&count<=clip.keys.size();clip.count=count;
        float previous=-1;
        for(unsigned k=0;ok&&k<count;++k)
        {
            auto& key=clip.keys[k];
            ok=std::fread(&key.time,4,1,file)==1&&std::fread(&key.sprite,4,1,file)==1&&
                std::isfinite(key.time)&&key.time>previous&&key.time<=clip.duration&&
                key.sprite<next.spriteCount&&(k||key.time==0);previous=key.time;
        }
    }
    for(unsigned i=0;ok&&i<next.count;++i)
    {
        auto& plant=next.plants[i];std::uint32_t row[3]{};
        ok=std::fread(row,4,3,file)==3&&row[0]>0&&row[1]<next.spriteCount&&row[2]<next.clipCount;
        plant={row[0],row[1],row[2]};
        if(ok&&breakable)
        {
            unsigned ids[2]{};float values[13]{};
            ok=std::fread(ids,4,2,file)==2&&std::fread(values,4,13,file)==13;
            auto& c=plant.collision;c.whole=plant.owner;c.remnant=ids[0];c.layer=ids[1];
            c.bounds={values[0],values[1],values[0],values[1]};
            for(unsigned v=0;v<4;++v)
            {
                c.vertices[v]={values[v*2],values[v*2+1]};
                c.bounds.left=std::min(c.bounds.left,values[v*2]);c.bounds.right=std::max(c.bounds.right,values[v*2]);
                c.bounds.bottom=std::min(c.bounds.bottom,values[v*2+1]);c.bounds.top=std::max(c.bounds.top,values[v*2+1]);
            }
            plant.breakage.angleOffset=values[8];
            for(unsigned d=0;d<2;++d)plant.breakage.debris[d]={values[9+d*2],values[10+d*2],false};
            ok=ok&&validDecor(c);
            for(float v:values)ok=ok&&std::isfinite(v);
        }
        for(unsigned j=0;j<i;++j)ok=ok&&next.plants[j].owner!=plant.owner;
    }
    for(unsigned i=next.count;ok&&i<next.animationCount();++i)
    {
        std::uint32_t row[4]{};
        ok=std::fread(row,4,4,file)==4&&row[0]>0&&row[1]<next.spriteCount&&
            row[2]<next.clipCount&&row[3]<=1;
        auto& plant=next.plants[i];plant.owner=row[0];plant.authored=row[1];plant.clip=row[2];plant.randomStart=row[3]!=0;
        for(unsigned j=0;j<i;++j)ok=ok&&next.plants[j].owner!=plant.owner;
    }
    ok=ok&&std::fgetc(file)==EOF&&!std::ferror(file);std::fclose(file);
    if(ok)*this=next;
    return ok;
}
void PlantAnimation::advance(float seconds,bool visible,const PlantClip& clip,EnemyRandom random,bool randomStart)
{
    if(!std::isfinite(seconds)||seconds<0)return;
    if(!started){started=true;if(!randomStart){sample=0;sampled=true;}}
    else if(!sampled){sample=random.range(0,1);sampled=true;}
    // Animator evaluates with visibility from the preceding rendered frame.
    if(sampled&&wasVisible)
    {
        if(!evaluated){phase=sample;evaluated=true;}
        else phase+=seconds/clip.duration;
        // Avoid loss of small deltas after a long session; looping phase only.
        if(phase>=1)phase-=std::floor(phase);
    }
    wasVisible=visible;
}
bool RoomPlants::bind(const PlantBank& bank,RoomVisualBank& room,EnemyRandom random)
{
    if(!bank.count||bank.count>4||bank.passiveCount>3||!bank.clipCount||bank.clipCount>2||
       bank.spriteCount!=room.atlas.sprites.size()||bank.drawCount!=room.draws.size()||
       room.cameraBounds.size()!=room.draws.size())return false;
    std::array<unsigned,7> draws{};
    for(unsigned i=0;i<bank.clipCount;++i)
    {
        const auto& clip=bank.clips[i];
        if(!std::isfinite(clip.duration)||clip.duration<=0||!clip.count||clip.count>clip.keys.size())return false;
        float previous=-1;
        for(unsigned k=0;k<clip.count;++k)
        {
            const auto& key=clip.keys[k];
            if(!std::isfinite(key.time)||key.time<=previous||key.time>clip.duration||
               key.sprite>=bank.spriteCount||(k==0&&key.time!=0))return false;
            previous=key.time;
        }
    }
    for(unsigned i=0;i<bank.animationCount();++i)
    {
        if(!bank.plants[i].owner||bank.plants[i].clip>=bank.clipCount||
           bank.plants[i].authored>=bank.spriteCount)return false;
        for(unsigned j=0;j<i;++j)if(bank.plants[i].owner==bank.plants[j].owner)return false;
        unsigned matches=0;
        for(unsigned d=0;d<room.draws.size();++d)if(room.draws[d].owner==bank.plants[i].owner)
        {draws[i]=d;++matches;}
        if(matches!=1||room.draws[draws[i]].sprite!=bank.plants[i].authored)return false;
    }
    bank_=&bank;room_=&room;random_=random;draws_=draws;restart();return true;
}
void RoomPlants::restart()
{
    states_={};passiveSeed_=0x504c414eu;
    if(!ready())return;
    for(unsigned i=0;i<bank_->animationCount();++i)
    {
        auto& draw=room_->draws[draws_[i]];draw.sprite=bank_->plants[i].authored;
        room_->cameraBounds[draws_[i]]=roomCameraBounds(room_->atlas.sprites[draw.sprite],draw);
    }
}
float RoomPlants::passiveUnit(void* context)
{
    auto& seed=*static_cast<unsigned*>(context);
    seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
    return float(seed>>8)*(1.f/16777216.f);
}
void RoomPlants::present(float seconds,float cameraX,float cameraY)
{
    if(!ready()||!std::isfinite(seconds)||seconds<0)return;
    for(unsigned i=0;i<bank_->animationCount();++i)
    {
        auto& state=states_[i];auto& draw=room_->draws[draws_[i]];
        auto& bounds=room_->cameraBounds[draws_[i]];
        const auto& clip=bank_->clips[bank_->plants[i].clip];
        // approximation: source random starts share Unity's global RNG. Keep
        // scenery-only clocks independent so adding art cannot change combat.
        const EnemyRandom random=i<bank_->count?random_:EnemyRandom{&passiveSeed_,passiveUnit};
        state.advance(seconds,bounds.contains(cameraX,cameraY),clip,random,bank_->plants[i].randomStart);
        const auto sprite=state.evaluated?clip.sprite(state.phase):bank_->plants[i].authored;
        if(sprite!=draw.sprite)
        {
            draw.sprite=sprite;
            bounds=roomCameraBounds(room_->atlas.sprites[sprite],draw);
        }
    }
}
}
