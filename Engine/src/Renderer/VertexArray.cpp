#include "Renderer/VertexArray.h"
#include "Core/Log.h"
#include <glad/glad.h>

namespace Engine {

    VertexArray::VertexArray() {
        glCreateVertexArrays(1, &m_ID);
        // A buffer without vertex colours (location 5) reads the generic value, which is context state and starts
        // at (0, 0, 0, 1): make it white once, so lit.vert's multiply changes nothing for those meshes
        static bool whiteDefault = false;
        if (!whiteDefault) { glVertexAttrib4f(5, 1.0f, 1.0f, 1.0f, 1.0f); whiteDefault = true; }
    }
    VertexArray::~VertexArray() { glDeleteVertexArrays(1, &m_ID); }

    void VertexArray::Bind() const { glBindVertexArray(m_ID); }
    void VertexArray::Unbind() { glBindVertexArray(0); }

    void VertexArray::AddVertexBuffer(std::shared_ptr<VertexBuffer> vb) {
        const auto& layout = vb->GetLayout();
        if (layout.GetElements().empty()) {
            ENGINE_ERROR("VertexBuffer has no layout! Call SetLayout() before AddVertexBuffer()");
            return;
        }

        glVertexArrayVertexBuffer(m_ID, m_BindingIndex, vb->GetID(), 0, layout.GetStride());

        for (const auto& e : layout.GetElements()) {
            const GLuint at = e.Location >= 0 ? (GLuint)e.Location : m_AttribIndex;
            glEnableVertexArrayAttrib(m_ID, at);
            if (e.Type == ShaderDataType::Int4)   // integers reach the shader as ivec4, not converted
                glVertexArrayAttribIFormat(m_ID, at, 4, GL_INT, e.Offset);
            else if (e.Type == ShaderDataType::UByte4Norm)
                glVertexArrayAttribFormat(m_ID, at, 4, GL_UNSIGNED_BYTE, GL_TRUE, e.Offset);
            else
                glVertexArrayAttribFormat(m_ID, at,
                    (GLint)ShaderDataTypeCount(e.Type), GL_FLOAT, GL_FALSE, e.Offset);
            glVertexArrayAttribBinding(m_ID, at, m_BindingIndex);
            if (e.Location < 0) m_AttribIndex++;
        }

        m_BindingIndex++;
        m_VertexBuffers.push_back(std::move(vb));
    }

    void VertexArray::SetIndexBuffer(std::shared_ptr<IndexBuffer> ib) {
        glVertexArrayElementBuffer(m_ID, ib->GetID());
        m_IndexBuffer = std::move(ib);
    }

} // namespace Engine