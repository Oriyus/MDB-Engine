#pragma once
#include <cstdint>

namespace Engine {

    struct RenderStats {
        static inline uint32_t DrawCalls = 0;
        static inline uint32_t Triangles = 0;
        static void Reset() { DrawCalls = 0; Triangles = 0; }
    };

} // namespace Engine