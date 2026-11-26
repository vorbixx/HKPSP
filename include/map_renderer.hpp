#pragma once
#include "map_view.hpp"
#include "collectibles.hpp"
#include "ui.hpp"
#include <citro2d.h>
namespace hk3ds {
class MapRenderer {
public:
    bool load();
    void release();
    bool needsDraw(const SimpleMap& map,Vec2 hero,const CollectibleProgress* inventory=nullptr,bool compass=true) const;
    void invalidate(){revision_=~0u;}
    void draw(const SimpleMap& map,Vec2 hero,const CollectibleProgress* inventory=nullptr,bool compass=true,ui::Canvas canvas={});
private:
    C2D_SpriteSheet page_=nullptr;
    MapCamera camera_{};
    int panX_=-1,panY_=-1;
    int x_=-1,y_=-1;
    bool compass_=true;
    unsigned revision_=~0u;
    unsigned inventoryRevision_=~0u;
    C2D_TextBuf text_=nullptr;
    std::size_t current_=~std::size_t(0);
    void countsText(const char*,ui::Canvas);
    void image(unsigned index,Vec2 anchor,float pixelsPerUnit,u32 color);
};
}
