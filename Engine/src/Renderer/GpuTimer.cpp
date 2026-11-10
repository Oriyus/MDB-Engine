#include "Renderer/GpuTimer.h"
#include "Core/Profiler.h"

#include <glad/glad.h>

namespace Engine {

    GpuTimer::GpuTimer() {}

    GpuTimer::~GpuTimer() {
        if (!m_Ready) return;
        for (auto& f : m_Frames) glDeleteQueries(kMarks, f.Queries);
    }

    void GpuTimer::BeginFrame() {
        if (!Profiler::Enabled()) return;
        if (!m_Ready) {
            for (auto& f : m_Frames) glGenQueries(kMarks, f.Queries);
            m_Ready = true;
        }
        m_Current = (m_Current + 1) % kFrames;
        Frame& f = m_Frames[m_Current];
        // This slot was written kFrames ago: its results are long done, read them before reusing the queries
        if (f.Pending) {
            GLint available = 0;
            glGetQueryObjectiv(f.Queries[f.Count - 1], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint64 prev = 0;
                glGetQueryObjectui64v(f.Queries[0], GL_QUERY_RESULT, &prev);
                const GLuint64 first = prev;
                for (int i = 1; i < f.Count; i++) {
                    GLuint64 t = 0;
                    glGetQueryObjectui64v(f.Queries[i], GL_QUERY_RESULT, &t);
                    Profiler::AddGpu(f.Names[i], (double)(t - prev) * 1e-9);
                    prev = t;
                }
                Profiler::AddGpu("GPU frame (all stages)", (double)(prev - first) * 1e-9);
            }
            f.Pending = false;
        }
        f.Count = 0;
        Mark("start");
    }

    void GpuTimer::Mark(const char* stage) {
        if (!m_Ready || m_Current < 0) return;
        Frame& f = m_Frames[m_Current];
        if (f.Count >= kMarks) return;
        glQueryCounter(f.Queries[f.Count], GL_TIMESTAMP);
        f.Names[f.Count] = stage;
        f.Count++;
        f.Pending = f.Count > 1;
    }

} // namespace Engine
