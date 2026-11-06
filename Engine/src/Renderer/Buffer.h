#pragma once
#include <cstdint>
#include <vector>

namespace Engine {

    enum class ShaderDataType { Float, Float2, Float3, Float4 };

    inline uint32_t ShaderDataTypeCount(ShaderDataType t) {
        switch (t) {
        case ShaderDataType::Float:  return 1;
        case ShaderDataType::Float2: return 2;
        case ShaderDataType::Float3: return 3;
        case ShaderDataType::Float4: return 4;
        }
        return 0;
    }

    struct BufferElement {
        ShaderDataType Type;
        uint32_t Offset = 0;   // filled in by BufferLayout
        BufferElement(ShaderDataType type) : Type(type) {}
        uint32_t Size() const { return ShaderDataTypeCount(Type) * sizeof(float); }
    };

    // Describes how the floats in a vertex buffer are laid out, e.g. {Float3 position, Float4 color}
    class BufferLayout {
    public:
        BufferLayout() = default;
        BufferLayout(std::initializer_list<BufferElement> elements) : m_Elements(elements) {
            uint32_t offset = 0;
            for (auto& e : m_Elements) {
                e.Offset = offset;
                offset += e.Size();
            }
            m_Stride = offset;
        }

        const std::vector<BufferElement>& GetElements() const { return m_Elements; }
        uint32_t GetStride() const { return m_Stride; }

    private:
        std::vector<BufferElement> m_Elements;
        uint32_t m_Stride = 0;
    };

    class VertexBuffer {
    public:
        VertexBuffer(const float* data, uint32_t sizeBytes);
        ~VertexBuffer();

        VertexBuffer(const VertexBuffer&) = delete;
        VertexBuffer& operator=(const VertexBuffer&) = delete;

        void SetLayout(const BufferLayout& layout) { m_Layout = layout; }
        const BufferLayout& GetLayout() const { return m_Layout; }
        uint32_t GetID() const { return m_ID; }

    private:
        uint32_t m_ID = 0;
        BufferLayout m_Layout;
    };

    class IndexBuffer {
    public:
        IndexBuffer(const uint32_t* indices, uint32_t count);
        ~IndexBuffer();

        IndexBuffer(const IndexBuffer&) = delete;
        IndexBuffer& operator=(const IndexBuffer&) = delete;

        uint32_t GetCount() const { return m_Count; }
        uint32_t GetID() const { return m_ID; }

    private:
        uint32_t m_ID = 0;
        uint32_t m_Count = 0;
    };

} // namespace Engine