#pragma once
#include <cstdint>

namespace Engine {

    // GPU time per stage of a frame (GL timestamp queries), for MDB_PROFILE=1: the CPU profiler only sees how long
    // GL calls take to submit, and a GPU-bound frame shows up there as time spent waiting. Call BeginFrame, then
    // Mark("stage") after each stage; a stage's time is from the previous mark to its own. Results are read three
    // frames later (no stall) and go to the profiler's report as "gpu:" lines. Does nothing when profiling is off.
    class GpuTimer {
    public:
        GpuTimer();
        ~GpuTimer();
        GpuTimer(const GpuTimer&) = delete;
        GpuTimer& operator=(const GpuTimer&) = delete;

        void BeginFrame();
        void Mark(const char* stage);      // a string literal (kept by pointer)

    private:
        static constexpr int kFrames = 4, kMarks = 32;
        struct Frame { uint32_t Queries[kMarks] = {}; const char* Names[kMarks] = {}; int Count = 0; bool Pending = false; };
        Frame m_Frames[kFrames];
        int m_Current = -1;
        bool m_Ready = false;
    };

} // namespace Engine
