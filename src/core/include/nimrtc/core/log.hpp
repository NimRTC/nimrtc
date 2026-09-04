#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace nimrtc::core::log {

// -----------------------------------------------------------------------------
// Severity levels
// -----------------------------------------------------------------------------
enum class Level : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
};

const char* to_string(Level level) noexcept;
Level parse_level(std::string_view s) noexcept;

// -----------------------------------------------------------------------------
// Sink — pluggable destination for log lines.
// -----------------------------------------------------------------------------
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(Level level, std::string_view msg) noexcept = 0;
};

using SinkPtr = std::shared_ptr<Sink>;

// Default sink writes to stderr in the form:
//   [Level] message
class StderrSink final : public Sink {
public:
    void write(Level level, std::string_view msg) noexcept override;
};

// -----------------------------------------------------------------------------
// Logger — singleton, thread-safe, lock-light (atomic level + shared sink).
// -----------------------------------------------------------------------------
class Logger {
public:
    static Logger& instance() noexcept;

    void set_level(Level min_level) noexcept;
    Level level() const noexcept;

    // Atomically replace the sink. Default is StderrSink.
    void set_sink(SinkPtr sink) noexcept;
    SinkPtr sink() const noexcept;

    // Bypass the level filter (used by Critical).
    void log(Level level, std::string_view msg) noexcept;

    // Convenience:
    void trace(std::string_view msg) noexcept { log(Level::Trace, msg); }
    void debug(std::string_view msg) noexcept { log(Level::Debug, msg); }
    void info (std::string_view msg) noexcept { log(Level::Info,  msg); }
    void warn (std::string_view msg) noexcept { log(Level::Warn,  msg); }
    void error(std::string_view msg) noexcept { log(Level::Error, msg); }
    void critical(std::string_view msg) noexcept { log(Level::Critical, msg); }
};

// -----------------------------------------------------------------------------
// Free-function convenience wrappers (inlined so they fold to member calls).
// -----------------------------------------------------------------------------
inline void set_level(Level l) noexcept          { Logger::instance().set_level(l); }
inline void trace(std::string_view m) noexcept   { Logger::instance().trace(m); }
inline void debug(std::string_view m) noexcept   { Logger::instance().debug(m); }
inline void info (std::string_view m) noexcept   { Logger::instance().info(m); }
inline void warn (std::string_view m) noexcept   { Logger::instance().warn(m); }
inline void error(std::string_view m) noexcept   { Logger::instance().error(m); }
inline void critical(std::string_view m) noexcept{ Logger::instance().critical(m); }

inline void set_sink(SinkPtr s) noexcept         { Logger::instance().set_sink(std::move(s)); }

} // namespace nimrtc::core::log