#include "Renderer/Texture.h"
#include "Core/Log.h"

#include <glad/glad.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <algorithm>
#include <cmath>

namespace Engine {

    Texture2D::Texture2D(const std::string& path, bool srgb) {
        stbi_set_flip_vertically_on_load(true);   // OpenGL's origin is bottom-left
        int w, h, channels;
        uint8_t* data = stbi_load(path.c_str(), &w, &h, &channels, 4);  // force RGBA
        if (!data) {
            ENGINE_ERROR("Failed to load texture: {}", path);
            return;
        }
        m_Width = (uint32_t)w;
        m_Height = (uint32_t)h;
        Create(data, srgb);
        stbi_image_free(data);
    }

    Texture2D::Texture2D(uint32_t width, uint32_t height, const uint8_t* rgbaPixels, bool srgb)
        : m_Width(width), m_Height(height) {
        Create(rgbaPixels, srgb);
    }

    void Texture2D::Create(const uint8_t* pixels, bool srgb) {
        uint32_t levels = 1 + (uint32_t)std::floor(std::log2((double)std::max(m_Width, m_Height)));
        m_Levels = levels;
        m_SRGB = srgb;

        // Keep the pixels of an image with transparency: alpha-tested use rebuilds its mipmaps (PreserveAlphaCoverage)
        const size_t count = size_t(m_Width) * m_Height;
        for (size_t i = 0; i < count; i++)
            if (pixels[i * 4 + 3] < 255) { m_Pixels.assign(pixels, pixels + count * 4); break; }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_ID);
        glTextureStorage2D(m_ID, (GLsizei)levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
            (GLsizei)m_Width, (GLsizei)m_Height);
        glTextureSubImage2D(m_ID, 0, 0, 0, (GLsizei)m_Width, (GLsizei)m_Height,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glGenerateTextureMipmap(m_ID);

        glTextureParameteri(m_ID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_ID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Anisotropic filtering (GL 4.6 / EXT_texture_filter_anisotropic, on every desktop GPU; our glad is 4.5, hence
        // the raw enums). Surfaces seen at a slant (ground, roofs, hanging leaf cards) otherwise take the mip level of
        // their most squashed direction: blurred, and alpha-cut leaves thin out with distance.
        static const float maxAniso = [] {
            float m = 0.0f;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &m);
            while (glGetError() != GL_NO_ERROR) {}          // not supported: leave it off
            return m;
        }();
        if (maxAniso > 1.0f) glTextureParameterf(m_ID, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, std::min(maxAniso, 16.0f));
        glTextureParameteri(m_ID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_ID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    // Coverage-preserving mipmaps (I. Castaño, "Computing Alpha Mipmaps"). Each level is a 2x2 box filter of the
    // one above (colour averaged in linear light for sRGB textures, like glGenerateMipmap). Alpha is filtered
    // from the unscaled chain, then each level gets its own scale: the alpha t that exactly the full-size share
    // of pixels reach becomes the cutoff (scale = cutoff / t, never below 1). Measured on the willow leaves (cutoff 0.5):
    // plain mips kept 60% of the leaf pixels at mip 4, 27% at 5 and 8% at 6; on screen the distant willows went
    // from 1,560 leaf pixels to 7,920 in the same view (9,100 with anisotropic filtering too).
    void Texture2D::PreserveAlphaCoverage(float cutoff) const {
        if (m_CoverageDone || !m_ID) return;
        m_CoverageDone = true;
        if (m_Pixels.empty() || m_Levels < 2 || cutoff <= 0.0f || cutoff >= 1.0f) { m_Pixels.clear(); m_Pixels.shrink_to_fit(); return; }

        static float toLinear[256];
        static uint8_t toSRGB[4096];
        static bool tables = false;
        if (!tables) {
            for (int i = 0; i < 256; i++) {
                const float c = i / 255.0f;
                toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            for (int i = 0; i < 4096; i++) {
                const float l = i / 4095.0f;
                const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                toSRGB[i] = (uint8_t)std::clamp(std::lround(c * 255.0f), 0L, 255L);
            }
            tables = true;
        }

        uint32_t w = m_Width, h = m_Height;
        std::vector<float> rgb(size_t(w) * h * 3), alpha(size_t(w) * h);
        size_t covered = 0;
        for (size_t i = 0; i < alpha.size(); i++) {
            for (int c = 0; c < 3; c++) rgb[i * 3 + c] = m_SRGB ? toLinear[m_Pixels[i * 4 + c]] : m_Pixels[i * 4 + c] / 255.0f;
            alpha[i] = m_Pixels[i * 4 + 3] / 255.0f;
            if (alpha[i] >= cutoff) covered++;
        }
        m_Pixels.clear();
        m_Pixels.shrink_to_fit();
        const double target = double(covered) / double(alpha.size());
        if (covered == 0) return;                         // nothing passes the cutoff at full size: plain mips are fine

        std::vector<uint8_t> out;
        std::vector<uint32_t> histogram(1024);
        for (uint32_t level = 1; level < m_Levels; level++) {
            const uint32_t w2 = std::max(w / 2, 1u), h2 = std::max(h / 2, 1u);
            std::vector<float> rgb2(size_t(w2) * h2 * 3), alpha2(size_t(w2) * h2);
            for (uint32_t y = 0; y < h2; y++)
                for (uint32_t x = 0; x < w2; x++) {
                    const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                    const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                    const size_t s[4] = { size_t(y0) * w + x0, size_t(y0) * w + x1, size_t(y1) * w + x0, size_t(y1) * w + x1 };
                    const size_t d = size_t(y) * w2 + x;
                    for (int c = 0; c < 3; c++)
                        rgb2[d * 3 + c] = 0.25f * (rgb[s[0] * 3 + c] + rgb[s[1] * 3 + c] + rgb[s[2] * 3 + c] + rgb[s[3] * 3 + c]);
                    alpha2[d] = 0.25f * (alpha[s[0]] + alpha[s[1]] + alpha[s[2]] + alpha[s[3]]);
                }
            rgb.swap(rgb2);
            alpha.swap(alpha2);
            w = w2;
            h = h2;

            // The alpha that the full-size share of pixels reach (from the top of a histogram)
            std::fill(histogram.begin(), histogram.end(), 0u);
            for (float a : alpha) histogram[std::min((size_t)(a * 1023.0f + 0.5f), size_t(1023))]++;
            const double want = target * double(alpha.size());
            double sum = 0.0;
            int bin = 1023;
            for (; bin > 0; bin--) { sum += histogram[bin]; if (sum >= want) break; }
            const float threshold = std::max(bin, 1) / 1023.0f;
            // Only ever up: broad shapes gain coverage as they shrink (the village leaves do), and thinning those
            // would trade one look for another; the problem being fixed is leaves vanishing
            const float scale = std::clamp(cutoff / threshold, 1.0f, 16.0f);

            out.resize(size_t(w) * h * 4);
            for (size_t i = 0; i < alpha.size(); i++) {
                for (int c = 0; c < 3; c++) {
                    const float v = rgb[i * 3 + c];
                    out[i * 4 + c] = m_SRGB ? toSRGB[std::clamp((int)(v * 4095.0f + 0.5f), 0, 4095)]
                                            : (uint8_t)std::clamp(std::lround(v * 255.0f), 0L, 255L);
                }
                out[i * 4 + 3] = (uint8_t)std::clamp(std::lround(alpha[i] * scale * 255.0f), 0L, 255L);
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTextureSubImage2D(m_ID, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        }
    }

    Texture2D::~Texture2D() {
        if (m_ID) glDeleteTextures(1, &m_ID);
    }

    void Texture2D::Bind(uint32_t slot) const {
        glBindTextureUnit(slot, m_ID);
    }

    std::shared_ptr<Texture2D> Texture2D::FromMemory(const uint8_t* data, size_t size, bool srgb) {
        stbi_set_flip_vertically_on_load(true);
        int w, h, channels;
        uint8_t* px = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
        if (!px) {
            ENGINE_ERROR("Failed to decode embedded texture");
            return nullptr;
        }
        auto tex = std::shared_ptr<Texture2D>(new Texture2D());   // private ctor, so no make_shared
        tex->m_Width = (uint32_t)w;
        tex->m_Height = (uint32_t)h;
        tex->Create(px, srgb);
        stbi_image_free(px);
        return tex;
    }

} // namespace Engine