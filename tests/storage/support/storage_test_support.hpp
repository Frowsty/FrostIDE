// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the frost::storage tests: the build and stress switches, raw byte access to
// database files (only while no connection to the file is open in this process: POSIX advisory
// locks belong to the process, so a plain open/close of a database file would drop a live
// connection's locks), raw-API oracles, and option presets. Header-only; test code only.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/storage/database.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::storage::test {

// TODO(M1): use tests/support/test_env.hpp (kSanitized, stressFull) once m1/piece-tree has merged.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
inline constexpr bool kSanitized = true;
#else
inline constexpr bool kSanitized = false;
#endif
#else
inline constexpr bool kSanitized = false;
#endif

#ifdef NDEBUG
inline constexpr bool kOptimized = true;
#else
inline constexpr bool kOptimized = false;
#endif

// The value of environment variable `name` (ASCII), or "" if unset.
inline std::string environment(const char* name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    wchar_t buffer[256];
    const DWORD length = GetEnvironmentVariableW(wide.c_str(), buffer, 256);
    if (length == 0 || length >= 256) {
        return {};
    }
    std::string value;
    for (DWORD i = 0; i < length; ++i) {
        value.push_back(static_cast<char>(buffer[i]));
    }
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// FROST_STRESS_FULL=1: the milestone exit scale for the stress tests.
inline bool stressFull() { return environment("FROST_STRESS_FULL") == "1"; }

// Whole-file read; "" with a test failure on error. Only while no connection has the file open.
inline std::string readBytes(const std::string& path) {
    auto bytes = io::realFileSystem().readFile(path);
    if (!bytes) {
        ADD_FAILURE() << bytes.error().describe();
        return {};
    }
    return std::move(*bytes);
}

// Whole-file write (atomic replace). Only while no connection has the file open.
inline void writeBytes(const std::string& path, std::string_view bytes) {
    if (auto written = io::realFileSystem().writeFileAtomic(path, bytes); !written) {
        ADD_FAILURE() << written.error().describe();
    }
}

// Overwrites bytes at `offset` (the file must be long enough). Only while no connection has it open.
inline void patchBytes(const std::string& path, std::size_t offset, std::string_view bytes) {
    std::string content = readBytes(path);
    ASSERT_LE(offset + bytes.size(), content.size());
    content.replace(offset, bytes.size(), bytes);
    writeBytes(path, content);
}

inline bool fileExists(const std::string& path) { return io::realFileSystem().status(path).has_value(); }

inline std::uint64_t fileSize(const std::string& path) {
    auto status = io::realFileSystem().status(path);
    return status ? status->size : 0;
}

// Deterministic pseudo-random bytes.
inline std::string randomBytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

// Options for tests that are not about durability.
inline OpenOptions fastOptions() {
    OpenOptions options;
    options.synchronous = Synchronous::Normal;
    return options;
}

/*
 * A raw sqlite3 connection (the oracle: same engine, separate code paths from the wrapper).
 * Opens `path` read-write without creating it. The raw API takes the plain UTF-8 path; tests keep
 * their scratch paths short enough for the default VFS.
 */
class RawDb {
public:
    explicit RawDb(const std::string& path, int flags = SQLITE_OPEN_READWRITE) {
        const int rc = sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr);
        if (rc != SQLITE_OK) {
            ADD_FAILURE() << "raw open '" << path << "': " << sqlite3_errstr(rc);
        }
    }
    ~RawDb() { sqlite3_close_v2(m_db); }
    RawDb(const RawDb&) = delete ("owns the connection");
    RawDb& operator=(const RawDb&) = delete ("owns the connection");

    [[nodiscard]] sqlite3* get() const noexcept { return m_db; }

    // Runs `sql`; a test failure on error.
    void exec(std::string_view sql) {
        char* message = nullptr;
        const std::string text(sql);
        if (sqlite3_exec(m_db, text.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            ADD_FAILURE() << "raw exec: " << (message != nullptr ? message : "?") << " [" << text << "]";
        }
        sqlite3_free(message);
    }

    // The first column of the first row as an integer; -1 (and a test failure) on error.
    std::int64_t queryInt(std::string_view sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
            ADD_FAILURE() << "raw prepare: " << sqlite3_errmsg(m_db);
            return -1;
        }
        std::int64_t value = -1;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            value = sqlite3_column_int64(stmt, 0);
        } else if (rc != SQLITE_DONE) {
            ADD_FAILURE() << "raw step: " << sqlite3_errmsg(m_db);
        }
        sqlite3_finalize(stmt);
        return value;
    }

    // The first column of the first row as text ("" when no row).
    std::string queryText(std::string_view sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
            ADD_FAILURE() << "raw prepare: " << sqlite3_errmsg(m_db);
            return {};
        }
        std::string value;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text != nullptr) {
                value.assign(reinterpret_cast<const char*>(text),
                             static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
            }
        }
        sqlite3_finalize(stmt);
        return value;
    }

    // PRAGMA integrity_check returned exactly "ok".
    bool integrityOk() { return queryText("PRAGMA integrity_check") == "ok"; }

private:
    sqlite3* m_db = nullptr;
};

// gtest helper: ASSERT that a Result succeeded, printing the error.
#define FROST_STORAGE_ASSERT_OK(expr)                                                                                  \
    do {                                                                                                               \
        const auto& frostResult_ = (expr);                                                                             \
        ASSERT_TRUE(frostResult_.has_value()) << frostResult_.error().describe();                                      \
    } while (false)

} // namespace frost::storage::test
