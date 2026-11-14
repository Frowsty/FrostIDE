// SPDX-License-Identifier: Apache-2.0
// FER-10: engine-owned scene test discovery, strict run-result parsing and cancellable CLI execution.
#pragma once

#include "frost/engine/engine_cli.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

struct EngineTestLocation {
    std::string path; // Exact engine-reported Assets/... path; resolved again before navigation.
    std::uint64_t line = 0;
    friend bool operator==(const EngineTestLocation&, const EngineTestLocation&) = default;
};
struct EngineTestFailure {
    std::string message, caseName, peer, rawLocation;
    std::optional<EngineTestLocation> location;
    friend bool operator==(const EngineTestFailure&, const EngineTestFailure&) = default;
};
enum class EngineTestOutcome : std::uint8_t { Passed, Failed, Skipped };
[[nodiscard]] std::string_view toString(EngineTestOutcome outcome) noexcept;

struct EngineTestCaseResult {
    std::string name, peer, skipReason;
    EngineTestOutcome outcome = EngineTestOutcome::Passed;
    std::optional<EngineTestLocation> definition;
    // Unknown unless a future engine explicitly reports a measured case duration.
    std::optional<std::chrono::microseconds> duration;
    friend bool operator==(const EngineTestCaseResult&, const EngineTestCaseResult&) = default;
};
struct EngineSceneTest {
    std::string id, scene; // Exact test.list scene identities; neither is inferred from Lua/source text.
    friend bool operator==(const EngineSceneTest&, const EngineSceneTest&) = default;
};
struct EngineTestListing {
    std::string projectPath, projectFile;
    std::vector<EngineSceneTest> tests; // test.list order, scene granularity only.
    friend bool operator==(const EngineTestListing&, const EngineTestListing&) = default;
};
struct EngineTestLogEntry {
    std::string level, logger, message;
    friend bool operator==(const EngineTestLogEntry&, const EngineTestLogEntry&) = default;
};
struct EngineSceneTestResult {
    std::string scene;
    EngineTestOutcome outcome = EngineTestOutcome::Passed;
    std::optional<std::chrono::microseconds> duration;
    std::vector<EngineTestCaseResult> cases; // Runtime-registered cases, only after an actual run.
    std::vector<EngineTestFailure> failures; // Actual assertion failures, script errors or engine run errors.
    std::string exitName, exitReason;        // Actual engine scene exit facts, not case attribution.
    std::optional<std::int64_t> exitCode;
    std::vector<EngineTestLogEntry> logTail; // Actual shared scene/peer log tail.
    std::vector<std::string> report; // Actual command report, retained once on the first scene for multi-scene output.
    friend bool operator==(const EngineSceneTestResult&, const EngineSceneTestResult&) = default;
};

// Pure, thread-safe parsers of command result objects, O(entries log entries), bounded to 100000
// discovered scenes/cases and 1 MiB per text value. Refuse malformed/unsupported listing versions,
// unsafe paths, inconsistent counts/statuses and duplicate scene IDs (Corrupt/Unsupported).
[[nodiscard]] Result<EngineTestListing> parseEngineTestListing(const rpc::Json& result);
// requestedScenes is the exact explicit selection sent to test.run. Refuses missing/extra/duplicate
// scene results; preserves engine case order, peers and unknown durations. Does not invent statuses.
[[nodiscard]] Result<std::vector<EngineSceneTestResult>>
parseEngineTestRun(const rpc::Json& result, std::span<const std::string> requestedScenes);

struct EngineTestProviderOptions {
    std::string program, project; // Absolute configured CLI and project directory or .frostproj.
    // Captured physical project directory. The optional selected file keeps its original spelling;
    // workers resolve/latch it at initial admission and compare exact target again before publication.
    std::string expectedProjectRoot;
    std::optional<std::string> expectedProjectFile;
    std::string folder = "Tests";
    std::uint64_t frames = 600;
    std::chrono::milliseconds discoveryTimeout{30000};
    std::optional<std::chrono::milliseconds> runTimeout{}; // No execution cap unless explicitly requested.
    std::string expectedProgram{}; // Discovery-worker physical CLI receipt; required before any child.
};
struct EngineTestExecution {
    // Only complete actual command reports. Cancelled/interrupted scenes get no invented result.
    std::vector<EngineSceneTestResult> results;
    std::optional<Error> error;
    bool cancelled = false;
};

// Worker-only synchronous operations through IProcessLauncher. The references/callback must outlive
// the call; admitted is thread-safe and checks the captured trust/source revision. Rechecks admission,
// cancellation and physical project identity before every child and before publication. Cancellation
// terminates/reaps the owned CLI tree. No source scan or fallback test data. Missing tools are errors.
[[nodiscard]] Result<EngineTestListing> discoverEngineSceneTests(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                                 const EngineTestProviderOptions& options,
                                                                 const std::function<bool()>& admitted,
                                                                 std::stop_token stop = {});
// Empty ids means every discovered scene. Rejects unlisted/duplicate ids before any child starts;
// sends one native test.run selection, preserving shared test-session state. The CLI only emits its
// final command report: cancellation/refusal/malformed output yields no invented partial results.
[[nodiscard]] EngineTestExecution runEngineSceneTests(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                      const EngineTestProviderOptions& options,
                                                      const EngineTestListing& listing,
                                                      std::span<const std::string> ids,
                                                      const std::function<bool()>& admitted, std::stop_token stop = {});
// Actual failed scene IDs from retained complete results, in listing order. Pure, O(listing*results).
[[nodiscard]] std::vector<std::string> failedEngineSceneTests(const EngineTestListing& listing,
                                                              std::span<const EngineSceneTestResult> results);
// Worker-only navigation check for a reported location. Validates current physical containment and
// a regular Assets/... file; no fallback line/path. Does not open the editor or mutate files.
[[nodiscard]] Result<std::string> resolveEngineTestLocation(io::IFileSystem& fs, std::string_view physicalProject,
                                                            const EngineTestLocation& location);

} // namespace frost::engine
