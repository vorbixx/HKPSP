#include "combat.hpp"
#include "hero_tuning.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds
{
namespace
{
bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
float cross(Vec2 a, Vec2 b, Vec2 c)
{ return (b.x-a.x)*(c.y-b.y) - (b.y-a.y)*(c.x-b.x); }
bool convex(const NailShape& shape)
{
    float sign = 0;
    for (std::size_t i=0; i<shape.count; ++i)
    {
        const float turn = cross(shape.vertices[i],shape.vertices[(i+1)%shape.count],shape.vertices[(i+2)%shape.count]);
        if (!std::isfinite(turn) || turn == 0 || (sign != 0 && (turn>0)!=(sign>0))) return false;
        sign = turn;
    }
    return true;
}
bool ignoredName(const char* name)
{
    return name && (std::strcmp(name,"_Props")==0 || std::strcmp(name,"_Scenery")==0 ||
                    std::strcmp(name,"_Enemies")==0 || std::strcmp(name,"Chunk")==0);
}
int roundEven(float value)
{
    const float low = std::floor(value), fraction = value-low;
    const int n = static_cast<int>(low);
    return n + (fraction>0.5f || (fraction==0.5f && n%2!=0));
}
}

bool NailBank::load(const char* path)
{
    auto* file = std::fopen(path,"rb");
    if (!file) return false;
    NailBank next;
    char magic[4]; std::uint32_t count = 0;
    bool ok = std::fread(magic,1,4,file)==4 && std::memcmp(magic,"HKN1",4)==0 &&
              std::fread(&count,4,1,file)==1 && count==nailVariantCount;
    for (auto& shape : next.shapes)
    {
        float transform[4]; std::uint32_t header[2];
        if (!ok) break;
        ok = std::fread(transform,4,4,file)==4 && std::fread(header,4,2,file)==2 &&
             std::fread(shape.clip,1,sizeof(shape.clip),file)==sizeof(shape.clip);
        if (!ok) break;
        shape.offset={transform[0],transform[1]}; shape.scale={transform[2],transform[3]};
        shape.layerMask=header[0]; shape.count=header[1];
        ok = finite(shape.offset) && finite(shape.scale) && shape.scale.x!=0 && shape.scale.y!=0 &&
             shape.count>=3 && shape.count<=maxNailVertices && std::memchr(shape.clip,0,sizeof(shape.clip));
        if (!ok) break;
        static_assert(sizeof(Vec2)==8,"Unexpected point layout");
        ok = std::fread(shape.vertices.data(),sizeof(Vec2),shape.count,file)==shape.count;
        for (std::size_t i=0; ok && i<shape.count; ++i) ok=finite(shape.vertices[i]);
        ok = ok && convex(shape);
    }
    ok = ok && std::fgetc(file)==EOF;
    std::fclose(file);
    if (!ok) return false;
    next.loaded_=true; *this=next; return true;
}
std::size_t nailVariant(const NailState& nail)
{
    if (nail.wall) return 4;
    if (nail.direction==Strike::Up) return 2;
    if (nail.direction==Strike::Down) return 3;
    return nail.alternate ? 1 : 0;
}
Vec2 nailPoint(const Hero& hero, const NailShape& shape, Vec2 local)
{
    const float mirror = hero.facingRight ? -1.0f : 1.0f;
    return {hero.position.x + mirror*(shape.offset.x + local.x*shape.scale.x*hero.nail.rangeScale),
            hero.position.y + shape.offset.y + local.y*shape.scale.y*hero.nail.rangeScale};
}
bool polygonOverlapsBox(const Vec2* points, std::size_t count, Rect box)
{
    if (count<2 || box.left>box.right || box.bottom>box.top) return false;
    const Vec2 center{(box.left+box.right)*0.5f,(box.bottom+box.top)*0.5f};
    const Vec2 extent{(box.right-box.left)*0.5f,(box.top-box.bottom)*0.5f};
    const auto separated = [&](Vec2 axis)
    {
        float low=points[0].x*axis.x+points[0].y*axis.y, high=low;
        for (std::size_t i=1; i<count; ++i)
        {
            const float value=points[i].x*axis.x+points[i].y*axis.y;
            low=std::min(low,value); high=std::max(high,value);
        }
        const float mid=center.x*axis.x+center.y*axis.y;
        const float radius=extent.x*std::abs(axis.x)+extent.y*std::abs(axis.y);
        return high<mid-radius || low>mid+radius;
    };
    if (separated({1,0}) || separated({0,1})) return false;
    for (std::size_t i=0; i<count; ++i)
    {
        const Vec2 a=points[i], b=points[(i+1)%count];
        if (separated({a.y-b.y,b.x-a.x})) return false;
    }
    return true;
}
bool validHitSegment(const std::array<Vec2,2>& segment){
    return finite(segment[0])&&finite(segment[1])&&
        std::abs(segment[0].x)<=100000&&std::abs(segment[0].y)<=100000&&
        std::abs(segment[1].x)<=100000&&std::abs(segment[1].y)<=100000&&
        (segment[0].x!=segment[1].x||segment[0].y!=segment[1].y);
}
bool validHitCircle(const CombatCircle& circle){
    return finite(circle.center)&&std::isfinite(circle.radius)&&circle.radius>0&&
        std::abs(circle.center.x)+circle.radius<=100000&&std::abs(circle.center.y)+circle.radius<=100000;
}
bool circleOverlapsBox(const CombatCircle& circle,Rect box)
{
    if(!validHitCircle(circle)||box.left>box.right||box.bottom>box.top)return false;
    const float dx=circle.center.x-std::clamp(circle.center.x,box.left,box.right);
    const float dy=circle.center.y-std::clamp(circle.center.y,box.bottom,box.top);
    return dx*dx+dy*dy<=circle.radius*circle.radius;
}
bool validHitTriangle(const std::array<Vec2,3>& triangle)
{
    for(const auto p:triangle)if(!finite(p)||std::abs(p.x)>100000||std::abs(p.y)>100000)return false;
    const auto a=triangle[0],b=triangle[1],c=triangle[2];
    const double area=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);
    return std::isfinite(area)&&area!=0;
}
bool polygonOverlapsCircle(const Vec2* points,std::size_t count,const CombatCircle& circle)
{
    if(!points||count<3||!validHitCircle(circle))return false;
    bool positive=false,negative=false;
    for(std::size_t i=0;i<count;++i){
        const auto a=points[i],b=points[(i+1)%count];
        const Vec2 edge{b.x-a.x,b.y-a.y},offset{circle.center.x-a.x,circle.center.y-a.y};
        const float cross=edge.x*offset.y-edge.y*offset.x;
        positive|=cross>0;negative|=cross<0;
        const float length=edge.x*edge.x+edge.y*edge.y;
        const float t=length>0?std::clamp((offset.x*edge.x+offset.y*edge.y)/length,0.f,1.f):0;
        const float dx=offset.x-t*edge.x,dy=offset.y-t*edge.y;
        if(dx*dx+dy*dy<=circle.radius*circle.radius)return true;
    }
    // Nail polygons are validated convex on load, with either winding.
    return !(positive&&negative);
}
bool nailOverlapsCircle(const Hero& hero,const NailShape& shape,const CombatCircle& circle)
{
    if(shape.count<3||shape.count>maxNailVertices)return false;
    std::array<Vec2,maxNailVertices> world{};
    for(std::size_t i=0;i<shape.count;++i)world[i]=nailPoint(hero,shape,shape.vertices[i]);
    return polygonOverlapsCircle(world.data(),shape.count,circle);
}
bool nailOverlaps(const Hero& hero, const NailShape& shape, Rect box)
{
    std::array<Vec2,maxNailVertices> world{};
    for (std::size_t i=0; i<shape.count; ++i) world[i]=nailPoint(hero,shape,shape.vertices[i]);
    return polygonOverlapsBox(world.data(),shape.count,box);
}
bool nailOverlapsPolygon(const Hero& hero,const NailShape& shape,const Vec2* points,unsigned count)
{
    if(shape.count<3 || shape.count>maxNailVertices||!points||count<2||count>16)return false;
    std::array<Vec2,maxNailVertices> world{};
    for(unsigned i=0;i<shape.count;++i)world[i]=nailPoint(hero,shape,shape.vertices[i]);
    const auto separated=[&](Vec2 axis)
    {
        float a=world[0].x*axis.x+world[0].y*axis.y,b=a;
        float c=points[0].x*axis.x+points[0].y*axis.y,d=c;
        for(unsigned i=1;i<shape.count;++i)
        {const float p=world[i].x*axis.x+world[i].y*axis.y;a=std::min(a,p);b=std::max(b,p);}
        for(unsigned i=1;i<count;++i)
        {const float p=points[i].x*axis.x+points[i].y*axis.y;c=std::min(c,p);d=std::max(d,p);}
        return b<c || d<a;
    };
    for(unsigned i=0;i<shape.count;++i)
    {const auto a=world[i],b=world[(i+1)%shape.count];if(separated({a.y-b.y,b.x-a.x}))return false;}
    for(unsigned i=0;i<count;++i)
    {const auto a=points[i],b=points[(i+1)%count];if(separated({a.y-b.y,b.x-a.x}))return false;}
    return true;
}
bool nailOverlapsQuad(const Hero& hero,const NailShape& shape,const std::array<Vec2,4>& quad)
{
    return nailOverlapsPolygon(hero,shape,quad.data(),unsigned(quad.size()));
}
static bool nailOverlapsPieces(const Hero& hero,const NailShape& shape,const CombatObject& object)
{
    if(object.hitPolygon&&!nailOverlapsQuad(hero,shape,*object.hitPolygon))return false;
    if(!object.hitTriangleCount&&!object.hitCircleCount&&!object.hitSegmentCount)return true;
    for(unsigned i=0;i<object.hitSegmentCount;++i)
        if(nailOverlapsPolygon(hero,shape,object.hitSegments[i].data(),2))return true;
    for(unsigned i=0;i<object.hitTriangleCount;++i)
        if(nailOverlapsPolygon(hero,shape,object.hitTriangles[i].data(),3))return true;
    for(unsigned i=0;i<object.hitCircleCount;++i)
        if(nailOverlapsCircle(hero,shape,object.hitCircles[i]))return true;
    return false;
}
static bool boxOverlapsPieces(Rect box,const CombatObject& object)
{
    if(object.hitPolygon&&!polygonOverlapsBox(object.hitPolygon->data(),4,box))return false;
    if(!object.hitTriangleCount&&!object.hitCircleCount&&!object.hitSegmentCount)return true;
    for(unsigned i=0;i<object.hitSegmentCount;++i)
        if(polygonOverlapsBox(object.hitSegments[i].data(),2,box))return true;
    for(unsigned i=0;i<object.hitTriangleCount;++i)
        if(polygonOverlapsBox(object.hitTriangles[i].data(),3,box))return true;
    for(unsigned i=0;i<object.hitCircleCount;++i)
        if(circleOverlapsBox(object.hitCircles[i],box))return true;
    return false;
}

bool EnemyHealth::blocksDirection(Strike direction) const
{
    if (!invincible) return false;
    const int d=invincibleFromDirection;
    if (d==0) return true;
    switch(direction)
    {
    case Strike::Right: return d==1 || d==5 || d==8 || d==10;
    case Strike::Up: return d==2 || (d>=5 && d<=9);
    case Strike::Left: return d==3 || d==6 || d==9 || d==11;
    case Strike::Down: return d==4 || (d>=7 && d<=11);
    }
    return false;
}
HitOutcome EnemyHealth::hit(const NailHit& hit, Vitals& heroVitals, const Charms& charms,
                           const AcceptedHit& accepted)
{
    if (dead || evasion>0 || hit.damage<=0) return HitOutcome::Ignored;
    lastDirection=hit.direction;
    if (!(spellVulnerable && hit.type==AttackType::Spell) && blocksDirection(hit.direction))
    {
        ++blocks; evasion=0.15f; return HitOutcome::Blocked;
    }
    if (accepted.callback) accepted.callback(accepted.receiver,hit,accepted.contact);
    ++hits;
    if (hit.type==AttackType::Nail && enemyType!=3 && enemyType!=6) heroVitals.gainSoul(charms);
    const int amount=damageOverride ? 1 : roundEven(hit.damage*hit.multiplier);
    hp=std::max(-50,hp-amount);
    if (hp<=0 && !specialDeath) { dead=true; return HitOutcome::Killed; }
    // IgnoreInvulnerable suppresses NEW evasion; it does not bypass the entry
    // guard or directional blocking. Alternate hit animations also skip evasion.
    // HealthManager.NonFatalHit uses a constant, not serialized invulnerableTime.
    if (!hit.ignoreInvulnerable && !alternateHitAnimation) evasion=0.2f;
    return hp<=0 ? HitOutcome::SpecialDeath : HitOutcome::Damaged;
}

std::size_t copyDiagnosticBounds(const CombatObject* objects,std::size_t count,Rect* out,std::size_t capacity)
{
    std::size_t used=0;
    const auto append=[&](Rect b){
        if(used<capacity&&b.left<b.right&&b.bottom<b.top)out[used++]=b;
    };
    for(std::size_t i=0;i<count&&used<capacity;++i){
        const auto& o=objects[i];
        if(!o.colliderEnabled)continue;
        if(!o.hitCircle||o.colliderCount==2){
            auto b=o.bounds;
            if(o.hitPolygon){
                const auto& p=*o.hitPolygon;
                b={p[0].x,p[0].y,p[0].x,p[0].y};
                for(const auto& v:p){b.left=std::min(b.left,v.x);b.right=std::max(b.right,v.x);
                    b.bottom=std::min(b.bottom,v.y);b.top=std::max(b.top,v.y);}
            }
            append(b);
        }
        if(o.hitCircle){const auto& c=*o.hitCircle;
            append({c.center.x-c.radius,c.center.y-c.radius,c.center.x+c.radius,c.center.y+c.radius});}
    }
    return used;
}

bool CombatScene::bind(CombatObject* objects, std::size_t count, const NailBank* bank)
{
    observedThorns_=0;hatchlingTime_=0;
    if (count>capacity || (count && !objects)) return false;
    for (std::size_t i=0; i<count; ++i)
        if (objects[i].layer>=32 || objects[i].colliderCount<1 || objects[i].colliderCount>2 || objects[i].parent < -1 || objects[i].parent>=static_cast<int>(count) ||
            (objects[i].hitCircle&&(!validHitCircle(*objects[i].hitCircle)||objects[i].hitPolygon||objects[i].hitTriangles||objects[i].hitCircles||objects[i].hitSegments)) ||
            ((objects[i].hitSegments!=nullptr)!=(objects[i].hitSegmentCount!=0)) ||
            (objects[i].hitSegmentCount&&(objects[i].hitSegmentCount>2048||objects[i].hitPolygon||objects[i].colliderCount!=1)) ||
            ((objects[i].hitTriangles!=nullptr)!=(objects[i].hitTriangleCount!=0)) ||
            ((objects[i].hitCircles!=nullptr)!=(objects[i].hitCircleCount!=0)) ||
            (objects[i].hitTriangleCount&&(objects[i].hitTriangleCount>2048||objects[i].hitPolygon||objects[i].colliderCount!=1)) ||
            (objects[i].hitCircleCount&&(objects[i].hitCircleCount>256||objects[i].hitPolygon||objects[i].colliderCount!=1))) return false;
    for(std::size_t i=0;i<count;++i)for(unsigned t=0;t<objects[i].hitTriangleCount;++t)
        if(!validHitTriangle(objects[i].hitTriangles[t]))return false;
    for(std::size_t i=0;i<count;++i)for(unsigned c=0;c<objects[i].hitCircleCount;++c)
        if(!validHitCircle(objects[i].hitCircles[c]))return false;
    for(std::size_t i=0;i<count;++i)for(unsigned s=0;s<objects[i].hitSegmentCount;++s)
        if(!validHitSegment(objects[i].hitSegments[s]))return false;
    for (std::size_t i=0; i<count; ++i)
    {
        int parent=objects[i].parent;
        for (std::size_t depth=0; parent>=0; ++depth)
        {
            if (depth>=count || parent==static_cast<int>(i)) return false;
            parent=objects[parent].parent;
        }
    }
    *this={}; objects_=objects; count_=count; bank_=bank; return true;
}
void CombatScene::charmEffects(Hero& hero,float dt)
{
    const bool thorns=observedThorns_!=hero.thornsSerial;
    observedThorns_=hero.thornsSerial;
    if(hero.actor==Actor::Dead)return;
    hatchlingTime_=hero.charms.glowingWomb?hatchlingTime_+dt:0;
    const bool hatchling=hatchlingTime_>=4&&hero.vitals.soul>=8;
    if(!thorns&&!hatchling)return;
    const float radius=thorns?2.5f:8.f;
    bool fed=false;
    for(std::size_t i=0;i<count_;++i){
        auto& target=objects_[i];if(!target.hasHealth||!target.colliderEnabled||target.health.dead)continue;
        float x=std::clamp(hero.position.x,target.bounds.left,target.bounds.right)-hero.position.x;
        float y=std::clamp(hero.position.y,target.bounds.bottom,target.bounds.top)-hero.position.y;
        if(target.hitCircle){
            const auto& circle=*target.hitCircle;
            const float dx=circle.center.x-hero.position.x,dy=circle.center.y-hero.position.y;
            const float distance=std::sqrt(dx*dx+dy*dy);
            const float scale=distance>circle.radius?1-circle.radius/distance:0;
            const float cx=dx*scale,cy=dy*scale;
            if(target.colliderCount==1||cx*cx+cy*cy<x*x+y*y){x=cx;y=cy;}
        }
        if(x*x+y*y>radius*radius)continue;
        // approximation: one bounded direct hit replaces hatchling flight.
        const NailHit hit{x<0?Strike::Left:Strike::Right,thorns?hero.nailDamage*2:9};
        Vitals ignored;const auto result=target.hit(hit,ignored,hero.charms);
        if(result==HitOutcome::Damaged||result==HitOutcome::Killed||result==HitOutcome::SpecialDeath){
            if(target.onHit)target.onHit(target.receiver,hit,result);
            ++events.tookDamage;if(result!=HitOutcome::Damaged)++events.zeroHP;
            mark(CombatPhase::HealthHit,{i,0,0,TriggerPhase::Enter},i,true,result);
            fed=true;
        }
        if(!thorns&&fed)break;
    }
    if(!thorns&&fed){hero.vitals.soul-=8;hatchlingTime_=0;}
}
void CombatScene::update(const Hero& hero, float dt)
{
    for (std::size_t v=0; v<nailVariantCount; ++v)
    {
        const int enabled=hero.nail.active && hero.nail.hitbox && nailVariant(hero.nail)==v;
        // LimitSendEvents observes collider state in Update, not FixedUpdate.
        updateLimiter(v,enabled);
    }
    for (std::size_t i=0; i<count_; ++i) updateHealth(i,dt);
}
std::size_t CombatScene::sentCount(std::size_t variant) const
{ return static_cast<std::size_t>(std::count(sent_[variant].begin(),sent_[variant].begin()+count_,true)); }
void CombatScene::mark(CombatPhase phase,const NailContact& contact,std::size_t target,bool accepted,HitOutcome outcome)
{ if (trace) trace(traceContext,{phase,contact,target,sentCount(contact.variant),accepted,outcome}); }
void CombatScene::updateLimiter(std::size_t variant,bool enabled)
{
    const bool changed=observed_[variant]!=static_cast<int>(enabled);
    if (changed) { sent_[variant].fill(false); observed_[variant]=enabled; }
    mark(CombatPhase::LimiterUpdate,{0,variant,0,TriggerPhase::Enter},0,changed);
}
void CombatScene::updateHealth(std::size_t object,float dt)
{ if (objects_[object].hasHealth) objects_[object].health.update(dt); }
void CombatScene::spellHit(Rect bounds,Strike direction,std::array<bool,capacity>& hit,Hero& hero,int damage)
{
    areaHit(bounds,{direction,damage,1,false,AttackType::Spell},hit,hero);
}
void CombatScene::superDashHit(Hero& hero)
{
    if(!hero.superDash.flying())return;
    constexpr Rect local{super_dash::damageOffsetX-super_dash::damageWidth*.5f,
        super_dash::damageOffsetY-super_dash::damageHeight*.5f,
        super_dash::damageOffsetX+super_dash::damageWidth*.5f,
        super_dash::damageOffsetY+super_dash::damageHeight*.5f};
    std::array<bool,capacity> hit{};
    areaHit(boxBounds(hero.position,local,hero.facingRight?-1.f:1.f),
        {hero.superDash.direction>0?Strike::Right:Strike::Left,super_dash::damage,1,false,AttackType::Generic,
         super_dash::recoilMultiplier},hit,hero,true);
}
void CombatScene::areaHit(Rect bounds,const NailHit& attack,std::array<bool,capacity>& hit,Hero& hero,bool superDashContacts)
{
    for(std::size_t i=0;i<count_;++i)
    {
        const auto& object=objects_[i];
        // Circles and segments include tangency and zero-width area queries.
        // Compound bounds must preserve that boundary contact.
        const bool boundsTouch=(object.hitCircleCount||object.hitSegmentCount)?
            bounds.left<=object.bounds.right&&bounds.right>=object.bounds.left&&
            bounds.bottom<=object.bounds.top&&bounds.top>=object.bounds.bottom:
            overlaps(bounds,object.bounds);
        const bool touching=object.colliderEnabled&&
            ((!object.hitCircle&&boundsTouch&&boxOverlapsPieces(bounds,object))||
             (object.hitCircle&&((object.colliderCount==2&&overlaps(bounds,object.bounds))||
                                circleOverlapsBox(*object.hitCircle,bounds))));
        if(!touching)continue;
        // FSM20881 excludes layers20/9. Trigger2dEvent.DoTriggerStay2D ignores
        // its serialized collideLayer field; its empty tag accepts every Stay.
        // GO5366 has no LimitSendEvents, so HealthManager evasion governs repeats.
        if(superDashContacts&&(objects_[i].layer==20||objects_[i].layer==9))continue;
        int index=static_cast<int>(i);
        for(int depth=0;index>=0&&depth<3;++depth)
        {
            auto& target=objects_[index];
            if(target.hasHealth&&!hit[index])
            {
                hit[index]=true;
                const auto outcome=target.hit(attack,hero.vitals,hero.charms);
                ++events.healthCalls;
                if(outcome!=HitOutcome::Ignored){
                    ++events.dealtDamage;++events.hitLanded;
                    if(outcome==HitOutcome::Blocked)++events.blockedHit;
                    else{++events.tookDamage;if(outcome==HitOutcome::Killed||outcome==HitOutcome::SpecialDeath)++events.zeroHP;}
                }
                if(target.onHit)target.onHit(target.receiver,attack,outcome);
            }
            else if(!target.hasHealth&&target.receivesAreaHits&&!hit[index]){
                hit[index]=true;
                if(target.onHit)target.onHit(target.receiver,attack,HitOutcome::Damaged);
            }
            index=target.parent;
        }
    }
}
void CombatScene::hitResponders(std::size_t object, Hero& hero, const NailContact& contact)
{
    int index=static_cast<int>(object);
    // TakeDamage calls HitTaker, which itself visits three hierarchy levels.
    for (int depth=0; index>=0 && depth<3; ++depth)
    {
        auto& target=objects_[index];
        if (target.hasHealth)
        {
            const NailHit hit{hero.nail.direction,hero.effectiveNailDamage()};
            const auto result=target.hit(hit,hero.vitals,hero.charms,&contact);
            ++events.healthCalls;
            mark(CombatPhase::HealthHit,contact,static_cast<std::size_t>(index),result!=HitOutcome::Ignored,result);
            if (target.onHit) target.onHit(target.receiver,hit,result);
            if (result!=HitOutcome::Ignored)
            {
                ++events.dealtDamage; ++events.hitLanded;
                if (result==HitOutcome::Blocked)
                {
                    ++events.blockedHit;
                    if (!target.health.preventInvincibleEffect && !target.health.dontClinkGates)
                    {
                        ++events.clink;
                        if (hero.nail.direction==Strike::Left || hero.nail.direction==Strike::Right) hero.nailContact();
                    }
                }
                else
                {
                    ++events.tookDamage;
                    if (result==HitOutcome::Killed || result==HitOutcome::SpecialDeath) ++events.zeroHP;
                }
            }
        }
        else if(target.onHit)
            target.onHit(target.receiver,{hero.nail.direction,hero.effectiveNailDamage()},HitOutcome::Damaged);
        index=target.parent;
    }
}
void CombatScene::sendDamage(const NailContact& contact, Hero& hero)
{
    const auto object=contact.object, variant=contact.variant;
    if (hero.nailDamage<=0 || objects_[object].layer==9 || objects_[object].layer==20) return;
    int index=static_cast<int>(object);
    for (int depth=0; index>=0 && depth<3; ++depth)
    {
        const bool accepted=!sent_[variant][index];
        sent_[variant][index]=true;
        mark(CombatPhase::LimiterAdd,contact,static_cast<std::size_t>(index),accepted);
        if (accepted)
        {
            if (ignoredName(objects_[index].name))
            {
                if (depth==0) return; // Collider branch CANCEL; parent branch FINISHED.
            }
            else
            {
                ++events.takeDamage;
                mark(CombatPhase::TakeDamage,contact,static_cast<std::size_t>(index),true);
                hitResponders(static_cast<std::size_t>(index),hero,contact);
            }
        }
        index=objects_[index].parent;
    }
}
void CombatScene::nailReaction(const NailContact& contact,Hero& hero)
{
    const auto& object=objects_[contact.object];
    ++events.nailCallbacks;
    mark(CombatPhase::NailReaction,contact,contact.object);
    const bool down=hero.nail.direction==Strike::Down;
    const bool bounceable=down ? ((object.layer==11 || object.layer==19 || object.layer==17) && !object.nonBouncer) :
        ((object.layer==11 && !object.nonBouncer) || (object.layer==19 && object.bounceShroom));
    if (bounceable)
    {
        hero.nailContact(true,down ? object.bigBouncer : object.bounceShroom,object.bounceShroom && !object.bigBouncer);
        if (object.bounceShroom && !(down && object.bigBouncer)) ++events.mushroomBounce;
    }
}
void CombatScene::damageCallback(const NailContact& contact,Hero& hero)
{
    ++events.damageCallbacks;
    mark(CombatPhase::DamageCallback,contact,contact.object);
    sendDamage(contact,hero);
}
void CombatScene::fixedUpdate(Hero& hero)
{
    if (!bank_ || !bank_->ready()) return;
    const bool enabled=hero.nail.active && hero.nail.hitbox;
    const std::size_t variant=nailVariant(hero.nail);
    // Each physical collider delivers its own reaction and damage FSM. Contacts
    // share a health owner and limiter. Snapshot before lethal callbacks.
    std::array<NailContact,capacity*2> pending{};
    std::size_t size=0;
    observeSimulationWork(SimulationWork::CombatTests,nailVariantCount*count_);
    for (std::size_t v=0;v<nailVariantCount;++v)
        for (std::size_t i=0;i<count_;++i)
        {
            const auto& object=objects_[i];
            const bool eligible=enabled && v==variant && object.colliderEnabled &&
                !(object.hasHealth && object.health.dead) &&
                (bank_->shapes[v].layerMask & (std::uint32_t{1}<<object.layer));
            for(unsigned c=0;c<2;++c){
                const bool circular=object.hitCircle&&(object.colliderCount==1||c==1);
                const bool touch=eligible&&c<object.colliderCount&&
                    (circular?nailOverlapsCircle(hero,bank_->shapes[v],*object.hitCircle):
                     (nailOverlaps(hero,bank_->shapes[v],object.bounds)&&
                      nailOverlapsPieces(hero,bank_->shapes[v],object)));
                if(touch)pending[size++]={i,v,c,touching_[v][i][c]?TriggerPhase::Stay:TriggerPhase::Enter};
                touching_[v][i][c]=touch;
            }
        }
    for (std::size_t i=0;i<size;++i)
    {
        nailReaction(pending[i],hero);
        damageCallback(pending[i],hero);
    }
}
}
