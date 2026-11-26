#include "../include/platform.hpp"
#include "draw_list.hpp"
#include "room_ge.hpp"
#include "asset_path.hpp"
#include "look_grade.hpp"
#include "perf.hpp"
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace hkpsp {
namespace {
using Bytes=std::vector<unsigned char>;
// PSL1 HeroLight stores P=.5*(1-a)+a*S, with S>=.5. Byte128 is neutral.
constexpr unsigned char lightDelta(unsigned p){return p>128?2*p-256:0;}
static_assert(lightDelta(128)==0&&lightDelta(129)==2&&lightDelta(255)==254,
              "Folded light must preserve black at neutral and add on dark scenes");
void be32(Bytes& b,std::uint32_t v){for(int shift=24;shift>=0;shift-=8)b.push_back(static_cast<unsigned char>(v>>shift));}
void chunk(Bytes& png,const char* name,const Bytes& data){
    be32(png,static_cast<std::uint32_t>(data.size()));const auto first=png.size();
    png.insert(png.end(),name,name+4);png.insert(png.end(),data.begin(),data.end());
    static const auto table=[](){std::array<std::uint32_t,256> t{};
        for(unsigned i=0;i<256;++i){auto c=i;for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320u:0);t[i]=c;}return t;}();
    std::uint32_t crc=~0u;for(auto i=first;i<png.size();++i)crc=table[(crc^png[i])&255]^(crc>>8);
    be32(png,~crc);
}
[[maybe_unused]] void savePng(const std::filesystem::path& path,const Bytes& rgba){
    Bytes raw;raw.reserve((screenWidth*4+1)*screenHeight);
    for(unsigned y=0;y<screenHeight;++y){raw.push_back(0);
        raw.insert(raw.end(),rgba.begin()+y*screenWidth*4,rgba.begin()+(y+1)*screenWidth*4);}
    Bytes zlib{0x78,0x01};
    for(std::size_t pos=0;pos<raw.size();){
        const auto count=std::min<std::size_t>(65535,raw.size()-pos);zlib.push_back(pos+count==raw.size()?1:0);
        for(auto v:{static_cast<unsigned>(count),static_cast<unsigned>(count)^65535u}){
            zlib.push_back(static_cast<unsigned char>(v));zlib.push_back(static_cast<unsigned char>(v>>8));}
        zlib.insert(zlib.end(),raw.begin()+pos,raw.begin()+pos+count);pos+=count;
    }
    std::uint32_t a=1,b=0;for(auto c:raw){a=(a+c)%65521;b=(b+a)%65521;}be32(zlib,(b<<16)|a);
    Bytes png{137,80,78,71,13,10,26,10},ihdr;be32(ihdr,screenWidth);be32(ihdr,screenHeight);
    ihdr.insert(ihdr.end(),{8,6,0,0,0});chunk(png,"IHDR",ihdr);chunk(png,"IDAT",zlib);chunk(png,"IEND",{});
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));
    if(!file)throw std::runtime_error("Cannot write PNG: "+path.string());
}
struct Triangle {
    std::array<unsigned,3> ids;
    float x0,y0,ax,ay,bx,by;
    bool valid;
    Triangle(const Quad& q,std::array<unsigned,3> indices):ids(indices){
        const auto p=q.positions[ids[0]],a=q.positions[ids[1]],b=q.positions[ids[2]];
        x0=p.x;y0=p.y;const float det=(a.x-p.x)*(b.y-p.y)-(a.y-p.y)*(b.x-p.x);
        valid=std::fabs(det)>1e-8f;const float inv=valid?1/det:0;
        ax=(b.y-p.y)*inv;ay=-(b.x-p.x)*inv;bx=-(a.y-p.y)*inv;by=(a.x-p.x)*inv;
    }
    bool sample(const Quad& q,float x,float y,int originU,int originV,float& u,float& v)const{
        if(!valid)return false;
        const float a=ax*(x-x0)+ay*(y-y0),b=bx*(x-x0)+by*(y-y0),c=1-a-b;
        if(a<0||b<0||c<0)return false;
        const std::array<float,3> w{{c*q.inverseDepth[ids[0]],a*q.inverseDepth[ids[1]],b*q.inverseDepth[ids[2]]}};
        const float sum=w[0]+w[1]+w[2];if(sum<=0)return false;
        // Interpolate sprite-local texel edges. Absolute atlas coordinates lose
        // fractional precision in the weighted sum and change bilinear weights
        // after a lossless integer repack. Keep the origin out of filtering too.
        const std::array<float,4> us{{q.u0-originU,q.u1-originU,q.u1-originU,q.u0-originU}},
                                 vs{{q.v0-originV,q.v0-originV,q.v1-originV,q.v1-originV}};
        u=(w[0]*us[ids[0]]+w[1]*us[ids[1]]+w[2]*us[ids[2]])/sum;
        v=(w[0]*vs[ids[0]]+w[1]*vs[ids[1]]+w[2]*vs[ids[2]])/sum;return true;
    }
};
class Host final:public Platform {
    HostOptions options;
    using Clock=std::chrono::steady_clock;
    Clock::time_point started=Clock::now(),deadline=started;
    std::ofstream metrics;
#ifdef HKPSP_PAGE_TRACE
    std::ofstream pageTrace,rectangleTrace;
    std::ofstream pageBanks;
    unsigned pageBank=0;
    bool pageBankDirty=true;
#endif
#if defined(HKPSP_PAGE_TRACE) || defined(HKPSP_GPU_ATTRIB)
    mutable std::vector<std::string> textureNames;
#endif
    mutable std::ofstream assetTrace;
    Bytes pixels=Bytes(screenWidth*screenHeight*4);
    Bytes gradeSource;
    unsigned frame=0;
    bool pacingStarted=false;
#ifdef HKPSP_GPU_ATTRIB
    gpu::Pixels coverage{};
    std::vector<gpu::Pixels> coverageRows;
    unsigned activePass=0;
    void writeOverdraw(const DrawList& list){
        if(coverageRows.size()!=gpu::frames)throw std::runtime_error("Incomplete overdraw tape");
        const auto open=[&](const char* name){
            std::ofstream file(std::filesystem::path(options.output)/name);
            file.exceptions(std::ios::badbit|std::ios::failbit);return file;
        };
        auto csv=open("gpu-overdraw.csv");csv<<"frame";
        for(const auto* p:gpu::passNames)csv<<','<<p<<"_pixels";
        csv<<'\n';gpu::Pixels sums{};
        for(unsigned f=0;f<coverageRows.size();++f){csv<<f;
            for(unsigned p=0;p<gpu::passCount;++p){csv<<','<<coverageRows[f][p];sums[p]+=coverageRows[f][p];}
            csv<<'\n';
        }
        csv.close();
        auto summary=open("gpu-overdraw-summary.csv");
        summary<<"config,pass,mean_pixels,mean_screen_units\n"<<std::fixed<<std::setprecision(9);
        // Geometry is identical for filtering/residency/CLUT variants. Removal
        // configs omit entire tagged groups; no depth/occlusion rejection exists.
        for(const auto& c:gpu::configs)for(unsigned p=0;p<gpu::passCount;++p){
            double mean=(c.passes&(1u<<p))?double(sums[p])/gpu::frames:0;
            if(p==unsigned(gpu::Pass::Grade)&&c.grade==gpu::Grade::CopyOnly)mean=0;
            if(p==unsigned(gpu::Pass::Grade)&&(c.grade==gpu::Grade::OneSweep||c.sweep==gpu::Sweep::Clut8))mean/=3;
            summary<<c.name<<','<<gpu::passNames[p]<<','<<mean<<','<<mean/(screenWidth*screenHeight)<<'\n';
        }
        summary.close();
        if(textureNames.size()!=list.textures->size())throw std::runtime_error("Texture page manifest mismatch");
        auto pages=open("gpu-pages.csv");pages<<"page,logical_name,width,height,bits\n";
        for(unsigned p=0;p<textureNames.size();++p){const auto& t=list.textures->at(p);
            pages<<p<<','<<textureNames[p]<<','<<t.width<<','<<t.height<<','<<t.bits<<'\n';}
        pages.close();
    }
#endif
    void raster(const Quad& q,const Texture& t,const hk3ds::cameraColor::Config& grade){
        if(q.blend==Blend::FadeBlack){
            const auto opacity=std::clamp(q.rgba[3],0.f,1.f);
            for(std::size_t i=0;i<pixels.size();i+=4)for(unsigned c=0;c<3;++c)
                pixels[i+c]=static_cast<unsigned char>(std::lround(pixels[i+c]*(1-opacity)));
#ifdef HKPSP_GPU_ATTRIB
            coverage[activePass]+=screenWidth*screenHeight;
#endif
            return;
        }
        if(q.blend==Blend::MaskAlpha){
            const auto a=q.positions[0],b=q.positions[2];
            for(int y=int(a.y);y<int(b.y);++y)for(int x=int(a.x);x<int(b.x);++x){
                if(x<0||y<0||x>=int(screenWidth)||y>=int(screenHeight))throw std::runtime_error("Invalid alpha span");
#ifdef HKPSP_GPU_ATTRIB
                ++coverage[activePass];
#endif
                pixels[(y*screenWidth+x)*4+3]=static_cast<unsigned char>(std::lround(q.rgba[3]*255));
            }
            return;
        }
        if(cameraGrade(q.blend)){
#ifdef HKPSP_GPU_ATTRIB
            coverage[activePass]+=screenWidth*screenHeight;
#endif
            if(q.blend==Blend::GradeGreen)gradeSource=pixels;
            if(gradeSource.size()!=pixels.size())throw std::runtime_error("Camera grade requires green first");
            const auto palette=cameraPalette(q.blend,grade);
            const unsigned channel=q.blend==Blend::GradeGreen?1:q.blend==Blend::GradeRed?0:2;
            for(std::size_t i=0;i<pixels.size();i+=4)for(unsigned c=0;c<3;++c){
                const unsigned value=palette[gradeSource[i+channel]][c];
                pixels[i+c]=static_cast<unsigned char>(q.blend==Blend::GradeGreen?value:std::min(255u,pixels[i+c]+value));
            }
            return;
        }
        const bool linear=q.material==hk3ds::RoomMaterial::LinearLight;
        const auto& original=t.paletteFor(q.clut);
        const bool foldedLight=linear&&q.blend==Blend::Multiply2;
        auto neutral=linear?linearLightPalette(t,q):Palette{};
        if(foldedLight)for(unsigned i=0;i<256;++i){
            for(unsigned c=0;c<3;++c)neutral[i][c]=lightDelta(original[i][c]);
            neutral[i][3]=255;
        }
        float left=q.positions[0].x,right=left,top=q.positions[0].y,bottom=top;
        for(const auto p:q.positions){left=std::min(left,p.x);right=std::max(right,p.x);top=std::min(top,p.y);bottom=std::max(bottom,p.y);}
        const int x0=static_cast<int>(std::clamp(std::floor(left),0.f,float(screenWidth)));
        const int y0=static_cast<int>(std::clamp(std::floor(top),0.f,float(screenHeight)));
        const int x1=static_cast<int>(std::clamp(std::ceil(right),0.f,float(screenWidth)));
        const int y1=static_cast<int>(std::clamp(std::ceil(bottom),0.f,float(screenHeight)));
        const Triangle first(q,{0,1,2}),second(q,{0,2,3});
        const int minU=std::max(0,int(std::floor(std::min(q.u0,q.u1))));
        const int maxU=std::min(int(t.width)-1,int(std::ceil(std::max(q.u0,q.u1)))-1);
        const int minV=std::max(0,int(std::floor(std::min(q.v0,q.v1))));
        const int maxV=std::min(int(t.height)-1,int(std::ceil(std::max(q.v0,q.v1)))-1);
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
            float u=0,v=0;
            if(!first.sample(q,x+.5f,y+.5f,minU,minV,u,v)&&!second.sample(q,x+.5f,y+.5f,minU,minV,u,v))continue;
#ifdef HKPSP_GPU_ATTRIB
            // Covered pixel centres, before texture alpha rejection. Count every
            // submitted quad, including alpha/stencil work and all grade sweeps.
            ++coverage[activePass];
#endif
            // UVs describe texel edges; centres are n+.5, as on the GE. Do not
            // shift quad UVs: subtract the half texel only when finding taps.
            // Clamp taps to this sprite, equivalent to its replicated 1px border.
            const float su=u-.5f,sv=v-.5f,fu=std::floor(su),fv=std::floor(sv);
            const float wx=su-fu,wy=sv-fv;
            std::array<float,4> color{};
            for(int j=0;j<2;++j)for(int i=0;i<2;++i){
                const int tx=std::clamp(minU+int(fu)+i,minU,maxU),ty=std::clamp(minV+int(fv)+j,minV,maxV);
                const auto& tap=(linear?neutral:original)[t.indices[ty*t.width+tx]];
                const float weight=(i?wx:1-wx)*(j?wy:1-wy);
                for(unsigned c=0;c<4;++c)color[c]+=tap[c]*weight;
            }
            const bool ambient=q.material==hk3ds::RoomMaterial::AmbientAdditive;
            const auto offset=(y*screenWidth+x)*4;
            const float alpha=q.blend==Blend::MaskedLiquid?pixels[offset+3]/255.f:
                std::min(1.f,color[3]/255.f*q.rgba[3]*(ambient?2:1));
            if(alpha<=0)continue;
            for(unsigned channel=0;channel<3;++channel){
                const float texel=color[channel]/255.f;
                const float s=std::min(1.f,(texel+(1-texel)*q.whiteFlash)*(linear&&!foldedLight?1:q.rgba[channel])*(ambient?2:1)),d=pixels[offset+channel]/255.f;
                float value;
                // Decode before filtering, matching the uploaded GE operand CLUT.
                // This preserves D+2P-1; 2PD loses the light on dark backgrounds.
                if(q.blend==Blend::ClampedAdd)value=d+(std::min(1.f,d+s)-d)*alpha;
                else if(foldedLight)value=d+s;
                else if(q.blend==Blend::Multiply2||linear)value=2*s*d; // GU ADD, DST_COLOR, SRC_COLOR.
                else if(q.material==hk3ds::RoomMaterial::Screen||q.material==hk3ds::RoomMaterial::DarknessCutout)
                    value=d+s*(q.blend==Blend::ScreenPremult?1:alpha)*(1-d);
                else if(q.material==hk3ds::RoomMaterial::Additive||q.material==hk3ds::RoomMaterial::LinearDodge||ambient)value=d+s*alpha;
                // Original LinearLight: lerp(D,clamp(D+2*S-1),A). Approximation:
                // 2*D*(.5+(S-.5)*A), with neutral alpha folded into the CLUT.
                // Untagged LinearDodge retains the raw additive API. Exported
                // source draws select ClampedAdd above: lerp(D,clamp(D+S),A).
                // The legacy approximation, unlike lerp(D,clamp(D+S),A),
                // it clamps AFTER blending and can saturate translucent cores.
                // VividLight still uses Normal outside this scope.
                else value=s*alpha+d*(1-alpha);
                pixels[offset+channel]=static_cast<unsigned char>(std::clamp(std::lround(value*255),0l,255l));
            }
        }
    }
public:
    explicit Host(const HostOptions& opts):options(opts){
#ifdef HKPSP_GPU_ATTRIB
        if(options.frames!=gpu::frames)throw std::runtime_error("GPU oracle requires 150 frames");
#endif
        std::filesystem::create_directories(options.output);
        metrics.open(std::filesystem::path(options.output)/"metrics.csv");
        if(!metrics)throw std::runtime_error("Cannot write metrics.csv");
        assetTrace.open(std::filesystem::path(options.output)/"assets.tsv");
        if(!assetTrace)throw std::runtime_error("Cannot write assets.tsv");
        metrics<<"frame,hero_x,hero_y,draws,quads,camera_x,camera_y,control_steps,velocity_x,velocity_y,actor,phase,health,soul,geo,scene,simulation_x,simulation_y,ability_phase,transition_fade\n"<<std::setprecision(9);
#ifdef HKPSP_PAGE_TRACE
        pageTrace.open(std::filesystem::path(options.output)/"page-trace.csv");
        pageTrace<<"frame,pages,bank,scene\n";
        if(!pageTrace)throw std::runtime_error("Cannot write page trace");
        pageBanks.open(std::filesystem::path(options.output)/"page-banks.csv");
        pageBanks<<"bank,scene,page,asset,bytes\n";
        if(!pageBanks)throw std::runtime_error("Cannot write page banks");
        rectangleTrace.open(std::filesystem::path(options.output)/"sprite-trace.csv");
        rectangleTrace<<"frame,page,u0,v0,u1,v1\n"<<std::setprecision(9);
        if(!rectangleTrace)throw std::runtime_error("Cannot write sprite trace");
#endif
    }
    hk3ds::Input poll()override{return {};}
    bool menusEnabled()const override{return options.menus&&!options.fixedPose;}
    std::string audioOutput()const override{return options.output;}
    std::string path(const std::string& name)const override{
        namespace fs=std::filesystem;
        if(!validAssetName(name))throw std::runtime_error("Invalid logical asset name");
        // Optional test input is supplied beside the converted assets, never upstream.
        // It is not an asset dependency and must not enter the staging trace.
        if(name=="residency-tape.hkir")return (fs::path(options.assets)/name).string();
        // Architecture boundary: original logical names select PSP texel tables
        // and regenerated sidecars here; unchanged metadata stays in the 3DS tree.
        const fs::path assets=options.assets;
        const auto resolved=[&]()->fs::path{
            if(!options.data.empty())return fs::path(options.data)/name;
            if(name=="menu-fill.hpt"||name=="title-background.hpt"||name=="title-logo.hpt"||
               name=="title-mote.hpt")return assets/"menus"/name;
            if(name=="rooms/Town/town-lift-visual.bin")return assets/"town-lift/town-lift.psp.bin";
            if(name.rfind("rooms/Town/town-lift-page-",0)==0)return assets/"town-lift"/("page-"+name.substr(26));
            if(name=="rooms/Crossroads_06/shaman-pillar-visual.bin")return assets/"shaman-pillar/shaman-pillar.psp.bin";
            if(name.rfind("rooms/Crossroads_49",0)==0&&name.find("/city-lift-art-visual.bin")!=std::string::npos)return assets/"city-lift-art/city-lift-art.psp.bin";
            if(name.rfind("rooms/Crossroads_49",0)==0&&name.find("/city-lift-art-page-")!=std::string::npos)return assets/"city-lift-art"/("page-"+name.substr(name.find("/city-lift-art-page-")+sizeof("/city-lift-art-page-")-1));
            constexpr char pillarPage[]="rooms/Crossroads_06/shaman-pillar-page-";
            if(name.rfind(pillarPage,0)==0)return assets/"shaman-pillar"/("page-"+name.substr(sizeof(pillarPage)-1));
            if(name=="rooms/Fungus2_04/mantis-gate-visual.bin")return assets/"mantis-gate/mantis-gate.psp.bin";
            constexpr char mantisPage[]="rooms/Fungus2_04/mantis-gate-page-";
            if(name.rfind(mantisPage,0)==0)return assets/"mantis-gate"/("page-"+name.substr(sizeof(mantisPage)-1));
            if(name=="rooms/Ruins1_05b/waterways-art-visual.bin")return assets/"waterways-art/waterways-art.psp.bin";
            constexpr char waterwaysPage[]="rooms/Ruins1_05b/waterways-art-page-";
            if(name.rfind(waterwaysPage,0)==0)return assets/"waterways-art"/("page-"+name.substr(sizeof(waterwaysPage)-1));
            // Toll machine art by scene (room_stage.py ROUTE_TOLL_ART, RTS4 kind 1 city, 2 mines, 3 greenpath).
            const std::array<std::pair<const char*,const char*>,3> tollArt{{
                {"Crossroads_49b","anim-libraries/route-tolls/city-toll-actor-art/city-toll-actor-art.psp.bin"},
                {"Mines_33","anim-libraries/route-tolls/mines-toll-actor-art/mines-toll-actor-art.psp.bin"},
                {"Fungus1_31","anim-libraries/greenpath-tolls/greenpath-toll-actor-art/greenpath-toll-actor-art.psp.bin"}}};
            for(const auto& toll:tollArt){
                const std::string directory=std::string("rooms/")+toll.first+"/toll-";
                if(name==directory+"visual.bin")return assets/toll.second;
                if(name==directory+"page-0.hpt")return (assets/toll.second).parent_path()/"page-0.hpt";
                if(name==directory+"page-1.hpt")return (assets/toll.second).parent_path()/"page-1.hpt";
            }
            if(name=="rooms/Room_shop/sly-actor-visual.bin")
                return assets/"anim-libraries/sly-shop/sly-shop-actor-art/sly-shop-actor-art.psp.bin";
            if(name=="rooms/Room_shop/sly-actor-page-0.hpt")
                return assets/"anim-libraries/sly-shop/sly-shop-actor-art/page-0.hpt";
            if(name=="rooms/Room_shop/sly-icons-visual.bin")return assets/"sly-shop/sly-shop-icons.psp.bin";
            if(name=="rooms/Room_shop/sly-icons-page-0.hpt")return assets/"sly-shop/page-0.hpt";
            if(name=="rooms/Room_mapper/sly-icons-visual.bin")return assets/"sly-shop/sly-shop-icons.psp.bin";
            if(name=="rooms/Room_mapper/sly-icons-page-0.hpt")return assets/"sly-shop/page-0.hpt";
            if(name=="rooms/Fungus2_23/bretta-quest-visual.bin")
                return assets/"anim-libraries/bretta-quest-art/bretta-quest-art/bretta-quest-art.psp.bin";
            if(name=="rooms/Fungus2_23/bretta-quest-page-0.hpt")
                return assets/"anim-libraries/bretta-quest-art/bretta-quest-art/page-0.hpt";
            // Scene-enemy render tables: repacked PSA1 banks from anim-libraries (room_stage.py stages them).
            if(name.rfind("rooms/scene-enemy/",0)==0){
                const std::array<std::pair<const char*,const char*>,9> sceneEnemy{{
                    {"tiktik","crossroads-enemies-a/tiktik-anim"},{"gruzzer","crossroads-enemies-a/gruzzer-anim"},
                    {"vengefly","crossroads-enemies-a/vengefly-anim"},{"aspid-hatchling","crossroads-enemies-a/aspid-hatchling-anim"},
                    {"enemy-b","enemy-b/enemy-b-anim"},{"enemy-c","enemy-c/enemy-c-scene"},
                    {"greenpath","greenpath-enemies/greenpath-scene"},{"elder-baldur","elder-baldur/elder-baldur-art"},
                    {"mantis","mantis-lords/mantis-art"}}};
                for(const auto& e:sceneEnemy){
                    const std::string stem=e.first;
                    const std::string pagePrefix="rooms/scene-enemy/"+stem+"-page-";
                    const std::string table=stem+(stem=="greenpath"||stem=="elder-baldur"||stem=="mantis"?"-art":"-anim");
                    if(name=="rooms/scene-enemy/"+table+".bin")return assets/"anim-libraries"/e.second/(table+".psp.bin");
                    if(name.rfind(pagePrefix,0)==0)return assets/"anim-libraries"/e.second/("page-"+name.substr(pagePrefix.size()));
                }
                throw std::runtime_error("Unknown scene enemy render asset");
            }
            if(name.rfind("rooms/",0)==0){
                const auto separator=name.find('/',6);
                if(separator==std::string::npos)throw std::runtime_error("Invalid room asset name");
                const auto leaf=name.substr(separator+1);
                const auto file=leaf=="static-room-visual.bin"?"room-visual.psp.bin":
                    leaf.rfind("static-room-page-",0)==0?leaf.substr(12):leaf;
                return assets/name.substr(6,separator-6)/file;
            }
            if(name.rfind("shared-page-",0)==0)return assets/"shared"/name.substr(7);
            if(name=="perf-tape.hkir")return fs::path(options.output)/name;
            if(name.rfind("audio-",0)==0)return assets/"audio"/name;
            const std::array<std::array<const char*,3>,8> actors{{
                {{"crawler-anim.bin","crawler","crawler"}},{{"buzzer-anim.bin","buzzer","buzzer"}},
                {{"shade-anim.bin","shade","shade"}},{{"geo-visual.bin","geo","geo"}},
                {{"great-door-visual.bin","great-door","great-door"}},{{"fury-visual.bin","fury","fury"}},
                {{"decor-fx-visual.bin","decor","decor-fx"}},{{"events-anim.bin","events","events"}}
            }};
            for(const auto& a:actors){
                if(name==a[0])return assets/a[1]/(name.substr(0,name.size()-4)+".psp.bin");
                const std::string prefix=std::string(a[2])+"-page-";
                if(name.rfind(prefix,0)==0)return assets/a[1]/name.substr(prefix.size()-5);
            }
            if(name=="decor-fx-source.bin")return fs::path(options.source)/"_private/converted/hero/romfs/decor-fx-visual.bin";
            if(name=="hero.bin")return assets/"hero/hero.psp.bin";
            if(name=="motes.bin")return assets/"motes/motes.psp.bin";
            if(name=="hud-visual.bin")return assets/"hud/hud-visual.psp.bin";
            if(name=="region-title.bin"||name.rfind("region-title-page-",0)==0)return assets/"hud"/name;
            if(name=="inventory.bin")return assets/"inventory/inventory.psp.bin";
            if(name=="map-page-0.hpt"||name=="map-regions.hpt")return assets/"map"/name;
            if(name.rfind("inventory-page-",0)==0)return assets/"inventory"/name.substr(10);
            if(name.rfind("hud-page-",0)==0)return assets/"hud"/name.substr(4);
            if(name=="fonts.bin")return assets/"fonts/fonts.bin";
            if(name=="text.bin"||name=="format.txt")return assets/"text"/name;
            if(name=="ability-panel.bin")return assets/"ability/ability-panel.psp.bin";
            if(name.rfind("ability-page-",0)==0)return assets/"ability"/name.substr(8);
            if(name=="spirit.bin")return assets/"spirit/spirit.psp.bin";
            if(name.rfind("spirit-page-",0)==0)return assets/"spirit"/name.substr(7);
            if(name.rfind("font-page-",0)==0)return assets/"fonts"/name.substr(5);
            if(name.rfind("mote-page-",0)==0)return assets/"motes"/name.substr(5);
            if(name=="lighting.bin")return fs::path(options.source)/"_private/converted/hero-light/lighting.bin";
            if(name=="lighting-atlas.bin")return assets/"hero-light/lighting.psp.bin";
            if(name=="lantern-glow.bin"||name=="lantern-glow.hpt")return assets/"hero-light"/name;
            if(name.rfind("light-page-",0)==0)return assets/"hero-light"/name.substr(6);
            if(name=="static-room-visual.bin")return assets/"Tutorial_01/room-visual.psp.bin";
            if(name=="static-plants.bin"||name=="static-room-dynamics.bin"||
               name=="static-room-ambient.bin"||name=="static-grass.bin")return assets/"Tutorial_01"/name;
            if(name.rfind("static-room-page-",0)==0)return assets/"Tutorial_01"/name.substr(12);
            if(name.rfind("page-",0)==0)return assets/"hero"/name;
            const fs::path converted=fs::path(options.source)/"_private/converted";
            if(name.rfind("static-",0)==0)return converted/"tutorial-static"/name;
            return converted/"hero/romfs"/name;
        }();
        // gpu-parity.cfg is an optional PSP-side probe (test-only marker that
        // the PSP data copy carries). It is not a converted asset, so it must not
        // enter the staging trace, where a missing optional file would fail staging.
        if(name!="gpu-parity.cfg"){assetTrace<<name<<'\t'<<fs::absolute(resolved).string()<<'\n';assetTrace.flush();}
#if defined(HKPSP_PAGE_TRACE) || defined(HKPSP_GPU_ATTRIB)
        if(name.size()>=4&&name.substr(name.size()-4)==".hpt")textureNames.push_back(name);
#endif
        if(!assetTrace)throw std::runtime_error("Asset trace write failed");
        return resolved.string();
    }
    std::string sceneRoot()const override{
        if(!options.data.empty())return options.data;
        return (std::filesystem::path(options.source)/"_private/converted/hero/romfs").string();
    }
    std::string inputReplay()const override{return options.replay;}
    std::string uiReplay()const override{return options.uiReplay;}
    std::string savePath()const override{return options.save;}
#ifdef HKPSP_PAGE_TRACE
    void releaseRoomAssets(unsigned retainedPages)override{
        if(retainedPages>textureNames.size())throw std::runtime_error("Invalid retained page count");
        textureNames.resize(retainedPages);++pageBank;pageBankDirty=true;
    }
#endif
    std::string sceneEnemyRoot()const override{
        if(!options.data.empty())return options.data;
        return (std::filesystem::path(options.assets)/"scene-enemies").string();
    }
    double seconds()const override{return std::chrono::duration<double>(Clock::now()-started).count();}
    const RenderPose* renderPose()const override{return options.fixedPose?&options.pose:nullptr;}
    bool roomGeEnabled()const override{return options.roomGe;}
    void beginFrame()override{
#ifdef HKPSP_GPU_ATTRIB
        coverage={};
#endif
        if(!pacingStarted){deadline=Clock::now();pacingStarted=true;}
        std::fill(pixels.begin(),pixels.end(),0);for(std::size_t i=3;i<pixels.size();i+=4)pixels[i]=255;
    }
    void submit(const DrawList& sourceList)override{
        DrawList expanded;
        if(sourceList.roomGe())expandRoomGe(sourceList,expanded);
        const auto& list=sourceList.roomGe()?expanded:sourceList;
        if(!list.textures)throw std::runtime_error("Draw list has no texture bank");
        for(const auto& q:list.quads){
            if(!samplesTexture(q.blend))continue;
            const auto& texture=list.textures->at(q.page);
            if(!texture.width||!texture.height)throw std::runtime_error("Vacated texture page "+std::to_string(q.page)+
                " in "+list.info.scene+" frame "+std::to_string(list.info.frame));
        }
#ifdef HKPSP_PAGE_TRACE
        if(pageBankDirty){
            if(textureNames.size()!=list.textures->size())throw std::runtime_error("Texture page manifest mismatch");
            for(unsigned p=0;p<list.textures->size();++p){
                const auto& t=list.textures->at(p);
                if(t.width&&t.height)pageBanks<<pageBank<<','<<list.info.scene<<','<<p<<','<<textureNames[p]<<','
                    <<t.width*t.height*t.bits/8+(t.bits==4?64:3072)<<'\n';
            }
            if(!pageBanks)throw std::runtime_error("Page bank trace write failed");
            pageBankDirty=false;
        }
        if(list.info.frame==0){
            std::ofstream sizes(std::filesystem::path(options.output)/"page-sizes.csv");
            sizes<<"page,bytes\n";
            for(unsigned p=0;p<list.textures->size();++p){
                const auto& t=list.textures->at(p);
                sizes<<p<<','<<t.width*t.height*t.bits/8+(t.bits==4?64:3072)<<'\n';
            }
            if(!sizes)throw std::runtime_error("Page size trace write failed");
        }
        std::vector<bool> seen(list.textures->size(),false);
        pageTrace<<list.info.frame<<',';
        for(const auto& q:list.quads){
            // Match PSP preparePages: these passes do not sample an asset page.
            if(!samplesTexture(q.blend))continue;
            rectangleTrace<<list.info.frame<<','<<q.page<<','<<q.u0<<','<<q.v0<<','<<q.u1<<','<<q.v1<<'\n';
            if(seen.at(q.page))continue;
            seen[q.page]=true;pageTrace<<q.page<<';';
        }
        pageTrace<<','<<pageBank<<','<<list.info.scene<<'\n';
        if(!pageTrace)throw std::runtime_error("Page trace write failed");
        if(!rectangleTrace)throw std::runtime_error("Sprite trace write failed");
#endif
#ifdef HKPSP_CPU_ATTRIB
        static std::vector<std::vector<Quad>> tape;
        const auto f=list.info.frame;
        if(cpu::configIndex==0){
            if(f!=tape.size())throw std::runtime_error("Quad tape frame order");
            tape.push_back(list.quads);
        }else if(f>=tape.size()||tape[f].size()!=list.quads.size()||
            std::memcmp(tape[f].data(),list.quads.data(),list.quads.size()*sizeof(Quad)))
            throw std::runtime_error("CPU switch quad mismatch at frame "+std::to_string(f));
        if(f+1==options.frames){
            std::ofstream report(std::filesystem::path(options.output)/"quad-parity.txt",std::ios::app);
            report<<cpu::config().name<<": "<<options.frames<<" frames, bit-identical quads\n";
            if(!report)throw std::runtime_error("Quad parity report write failed");
        }
        return;
#endif
#ifndef HKPSP_PAGE_TRACE
        const bool capture=list.info.frame%options.every==0||list.info.frame+1==options.frames;
        if(options.raster&&(!options.capturesOnly||capture))for(const auto& q:list.quads){
#ifdef HKPSP_GPU_ATTRIB
            activePass=unsigned(list.gpuPasses.at(&q-list.quads.data()));
#endif
            static const Texture untextured;
            raster(q,samplesTexture(q.blend)?list.textures->at(q.page):untextured,list.cameraColor);
        }
#endif
        const auto& i=list.info;frame=i.frame;
        metrics<<i.frame<<','<<i.heroX<<','<<i.heroY<<','<<list.quads.size()<<','<<list.quads.size()<<','
               <<i.cameraX<<','<<i.cameraY<<','<<i.controlSteps<<','<<i.velocityX<<','<<i.velocityY<<','
               <<i.actor<<','<<i.phase<<','<<i.health<<','<<i.soul<<','<<i.geo<<','<<i.scene<<','<<i.simulationX<<','<<i.simulationY<<','<<i.abilityPhase<<','<<i.transitionFade<<'\n';
        if(!metrics)throw std::runtime_error("Metrics write failed");
#ifdef HKPSP_GPU_ATTRIB
        coverageRows.push_back(coverage);
        if(frame+1==options.frames)writeOverdraw(list);
#endif
#ifndef HKPSP_PAGE_TRACE
        if(options.raster&&(frame%options.every==0||frame+1==options.frames)){std::ostringstream name;name<<"frame-"<<std::setw(4)<<std::setfill('0')<<frame<<".png";
            savePng(std::filesystem::path(options.output)/name.str(),pixels);}
#endif
    }
    void endFrame()override{
        if(options.realtime){deadline+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/30));
            std::this_thread::sleep_until(deadline);}
    }
};
}
std::unique_ptr<Platform> makeHost(const HostOptions& options){return std::make_unique<Host>(options);}
}
