#pragma once
#include <cstdint>

namespace Engine {

    // Counters for the frame being drawn. Reset() runs at the start of every frame and keeps the
    // finished frame's numbers in LastFrame (what scripts and the game read).
    struct RenderStats {
        struct Counts { uint32_t DrawCalls = 0, Triangles = 0, Instances = 0, Culled = 0, ShadowSkipped = 0, StaticBatches = 0, StaticMerged = 0, Particles = 0, Decals = 0, Foliage = 0, LodReduced = 0, LodCulled = 0; };

        static inline uint32_t DrawCalls = 0;
        static inline uint32_t Triangles = 0;
        static inline uint32_t Instances = 0;     // objects drawn by instanced calls (camera + shadow passes)
        static inline uint32_t Culled = 0;        // objects outside the camera view, not drawn by it
        static inline uint32_t ShadowSkipped = 0; // (caster, shadow view) pairs skipped: outside that light's view or reach
        static inline uint32_t StaticBatches = 0; // merged static meshes drawn this frame (Scene::BuildStaticBatches)
        static inline uint32_t StaticMerged = 0;  // objects they replace
        static inline uint32_t Particles = 0;     // particles drawn this frame
        static inline uint32_t Decals = 0;        // decals drawn this frame
        static inline uint32_t Foliage = 0;       // grass / flower clumps drawn this frame (FoliageRenderer)
        static inline uint32_t LodReduced = 0;    // mesh renderers drawn at a coarser level of detail
        static inline uint32_t LodCulled = 0;     // mesh renderers beyond their cull distance
        static inline Counts LastFrame;

        static void Reset() {
            LastFrame = { DrawCalls, Triangles, Instances, Culled, ShadowSkipped, StaticBatches, StaticMerged, Particles, Decals, Foliage, LodReduced, LodCulled };
            DrawCalls = 0; Triangles = 0; Instances = 0; Culled = 0; ShadowSkipped = 0; StaticBatches = 0; StaticMerged = 0; Particles = 0; Decals = 0; Foliage = 0; LodReduced = 0; LodCulled = 0;
        }
    };

} // namespace Engine
