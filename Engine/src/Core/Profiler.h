#pragma once

namespace Engine {

    // CPU section timer. With the environment variable MDB_PROFILE=1, every 300 frames the log gets the average
    // milliseconds per frame of each PROFILE_SCOPE, nested scopes indented under the one they ran inside, plus
    // the whole frame. Off (the default) a scope costs one branch. It times the CPU only: GL calls are counted
    // as long as they take to submit (a GPU-bound frame shows up as time in "Swap").
    class Profiler {
    public:
        static bool Enabled();                 // MDB_PROFILE=1, read once
        static void Begin(const char* name);   // name: a string literal (kept by pointer)
        static void End();
        static void EndFrame();                // the Application calls it once per frame
        // GPU time of a stage (GpuTimer), averaged per frame and printed as "gpu:" lines in the same report
        static void AddGpu(const char* name, double seconds);
    };

    struct ProfileScope {
        bool On;
        explicit ProfileScope(const char* name) : On(Profiler::Enabled()) { if (On) Profiler::Begin(name); }
        ~ProfileScope() { if (On) Profiler::End(); }
        ProfileScope(const ProfileScope&) = delete;
        ProfileScope& operator=(const ProfileScope&) = delete;
    };

} // namespace Engine

#define ENGINE_PROFILE_CAT2(a, b) a##b
#define ENGINE_PROFILE_CAT(a, b) ENGINE_PROFILE_CAT2(a, b)
#define PROFILE_SCOPE(name) ::Engine::ProfileScope ENGINE_PROFILE_CAT(profileScope_, __LINE__)(name)
