#include "Core/Profiler.h"
#include "Core/Log.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace Engine {

    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr int kReportFrames = 300;

        struct Node {
            const char* Name;
            int Parent;                 // -1 = top level
            int Depth;
            double Total = 0.0;         // seconds over the report window
            long long Calls = 0;
        };
        struct State {
            std::vector<Node> Nodes;
            struct Open { int Node; Clock::time_point Start; };
            std::vector<Open> Stack;
            int Frames = 0;
            Clock::time_point WindowStart = Clock::now();
            struct Gpu { const char* Name; double Total = 0.0; long long Samples = 0; };
            std::vector<Gpu> Gpus;                  // in first-seen order
        };
        State& S() { static State s; return s; }

        int FindOrAdd(const char* name, int parent) {
            auto& nodes = S().Nodes;
            for (int i = 0; i < (int)nodes.size(); i++)
                if (nodes[i].Parent == parent && nodes[i].Name == name) return i;
            nodes.push_back({ name, parent, parent < 0 ? 0 : nodes[parent].Depth + 1 });
            return (int)nodes.size() - 1;
        }

        void Print(int parent, double frames) {
            const auto& nodes = S().Nodes;
            for (int i = 0; i < (int)nodes.size(); i++) {
                const Node& n = nodes[i];
                if (n.Parent != parent || n.Calls == 0) continue;
                ENGINE_INFO("profile: {}{:<{}} {:8.3f} ms {:7.1f} calls", std::string(n.Depth * 2, ' '), n.Name,
                    34 - n.Depth * 2, n.Total * 1000.0 / frames, n.Calls / frames);
                Print(i, frames);
            }
        }
    }

    bool Profiler::Enabled() {
        static const bool on = [] { const char* v = std::getenv("MDB_PROFILE"); return v && std::string(v) == "1"; }();
        return on;
    }

    void Profiler::Begin(const char* name) {
        auto& s = S();
        const int node = FindOrAdd(name, s.Stack.empty() ? -1 : s.Stack.back().Node);
        s.Stack.push_back({ node, Clock::now() });
    }

    void Profiler::End() {
        auto& s = S();
        if (s.Stack.empty()) return;
        const auto open = s.Stack.back();
        s.Stack.pop_back();
        Node& n = s.Nodes[open.Node];
        n.Total += std::chrono::duration<double>(Clock::now() - open.Start).count();
        n.Calls++;
    }

    void Profiler::AddGpu(const char* name, double seconds) {
        auto& gpus = S().Gpus;
        for (auto& g : gpus) if (g.Name == name) { g.Total += seconds; g.Samples++; return; }
        gpus.push_back({ name, seconds, 1 });
    }

    void Profiler::EndFrame() {
        if (!Enabled()) return;
        auto& s = S();
        if (++s.Frames < kReportFrames) return;
        const double seconds = std::chrono::duration<double>(Clock::now() - s.WindowStart).count();
        ENGINE_INFO("profile: ---- {} frames, {:.3f} ms a frame ({:.0f} FPS)", s.Frames, seconds * 1000.0 / s.Frames, s.Frames / seconds);
        Print(-1, s.Frames);
        for (auto& g : s.Gpus)
            if (g.Samples > 0) ENGINE_INFO("profile: gpu: {:<29} {:8.3f} ms", g.Name, g.Total * 1000.0 / (double)g.Samples);
        for (auto& g : s.Gpus) { g.Total = 0.0; g.Samples = 0; }
        for (auto& n : s.Nodes) { n.Total = 0.0; n.Calls = 0; }
        s.Frames = 0;
        s.WindowStart = Clock::now();
    }

} // namespace Engine
