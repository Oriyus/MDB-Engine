#pragma once
#include <glm/glm.hpp>
#include <memory>

namespace Engine {

    class Texture2D;

    // A shape projected down onto whatever is under it (ground, steps, ramps): target circles, area
    // templates, movement ranges, path markers. Scripts add them every frame (decal.draw, like ui.*);
    // SceneRenderer draws them after the opaque scene by rebuilding each pixel's world position from the
    // depth buffer and keeping the pixels inside the decal's box.
    struct Decal {
        enum class Shape { Circle, Square, Texture };
        glm::vec3 Position{ 0.0f };        // the centre of the box (on the ground)
        glm::vec3 Size{ 1.0f };            // box: width (x), height (y, how far up / down it reaches), depth (z)
        float Yaw = 0.0f;                  // radians, about the world up axis
        Shape Kind = Shape::Circle;
        glm::vec3 Color{ 1.0f };           // sRGB, like every colour picker
        float Alpha = 0.6f;
        float Thickness = 0.0f;            // 0 = filled; > 0 = an outline this wide (fraction of the half size)
        float Fill = 0.0f;                 // outlines: the inside's alpha, as a fraction of Alpha
        float Softness = 0.04f;            // edge blur (fraction of the half size)
        float Emission = 1.0f;             // brightness (> 1 glows with bloom)
        std::shared_ptr<Texture2D> Texture; // Shape::Texture: its alpha is the shape, its colour tints
    };

} // namespace Engine
