/**
 * @file src/log/src/log.cpp
 * @brief Logger singleton implementation.
 *
 * Moved from src/core/src/log.cpp per ARCHITECTURE.md Layout Invariant 1:
 * src/core/ is header-only INTERFACE; log implementation lives here.
 */

#include <nimrtc/core/log.hpp>

#include <atomic>
#include <cstdio>
#include <memory>

namespace nimrtc::core::log {

// ---------------------------------------------------------------------------
// Level <-> string helpers
// ---------------------------------------------------------------------------

const char* to_string(Level level) noexcept {
    switch (level) {
        case Level::Trace:    return "trace";
        case Level::Debug:    return "debug";
        case Level::Info:     return "info";
        case Level::Warn:     return "warn";
        case Level::Error:    return "error";
        case Level::Critical: return "critical";
    }
    return "?";
}

Level parse_level(std::string_view s) noexcept {
    if (s == "trace")    return Level::Trace;
    if (s == "debug")    return Level::Debug;
    if (s == "info")     return Level::Info;
    if (s == "warn")     return Level::Warn;
    if (s == "error")    return Level::Error;
    if (s == "critical") return Level::Critical;
    return Level::Info;
}

// ---------------------------------------------------------------------------
// StderrSink
// ---------------------------------------------------------------------------

void StderrSink::write(Level level, std::string_view msg) noexcept {
    std::fprintf(stderr, "[%s] %.*s\n",
                 to_string(level),
                 static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Logger — singleton, atomic level, shared sink
// ---------------------------------------------------------------------------

namespace {
struct State {
    std::atomic<Level>   level{Level::Info};
    std::shared_ptr<Sink> sink = std::make_shared<StderrSink>();
};
State& state() {
    static State s;
    return s;
}
} // anonymous namespace

Logger& Logger::instance() noexcept {
    static Logger inst;
    return inst;
}

void Logger::set_level(Level min_level) noexcept {
    state().level.store(min_level, std::memory_order_relaxed);
}

Level Logger::level() const noexcept {
    return state().level.load(std::memory_order_relaxed);
}

void Logger::set_sink(SinkPtr sink) noexcept {
    if (sink) state().sink = std::move(sink);
}

SinkPtr Logger::sink() const noexcept {
    return state().sink;
}

void Logger::log(Level level, std::string_view msg) noexcept {
    if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(this->level())) {
        return;
    }
    if (auto s = state().sink) {
        s->write(level, msg);
    }
}

} // namespace nimrtc::core::log
