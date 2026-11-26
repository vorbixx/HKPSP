#include "save.hpp"
#include "enemy_rewards.hpp"
#include "system_settings.hpp"
#include "scene.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <initializer_list>
#include <memory>
#include <new>
namespace hk3ds
{
bool TutorialSave::clear(const char* path)
{
    if(!path||!path[0])return false;
    bool ok=true;
    for(const char* suffix:{"",".bak",".tmp"}){
        char file[256];
        if(std::snprintf(file,sizeof(file),"%s%s",path,suffix)>=int(sizeof(file)))return false;
        if(std::remove(file)!=0&&errno!=ENOENT)ok=false;
    }
    return ok;
}
namespace
{
std::uint32_t checksumPart(std::uint32_t crc,const unsigned char* data,unsigned n,std::uint32_t polynomial)
{
    for(unsigned i=0;i<n;++i)
    {crc^=data[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(polynomial&(~(crc&1u)+1u));}
    return crc;
}
std::uint32_t checksum(const unsigned char* data,unsigned n) {return ~checksumPart(0xffffffffu,data,n,0xedb88320u);}
struct Record {char magic[4];std::uint32_t version,build,broken,flags,geo,crawlerKills,buzzerKills,shadePresent,shadeGeo;float shadeX,shadeY;std::uint32_t shadeHealth;ConditionalSave conditional;char scene[32],gate[32];RestSave resting;CollectibleProgress collectibles;CharmInventory charms;std::uint32_t watcherBattleActivated,watcherChandelier,uumuuEncountered,uumuuDefeated;SavedPlayerData playerData;CartographySave cartography;SceneBoolSave sceneBools;std::uint32_t crc;};
static_assert(offsetof(Record,watcherBattleActivated)==772,"v8 save prefix");
static_assert(offsetof(Record,playerData)==788,"combined save prefix");
static_assert(offsetof(Record,cartography)==1300&&offsetof(Record,sceneBools)==1368&&offsetof(Record,crc)==1880&&sizeof(Record)==1884,"save format");
bool validShade(std::uint32_t present,std::uint32_t geo,float x,float y)
{return present<=1&&geo<=9999999&&std::isfinite(x)&&std::isfinite(y)&&std::abs(x)<10000&&std::abs(y)<10000;}
// Decodes one event record of any released layout; size is its exact length.
bool decodeRecord(const unsigned char* data,std::size_t size,Record& r)
{
    r={}; // A short legacy record must not inherit fields from a rejected full record.
    if(size>sizeof(r))return false;
    std::memcpy(&r,data,size);bool ok=true;
    // The map and named-value branches both wrote v9/v11; size identifies them.
    if((size==844&&r.version==9)||(size==860&&r.version==11)){
        auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::uint32_t crc;std::memcpy(&crc,bytes+size-4,4);
        ok &= crc==checksum(bytes,unsigned(size)-4);
        CartographySave legacyCartography{};
        std::memcpy(&legacyCartography,bytes+772,sizeof(legacyCartography));
        if(size==860)std::memmove(&r.watcherBattleActivated,bytes+840,16);
        else r.watcherBattleActivated=r.watcherChandelier=r.uumuuEncountered=r.uumuuDefeated=0;
        r.playerData={};r.cartography=legacyCartography;
    }
    else if(size==36&&((r.version==1&&r.flags<=TutorialSave::legacyFlags)||
                  (r.version==2&&r.flags<=TutorialSave::validFlags)))
    {
        const auto oldCrc=r.shadePresent;
        ok &= oldCrc==checksum(reinterpret_cast<const unsigned char*>(&r),32);
        r.shadePresent=r.shadeGeo=r.shadeHealth=0;r.shadeX=r.shadeY=0;
    }
    else if(size==56&&((r.version==2&&r.flags<=TutorialSave::legacyFlags)||r.version==3))
    {
        ok &= r.conditional.mrMushroomState==checksum(reinterpret_cast<const unsigned char*>(&r),52);
        r.conditional={};
    }
    else if(size==80&&r.version==4){std::uint32_t crc;std::memcpy(&crc,r.scene,4);ok &= crc==checksum(reinterpret_cast<const unsigned char*>(&r),76);}
    else if(size==144&&r.version==5){std::uint32_t crc;std::memcpy(&crc,&r.resting,4);ok &= crc==checksum(reinterpret_cast<const unsigned char*>(&r),140);}
    else if(r.version==6&&(size==156||size==228||size==680)){
        std::uint32_t crc;auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::memcpy(&crc,bytes+size-4,4);ok &= crc==checksum(bytes,unsigned(size)-4);
        if(size==156){std::memcpy(&r.charms,bytes+140,sizeof(r.charms));r.resting={};r.collectibles={};}
        else if(size==680){
            std::memmove(&r.collectibles,bytes+140,sizeof(r.collectibles));r.resting={};
            // The incoming collectibles layout assigned this bit to Hornet before integration.
            if(r.flags&TutorialSave::mawlekFlag)
                r.flags=(r.flags&~TutorialSave::mawlekFlag)|TutorialSave::hornetFlag;
        }
        else r.collectibles={};
    }
    else if(size==764&&r.version==7){
        std::uint32_t crc;std::memcpy(&crc,reinterpret_cast<unsigned char*>(&r)+760,4);
        ok &= crc==checksum(reinterpret_cast<const unsigned char*>(&r),760);r.charms={};
    }
    else if(size==776&&r.version==8){
        ok &= r.watcherBattleActivated==checksum(reinterpret_cast<const unsigned char*>(&r),772);
    }
    else if(size==784&&r.version==9){
        auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::uint32_t crc;std::memcpy(&crc,bytes+780,4);
        ok &= crc==checksum(bytes,780);
        std::memcpy(&r.watcherBattleActivated,bytes+772,8);
        r.cartography={};r.uumuuEncountered=r.uumuuDefeated=0;
    }
    else if(size==792&&r.version==10){
        std::uint32_t crc;auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::memcpy(&crc,bytes+788,4);ok &= crc==checksum(bytes,788);
        r.playerData={};
    }
    else if(size==1288&&r.version==9){
        // The branches used distinct v9 layouts, distinguished by size.
        // Verify its checksum before moving the overlapping legacy table.
        std::uint32_t crc;auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::memcpy(&crc,bytes+1284,4);ok &= crc==checksum(bytes,1284);
        std::memmove(&r.playerData,bytes+772,sizeof(r.playerData));
        r.watcherBattleActivated=r.watcherChandelier=r.uumuuEncountered=r.uumuuDefeated=0;
    }
    else if(size==1304&&r.version==11){
        std::uint32_t crc;auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::memcpy(&crc,bytes+1300,4);ok &= crc==checksum(bytes,1300);
        r.cartography={};
    }
    else if(size==1372&&r.version==12){
        std::uint32_t crc;auto* bytes=reinterpret_cast<unsigned char*>(&r);
        std::memcpy(&crc,bytes+1368,4);ok &= crc==checksum(bytes,1368);r.sceneBools={};
    }
    else ok &= size==sizeof(r)&&r.version==13&&r.crc==checksum(reinterpret_cast<const unsigned char*>(&r),sizeof(r)-4);
    if(r.version<=8){
        r.watcherBattleActivated=r.watcherChandelier=r.uumuuEncountered=r.uumuuDefeated=0;
        r.playerData={};
    }
    if(size<144){std::memset(r.scene,0,32);std::memset(r.gate,0,32);std::strcpy(r.scene,"Tutorial_01");}
    if(size<=144){r.resting={};r.collectibles={};}
    ok &= validSceneIdentifier(r.scene)&&validGateIdentifier(r.gate,true);
    ok &= !std::memcmp(r.magic,"HKSV",4)&&r.build==22529139&&r.broken<=127&&r.flags<=TutorialSave::validFlags&&r.geo<=9999999;
    ok &= validShade(r.shadePresent,r.shadeGeo,r.shadeX,r.shadeY)&&r.shadeHealth<=10&&(!r.shadePresent||r.shadeHealth>0);
    ok &= r.watcherBattleActivated<=1&&r.watcherChandelier<=1;
    ok &= r.uumuuEncountered<=1&&r.uumuuDefeated<=1;
    if(r.flags&128)r.charms.owned|=charmBit(6);
    if(r.version<=7)for(unsigned i=0;i<13;++i)if(const auto id=legacyCharmIds[i]){
        if(r.resting.ownedCharms&(1u<<i))r.charms.owned|=charmBit(id);
        if(r.resting.equippedCharms&(1u<<i))r.charms.equipped|=charmBit(id);
    }
    ok &= r.charms.valid()&&r.conditional.valid()&&r.resting.valid()&&r.collectibles.valid()&&r.playerData.valid()&&r.cartography.valid()&&r.sceneBools.valid();
    return ok;
}
struct Slot {SaveSlotHeader header;Record record;};
static_assert(sizeof(SaveSlotHeader)==16&&sizeof(Slot)==16+sizeof(Record)&&sizeof(Record)<=saveSlotPayloadLimit,"slot layout");
// CRC-32C, deliberately not the record's CRC-32: a record ends with its own
// CRC-32, and a CRC-32 over such a payload has the same value for every
// payload, so it would accept a new header torn onto the old record
// (guard: the torn-write sweep in tests/save_slot_checks.hpp).
std::uint32_t slotChecksum(const SaveSlotHeader& header,const unsigned char* payload)
{
    constexpr std::uint32_t castagnoli=0x82f63b78u;
    const auto crc=checksumPart(0xffffffffu,reinterpret_cast<const unsigned char*>(&header),12,castagnoli);
    return ~checksumPart(crc,payload,header.payloadBytes,castagnoli);
}
// Heap scratch: the save worker's stack is small and already holds a record.
struct SaveScratch {unsigned char file[saveContainerBytes];Record first,second;};
bool decodeSlot(const unsigned char* file,unsigned size,unsigned index,Record& r,std::uint32_t& sequence)
{
    const unsigned offset=index*saveSlotStride;SaveSlotHeader header;
    if(size<offset+sizeof(header))return false;
    std::memcpy(&header,file+offset,sizeof(header));
    if(std::memcmp(header.magic,"HKSL",4)||header.payloadBytes>saveSlotPayloadLimit||
       size<offset+sizeof(header)+header.payloadBytes)return false;
    const auto* payload=file+offset+sizeof(header);
    if(header.crc!=slotChecksum(header,payload)||!decodeRecord(payload,header.payloadBytes,r))return false;
    sequence=header.sequence;return true;
}
// Finds the newest valid record of a container or a legacy single-record file
// and describes where the next save must go.
const Record* scanSave(const char* path,SaveStorage& storage,SaveScratch& scratch,SaveCursor& cursor)
{
    cursor={};cursor.known=true;cursor.sequence=1;
    const long length=storage.read(path,scratch.file,sizeof(scratch.file));
    if(length<=0)return nullptr;
    const unsigned size=length>long(sizeof(scratch.file))?unsigned(sizeof(scratch.file)):unsigned(length);
    std::uint32_t sequences[2]{};
    const bool valid[2]={decodeSlot(scratch.file,size,0,scratch.first,sequences[0]),
                         decodeSlot(scratch.file,size,1,scratch.second,sequences[1])};
    if(valid[0]||valid[1]){
        const unsigned newest=valid[1]&&(!valid[0]||sequences[1]>sequences[0])?1u:0u;
        cursor.inPlace=length==long(saveContainerBytes);
        cursor.target=std::uint8_t(newest^1u);cursor.sequence=sequences[newest]+1;
        return newest?&scratch.second:&scratch.first;
    }
    return length<=long(sizeof(Record))&&decodeRecord(scratch.file,size,scratch.first)?&scratch.first:nullptr;
}
bool installFile(const char* path,const unsigned char* data,unsigned bytes)
{
    char temporary[512],backup[512];
    if(std::snprintf(temporary,sizeof(temporary),"%s.tmp",path)>=int(sizeof(temporary))||
       std::snprintf(backup,sizeof(backup),"%s.bak",path)>=int(sizeof(backup)))return false;
    auto* f=std::fopen(temporary,"wb");if(!f)return false;
    bool ok=std::fwrite(data,1,bytes,f)==bytes;ok &= std::fflush(f)==0;ok &= std::fclose(f)==0;
    if(ok&&std::rename(temporary,path)!=0)
    {
        // Windows/FAT implementations may reject rename over an existing file.
        // Keep the previous complete record until replacement succeeds.
        auto* previous=std::fopen(path,"rb");
        const bool exists=previous!=nullptr;if(previous)std::fclose(previous);
        if(exists)std::remove(backup);
        if(exists&&std::rename(path,backup)==0)
        {
            ok=std::rename(temporary,path)==0;
            if(ok)std::remove(backup);
            else std::rename(backup,path);
        }
        else ok=false;
    }
    if(!ok)std::remove(temporary);
    return ok;
}
std::uint64_t (*probeClock)()=nullptr;
SaveOperationTicks* probeResult=nullptr;
struct StdioSaveStorage final:SaveStorage
{
    long read(const char* path,unsigned char* out,unsigned capacity) override
    {
        auto* file=std::fopen(path,"rb");if(!file)return -1;
        long size=long(std::fread(out,1,capacity,file));
        if(size==long(capacity)&&std::fgetc(file)!=EOF)++size;
        const bool failed=std::ferror(file)!=0;std::fclose(file);
        return failed?-1:size;
    }
    bool writeAt(const char* path,unsigned offset,const unsigned char* data,unsigned bytes) override
    {
        const auto now=[]{return probeClock?probeClock():0;};
        const auto started=now();
        auto* file=std::fopen(path,"r+b");if(!file)return false;
        // Unbuffered: the slot reaches the card as one write call.
        std::setvbuf(file,nullptr,_IONBF,0);
        const auto opened=now();
        bool ok=std::fseek(file,long(offset),SEEK_SET)==0&&std::fwrite(data,1,bytes,file)==bytes;
        ok &= std::fflush(file)==0;
        const auto written=now();
        ok &= std::fclose(file)==0;
        if(probeResult)*probeResult={opened-started,written-opened,now()-written,false};
        return ok;
    }
    bool replace(const char* path,const unsigned char* data,unsigned bytes) override
    {
        const auto started=probeClock?probeClock():0;
        const bool ok=installFile(path,data,bytes);
        if(probeResult)*probeResult={0,(probeClock?probeClock():0)-started,0,true};
        return ok;
    }
};
}
SaveStorage& stdioSaveStorage() {static StdioSaveStorage storage;return storage;}
void probeSaveStorage(std::uint64_t (*clock)(),SaveOperationTicks* result) {probeClock=clock;probeResult=result;}
bool TutorialSave::load(const char* path) {return load(path,stdioSaveStorage());}
bool TutorialSave::write(const char* path) const {SaveCursor cursor;return write(path,cursor,stdioSaveStorage());}
bool TutorialSave::load(const char* path,SaveStorage& storage)
{
    const std::unique_ptr<SaveScratch> scratch(new(std::nothrow) SaveScratch);
    if(!scratch)return false;
    SaveCursor cursor;
    const Record* found=scanSave(path,storage,*scratch,cursor);
    if(!found)
    {
        // Recover the previous complete file if a whole-file replacement was
        // interrupted between moving the old file and installing the new one.
        char backup[512];
        if(std::snprintf(backup,sizeof(backup),"%s.bak",path)>=int(sizeof(backup)))return false;
        found=scanSave(backup,storage,*scratch,cursor);
        if(!found)return false;
    }
    const Record& r=*found;
    broken=r.broken;flags=r.flags;geo=r.geo;crawlerKills=r.crawlerKills;buzzerKills=r.buzzerKills;
    shadePresent=r.shadePresent;shadeGeo=r.shadeGeo;shadeX=r.shadeX;shadeY=r.shadeY;shadeHealth=r.shadeHealth;conditional=r.conditional;
    std::memcpy(scene,r.scene,32);std::memcpy(gate,r.gate,32);charms=r.charms;resting=r.resting;collectibles=r.collectibles;
    cartography=r.cartography;sceneBools=r.sceneBools;
    watcherProgress={r.watcherBattleActivated!=0,r.watcherChandelier!=0};
    uumuuProgress={r.uumuuEncountered!=0,r.uumuuDefeated!=0};playerData=r.playerData;enemyDeaths.reset();return true;
}
bool TutorialSave::write(const char* path,SaveCursor& cursor,SaveStorage& storage) const
{
    if(broken>127||flags>validFlags||geo>9999999||!validShade(shadePresent,shadeGeo,shadeX,shadeY)||shadeHealth>10||(!shadeHealth&&shadePresent)||!conditional.valid())return false;
    if(!validSceneIdentifier(scene)||!validGateIdentifier(gate,true)||!resting.valid()||!collectibles.valid()||!charms.valid()||!playerData.valid()||!cartography.valid()||!sceneBools.valid())return false;
    Slot slot{};Record& r=slot.record;std::memcpy(r.magic,"HKSV",4);r.version=13;r.build=22529139;
    r.broken=broken;r.flags=flags;r.geo=geo;r.crawlerKills=crawlerKills;r.buzzerKills=buzzerKills;
    r.shadePresent=shadePresent;r.shadeGeo=shadeGeo;r.shadeX=shadeX;r.shadeY=shadeY;r.shadeHealth=shadeHealth;
    r.conditional=conditional;r.resting=resting;r.collectibles=collectibles;r.charms=charms;r.playerData=playerData;r.cartography=cartography;r.sceneBools=sceneBools;
    // Reuse v13 PersistentBoolItem storage; bench-reset deaths stay session-only.
    EnemyLifeState deaths;
    if(!deaths.restoreSnapshot(enemyDeaths)||!deaths.mergePersistent(r.sceneBools))return false;
    r.watcherBattleActivated=watcherProgress.battleActivated?1u:0u;
    r.watcherChandelier=watcherProgress.watcherChandelier?1u:0u;
    r.uumuuEncountered=uumuuProgress.encounteredMegaJelly?1u:0u;
    r.uumuuDefeated=uumuuProgress.defeatedMegaJelly?1u:0u;
    std::memcpy(r.scene,scene,32);std::memcpy(r.gate,gate,32);
    r.crc=checksum(reinterpret_cast<const unsigned char*>(&r),sizeof(r)-4);
    if(!cursor.known){
        const std::unique_ptr<SaveScratch> scratch(new(std::nothrow) SaveScratch);
        if(!scratch)return false;
        scanSave(path,storage,*scratch,cursor);
    }
    // A wrapped sequence restarts through the whole-file path, which leaves no older slot behind.
    if(!cursor.sequence){cursor.inPlace=false;cursor.sequence=1;}
    std::memcpy(slot.header.magic,"HKSL",4);slot.header.sequence=cursor.sequence;slot.header.payloadBytes=sizeof(Record);
    const auto* bytes=reinterpret_cast<const unsigned char*>(&slot);
    slot.header.crc=slotChecksum(slot.header,bytes+sizeof(SaveSlotHeader));
    bool ok=false;
    if(cursor.inPlace)ok=storage.writeAt(path,cursor.target*saveSlotStride,bytes,sizeof(slot));
    else{
        // New file, legacy record or damaged size: install a full-size container
        // so every later save is a single in-place write.
        const std::unique_ptr<unsigned char[]> image(new(std::nothrow) unsigned char[saveContainerBytes]());
        if(image){std::memcpy(image.get(),bytes,sizeof(slot));ok=storage.replace(path,image.get(),saveContainerBytes);}
        cursor.target=0;
    }
    if(!ok){cursor={};return false;}
    cursor.inPlace=true;cursor.target^=1;++cursor.sequence;
    return true;
}

namespace {
bool readSettings(const char* path,SystemSettings& result)
{
    unsigned char record[16]{};
    auto* file=std::fopen(path,"rb");if(!file)return false;
    const auto size=std::fread(record,1,sizeof(record),file);
    const bool end=std::fgetc(file)==EOF;std::fclose(file);
    const unsigned version=record[4],payload=version>=3?12:8;
    if(!end||size!=payload+4||std::memcmp(record,"HKOP",4)||version<1||version>4||
       record[5]>10||record[6]>10||(version>=2&&record[7]>2)||(version>=3&&record[8]>10)||
       (version==4&&(record[9]>2||record[10]>1||record[11]>1)))return false;
    std::uint32_t crc=0;std::memcpy(&crc,record+payload,4);
    if(crc!=checksum(record,payload))return false;
    result.musicVolume=record[5];result.soundVolume=record[6];
    result.graphics=version>=2?static_cast<GraphicsMode>(record[7]):GraphicsMode::Auto;
    result.masterVolume=version>=3?record[8]:10;
    result.cameraShake=version==4?static_cast<SystemSettings::CameraShake>(record[9]):SystemSettings::CameraShake::On;
    result.hudAppearance=version==4?static_cast<SystemSettings::HudAppearance>(record[10]):SystemSettings::HudAppearance::Small;
    result.backerCredits=version==4&&record[11];
    return true;
}
}
bool SystemSettings::load(const char* path)
{
    if(readSettings(path,*this))return true;
    char backup[512];
    return std::snprintf(backup,sizeof(backup),"%s.bak",path)<int(sizeof(backup))&&readSettings(backup,*this);
}
bool SystemSettings::write(const char* path) const
{
    if(masterVolume>10||musicVolume>10||soundVolume>10||static_cast<unsigned>(graphics)>2||
       unsigned(cameraShake)>2||unsigned(hudAppearance)>1)return false;
    const bool gameOptions=cameraShake!=CameraShake::On||hudAppearance!=HudAppearance::Small||backerCredits;
    const unsigned version=gameOptions?4:masterVolume!=10?3:2,payload=version>=3?12:8;
    unsigned char record[16]={'H','K','O','P',static_cast<unsigned char>(version),
        musicVolume,soundVolume,static_cast<unsigned char>(graphics),masterVolume,
        static_cast<unsigned char>(cameraShake),static_cast<unsigned char>(hudAppearance),static_cast<unsigned char>(backerCredits)};
    const auto crc=checksum(record,payload);std::memcpy(record+payload,&crc,4);
    char temporary[512],backup[512];
    if(std::snprintf(temporary,sizeof(temporary),"%s.tmp",path)>=int(sizeof(temporary))||
       std::snprintf(backup,sizeof(backup),"%s.bak",path)>=int(sizeof(backup)))return false;
    auto* file=std::fopen(temporary,"wb");if(!file)return false;
    bool ok=std::fwrite(record,1,payload+4,file)==payload+4;
    ok &= std::fflush(file)==0;ok &= std::fclose(file)==0;
    if(ok&&std::rename(temporary,path)!=0){
        auto* previous=std::fopen(path,"rb");const bool exists=previous!=nullptr;
        if(previous)std::fclose(previous);
        if(exists)std::remove(backup);
        if(exists&&std::rename(path,backup)==0){
            ok=std::rename(temporary,path)==0;
            if(ok)std::remove(backup);else std::rename(backup,path);
        }else ok=false;
    }
    if(!ok)std::remove(temporary);
    return ok;
}
}
