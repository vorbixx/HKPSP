#include "collectibles.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
bool CollectibleProgress::contains(unsigned id) const {
    const auto end=collected.begin()+std::min<std::uint32_t>(count,capacity);
    return std::find(collected.begin(),end,id)!=end;
}
bool CollectibleProgress::valid() const {
    if(count>capacity||grubs>46||maskShards>16||vesselFragments>9||rancidEggs>999||journals>999)return false;
    for(unsigned i=0;i<count;++i){if(!collected[i])return false;for(unsigned j=0;j<i;++j)if(collected[i]==collected[j])return false;}
    return true;
}
bool Collectibles::recordSourceEvent(unsigned id) {
    return remember(id);
}
bool Collectibles::bind(const std::vector<PickupRecord>& records) {
    if(records.size()>hits_.size())return false;
    for(unsigned i=0;i<records.size();++i){const auto& r=records[i];
        if(!r.id||!r.owner||unsigned(r.kind)>unsigned(PickupKind::Journal)||!r.hits||r.hits>100||!r.value||r.value>10000||
           !std::isfinite(r.position.x)||!std::isfinite(r.position.y)||!std::isfinite(r.bounds.left)||!std::isfinite(r.bounds.right)||
           !std::isfinite(r.bounds.bottom)||!std::isfinite(r.bounds.top)||r.bounds.left>=r.bounds.right||r.bounds.bottom>=r.bounds.top)return false;
        if(r.requireHeroRange&&(r.kind!=PickupKind::Grub||!std::isfinite(r.heroRange.left)||!std::isfinite(r.heroRange.right)||
           !std::isfinite(r.heroRange.bottom)||!std::isfinite(r.heroRange.top)||
           r.heroRange.left>=r.heroRange.right||r.heroRange.bottom>=r.heroRange.top))return false;
        for(unsigned j=0;j<i;++j)if(records[j].id==r.id)return false;
    }
    records_=records;hits_={};attacks_={};return true;
}
bool Collectibles::load(const char* path) {
    auto* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4]{};unsigned count=0;
    bool ok=std::fread(magic,4,1,f)==1;
    const bool ranges=ok&&!std::memcmp(magic,"PUP2",4);
    ok=ok&&(ranges||!std::memcmp(magic,"PUP1",4))&&std::fread(&count,4,1,f)==1&&count<=128;
    std::vector<PickupRecord> records(ok?count:0);
    for(auto& r:records){unsigned owners=0;
        ok=ok&&std::fread(&r.id,4,1,f)==1&&std::fread(&r.owner,4,1,f)==1&&std::fread(&r.kind,4,1,f)==1&&
            std::fread(&r.hits,4,1,f)==1&&std::fread(&r.value,4,1,f)==1&&std::fread(&r.position,sizeof(Vec2),1,f)==1&&
            std::fread(&r.bounds,sizeof(Rect),1,f)==1&&std::fread(&owners,4,1,f)==1&&owners<=128;
        if(ok){r.visualOwners.resize(owners);ok=std::fread(r.visualOwners.data(),4,owners,f)==owners;}
        if(ok&&ranges){unsigned flags=0;
            ok=std::fread(&flags,4,1,f)==1&&flags<=1&&std::fread(&r.heroRange,sizeof(Rect),1,f)==1;
            r.requireHeroRange=flags==1;
        }
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);return ok&&bind(records);
}
void Collectibles::apply(Vitals& vitals,bool heal) const {
    const int maximum=5+int(progress_.maskShards/4);
    if(heal&&vitals.maxHealth<maximum)vitals.health=maximum;
    vitals.maxHealth=maximum;vitals.reserveMax=int(progress_.vesselFragments/3)*33;
    vitals.health=std::min(vitals.health,maximum);vitals.reserve=std::min(vitals.reserve,vitals.reserveMax);
}
void Collectibles::restore(const CollectibleProgress& progress,Vitals& vitals) {
    if(!progress.valid())return;
    progress_=progress;hits_={};attacks_={};apply(vitals,true);
}
bool Collectibles::remember(unsigned sourceId) {
    if(!sourceId||!progress_.valid()||progress_.contains(sourceId)||progress_.count>=CollectibleProgress::capacity)return false;
    progress_.collected[progress_.count++]=sourceId;
    return true;
}
bool Collectibles::undoRemember(unsigned sourceId) {
    if(!sourceId||!progress_.valid()||!progress_.count||progress_.collected[progress_.count-1]!=sourceId)return false;
    progress_.collected[--progress_.count]=0;
    return true;
}
bool Collectibles::update(Hero& hero,const NailBank& nails,unsigned& geo) {
    if(hero.vitals.health<=0||!hero.controlEnabled)return false;
    bool changed=false;
    for(unsigned i=0;i<records_.size();++i){const auto& r=records_[i];
        if(progress_.contains(r.id)||progress_.count==CollectibleProgress::capacity)continue;
        if(r.requireHeroRange&&!overlaps(bodyBounds(hero.position),r.heroRange))continue;
        const bool struck=r.kind==PickupKind::Grub||r.kind==PickupKind::GeoDeposit;
        if(struck){
            if(!hero.nail.hitbox||attacks_[i]==hero.nail.serial||!nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],r.bounds))continue;
            attacks_[i]=hero.nail.serial;if(++hits_[i]<r.hits)continue;
        }else if(!overlaps(bodyBounds(hero.position),r.bounds))continue;
        auto next=progress_;next.collected[next.count++]=r.id;
        switch(r.kind){
        case PickupKind::Grub:++next.grubs;break;
        case PickupKind::MaskShard:++next.maskShards;break;
        case PickupKind::VesselFragment:++next.vesselFragments;break;
        case PickupKind::GeoDeposit:break;
        case PickupKind::RancidEgg:++next.rancidEggs;break;
        case PickupKind::Journal:++next.journals;break;
        }
        if(!next.valid())continue;
        progress_=next;if(r.kind==PickupKind::GeoDeposit)geo=std::min(9999999u,geo+r.value);
        apply(hero.vitals,true);changed=true;
    }
    return changed;
}
bool Collectibles::hides(unsigned owner) const {
    for(const auto& r:records_)if(progress_.contains(r.id)&&(r.owner==owner||std::find(r.visualOwners.begin(),r.visualOwners.end(),owner)!=r.visualOwners.end()))return true;
    return false;
}
}
