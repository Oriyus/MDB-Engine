#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <cstddef>

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

    private:
        Texture2D() = default;
        void Create(const uint8_t* pixels, bool srgb);

        uint32_t m_ID = 0;
        uint32_t m_Width = 0, m_Height = 0;
    };

} // namespace Engine