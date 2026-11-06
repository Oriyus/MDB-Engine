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

        glCreateTextures(GL_TEXTURE_2D, 1, &m_ID);
        glTextureStorage2D(m_ID, (GLsizei)levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
            (GLsizei)m_Width, (GLsizei)m_Height);
        glTextureSubImage2D(m_ID, 0, 0, 0, (GLsizei)m_Width, (GLsizei)m_Height,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glGenerateTextureMipmap(m_ID);

        glTextureParameteri(m_ID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_ID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_ID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_ID, GL_TEXTURE_WRAP_T, GL_REPEAT);
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