#include "draw_list.hpp"
#include "../include/platform.hpp"
#include "sprite_layout.hpp"
#include "chunked_scratch.hpp"
#include "perf.hpp"
#include "room_ge.hpp"
#ifdef __PSP__
#include <pspkernel.h>
#endif
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace hkpsp {
namespace {
class Reader {
    ChunkedScratch<unsigned char,16384> bytes;
    std::size_t offset=0;
    std::string name;
public:
    // Read to EOF in chunks: the size never comes from a seek, so a file system whose
    // seek/tell differs (real PSP vs PPSSPP, user bug 2026-11-15) cannot shorten the data.
    explicit Reader(const std::string& path):name(path) {
        std::unique_ptr<std::FILE,decltype(&std::fclose)> handle(std::fopen(path.c_str(),"rb"),&std::fclose);
        auto* file=handle.get();
        if(!file)throw std::runtime_error("Missing asset: "+path);
        unsigned char chunk[16384];
        for(;;){
            const std::size_t got=std::fread(chunk,1,sizeof chunk,file);
            bytes.append(chunk,got);
            if(got<sizeof chunk)break;
            if(bytes.size()>64u*1024u*1024u)fail();
        }
        const bool error=std::ferror(file)!=0;
        if(error)fail();
    }
    // The message names the failing check (source line), the read offset and the file size,
    // so a hardware-only rejection can be diagnosed from one screen photo.
    [[noreturn]] void fail(unsigned line=__builtin_LINE())const {
        throw std::runtime_error("Invalid asset: "+name+" (draw_list.cpp:"+std::to_string(line)+
                                 ", offset "+std::to_string(offset)+" of "+std::to_string(bytes.size())+" bytes)");
    }
    unsigned u8(){if(offset>=bytes.size())fail();return bytes[offset++];}
    unsigned u16(){const unsigned a=u8();return a|(u8()<<8);}
    std::uint32_t u32(){const auto a=u16();return a|(std::uint32_t(u16())<<16);}
    float f32(){const auto bits=u32();float value;std::memcpy(&value,&bits,4);if(!std::isfinite(value))fail();return value;}
    void magic(const char* value){for(unsigned i=0;i<4;++i)if(u8()!=static_cast<unsigned char>(value[i]))fail();}
    void exact(std::uint64_t expected){if(expected!=bytes.size())fail();}
    void atLeast(std::uint64_t expected){if(expected>bytes.size())fail();}
    std::size_t remaining()const{return bytes.size()-offset;}
    bool starts(const char* value)const{
        if(remaining()<4)return false;
        for(unsigned i=0;i<4;++i)if(bytes[offset+i]!=static_cast<unsigned char>(value[i]))return false;
        return true;
    }
    void end(){if(offset!=bytes.size())fail();}
};
struct Header {unsigned size,pages,sprites,a,b;};
Header header(Reader& r,const char* magic,unsigned* version=nullptr) {
    r.magic(magic);const unsigned value=r.u32();
    if(value!=1&&!(version&&value==2))r.fail();
    if(version)*version=value;
    Header h{r.u32(),r.u32(),r.u32(),r.u32(),r.u32()};
    if(h.size!=512||h.pages==0||h.pages>128||h.sprites==0||h.sprites>65536||h.a==0||h.a>65536||h.b>1048576)r.fail();
    return h;
}
hk3ds::SpriteFrame sprite(Reader& r,const Header& h,bool logical=false) {
    hk3ds::SpriteFrame s{r.u32(),r.u32(),r.u32(),r.u32(),r.u32(),r.f32(),r.f32(),r.f32(),r.f32()};
    if(s.page>=h.pages||!s.width||!s.height||s.width>16384||s.height>16384||
       (!logical&&(s.width>h.size||s.height>h.size||s.x>h.size-s.width||s.y>h.size-s.height))||
       s.left>=s.right||s.bottom>=s.top||
       std::fabs(s.left)>100000||std::fabs(s.right)>100000||std::fabs(s.bottom)>100000||std::fabs(s.top)>100000)r.fail();
    return s;
}
void uv(Quad& q,const hk3ds::SpriteFrame& s){
    q.page=s.page;q.clut=s.clut;q.u0=float(s.x);q.v0=float(s.y);q.u1=float(s.x+s.width);q.v1=float(s.y+s.height);
}
// Optional footer follows all existing records (including animation events).
void spriteCluts(Reader& r,std::vector<hk3ds::SpriteFrame>& sprites){
    if(!r.remaining())return;
    r.magic("SCL1");const unsigned count=r.u32();
    if(count!=sprites.size()||r.remaining()<4ull*count)r.fail();
    for(auto& s:sprites){s.clut=r.u32();if(s.clut>=128)r.fail();}
}
std::shared_ptr<const ClutBank> readCluts(const std::string& texture,unsigned count){
    const auto slash=texture.find_last_of("/\\");
    const auto file=texture.substr(slash==std::string::npos?0:slash+1);
    // Flat PSP staging uses the logical page prefix to separate CLUT namespaces.
    const auto page=file.rfind("-page-");
    const auto palette=page==std::string::npos?"palettes.pcl":file.substr(0,page)+"-palettes.pcl";
    const auto path=(slash==std::string::npos?std::string{}:texture.substr(0,slash+1))+palette;
    // Expired banks are removed so room changes cannot accumulate live resources.
    static std::vector<std::pair<std::string,std::weak_ptr<const ClutBank>>> cache;
    for(auto i=cache.begin();i!=cache.end();){
        if(auto bank=i->second.lock()){
            if(i->first==path){if(bank->size()!=count)throw std::runtime_error("HPT2 CLUT count mismatch: "+texture);return bank;}
            ++i;
        }else i=cache.erase(i);
    }
    Reader r(path);r.magic("PCL1");if(r.u32()!=1||r.u32()!=count)r.fail();
    r.exact(12ull+1024ull*count);
    auto bank=std::make_shared<ClutBank>(count);
    for(auto& clut:*bank){
        for(auto& entry:clut.raw)for(auto& c:entry)c=static_cast<unsigned char>(r.u8());
        for(unsigned i=0;i<256;++i)for(unsigned c=0;c<4;++c){
            const auto& p=clut.raw[i];
            clut.premultiplied[i][c]=c==3?p[c]:static_cast<unsigned char>((unsigned(p[c])*p[3]+127)/255);
            // Same positive folded-light transform as Page::load().
            clut.light[i][c]=c==3?255:static_cast<unsigned char>(p[c]>128?2*p[c]-256:0);
        }
    }
    r.end();
#ifdef __PSP__
    sceKernelDcacheWritebackRange(bank->data(),bank->size()*sizeof(Clut));
#endif
    cache.emplace_back(path,bank);return bank;
}
void require(bool condition){if(!condition)throw std::runtime_error("draw-list contract failed");}
void near(float a,float b){require(std::fabs(a-b)<.0001f);}
template<class F> void roomSprites(const DrawList& list,const hk3ds::RoomVisualBank& bank,unsigned index,F callback){
    if(list.roomTiles&&index<list.roomTiles->size()&&!(*list.roomTiles)[index].empty()){
        for(const auto& part:(*list.roomTiles)[index])callback(part);
    }else callback(bank.atlas.sprites.at(index));
}
}
hk3ds::RoomVisualBank readLightAtlas(const std::string& path) {
    Reader r(path);r.magic("PSL1");if(r.u32()!=1)r.fail();
    Header h{r.u32(),r.u32(),r.u32(),0,0};
    if(h.size!=512||h.pages==0||h.pages>4||h.sprites!=5)r.fail();
    r.exact(20+36*5);
    hk3ds::RoomVisualBank bank;bank.atlas.pageCount=h.pages;
    bank.atlas.sprites.reserve(h.sprites);
    for(unsigned i=0;i<h.sprites;++i)bank.atlas.sprites.push_back(sprite(r,h));
    r.end();return bank;
}
hk3ds::RoomVisualBank readRoom(const std::string& path,RoomTiles* tiles) {
    Reader r(path);unsigned version=0;const auto h=header(r,"PSR1",&version);
    r.atLeast(28ull+36ull*h.sprites+72ull*h.a+24ull*h.b+4);
    if(h.b>h.a)r.fail();
    hk3ds::RoomVisualBank bank;bank.atlas.pageCount=h.pages;
    bank.atlas.sprites.reserve(h.sprites);bank.draws.reserve(h.a);bank.cameraBounds.reserve(h.a);
    for(unsigned i=0;i<h.sprites;++i)bank.atlas.sprites.push_back(sprite(r,h,version==2));
    for(unsigned i=0;i<h.a;++i){
        hk3ds::RoomDraw d;d.sprite=r.u32();d.owner=r.u32();
        for(auto& v:d.values){v=r.f32();if(std::fabs(v)>100000)r.fail();}
        if(d.sprite>=h.sprites||d.values[15]<0||d.values[15]>7||d.values[15]!=std::floor(d.values[15]))r.fail();
        for(unsigned c=7;c<=10;++c)if(d.values[c]<0||d.values[c]>1)r.fail();
        bank.draws.push_back(d);
    }
    std::vector<bool> grassSeen(h.a);
    for(unsigned i=0;i<h.b;++i){
        const auto index=r.u32();if(index>=h.a||grassSeen[index])r.fail();grassSeen[index]=true;
        bank.draws[index].grass={r.f32(),r.f32(),r.f32(),r.f32(),r.f32()};
    }
    bank.blurSplitDepth=r.f32();if(bank.blurSplitDepth< -28.1f||bank.blurSplitDepth>961.9f)r.fail();
    RoomTiles loaded;
    if(version==2){
        loaded.resize(h.sprites);const unsigned count=r.u32();
        if(!count||count>h.sprites)r.fail();
        unsigned previous=0;
        for(unsigned i=0;i<count;++i){
            const unsigned index=r.u32(),parts=r.u32();
            if(index>=h.sprites||(i&&index<=previous)||parts<2||parts>4096)r.fail();
            previous=index;const auto& full=bank.atlas.sprites[index];
            struct Region {unsigned x,y,w,h;};std::vector<Region> regions;
            std::uint64_t area=0;
            for(unsigned j=0;j<parts;++j){
                const unsigned x=r.u32(),y=r.u32();auto part=sprite(r,h);
                if(x>full.width||y>full.height||part.width>full.width-x||part.height>full.height-y)r.fail();
                for(const auto& old:regions)
                    if(x<old.x+old.w&&old.x<x+part.width&&y<old.y+old.h&&old.y<y+part.height)r.fail();
                regions.push_back({x,y,part.width,part.height});area+=std::uint64_t(part.width)*part.height;
                // Derive geometry from integer texel partitions, avoiding seams
                // caused by independently rounded exporter geometry.
                part.left=full.left+(full.right-full.left)*float(x)/float(full.width);
                part.right=full.left+(full.right-full.left)*float(x+part.width)/float(full.width);
                part.top=full.top-(full.top-full.bottom)*float(y)/float(full.height);
                part.bottom=full.top-(full.top-full.bottom)*float(y+part.height)/float(full.height);
                loaded[index].push_back(part);
            }
            if(area!=std::uint64_t(full.width)*full.height)r.fail();
        }
        for(unsigned i=0;i<h.sprites;++i)if(loaded[i].empty()){
            const auto& s=bank.atlas.sprites[i];
            if(s.width>h.size||s.height>h.size||s.x>h.size-s.width||s.y>h.size-s.height)r.fail();
        }
    }
    spriteCluts(r,bank.atlas.sprites);
    for(const auto& d:bank.draws)bank.cameraBounds.push_back(hk3ds::roomCameraBounds(bank.atlas.sprites[d.sprite],d,480,272));
    if(!bank.loadSwitches(path.c_str()))throw std::runtime_error("Invalid room switches: "+path);
    bank.extendSwitchBounds(480,272);
    r.end();if(tiles)*tiles=std::move(loaded);return bank;
}
hk3ds::AnimationBank readAnimation(const std::string& path,std::string* pool) {
    if(pool)pool->clear();
    Reader r(path);const auto h=header(r,"PSA1");r.atLeast(28ull+36ull*h.sprites+68ull*h.a+4ull*h.b);
    if(!h.b)r.fail();
    hk3ds::AnimationBank bank;bank.pageCount=h.pages;
    bank.sprites.reserve(h.sprites);bank.clips.reserve(h.a);bank.frames.reserve(h.b);
    for(unsigned i=0;i<h.sprites;++i)bank.sprites.push_back(sprite(r,h));
    for(unsigned i=0;i<h.a;++i){
        hk3ds::Clip c{};for(auto& ch:c.name)ch=static_cast<char>(r.u8());
        c.fps=r.f32();c.wrap=r.u32();c.loopStart=r.u32();c.first=r.u32();c.count=r.u32();
        if(!std::memchr(c.name,0,sizeof(c.name))||c.fps<=0||!(c.wrap<=3||c.wrap==6)||!c.count||
           c.first>h.b||c.count>h.b-c.first||(c.wrap==1&&c.loopStart>=c.count))r.fail();
        bank.clips.push_back(c);
    }
    for(unsigned i=0;i<h.b;++i){const auto f=r.u32();if(f>=h.sprites)r.fail();bank.frames.push_back(f);}
    if(r.starts("AEV1")){
        r.magic("AEV1");const auto count=r.u32();
        if(count>h.b||r.remaining()<76ull*count)r.fail();
        std::vector<hk3ds::AnimationEvent> events;
        events.reserve(count);
        for(unsigned i=0;i<count;++i){
            hk3ds::AnimationEvent event{};event.frame=r.u32();
            for(auto& ch:event.info)ch=static_cast<char>(r.u8());
            const auto integer=r.u32();std::memcpy(&event.integer,&integer,4);event.value=r.f32();
            if(event.frame>=h.b||(i&&event.frame<=events.back().frame)||!std::memchr(event.info,0,sizeof(event.info)))r.fail();
            events.push_back(event);
        }
        bank.events.replace(std::move(events));
    }
    spriteCluts(r,bank.sprites);
    if(r.starts("PPG1")){
        r.magic("PPG1");const auto count=r.u32();
        if(!count||count>64||r.remaining()!=count)r.fail();
        std::string name;
        for(unsigned i=0;i<count;++i){
            const char c=static_cast<char>(r.u8());
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))r.fail();
            name+=c;
        }
        if(pool)*pool=std::move(name);
    }
    r.end();return bank;
}
AnimationAtlas AnimationPages::load(Platform& platform,std::vector<Texture>& textures,
        const std::string& table,const std::string& prefix,bool unpack,
        std::initializer_list<const char*> cpuClips){
    std::string pool;AnimationAtlas result;
    result.bank=readAnimation(platform.path(table),&pool);
    result.page=static_cast<unsigned>(textures.size());
    const Pool* found=nullptr;
    for(const auto& entry:pools)if(entry.name==pool&&!pool.empty()){
        if(entry.count!=result.bank.pageCount||entry.first+entry.count>textures.size())
            throw std::runtime_error("Shared page pool count mismatch: "+table);
        result.page=entry.first;found=&entry;break;
    }
    const auto name=pool.empty()?prefix:pool;
    const auto pagePrefix=name.empty()?"page-":name+"-page-";
    std::vector<bool> cpuPages(result.bank.pageCount,unpack&&cpuClips.size()==0);
    if(unpack)for(const auto* name:cpuClips){
        const auto* clip=result.bank.find(name);
        if(!clip)throw std::runtime_error("Missing CPU-sampled clip: "+std::string(name));
        for(unsigned i=0;i<clip->count;++i)
            cpuPages.at(result.bank.sprites.at(result.bank.frames.at(clip->first+i)).page)=true;
    }
    std::vector<std::string> paths;
    const auto read=[&](unsigned i,bool cpu){
        const auto path=found?found->paths.at(i):platform.path(pagePrefix+std::to_string(i)+".hpt");
        if(!found)paths.push_back(path);
        auto texture=readTexture(path,cpu);
        if(texture.width!=512||texture.height!=512)throw std::runtime_error("Atlas/page dimensions disagree: "+table);
        return texture;
    };
    if(!found){
        for(unsigned i=0;i<result.bank.pageCount;++i){
            textures.push_back(read(i,cpuTextureIndices||cpuPages[i]));
        }
    }
    // A pooled HUD must not unpack the entire pool on PSP. Keep only pages its
    // sprites can sample on the CPU; all other pool pages stay GE-only.
    if(found&&unpack)for(unsigned i=0;i<result.bank.pageCount;++i)if(cpuPages[i]){
        auto& texture=textures.at(result.page+i);
        if(texture.indices.empty())texture=read(i,true);
    }
    if(!found&&!pool.empty())pools.push_back({pool,result.page,result.bank.pageCount,std::move(paths)});
    return result;
}
Texture readTexture(const std::string& path,bool unpack) {
    Reader r(path);const bool mixed=r.starts("HPT2");r.magic(mixed?"HPT2":"HPT1");
    Texture t;t.width=r.u16();t.height=r.u16();
    const unsigned psm=r.u8();t.bits=psm==4?4:8;
    const unsigned entries=t.bits==4?16:256,row=t.width*t.bits/8;
    if(!t.width||!t.height||t.width>512||t.height>512||row%16||t.height%8||
       (t.width&(t.width-1))||(t.height&(t.height-1))||
       (psm!=4&&psm!=5)||r.u8()!=3||r.u8()!=1||r.u8()!=0)r.fail();
    const unsigned count=r.u32();
    if(mixed){
        if(psm!=5||!count||count>128)r.fail();
        r.exact(16ull+std::uint64_t(row)*t.height);t.cluts=readCluts(path,count);
    }else{
        if(count!=entries)r.fail();
        r.exact(16ull+entries*4ull+std::uint64_t(row)*t.height);
        for(unsigned i=0;i<entries;++i)for(auto& c:t.palette[i])c=static_cast<unsigned char>(r.u8());
    }
    if(unpack)t.indices.resize(t.width*t.height);
    for(unsigned by=0;by<t.height;by+=8)for(unsigned bx=0;bx<row;bx+=16)
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x){
            const auto value=static_cast<unsigned char>(r.u8());
            const unsigned offset=(by+y)*t.width+(bx+x)*(8/t.bits);
            if(unpack){
                t.indices[offset]=t.bits==4?value&15:value;
                if(t.bits==4)t.indices[offset+1]=value>>4;
            }
        }
    r.end();return t;
}
const Glyph& FontBank::glyph(unsigned font,unsigned code)const {
    const auto& f=fonts.at(font);
    for(unsigned i=f.first;i<f.first+f.count;++i)if(glyphs[i].code==code)return glyphs[i];
    throw std::runtime_error("Missing PFT1 glyph");
}
float FontBank::kern(unsigned font,unsigned left,unsigned right)const {
    const auto key=std::array<unsigned,3>{font,left,right};
    const auto i=std::lower_bound(kerning.begin(),kerning.end(),key,[](const Kern& k,const auto& v){
        return std::array<unsigned,3>{k.font,k.left,k.right}<v;
    });
    return i!=kerning.end()&&std::array<unsigned,3>{i->font,i->left,i->right}==key?i->adjustment:0;
}
FontBank readFonts(const std::string& path) {
    Reader r(path);r.magic("PFT1");if(r.u32()!=1||r.u32()!=256)r.fail();
    FontBank b;b.pageCount=r.u32();const unsigned nf=r.u32(),ng=r.u32(),nk=r.u32();
    if(!b.pageCount||b.pageCount>32||nf!=4||!ng||ng>4096||nk>65536)r.fail();
    r.exact(28ull+nf*32ull+ng*40ull+nk*16ull);
    b.fonts.reserve(nf);b.glyphs.reserve(ng);b.kerning.reserve(nk);
    unsigned first=0;constexpr unsigned sizes[]={15,11,14,18};
    for(unsigned i=0;i<nf;++i){
        FontMetrics f{r.u32(),r.u32(),r.u32(),r.u32(),r.f32(),r.f32(),r.f32(),r.f32()};
        if(f.id!=i||f.size!=sizes[i]||f.first!=first||!f.count||f.count>ng-first||
           f.ascent<0||f.descent<0||f.capitals<(i>=2?11:9)||f.lineHeight<f.capitals)r.fail();
        first+=f.count;b.fonts.push_back(f);
    }
    if(first!=ng)r.fail();
    for(unsigned i=0;i<ng;++i){
        Glyph g{r.u32(),r.u32(),r.u32(),r.u32(),r.u32(),r.u32(),r.u32(),r.f32(),r.f32(),r.f32()};
        if(g.font>=nf||g.code>0x10ffff||(g.code>=0xd800&&g.code<=0xdfff)||g.page>=b.pageCount||
           !g.width||!g.height||g.width>256||g.height>256||g.x>256-g.width||g.y>256-g.height||g.advance<0)r.fail();
        const auto& f=b.fonts[g.font];if(i<f.first||i>=f.first+f.count)r.fail();
        for(const auto& old:b.glyphs)if(old.font==g.font&&old.code==g.code)r.fail();
        b.glyphs.push_back(g);
    }
    for(unsigned i=0;i<nk;++i){
        Kern k{r.u32(),r.u32(),r.u32(),r.f32()};
        if(k.font>=nf||k.adjustment==0)r.fail();
        b.glyph(k.font,k.left);b.glyph(k.font,k.right);b.kerning.push_back(k);
    }
    const auto key=[](const Kern& k){return std::array<unsigned,3>{k.font,k.left,k.right};};
    std::sort(b.kerning.begin(),b.kerning.end(),[&](const Kern& a,const Kern& c){return key(a)<key(c);});
    for(unsigned i=1;i<nk;++i)if(key(b.kerning[i-1])==key(b.kerning[i]))r.fail();
    r.end();return b;
}
bool roomQuad(const hk3ds::SpriteFrame& s,const hk3ds::RoomDraw& draw,hk3ds::Vec2 camera,float seconds,Quad& q) {
    const auto v=hk3ds::roomDrawValues(draw,seconds);
    if(!hk3ds::roomDrawHasOpacity(draw)){HKPSP_PROFILE(++perf::current[perf::RejectOpacity];)return false;}
    HKPSP_PROFILE(++perf::current[perf::RoomCpuProjected];)
#ifdef HKPSP_RENDER_PROBE
    const float focal=cpu::config().c1?cpu::focal:hk3ds::roomFocalLength(screenWidth,screenHeight);
#else
    const float focal=hk3ds::roomFocalLength(screenWidth,screenHeight);
#endif
    const std::array<Point,4> local{{{s.left,s.top},{s.right,s.top},{s.right,s.bottom},{s.left,s.bottom}}};
    float left=std::numeric_limits<float>::max(),right=-left,top=left,bottom=-left,nearest=left,farthest=-left;
    uv(q,s);q.material=hk3ds::roomMaterial(draw);q.rgba={v[7],v[8],v[9],v[10]};
    // shader557: the exported source clamps the sum before source-alpha lerp.
    q.blend=q.material==hk3ds::RoomMaterial::LinearDodge?Blend::ClampedAdd:Blend::Room;
    // main.cpp:325-328 and room_renderer.cpp:513-526: the same focal/frustum.
    for(unsigned i=0;i<4;++i){
        const auto p=local[i];const float z=38.1f+v[6]+v[13]*p.x+v[14]*p.y;
        nearest=std::min(nearest,z);farthest=std::max(farthest,z);
        q.inverseDepth[i]=1/std::max(10.f,z);
        q.positions[i]={screenWidth*.5f+focal*(v[0]*p.x+v[1]*p.y+v[2]-camera.x)*q.inverseDepth[i],
                       screenHeight*.5f-focal*(v[3]*p.x+v[4]*p.y+v[5]-camera.y)*q.inverseDepth[i]};
        left=std::min(left,q.positions[i].x);right=std::max(right,q.positions[i].x);
        top=std::min(top,q.positions[i].y);bottom=std::max(bottom,q.positions[i].y);
    }
    const bool visible=!(farthest<10||nearest>1000||right<0||left>screenWidth||bottom<0||top>screenHeight);
    HKPSP_PROFILE(if(!visible)++perf::current[perf::RejectFrustum];)
    return visible;
}
bool cacheRoomQuad(const hk3ds::SpriteFrame& sprite,const hk3ds::RoomDraw& draw,bool background,
                   float splitDepth,RoomQuadCache& result) {
    hk3ds::RoomSpriteClip clip{sprite};
    if(!pspRoomClip(sprite,draw,background,clip,splitDepth))return false;
    result={};result.clip={clip.left,clip.top,clip.right,clip.bottom};
    result.homogeneousClip=clip.homogeneousClip;
    const auto& s=clip.sprite;const auto& v=draw.values;auto& q=result.world;
    uv(q,sprite);q.material=hk3ds::roomMaterial(draw);q.rgba={v[7],v[8],v[9],v[10]};
    q.blend=q.material==hk3ds::RoomMaterial::LinearDodge?Blend::ClampedAdd:Blend::Room;
    q.u0=sprite.x+clip.left*sprite.width;q.u1=sprite.x+clip.right*sprite.width;
    q.v0=sprite.y+clip.top*sprite.height;q.v1=sprite.y+clip.bottom*sprite.height;
    const std::array<Point,4> local{{{s.left,s.top},{s.right,s.top},{s.right,s.bottom},{s.left,s.bottom}}};
    float nearest=std::numeric_limits<float>::max(),farthest=-nearest;
    for(unsigned i=0;i<4;++i){
        const auto p=local[i];const float z=38.1f+v[6]+v[13]*p.x+v[14]*p.y;
        nearest=std::min(nearest,z);farthest=std::max(farthest,z);
        q.inverseDepth[i]=1/std::max(10.f,z);
        q.positions[i]={v[0]*p.x+v[1]*p.y+v[2],v[3]*p.x+v[4]*p.y+v[5]};
    }
    result.depthVisible=!(farthest<10||nearest>1000);
    return true;
}
bool projectRoomQuad(const RoomQuadCache& cached,hk3ds::Vec2 camera,float focal,Quad& q) {
    if(cached.world.rgba[3]<=0){HKPSP_PROFILE(++perf::current[perf::RejectOpacity];)return false;}
    HKPSP_PROFILE(++perf::current[perf::RoomCpuProjected];)
    q=cached.world;
    float left=std::numeric_limits<float>::max(),right=-left,top=left,bottom=-left;
    for(unsigned i=0;i<4;++i){
        const auto p=cached.world.positions[i];
        // Keep focal * (world - camera) * inverseDepth, exactly as roomQuad.
        q.positions[i]={screenWidth*.5f+focal*(p.x-camera.x)*q.inverseDepth[i],
                        screenHeight*.5f-focal*(p.y-camera.y)*q.inverseDepth[i]};
        left=std::min(left,q.positions[i].x);right=std::max(right,q.positions[i].x);
        top=std::min(top,q.positions[i].y);bottom=std::max(bottom,q.positions[i].y);
    }
    const bool visible=cached.depthVisible&&!(right<0||left>screenWidth||bottom<0||top>screenHeight);
    if(visible&&cached.homogeneousClip)throw std::runtime_error("Diagonal split-plane clipping is unsupported");
    HKPSP_PROFILE(if(!visible)++perf::current[perf::RejectFrustum];)
    return visible;
}
static void applyRoomAmbient(const DrawList& list,Quad& q,std::uint32_t owner){
    // room_renderer.cpp:601-603: unlit haze/particles retain authored tint.
    if(list.roomLitOwners&&std::binary_search(list.roomLitOwners->begin(),list.roomLitOwners->end(),owner))
        for(unsigned i=0;i<3;++i)q.rgba[i]=std::clamp(q.rgba[i]*list.roomAmbient[i],0.f,1.f);
}
void reorderRoomTail(DrawList& list,std::size_t first){
    if(!cpu::clutReorder||list.quads.size()<2)return;
    const auto last=list.quads.size()-1;
    if(first>=last)return;
    // Match the backend's C3 admission. Special materials, grade and mask/HUD
    // passes remain barriers even when their bounding rectangles are disjoint.
    const auto simple=[](const Quad& q){return q.blend!=Blend::MaskAlpha&&q.blend!=Blend::ClampedAdd&&
        q.blend!=Blend::GradeGreen&&q.blend!=Blend::GradeRed&&q.blend!=Blend::GradeBlue&&
        q.whiteFlash<=0&&q.material!=hk3ds::RoomMaterial::LinearLight&&
        q.material!=hk3ds::RoomMaterial::AmbientAdditive;};
    const auto& q=list.quads.back();if(!simple(q))return;
    struct Bounds {float left,top,right,bottom;};
    const auto bounds=[](const Quad& v){
        Bounds r{v.positions[0].x,v.positions[0].y,v.positions[0].x,v.positions[0].y};
        for(const auto p:v.positions){
            r.left=std::min(r.left,p.x);r.right=std::max(r.right,p.x);
            r.top=std::min(r.top,p.y);r.bottom=std::max(r.bottom,p.y);
        }
        return r;
    };
    const auto a=bounds(q);
    const auto begin=std::max(first,last>32?last-32:std::size_t(0));
    for(auto j=last;j>begin;){
        const auto& other=list.quads[--j];
        if(other.blend!=q.blend||!simple(other))break;
        if(sameBatchState(q,other)){
            // At most 31 Quad moves; no allocation and no whole-list search.
            std::rotate(list.quads.begin()+j+1,list.quads.end()-1,list.quads.end());
            return;
        }
        const auto b=bounds(other);
        if(!(a.right<b.left||b.right<a.left||a.bottom<b.top||b.bottom<a.top))break;
    }
}
void appendRoom(DrawList& list,const hk3ds::RoomVisualBank& bank,hk3ds::Vec2 camera,float seconds,bool behindHero,bool background,
                float (*visibility)(unsigned,void*),void* context,const hk3ds::RoomSortBoundary* split,bool afterSplit,
                const hk3ds::RoomVisualBank* particles,unsigned particleSprite,const std::vector<RoomDrawRef>* selected,
                const RoomSelection* selection) {
    // room_renderer.cpp:481-526: exported layer/order/depth order, pass split and owner alpha.
    if(selection&&(!selected||!selection->alpha||selection->first>selection->last||selection->last>selected->size()))
        throw std::invalid_argument("Invalid prepared room selection");
    std::size_t roomIndex=selection?selection->first:0,particleIndex=0;
    const auto roomCount=selection?selection->last:selected?selected->size():bank.draws.size();
    const auto roomRef=[&](){return selected?(*selected)[roomIndex]:RoomDrawRef{static_cast<unsigned>(roomIndex),nullptr};};
    const float focal=hk3ds::roomFocalLength(screenWidth,screenHeight);
    const auto particleCount=particles?particles->draws.size():0;
    HKPSP_PROFILE(const auto roomFirst=list.quads.size();)
    std::size_t groupFirst=list.quads.size();
    float layer=0,depth=0;bool haveGroup=false;
    const auto emit=[&](Quad& q,const hk3ds::RoomDraw& d){
        if(!haveGroup||layer!=d.values[11]||depth!=d.values[6]){
            groupFirst=list.quads.size();layer=d.values[11];depth=d.values[6];haveGroup=true;
        }
        q.order=list.nextOrder();list.quads.push_back(q);
        reorderRoomTail(list,groupFirst);
    };
#ifdef HKPSP_RENDER_PROBE
    // Split the coarse filter and projection phases without per-record clocks.
    // Pointers stay valid until this append returns; scratch retains its capacity.
    struct Candidate {const hk3ds::RoomDraw* draw;hk3ds::RoomSpriteClip clip;const hk3ds::SpriteFrame* sprite;float alpha;const RoomQuadCache* cached;};
    static std::vector<Candidate> filtered;
    filtered.clear();
    HKPSP_PHASE(Filter,
        while(roomIndex<roomCount||particleIndex<particleCount){
            bool particle=roomIndex==roomCount;
            if(!particle&&particleIndex<particleCount){const auto& v=particles->draws[particleIndex].values;
                particle=!hk3ds::roomDrawBefore(bank.draws[roomRef().index],v[11],v[12],v[6]);}
            const auto ref=particle?RoomDrawRef{}:roomRef();
            const auto& source=particle?particles->draws[particleIndex++]:bank.draws[ref.index];
            if(!particle)++roomIndex;
            const auto admit=[&](const hk3ds::RoomDraw& d){
                HKPSP_PROFILE(++perf::current[perf::Draws];)
                HKPSP_PROFILE(++perf::current[perf::RoomTests];)
                if(!ref.cached&&hk3ds::roomDrawBehindHero(d)!=behindHero){HKPSP_PROFILE(++perf::current[perf::RejectPass];)return;}
                if(split&&(!selection||!selection->splitSelected||particle)&&
                   hk3ds::roomDrawBefore(d,split->layer,split->order,split->depth)==afterSplit){HKPSP_PROFILE(++perf::current[perf::RejectSplit];)return;}
                const float alpha=particle?1:selection?selection->alpha->at(ref.owner):visibility?visibility(d.owner,context):1;
                if(alpha<=0){HKPSP_PROFILE(++perf::current[perf::RejectAlpha];)return;}
                if(ref.cached){filtered.push_back({&source,{},nullptr,alpha,ref.cached});return;}
                roomSprites(list,bank,d.sprite+(particle?particleSprite:0),[&](const auto& sprite){
                hk3ds::RoomSpriteClip clip{sprite};
                if(!pspRoomClip(sprite,d,background,clip,bank.blurSplitDepth)){HKPSP_PROFILE(++perf::current[perf::RejectClip];)return;}
                filtered.push_back({&source,clip,&sprite,alpha,nullptr});
                });
            };
            if(cpu::config().c1)admit(source);
            else {auto d=source;admit(d);}
        }
    );
    HKPSP_PHASE(Projection,
        for(const auto& c:filtered){
            HKPSP_PROFILE(if(!roomGeOwnerOpaque(c.alpha))++perf::current[perf::RoomAlphaFallback];)
            Quad q;if(c.cached?!projectRoomQuad(*c.cached,camera,focal,q):!roomQuad(c.clip.sprite,*c.draw,camera,seconds,q))continue;
            HKPSP_PROFILE(++perf::current[perf::RoomQuads];)
            if(!c.cached){const auto& sprite=*c.sprite;const auto& clip=c.clip;
                if(clip.homogeneousClip)throw std::runtime_error("Diagonal split-plane clipping is unsupported");
                q.u0=sprite.x+clip.left*sprite.width;q.u1=sprite.x+clip.right*sprite.width;
                q.v0=sprite.y+clip.top*sprite.height;q.v1=sprite.y+clip.bottom*sprite.height;}
            applyRoomAmbient(list,q,c.draw->owner);
            q.rgba[3]*=c.alpha;emit(q,*c.draw);
        }
    );
#else
    // Merge authored layer/order/depth streams; room precedes ambient at ties.
    while(roomIndex<roomCount||particleIndex<particleCount){
        bool particle=roomIndex==roomCount;
        if(!particle&&particleIndex<particleCount){const auto& v=particles->draws[particleIndex].values;
            particle=!hk3ds::roomDrawBefore(bank.draws[roomRef().index],v[11],v[12],v[6]);}
        const auto ref=particle?RoomDrawRef{}:roomRef();
        const auto& d=particle?particles->draws[particleIndex++]:bank.draws[ref.index];
        if(!particle)++roomIndex;
        HKPSP_PROFILE(++perf::current[perf::Draws];)
        if(!ref.cached&&hk3ds::roomDrawBehindHero(d)!=behindHero)continue;
        if(split&&(!selection||!selection->splitSelected||particle)&&
           hk3ds::roomDrawBefore(d,split->layer,split->order,split->depth)==afterSplit)continue;
        const float alpha=particle?1:selection?selection->alpha->at(ref.owner):visibility?visibility(d.owner,context):1;
        if(alpha<=0)continue;
        if(ref.cached){
            Quad q;if(!projectRoomQuad(*ref.cached,camera,focal,q))continue;
            applyRoomAmbient(list,q,d.owner);
            q.rgba[3]*=alpha;emit(q,d);continue;
        }
        roomSprites(list,bank,d.sprite+(particle?particleSprite:0),[&](const auto& sprite){hk3ds::RoomSpriteClip clip{sprite};
        if(!pspRoomClip(sprite,d,background,clip,bank.blurSplitDepth))return;
        Quad q;if(roomQuad(clip.sprite,d,camera,seconds,q)){
            if(clip.homogeneousClip)throw std::runtime_error("Diagonal split-plane clipping is unsupported");
            q.u0=sprite.x+clip.left*sprite.width;q.u1=sprite.x+clip.right*sprite.width;
            q.v0=sprite.y+clip.top*sprite.height;q.v1=sprite.y+clip.bottom*sprite.height;
            q.rgba[3]*=alpha;
            applyRoomAmbient(list,q,d.owner);
            emit(q,d);
        }
        });
    }
#endif
    HKPSP_PROFILE(if(roomFirst<list.quads.size())list.roomRanges.push_back({roomFirst,list.quads.size()});)
}
void appendHero(DrawList& list,const hk3ds::AnimationBank& bank,const hk3ds::AnimationPlayer& player,
                hk3ds::Vec2 position,hk3ds::Vec2 camera,bool facingRight,unsigned firstPage,bool presentation,float flash) {
    ActorPose pose;pose.position=position;pose.scale.x=(facingRight?-1.f:1.f)*(presentation?1.f:1.0037942f);
    pose.flash=flash;appendActor(list,bank,player,pose,camera,firstPage);
}
void appendActor(DrawList& list,const hk3ds::AnimationBank& bank,const hk3ds::AnimationPlayer& player,
                 const ActorPose& pose,hk3ds::Vec2 camera,unsigned firstPage) {
    HKPSP_PROFILE(++perf::current[perf::Draws];)
    const auto* s=player.current(bank);if(!s)return;
    if(pose.alpha<=0||pose.scale.x==0||pose.scale.y==0)return;
    const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/(38.1f+pose.depth);
    const float x=screenWidth*.5f+(pose.position.x-camera.x)*ppu;
    const float y=screenHeight*.5f-(pose.position.y-camera.y)*ppu;
    const auto layout=hk3ds::spriteLayout(*s,x,y,ppu,pose.scale.x,pose.scale.y);
    Quad q;uv(q,*s);q.page+=firstPage;
    if(pose.scale.x<0)std::swap(q.u0,q.u1);
    if(pose.scale.y<0)std::swap(q.v0,q.v1);
    const float right=layout.x+std::fabs(layout.scaleX)*s->width,bottom=layout.y+std::fabs(layout.scaleY)*s->height;
    q.positions={{{layout.x,layout.y},{right,layout.y},{right,bottom},{layout.x,bottom}}};
    if(pose.rotation!=0){
        const float c=std::cos(pose.rotation),sn=std::sin(pose.rotation);
        for(auto& p:q.positions){const float dx=p.x-x,dy=p.y-y;p={x+c*dx+sn*dy,y-sn*dx+c*dy};}
    }
    float left=screenWidth,top=screenHeight,r=0,b=0;
    for(const auto& p:q.positions){left=std::min(left,p.x);top=std::min(top,p.y);r=std::max(r,p.x);b=std::max(b,p.y);}
    if(r<=0||b<=0||left>=screenWidth||top>=screenHeight)return;
    q.rgba={pose.tint[0],pose.tint[1],pose.tint[2],pose.alpha};
    q.whiteFlash=std::clamp(pose.flash,0.f,1.f);q.material=pose.material;q.blend=pose.blend;
    q.order=list.nextOrder();list.quads.push_back(q);
}
void appendLight(DrawList& list,const hk3ds::RoomVisualBank& atlas,const hk3ds::LightingConfig& config,
                 hk3ds::Vec2 hero,hk3ds::Vec2 camera,bool facingRight,unsigned firstPage,
                 unsigned kind,float scale,std::array<float,3> rgb,float alpha) {
    // lighting.cpp:60-106,127-140: HeroLight, donut, then final vignette/borders.
    // HKL4 has world bounds, no atlas texels; PSL1 owns PSP UV rectangles.
    if(atlas.atlas.sprites.size()!=5)throw std::runtime_error("Invalid PSP light atlas");
    const float sign=facingRight?-1.f:1.f;
    float sx=sign*scale,sy=scale,depth=0;
    if(kind==0){hero.y+=config.transform[1];depth=config.transform[2];}
    else if(kind==1){hero.y+=config.transform[4];depth=config.transform[5];}
    else {const auto& t=config.glowTransform;hero.x+=sign*t[2];hero.y+=t[3];
        sx=sign*t[0];sy=t[1];depth=t[4];}
    const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/(38.1f+depth);
    const auto emit=[&](unsigned index,float left,float bottom,float right,float top){
        HKPSP_PROFILE(++perf::current[perf::Draws];)
        const auto& s=atlas.atlas.sprites.at(index);Quad q;uv(q,s);q.page+=firstPage;
        const float a=screenWidth*.5f+(hero.x-camera.x+left*sx)*ppu;
        const float b=screenWidth*.5f+(hero.x-camera.x+right*sx)*ppu;
        const float y=screenHeight*.5f-(hero.y-camera.y+top*sy)*ppu;
        const float end=screenHeight*.5f-(hero.y-camera.y+bottom*sy)*ppu;
        q.positions={{{std::min(a,b),y},{std::max(a,b),y},{std::max(a,b),end},{std::min(a,b),end}}};
        if(sx<0)std::swap(q.u0,q.u1);
        if(kind==0||index==2)q.blend=Blend::Multiply2;
        // LinearLight + Multiply2 identifies PSL1's already folded HeroLight
        // operand. Both backends decode its positive signed intercept; the
        // ordinary Multiply2 donut and raw room LinearLight stay distinct.
        if(kind==0){
            q.material=hk3ds::RoomMaterial::LinearLight;
            q.rgba={rgb[0]*alpha,rgb[1]*alpha,rgb[2]*alpha,1};
        }
        if(index==3)q.material=hk3ds::RoomMaterial::Additive;
        q.order=list.nextOrder();list.quads.push_back(q);
    };
    const auto& s=atlas.atlas.sprites.at(kind);
    emit(kind,s.left,s.bottom,s.right,s.top);
    if(kind==2)emit(3,s.left,s.bottom,s.right,s.top);
    if(kind==1)for(const auto& r:config.borders)emit(4,r.left,r.bottom,r.right,r.top);
}
void appendCameraGrade(DrawList& list){
    for(const auto blend:{Blend::GradeGreen,Blend::GradeRed,Blend::GradeBlue}){
        HKPSP_PROFILE(++perf::current[perf::Draws];)
        Quad q;q.blend=blend;q.u1=screenWidth;q.v1=screenHeight;
        q.positions={{{0,0},{screenWidth,0},{screenWidth,screenHeight},{0,screenHeight}}};
        q.order=list.nextOrder();list.quads.push_back(q);
    }
}
void checkDrawList() {
    const hk3ds::SpriteFrame s{0,3,7,20,30,-.7f,-1.1f,.8f,1.4f};
    hk3ds::RoomDraw d;d.values={-1.2f,0,2,0,.9f,3,2,1,1,1,1,0,0,0,0,0};
    const hk3ds::Vec2 camera{1,2};const float focal=hk3ds::roomFocalLength(480,272);Quad q;
#ifdef HKPSP_RENDER_PROBE
    // Standalone self-tests do not enter runGame's per-frame lens preparation.
    cpu::focal=focal;
#endif
    hk3ds::RoomFlatSprite flat;require(hk3ds::roomFlatSprite(s,d.values,1,2,480,272,focal,flat));
    require(roomQuad(s,d,camera,0,q));near(q.positions[1].x,flat.x);near(q.positions[0].x,flat.right);
    near(q.positions[0].y,flat.y);near(q.positions[2].y,flat.bottom);near(q.u0,3);near(q.u1,23);
    const float a=.37f,c=std::cos(a),sn=std::sin(a);
    d.values[0]=c;d.values[1]=-sn;d.values[3]=sn;d.values[4]=c;
    hk3ds::RoomRotatedSprite rot;require(hk3ds::roomRotatedSprite(s,d.values,1,2,480,272,focal,rot));
    require(roomQuad(s,d,camera,0,q));
    const std::array<Point,4> corners{{{-rot.centerX(),-rot.centerY()},{rot.centerX(),-rot.centerY()},
                                     {rot.centerX(),rot.centerY()},{-rot.centerX(),rot.centerY()}}};
    for(unsigned i=0;i<4;++i){const auto p=corners[i];
        near(q.positions[i].x,rot.x+p.x*std::cos(rot.angle)-p.y*std::sin(rot.angle));
        near(q.positions[i].y,rot.y+p.x*std::sin(rot.angle)+p.y*std::cos(rot.angle));}
    d.values[10]=0;require(!roomQuad(s,d,camera,0,q));d.values[10]=1;
    d.values[2]=10000;require(!roomQuad(s,d,camera,0,q));
    // A far draw on a foreground sorting layer still belongs to the pre-hero
    // background pass (main.cpp:401-411); it must not cover the hero afterwards.
    hk3ds::RoomVisualBank bank;bank.atlas.sprites.push_back(s);d.values[2]=2;
    d.values[6]=20;d.values[11]=1;bank.draws.push_back(d);DrawList list;
    appendRoom(list,bank,camera,0,false,true);require(list.quads.size()==1);
    appendRoom(list,bank,camera,0,false,false);require(list.quads.size()==1);
    // Cache parity includes clipped/reflected UVs, near/far limits, translucent
    // owners, split boundaries, particle ties and off-screen cameras.
    unsigned cacheChecks=0;
    for(unsigned kind=0;kind<8;++kind)for(unsigned variant=0;variant<9;++variant){
        auto pose=d;pose.values[15]=float(kind);pose.values[10]=variant==0?0.f:.73f;
        pose.values[0]=variant%2?-c:c;pose.values[1]=-sn;pose.values[3]=sn;pose.values[4]=c;
        pose.values[2]=2.12345f;pose.values[5]=-1.23456f;
        pose.values[6]=variant==1?-40.f:variant==2?1001.f:variant==3?-28.1f:13.71f;
        pose.values[13]=variant>=4&&variant<=6?.41f:0;
        pose.values[14]=variant==7?-.37f:0;
        auto sprite=s;sprite.x=7;sprite.y=11;sprite.width=53;sprite.height=39;
        if(variant==8)std::swap(sprite.left,sprite.right);
        hk3ds::RoomVisualBank fixture;fixture.atlas.sprites.push_back(sprite);fixture.draws.push_back(pose);
        hk3ds::RoomVisualBank particle;particle.draws.push_back(pose);
        for(unsigned pass=0;pass<4;++pass){
            RoomQuadCache cached;std::vector<RoomDrawRef> selected;
            if(hk3ds::roomDrawBehindHero(pose)==(pass%2==0)&&
               cacheRoomQuad(sprite,pose,pass<2,fixture.blurSplitDepth,cached))selected.push_back({0,&cached});
            for(const auto eye:{hk3ds::Vec2{1.125f,-2.75f},hk3ds::Vec2{-100,50},hk3ds::Vec2{1000,-500}})
                for(unsigned boundary=0;boundary<3;++boundary){
                    DrawList expected,actual;Quad prefix;prefix.order=17;expected.quads.push_back(prefix);actual=expected;
                    const hk3ds::RoomSortBoundary split{pose.values[11],pose.values[12],pose.values[6]};
                    const auto alpha=[](unsigned,void*){return .413f;};
                    appendRoom(expected,fixture,eye,2.3f,pass%2==0,pass<2,alpha,nullptr,
                               boundary?&split:nullptr,boundary==2,&particle);
                    appendRoom(actual,fixture,eye,2.3f,pass%2==0,pass<2,alpha,nullptr,
                               boundary?&split:nullptr,boundary==2,&particle,0,&selected);
                    if(expected.quads.size()!=actual.quads.size()||
                       std::memcmp(expected.quads.data(),actual.quads.data(),actual.quads.size()*sizeof(Quad)))
                        throw std::runtime_error("Static cache parity: material="+std::to_string(kind)+
                            " variant="+std::to_string(variant)+" pass="+std::to_string(pass)+
                            " boundary="+std::to_string(boundary)+" camera="+std::to_string(eye.x)+","+std::to_string(eye.y));
                    ++cacheChecks;
                }
        }
    }
    require(cacheChecks==2592);
    std::printf("Static quad cache: %u bit-identical pass checks\n",cacheChecks);
    // Keep the baked HeroLight dispatch distinct from the donut's multiply.
    bank.atlas.sprites.assign(5,s);hk3ds::LightingConfig lighting;list.quads.clear();
    appendLight(list,bank,lighting,{},camera,false,0,0,3);
    require(list.quads.size()==1&&list.quads[0].blend==Blend::Multiply2&&
            list.quads[0].material==hk3ds::RoomMaterial::LinearLight);
    list.quads.clear();appendLight(list,bank,lighting,{},camera,false,0,2,1);
    require(list.quads.size()==2&&list.quads[0].blend==Blend::Multiply2&&
            list.quads[0].material==hk3ds::RoomMaterial::Normal&&
            list.quads[1].material==hk3ds::RoomMaterial::Additive);
}
}
