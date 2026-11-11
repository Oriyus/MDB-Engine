#pragma once
#include "Renderer/Material.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Mesh;
    class Scene;

    // What a terrain brush does (Editor/src/TerrainEditor; TerrainData::ApplyBrush)
    enum class TerrainBrushMode { Raise, Lower, Smooth, Flatten, Noise, Paint };

    // One stroke step. Radius and the centre are in the terrain entity's own metres (X/Z, centred on it).
    struct TerrainBrush {
        TerrainBrushMode Mode = TerrainBrushMode::Raise;
        float Radius = 4.0f;          // metres
        float Strength = 0.5f;        // 0..1
        float Falloff = 0.6f;         // 0 = a hard edge, 1 = soft all the way from the centre
        float Target = 0.0f;          // Flatten: the height to level to (metres, the entity's space)
        int   Layer = 0;              // Paint: which layer (0..3)
        float NoiseScale = 3.0f;      // Noise: feature size in metres
        uint32_t Seed = 1;            // Noise
    };

    // Fractal noise for TerrainData::Generate
    struct TerrainNoise {
        float Height = 8.0f;          // metres from the lowest to the highest point
        float FeatureSize = 24.0f;    // metres: the size of the largest hills
        int   Octaves = 5;
        float Roughness = 0.5f;       // how much each smaller octave adds (persistence)
        float Ridges = 0.0f;          // 0 = round hills, 1 = sharp ridges (folded noise)
        uint32_t Seed = 1;
        bool  Add = false;            // add to the current heights instead of replacing them
    };

    // A heightmap terrain's data: heights (a square grid of Resolution^2 samples over the component's Size) and
    // paint weights (RGBA8 per texel = layers 0..3, SplatResolution^2 over the same area). It is shared by
    // snapshots (undo, Play): the editor copies it before every stroke, so a copy never changes under them.
    // Copies share the chunk meshes until they change (copy on write per chunk).
    // Stored in <Assets>/<DataPath> (".terrain": a small header, float heights, then the weights; Save / Load).
    // The surface between samples is two triangles per grid square split along (x, z) -> (x+1, z+1), the same as
    // Jolt's height field, so what is drawn, what HeightAt says and what bodies stand on agree.
    class TerrainData {
    public:
        static constexpr uint32_t ChunkQuads = 32;       // grid squares per chunk side (one mesh and draw each)
        static constexpr uint32_t MinResolution = 33, MaxResolution = 1025;

        explicit TerrainData(uint32_t resolution = 129, float height = 0.0f);
        TerrainData(const TerrainData& other);           // CPU data copied, chunk meshes shared until they change
        TerrainData& operator=(const TerrainData&) = delete;
        ~TerrainData();

        uint32_t Resolution() const { return m_Res; }
        uint32_t SplatResolution() const { return m_SplatRes; }
        static uint32_t SplatResolutionFor(uint32_t resolution);

        // ---- heights (metres, in the entity's space; row-major, z * Resolution + x)
        const std::vector<float>& Heights() const { return *m_Heights; }
        float At(int x, int z) const;                    // clamped to the grid
        void Set(int x, int z, float h);                 // marks it changed
        // The surface height at a point in the entity's X/Z (metres, centred on it). False outside the terrain.
        bool HeightAt(const glm::vec2& size, float x, float z, float& h) const;
        glm::vec3 NormalAt(const glm::vec2& size, float x, float z) const;   // the entity's space, unit length
        float MinHeight() const;
        float MaxHeight() const;

        // ---- paint (weights 0..255 per layer, summing to 255)
        const std::vector<uint8_t>& Splat() const { return *m_Splat; }
        // The weights at a point (u, v = 0..1 over the area), bilinear, summing to 1
        glm::vec4 WeightsAt(float u, float v) const;

        // ---- editing. Marks what changed (chunks to rebuild, paint to upload) and bumps Revision.
        void ApplyBrush(const TerrainBrush& brush, const glm::vec2& size, const glm::vec2& centre, float dt);
        void Generate(const TerrainNoise& noise, const glm::vec2& size);
        void Fill(float height);                         // flat at this height
        void FillPaint(int layer);                       // everything painted with one layer
        // Paint by the shape of the ground: steepLayer where it is steeper than steepDegrees, lowLayer below lowHeight,
        // highLayer above highHeight, baseLayer elsewhere (-1 = not used), with soft edges (about 6 degrees / 1 m)
        struct AutoPaintRules {
            int BaseLayer = 0;
            int SteepLayer = 2;  float SteepDegrees = 32.0f;
            int LowLayer = 3;    float LowHeight = 0.5f;
            int HighLayer = -1;  float HighHeight = 20.0f;
        };
        void AutoPaint(const glm::vec2& size, const AutoPaintRules& rules);
        // Heights from an image (8 or 16 bit grey; resampled to this resolution): black = low, white = low + range
        bool ImportHeightmap(const std::string& imagePath, float low, float range, std::string* error = nullptr);
        // A copy at another resolution (heights and paint resampled)
        std::shared_ptr<TerrainData> Resampled(uint32_t resolution) const;

        // Bumped by every change. Hash() is of the contents (cached per revision): scene JSON writes it, so the
        // editor's undo history sees terrain edits and the save game diff sees nothing when nothing changed.
        uint64_t Revision() const { return m_Revision; }
        uint64_t Hash() const;

        // ---- ray test in the entity's space (direction need not be unit length). t along the direction.
        bool Raycast(const glm::vec2& size, const glm::vec3& origin, const glm::vec3& direction, float maxT, float& t) const;

        // ---- rendering (GL thread): rebuild the chunks that changed and upload changed paint
        struct Chunk { std::shared_ptr<Mesh> Geometry; glm::vec3 Min{ 0.0f }, Max{ 0.0f }; };
        void UpdateGPU(const glm::vec2& size);
        const std::vector<Chunk>& Chunks() const { return m_Chunks; }
        uint32_t SplatTexture() const;                   // GL texture (RGBA8, linear), 0 before UpdateGPU
        TerrainMaterial Material;                        // filled each frame by Scene::BuildDrawList (DrawBatch::Terrain)
        uint64_t DrawnRevision = ~0ull;                  // Scene::BuildDrawList: the revision drawn last frame

        // ---- files
        bool Save(const std::string& path) const;        // written to a temporary file, then renamed
        static std::shared_ptr<TerrainData> Load(const std::string& path, std::string* error = nullptr);

    private:
        void MarkHeights(int x0, int z0, int x1, int z1);   // samples changed (inclusive)
        void MarkSplat(int x0, int z0, int x1, int z1);     // texels changed (inclusive)
        void MarkAll();
        float SampleHeight(float gx, float gz) const;       // grid coordinates, the triangles' surface

        uint32_t m_Res = 0, m_SplatRes = 0;
        // Shared with copies until one of them changes it (copy on write): a sculpt stroke copies only the heights,
        // a paint stroke only the weights, so undo steps stay small
        std::shared_ptr<std::vector<float>> m_Heights;
        std::shared_ptr<std::vector<uint8_t>> m_Splat;
        void OwnHeights() { if (m_Heights.use_count() > 1) m_Heights = std::make_shared<std::vector<float>>(*m_Heights); }
        void OwnSplat() { if (m_Splat.use_count() > 1) m_Splat = std::make_shared<std::vector<uint8_t>>(*m_Splat); }
        uint64_t m_Revision = 1;
        mutable uint64_t m_HashRevision = 0, m_Hash = 0;
        mutable uint64_t m_RangeRevision = 0;
        mutable float m_Min = 0.0f, m_Max = 0.0f;

        // GPU side
        std::vector<Chunk> m_Chunks;
        std::vector<uint8_t> m_ChunkDirty;                  // per chunk
        glm::vec2 m_BuiltSize{ 0.0f };
        struct GLTexture;
        std::shared_ptr<GLTexture> m_SplatTex;              // shared by copies until one of them paints
        bool m_SplatOwned = false;
        bool m_SplatDirty = true;
        glm::ivec4 m_SplatRect{ 0 };                        // dirty texels: x0, z0, x1, z1 (inclusive)
    };

    // The world-space surface under a point of the first terrain that covers it (any rotation and scale of the
    // terrain entity). False if no terrain is there. normal: optional, world space.
    bool TerrainSurfaceAt(Scene& scene, float x, float z, float& y, glm::vec3* normal = nullptr);
    // A hash of every terrain's placement, size and contents (the grass scatters again when it changes)
    uint64_t TerrainsHash(Scene& scene);

} // namespace Engine
