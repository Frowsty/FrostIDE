// SPDX-License-Identifier: Apache-2.0
//
// Tests for the logging facade (frost/core/log.hpp): level filtering before formatting, the
// sink seam, level names, and that formatting failures never escape a call.

#include "frost/core/log.hpp"

#include <gtest/gtest.h>

#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::log::Level;

struct Record {
    Level level;
    std::string category;
    std::string message;
};

// Collects records; thread-safe like a real sink must be.
class RecordingSink final : public frost::log::ISink {
public:
    void write(Level level, std::string_view category, std::string_view message) noexcept override {
        const std::scoped_lock lock(m_mutex);
        m_records.push_back({level, std::string(category), std::string(message)});
    }
    void flush() noexcept override {
        const std::scoped_lock lock(m_mutex);
        ++m_flushes;
    }
    std::vector<Record> records() {
        const std::scoped_lock lock(m_mutex);
        return m_records;
    }
    int flushes() {
        const std::scoped_lock lock(m_mutex);
        return m_flushes;
    }

private:
    std::mutex m_mutex;
    std::vector<Record> m_records;
    int m_flushes = 0;
};

// Installs a RecordingSink and a level for one test and restores the previous state.
class LogFacade : public ::testing::Test {
protected:
    void SetUp() override {
        m_previousSink = frost::log::setSink(&m_sink);
        m_previousLevel = frost::log::level();
        frost::log::setLevel(Level::Info);
    }
    void TearDown() override {
        frost::log::setSink(m_previousSink);
        frost::log::setLevel(m_previousLevel);
    }

    RecordingSink m_sink;

private:
    frost::log::ISink* m_previousSink = nullptr;
    Level m_previousLevel = Level::Info;
};

// Counts how often it is formatted, to show disabled records are never formatted.
struct CountedValue {
    int* formats;
};

// Throws while formatting, standing in for a formatter that fails.
struct ThrowingValue {};

} // namespace

template<>
struct std::formatter<CountedValue> : std::formatter<int> {
    auto format(const CountedValue& value, std::format_context& ctx) const {
        ++*value.formats;
        return std::formatter<int>::format(*value.formats, ctx);
    }
};

template<>
struct std::formatter<ThrowingValue> : std::formatter<int> {
    auto format(const ThrowingValue&, std::format_context& ctx) const -> std::format_context::iterator {
        throw std::runtime_error("formatter failed");
        (void)ctx;
    }
};

namespace {

TEST_F(LogFacade, WritesFormattedRecordsAtOrAboveTheLevel) {
    frost::log::info("app", "opened {} files in {} ms", 3, 12);
    frost::log::warn("io", "slow disk");
    frost::log::debug("app", "hidden {}", 1);
    const auto records = m_sink.records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].level, Level::Info);
    EXPECT_EQ(records[0].category, "app");
    EXPECT_EQ(records[0].message, "opened 3 files in 12 ms");
    EXPECT_EQ(records[1].level, Level::Warn);
    EXPECT_EQ(records[1].message, "slow disk");
}

TEST_F(LogFacade, DisabledRecordsAreNeverFormatted) {
    int formats = 0;
    frost::log::setLevel(Level::Error);
    frost::log::warn("app", "{}", CountedValue{&formats});
    EXPECT_EQ(formats, 0);
    frost::log::error("app", "{}", CountedValue{&formats});
    EXPECT_EQ(formats, 1);
    EXPECT_EQ(m_sink.records().size(), 1u);
}

TEST_F(LogFacade, EnabledFollowsTheThresholdAndOffIsNeverEnabled) {
    frost::log::setLevel(Level::Trace);
    EXPECT_TRUE(frost::log::enabled(Level::Trace));
    EXPECT_TRUE(frost::log::enabled(Level::Critical));
    EXPECT_FALSE(frost::log::enabled(Level::Off));
    frost::log::setLevel(Level::Off);
    EXPECT_FALSE(frost::log::enabled(Level::Critical));
    EXPECT_FALSE(frost::log::enabled(Level::Off));
    frost::log::critical("app", "dropped");
    EXPECT_TRUE(m_sink.records().empty());
}

TEST_F(LogFacade, AFailingFormatterBecomesADroppedRecord) {
    frost::log::error("app", "{}", ThrowingValue{});
    const auto records = m_sink.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "<log record dropped: formatting failed>");
}

TEST_F(LogFacade, NoSinkDiscardsRecordsAndFlushIsANoOp) {
    frost::log::ISink* mine = frost::log::setSink(nullptr);
    EXPECT_EQ(mine, &m_sink);
    frost::log::info("app", "nobody listens");
    frost::log::flush();
    EXPECT_EQ(frost::log::setSink(&m_sink), nullptr);
    EXPECT_TRUE(m_sink.records().empty());
    EXPECT_EQ(m_sink.flushes(), 0);
    frost::log::flush();
    EXPECT_EQ(m_sink.flushes(), 1);
}

TEST_F(LogFacade, WriteRecordPassesPreformattedTextThrough) {
    frost::log::writeRecord(Level::Info, "qt", "a {literal} brace");
    const auto records = m_sink.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "a {literal} brace");
    EXPECT_EQ(records[0].category, "qt");
}

TEST_F(LogFacade, ConcurrentWritersAllArrive) {
    constexpr int kThreads = 4;
    constexpr int kEach = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kEach; ++i) {
                frost::log::info("app", "t{} i{}", t, i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(m_sink.records().size(), static_cast<std::size_t>(kThreads * kEach));
}

TEST(LogLevels, NamesRoundTripAndParseIgnoresCase) {
    for (Level level :
         {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Critical, Level::Off}) {
        Level parsed = Level::Off;
        ASSERT_TRUE(frost::log::parseLevel(frost::log::toString(level), parsed)) << frost::log::toString(level);
        EXPECT_EQ(parsed, level);
    }
    Level parsed = Level::Info;
    EXPECT_TRUE(frost::log::parseLevel("WARNING", parsed));
    EXPECT_EQ(parsed, Level::Warn);
    EXPECT_TRUE(frost::log::parseLevel("Debug", parsed));
    EXPECT_EQ(parsed, Level::Debug);
    parsed = Level::Error;
    EXPECT_FALSE(frost::log::parseLevel("verbose", parsed));
    EXPECT_FALSE(frost::log::parseLevel("", parsed));
    EXPECT_EQ(parsed, Level::Error);
    EXPECT_EQ(frost::log::toString(static_cast<Level>(42)), "?");
}

} // namespace
