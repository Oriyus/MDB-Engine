#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

    class Camera;
    class Texture2D;

    // One point or spot light of the frame, as Scene::BuildDrawList gathers it (DrawList::Lights). The first entries
    // are the direct slots: index i < LightClusters::MaxDirect with Shadows set is shaded from uniforms. A slot nobody
    // holds is a placeholder (Range 0, no colour).
    struct PointLightData {
        glm::vec3 Pos{ 0.0f };
        glm::vec3 Color{ 0.0f };     // already multiplied by intensity (linear)
        float Range = 0.0f;          // metres: the light fades to nothing there (0 = a placeholder)
        float Radius = 0.0f;         // its size (PointLightComponent::Radius)
        bool Contact = true;         // contact shadows (PointLightComponent::ContactShadows)
        // An area light (PointLightComponent::Shape: 0 sphere, 1 tube, 2 rect): AreaX = its local X axis x half the
        // width (a tube's half length), AreaHalfY = a rect's half height, Extent = how far it reaches past its middle.
        // Dir is its -Z (a rect shines that way)
        int Shape = 0;
        glm::vec3 AreaX{ 0.0f };
        float AreaHalfY = 0.0f, Extent = 0.0f;
        bool Shadows = false;        // holds a shadow slot (its index) this frame
        uint32_t Entity = 0xFFFFFFFFu;   // entt integral (placeholders: none)
        // Spot lights: the direction it shines (world, unit) and the cosines of the cone's outer and inner half
        // angles (fully lit inside the inner one, dark outside the outer one). A point light: -2 and -1 (every way)
        glm::vec3 Dir{ 0.0f, 0.0f, -1.0f };
        float CosOuter = -2.0f, CosInner = -1.0f;
        bool IsSpot() const { return CosOuter > -1.5f; }
        // A light shadowed through the shadow atlas: its entry (ShadowContext::AtlasEntries) and its squares' records
        // in the tile list (LightClusters::SetShadowTiles): AtlasSpot = a spot's one square, AtlasCube = the first of six
        // (one per cube face, +X, -X, +Y, -Y, +Z, -Z) for a point light or a spot wider than SpotMapMaxCosOuter; -1 = none
        int AtlasEntry = -1;
        int AtlasSpot = -1;
        int AtlasCube = -1;
        // A cookie: an image the light shines through (spots: projected along the cone; points: a panorama round the
        // light, its middle along Dir), and its turn about Dir (radians, from the basis CookieBasis builds). A flipbook
        // cookie is a grid of CookieGrid frames (left to right, top row first), showing CookieFrame.
        // Profile: an IES profile baked into an image (IesProfile::Bake), its axis along Dir. Dir is set for any light
        // with a cookie or profile (points: the entity's -Z)
        const Texture2D* Cookie = nullptr;
        float CookieRoll = 0.0f;
        glm::ivec2 CookieGrid{ 1, 1 };
        int CookieFrame = 0;
        const Texture2D* Profile = nullptr;
        static constexpr float SpotMapMaxCosOuter = 0.5f;          // half angle 60 degrees: a 120 degree cone
        bool UsesSpotMap() const { return IsSpot() && CosOuter >= SpotMapMaxCosOuter; }
        // From PointLightComponent::SpotAngle (full angle, degrees; 0 = point) and SpotBlend (0..1)
        void SetCone(float spotAngleDegrees, float blend);
    };

    // How bright a point light is at distance d, before its colour: the engine's soft falloff times a window that
    // reaches 0 at the range. lit.frag (lights.glsl) uses the same formula.
    float PointLightFalloff(float d, float range);
    // The range of a light with no range set: where its falloff drops below 1/100 of a unit of light (an intensity 1
    // white light: about 70 m), at most 100 m
    float AutoPointLightRange(const glm::vec3& colorTimesIntensity);
    // The smallest sphere (centre, radius) holding everything a light reaches: its range sphere, or for a spot light
    // the cone out to its range (light_clusters.comp computes the same)
    glm::vec4 PointLightBounds(const PointLightData& light);
    // A shadowed spot light's view-projection (its cone plus a little room for the soft filtering, out to its range)
    glm::mat4 SpotShadowMatrix(const PointLightData& light);
    // One square of the shadow atlas as the shaders read it (storage buffer binding 9): the view-projection it was drawn
    // with, and Rect = (corner x, y, size) / atlas size and its far plane (Size 0: no square, lit)
    struct ShadowTileGpu { glm::mat4 ViewProj{ 1.0f }; glm::vec4 Rect{ 0.0f }; };
    // A cube face's view-projection for the atlas: 90 degrees plus a guard band of a few texels, so the soft filtering
    // of a point near the face's edge stays inside its square (lights.glsl picks the face by the major axis)
    glm::mat4 AtlasFaceMatrix(const glm::vec3& pos, int face, uint32_t tile, float farPlane);

    // The plane across a spot's cone that cookies are laid in, before their roll: right and up, perpendicular to dir
    // (lights.glsl builds the same)
    void CookieBasis(const glm::vec3& dir, glm::vec3& right, glm::vec3& up);

    // Clustered lighting: the camera's view split into Tiles X x Tiles Y screen tiles and Slices depth slices
    // (logarithmic from the near plane to Far), each listing the lights whose bounds reach into it. Three storage
    // buffers: lights (binding 5), cells = (first, count) per cluster (6) and the light indices the cells point into
    // (7). Shaders (lights.glsl) find a pixel's cluster from its screen position and view depth and shade only those.
    //
    // Binning (which lights go into which cluster) runs either on the GPU (`light_clusters.comp`, one work group per
    // screen tile: each light's bounds against the tile once, then against the boxes of the slices it spans; lights
    // stay in index order) or on the CPU
    // (each light's bounds against each slice's tiles). The GPU path reads its stats back three frames later
    // (MaxPerCluster, VisibleLights, References lag), and grows its index list when one frame needed more.
    class LightClusters {
    public:
        static constexpr int TilesX = 16, TilesY = 9, Slices = 24;
        static constexpr int Count = TilesX * TilesY * Slices;
        static constexpr uint32_t MaxLights = 4096;                // gathered beyond this are dropped (warned once)
        static constexpr uint32_t GpuMaxPerCluster = 256;          // the GPU path keeps at most this many per cluster

        LightClusters();
        ~LightClusters();
        LightClusters(const LightClusters&) = delete;
        LightClusters& operator=(const LightClusters&) = delete;

        // The direct (shadow slot) light i's cookie for the uniforms (Build): (cookie layer, roll, profile layer, 0) and
        // (columns, rows, frame, 0); layers -1 = none
        glm::vec4 DirectCookie(int i) const { return m_DirectCookie[i]; }
        glm::vec4 DirectFrames(int i) const { return m_DirectFrames[i]; }
        static constexpr int MaxDirect = 4;
        // The atlas squares of this frame (binding 9; uploaded by Upload, bound by Bind)
        void SetShadowTiles(std::vector<ShadowTileGpu> tiles) { m_Tiles = std::move(tiles); }

        // Every light without a direct slot (Shadows false, Range > 0): the most useful shadowed ones (at most
        // MaxDirect) are shaded directly from uniforms, which is cheaper for a handful of lights.
        // gpu: bin with the compute shader in Upload (falls back to the CPU if it isn't available)
        void Build(const std::vector<PointLightData>& lights, const Camera& camera, bool gpu = false);
        void Upload();
        void Bind();                                                // bindings 5, 6, 7 and the cookies on unit 20 (dispatches a pending GPU binning first)
        // The GPU binning of the last Upload. Bind runs it (the first SetLightingUniforms of the frame, in the depth
        // pre-pass: by then the frame's uploads are done; dispatching right after them made the GPU wait on them).
        // Does nothing if none is pending
        void Dispatch();

        // The compute shader (`<shaderDir>/light_clusters.comp`), shared by every LightClusters: SceneRenderer sets the
        // folder; it is built on first use. GpuAvailable: it compiled. ReloadShader: F5.
        static void SetShaderDir(const std::string& dir);
        static bool GpuAvailable();
        static void ReloadShader();

        // For the shaders: slice = log(view depth) * ZScale + ZBias
        float ZScale() const { return m_ZScale; }
        float ZBias() const { return m_ZBias; }
        const glm::vec4& ViewRow() const { return m_ViewRow; }   // row 2 of the view matrix: view z = dot(row, (p, 1))

        // Stats (CPU: the last Build; GPU: a frame read back a few frames later)
        uint32_t LightCount() const { return m_LightCount; }       // lights uploaded (clustered)
        uint32_t VisibleLights() const { return m_Visible; }       // lights in at least one cluster
        uint32_t MaxPerCluster() const { return m_MaxPerCluster; }
        uint32_t References() const { return m_References; }      // (cluster, light) pairs
        bool BinnedOnGpu() const { return m_Gpu; }                 // the last Build

        // A cluster's lights (tests, debug): the light indices. CPU binning only (empty after a GPU Build)
        std::vector<uint32_t> LightsAt(int x, int y, int slice) const;

    private:
        void DispatchGpu();
        int CookieLayer(const Texture2D* cookie);
        void UploadCookies();
        void ReadGpuStats();

        struct GpuLight { glm::vec4 PosRange; glm::vec4 Color; glm::vec4 Spot; glm::vec4 Shadow; glm::vec4 Cookie; glm::vec4 Extra; glm::vec4 Area; };   // std430: 112 bytes (Extra = radius, contact shadows 1 / 0, shape, extent; Area = X axis x half width, half height) (Color.w cos inner, Spot = dir, cos outer, Shadow = spot map layer, far cube layer, cookie layer (-1 = none), cookie roll, Cookie = profile layer, columns, rows, frame)
        glm::vec4 m_DirectCookie[MaxDirect]{}, m_DirectFrames[MaxDirect]{};
        std::vector<GpuLight> m_Lights;
        std::vector<ShadowTileGpu> m_Tiles;
        uint32_t m_TileBuffer = 0;
        size_t m_TileCapacity = 0;
        std::vector<const PointLightData*> m_Kept;                 // Build scratch: the lights clustered
        std::vector<glm::uvec2> m_Cells;                            // per cluster: first index, count
        std::vector<uint32_t> m_Indices;
        std::vector<std::pair<uint32_t, uint32_t>> m_Pairs;         // Build scratch: (cluster, light)
        float m_ZScale = 0.0f, m_ZBias = 0.0f;
        float m_Near = 0.1f, m_Far = 500.0f, m_TanH = 1.0f, m_TanV = 1.0f;
        glm::mat4 m_View{ 1.0f };
        glm::vec4 m_ViewRow{ 0.0f, 0.0f, -1.0f, 0.0f };
        uint32_t m_LightCount = 0, m_Visible = 0, m_MaxPerCluster = 0, m_References = 0;
        bool m_Warned = false, m_Gpu = false, m_WarnedFull = false;
        uint32_t m_Buffers[3] = { 0, 0, 0 };
        size_t m_Capacity[3] = { 0, 0, 0 };
        // GPU binning: the index list's size (in indices), and the stats read back through a ring of slots (a fence each)
        uint32_t m_GpuIndexCapacity = 0;
        static constexpr int kStatsRing = 3;
        uint32_t m_StatsBuffer = 0, m_Readback = 0;               // counters (GPU memory); their copies, mapped
        void* m_ReadbackPtr = nullptr;
        void* m_Fence[kStatsRing] = {};
        int m_StatsSlot = 0;
        bool m_DispatchPending = false;
        // Cookies and profiles: each distinct image copied (scaled) into a layer of one sRGB array (unit 20), kept while used,
        // the least recently used layer given up when a new one is needed
        static constexpr int CookieLayers = 12, CookieSize = 512;
        struct CookieSlot { const Texture2D* Tex = nullptr; uint32_t Id = 0; uint64_t LastUsed = 0; bool Dirty = false; };
        CookieSlot m_Cookies[CookieLayers];
        uint32_t m_CookieTex = 0, m_CookieFbo[2] = { 0, 0 };
        uint64_t m_Frame = 0;
        bool m_WarnedCookies = false;
    };

} // namespace Engine
