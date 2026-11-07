#include "Renderer/DrawList.h"
#include <glad/glad.h>
#include <algorithm>
#include <tuple>

namespace Engine {

    bool DrawBatch::operator<(const DrawBatch& o) const {
        return std::tie(Geometry, Part, Albedo, Normal, MetalRough, Emissive, Mode, Cutoff, Bones) <
            std::tie(o.Geometry, o.Part, o.Albedo, o.Normal, o.MetalRough, o.Emissive, o.Mode, o.Cutoff, o.Bones);
    }

    DrawList::DrawList() { glCreateBuffers(1, &m_Buffer); }
    DrawList::~DrawList() { glDeleteBuffers(1, &m_Buffer); }

    void DrawList::Clear() {
        Instances.clear();
        Opaque.clear();
        Transparent.clear();
        Shadow.clear();
        Culled = 0;
    }

    void DrawList::Finish(std::vector<Item>& opaque, std::vector<Item>& transparent, std::vector<Item>& shadow) {
        // Equal states next to each other; stable, so the order inside a batch doesn't flicker
        auto byState = [](const Item& a, const Item& b) { return a.State < b.State; };
        std::stable_sort(opaque.begin(), opaque.end(), byState);
        std::stable_sort(shadow.begin(), shadow.end(), byState);
        std::stable_sort(transparent.begin(), transparent.end(),
            [](const Item& a, const Item& b) { return a.Distance > b.Distance; });   // farthest first

        Instances.reserve(opaque.size() + transparent.size() + shadow.size());
        auto emit = [this](std::vector<Item>& items, std::vector<DrawBatch>& out) {
            for (const Item& it : items) {
                if (out.empty() || !out.back().SameState(it.State)) {
                    out.push_back(it.State);
                    out.back().First = (uint32_t)Instances.size();
                    out.back().Count = 0;
                }
                Instances.push_back(it.Instance);
                out.back().Count++;
            }
        };
        emit(opaque, Opaque);
        emit(transparent, Transparent);
        emit(shadow, Shadow);
    }

    void DrawList::Upload() {
        const size_t bytes = std::max<size_t>(Instances.size(), 1) * sizeof(InstanceData);
        if (bytes > m_Capacity) m_Capacity = std::max(bytes, m_Capacity * 2);
        // A fresh store every frame (orphaning): the GPU may still be reading last frame's
        glNamedBufferData(m_Buffer, (GLsizeiptr)m_Capacity, nullptr, GL_DYNAMIC_DRAW);
        if (!Instances.empty())
            glNamedBufferSubData(m_Buffer, 0, (GLsizeiptr)(Instances.size() * sizeof(InstanceData)), Instances.data());
    }

    void DrawList::Bind() const { glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_Buffer); }

} // namespace Engine
