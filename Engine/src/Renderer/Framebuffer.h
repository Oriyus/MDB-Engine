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
        // Motion vectors (RG16F): how far each pixel's surface moved on screen since last frame, in UV (current -
        // previous). kNoVelocity where nothing wrote one (sky, see-through parts): work it out from the depth.
        uint32_t GetVelocityAttachment() const { return m_VelocityTex; }
        static constexpr float kNoVelocity = 10000.0f;
        // Let the following draws write location 2 (lit.frag's Velocity). Off again after the passes that write it.
        void SetVelocityWrites(bool on) const;

    private:
        void Invalidate();   // (re)create all GL objects at the current size
        void Destroy();

        uint32_t m_FBO = 0, m_ColorTex = 0, m_IdTex = 0, m_Depth = 0, m_VelocityTex = 0;
        uint32_t m_Width = 1, m_Height = 1;
    };

} // namespace Engine