#include "Core/Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <mutex>

namespace Engine {

    namespace {
        // Keeps the last lines in memory for the editor's Console
        class MemorySink : public spdlog::sinks::base_sink<std::mutex> {
        public:
            std::vector<LogEntry> Copy() {
                std::lock_guard<std::mutex> lock(mutex_);
                return { m_Lines.begin(), m_Lines.end() };
            }
            void Clear() {
                std::lock_guard<std::mutex> lock(mutex_);
                m_Lines.clear();
            }
            uint64_t Count() {
                std::lock_guard<std::mutex> lock(mutex_);
                return m_Count;
            }

        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override {
                LogEntry e;
                e.Level = (int)msg.level;
                e.Logger.assign(msg.logger_name.begin(), msg.logger_name.end());
                e.Text.assign(msg.payload.begin(), msg.payload.end());
                const std::time_t t = std::chrono::system_clock::to_time_t(msg.time);
                std::tm tm{};
                localtime_s(&tm, &t);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
                e.Time = buf;
                m_Lines.push_back(std::move(e));
                if (m_Lines.size() > 2000) m_Lines.pop_front();
                m_Count++;
            }
            void flush_() override {}

        private:
            std::deque<LogEntry> m_Lines;
            uint64_t m_Count = 0;
        };

        std::shared_ptr<MemorySink> s_Memory;
    }

    void Log::Init() {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern("%^[%T] %n: %v%$");
        s_Memory = std::make_shared<MemorySink>();
        const spdlog::sinks_init_list sinks = { console, s_Memory };

        s_CoreLogger = std::make_shared<spdlog::logger>("ENGINE", sinks);
        s_CoreLogger->set_level(spdlog::level::trace);
        spdlog::register_logger(s_CoreLogger);

        s_ClientLogger = std::make_shared<spdlog::logger>("GAME", sinks);
        s_ClientLogger->set_level(spdlog::level::trace);
        spdlog::register_logger(s_ClientLogger);
    }

    std::vector<LogEntry> Log::History() { return s_Memory ? s_Memory->Copy() : std::vector<LogEntry>{}; }
    void Log::ClearHistory() { if (s_Memory) s_Memory->Clear(); }
    uint64_t Log::HistoryCount() { return s_Memory ? s_Memory->Count() : 0; }

} // namespace Engine
