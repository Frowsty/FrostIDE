// SPDX-License-Identifier: Apache-2.0
//
// The logging facade's state: the installed sink and the level threshold (frost/core/log.hpp).

#include "frost/core/log.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace frost::log {
namespace {

// constinit: no dynamic initialisation (startup hygiene, PLAN.md 2.1).
constinit std::atomic<ISink*> g_sink{nullptr};
constinit std::atomic<Level> g_level{Level::Info};

constexpr std::array<std::string_view, 7> kNames{"trace", "debug", "info", "warn", "error", "critical", "off"};

[[nodiscard]] char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view toString(Level level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < kNames.size() ? kNames[index] : std::string_view("?");
}

bool parseLevel(std::string_view text, Level& out) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (equalsIgnoringCase(text, kNames[i])) {
            out = static_cast<Level>(i);
            return true;
        }
    }
    if (equalsIgnoringCase(text, "warning")) {
        out = Level::Warn;
        return true;
    }
    return false;
}

ISink* setSink(ISink* sink) noexcept { return g_sink.exchange(sink, std::memory_order_acq_rel); }

void setLevel(Level level) noexcept { g_level.store(level, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

bool enabled(Level level) noexcept {
    // Budget: B5. Level::Off is never enabled, even with the threshold at Off.
    return level != Level::Off && level >= g_level.load(std::memory_order_relaxed);
}

void writeRecord(Level level, std::string_view category, std::string_view message) noexcept {
    if (!enabled(level)) {
        return;
    }
    if (ISink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink->write(level, category, message);
    }
}

void flush() noexcept {
    if (ISink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink->flush();
    }
}

} // namespace frost::log
