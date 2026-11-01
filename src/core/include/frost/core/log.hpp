// SPDX-License-Identifier: Apache-2.0
//
// frost::log: the logging facade every module writes through (PLAN.md section 2, the
// Self-diagnostics row). It formats with std::format and hands one finished record to the
// installed ISink; the backend (frost::diag's asynchronous spdlog sink, ADR 0022) lives above
// core, so core and every module below diag can log without depending on it.
//
/*
 * Behaviour:
 *   - Records below level() are discarded before formatting; enabled() is one relaxed atomic
 *     load (Budget: B5), so a disabled call costs a load and a branch on hot paths.
 *   - With no sink installed, records are discarded too (tests, tools, and start-up before the
 *     backend exists).
 *   - Formatting failures (a formatter throwing, std::bad_alloc) are caught; the record becomes
 *     "<log record dropped: ...>" so a call never throws and never ends the process.
 *   - Categories are short ASCII names chosen by the caller ("app", "qt", "diag", ...).
 *
 * Thread safety: every function may be called from any thread at any time. A sink receives
 * concurrent write() calls and must serialise internally.
 *
 * Sink lifetime: setSink() publishes a pointer and returns the previous one, but a thread that
 * loaded the previous sink may still be inside its write() for a moment, so a sink must stay
 * valid until no thread can be writing to it. frost's program installs a process-lifetime sink
 * that is never destroyed; tests install sinks while only their own thread logs.
 */
#pragma once

#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace frost::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Critical, Off };

// "trace", "debug", "info", "warn", "error", "critical", "off"; "?" outside the enumeration.
[[nodiscard]] std::string_view toString(Level level) noexcept;

// Parses toString()'s names, case-insensitively, plus "warning"; returns false on anything
// else (then `out` is unchanged).
[[nodiscard]] bool parseLevel(std::string_view text, Level& out) noexcept;

/*
 * Receives finished records. write() and flush() must not throw and must be safe to call
 * concurrently from any thread. Sinks are never deleted through this interface (protected
 * non-virtual destructor), so implementations may be trivially destructible statics.
 */
class ISink {
public:
    // One record: `category` and `message` are valid only during the call.
    virtual void write(Level level, std::string_view category, std::string_view message) noexcept = 0;
    // Makes every record written before the call durable as far as the sink can (bounded time).
    virtual void flush() noexcept = 0;

protected:
    constexpr ISink() noexcept = default;
    constexpr ISink(const ISink&) noexcept = default;
    constexpr ISink& operator=(const ISink&) noexcept = default;
    ~ISink() = default;
};

// Installs `sink` (nullptr removes it) and returns the previous one; acquire-release, so a
// writer that sees the new sink also sees everything its installer set up. See "Sink lifetime".
ISink* setSink(ISink* sink) noexcept;

// The threshold: records at `level` or above are written. Default Level::Info. Relaxed.
void setLevel(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

// Whether a record at `level` would reach a sink (ignoring whether one is installed).
// Budget: B5 (one relaxed load).
[[nodiscard]] bool enabled(Level level) noexcept;

// Writes an already formatted record if `level` is enabled and a sink is installed.
void writeRecord(Level level, std::string_view category, std::string_view message) noexcept;

// Flushes the installed sink, if any.
void flush() noexcept;

namespace detail {
// Formats into a std::string and writes it; catches every exception (see the header comment).
template<class... A>
void formatAndWrite(Level lvl, std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    try {
        const std::string message = std::format(format, std::forward<A>(args)...);
        writeRecord(lvl, category, message);
    } catch (const std::bad_alloc&) {
        writeRecord(lvl, category, "<log record dropped: out of memory>");
    } catch (...) {
        writeRecord(lvl, category, "<log record dropped: formatting failed>");
    }
}
} // namespace detail

// Formats and writes one record when `lvl` is enabled; never throws.
template<class... A>
void write(Level lvl, std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    if (enabled(lvl)) {
        detail::formatAndWrite(lvl, category, format, std::forward<A>(args)...);
    }
}

template<class... A>
void trace(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Trace, category, format, std::forward<A>(args)...);
}
template<class... A>
void debug(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Debug, category, format, std::forward<A>(args)...);
}
template<class... A>
void info(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Info, category, format, std::forward<A>(args)...);
}
template<class... A>
void warn(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Warn, category, format, std::forward<A>(args)...);
}
template<class... A>
void error(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Error, category, format, std::forward<A>(args)...);
}
template<class... A>
void critical(std::string_view category, std::format_string<A...> format, A&&... args) noexcept {
    write(Level::Critical, category, format, std::forward<A>(args)...);
}

} // namespace frost::log
