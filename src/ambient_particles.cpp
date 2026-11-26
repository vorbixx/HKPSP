#include "ambient_particles.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds
{
namespace
{
bool readU32(FILE* f,unsigned& v){return std::fread(&v,sizeof(v),1,f)==1;}
bool readI32(FILE* f,int& v){static_assert(sizeof(int)==4);return std::fread(&v,sizeof(v),1,f)==1;}
bool readFloats(FILE* f,float* v,unsigned count){return std::fread(v,sizeof(float),count,f)==count;}
bool finiteRange(float a,float b){return std::isfinite(a)&&std::isfinite(b)&&a<=b;}
Vec2 rotateXY(const std::array<float,4>& q,Vec2 v,float z,float& outZ)
{
    const float x=q[0],y=q[1],w=q[3];
    const float tx=2*(y*z-q[2]*v.y),ty=2*(q[2]*v.x-x*z),tz=2*(x*v.y-y*v.x);
    const float rx=v.x+w*tx+(y*tz-q[2]*ty);
    const float ry=v.y+w*ty+(q[2]*tx-x*tz);
    outZ=z+w*tz+(x*ty-y*tx);
    return {rx,ry};
}
}

bool AmbientParticles::load(const char* path,unsigned spriteOverride)
{
    if(!path)return false;
    const bool preserveAtlas=bound_;
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    char magic[4]{};unsigned version=0,emitters=0,total=0,sprite=0,edges=0;
    bool ok=std::fread(magic,1,4,f)==4&&std::equal(magic,magic+4,"HAP1")&&
        readU32(f,version)&&readU32(f,emitters)&&readU32(f,total)&&readU32(f,sprite)&&
        readU32(f,edges)&&(version>=2&&version<=5)&&emitters<=emitterCount&&total<=capacity;
    if(ok&&emitters==0){
        ok=total==0&&edges==0&&std::fgetc(f)==EOF;
        std::fclose(f);if(ok)clear();return ok;
    }
    // The two historical v3 layouts have disjoint exact lengths. A malformed
    // bank is rejected before mutating the current bank or its atlas binding.
    const long position=std::ftell(f);
    ok=ok&&position==24&&std::fseek(f,0,SEEK_END)==0;
    const long bytes=ok?std::ftell(f):-1;
    ok=ok&&std::fseek(f,position,SEEK_SET)==0&&emitters>0&&total>0&&
        (edges==edgeCount||(version==5&&edges==0));
    const bool transformedBank=version==3&&bytes==24+long(emitters)*620+long(edges)*20;
    const long stride=version==2?556:version==3?(transformedBank?620:560):version==4?572:576;
    ok=ok&&bytes==24+long(emitters)*stride+long(edges)*20;
    auto next=std::array<Emitter,emitterCount>{};
    auto nextEdges=std::array<Edge,edgeCount>{};
    for(unsigned i=0;ok&&i<emitters;++i){
        auto& e=next[i];
        if(transformedBank){
            unsigned shape=0,material=0;float extra[13]{};
            ok=readU32(f,shape)&&readU32(f,e.sprite)&&readU32(f,material)&&readFloats(f,extra,13);
            e.shape=shape;e.material=material==4?7:(material==1?1:0);e.alphaGain=extra[12];
            e.halfExtent={extra[0],extra[2]};
            e.transformedShape=true;
            std::copy(extra,extra+3,e.shapeHalf.begin());
            std::copy(extra+3,extra+12,e.shapeBasis.begin());
            ok=ok&&std::isfinite(e.alphaGain)&&e.alphaGain>0&&e.alphaGain<=4&&
                (shape==5||shape==6||shape==12)&&(material==0||material==1||material==4);
            for(float half:e.shapeHalf)ok=ok&&std::isfinite(half)&&half>=0;
            for(float value:e.shapeBasis)ok=ok&&std::isfinite(value);
        } else {
            e.sprite=sprite;e.material=unsigned(RoomMaterial::AmbientAdditive);e.alphaGain=1;e.shape=6;
            if(version>=3)ok=readU32(f,e.sprite);
        }
        int sourceLayer=0;
        ok=ok&&readU32(f,e.count)&&readI32(f,sourceLayer)&&readI32(f,e.order)&&readU32(f,e.flags);
        // Repair the verified legacy foreground ID before ordinal validation.
        if(version==2&&sourceLayer==-1717784197)sourceLayer=11;
        // v3 stores fixed random texture-sheet rows in flags 8..15.
        ok=ok&&!(e.flags&~(transformedBank?0xff07u:version==5?0x1ffu:0xffu))&&sourceLayer>=0&&sourceLayer<=255;
        ok=ok&&(edges||((e.flags&128u)&&!(e.flags&32u)))&&(!(e.flags&256u)||(e.flags&128u));
        e.frames=transformedBank?1+((e.flags>>8)&255u):1;
        if(!transformedBank)e.material=(e.flags&16u)?0:(e.flags&8u)?1:unsigned(RoomMaterial::AmbientAdditive);
        (void)sourceLayer;
        e.layer=sourceLayer; // Room renderer sorts selected particles by depth.
        float base[37]{};
        ok=ok&&readFloats(f,base,37);
        if(!ok)break;
        unsigned n=0;
        e.position={base[n],base[n+1]};e.z=base[n+2];n+=3;
        for(float& q:e.rotation)q=base[n++];
        e.scaleX=base[n++];e.scaleY=base[n++];e.scaleZ=base[n++];
        if(!transformedBank){e.halfExtent={base[n++],base[n++]};}else n+=2;
        e.shapeAngle=base[n++];
        e.rate=base[n++];float rateMax=base[n++];e.rate=(e.rate+rateMax)*.5f;
        e.lifeMin=base[n++];e.lifeMax=base[n++];
        e.sizeMin=base[n++];e.sizeMax=base[n++];
        e.speedMin=base[n++];e.speedMax=base[n++];
        e.gravityMin=base[n++];e.gravityMax=base[n++];
        e.velocityMin={base[n],base[n+2]};e.velocityZMin=base[n+4];
        e.velocityMax={base[n+1],base[n+3]};e.velocityZMax=base[n+5];n+=6;
        for(float& c:e.colorMin)c=base[n++];
        for(float& c:e.colorMax)c=base[n++];
        ok=e.count>0&&e.count<=capacity&&finiteRange(e.rate,rateMax)&&e.rate>0&&
            finiteRange(e.lifeMin,e.lifeMax)&&e.lifeMin>0&&std::isfinite(e.sizeMin)&&std::isfinite(e.sizeMax)&&
            finiteRange(e.speedMin,e.speedMax)&&finiteRange(e.gravityMin,e.gravityMax)&&
            finiteRange(e.velocityMin.x,e.velocityMax.x)&&finiteRange(e.velocityMin.y,e.velocityMax.y)&&
            finiteRange(e.velocityZMin,e.velocityZMax)&&std::isfinite(e.position.x)&&std::isfinite(e.position.y)&&
            std::isfinite(e.z)&&std::isfinite(e.scaleX)&&e.scaleX!=0&&std::isfinite(e.scaleY)&&
            std::isfinite(e.scaleZ)&&e.scaleZ!=0&&
            std::isfinite(e.shapeAngle)&&std::isfinite(e.halfExtent.x)&&std::isfinite(e.halfExtent.y)&&
            e.halfExtent.x>=0&&e.halfExtent.y>=0&&(!(e.flags&128u)||(version>=5&&!(e.flags&32u)));
        for(float q:e.rotation)ok=ok&&std::isfinite(q);
        for(float c:e.colorMin)ok=ok&&std::isfinite(c);
        for(float c:e.colorMax)ok=ok&&std::isfinite(c);
        if(!ok||e.shape>12)break;
        for(auto& c:e.color){
            ok=ok&&readFloats(f,c.data(),4);
            for(float channel:c)ok=ok&&std::isfinite(channel);
        }
        for(float& s:e.size)ok=ok&&readFloats(f,&s,1)&&std::isfinite(s)&&s>=0;
        ok=ok&&readFloats(f,&e.angleMin,1)&&readFloats(f,&e.angleMax,1)&&finiteRange(e.angleMin,e.angleMax);
        for(unsigned s=0;s<curveSamples;++s)
            ok=ok&&readFloats(f,&e.sizeMaxCurve[s],1)&&finiteRange(e.size[s],e.sizeMaxCurve[s]);
        if(version>=4)ok=ok&&readFloats(f,&e.spinMin,1)&&readFloats(f,&e.spinMax,1)&&
            finiteRange(e.spinMin,e.spinMax)&&readU32(f,e.spriteCount)&&e.spriteCount>0&&e.spriteCount<=64;
        if(version>=5)ok=ok&&readFloats(f,&e.halfHeight,1)&&std::isfinite(e.halfHeight)&&e.halfHeight>=0;
    }
    for(unsigned i=emitters;i<emitterCount;++i)next[i]={};
    float previous=0;
    for(unsigned i=0;ok&&i<edges;++i){
        auto& edge=nextEdges[i];
        float values[5]{};ok=ok&&readFloats(f,values,5);
        if(!ok)break;
        for(unsigned i=0;i<4;++i)ok=ok&&std::isfinite(values[i])&&std::abs(values[i])<=5;
        const float length=std::hypot(values[2]-values[0],values[3]-values[1]);
        ok=ok&&length>0&&std::isfinite(values[4])&&values[4]>previous&&
            std::abs(values[4]-previous-length)<.0001f;
        edge={{values[0],values[1]},{values[2],values[3]},values[4]};previous=values[4];
    }
    ok=ok&&std::fgetc(f)==EOF;std::fclose(f);
    if(!ok)return false;
    unsigned sum=0;for(unsigned i=0;i<emitters;++i)sum+=next[i].count;
    if(sum!=total)return false;
    if(spriteOverride!=~0u){
        for(unsigned i=0;i<emitters;++i){
            if(next[i].sprite<sprite||next[i].sprite-sprite>~0u-spriteOverride)return false;
            next[i].sprite=spriteOverride+(next[i].sprite-sprite);
        }
    }
    if(preserveAtlas)for(unsigned i=0;i<emitters;++i)
        if(next[i].sprite>=visuals.atlas.sprites.size()||
           std::max(next[i].frames,next[i].spriteCount)>visuals.atlas.sprites.size()-next[i].sprite)return false;
    AnimationBank savedAtlas=preserveAtlas?std::move(visuals.atlas):AnimationBank{};
    emitters_=next;edges_=nextEdges;sprite_=spriteOverride==~0u?sprite:spriteOverride;activeCapacity_=total;activeEmitters_=emitters;
    ready_=true;bound_=preserveAtlas;count_=0;
    visuals.ambientAlphaGainApplied=transformedBank;
    emissionClock_.fill(0);particles_.fill({});liveByEmitter_.fill(0);freeSlots_.fill(~std::uint32_t{0});
    visuals.draws.clear();visuals.cameraBounds.clear();visuals.atlas=AnimationBank{};
    visuals.draws.reserve(activeCapacity_);
    if(preserveAtlas)visuals.atlas=std::move(savedAtlas);
    return true;
}

bool AmbientParticles::selectZone(unsigned zone)
{
    if(zone==5)return load("romfs:/ambient-particles.bin");
    if(zone!=9&&zone!=10&&zone!=25)return false;
    char path[64];std::snprintf(path,sizeof(path),"romfs:/deepnest-ambient-%u.bin",zone);
    return load(path);
}

bool AmbientParticles::bindVisuals(const RoomVisualBank& source,bool roomTextures)
{
    bound_=false;
    if(!ready_||sprite_>=source.atlas.sprites.size())return false;
    for(unsigned i=0;i<activeEmitters_;++i)
        if(emitters_[i].sprite>=source.atlas.sprites.size()||
           std::max(emitters_[i].frames,emitters_[i].spriteCount)>source.atlas.sprites.size()-emitters_[i].sprite)return false;
    visuals.atlas=source.atlas;visuals.usesRoomTextures=roomTextures;bound_=true;return true;
}

bool AmbientParticles::lifetimeInvariant() const
{
    std::array<unsigned,emitterCount> perEmitter{};unsigned alive=0;
    for(const auto& p:particles_){
        if(!p.alive)continue;
        ++alive;
        if(p.emitter>=emitterCount||!std::isfinite(p.age)||!std::isfinite(p.life)||
           p.age<0||p.life<=0||p.age>=p.life)return false;
        // Tutorial_01's four source emitters have zero gravity. Rewind their
        // constant-velocity motion, then undo the emitter rotation to validate
        // the authored XZ plane footprint for every live particle.
        const auto& e=emitters_[p.emitter];
        if(p.birthEdge>=edgeCount||!finiteRange(e.angleMin,p.angle)||p.angle>e.angleMax||
           !std::isfinite(p.sizeBlend)||p.sizeBlend<0||p.sizeBlend>1||
           !finiteRange(e.spinMin,p.spin)||p.spin>e.spinMax||p.frame>=std::max(e.frames,e.spriteCount))return false;
        const Vec2 emittedWorld{p.position.x-p.velocity.x*p.age-e.position.x-p.birthCamera.x,
                                p.position.y-p.velocity.y*p.age-4.905f*p.gravity*p.age*p.age-e.position.y-p.birthCamera.y};
        float emittedZ=0;
        Vec2 local;
        if(e.transformedShape){
            const auto& b=e.shapeBasis;
            const std::array<float,9> inverse{{b[4]*b[8]-b[5]*b[7],b[2]*b[7]-b[1]*b[8],b[1]*b[5]-b[2]*b[4],
                b[5]*b[6]-b[3]*b[8],b[0]*b[8]-b[2]*b[6],b[2]*b[3]-b[0]*b[5],
                b[3]*b[7]-b[4]*b[6],b[1]*b[6]-b[0]*b[7],b[0]*b[4]-b[1]*b[3]}};
            const float determinant=b[0]*inverse[0]+b[1]*inverse[3]+b[2]*inverse[6];
            if(!std::isfinite(determinant)||std::abs(determinant)<1e-12f)return false;
            const float worldZ=p.z-p.velocityZ*p.age-e.z;
            local={(inverse[0]*emittedWorld.x+inverse[1]*emittedWorld.y+inverse[2]*worldZ)/determinant,
                   (inverse[3]*emittedWorld.x+inverse[4]*emittedWorld.y+inverse[5]*worldZ)/determinant};
            emittedZ=(inverse[6]*emittedWorld.x+inverse[7]*emittedWorld.y+inverse[8]*worldZ)/determinant;
            if(std::abs(local.x)>e.shapeHalf[0]+.002f||std::abs(local.y)>e.shapeHalf[1]+.002f||
               std::abs(emittedZ)>e.shapeHalf[2]+.002f)return false;
            if(e.shape!=6){++perEmitter[p.emitter];continue;}
        }else local=rotateXY({-e.rotation[0],-e.rotation[1],-e.rotation[2],e.rotation[3]},
                              emittedWorld,p.z-p.velocityZ*p.age-e.z,emittedZ);
        if(!e.transformedShape&&(std::abs(local.x)>e.halfExtent.x+.002f||
           std::abs(local.y)>e.halfHeight+.002f||std::abs(emittedZ)>e.halfExtent.y+.002f))return false;
        if(e.flags&(32u|128u)){++perEmitter[p.emitter];continue;}
        const auto& edge=edges_[p.birthEdge];
        const Vec2 a{edge.start.x*e.scaleX,edge.start.y*e.scaleZ};
        const Vec2 b{edge.end.x*e.scaleX,edge.end.y*e.scaleZ};
        const float dx=b.x-a.x,dz=b.y-a.y,length=std::hypot(dx,dz);
        const float along=((local.x-a.x)*dx+(emittedZ-a.y)*dz)/length;
        if(along<-.002f||along>length+.002f||
           std::abs((local.x-a.x)*dz-(emittedZ-a.y)*dx)>.002f*length)return false;
        ++perEmitter[p.emitter];
    }
    if(alive!=count_||alive>activeCapacity_)return false;
    for(unsigned i=0;i<activeEmitters_;++i)if(perEmitter[i]>emitters_[i].count)return false;
#if HK3DS_PARTICLE_ANIMATION_FAST
    if(perEmitter!=liveByEmitter_)return false;
    for(unsigned i=0;i<capacity;++i)
        if(bool(freeSlots_[i/32]&(std::uint32_t{1}<<(i%32)))==particles_[i].alive)return false;
#endif
    return true;
}

void AmbientParticles::reset(unsigned seed)
{
    seed_=seed?seed:1;count_=0;emissionClock_.fill(0);particles_.fill({});
    liveByEmitter_.fill(0);freeSlots_.fill(~std::uint32_t{0});
    visuals.draws.clear();visuals.cameraBounds.clear();
}

void AmbientParticles::clear()
{
    ready_=bound_=false;activeEmitters_=activeCapacity_=0;
    reset();visuals.atlas=AnimationBank{};visuals.usesRoomTextures=false;
}

float AmbientParticles::random(float low,float high)
{
    seed_=seed_*1664525u+1013904223u;
    const float unit=float(seed_>>8)*(1.0f/16777216.0f);
    return low+(high-low)*unit;
}

void AmbientParticles::spawn(unsigned emitter,Vec2 camera,float age)
{
    if(emitter>=activeEmitters_||count_>=activeCapacity_)return;
    unsigned slot=activeCapacity_;
#if HK3DS_PARTICLE_ANIMATION_FAST
    if(liveByEmitter_[emitter]>=emitters_[emitter].count)return;
    for(unsigned word=0;word<freeSlots_.size();++word){
        if(freeSlots_[word]){slot=word*32+unsigned(__builtin_ctz(freeSlots_[word]));break;}
    }
    if(slot>=activeCapacity_)return;
    freeSlots_[slot/32]&=~(std::uint32_t{1}<<(slot%32));
    ++liveByEmitter_[emitter];
#else
    unsigned alive=0;
    for(unsigned i=0;i<capacity;++i){
        if(i>=activeCapacity_)break;
        if(particles_[i].alive){alive+=particles_[i].emitter==emitter;continue;}
        if(slot==activeCapacity_)slot=i;
    }
    if(alive>=emitters_[emitter].count||slot==activeCapacity_)return;
#endif
    const auto& e=emitters_[emitter];
    auto& p=particles_[slot];p={};p.alive=true;p.emitter=emitter;
    if(e.frames>1)p.frame=std::uint8_t(std::min(e.frames-1,unsigned(random(0,float(e.frames)))));
    // approximation: unique triangle edges weighted by local length; the native
    // Edge/Random consumer's enumeration and weight space remain unresolved.
    float localZ=0;Vec2 point{};
    if(e.shape==5||(!e.transformedShape&&(e.flags&128u))){point={random(-e.halfExtent.x,e.halfExtent.x),random(-(e.transformedShape?e.shapeHalf[1]:e.halfHeight),e.transformedShape?e.shapeHalf[1]:e.halfHeight)};localZ=random(-e.halfExtent.y,e.halfExtent.y);p.birthEdge=0;}
    else if(e.shape==12||(!e.transformedShape&&(e.flags&32u))){
        // approximation: Unity's Edge shape is a one-dimensional segment;
        // use the recovered radius as its half-length.
        point={random(-e.halfExtent.x,e.halfExtent.x),0};
        localZ=0;p.birthEdge=0;
    } else {
        const float selection=random(0,edges_.back().cumulative);
        const auto selected=std::upper_bound(edges_.begin(),edges_.end(),selection,
            [](float value,const Edge& edge){return value<edge.cumulative;});
        p.birthEdge=std::min(unsigned(selected-edges_.begin()),edgeCount-1);
        const auto& edge=edges_[p.birthEdge];const float along=random(0,1);
        point={(edge.start.x+(edge.end.x-edge.start.x)*along)*(e.transformedShape?1:e.scaleX),0};
        localZ=(edge.start.y+(edge.end.y-edge.start.y)*along)*(e.transformedShape?1:e.scaleZ);
    }
    float pz=0;Vec2 transformed;
    if(e.transformedShape){
        const auto& b=e.shapeBasis;
        transformed={b[0]*point.x+b[1]*point.y+b[2]*localZ,
                     b[3]*point.x+b[4]*point.y+b[5]*localZ};
        pz=b[6]*point.x+b[7]*point.y+b[8]*localZ;
    }else transformed=rotateXY(e.rotation,point,localZ,pz);
    // Simulation Space = World: only the emitter follows the camera. Keep the
    // birth origin for the lifetime footprint invariant, including camera moves.
    if(!e.transformedShape&&(e.flags&256u))camera={};
    p.birthCamera=camera;
    p.position={camera.x+e.position.x+transformed.x,camera.y+e.position.y+transformed.y};p.z=e.z+pz;
    Vec2 velocity{random(e.velocityMin.x,e.velocityMax.x),random(e.velocityMin.y,e.velocityMax.y)};
    p.velocityZ=random(e.velocityZMin,e.velocityZMax);
    p.velocity=((e.transformedShape&&(e.flags&4u))||(!e.transformedShape&&(e.flags&64u)))?velocity:
        rotateXY(e.rotation,velocity,p.velocityZ,p.velocityZ);
    const float speed=random(e.speedMin,e.speedMax);
    const bool worldBox=!e.transformedShape&&(e.flags&256u);
    float normalZ=0;Vec2 normal=rotateXY(e.rotation,{0,worldBox?0.f:speed},worldBox?speed:0.f,normalZ);
    p.velocity.x+=normal.x;p.velocity.y+=normal.y;p.velocityZ+=normalZ;
    p.life=random(e.lifeMin,e.lifeMax);p.age=std::clamp(age,0.0f,std::nextafter(p.life,0.0f));
    p.size=random(e.sizeMin,e.sizeMax);p.gravity=random(e.gravityMin,e.gravityMax);
    p.angle=random(e.angleMin,e.angleMax);
    if(e.spinMin!=0||e.spinMax!=0)p.spin=random(e.spinMin,e.spinMax);
    if(e.spriteCount>1)p.frame=std::min(e.spriteCount-1,unsigned(random(0,float(e.spriteCount))));
    p.cosAngle=std::cos(p.angle+p.spin*p.age);p.sinAngle=std::sin(p.angle+p.spin*p.age);
    // approximation: hold the size-module random factor for the particle lifetime;
    // the managed MinMaxCurve API does not establish native cache lifetime.
    p.sizeBlend=random(0,1);
    if(!e.transformedShape&&(e.flags&4u)){
        const float blend=random(0,1);
        for(unsigned c=0;c<4;++c)p.startColor[c]=e.colorMin[c]+(e.colorMax[c]-e.colorMin[c])*blend;
    }else for(unsigned c=0;c<4;++c)p.startColor[c]=random(e.colorMin[c],e.colorMax[c]);
    if(p.age>0){
        const float gravity=-9.81f*p.gravity;
        p.position.x+=p.velocity.x*p.age;
        p.position.y+=p.velocity.y*p.age+.5f*gravity*p.age*p.age;
        p.z+=p.velocityZ*p.age;
        p.velocity.y+=gravity*p.age;
    }
    p.history.reset(p.position);++count_;
}

void AmbientParticles::prewarm(Vec2 camera)
{
    if(!ready_)return;
    reset(seed_);
    float duration=0;
    for(unsigned i=0;i<activeEmitters_;++i)duration=std::max(duration,emitters_[i].lifeMax);
    constexpr float step=1.0f/60.0f;
    // Two lifetimes let the pool reach a stable age distribution through its
    // authored emission and expiry rules, without forcing the pool to capacity.
    for(float elapsed=0;elapsed<duration*2;elapsed+=step)
        update(std::min(step,duration*2-elapsed),camera);
}

float AmbientParticles::curve(const std::array<float,curveSamples>& values,float t) const
{
    const float x=std::clamp(t,0.0f,1.0f)*float(curveSamples-1);
    const unsigned lo=unsigned(x),hi=std::min(lo+1,curveSamples-1);
    return values[lo]+(values[hi]-values[lo])*(x-float(lo));
}

std::array<float,4> AmbientParticles::colorCurve(const Emitter& e,float t) const
{
    const float x=std::clamp(t,0.0f,1.0f)*float(curveSamples-1);
    const unsigned lo=unsigned(x),hi=std::min(lo+1,curveSamples-1);std::array<float,4> c{};
    for(unsigned i=0;i<4;++i)c[i]=e.color[lo][i]+(e.color[hi][i]-e.color[lo][i])*(x-float(lo));
    return c;
}

void AmbientParticles::update(float seconds,Vec2 camera)
{
    if(!ready_||!std::isfinite(seconds)||seconds<=0||
       !std::isfinite(camera.x)||!std::isfinite(camera.y))return;
    observeSimulationWork(SimulationWork::AmbientSlots,activeCapacity_);
    for(unsigned i=0;i<activeCapacity_;++i){
        auto& p=particles_[i];if(!p.alive)continue;
        PresentationPosition::Step step(p.history,p.position);
        p.age+=seconds;
        if(p.age>=p.life){
            p.alive=false;--count_;
#if HK3DS_PARTICLE_ANIMATION_FAST
            --liveByEmitter_[p.emitter];freeSlots_[i/32]|=std::uint32_t{1}<<(i%32);
#endif
            continue;
        }
        // Match the constant-acceleration path used for pre-aged births.
        const float gravity=-9.81f*p.gravity;
        p.position.x+=p.velocity.x*seconds;p.position.y+=p.velocity.y*seconds;
        p.position.y+=.5f*gravity*seconds*seconds;
        p.velocity.y+=gravity*seconds;
        p.z+=p.velocityZ*seconds;
        if(p.spin!=0){p.cosAngle=std::cos(p.angle+p.spin*p.age);p.sinAngle=std::sin(p.angle+p.spin*p.age);}
    }
    for(unsigned e=0;e<activeEmitters_;++e){
        auto& clock=emissionClock_[e];const auto& config=emitters_[e];
        const float total=clock+seconds*config.rate;unsigned births=unsigned(total);
        clock=total-float(births);
        for(unsigned b=0;b<births;++b)spawn(e,camera,float(births-1-b)/config.rate);
    }
}

void AmbientParticles::present(PresentationTime time)
{
    PerformanceScope timing(PerformanceSection::AmbientPresent);
    visuals.draws.clear();visuals.cameraBounds.clear();
    if(!ready_||!bound_)return;
    for(const auto& p:particles_){
        if(!p.alive)continue;
        const auto& e=emitters_[p.emitter];const float t=p.age/p.life;
        const Vec2 pos=p.history.sample(p.position,time);
        RoomDraw draw;draw.sprite=e.sprite+p.frame;
#if HK3DS_PARTICLE_ANIMATION_FAST
        auto c=(e.flags&1u)?colorCurve(e,t):std::array<float,4>{1,1,1,1};
        float lifeSize=1;
        if(e.flags&2u){
            const float minSize=curve(e.size,t);
            lifeSize=minSize+(curve(e.sizeMaxCurve,t)-minSize)*p.sizeBlend;
        }
#else
        auto c=colorCurve(e,t);
        const float minSize=curve(e.size,t);
        float lifeSize=minSize+(curve(e.sizeMaxCurve,t)-minSize)*p.sizeBlend;
        if(!(e.flags&1u))c={1,1,1,1};
        if(!(e.flags&2u))lifeSize=1;
#endif
        const float size=p.size*lifeSize;
        // shader462 doubles RGB after texturing (ambient TEV stage). Its alpha
        // factor2 fits the authored envelopes and remains in the vertex colour.
        draw.values={size*p.cosAngle,-size*p.sinAngle,pos.x,size*p.sinAngle,size*p.cosAngle,pos.y,p.z,
            std::clamp(p.startColor[0]*c[0],0.0f,1.0f),
            std::clamp(p.startColor[1]*c[1],0.0f,1.0f),
            std::clamp(p.startColor[2]*c[2],0.0f,1.0f),
            std::clamp(p.startColor[3]*c[3]*e.alphaGain,0.0f,1.0f),
            float(e.layer),float(e.order),0,0,float(RoomMaterial::Additive)};
        draw.values[15]=float(e.material);
        visuals.draws.push_back(draw);
        // Empty cameraBounds leaves projection/culling to the room renderer;
        // emitted particles can outlive a camera move or teleport.
    }
}
}
