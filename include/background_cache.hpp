#pragma once
#include "room_visual.hpp"

namespace hk3ds {
// A prefix still needs live particles and blur. Avoid its extra capture pass
// while the camera moves; full-background reuse retains its subpixel threshold.
class BackgroundCacheCamera {
    const void* owner_=nullptr;
    unsigned revision_=0;
    float x_=0,y_=0;
    bool valid_=false;
public:
    bool stationary(const void* owner,unsigned revision,float x,float y) {
        const bool same=valid_&&owner==owner_&&revision==revision_&&x==x_&&y==y_;
        owner_=owner;revision_=revision;x_=x;y_=y;valid_=true;
        return same;
    }
};
// Live particles retain painter order and blur: only room draws preceding all
// far particles may be restored from an unblurred cache.
struct BackgroundCachePlan {
    std::size_t prefix;
    bool live=false;
};
inline BackgroundCachePlan backgroundCachePlan(const RoomVisualBank& room,
                                               const RoomVisualBank* ambient,
                                               const RoomVisualBank* decor,bool allowPrefix=true) {
    BackgroundCachePlan plan{room.draws.size()};
    for(const auto* bank:{decor,ambient})if(bank)for(const auto& draw:bank->draws){
        if(!roomDrawHasOpacity(draw)||!roomDrawBehindHero(draw)||draw.sprite>=bank->atlas.sprites.size())continue;
        RoomSpriteClip clip;
        if(!roomEffectClipSprite(bank->atlas.sprites[draw.sprite],draw,true,clip,room.blurSplitDepth))continue;
        plan.live=true;
        if(!allowPrefix)return {0,true};
        std::size_t first=0;
        const auto& v=draw.values;
        while(first<plan.prefix&&roomDrawBefore(room.draws[first],v[11],v[12],v[6]))++first;
        plan.prefix=first;
    }
    return plan;
}
// Compare against the last rendered image, never against the preceding frame:
// subpixel camera/animation motion must eventually trigger a refresh.
class BackgroundCacheInputs {
    struct Sample {
        std::array<float,16> values;
        SpriteFrame sprite;
        std::uint32_t owner,group;
    };
    static constexpr unsigned capacity=512;
    std::vector<Sample> rendered_,pending_;
    const void* owner_=nullptr;
    unsigned revision_=0,features_=0;
    bool valid_=false,overflow_=false;
    float focal_=0,ppu_=0;
public:
    using Visibility=float(*)(std::uint32_t,void*);
    static constexpr float motionPixels=.25f;
    void invalidate(){valid_=false;}
    void begin(const void* owner,unsigned revision,unsigned features,float ppu=0) {
        if(owner!=owner_||revision!=revision_||features!=features_||ppu!=ppu_)invalidate();
        ppu_=ppu;
        owner_=owner;revision_=revision;features_=features;
        pending_.clear();overflow_=false;focal_=roomFocalLength();
    }
    void append(const RoomVisualBank& bank,unsigned group,float split,
                float cameraX,float cameraY,float time,bool grassVisible,
                Visibility visible=nullptr,void* context=nullptr,bool behindOnly=false,
                std::size_t last=std::numeric_limits<std::size_t>::max()) {
        for(std::size_t index=0;index<std::min(last,bank.draws.size());++index){
            // Match the renderer's conservative bounds, including grass sway.
            // Offscreen animation cannot change the cached image.
            if(bank.cameraBounds.size()==bank.draws.size()&&!bank.cameraBounds[index].contains(cameraX,cameraY))continue;
            const auto& draw=bank.draws[index];
            if(!roomDrawHasOpacity(draw)||(behindOnly&&!roomDrawBehindHero(draw)))continue;
            if(!grassVisible&&(draw.grass.amplitude!=0||draw.grass.push!=0))continue;
            const float alpha=visible?visible(draw.owner,context):1;
            if(alpha<=0||draw.sprite>=bank.atlas.sprites.size())continue;
            RoomSpriteClip clip{bank.atlas.sprites[draw.sprite]};
            if(!roomEffectClipSprite(bank.atlas.sprites[draw.sprite],draw,true,clip,split))continue;
            if(pending_.size()==capacity){overflow_=true;return;}
            auto values=roomDrawValues(draw,time,bank.scaleFor(draw));
            values[2]-=cameraX;values[5]-=cameraY;values[10]*=alpha;
            pending_.push_back({values,clip.sprite,draw.owner,group});
        }
    }
    bool reusable() const {
        if(!valid_||overflow_||pending_.size()!=rendered_.size())return false;
        for(unsigned i=0;i<pending_.size();++i){
            const auto& a=pending_[i];const auto& b=rendered_[i];
            const auto& s=a.sprite;const auto& t=b.sprite;
            if(a.owner!=b.owner||a.group!=b.group||s.page!=t.page||s.x!=t.x||s.y!=t.y||
               s.width!=t.width||s.height!=t.height||s.left!=t.left||s.bottom!=t.bottom||
               s.right!=t.right||s.top!=t.top)return false;
            float depth=1000;
            for(float x:{s.left,s.right})for(float y:{s.bottom,s.top})
                depth=std::min(depth,38.1f+a.values[6]+a.values[13]*x+a.values[14]*y);
            const float scale=focal_/std::max(10.f,depth);
            for(unsigned k=0;k<a.values.size();++k){
                const float delta=std::fabs(a.values[k]-b.values[k]);
                if(!std::isfinite(delta))return false;
                if(k==2||k==5){if(delta*scale>motionPixels)return false;}
                else if(delta!=0)return false;
            }
        }
        return true;
    }
    void commit(){rendered_=pending_;valid_=!overflow_;}
};
}
