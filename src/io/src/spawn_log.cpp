// SPDX-License-Identifier: Apache-2.0
//
// SpawnLogLauncher (frost/io/spawn_log.hpp): the session file, the SHA-256 of each executable
// (FIPS 180-4, implemented from the standard's text), and the IProcess and IAsyncProcess wrappers
// that add the exit records.

#include "frost/io/spawn_log.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/sched/task.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <mutex> // std::call_once
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::io {
namespace {

// --- SHA-256 (FIPS 180-4, sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2) -------------------------

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

// Processes one 64-byte block into the hash state `h`.
void compress(std::array<std::uint32_t, 8>& h, const unsigned char* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = (std::uint32_t{block[4 * t]} << 24) | (std::uint32_t{block[4 * t + 1]} << 16) |
               (std::uint32_t{block[4 * t + 2]} << 8) | std::uint32_t{block[4 * t + 3]};
    }
    for (std::size_t t = 16; t < 64; ++t) {
        const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (std::size_t t = 0; t < 64; ++t) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = k + s1 + choose + kRoundConstants[t] + w[t];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

// --- JSON output ----------------------------------------------------------------------------------

// Appends well-formed UTF-8 `text` to `out` with JSON's mandatory escapes.
void appendEscaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
}

// `text` as a JSON string literal. Control characters are escaped; each byte that is not part of
// well-formed UTF-8 (a POSIX path may hold one) becomes U+FFFD, so every record stays valid JSON.
std::string jsonString(std::string_view text) {
    std::string out = "\"";
    while (!text.empty()) {
        const std::size_t valid = validUtf8Prefix(text);
        appendEscaped(out, text.substr(0, valid));
        if (valid == text.size())
            break;
        out += "\xEF\xBF\xBD"; // U+FFFD for the ill-formed byte.
        text.remove_prefix(valid + 1);
    }
    out += '"';
    return out;
}

// An ISO-8601 UTC time with milliseconds: 2026-11-07T20:01:02.345Z.
std::string isoTime(IClock::SystemTime time) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(time));
}

std::uint32_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

} // namespace

std::string sha256Hex(std::string_view bytes) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t whole = bytes.size() / 64;
    for (std::size_t i = 0; i < whole; ++i) {
        compress(h, data + 64 * i);
    }
    // The padding (5.1.1): 0x80, zeros, and the message length in bits as a 64-bit big-endian number.
    std::array<unsigned char, 128> tail{};
    const std::size_t rest = bytes.size() - 64 * whole;
    for (std::size_t i = 0; i < rest; ++i) {
        tail[i] = data[64 * whole + i];
    }
    tail[rest] = 0x80;
    const std::size_t tailBlocks = rest + 1 + 8 <= 64 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[(64 * tailBlocks) - 1 - i] = static_cast<unsigned char>(bits >> (8U * i));
    }
    for (std::size_t i = 0; i < tailBlocks; ++i) {
        compress(h, tail.data() + 64 * i);
    }
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : h) {
        hex += std::format("{:08x}", word);
    }
    return hex;
}

struct SpawnLogLauncher::State {
    IFileSystem& fs;
    const IClock& clock;
    std::string path;

    mutable Mutex guard;
    std::string contents FROST_GUARDED_BY(guard); // Every record so far, one per line.
    std::uint64_t nextId FROST_GUARDED_BY(guard) = 1;
    std::optional<Error> writeError FROST_GUARDED_BY(guard){};
    // path -> (size, mtime, digest): each image is hashed once while it stays unchanged.
    std::map<std::string, std::tuple<std::uint64_t, IClock::SystemTime, std::string>, std::less<>>
        digests FROST_GUARDED_BY(guard);

    State(IFileSystem& fileSystem, const IClock& systemClock, std::string file)
        : fs(fileSystem), clock(systemClock), path(std::move(file)) {}

    // Appends `record` (one JSON object without the newline) and rewrites the file. Caller holds `guard`.
    void appendLocked(const std::string& record) FROST_REQUIRES(guard) {
        contents += record;
        contents += '\n';
        if (auto written = fs.writeFileAtomic(path, contents); !written) {
            writeError = std::move(written.error());
        }
    }

    void append(const std::string& record) {
        const LockGuard lock(guard);
        appendLocked(record);
    }

    std::uint64_t takeId() {
        const LockGuard lock(guard);
        return nextId++;
    }

    // `"sha256":"..."` or `"sha256":null,"sha256Error":"..."` for the executable at `executable`.
    std::string digestField(const std::string& executable) {
        auto status = fs.status(executable);
        if (!status) {
            return R"("sha256":null,"sha256Error":)" + jsonString(status.error().describe());
        }
        {
            const LockGuard lock(guard);
            if (auto it = digests.find(executable); it != digests.end() && std::get<0>(it->second) == status->size &&
                                                    std::get<1>(it->second) == status->mtime) {
                return R"("sha256":)" + jsonString(std::get<2>(it->second));
            }
        }
        auto bytes = fs.readFile(executable);
        if (!bytes) {
            return R"("sha256":null,"sha256Error":)" + jsonString(bytes.error().describe());
        }
        std::string digest = sha256Hex(*bytes);
        {
            const LockGuard lock(guard);
            digests.insert_or_assign(executable, std::tuple{status->size, status->mtime, digest});
        }
        return R"("sha256":)" + jsonString(digest);
    }

    // The fields every launch record shares: id, time, executable, argv (or rawArguments) and cwd.
    // `time` is when the launch began (taken before it, so a slow launch cannot make the start look late).
    std::string launchFields(std::string_view event, std::uint64_t id, const ProcessSpec& spec,
                             IClock::SystemTime time) const {
        std::string out =
            std::format(R"({{"event":{},"id":{},"time":{},"executable":{},"argv":[{})", jsonString(event), id,
                        jsonString(isoTime(time)), jsonString(spec.executable), jsonString(spec.executable));
        for (const std::string& argument : spec.arguments) {
            out += ',';
            out += jsonString(argument);
        }
        out += ']';
        if (spec.rawArguments) {
            out += R"(,"rawArguments":)" + jsonString(*spec.rawArguments);
        }
        out += R"(,"cwd":)" + jsonString(spec.workingDirectory);
        return out;
    }

    void recordExit(std::uint64_t id, int exitCode, int signal) {
        append(std::format(R"({{"event":"exit","id":{},"time":{},"exitCode":{},"signal":{}}})", id,
                           jsonString(isoTime(clock.systemNow())), exitCode, signal));
    }
};

namespace {

using State = SpawnLogLauncher::State;

// Records the exit of process `id` once, the first time a status is seen.
class ExitRecorder {
public:
    ExitRecorder(std::shared_ptr<State> state, std::uint64_t id) : m_state(std::move(state)), m_id(id) {}

    void seen(int exitCode, int signal) {
        std::call_once(m_once, [&] { m_state->recordExit(m_id, exitCode, signal); });
    }

private:
    std::shared_ptr<State> m_state;
    std::uint64_t m_id;
    std::once_flag m_once;
};

class LoggedProcess final : public IProcess {
public:
    LoggedProcess(std::unique_ptr<IProcess> inner, std::shared_ptr<ExitRecorder> exit)
        : m_inner(std::move(inner)), m_exit(std::move(exit)) {}

    [[nodiscard]] ProcessId pid() const noexcept override { return m_inner->pid(); }
    [[nodiscard]] Result<void> writeStdin(std::string_view bytes) override { return m_inner->writeStdin(bytes); }
    void closeStdin() noexcept override { m_inner->closeStdin(); }
    [[nodiscard]] Result<std::size_t> readStdout(std::span<char> buffer) override {
        return m_inner->readStdout(buffer);
    }
    [[nodiscard]] Result<std::size_t> readStderr(std::span<char> buffer) override {
        return m_inner->readStderr(buffer);
    }
    [[nodiscard]] Result<ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        auto status = m_inner->wait(timeout);
        if (status) {
            m_exit->seen(status->exitCode, status->signal);
        }
        return status;
    }
    [[nodiscard]] Result<void> terminate() override { return m_inner->terminate(); }
    [[nodiscard]] Result<ProcessResult> communicate(std::string_view stdinData, IClock::Duration timeout) override {
        auto result = m_inner->communicate(stdinData, timeout);
        if (result) {
            m_exit->seen(result->exitCode, result->signal);
        }
        return result;
    }
    [[nodiscard]] Result<std::vector<ContainedProcess>> containedProcesses() override {
        return m_inner->containedProcesses();
    }
    [[nodiscard]] Result<void> terminateContained(ProcessId pid) override { return m_inner->terminateContained(pid); }
    [[nodiscard]] Result<void> releaseContainer() override { return m_inner->releaseContainer(); }
    void abandonOutput() noexcept override { m_inner->abandonOutput(); }

private:
    std::unique_ptr<IProcess> m_inner;
    std::shared_ptr<ExitRecorder> m_exit;
};

sched::Task<Result<ExitStatus>> loggedExit(IAsyncProcess& inner, std::shared_ptr<ExitRecorder> exit,
                                           std::stop_token token) {
    auto status = co_await inner.exited(std::move(token));
    if (status) {
        exit->seen(status->exitCode, status->signal);
    }
    co_return status;
}

sched::Task<Result<ProcessResult>> loggedCommunicate(IAsyncProcess& inner, std::shared_ptr<ExitRecorder> exit,
                                                     std::string stdinData, IClock::Duration timeout,
                                                     std::stop_token token) {
    auto result = co_await inner.communicate(std::move(stdinData), timeout, std::move(token));
    if (result) {
        exit->seen(result->exitCode, result->signal);
    }
    co_return result;
}

// The tasks borrow the inner process: like every IAsyncProcess task, they must complete before the
// process is destroyed (async_process.hpp), and this wrapper owns the inner process.
class LoggedAsyncProcess final : public IAsyncProcess {
public:
    LoggedAsyncProcess(std::unique_ptr<IAsyncProcess> inner, std::shared_ptr<ExitRecorder> exit)
        : m_inner(std::move(inner)), m_exit(std::move(exit)) {}

    [[nodiscard]] ProcessId pid() const noexcept override { return m_inner->pid(); }
    [[nodiscard]] sched::Task<Result<std::size_t>> readStdout(std::span<char> buffer, std::stop_token token) override {
        return m_inner->readStdout(buffer, std::move(token));
    }
    [[nodiscard]] sched::Task<Result<std::size_t>> readStderr(std::span<char> buffer, std::stop_token token) override {
        return m_inner->readStderr(buffer, std::move(token));
    }
    [[nodiscard]] sched::Task<Result<void>> writeStdin(std::string bytes, std::stop_token token) override {
        return m_inner->writeStdin(std::move(bytes), std::move(token));
    }
    void closeStdin() noexcept override { m_inner->closeStdin(); }
    [[nodiscard]] sched::Task<Result<ExitStatus>> exited(std::stop_token token) override {
        return loggedExit(*m_inner, m_exit, std::move(token));
    }
    [[nodiscard]] Result<void> terminate() override { return m_inner->terminate(); }
    [[nodiscard]] sched::Task<Result<ProcessResult>> communicate(std::string stdinData, IClock::Duration timeout,
                                                                 std::stop_token token) override {
        return loggedCommunicate(*m_inner, m_exit, std::move(stdinData), timeout, std::move(token));
    }

private:
    std::unique_ptr<IAsyncProcess> m_inner;
    std::shared_ptr<ExitRecorder> m_exit;
};

} // namespace

Result<std::unique_ptr<SpawnLogLauncher>> SpawnLogLauncher::open(IProcessLauncher& inner, IFileSystem& fs,
                                                                 const IClock& clock, std::string_view directory) {
    if (!isAbsolutePath(directory)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the spawn log directory must be an absolute path: '{}'", directory));
    }
    auto normalized = normalizePath(directory);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (auto created = fs.createDirectories(*normalized); !created) {
        return std::unexpected(std::move(created.error()));
    }
    // The process-wide session number keeps two sessions opened in one millisecond apart.
    static constinit std::atomic<std::uint32_t> sessions{0};
    const std::uint32_t session = sessions.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now = std::chrono::floor<std::chrono::milliseconds>(clock.systemNow());
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const std::string name = std::format("spawn-{:%Y%m%dT%H%M%S}.{:03d}-{}-{}.jsonl", seconds,
                                         static_cast<int>((now - seconds).count()), currentProcessId(), session);
    auto file = joinPath(*normalized, name);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    if (auto written = fs.writeFileAtomic(*file, ""); !written) {
        return std::unexpected(std::move(written.error()));
    }
    auto state = std::make_shared<State>(fs, clock, std::move(*file));
    return std::unique_ptr<SpawnLogLauncher>(new SpawnLogLauncher(inner, std::move(state)));
}

SpawnLogLauncher::SpawnLogLauncher(IProcessLauncher& inner, std::shared_ptr<State> state)
    : m_inner(inner), m_state(std::move(state)) {}

SpawnLogLauncher::~SpawnLogLauncher() = default;

Result<std::unique_ptr<IProcess>> SpawnLogLauncher::launch(const ProcessSpec& spec) {
    const std::uint64_t id = m_state->takeId();
    const IClock::SystemTime started = m_state->clock.systemNow();
    auto process = m_inner.launch(spec);
    if (!process) {
        m_state->append(m_state->launchFields("launch-failed", id, spec, started) + R"(,"error":)" +
                        jsonString(process.error().describe()) + "}");
        return process;
    }
    // Hashed after the launch: the image the process runs (Windows keeps a running image locked).
    const std::string digest = m_state->digestField(spec.executable);
    m_state->append(m_state->launchFields("start", id, spec, started) + "," + digest +
                    std::format(R"(,"pid":{},"async":false}})", (*process)->pid()));
    return std::unique_ptr<IProcess>(
        std::make_unique<LoggedProcess>(std::move(*process), std::make_shared<ExitRecorder>(m_state, id)));
}

Result<std::unique_ptr<IAsyncProcess>> SpawnLogLauncher::launchAsync(const ProcessSpec& spec,
                                                                     const AsyncContext& context) {
    const std::uint64_t id = m_state->takeId();
    const IClock::SystemTime started = m_state->clock.systemNow();
    auto process = m_inner.launchAsync(spec, context);
    if (!process) {
        m_state->append(m_state->launchFields("launch-failed", id, spec, started) + R"(,"error":)" +
                        jsonString(process.error().describe()) + "}");
        return process;
    }
    const std::string digest = m_state->digestField(spec.executable);
    m_state->append(m_state->launchFields("start", id, spec, started) + "," + digest +
                    std::format(R"(,"pid":{},"async":true}})", (*process)->pid()));
    return std::unique_ptr<IAsyncProcess>(
        std::make_unique<LoggedAsyncProcess>(std::move(*process), std::make_shared<ExitRecorder>(m_state, id)));
}

const std::string& SpawnLogLauncher::path() const noexcept { return m_state->path; }

std::optional<Error> SpawnLogLauncher::lastWriteError() const {
    const LockGuard lock(m_state->guard);
    return m_state->writeError;
}

} // namespace frost::io
