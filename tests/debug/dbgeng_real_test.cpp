// SPDX-License-Identifier: Apache-2.0
// FER-16 real acceptance: stdio DAP, actual engine Handle/Entity Natvis, real engine crash dumps.
// Values are checked against native getters executed in the same debuggee, never mirrored types.
#include "frost/debug/dbgeng_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include "support/real_engine_session.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef FROST_DBGENG_ADAPTER_PATH
#error "root CMake must provide the actual frost-dbgeng-dap executable"
#endif
namespace frost::debug {
namespace {
using Json = rpc::Json;
using namespace std::chrono_literals;

class DbgEngReal : public testing::Test {
protected:
    void SetUp() override {
        auto prepared = build.prepare({"FrostEditor", "FrostDbgEngFixture", "FrostTestCrashChild"});
        ASSERT_TRUE(prepared.has_value()) << prepared.error().describe();
        const std::string binaries(io::parentPath(build.program));
        fixtureProgram = io::joinPath(binaries, "FrostDbgEngFixture.exe").value_or("");
        fixtureSource = io::joinPath(build.corpus, "Frost-Tests/Helpers/DbgEngEngineMain.cpp").value_or("");
        natvisFile = io::joinPath(build.corpus, "Frost.natvis").value_or("");
        crashChild = io::joinPath(binaries, "FrostTestCrashChild.exe").value_or("");
        for (const auto& artifact : {fixtureProgram, fixtureSource, natvisFile, crashChild,
                                     binaries + "/FrostDbgEngFixture.pdb", binaries + "/FrostTestCrashChild.pdb"}) {
            ASSERT_TRUE(io::realFileSystem().exists(artifact).value_or(false))
                << "the locked Debug engine did not provide required artifact: " << artifact;
        }
    }
    frost::test::RealEngineBuild build;
    std::string fixtureProgram, fixtureSource, natvisFile, crashChild;
};
class Peer {
public:
    Result<void> start() {
        auto opened = dbgEngDapAdapterFactory(io::realProcessLauncher(), FROST_DBGENG_ADAPTER_PATH)(
            {.onEvent = [this](std::string_view event, const Json& body) {
                std::scoped_lock lock(m_mutex);
                if (event == "output") {
                    const auto text = body.stringAt("output");
                    if (m_output.size() + text.size() <= 1024 * 1024)
                        m_output += text;
                } else if (m_events.size() < 4096) {
                    m_events.emplace_back(std::string(event), body);
                }
            }});
        if (!opened)
            return std::unexpected(std::move(opened.error()));
        m_client = std::move(*opened);
        auto initialized = request("initialize", dap::initializeArguments(kDbgEngAdapterId));
        if (!initialized)
            return std::unexpected(std::move(initialized.error()));
        return {};
    }
    ~Peer() {
        if (m_client) {
            m_client->close();
            m_client.reset(); // Join callbacks before destroying their mutex/event storage.
        }
    }
    Result<Json> request(std::string command, Json arguments = Json::object()) {
        auto promise = std::make_shared<std::promise<Result<Json>>>();
        auto answer = promise->get_future();
        m_client->request(std::move(command), std::move(arguments),
                          [promise](Result<Json> result) { promise->set_value(std::move(result)); });
        if (answer.wait_for(60s) != std::future_status::ready)
            return fail(ErrorCode::Timeout, "DbgEng acceptance request timed out");
        return answer.get();
    }
    std::optional<Json> event(std::string_view name) {
        std::scoped_lock lock(m_mutex);
        for (auto it = m_events.rbegin(); it != m_events.rend(); ++it)
            if (it->first == name)
                return it->second;
        return std::nullopt;
    }
    std::string output() {
        std::scoped_lock lock(m_mutex);
        return m_output;
    }
    std::optional<Json> printedOracle() {
        const auto bytes = output();
        const auto begin = bytes.find("{\"handle\":");
        if (begin == std::string::npos)
            return std::nullopt;
        const auto end = bytes.find('\n', begin);
        auto parsed = rpc::parseJson(bytes.substr(begin, end - begin));
        return parsed ? std::optional(std::move(*parsed)) : std::nullopt;
    }

private:
    std::unique_ptr<IDebugAdapterClient> m_client;
    std::mutex m_mutex;
    std::vector<std::pair<std::string, Json>> m_events;
    std::string m_output;
};

std::optional<std::uint64_t> unsignedDisplay(std::string_view text) {
    // Test-only interpretation of a scalar debugger display, not the product's variable model.
    std::string clean;
    for (char c : text) {
        if (c != '`')
            clean.push_back(c);
    }
    std::string_view value(clean);
    int base = 10;
    if (value.starts_with("0x")) {
        value.remove_prefix(2);
        base = 16;
    } else if (value.starts_with("0n"))
        value.remove_prefix(2);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr == value.data())
        return std::nullopt;
    return result;
}

std::map<std::string, Json> namedValues(const Json& result) {
    std::map<std::string, Json> values;
    if (const auto* list = result.find("variables"); list != nullptr)
        for (const Json& value : list->items())
            values.emplace(std::string(value.stringAt("name")), value);
    return values;
}

std::int64_t markerLine(std::string_view source) {
    const auto marker = source.find("// FROST_DBGENG_BREAK_HERE");
    return marker == std::string_view::npos
               ? 0
               : 1 + std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(marker), '\n');
}

Result<Json> firstFrame(Peer& peer) {
    const auto stopped = peer.event("stopped");
    if (!stopped || !stopped->integerAt("threadId"))
        return fail(ErrorCode::Corrupt, "real stopped event did not identify an OS thread");
    auto stack = peer.request("stackTrace", Json::object().set("threadId", *stopped->integerAt("threadId")));
    if (!stack)
        return std::unexpected(std::move(stack.error()));
    const auto* frames = stack->find("stackFrames");
    if (frames == nullptr || frames->size() == 0)
        return fail(ErrorCode::NotFound, "real stack trace has no frames");
    return frames->items().front();
}

TEST_F(DbgEngReal, ActualEngineNatvisChildrenMatchNativeGettersIncludingInvalidHandles) {
    Peer peer;
    ASSERT_TRUE(peer.start().has_value());
    DbgEngTarget target;
    target.native.program = fixtureProgram;
    target.natvisFiles = {natvisFile};
    auto arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments.has_value()) << arguments.error().describe();
    auto launched = peer.request("launch", std::move(*arguments));
    ASSERT_TRUE(launched.has_value()) << launched.error().describe() << peer.output();
    auto source = io::realFileSystem().readFile(fixtureSource);
    ASSERT_TRUE(source.has_value());
    const auto line = markerLine(*source);
    ASSERT_GT(line, 0);
    auto breakpoints =
        peer.request("setBreakpoints", Json::object()
                                           .set("source", Json::object().set("path", fixtureSource))
                                           .set("breakpoints", Json::array().push(Json::object().set("line", line))));
    ASSERT_TRUE(breakpoints.has_value()) << breakpoints.error().describe();
    ASSERT_EQ(breakpoints->find("breakpoints")->size(), 1U);
    ASSERT_EQ(breakpoints->find("breakpoints")->items()[0].boolAt("verified"), true) << peer.output();
    ASSERT_TRUE(peer.request("configurationDone").has_value());
    ASSERT_TRUE(test::waitUntil([&] { return peer.event("stopped").has_value(); }, 60s)) << peer.output();
    auto frame = firstFrame(peer);
    ASSERT_TRUE(frame.has_value()) << frame.error().describe();
    EXPECT_NE(frame->stringAt("name").find("FrostDbgEngCheckpoint"), std::string_view::npos);
    const auto frameId = *frame->integerAt("id");
    auto scopes = peer.request("scopes", Json::object().set("frameId", frameId));
    ASSERT_TRUE(scopes.has_value()) << scopes.error().describe();
    const auto* actualScopes = scopes->find("scopes");
    ASSERT_NE(actualScopes, nullptr);
    const auto parameters = std::ranges::find_if(
        actualScopes->items(), [](const Json& scope) { return scope.stringAt("presentationHint") == "arguments"; });
    ASSERT_NE(parameters, actualScopes->items().end());
    const auto reference = *parameters->integerAt("variablesReference");
    auto locals = peer.request("variables", Json::object().set("variablesReference", reference));
    ASSERT_TRUE(locals.has_value()) << locals.error().describe();
    const auto variables = namedValues(*locals);
    std::map<std::string, std::map<std::string, Json>> visualized;
    for (const auto name : {"handle", "invalid", "reserved", "zeroGeneration", "entity"}) {
        const auto found = variables.find(name);
        ASSERT_NE(found, variables.end()) << "missing actual engine local " << name << peer.output();
        auto children = peer.request(
            "variables", Json::object().set("variablesReference", *found->second.integerAt("variablesReference")));
        ASSERT_TRUE(children.has_value()) << children.error().describe();
        visualized.emplace(name, namedValues(*children));
    }
    EXPECT_TRUE(variables.at("handle").stringAt("value").starts_with("Handle (index="));
    for (const auto name : {"invalid", "reserved", "zeroGeneration"})
        EXPECT_TRUE(variables.at(name).stringAt("value").starts_with("Invalid Handle (raw=")) << name;
    auto evaluatedRaw =
        peer.request("evaluate", Json::object().set("expression", "handle.m_Value").set("frameId", frameId));
    ASSERT_TRUE(evaluatedRaw.has_value()) << evaluatedRaw.error().describe();
    // Independent native storage/getter oracle: the real debuggee executes its getters after
    // continuing. It also reports this run's actual ASLR scene pointer (not another run's).
    ASSERT_TRUE(peer.request("continue", Json::object().set("threadId", *peer.event("stopped")->integerAt("threadId")))
                    .has_value());
    ASSERT_TRUE(test::waitUntil([&] { return peer.printedOracle().has_value(); }, 60s)) << peer.output();
    const auto oracle = peer.printedOracle();
    ASSERT_TRUE(oracle.has_value());
    ASSERT_TRUE(test::waitUntil([&] { return peer.event("exited").has_value(); }, 60s)) << peer.output();
    EXPECT_EQ(peer.event("exited")->integerAt("exitCode"), 0);
    EXPECT_EQ(unsignedDisplay(evaluatedRaw->stringAt("result")), oracle->find("handle")->unsignedAt("raw"));
    EXPECT_EQ(peer.event("stopped")->unsignedAt("threadId"), oracle->unsignedAt("threadId"));
    for (const auto name : {"handle", "invalid", "reserved", "zeroGeneration"}) {
        const auto& children = visualized.at(name);
        for (const auto& [child, key] : std::vector<std::pair<std::string, std::string>>{
                 {"Raw", "raw"}, {"Index", "index"}, {"Generation", "generation"}}) {
            const auto found = children.find(child);
            ASSERT_NE(found, children.end()) << name << '/' << child;
            EXPECT_EQ(unsignedDisplay(found->second.stringAt("value")), oracle->find(name)->unsignedAt(key))
                << name << '/' << child;
        }
        ASSERT_TRUE(children.contains("Structurally valid"));
        const auto actual = children.at("Structurally valid").stringAt("value");
        EXPECT_TRUE(actual == (oracle->find(name)->boolAt("valid").value() ? "true" : "false") ||
                    actual == (oracle->find(name)->boolAt("valid").value() ? "True" : "False"));
    }
    ASSERT_TRUE(visualized.at("entity").contains("Scene"));
    EXPECT_EQ(unsignedDisplay(visualized.at("entity").at("Scene").stringAt("value")), oracle->unsignedAt("scene"));
    ASSERT_TRUE(visualized.at("entity").contains("Handle"));
    EXPECT_EQ(unsignedDisplay(visualized.at("entity").at("Handle").stringAt("value")),
              oracle->unsignedAt("entityHandle"));
    // Old references cannot alias a new stop or inspect a running/ended target.
    EXPECT_FALSE(peer.request("variables", Json::object().set("variablesReference", reference)).has_value());
    ASSERT_TRUE(peer.request("disconnect", Json::object().set("terminateDebuggee", true)).has_value());
}

TEST_F(DbgEngReal, OpensAnActualEngineCrashDumpAndRefusesEveryExecutionOrMutationRequest) {
    frost::test::ScratchDir scratch("dbgeng-dump");
    io::ProcessSpec child;
    child.executable = crashChild;
    child.arguments = {"--directory", scratch.join("crashes"), "--mode", "segv", "--secrets", scratch.join("secrets")};
    auto process = io::realProcessLauncher().launch(child);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto crashed = (*process)->communicate({}, 120s);
    ASSERT_TRUE(crashed.has_value()) << crashed.error().describe();
    ASSERT_NE(crashed->exitCode, 0);
    auto reports = io::realFileSystem().listDirectory(scratch.join("crashes"));
    ASSERT_TRUE(reports.has_value());
    std::string dump;
    for (const auto& report : *reports) {
        if (report.type == io::FileType::Directory) {
            auto path = io::joinPath(scratch.join("crashes"), report.name);
            ASSERT_TRUE(path.has_value());
            auto file = io::joinPath(*path, "crash.dmp");
            ASSERT_TRUE(file.has_value());
            if (io::realFileSystem().exists(*file).value_or(false))
                dump = std::move(*file);
        }
    }
    ASSERT_FALSE(dump.empty()) << "the engine did not produce a real minidump";
    Peer peer;
    ASSERT_TRUE(peer.start().has_value());
    DbgEngTarget target;
    target.localSymbolPaths = {std::string(io::parentPath(crashChild))};
    target.dumpFile = dump;
    auto arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments.has_value());
    auto opened = peer.request("launch", std::move(*arguments));
    ASSERT_TRUE(opened.has_value()) << opened.error().describe() << peer.output();
    ASSERT_TRUE(peer.request("configurationDone").has_value());
    ASSERT_TRUE(test::waitUntil([&] { return peer.event("stopped").has_value(); })) << peer.output();
    auto frame = firstFrame(peer);
    ASSERT_TRUE(frame.has_value()) << frame.error().describe();
    EXPECT_NE(frame->stringAt("name").find("Crash"), std::string_view::npos) << frame->dump();
    EXPECT_FALSE(peer.event("process").has_value()) << "a dump must not invent a live process";
    for (const auto command :
         {"continue", "next", "stepIn", "stepOut", "pause", "restart", "attach", "setVariable", "terminate"}) {
        EXPECT_FALSE(
            peer.request(command, Json::object().set("threadId", *peer.event("stopped")->integerAt("threadId")))
                .has_value())
            << command;
    }
    EXPECT_FALSE(
        peer.request("evaluate", Json::object().set("expression", "x = 7").set("frameId", *frame->integerAt("id")))
            .has_value());
    EXPECT_FALSE(
        peer.request("evaluate", Json::object().set("expression", "function()").set("frameId", *frame->integerAt("id")))
            .has_value());
    EXPECT_TRUE(peer.request("disconnect", Json::object().set("terminateDebuggee", false)).has_value());
}

TEST_F(DbgEngReal, MissingDumpAndNatvisAreReportedAsErrorsWithoutInventedStoppedState) {
    frost::test::ScratchDir scratch("dbgeng-missing");
    Peer peer;
    ASSERT_TRUE(peer.start().has_value());
    DbgEngTarget target;
    target.native.program = fixtureProgram;
    target.dumpFile = scratch.join("missing.dmp");
    auto arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments.has_value());
    EXPECT_FALSE(peer.request("launch", std::move(*arguments)).has_value());
    EXPECT_FALSE(peer.event("stopped").has_value());
    Peer missingNatvis;
    ASSERT_TRUE(missingNatvis.start().has_value());
    target.dumpFile.clear();
    target.natvisFiles = {scratch.join("missing.natvis")};
    arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments.has_value());
    EXPECT_FALSE(missingNatvis.request("launch", std::move(*arguments)).has_value());
    EXPECT_FALSE(missingNatvis.event("stopped").has_value());
}

TEST(DbgEngTransportReal, ClosingInputCancelsBackpressuredOutputWithoutAnOwnerThreadDeadlock) {
    auto process = io::realProcessLauncher().launch(dbgEngDapProcessSpec(FROST_DBGENG_ADAPTER_PATH));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto initialize = Json::object()
                          .set("seq", 1)
                          .set("type", "request")
                          .set("command", "initialize")
                          .set("arguments", dap::initializeArguments(kDbgEngAdapterId));
    ASSERT_TRUE((*process)->writeStdin(rpc::encodeFrame(initialize.dump())).has_value());
    rpc::FrameDecoder initialResponse;
    std::array<char, 4096> buffer{};
    bool initialized = false;
    while (!initialized) {
        auto read = (*process)->readStdout(buffer);
        ASSERT_TRUE(read.has_value());
        ASSERT_NE(*read, 0U);
        initialResponse.feed(std::string_view(buffer.data(), *read));
        if (auto frame = initialResponse.next(); frame) {
            ASSERT_TRUE(frame->has_value());
            auto response = rpc::parseJson(frame->value());
            ASSERT_TRUE(response.has_value());
            ASSERT_EQ(response->integerAt("request_seq"), 1);
            ASSERT_EQ(response->boolAt("success"), true);
            initialized = true;
        }
    }
    // process_win.cpp creates 64 KiB pipes. A 512 KiB response cannot finish while
    // this parent consumes only a bounded prefix, establishing actual writer backpressure.
    auto request = Json::object().set("seq", 2).set("type", "request").set("command", std::string(512 * 1024, 'x'));
    ASSERT_TRUE((*process)->writeStdin(rpc::encodeFrame(request.dump())).has_value());
    std::string prefix;
    std::array<char, 256> fragment{};
    while (prefix.find("\"command\":\"xxxxxxxx") == std::string::npos && prefix.size() < 4096) {
        auto read = (*process)->readStdout(fragment);
        ASSERT_TRUE(read.has_value());
        ASSERT_NE(*read, 0U);
        prefix.append(fragment.data(), *read);
    }
    ASSERT_NE(prefix.find("\"command\":\"xxxxxxxx"), std::string::npos);
    ASSERT_LT(prefix.size(), 4096U);
    // Deliberately leave the remaining >500 KiB undrained while requesting EOF shutdown.
    (*process)->closeStdin();
    auto exited = (*process)->wait(5s);
    if (!exited) {
        (void)(*process)->terminate();
        (void)(*process)->wait(5s);
    }
    ASSERT_TRUE(exited.has_value()) << "output backpressure prevented adapter shutdown";
    EXPECT_EQ(exited->exitCode, 0);
}

} // namespace
} // namespace frost::debug
