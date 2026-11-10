#include "Renderer/DrawList.h"
#include <glad/glad.h>
#include <algorithm>
#include <functional>

namespace Engine {

    // Field by field (the order std::tie gave), without building tuples: it runs thousands of times a frame
    bool DrawBatch::operator<(const DrawBatch& o) const {
        if (Geometry != o.Geometry) return std::less<>()(Geometry, o.Geometry);
        if (Part != o.Part) return Part < o.Part;
        if (Albedo != o.Albedo) return std::less<>()(Albedo, o.Albedo);
        if (Normal != o.Normal) return std::less<>()(Normal, o.Normal);
        if (MetalRough != o.MetalRough) return std::less<>()(MetalRough, o.MetalRough);
        if (Emissive != o.Emissive) return std::less<>()(Emissive, o.Emissive);
        if (Mode != o.Mode) return Mode < o.Mode;
        if (Cutoff != o.Cutoff) return Cutoff < o.Cutoff;
        if (DoubleSided != o.DoubleSided) return DoubleSided < o.DoubleSided;
        return std::less<>()(Bones, o.Bones);
    }

    DrawList::DrawList() { glCreateBuffers(2, m_Buffers); }
    DrawList::~DrawList() { glDeleteBuffers(2, m_Buffers); }

    void DrawList::Clear() {
        Instances.clear();
        Indices.clear();
        Opaque.clear();
        Transparent.clear();
        Selected.clear();
        for (auto& v : ShadowViews) v.clear();
        Views.clear();
        CascadeSplits = glm::vec4(0.0f);
        CascadeCount = 0;
        Culled = 0;
        ShadowSkipped = 0;
    }

    void DrawList::AddBatches(std::vector<Item>& items, std::vector<DrawBatch>& out, Order order) {
        // Stable, so the order inside a batch doesn't flicker from frame to frame. Indices are sorted, not the
        // items themselves (about 80 bytes each: moving them dominated the sort)
        std::vector<uint32_t>& order_ = m_SortOrder;
        order_.resize(items.size());
        for (uint32_t i = 0; i < (uint32_t)order_.size(); i++) order_[i] = i;
        if (order == Order::ByState)
            std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return items[a].State < items[b].State; });
        else
            std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return items[a].Distance > items[b].Distance; });

        for (uint32_t i : order_) {
            const Item& it = items[i];
            if (out.empty() || !out.back().SameState(it.State)) {
                out.push_back(it.State);
                out.back().First = (uint32_t)Indices.size();
                out.back().Count = 0;
            }
            Indices.push_back(it.Instance);
            out.back().Count++;
        }
    }

    void DrawList::Upload() {
        auto upload = [this](int i, const void* data, size_t bytes) {
            if (bytes > m_Capacity[i]) m_Capacity[i] = std::max(bytes, m_Capacity[i] * 2);
            if (m_Capacity[i] == 0) m_Capacity[i] = 256;
            // A fresh store every frame (orphaning): the GPU may still be reading last frame's
            glNamedBufferData(m_Buffers[i], (GLsizeiptr)m_Capacity[i], nullptr, GL_DYNAMIC_DRAW);
            if (bytes) glNamedBufferSubData(m_Buffers[i], 0, (GLsizeiptr)bytes, data);
        };
        upload(0, Instances.data(), Instances.size() * sizeof(InstanceData));
        upload(1, Indices.data(), Indices.size() * sizeof(uint32_t));
    }

    void DrawList::Bind() const {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_Buffers[0]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_Buffers[1]);
    }

} // namespace Engine
