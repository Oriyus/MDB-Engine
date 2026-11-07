#pragma once
#include <cstdint>

namespace Engine {

    // Counters for the frame being drawn. Reset() runs at the start of every frame and keeps the
    // finished frame's numbers in LastFrame (what scripts and the game read).
    struct RenderStats {
        struct Counts { uint32_t DrawCalls = 0, Triangles = 0, Instances = 0, Culled = 0; };

        static inline uint32_t DrawCalls = 0;
        static inline uint32_t Triangles = 0;
        static inline uint32_t Instances = 0;     // objects drawn by instanced calls (camera + shadow passes)
        static inline uint32_t Culled = 0;        // objects outside the camera view, not drawn by it
        static inline Counts LastFrame;

        static void Reset() {
            LastFrame = { DrawCalls, Triangles, Instances, Culled };
            DrawCalls = 0; Triangles = 0; Instances = 0; Culled = 0;
        }
    };

} // namespace Engine
