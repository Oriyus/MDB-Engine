#pragma once
#include <memory>

namespace Engine {

    class Scene;
    class Camera;
    class Shader;
    class DrawList;
    class Environment;
    struct ShadowContext;

    // Draws every FoliageComponent: grass and flower clumps, GPU-instanced (one storage buffer per field, split
    // into 8 m chunks for frustum culling, one instanced draw per visible chunk and kind). Per field it keeps
    //  - the clumps, scattered again when the settings, the entity's placement or (AvoidStatic) the Static meshes
    //    standing on the ground change (a jittered grid, so the cover is even; Seed makes it repeatable);
    //  - a trample map (RG32F, about 0.2 m texels): moving dynamic / kinematic bodies and foliage.push write a
    //    push away from themselves into it, and it fades back over Recovery seconds.
    // Wind and trampling are done in foliage.vert; the clumps are lit by lit.frag.
    class FoliageRenderer {
    public:
        FoliageRenderer();
        ~FoliageRenderer();
        FoliageRenderer(const FoliageRenderer&) = delete;
        FoliageRenderer& operator=(const FoliageRenderer&) = delete;

        // Into the bound HDR target, after the opaque scene (its lighting uniforms are set on `shader`). update: advance
        // time, wind and trampling (once a frame: the depth pre-pass draws with update = true, the lit pass with false,
        // so both see exactly the same blades)
        void Render(Scene& scene, const Camera& camera, Shader& shader, const DrawList& list,
                    const ShadowContext* shadows, const Environment* env, bool update = true);

        // Bind the ground-shade maps of the fields (storage buffer binding 10, read by lit.frag's GroundGrassShade) for
        // this camera: call before the opaque pass. Always binds (an empty list without grass), so another renderer's
        // maps are never read. The maps are made when a field scatters (Render), so with the depth pre-pass they are
        // this frame's, otherwise last frame's.
        void BindGroundShade(const Camera& camera);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
