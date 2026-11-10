#pragma once
#include <cstdint>
#include <vector>

namespace Engine {

    class Framebuffer {
    public:
        Framebuffer(uint32_t width, uint32_t height);
        ~Framebuffer();

        Framebuffer(const Framebuffer&) = delete;
        Framebuffer& operator=(const Framebuffer&) = delete;

        void Bind() const;                 // also sets glViewport to our size
        static void Unbind();              // back to the window
        void Clear(float r, float g, float b, float a) const;
        void Resize(uint32_t width, uint32_t height);

        // Entity ID under a pixel (origin bottom-left), or -1 for background / out of bounds
        int ReadEntityID(int x, int y) const;
        // Every distinct entity ID (>= 0) in a rectangle (origin bottom-left, clipped to the target), sorted
        std::vector<int> ReadEntityIDs(int x, int y, int w, int h) const;
        float ReadDepth(int x, int y) const;    // 0..1 depth at a pixel (1 = nothing drawn: the sky)

        uint32_t GetColorAttachment() const { return m_ColorTex; }
        uint32_t GetWidth()  const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }
        uint32_t GetDepthAttachment() const { return m_Depth; }
        uint32_t GetEntityIDAttachment() const { return m_IdTex; }   // R32I

    private:
        void Invalidate();   // (re)create all GL objects at the current size
        void Destroy();

        uint32_t m_FBO = 0, m_ColorTex = 0, m_IdTex = 0, m_Depth = 0;
        uint32_t m_Width = 1, m_Height = 1;
    };

} // namespace Engine