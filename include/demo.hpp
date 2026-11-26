#pragma once

namespace hk3ds
{
constexpr float kScreenWidth = 400.0f;
constexpr float kScreenHeight = 240.0f;
constexpr float kRectangleWidth = 40.0f;
constexpr float kRectangleHeight = 30.0f;
constexpr float kMoveSpeed = 120.0f; // Pixels per second along each axis.

struct InputState
{
    int horizontal = 0;
    int vertical = 0;
};

struct Rectangle
{
    float x = (kScreenWidth - kRectangleWidth) / 2.0f;
    float y = (kScreenHeight - kRectangleHeight) / 2.0f;
};

void updateRectangle(Rectangle& rectangle, const InputState& input, float stepSeconds);
}
