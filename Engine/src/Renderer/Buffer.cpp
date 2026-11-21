#include "Renderer/Buffer.h"
#include <glad/glad.h>

namespace Engine {

    VertexBuffer::VertexBuffer(const float* data, uint32_t sizeBytes) {
        glCreateBuffers(1, &m_ID);
#ifdef __EMSCRIPTEN__
        mdbgl_MarkBuffer(m_ID, GL_ARRAY_BUFFER);    // vertex data, not a storage buffer (Engine/web/glad/glad.h)
#endif
        glNamedBufferStorage(m_ID, sizeBytes, data, GL_DYNAMIC_STORAGE_BIT);
    }

    VertexBuffer::~VertexBuffer() {
        glDeleteBuffers(1, &m_ID);
    }

    IndexBuffer::IndexBuffer(const uint32_t* indices, uint32_t count) : m_Count(count) {
        glCreateBuffers(1, &m_ID);
#ifdef __EMSCRIPTEN__
        mdbgl_MarkBuffer(m_ID, GL_ELEMENT_ARRAY_BUFFER);
#endif
        glNamedBufferStorage(m_ID, count * sizeof(uint32_t), indices, GL_DYNAMIC_STORAGE_BIT);
    }

    IndexBuffer::~IndexBuffer() {
        glDeleteBuffers(1, &m_ID);
    }

} // namespace Engine