#include "uumuu_colliders.hpp"
#include "uumuu_room_data.hpp"
#include <cmath>

namespace hk3ds {
namespace {
namespace data=uumuu_room_data;
bool finitePosition(Vec2 p){return std::isfinite(p.x)&&std::isfinite(p.y);}
static_assert(!data::boxColliderEnabled&&!data::circleColliderEnabled&&
              data::boxColliderIsTrigger&&data::circleColliderIsTrigger,
              "Uumuu body admission requires its disabled source triggers");
static_assert(data::damageEnabled&&data::hazardType==1&&!data::shadowDashHazard&&
              !data::damageResetOnEnable,"Uumuu requires ordinary source DamageHero behavior");
static_assert(sizeof(UumuuColliders)<=128,"Uumuu collider adapter exceeded its fixed-state estimate");
}
bool UumuuColliders::bind(CombatObject& object,Vec2 position){
    if(object_||object.hitPolygon||object.hitCircle||!finitePosition(position))return false;
    object_=&object;previousBounds_=object.bounds;
    previousCount_=object.colliderCount;previousLayer_=object.layer;previousEnabled_=object.colliderEnabled;
    object.colliderCount=2;object.layer=data::bossLayer;object.hitCircle=&circle_;
    return sync(position,false);
}
void UumuuColliders::reset(){
    if(object_&&object_->hitCircle==&circle_){
        object_->hitCircle=nullptr;object_->bounds=previousBounds_;
        object_->colliderCount=previousCount_;object_->layer=previousLayer_;object_->colliderEnabled=previousEnabled_;
    }
    object_=nullptr;
}
bool UumuuColliders::sync(Vec2 position,bool enabled){
    if(!object_||!finitePosition(position))return false;
    const Vec2 boxCenter{position.x+data::boxColliderOffsetX,position.y+data::boxColliderOffsetY};
    object_->bounds={boxCenter.x-data::boxSizeX*.5f,boxCenter.y-data::boxSizeY*.5f,
                     boxCenter.x+data::boxSizeX*.5f,boxCenter.y+data::boxSizeY*.5f};
    circle_={{position.x+data::circleColliderOffsetX,position.y+data::circleColliderOffsetY},data::circleRadius};
    object_->colliderEnabled=enabled;position_=position;return true;
}
void UumuuColliders::contactHero(Hero& hero,Rect body)const{
    if(!object_||!object_->colliderEnabled)return;
    if(boxSensorOverlap(object_->bounds,body)||circleOverlapsBox(circle_,body))
        hero.takeDamage(data::damage,position_.x<hero.position.x?-1:1);
}
}
