#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <cstddef>
#include <vector>

namespace Engine {

    class Texture2D {
    public:
        // Decode a PNG/JPG/etc. held in memory (used for textures embedded in model files)
        static std::shared_ptr<Texture2D> FromMemory(const uint8_t* data, size_t size, bool srgb = true);

        // Load from an image file (png, jpg, tga, bmp)
        explicit Texture2D(const std::string& path, bool srgb = true);
        Texture2D(uint32_t width, uint32_t height, const uint8_t* rgbaPixels, bool srgb = true);
        ~Texture2D();

        Texture2D(const Texture2D&) = delete;
        Texture2D& operator=(const Texture2D&) = delete;

        void Bind(uint32_t slot = 0) const;

        uint32_t GetWidth()  const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }
        uint32_t GetID() const { return m_ID; }        // the GL texture (0 if loading failed)

        // Alpha-tested (Cutout) use: rebuild the smaller mip levels so the share of pixels at or above `cutoff`
        // stays what it is at full size. Plain mipmaps average thin leaves into faint alpha that falls below the
        // cutoff, so alpha-cut foliage thins out and vanishes with distance. Done once, on first use (the
        // first cutoff wins: a texture is rarely cut at two different cutoffs); cheap to call every frame.
        // Only textures with transparency keep the full-size pixels this needs, until it has run.
        void PreserveAlphaCoverage(float cutoff) const;
        // Never used alpha-tested (alpha holds data, e.g. a height): free the CPU copy kept for the above
        void DropPixels() const { m_Pixels.clear(); m_Pixels.shrink_to_fit(); m_CoverageDone = true; }

    private:
        Texture2D() = default;
        void Create(const uint8_t* pixels, bool srgb);

        uint32_t m_ID = 0;
        uint32_t m_Width = 0, m_Height = 0;
        uint32_t m_Levels = 1;
        bool m_SRGB = true;
        mutable std::vector<uint8_t> m_Pixels;          // level 0 RGBA, kept while coverage may still be fixed
        mutable bool m_CoverageDone = false;
    };

} // namespace Engine