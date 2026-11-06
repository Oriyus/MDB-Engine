#pragma once
#include <memory>
#include <spdlog/spdlog.h>

namespace Engine {

    class Log {
    public:
        static void Init();
        static std::shared_ptr<spdlog::logger>& Core() { return s_CoreLogger; }
        static std::shared_ptr<spdlog::logger>& Client() { return s_ClientLogger; }

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