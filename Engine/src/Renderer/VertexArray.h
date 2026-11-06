#pragma once
#include "Renderer/Buffer.h"
#include <memory>
#include <vector>

namespace Engine {

    class VertexArray {
    public:
        VertexArray();
        ~VertexArray();

        VertexArray(const VertexArray&) = delete;
        VertexArray& operator=(const VertexArray&) = delete;

        void Bind() const;
        static void Unbind();

        void AddVertexBuffer(std::shared_ptr<VertexBuffer> vb);
        void SetIndexBuffer(std::shared_ptr<IndexBuffer> ib);

        const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const { return m_IndexBuffer; }

    private:
        uint32_t m_ID = 0;
        uint32_t m_AttribIndex = 0;     // next free attribute slot
        uint32_t m_BindingIndex = 0;    // next free buffer binding slot
        std::vector<std::shared_ptr<VertexBuffer>> m_VertexBuffers;  // keeps buffers alive
        std::shared_ptr<IndexBuffer> m_IndexBuffer;
    };

} // namespace Engine