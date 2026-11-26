#include "hero_death.hpp"
#include "render_memory.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hk3ds {
static_assert(sizeof(HeroDeathPresentation)<=heroDeathStateBudgetBytes,"death presentation ordinary-heap budget");
namespace {
constexpr float pi=3.14159265358979323846f;
struct CurveKey { float time,value,inSlope,outSlope; };
template<unsigned N> float curve(float time,const CurveKey (&keys)[N])
{
    if(time<=keys[0].time)return keys[0].value;
    for(unsigned i=1;i<N;++i)if(time<keys[i].time){
        const auto& a=keys[i-1];const auto& b=keys[i];
        const float dt=b.time-a.time,t=(time-a.time)/dt,t2=t*t,t3=t2*t;
        return (2*t3-3*t2+1)*a.value+(t3-2*t2+t)*dt*a.outSlope+
            (-2*t3+3*t2)*b.value+(t3-t2)*dt*b.inSlope;
    }
    return keys[N-1].value;
}
}
float HeroDeathPresentation::flashOpacity(bool rays) const
{
    // Native AnimationClip 1133 / 1131 alpha Hermite keys (Sprites/Default).
    if(!active_)return 0;
    const CurveKey ring[]={{0,1,0,0},{.33333334f,0,5.364418e-7f,0}};
    const CurveKey ray[]={{0,1,0,0},{.1f,1,0,0},{.15f,0,5.8460214e-6f,0}};
    return std::clamp(rays?curve(elapsed_,ray):curve(elapsed_,ring),0.f,1.f);
}
float HeroDeathPresentation::flashScale(bool rays) const
{
    // X/Y differ by <0.000006 in the source; a uniform quad uses the X curve.
    const CurveKey ring[]={{0,2.6180742f,0,0},{.083333336f,3.800456f,12.151549f,12.151552f},
        {.16666667f,4.643333f,20.22905f,20.229046f},{.25f,10.109707f,2.2319211e-5f,0}};
    const CurveKey ray[]={{0,2.1074777f,0,0},{.05f,2.4657488f,6.295036f,6.295035f},
        {.1f,2.7369812f,-8.1237874e-7f,0}};
    return rays?curve(elapsed_,ray):curve(elapsed_,ring);
}
float HeroDeathParticle::drawSize() const
{
    const float t=std::clamp(age/lifetime,0.f,1.f);
    // approximation: linear interpolation of the exported particle size curves.
    if(kind==0){
        const float low=t<.83148503f?.5675675f+(.435755f-.5675675f)*t/.83148503f:
            .435755f+(.06756492f-.435755f)*(t-.83148503f)/(1-.83148503f);
        const float high=t<.79142064f?1-.0918519f*t/.79142064f:
            .9081481f+(.33783346f-.9081481f)*(std::min(t,.9970501f)-.79142064f)/(.9970501f-.79142064f);
        return size*(low+(high-low)*sizeCurveMix);
    }
    if(kind==1)return size*(t<.7415425f?.35135144f+(.6829743f-.35135144f)*t/.7415425f:
        .6829743f*(1-t)/(1-.7415425f));
    return size;
}
void HeroDeathPresentation::reset()
{
    for(auto& p:particles_)p.active=false;
    bank_=nullptr;body={};flash={};flashRays={};active_=nail_=head_=false;elapsed_=physicsElapsed_=0;emitted_={};
}
float HeroDeathPresentation::random(float low,float high)
{
    seed_=seed_*1664525u+1013904223u;
    return low+(high-low)*(float(seed_>>8)/16777216.f);
}
void HeroDeathPresentation::begin(const Hero& hero,const AnimationBank& bank)
{
    reset();bank_=&bank;active_=true;facingRight=hero.facingRight;seed_=0x484b4445u;
    // Hero Death child local X=.21; it inherits Knight's negative X when facing right.
    origin={hero.position.x+(facingRight?-.21f:.21f),hero.position.y};
    flashRotation=random(-15,15)*pi/180;
    body.play(bank.find("Death"),0,true);
    flash.play(bank.find("Death Flash"),0,true);flashRays.play(bank.find("Death Flash Rays"),0,true);
}
HeroDeathParticle* HeroDeathPresentation::spawn(unsigned kind,Vec2 position,Vec2 velocity,float lifetime,float size)
{
    for(auto& p:particles_)if(!p.active){
        p={};p.active=true;p.kind=kind;p.position=position;p.velocity=velocity;
        p.lifetime=lifetime;p.size=size;p.history.reset(position);
        const char* ash[]={"Death Ash0","Death Ash1","Death Ash2"};
        p.clip=bank_->find(kind<2?"Death Mote":kind<4?ash[unsigned(random(0,3))]:kind==4?"Death Nail":"Death Head Normal");
        if(kind==0){p.accelerationY=random(2,10);p.sizeCurveMix=random(0,1);}
        if(kind==1)p.accelerationY=random(0,.5f);
        if(kind==2||kind==3)p.spin=random(-1.7453293f,1.7453293f);
        return &p;
    }
    return nullptr;
}
void HeroDeathPresentation::update(float seconds)
{
    if(!active_||seconds<=0)return;
    elapsed_+=seconds;body.advance(seconds);flash.advance(seconds);flashRays.advance(seconds);
    // Hero Death Anim: Start Wait(.5), Blow, Ash Particle until Death completes.
    if(!nail_&&elapsed_>=.5f){
        nail_=true;const float a=random(50,130)*pi/180,s=random(18,22);
        if(auto* p=spawn(4,origin,{std::cos(a)*s,std::sin(a)*s},std::numeric_limits<float>::max(),1)){
                p->angle=170.0539f*pi/180;
                // approximation: SpinSelf's velocity-derived torque uses a damped spin.
                p->spin=-7.5f;
        }
    }
    if(!head_&&body.complete){
        head_=true;const bool positive=!facingRight;
        const float a=(positive?100.f:80.f)*pi/180;
        if(auto* p=spawn(5,{origin.x+(positive?.2f:-.2f),origin.y-.02f},
                         {15*std::cos(a),15*std::sin(a)},std::numeric_limits<float>::max(),1)){
            p->angle=(positive?-15.f:20.f)*pi/180;
            // approximation: initial torque represented as a damped angular velocity.
            p->spin=positive?2.f:-2.f;
        }
    }
}
void HeroDeathPresentation::fixedUpdate(float seconds,const World& world)
{
    if(!active_||seconds<=0)return;
    physicsElapsed_+=seconds;
    // Exported ParticleSystem rates, start delays, offsets, speeds and lifetimes.
    // Ash kinds 2/3 are resources.assets:7381 / resources.assets:7366.
    const float delays[]={.02f,.5f,.5f,.5f},durations[]={.13f,1.5f,1.3f,1.3f},rates[]={3000,40,30,30};
    for(unsigned k=0;k<4;++k){
        const unsigned target=unsigned(std::clamp(physicsElapsed_-delays[k],0.f,durations[k])*rates[k]+.0001f);
        while(emitted_[k]<target){
            ++emitted_[k];float a=random(0,2*pi),r=std::sqrt(random(0,1));
            Vec2 pos=origin,vel{};float life=5,size=1;
            if(k==0){pos.y-=.43f;pos.x+=r*1.2f*std::cos(a);pos.y+=r*1.2f*std::sin(a);
                const float speed=random(7,40);vel={speed*std::cos(a),speed*std::sin(a)};life=random(.7f,1.25f);}
            else if(k==1){pos.x+=facingRight?.16f:-.16f;pos.y-=.84f;
                pos.x+=r*.46243668f*std::cos(a);pos.y+=r*.46243668f*std::sin(a);life=random(1,2);size=random(.25f,1);}
            else{
                const float sign=facingRight?-1.f:1.f;
                pos.x+=sign*(k==2?-.31f:-.040000916f);pos.y+=k==2?-.57f:-.5199995f;
                const float rotation=(k==2?135.01247f:-37.758945f)*pi/180;
                a=rotation+random(0,80)*pi/180;const float speed=random(20,30)*.7602f;
                vel={sign*speed*std::cos(a),speed*std::sin(a)};size=random(.75f,1)*.7602f;
            }
            if(auto* p=spawn(k,pos,vel,life,size))if(k>=2)p->angle=random(0,2*pi);
        }
    }
    for(auto& p:particles_)if(p.active){
        PresentationPosition::Step step(p.history,p.position);
        p.age+=seconds;if(p.age>=p.lifetime){p.active=false;continue;}
        p.velocity.y+=p.accelerationY*seconds;
        if(p.kind==0){
            // approximation: LimitVelocityOverLifetime magnitude=0, dampen=.1 at 60 Hz.
            const float damping=std::pow(.9f,seconds*60);p.velocity.x*=damping;p.velocity.y*=damping;
        }
        // ParticleSystems use Physics.gravity; Rigidbody2D debris uses Physics2D.gravity.
        if(p.kind>=2)p.velocity.y-=(p.kind<4?9.81f*2.f:60.f*(p.kind==4?.8f:1.5f))*seconds;
        p.angle+=p.spin*seconds;
        if(p.kind<4){p.position.x+=p.velocity.x*seconds;p.position.y+=p.velocity.y*seconds;}
        else{
            // approximation: swept boxes in the real room replace Unity's rotating collider.
            // Bounds of the exported PolygonCollider2D vertices, resources 6648 / 6436.
            const Rect bounds=p.kind==4?Rect{-.40625f,-.203125f,.4453125f,.171875f}:
                Rect{-.515625f,-.5703125f,.5078125f,.5234375f};
            const auto old=p.velocity;
            const auto contacts=moveBox(p.position,p.velocity,bounds,seconds,world);
            const float bounce=p.kind==4?.3f:.5f;
            if(contacts.ground&&old.y<-1)p.velocity.y=-old.y*bounce;
            if((contacts.left||contacts.right)&&std::abs(old.x)>1)p.velocity.x=-old.x*bounce;
            if(contacts.ground)p.velocity.x*=.9f;
            p.spin/=1+(p.kind==4?2.f:.5f)*seconds;
            if(p.kind==5&&contacts.ground)p.clip=bank_->find("Death Head Cracked");
        }
    }
}
unsigned HeroDeathPresentation::activeCount() const
{unsigned count=0;for(const auto& p:particles_)count+=p.active;return count;}
}
