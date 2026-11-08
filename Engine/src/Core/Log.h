#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

namespace Engine {

    // One logged line, kept in memory for an in-app console (the editor's Console panel)
    struct LogEntry {
        int Level = 0;              // spdlog::level (2 info, 3 warn, 4 error...)
        std::string Logger;         // "ENGINE" or "GAME"
        std::string Time;           // "HH:MM:SS"
        std::string Text;
    };

    class Log {
    public:
        static void Init();
        static std::shared_ptr<spdlog::logger>& Core() { return s_CoreLogger; }
        static std::shared_ptr<spdlog::logger>& Client() { return s_ClientLogger; }

        // The last lines logged (at most 2000), oldest first. Thread-safe copies.
        static std::vector<LogEntry> History();
        static void ClearHistory();
        static uint64_t HistoryCount();     // grows with every line logged (to notice new ones)

    private:
        static inline std::shared_ptr<spdlog::logger> s_CoreLogger;
        static inline std::shared_ptr<spdlog::logger> s_ClientLogger;
    };

} // namespace Engine

// Engine-side logging
#define ENGINE_TRACE(...) ::Engine::Log::Core()->trace(__VA_ARGS__)
#define ENGINE_INFO(...)  ::Engine::Log::Core()->info(__VA_ARGS__)
#define ENGINE_WARN(...)  ::Engine::Log::Core()->warn(__VA_ARGS__)
#define ENGINE_ERROR(...) ::Engine::Log::Core()->error(__VA_ARGS__)

// Game-side logging
#define LOG_TRACE(...) ::Engine::Log::Client()->trace(__VA_ARGS__)
#define LOG_INFO(...)  ::Engine::Log::Client()->info(__VA_ARGS__)
#define LOG_WARN(...)  ::Engine::Log::Client()->warn(__VA_ARGS__)
#define LOG_ERROR(...) ::Engine::Log::Client()->error(__VA_ARGS__)
