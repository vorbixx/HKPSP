#include "uumuu_motion.hpp"
#include "uumuu_motion_data.hpp"
#include "uumuu_attacks.hpp"
#include "uumuu.hpp"
#include <cmath>

namespace hk3ds {
namespace data=uumuu_motion_data;
namespace {
bool finite(Vec2 p){return std::isfinite(p.x)&&std::isfinite(p.y);}
Vec2 normalized(Vec2 p){
    const float length=std::hypot(p.x,p.y);
    // Unity Vector2/Vector3.Normalize's small-vector cutoff.
    return length>1e-5f?Vec2{p.x/length,p.y/length}:Vec2{};
}
}
bool UumuuMotion::bind(Vec2 position,Vec2 velocity){
    if(!finite(position)||!finite(velocity))return false;
    position_=position;velocity_=velocity;force_={};
    mode_=UumuuMotionMode::Coast;boundsAwake_=false;
    bounceLastPosition_={};bounceDisplacement_={};bounceSpeed_=0;
    bounceStep_=0;bouncing_=true;
    presentation_.reset(position_);return true;
}
bool UumuuMotion::enter(UumuuMotionMode mode,Vec2 hero){
    if(!finite(hero))return false;
    mode_=mode;actions(hero);return true;
}
bool UumuuMotion::setVelocity(Vec2 velocity){
    if(!finite(velocity))return false;
    velocity_=velocity;return true;
}
bool UumuuMotion::applyRecoil(Vec2 position){
    if(!finite(position))return false;
    position_=position;return true;
}
bool UumuuMotion::place(Vec2 position){
    if(!finite(position))return false;
    position_=position;presentation_.reset(position_);return true;
}
bool UumuuMotion::onAttackEvents(const UumuuAttackEvents& events,Vec2 hero){
    return events.enterMotion&&enter(events.motion,hero);
}
void UumuuMotion::actions(Vec2 hero){
    if(mode_==UumuuMotionMode::Dampen){
        velocity_.x*=data::damping;velocity_.y*=data::damping;return;
    }
    if(mode_==UumuuMotionMode::Coast)return;
    const float speed=mode_==UumuuMotionMode::Idle?data::idleSpeed:
        mode_==UumuuMotionMode::Wounded?data::woundedSpeed:data::anticSpeed;
    const float force=mode_==UumuuMotionMode::Idle?data::idleForce:
        mode_==UumuuMotionMode::Wounded?data::woundedForce:data::anticForce;
    Vec2 delta{hero.x-position_.x,hero.y-position_.y};
    const float distance=std::hypot(delta.x,delta.y);
    if(distance>1){delta.x/=distance;delta.y/=distance;}
    // AddForce queues force; ChaseObjectV2 caps the current velocity before
    // the body step. It does not clamp the newly integrated velocity.
    force_.x+=delta.x*force;force_.y+=delta.y*force;
    const float magnitude=std::hypot(velocity_.x,velocity_.y);
    if(magnitude>speed){velocity_.x*=speed/magnitude;velocity_.y*=speed/magnitude;}
}
void UumuuMotion::fixedUpdate(float dt,Vec2 hero){
    if(!std::isfinite(dt)||dt<=0||!finite(hero))return;
    PresentationPosition::Step step(presentation_,position_);
    actions(hero);
    // Mass/damping/gravity are validated by the converter. This free-body
    // integration is separate from the owning room's collision/recoil adapter;
    // native Unity callback ordering is not established by the saved packet.
    velocity_.x+=force_.x*dt/data::mass;velocity_.y+=force_.y*dt/data::mass;
    force_={};position_.x+=velocity_.x*dt;position_.y+=velocity_.y*dt;
}
void UumuuMotion::sampleBounce(){
    if(!bouncing_)return;
    if(bounceStep_>=3){
        bounceDisplacement_={position_.x-bounceLastPosition_.x,position_.y-bounceLastPosition_.y};
        bounceLastPosition_=position_;bounceSpeed_=std::hypot(velocity_.x,velocity_.y);
        bounceStep_=0;
    }else ++bounceStep_;
}
bool UumuuMotion::collisionEnter(Vec2 normal,EnemyRandom random){
    if(!bouncing_||bounceSpeed_<=data::bounceThreshold||!finite(normal))return false;
    const Vec2 direction=normalized(bounceDisplacement_);
    const float dot=direction.x*normal.x+direction.y*normal.y;
    const Vec2 reflected=normalized({direction.x-2*dot*normal.x,direction.y-2*dot*normal.y});
    // Reflect the sampled displacement, then normalize again as ObjectBounce
    // does. Neither the live velocity nor a normalized replacement normal is used.
    const float speed=bounceSpeed_*data::bounceFactor*random.range(.8f,1.2f);
    velocity_={reflected.x*speed,reflected.y*speed};return true;
}
void UumuuMotion::awakeBounds(){boundsAwake_=true;updateBounds();}
void UumuuMotion::updateBounds(){
    if(!boundsAwake_)return;
    const Vec2 previous=position_;
    if(position_.x<data::minX){position_.x=data::minX;velocity_.x=data::inwardSpeed;}
    else if(position_.x>data::maxX){position_.x=data::maxX;velocity_.x=-data::inwardSpeed;}
    if(position_.y<data::minY){position_.y=data::minY;velocity_.y=data::inwardSpeed;}
    else if(position_.y>data::maxY){position_.y=data::maxY;velocity_.y=-data::inwardSpeed;}
    if(previous.x!=position_.x||previous.y!=position_.y)presentation_.reset(position_);
}

UumuuRecoil::UumuuRecoil():baseSpeed_(data::recoilSpeed){}
bool UumuuRecoil::onShellPhase(UumuuShellPhase phase){
    if(phase==UumuuShellPhase::Wounded)baseSpeed_=data::woundedRecoilSpeed;
    else if(phase==UumuuShellPhase::Recover)baseSpeed_=data::guardedRecoilSpeed;
    else return false;
    return true;
}
bool UumuuRecoil::begin(Strike strike,float magnitude,Rect box,Vec2& body,Vec2& transform,const World& world,
                        void* receiver,void (*started)(void*,Strike)){
    if(active_||!std::isfinite(magnitude)||!finite(body)||!finite(transform)||
       !std::isfinite(box.left)||!std::isfinite(box.right)||!std::isfinite(box.bottom)||
       !std::isfinite(box.top)||box.left>=box.right||box.bottom>=box.top)return false;
    Vec2 direction{};
    switch(strike){
    case Strike::Right:direction={1,0};break;
    case Strike::Left:direction={-1,0};break;
    case Strike::Up:direction={0,1};break;
    case Strike::Down:direction={0,-1};break;
    default:return false;
    }
    const float speed=baseSpeed_*magnitude;
    if(!std::isfinite(speed))return false;
    direction_=direction;box_=box;speed_=speed;time_=data::recoilDuration;
    active_=true;sweeping_=true;
    if(started)started(receiver,strike);
    updatePhysics(0,body,transform,world);return true;
}
bool UumuuRecoil::cancel(){
    if(!active_)return false;
    active_=false;return true;
}
bool UumuuRecoil::updatePhysics(float dt,Vec2& body,Vec2& transform,const World& world){
    if(!active_)return false;
    if(sweeping_&&translateRecoil(body,transform,box_,direction_,speed_*dt,world))sweeping_=false;
    time_-=dt;
    return time_<=0&&cancel();
}
bool UumuuRecoil::fixedUpdate(float dt,Vec2& body,Vec2& transform,const World& world){
    if(!std::isfinite(dt)||dt<=0||!finite(body)||!finite(transform))return false;
    return updatePhysics(dt,body,transform,world);
}
}
