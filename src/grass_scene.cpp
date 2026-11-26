#include "grass_scene.hpp"
#include "breakable.hpp"
#include <algorithm>
#include <cmath>
namespace hk3ds {
namespace {
float depthGain(float z){return 1+std::fabs(std::clamp(z,-1.f,1.f));}
bool grassOverlaps(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
unsigned mix(unsigned x){x+=0x9e3779b9u;x=(x^(x>>16))*0x85ebca6bu;x=(x^(x>>13))*0xc2b2ae35u;return x^(x>>16);}
}
bool RoomGrass::bind(const GrassBank& bank,RoomVisualBank& room)
{
    if(bank.count!=grassCapacity||bank.spriteCount!=room.atlas.sprites.size()||bank.drawCount!=room.draws.size())return false;
    for(unsigned i=0;i<bank.count;++i){
        const auto& c=bank.bushes[i];unsigned d=0;
        for(;d<room.draws.size()&&room.draws[d].owner!=c.drawOwner;++d){}
        if(d==room.draws.size())return false;
        const auto& draw=room.draws[d];
        if(!states_[i].bind(c,draw.grass.push/depthGain(draw.values[6])))return false;
        originals_[i]=draw;draws_[i]=d;
        // Admit the entire attack envelope and every animation/replacement
        // geometry once, so camera culling cannot freeze a bent bush at an edge.
        RoomCameraBounds bounds{1e9f,1e9f,-1e9f,-1e9f};
        for(unsigned frame=0;frame<6;++frame){
            auto pose=draw;
            const unsigned sprite=frame<4?c.pushSprites[frame]:frame==4?c.idleSprite:c.stubSprite;
            if(sprite>=room.atlas.sprites.size())return false;
            if(frame==5){pose.values=c.stubValues;pose.grass={c.stubGrass[0],c.stubGrass[1],c.stubGrass[2],c.stubGrass[3],c.stubGrass[4]};}
            pose.grass.amplitude+=.31f*depthGain(pose.values[6]);
            if(!roomEffectSplitSupported(room.atlas.sprites[sprite],pose,room.blurSplitDepth))return false;
            const auto b=roomCameraBounds(room.atlas.sprites[sprite],pose);
            bounds.left=std::min(bounds.left,b.left);bounds.bottom=std::min(bounds.bottom,b.bottom);
            bounds.right=std::max(bounds.right,b.right);bounds.top=std::max(bounds.top,b.top);
        }
        if(room.cameraBounds.size()==room.draws.size())room.cameraBounds[d]=bounds;
    }
    bank_=&bank;room_=&room;restart();return true;
}
void RoomGrass::restart()
{
    players_={};nails_={};sounds_={};
    if(!bank_)return;
    for(unsigned i=0;i<bank_->count;++i){states_[i].restart();room_->draws[draws_[i]]=originals_[i];}
}
void RoomGrass::sound(unsigned i,GrassSound s)
{
    if(s!=GrassSound::None)++sounds_[i][static_cast<unsigned>(s)-1];
}
void RoomGrass::present(unsigned i)
{
    auto& draw=room_->draws[draws_[i]];const auto& c=bank_->bushes[i];
    if(states_[i].cut()){
        draw.values=c.stubValues;draw.grass={c.stubGrass[0],c.stubGrass[1],c.stubGrass[2],c.stubGrass[3],c.stubGrass[4]};
    }
    draw.sprite=states_[i].sprite();
    draw.grass.push=states_[i].push()*depthGain(draw.values[6]);
}
void RoomGrass::update(float seconds,const Hero& hero,const NailBank& nails,DecorEffects& fx,const World& world)
{
    if(!bank_||seconds<=0)return;
    const auto& shape=nails.shapes[nailVariant(hero.nail)];
    const auto attacker=nailPoint(hero,shape,{0,0});
    Rect attackBounds{1e9f,1e9f,-1e9f,-1e9f};
    for(unsigned v=0;v<shape.count;++v){const auto p=nailPoint(hero,shape,shape.vertices[v]);
        attackBounds.left=std::min(attackBounds.left,p.x);attackBounds.right=std::max(attackBounds.right,p.x);
        attackBounds.bottom=std::min(attackBounds.bottom,p.y);attackBounds.top=std::max(attackBounds.top,p.y);}
    const Vec2 attackCenter{(attackBounds.left+attackBounds.right)*.5f,(attackBounds.bottom+attackBounds.top)*.5f};
    for(unsigned i=0;i<bank_->count;++i){
        const auto& c=bank_->bushes[i];if(!c.interactive())continue;
        auto& state=states_[i];const Rect box{c.left,c.bottom,c.right,c.top};
        const bool player=grassOverlaps(bodyBounds(hero.position),box);
        if(player&&!players_[i])sound(i,state.enter(hero.position.x));
        if(!player&&players_[i])state.exitPlayer();
        players_[i]=player;
        sound(i,state.walk(hero.velocity.x));
        const bool nail=hero.nail.active&&hero.nail.hitbox&&nailOverlaps(hero,shape,box);
        if(nail&&!nails_[i]){
            // approximation: native trigger callback order is unprobed; execute
            // body reaction before cut, preserving both source sound events.
            sound(i,state.enter(attacker.x,GrassContact::Nail));
            const auto event=state.cut(attacker.x);sound(i,event);
            if(event==GrassSound::Cut)fx.grassCut(c.owner,{(attackCenter.x+(c.left+c.right)*.5f)*.5f,
                (attackCenter.y+(c.bottom+c.top)*.5f)*.5f},c.z,hero.nail.direction,world);
        }
        nails_[i]=nail;state.advance(seconds);present(i);
    }
}
bool RoomGrass::cut(unsigned owner,const Hero& hero,DecorEffects& fx,const World& world)
{
    if(!bank_)return false;
    for(unsigned i=0;i<bank_->count;++i)if(bank_->bushes[i].owner==owner){
        const auto& c=bank_->bushes[i];const auto event=states_[i].cut(hero.position.x);
        if(event==GrassSound::None)return false;
        sound(i,event);fx.grassCut(owner,{(c.left+c.right)*.5f,(c.bottom+c.top)*.5f},c.z,
            hero.facingRight?Strike::Right:Strike::Left,world);present(i);return true;
    }
    return false;
}
unsigned RoomGrass::cuts() const
{
    unsigned result=0;if(bank_)for(unsigned i=0;i<bank_->count;++i)result+=states_[i].cut();return result;
}
void RoomGrass::dispatchAudio(AudioVoices& voices,AudioCursor& cursor,Vec2 listener) const
{
    if(!bank_)return;
    constexpr unsigned first[]={unsigned(EnemyClip::GrassMove1),unsigned(EnemyClip::GrassStub1),unsigned(EnemyClip::GrassCut1)};
    constexpr unsigned count[]={6,5,4};
    for(unsigned i=0;i<bank_->count;++i){
        const auto& source=bank_->bushes[i];
        for(unsigned b=0;b<3;++b){
            while(cursor[i][b]<sounds_[i][b]){
                const auto random=mix(source.owner^mix(++cursor[i][b])^mix(b));
                const unsigned variant=(std::uint64_t(random)*count[b])>>32;
                voices.play(static_cast<EnemyClip>(first[b]+variant),source.owner,random);
            }
            cursor[i][b]=sounds_[i][b];
        }
        // GrassBehaviour.PlayRandomSound uses this object's AudioSource for
        // move, cut and stub cues. Keep playing one-shots camera-relative too.
        // All 41 level6 grass sources have blend/volume 1, linear 39..50.
        // The whole/stub draw includes the existing GrassBehaviour.Start Z
        // jitter sample; keep sound and presentation on that same depth.
        const auto& active=voices.voices();
        if(std::any_of(active.begin(),active.end(),[&](const AudioVoice& v){return v.active&&v.owner==source.owner;}))
            voices.updateOwnerMix(source.owner,linearSpatialMix(source.x-listener.x,source.y-listener.y,
                originals_[i].values[6]+38.1f,39,50));
    }
}
}
