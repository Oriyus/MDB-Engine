#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

    class Shader;

    // The shadow atlas: one square 16-bit depth texture (made on first use) holding the shadow maps of every shadowed
    // light beyond the 4 sharp cubes: a spot light of at most 120 degrees takes one square (a perspective view of its
    // cone), a point light (or a wider spot) one square per cube face it needs. Squares are powers of two from MinTile
    // up, handed out by a buddy allocator (a free list per size; four free neighbours merge back), so a light keeps its
    // squares, and their cached contents, while its size stays the same. Like the cubes, each square stores the
    // distance from the light / its far plane (shadow.frag), compared in hardware (sampler2DShadow).
    class ShadowAtlas {
    public:
        static constexpr uint32_t MinTile = 64;

        explicit ShadowAtlas(uint32_t size = 4096);
        ~ShadowAtlas();
        ShadowAtlas(const ShadowAtlas&) = delete;
        ShadowAtlas& operator=(const ShadowAtlas&) = delete;

        uint32_t Size() const { return m_Size; }
        // A new size (a power of two, 1024..8192): every square is given up and the texture made again on next use
        void Resize(uint32_t size);

        // A free tile x tile square (tile a power of two, MinTile..Size): its corner in texels. False if none is free
        bool Alloc(uint32_t tile, glm::uvec2& at);
        void Free(const glm::uvec2& at, uint32_t tile);
        uint64_t UsedArea() const { return m_Used; }           // texels handed out

        void BindTile(const glm::uvec2& at, uint32_t tile) const;   // FBO, viewport and scissor on it, depth cleared (allocates first)
        void EndTiles() const;                                  // scissor off
        // texture 0 until allocated; rawUnit: the same texture again, read as plain depth (no comparison: PCSS blocker search)
        void BindTexture(uint32_t unit, uint32_t rawUnit) const;
        bool Allocated() const { return m_Tex != 0; }
        // A view of the texture to look at (Viewport > Debug > Shadow Atlas): no comparison, depth as grey; 0 until allocated
        uint32_t DebugView() const;

    private:
        int Level(uint32_t tile) const;                         // 0 = the whole atlas
        void Allocate() const;
        void Release();
        mutable uint32_t m_FBO = 0, m_Tex = 0;
        mutable uint32_t m_RawSampler = 0, m_DebugView = 0;
        uint32_t m_Size = 0;
        uint64_t m_Used = 0;
        std::vector<std::vector<glm::uvec2>> m_Free;            // per level: free squares' corners
    };

    // The sun's depth maps: one layer per cascade (a 2D array texture). Cascades cover slices of the
    // camera view, near ones sharper; with cascades off only layer 0 is used, for a fixed area.
    class DirectionalShadowMap {
    public:
        static constexpr uint32_t MaxCascades = 4;

        explicit DirectionalShadowMap(uint32_t size = 2048);
        ~DirectionalShadowMap();
        DirectionalShadowMap(const DirectionalShadowMap&) = delete;
        DirectionalShadowMap& operator=(const DirectionalShadowMap&) = delete;

        void Bind(uint32_t cascade) const;                    // binds FBO to that layer, sets viewport, clears depth
        void BindTexture(uint32_t slot) const;
        uint32_t GetSize() const { return m_Size; }

    private:
        uint32_t m_FBO = 0, m_Tex = 0, m_Size = 0;
    };

    struct ShadowSettings {
        bool  Enabled = true;
        float PointBias = 0.05f;      // world units
        float PointFar = 30.0f;       // how far point-light shadows reach
        // Re-render a still point light's shadow cube only every N frames (staggered across lights). A light that moved, or
        // has a moving object or an animated model in reach, is re-rendered at once; swaying (wind) does not count, so
        // tree shadows from lamps update at the lower rate. 1 = every frame. Saved as shadows.pointInterval.
        int   PointUpdateInterval = 2;
        // Shadowed lights beyond the 4 direct ones: the next AtlasLights most useful (points and spots) get
        // squares of the shadow atlas, sized by how much of the view their reach covers, and are shaded through the
        // clusters (0..MaxAtlasLights; the 4 direct lights have squares too, the biggest). The atlas is AtlasSize texels
        // square (1024..8192; 16-bit: 128 MB at 8192, 32 MB at 4096).
        // Saved as shadows.atlasLights / atlasSize (older files: extraPointShadows).
        static constexpr int MaxAtlasLights = 64;
        int   AtlasLights = 24;
        int   AtlasSize = 8192;
        // Contact shadows: a short ray from each lit pixel towards each point / spot light through the depth of the
        // scene, darkening what the shadow maps miss (small things, the gap under an object, lights without shadows).
        // ContactLength = the ray's length in metres (0 = off). Saved as shadows.contactLength.
        float ContactLength = 0.25f;
        bool  SunContact = true;      // the sun gets contact shadows too (one more ray a pixel). Saved as shadows.sunContact.
        float SunBias = 0.002f;       // sun depth bias; scaled to each cascade's texel size (0.002 = default)
        // Cascaded sun shadows follow the camera: CascadeCount slices up to ShadowDistance from it
        bool  SunCascades = true;
        int   CascadeCount = 4;         // 1..4
        float ShadowDistance = 60.0f;   // how far from the camera sun shadows reach
        float CascadeSplit = 0.75f;     // 0 = even slices, 1 = logarithmic (more detail near the camera)
        bool  CascadeDebug = false;     // tint each cascade (a debug view, not saved)
        // Without cascades: a fixed area around the origin
        float SunExtent = 12.0f;      // half-width of the area around the origin the sun shadows cover
        float SunDistance = 30.0f;    // how far back the sun "camera" sits (casters this far behind still count)
    };

    // Everything the scene needs to render shadows, handed over by the app
    struct ShadowContext {
        ShadowAtlas* Atlas = nullptr;            // every shadowed point and spot light
        DirectionalShadowMap* Sun = nullptr;
        Shader* PointDepth = nullptr;
        Shader* SunDepth = nullptr;
        ShadowSettings Settings;

        // Shadow cache (runtime, kept across frames by the renderer): what a light's atlas squares hold
        // Cone: a spot light's direction + cos outer (which faces it draws; for a spot map, where it points); Range: its far plane
        struct PointCache { glm::vec3 Pos{ 0.0f }; glm::vec4 Cone{ 0.0f }; float Range = 0.0f; bool Valid = false; bool MovedNear = false; };
        mutable uint32_t Frame = 0;
        mutable uint32_t PointLightsDrawn = 0;      // lights re-rendered last frame (stats; spot maps included)
        // Which light holds each direct slot (LightClusters::MaxDirect = 4; entt integral, ~0u = none): kept while it
        // stays among the most useful shadowed lights (Scene::BuildDrawList assigns them)
        mutable uint32_t SlotOwner[4] = { ~0u, ~0u, ~0u, ~0u };
        // The atlas lights (Scene::BuildDrawList keeps them): each light's squares, kept while it stays among the
        // AtlasLights most useful and its size stays the same, and what they hold
        struct AtlasEntry {
            uint32_t Entity = ~0u;
            bool Cube = false;            // one square per face in FaceMask (a point light, or a spot wider than 120 degrees), else one
            uint8_t FaceMask = 0;         // cube faces (+X, -X, +Y, -Y, +Z, -Z) it holds squares for
            uint32_t Tile = 0;            // texels per side (0 = none: it got no room)
            glm::uvec2 At[6]{};           // each square's corner (a spot: At[0])
            float Far = 0.0f;             // the far plane (distance / Far is stored)
            PointCache Cache;
            bool Kept = false;            // still wanted this frame (scratch)
        };
        mutable std::vector<AtlasEntry> AtlasEntries;
        mutable uint32_t AtlasLightsHeld = 0;       // stats: atlas lights with squares this frame
        // The scene depth after the depth pre-pass (SceneRenderer copies it for the opaque and grass passes; 0 = none:
        // no contact shadows)
        uint32_t ContactDepth = 0;
        // Ambient occlusion for the lit passes (SceneRenderer computes it from the depth pre-pass first): the half-
        // resolution (AO, view depth) texture, and how strong; 0 = not here (applied afterwards, or off)
        uint32_t AOMap = 0;
        float AOStrength = 1.0f;
        glm::vec2 ScreenSize{ 1.0f };
    };

} // namespace Engine