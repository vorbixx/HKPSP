#include "room_entities.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds {
float Buzzer::artScale()const
{return animationFacingScale("buzzer/vengefly",-scaleX,std::abs(scaleX));}
namespace {
bool visible(Vec2 a,Vec2 b,const World&w) { float x=b.x-a.x,y=b.y-a.y,len=std::sqrt(x*x+y*y),d=0;return len==0 || !rayTerrain(a,{x/len,y/len},len,w,d); }
float clampSpeed(float v,float max) { return std::max(-max,std::min(max,v)); }
bool finite(float v) { return std::isfinite(v); }
bool readFloats(FILE*f,float*v,unsigned n) { if(std::fread(v,4,n,f)!=n)return false;for(unsigned i=0;i<n;++i)if(!finite(v[i]))return false;return true; }
bool readUInts(FILE*f,unsigned*v,unsigned n) { return std::fread(v,4,n,f)==n; }
}
bool RoomEntityBank::load(const char*path) {
 FILE*f=std::fopen(path,"rb");if(!f)return false;RoomEntityBank next;char magic[4];unsigned counts[2];
 bool ok=std::fread(magic,1,4,f)==4 && !std::memcmp(magic,"RME1",4) && readUInts(f,counts,2) && counts[0]<=maxBuzzers && counts[1]<=maxStalactites;
 if(ok) {next.buzzerCount=counts[0];next.stalactiteCount=counts[1];}
 for(unsigned i=0;ok&&i<next.buzzerCount;++i) {
  auto&c=next.buzzers[i];float v[25];unsigned u[4];ok=readFloats(f,v,25)&&readUInts(f,u,4);if(!ok)break;
  c.spawn={v[0],v[1]};c.body={v[2],v[3],v[4],v[5]};c.heroHurtbox={v[6],v[7],v[8],v[9]};
  c.alertRadius=v[10];c.alertOffsetY=v[11];c.idleMin=v[12];c.idleMax=v[13];c.idleSpeed=v[14];c.idleAcceleration=v[15];c.roamingRange=v[16];
  c.chaseSpeed=v[17];c.chaseAcceleration=v[18];c.attentionSpan=v[19];c.startleDuration=v[20];c.recoilSpeed=v[21];c.recoilDuration=v[22];c.stopDeceleration=v[23];c.stopDuration=v[24];
  c.hp=u[0];c.damage=u[1];c.layer=u[2];c.smallGeo=u[3];ok=c.hp>0&&c.alertRadius>0&&c.startleDuration>0;
 }
 for(unsigned i=0;ok&&i<next.stalactiteCount;++i) {
  auto&c=next.stalactites[i];float v[16];unsigned u[5];ok=readFloats(f,v,16)&&readUInts(f,u,5);if(!ok)break;
  c.spawn={v[0],v[1]};c.trigger={v[2],v[3],v[4],v[5]};c.heroHurtbox={v[6],v[7],v[8],v[9]};c.fallOffset=v[10];c.fallDelay=v[11];c.hitSpeed=v[12];c.gravity=v[13];
  c.layer=u[0];c.vertexCount=u[1];c.enemyDamage=u[2];c.embeddedLayer=u[3];c.embeddedVertexCount=u[4];c.embeddedOffset={v[14],v[15]};ok=c.vertexCount>=3&&c.vertexCount<=c.vertices.size()&&c.fallDelay>=0&&c.hitSpeed>0;
  for(unsigned j=0;ok&&j<c.vertexCount;++j) {float p[2];ok=readFloats(f,p,2);c.vertices[j]={p[0],p[1]};}
  ok=ok&&c.embeddedVertexCount>=3&&c.embeddedVertexCount<=16;
  for(unsigned j=0;ok&&j<c.embeddedVertexCount;++j) {float p[2];ok=readFloats(f,p,2);c.embeddedVertices[j]={p[0],p[1]};}
 }
 if(ok&&std::fgetc(f)!=EOF) {
  std::fseek(f,-1,SEEK_CUR);unsigned count=0;
  ok=std::fread(magic,1,4,f)==4&&!std::memcmp(magic,"SLP1",4)&&readUInts(f,&count,1)&&count<=next.stalactiteCount;
  for(unsigned i=0;ok&&i<count;++i){
   unsigned actor=0;float pose[2];ok=readUInts(f,&actor,1)&&readFloats(f,pose,2)&&actor<next.stalactiteCount;
   if(ok){auto&c=next.stalactites[actor];ok=c.landingStep==0&&pose[1]>0&&pose[1]<=.1f;
    if(ok){c.verticalRestY=pose[0];c.landingStep=pose[1];}}
  }
  if(ok)ok=std::fgetc(f)==EOF;
 }
 std::fclose(f);if(ok)*this=next;return ok;
}
bool Buzzer::bind(const BuzzerConfig&c,CombatObject&o,EnemyRandom r,const AnimationBank* animations) {
 if(!c.hp||c.chaseSpeed<=0||c.startleDuration<=0)return false;
 if(animations)for(const char* name:{"Idle","Startle","Chase","TurnToIdle","TurnToFly"})if(!animations->find(name))return false;
 *this=Buzzer{};config_=c;object_=&o;random_=r;position=c.spawn;animations_=animations;
 o={};o.name="Buzzer";o.layer=c.layer;o.hasHealth=true;o.health.hp=static_cast<int>(c.hp);o.receiver=this;o.onHit=receive;enter(BuzzerState::Idle);sync();return true;
}
Rect Buzzer::bounds()const{return boxBounds(position,config_.body);}
void Buzzer::sync(){if(object_){object_->bounds=bounds();object_->colliderEnabled=alive();}}
void Buzzer::enter(BuzzerState s) {
 const auto previous=state;state=s;timer_=0;
 if(s==BuzzerState::Startle&&previous!=BuzzerState::Startle)++startleSerial;
 if(s==BuzzerState::Idle){idleOrigin_=position;wait_=0;acceleration_={};buzz(0);}
 if(s==BuzzerState::Startle){velocity={};faceHero_=true;}
 if(s==BuzzerState::Stop){auto slow=[&](float v){return v<0?std::min(0.0f,v+config_.stopDeceleration):std::max(0.0f,v-config_.stopDeceleration);};velocity.x=slow(velocity.x);velocity.y=slow(velocity.y);}
 if(!animations_)return;
 if(s==BuzzerState::Idle)animation.play(animations_->require("Idle"));
 else if(s==BuzzerState::Startle)animation.play(animations_->require("Startle"));
 else if(s==BuzzerState::Stop)animation.play(animations_->require("Idle"),3,true);
 else if(s==BuzzerState::ChaseInSight&&previous==BuzzerState::Startle)animation.play(animations_->require("Chase"),3,true);
}
void Buzzer::face(float dt,const Hero& hero) {
 if(faceHero_){scaleX=position.x<hero.position.x?-1.0f:1.0f;faceHero_=false;}
 int index=-1;
 if(state==BuzzerState::Idle)index=0;
 else if(state==BuzzerState::ChaseInSight)index=1;
 else if(state==BuzzerState::ChaseOutOfSight)index=2;
 if(index<0)return;
 // Each original FaceDirection action retains its own timer across entries.
 auto& pause=turnPause_[index];
 if(pause>0){pause-=dt;return;}
 const float next=velocity.x>0?-1.0f:1.0f;
 if(next==scaleX)return;
 scaleX=next;pause=.5f;
 if(animations_)animation.play(animations_->require(index==0?"TurnToIdle":"TurnToFly"),0,true);
}
void Buzzer::update(float dt,const Hero&h,const World&w) {
 if(!alive())return;
 float dx=h.position.x-position.x,dy=h.position.y-(position.y+config_.alertOffsetY);
 Rect hero=boxBounds(h.position,config_.heroHurtbox);float cx=std::max(hero.left,std::min(position.x,hero.right));float cy=std::max(hero.bottom,std::min(position.y+config_.alertOffsetY,hero.top));
 dx=cx-position.x;dy=cy-position.y-config_.alertOffsetY;sees_=dx*dx+dy*dy<=config_.alertRadius*config_.alertRadius&&visible(position,h.position,w);
 timer_+=dt;
 switch(state) {
 case BuzzerState::Idle: if(sees_)enter(BuzzerState::Startle);break;
 case BuzzerState::Startle:if(timer_>=config_.startleDuration){enter(BuzzerState::ChaseInSight);chase(h);}break;
 case BuzzerState::ChaseInSight:enter(BuzzerState::ChaseOutOfSight);chase(h);break;
 case BuzzerState::ChaseOutOfSight:if(sees_){enter(BuzzerState::ChaseInSight);chase(h);}else if(timer_>=config_.attentionSpan)enter(BuzzerState::Stop);break;
 case BuzzerState::Stop:if(timer_>=config_.stopDuration)enter(BuzzerState::Idle);break;
 case BuzzerState::Dead:break;
 }
 face(dt,h);animation.advance(dt);sync();
}
void Buzzer::buzz(float dt) {
 auto axis=[&](float p,float origin,float&v,float&a){if((p<origin-config_.roamingRange&&v<0)||(p>origin+config_.roamingRange&&v>0)){a=(p<origin?1:-1)*config_.idleAcceleration/2000;v/=1.125f;wait_=random_.range(config_.idleMin,config_.idleMax);}};
 axis(position.y,idleOrigin_.y,velocity.y,acceleration_.y);axis(position.x,idleOrigin_.x,velocity.x,acceleration_.x);
 if(wait_<=0){auto sample=[&](float p,float origin){float low=-config_.idleAcceleration,high=config_.idleAcceleration;if(p<origin-config_.roamingRange)low=0;else if(p>origin+config_.roamingRange)high=0;return random_.range(low,high)/2000;};acceleration_.y=sample(position.y,idleOrigin_.y);acceleration_.x=sample(position.x,idleOrigin_.x);wait_=random_.range(config_.idleMin,config_.idleMax);}
 if(wait_>0) { wait_-=dt; }
 velocity.x=clampSpeed(velocity.x+acceleration_.x,config_.idleSpeed);velocity.y=clampSpeed(velocity.y+acceleration_.y,config_.idleSpeed);
}
void Buzzer::chase(const Hero&h){velocity.x=clampSpeed(velocity.x+(position.x<h.position.x?1:-1)*config_.chaseAcceleration,config_.chaseSpeed);velocity.y=clampSpeed(velocity.y+(position.y<h.position.y?1:-1)*config_.chaseAcceleration,config_.chaseSpeed);}
void Buzzer::fixedUpdate(float dt,const Hero&h,const World&w){
 PresentationPosition::Step step(presentation_,position);
 if(soundCorpseActive_){
  // This is a sound-only landing approximation: the saved exports do not
  // include Corpse1312 physics values. It follows the actor's last velocity
  // with approximate gravity, then uses the source ground sweep to dispatch.
  soundCorpseVelocity_.y-=60.0f*dt;
  float distance=0;
  const float reach=std::max(0.08f,std::abs(soundCorpseVelocity_.y*dt)+0.08f);
  if(soundCorpseVelocity_.y<=0&&sweepBox(soundCorpsePosition_,config_.body,{0,-1},reach,w,distance)){
   ++corpseSplatSerial;soundCorpseActive_=false;
  }else{soundCorpsePosition_.x+=soundCorpseVelocity_.x*dt;soundCorpsePosition_.y+=soundCorpseVelocity_.y*dt;}
  if(soundCorpsePosition_.y<-10)soundCorpseActive_=false;
 }
 if(!alive())return;
 if(recoil_>0){if(recoilSweeping_){Vec2 transform=position;recoilSweeping_=!translateRecoil(position,transform,config_.body,recoilDirection_,config_.recoilSpeed*recoilMagnitude_*dt,w);}recoil_=std::max(0.0f,recoil_-dt);}if(state==BuzzerState::Idle)buzz(dt);else if(state==BuzzerState::ChaseInSight||state==BuzzerState::ChaseOutOfSight)chase(h);else if(state==BuzzerState::Stop){auto slow=[&](float v){return v<0?std::min(0.0f,v+config_.stopDeceleration):std::max(0.0f,v-config_.stopDeceleration);};velocity.x=slow(velocity.x);velocity.y=slow(velocity.y);}moveBox(position,velocity,config_.body,dt,w);sync();}
void Buzzer::contactHero(Hero&h)const{if(alive()&&overlaps(bounds(),boxBounds(h.position,config_.heroHurtbox)))h.takeDamage(static_cast<int>(config_.damage),h.position.x<position.x?1:-1);}
void Buzzer::receive(void*self,const NailHit&h,HitOutcome o){static_cast<Buzzer*>(self)->hit(h,o);}
void Buzzer::hit(const NailHit&h,HitOutcome o){if(o!=HitOutcome::Damaged&&o!=HitOutcome::Killed)return;++injurySerial;if(o==HitOutcome::Killed){if(state==BuzzerState::Idle)++startleSerial;++killSerial;state=BuzzerState::Dead;soundCorpsePosition_=position;soundCorpseVelocity_=velocity;soundCorpseActive_=true;velocity={};killFreezeRequested=true;geoReleased=config_.smallGeo;}else{if(state==BuzzerState::Idle)enter(BuzzerState::Startle);if(recoil_>0){sync();return;}recoil_=config_.recoilDuration;recoilMagnitude_=h.recoilMultiplier;recoilSweeping_=true;recoilDirection_={};switch(h.direction){case Strike::Right:recoilDirection_.x=1;break;case Strike::Left:recoilDirection_.x=-1;break;case Strike::Up:recoilDirection_.y=1;break;case Strike::Down:recoilDirection_.y=-1;break;default:break;}}sync();}
bool Stalactite::bind(const StalactiteConfig&c,CombatObject&o){if(c.vertexCount<3||c.vertexCount>16||c.embeddedVertexCount<3||c.embeddedVertexCount>16||c.hitSpeed<=0)return false;*this=Stalactite{};config_=c;position=c.spawn;object_=&o;o={};o.name="Stalactite";o.layer=c.layer;o.receiver=this;o.onHit=receive;sync();return true;}
std::size_t Stalactite::polygon(Vec2*v)const{float radians=rotation*0.017453292519943295f,s=std::sin(radians),c=std::cos(radians);unsigned count=state==StalactiteState::Embedded?config_.embeddedVertexCount:config_.vertexCount;for(unsigned i=0;i<count;++i){auto p=state==StalactiteState::Embedded?config_.embeddedVertices[i]:config_.vertices[i];if(state==StalactiteState::Embedded){p.x+=config_.embeddedOffset.x;p.y+=config_.embeddedOffset.y;}v[i]={position.x+p.x*c-p.y*s,position.y+p.x*s+p.y*c};}return count;}
Rect Stalactite::bounds()const{Vec2 p[16];auto n=polygon(p);Rect r{p[0].x,p[0].y,p[0].x,p[0].y};for(std::size_t i=1;i<n;++i){r.left=std::min(r.left,p[i].x);r.right=std::max(r.right,p[i].x);r.bottom=std::min(r.bottom,p[i].y);r.top=std::max(r.top,p[i].y);}return r;}
bool Stalactite::overlapsBody(Rect r)const{Vec2 p[16];return polygonOverlapsBox(p,polygon(p),r);}
void Stalactite::sync(){if(object_){object_->bounds=bounds();object_->layer=state==StalactiteState::Embedded?config_.embeddedLayer:config_.layer;object_->colliderEnabled=active();}}
void Stalactite::fall(float delay){++cues[unsigned(StalactiteCue::Start)];position.y-=config_.fallOffset;state=StalactiteState::Warning;timer_=delay;sync();}
void Stalactite::update(float dt,const Hero&h,const World&w){if(!active())return;bool inside=overlaps(boxBounds(position,config_.trigger),boxBounds(h.position,config_.heroHurtbox));if(state==StalactiteState::Hanging&&inside&&!triggerInside_&&visible(position,h.position,w))fall(config_.fallDelay);triggerInside_=inside;if(state==StalactiteState::Warning){timer_-=dt;if(timer_<=0){state=StalactiteState::Falling;++cues[unsigned(StalactiteCue::Fall)];damagesHero=!heroDamageRemoved_;damagesEnemies=true;}}sync();}
void Stalactite::fixedUpdate(float dt,const World&w){
 PresentationPosition::Step step(presentation_,position);
 if(state!=StalactiteState::Falling)return;
 if(gravity_)velocity.y+=config_.gravity*dt;
 Vec2 p[16];auto n=polygon(p);Vec2 delta{velocity.x*dt,velocity.y*dt};
 float len=std::sqrt(delta.x*delta.x+delta.y*delta.y),travel=len;bool land=false;
 for(std::size_t i=0;i<n&&len>0;++i){float distance=0;
  if(rayTerrain(p[i],{delta.x/len,delta.y/len},len,w,distance)){travel=std::min(travel,distance);land=true;}}
 float t=len>0?travel/len:0;position.x+=delta.x*t;position.y+=delta.y*t;
 if(land){
  // Native trigger enter freezes the completed physics step, which can be
  // below first contact. Only use a recorded pose near this vertical impact;
  // nail deflections and contacts with a different surface retain their sweep.
  const float depth=position.y-config_.verticalRestY;
  if(config_.landingStep>0&&gravity_&&rotation==0&&position.x==config_.spawn.x&&
     velocity.y<0&&depth>=0&&depth<=-velocity.y*config_.landingStep)
   position.y=config_.verticalRestY;
  terrainEnter();
 }
 sync();
}
void Stalactite::terrainEnter(){if(state==StalactiteState::Falling){state=StalactiteState::Embedded;++cues[unsigned(StalactiteCue::Land)];velocity={};damagesHero=false;damagesEnemies=false;sync();}}
void Stalactite::contactHero(Hero&h)const{if(active()&&damagesHero&&overlapsBody(boxBounds(h.position,config_.heroHurtbox)))h.takeDamage(1,h.position.x<position.x?1:-1);}
void Stalactite::receive(void*self,const NailHit&h,HitOutcome){static_cast<Stalactite*>(self)->nail(h);}
void Stalactite::nail(const NailHit&h){if(!active())return;if(state==StalactiteState::Embedded){++cues[unsigned(StalactiteCue::Break)];lastStrike=h.direction;state=StalactiteState::Broken;sync();return;}if(state==StalactiteState::Hanging)fall(0);heroDamageRemoved_=true;damagesHero=false;lastStrike=h.direction;if(h.direction==Strike::Up){++cues[unsigned(StalactiteCue::UpBreak)];state=StalactiteState::Broken;damagesEnemies=false;sync();return;}++cues[unsigned(StalactiteCue::Hit)];rotation=h.direction==Strike::Right?45:h.direction==Strike::Left?-45:0;float r=rotation*0.017453292519943295f;velocity={std::sin(r)*config_.hitSpeed,-std::cos(r)*config_.hitSpeed};gravity_=false;sync();}
}
