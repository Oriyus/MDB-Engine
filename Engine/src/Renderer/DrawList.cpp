#include "Renderer/DrawList.h"
#include <glad/glad.h>
#include <algorithm>
#include <tuple>

namespace Engine {

    bool DrawBatch::operator<(const DrawBatch& o) const {
        return std::tie(Geometry, Part, Albedo, Normal, MetalRough, Emissive, Mode, Cutoff, Bones) <
            std::tie(o.Geometry, o.Part, o.Albedo, o.Normal, o.MetalRough, o.Emissive, o.Mode, o.Cutoff, o.Bones);
    }

    DrawList::DrawList() { glCreateBuffers(2, m_Buffers); }
    DrawList::~DrawList() { glDeleteBuffers(2, m_Buffers); }

    void DrawList::Clear() {
        Instances.clear();
        Indices.clear();
        Opaque.clear();
        Transparent.clear();
        for (auto& v : ShadowViews) v.clear();
        Views.clear();
        CascadeSplits = glm::vec4(0.0f);
        CascadeCount = 0;
        Culled = 0;
        ShadowSkipped = 0;
    }

    void DrawList::AddBatches(std::vector<Item>& items, std::vector<DrawBatch>& out, Order order) {
        // Stable, so the order inside a batch doesn't flicker from frame to frame
        if (order == Order::ByState)
            std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.State < b.State; });
        else
            std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.Distance > b.Distance; });

        for (const Item& it : items) {
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
