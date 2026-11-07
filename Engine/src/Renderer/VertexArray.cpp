#include "Renderer/VertexArray.h"
#include "Core/Log.h"
#include <glad/glad.h>

namespace Engine {

    VertexArray::VertexArray() { glCreateVertexArrays(1, &m_ID); }
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
            glEnableVertexArrayAttrib(m_ID, m_AttribIndex);
            if (e.Type == ShaderDataType::Int4)   // integers reach the shader as ivec4, not converted
                glVertexArrayAttribIFormat(m_ID, m_AttribIndex, 4, GL_INT, e.Offset);
            else
                glVertexArrayAttribFormat(m_ID, m_AttribIndex,
                    (GLint)ShaderDataTypeCount(e.Type), GL_FLOAT, GL_FALSE, e.Offset);
            glVertexArrayAttribBinding(m_ID, m_AttribIndex, m_BindingIndex);
            m_AttribIndex++;
        }

        m_BindingIndex++;
        m_VertexBuffers.push_back(std::move(vb));
    }

    void VertexArray::SetIndexBuffer(std::shared_ptr<IndexBuffer> ib) {
        glVertexArrayElementBuffer(m_ID, ib->GetID());
        m_IndexBuffer = std::move(ib);
    }

} // namespace Engine