// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost/io/spawn_log.hpp (FND-8): SHA-256 against the FIPS 180-2 example vectors, and the
// SpawnLogLauncher over a MemFs and the ScriptedProcessLauncher: one session file of JSON lines with
// the start (absolute executable, its digest, argv, cwd, pid), the launch failures and the exit
// records, the digest cache keyed by size and modification time, and blocking and async launches.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/sched/sim_executor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::ProcessSpec;
using frost::io::ScriptedProcess;
using frost::io::ScriptedProcessLauncher;
using frost::io::SpawnLogLauncher;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kRoot = kWindows ? "C:\\work" : "/work";

/*
 * A minimal JSON reader for the records (objects of strings, integers, booleans, null and arrays),
 * so the io tests need no other module (verify's TSan selection follows the CMake dependencies). It
 * accepts RFC 8259 for these kinds, with \uXXXX escapes limited to ASCII, and returns nullopt for
 * anything else.
 */
struct Json {
    enum class Kind { Null, Bool, Integer, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string string;
    std::vector<Json> elements;
    std::map<std::string, Json, std::less<>> members;

    [[nodiscard]] bool isNull() const { return kind == Kind::Null; }
    [[nodiscard]] std::optional<bool> asBool() const {
        return kind == Kind::Bool ? std::optional<bool>(boolean) : std::nullopt;
    }
    [[nodiscard]] std::optional<std::int64_t> asInteger() const {
        return kind == Kind::Integer ? std::optional<std::int64_t>(integer) : std::nullopt;
    }
    [[nodiscard]] const std::string* asString() const { return kind == Kind::String ? &string : nullptr; }
    [[nodiscard]] std::size_t size() const { return elements.size(); }
    [[nodiscard]] const std::vector<Json>& items() const { return elements; }
    [[nodiscard]] const Json* find(std::string_view key) const {
        const auto it = members.find(key);
        return it != members.end() ? &it->second : nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : m_text(text) {}

    std::optional<Json> document() {
        auto value = parse();
        skipSpace();
        return value && m_at == m_text.size() ? value : std::nullopt;
    }

private:
    void skipSpace() {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
            ++m_at;
    }
    bool take(std::string_view token) {
        if (m_text.substr(m_at, token.size()) != token)
            return false;
        m_at += token.size();
        return true;
    }
    static int hexDigit(char h) {
        if (h >= '0' && h <= '9')
            return h - '0';
        if (h >= 'a' && h <= 'f')
            return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
            return h - 'A' + 10;
        return -1;
    }
    std::optional<std::string> parseString() {
        if (!take("\""))
            return std::nullopt;
        std::string out;
        while (m_at < m_text.size()) {
            const char c = m_text[m_at++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                return std::nullopt;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_at >= m_text.size())
                return std::nullopt;
            switch (const char e = m_text[m_at++]; e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (m_at + 4 > m_text.size())
                    return std::nullopt;
                int code = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hexDigit(m_text[m_at++]);
                    if (digit < 0)
                        return std::nullopt;
                    code = code * 16 + digit;
                }
                if (code >= 0x80)
                    return std::nullopt;
                out += static_cast<char>(code);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> parse() {
        skipSpace();
        Json value;
        if (take("null"))
            return value;
        if (take("true") || take("false")) {
            value.kind = Json::Kind::Bool;
            value.boolean = m_text[m_at - 1] == 'e' && m_text[m_at - 2] == 'u';
            return value;
        }
        if (m_at < m_text.size() && m_text[m_at] == '"') {
            auto text = parseString();
            if (!text)
                return std::nullopt;
            value.kind = Json::Kind::String;
            value.string = std::move(*text);
            return value;
        }
        if (take("[")) {
            value.kind = Json::Kind::Array;
            skipSpace();
            if (take("]"))
                return value;
            for (;;) {
                auto element = parse();
                if (!element)
                    return std::nullopt;
                value.elements.push_back(std::move(*element));
                skipSpace();
                if (take("]"))
                    return value;
                if (!take(","))
                    return std::nullopt;
            }
        }
        if (take("{")) {
            value.kind = Json::Kind::Object;
            skipSpace();
            if (take("}"))
                return value;
            for (;;) {
                skipSpace();
                auto key = parseString();
                skipSpace();
                if (!key || !take(":"))
                    return std::nullopt;
                auto member = parse();
                if (!member || !value.members.emplace(std::move(*key), std::move(*member)).second)
                    return std::nullopt;
                skipSpace();
                if (take("}"))
                    return value;
                if (!take(","))
                    return std::nullopt;
            }
        }
        // An integer, -?(0|[1-9][0-9]*): the only numbers the records hold.
        const bool negative = take("-");
        const std::size_t digits = m_at;
        while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
            ++m_at;
        if (m_at == digits || (m_text[digits] == '0' && m_at - digits > 1) || m_at - digits > 18)
            return std::nullopt;
        value.kind = Json::Kind::Integer;
        for (std::size_t i = digits; i < m_at; ++i)
            value.integer = value.integer * 10 + (m_text[i] - '0');
        if (negative)
            value.integer = -value.integer;
        return value;
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

std::string at(std::string_view relative) { return frost::io::joinPath(kRoot, relative).value(); }

// The session file's records, parsed.
std::vector<Json> records(frost::io::MemFs& fs, const SpawnLogLauncher& log) {
    auto text = fs.readFile(log.path());
    EXPECT_TRUE(text.has_value());
    std::vector<Json> out;
    std::string_view rest = text ? std::string_view(*text) : std::string_view();
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        EXPECT_NE(end, std::string_view::npos) << "every record ends with a newline";
        auto parsed = JsonReader(rest.substr(0, end)).document();
        EXPECT_TRUE(parsed.has_value()) << rest.substr(0, end);
        if (parsed)
            out.push_back(std::move(*parsed));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
    }
    return out;
}

std::string text(const Json& record, std::string_view key) {
    const Json* value = record.find(key);
    const std::string* s = value != nullptr ? value->asString() : nullptr;
    return s != nullptr ? *s : std::string("<missing ") + std::string(key) + ">";
}

std::int64_t number(const Json& record, std::string_view key) {
    const Json* value = record.find(key);
    return value != nullptr ? value->asInteger().value_or(-1) : -1;
}

// FIPS 180-2 appendix B (one block, two blocks, a million 'a') and the empty message.
TEST(Sha256, MatchesTheFipsExamples) {
    EXPECT_EQ(frost::io::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(frost::io::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(frost::io::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(frost::io::sha256Hex(std::string(1'000'000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

class SpawnLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(at("bin")).has_value());
        ASSERT_TRUE(fs.writeFileAtomic(tool, "the tool's bytes").has_value());
        auto opened = SpawnLogLauncher::open(scripted, fs, clock, at("logs/spawn"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        log = std::move(*opened);
    }

    ProcessSpec spec() const {
        ProcessSpec s;
        s.executable = tool;
        s.arguments = {"--log=error", "a \"quoted\" arg"};
        s.workingDirectory = at("project");
        return s;
    }

    frost::ManualClock clock{
        frost::IClock::SteadyTime{},
        frost::IClock::SystemTime{std::chrono::sys_days{std::chrono::year{2026} / 11 / 7}.time_since_epoch()}};
    frost::io::MemFs fs{clock};
    ScriptedProcessLauncher scripted;
    std::string tool = at(kWindows ? "bin\\tool.exe" : "bin/tool");
    std::unique_ptr<SpawnLogLauncher> log;
};

TEST_F(SpawnLogTest, RecordsTheStartAndTheExitOfABlockingLaunch) {
    EXPECT_TRUE(log->path().starts_with(at("logs/spawn"))) << log->path();
    EXPECT_TRUE(log->path().ends_with(".jsonl")) << log->path();
    EXPECT_EQ(fs.readFile(log->path()).value(), "") << "open() creates the session file";
    scripted.expect(ScriptedProcess{.expectExecutable = tool, .exitCode = 3, .stdoutBytes = "out"});
    auto process = log->launch(spec());
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    clock.advance(1500ms);
    auto result = (*process)->communicate("", 1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stdoutBytes, "out") << "the decorator passes the streams through";
    ASSERT_TRUE((*process)->wait().has_value());
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 2U) << "one exit record however often the status is read";
    const Json& start = all[0];
    EXPECT_EQ(text(start, "event"), "start");
    EXPECT_EQ(number(start, "id"), 1);
    EXPECT_EQ(text(start, "time"), "2026-11-07T00:00:00.000Z");
    EXPECT_EQ(text(start, "executable"), tool);
    EXPECT_EQ(text(start, "sha256"), frost::io::sha256Hex("the tool's bytes"));
    const Json* argv = start.find("argv");
    ASSERT_NE(argv, nullptr);
    ASSERT_EQ(argv->size(), 3U);
    EXPECT_EQ(*argv->items()[0].asString(), tool);
    EXPECT_EQ(*argv->items()[2].asString(), "a \"quoted\" arg");
    EXPECT_EQ(text(start, "cwd"), at("project"));
    EXPECT_EQ(number(start, "pid"), (*process)->pid());
    const Json& exit = all[1];
    EXPECT_EQ(text(exit, "event"), "exit");
    EXPECT_EQ(number(exit, "id"), 1);
    EXPECT_EQ(text(exit, "time"), "2026-11-07T00:00:01.500Z");
    EXPECT_EQ(number(exit, "exitCode"), 3);
    EXPECT_EQ(number(exit, "signal"), 0);
    EXPECT_FALSE(log->lastWriteError().has_value());
}

TEST_F(SpawnLogTest, RecordsALaunchFailureAndReturnsItsError) {
    scripted.expect(ScriptedProcess{.launchError = frost::Error(ErrorCode::NotFound, "no such tool")});
    auto failed = log->launch(spec());
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::NotFound);
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(text(all[0], "event"), "launch-failed");
    EXPECT_NE(text(all[0], "error").find("no such tool"), std::string::npos) << text(all[0], "error");
    EXPECT_EQ(all[0].find("pid"), nullptr);
}

TEST_F(SpawnLogTest, AMissingExecutableHasNoDigestButTheLaunchStillRuns) {
    ProcessSpec missing = spec();
    missing.executable = at(kWindows ? "bin\\gone.exe" : "bin/gone");
    scripted.expect(ScriptedProcess{});
    ASSERT_TRUE(log->launch(missing).has_value());
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 1U);
    ASSERT_NE(all[0].find("sha256"), nullptr);
    EXPECT_TRUE(all[0].find("sha256")->isNull());
    EXPECT_FALSE(text(all[0], "sha256Error").starts_with("<missing")) << "the reason is recorded";
    EXPECT_EQ(text(all[0], "event"), "start");
}

TEST_F(SpawnLogTest, TheDigestFollowsAChangedImage) {
    scripted.expect(ScriptedProcess{});
    scripted.expect(ScriptedProcess{});
    scripted.expect(ScriptedProcess{});
    ASSERT_TRUE(log->launch(spec()).has_value());
    ASSERT_TRUE(log->launch(spec()).has_value());
    clock.advance(2s); // A new modification time, and a new size.
    ASSERT_TRUE(fs.writeFileAtomic(tool, "a rebuilt tool").has_value());
    ASSERT_TRUE(log->launch(spec()).has_value());
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(text(all[0], "sha256"), frost::io::sha256Hex("the tool's bytes"));
    EXPECT_EQ(text(all[1], "sha256"), text(all[0], "sha256"));
    EXPECT_EQ(text(all[2], "sha256"), frost::io::sha256Hex("a rebuilt tool"));
    EXPECT_EQ(number(all[2], "id"), 3);
}

TEST_F(SpawnLogTest, RecordsAsyncLaunchesAndTheirExit) {
    frost::sched::SimExecutor sim;
    scripted.expect(ScriptedProcess{.exitCode = 5, .stdoutBytes = "x"});
    // The scripted launcher ignores the reactor (scripted_process_launcher.hpp); the SimExecutor drives it.
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value()) << reactor.error().describe();
    auto process = log->launchAsync(spec(), frost::io::AsyncContext{**reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::array<char, 4> buffer{};
    EXPECT_EQ(sim.spawn(frost::sched::Lane::UserVisible, (*process)->readStdout(buffer)).join().value(), 1U);
    const auto status = sim.spawn(frost::sched::Lane::UserVisible, (*process)->exited()).join();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, 5);
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0].find("async")->asBool(), true);
    EXPECT_EQ(text(all[1], "event"), "exit");
    EXPECT_EQ(number(all[1], "exitCode"), 5);
}

TEST_F(SpawnLogTest, EscapesControlCharactersInArguments) {
    ProcessSpec special = spec();
    special.arguments = {std::string("tab\there\x01line\nend")};
    scripted.expect(ScriptedProcess{});
    ASSERT_TRUE(log->launch(special).has_value());
    const std::string raw = fs.readFile(log->path()).value();
    EXPECT_NE(raw.find(R"(tab\there\u0001line\nend)"), std::string::npos) << raw;
    const auto all = records(fs, *log);
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(*all[0].find("argv")->items()[1].asString(), special.arguments[0]);
    EXPECT_EQ(all[0].find("rawArguments"), nullptr);
    EXPECT_EQ(all[0].find("async")->asBool(), false);
}

TEST(SpawnLog, NeedsAnAbsoluteDirectory) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    ScriptedProcessLauncher scripted;
    auto opened = SpawnLogLauncher::open(scripted, fs, clock, "relative/logs");
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
