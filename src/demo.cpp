#include "demo.hpp"

#include <algorithm>

namespace hk3ds
{
void updateRectangle(Rectangle& rectangle, const InputState& input, float stepSeconds)
{
    rectangle.x += input.horizontal * kMoveSpeed * stepSeconds;
    rectangle.y += input.vertical * kMoveSpeed * stepSeconds;

    rectangle.x = std::clamp(rectangle.x, 0.0f, kScreenWidth - kRectangleWidth);
    rectangle.y = std::clamp(rectangle.y, 0.0f, kScreenHeight - kRectangleHeight);
}
}
