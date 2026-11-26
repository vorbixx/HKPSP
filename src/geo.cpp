#include "geo.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds {
bool GeoConfig::load(const char*path)
{
    FILE*f=std::fopen(path,"rb");if(!f)return false;
    GeoConfig c;char magic[4];float v[18]{};
    bool ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"GEO1",4)&&
        std::fread(v,4,18,f)==18&&std::fread(&c.value,4,1,f)==1;
    for(auto&e:c.enemies){float p[2]{};ok=ok&&std::fread(p,4,2,f)==2&&std::fread(e.journalKey,1,48,f)==48;e.effectOrigin={p[0],p[1]};ok=ok&&std::isfinite(p[0])&&std::isfinite(p[1])&&std::memchr(e.journalKey,0,48)&&e.journalKey[0];}
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    for(float x:v)ok=ok&&std::isfinite(x);
    c.body={v[0],v[1],v[2],v[3]};c.material={v[4],v[5]};c.gravity=v[6];c.pickupDelay=v[7];
    c.speedMin=v[8];c.speedMax=v[9];c.angleMin=v[10];c.angleMax=v[11];c.bounceFactor=v[12];c.bounceThreshold=v[13];
    c.bounceRandomMin=v[14];c.bounceRandomMax=v[15];c.depthMin=v[16];c.depthMax=v[17];
    ok=ok&&c.body.left<c.body.right&&c.body.bottom<c.body.top&&c.value>0&&c.pickupDelay>=0&&c.speedMin>=0&&c.speedMin<=c.speedMax&&c.angleMin<=c.angleMax&&c.bounceRandomMin>=0&&c.bounceRandomMin<=c.bounceRandomMax;
    if(ok)*this=c;
    return ok;
}
JournalEvent recordJournalKill(JournalEnemy enemy,const GeoConfig& config,JournalProgress& progress,bool hasJournal)
{
    auto index=static_cast<unsigned>(enemy);JournalEvent event;event.enemy=enemy;
    if(index>=config.enemies.size())return event;
    event.key=config.enemies[index].journalKey;
    if(!event.key[0])return event;
    if(!progress.killed){event.firstKill=true;progress.killed=true;progress.newData=true;}
    if(progress.remaining){--progress.remaining;if(!progress.remaining){event.notesCompleted=true;progress.newData=true;}}
    event.showMessage=hasJournal&&(event.firstKill||event.notesCompleted);
    return event;
}
bool GeoBank::bind(const GeoConfig&config,EnemyRandom random)
{
    if(config.value==0||config.body.left>=config.body.right||config.body.bottom>=config.body.top||config.pickupDelay<0)return false;
    *this=GeoBank{};config_=config;random_=random;ready_=true;return true;
}
unsigned GeoBank::activeCount()const {unsigned n=0;for(const auto&c:coins_)if(c.active)++n;return n;}
bool GeoBank::spawn(unsigned count,JournalEnemy enemy,Vec2 position,float scaleX)
{
    const auto index=static_cast<unsigned>(enemy);
    if(!ready_||index>=config_.enemies.size()||count>capacity-activeCount())return false;
    auto offset=config_.enemies[index].effectOrigin;
    position.x+=offset.x*scaleX;position.y+=offset.y;
    return spawnCoins(count,position,0,config_.value);
}
bool GeoBank::spawnReward(unsigned small,unsigned medium,unsigned large,Vec2 position,bool megaFling)
{
    const unsigned available=capacity-activeCount();
    if(!ready_||small>available||medium>available-small||large>available-small-medium)return false;
    return spawnCoins(small,position,0,config_.value,megaFling)&&spawnCoins(medium,position,1,5,megaFling)&&spawnCoins(large,position,2,25,megaFling);
}
bool GeoBank::spawnCoins(unsigned count,Vec2 position,unsigned size,unsigned value,bool megaFling)
{
    if(!ready_||count>capacity-activeCount())return false;
    for(auto&c:coins_){if(!count)break;if(c.active)continue;--count;const unsigned generation=c.spawnGeneration+1;c=GeoCoin{};c.spawnGeneration=generation;c.active=true;c.position=position;
        c.size=size;c.value=value;
        // Prefab OnEnable consumes its depth choice before FlingUtils assigns velocity.
        c.depth=random_.range(config_.depthMin,config_.depthMax);
        // HealthManager.Die uses a wider, faster fling for megaFlingGeo.
        float speed=random_.range(megaFling?30.f:config_.speedMin,megaFling?45.f:config_.speedMax);
        float angle=random_.range(megaFling?65.f:config_.angleMin,megaFling?115.f:config_.angleMax)*0.017453292519943295f;
        c.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
        // ObjectBounce's initial lastPos is its source zero-initialized field.
    }
    return true;
}
void GeoBank::update(float dt){if(dt<=0||!std::isfinite(dt))return;for(auto&c:coins_)if(c.active)c.age+=dt;}
Rect GeoBank::bounds(unsigned index)const {return index<capacity?boxBounds(coins_[index].position,config_.body):Rect{};}
void GeoBank::fixedUpdate(float dt,const World&world)
{
    if(!ready_||dt<=0||!std::isfinite(dt))return;
    for(auto&coin:coins_){if(!coin.active)continue;
        PresentationPosition::Step step(coin.presentation,coin.position);
        if(coin.sampleStep>=3){coin.sampleDisplacement={coin.position.x-coin.lastSamplePosition.x,coin.position.y-coin.lastSamplePosition.y};coin.lastSamplePosition=coin.position;coin.sampleSpeed=std::sqrt(coin.velocity.x*coin.velocity.x+coin.velocity.y*coin.velocity.y);coin.sampleStep=0;}else ++coin.sampleStep;
        coin.velocity.y+=config_.gravity*dt;
        auto contacts=moveBox(coin.position,coin.velocity,config_.body,dt,world,config_.material);
        unsigned ids[16]{};unsigned count=0;
        auto enter=[&](unsigned id,Vec2 normal){bool known=false;for(unsigned i=0;i<coin.contactCount;++i)known=known||coin.contactIds[i]==id;for(unsigned i=0;i<count;++i)known=known||ids[i]==id;if(count<16)ids[count++]=id;if(known)return;coin.idle=true;++coin.collisionGeneration;
            if(coin.sampleSpeed>config_.bounceThreshold){float length=std::sqrt(coin.sampleDisplacement.x*coin.sampleDisplacement.x+coin.sampleDisplacement.y*coin.sampleDisplacement.y);if(length>0){Vec2 d{coin.sampleDisplacement.x/length,coin.sampleDisplacement.y/length};float dot=d.x*normal.x+d.y*normal.y;float speed=coin.sampleSpeed*config_.bounceFactor*random_.range(config_.bounceRandomMin,config_.bounceRandomMax);coin.velocity={(d.x-2*dot*normal.x)*speed,(d.y-2*dot*normal.y)*speed};}else coin.velocity={};++bounces;}
        };
        if(world.shapeCount){for(std::size_t i=0;i<contacts.count;++i)enter(contacts.surfaces[i].id,contacts.surfaces[i].normal);}
        else {if(contacts.ground)enter(1,{0,1});if(contacts.roof)enter(2,{0,-1});if(contacts.left)enter(3,{1,0});if(contacts.right)enter(4,{-1,0});}
        coin.contactCount=count;for(unsigned i=0;i<count;++i)coin.contactIds[i]=ids[i];
    }
}
unsigned GeoBank::contactHero(Rect heroBox)
{
    unsigned value=0;
    for(unsigned i=0;i<capacity;++i){auto&c=coins_[i];if(!c.active)continue;const bool touch=overlaps(bounds(i),heroBox);if(touch&&!c.touchingHero&&c.age>=config_.pickupDelay){c.active=false;value+=c.value;++pickups;}c.touchingHero=touch;}
    collectedValue+=value;
    return value;
}
void GeoBank::gather(Vec2 hero,float dt)
{
    if(!ready_||!std::isfinite(dt)||dt<=0)return;
    for(auto& c:coins_)if(c.active&&c.age>=config_.pickupDelay){
        const float x=hero.x-c.position.x,y=hero.y-c.position.y,d=std::sqrt(x*x+y*y);
        // approximation: homing Geo instead of separate insect actors.
        if(d<.7f){c.active=false;++pickups;collectedValue+=c.value;continue;}
        const float step=std::min(d,20.f*dt);c.position.x+=x/d*step;c.position.y+=y/d*step;c.velocity={};
    }
}
}
