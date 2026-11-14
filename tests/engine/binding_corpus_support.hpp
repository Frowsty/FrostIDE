// SPDX-License-Identifier: Apache-2.0
// FEC-13 real-tool corpus support: retained evidence, existing pinned-clone provisioning and owned LSP lifetime.
#pragma once

#include "frost/engine/binding_queries.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/resource_governor.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/real_engine_session.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::test::binding {
using rpc::Json;
using namespace std::chrono_literals;

inline bool wait(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 90s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class Evidence {
public:
    Evidence() {
        root = io::joinPath(FROST_BINDING_CORPUS_ARTIFACTS,
                            std::format("fec13-{}", std::chrono::steady_clock::now().time_since_epoch().count()))
                   .value_or("");
        EXPECT_TRUE(io::realFileSystem().createDirectories(root));
    }
    Result<void> write(std::string_view name, std::string_view bytes) const {
        auto path = io::joinPath(root, name);
        if (!path)
            return std::unexpected(path.error());
        auto parent = io::realFileSystem().createDirectories(io::parentPath(*path));
        if (!parent)
            return parent;
        return io::realFileSystem().writeFileAtomic(*path, bytes);
    }
    Result<io::ProcessResult> command(std::string tag, std::string executable, std::vector<std::string> arguments,
                                      std::string directory, std::chrono::milliseconds timeout = 5min) const {
        io::ProcessSpec spec;
        spec.executable = std::move(executable);
        spec.arguments = std::move(arguments);
        spec.workingDirectory = std::move(directory);
        auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, timeout);
        if (!result)
            return std::unexpected(result.error());
        auto output = write(tag + ".stdout", result->stdoutBytes);
        auto errors = write(tag + ".stderr", result->stderrBytes);
        Json args = Json::array();
        for (const auto& argument : spec.arguments)
            args.push(argument);
        auto invocation = write(tag + ".json", Json::object()
                                                   .set("program", spec.executable)
                                                   .set("arguments", args)
                                                   .set("workingDirectory", spec.workingDirectory)
                                                   .set("exitCode", result->exitCode)
                                                   .dump());
        if (!output || !errors || !invocation)
            return fail(ErrorCode::IoError, "could not retain real-tool evidence");
        return result;
    }
    std::string root; // Deliberately retained after failure/success; no destructor removes evidence/clone.
};

inline Result<std::string> disposableClone(Evidence& evidence, const std::string& pinned) {
    const auto root = io::joinPath(evidence.root, "corpora").value_or("");
    const auto invoke = [&](std::string tag, std::vector<std::string> tail) -> Result<void> {
        std::vector<std::string> args{"-B", FROST_BINDING_CORPORA_SCRIPT, "--repo", FROST_BINDING_REPO_ROOT, "--root",
                                      root};
        args.insert(args.end(), tail.begin(), tail.end());
        auto result = evidence.command(std::move(tag), FROST_TEST_PYTHON, std::move(args), evidence.root, 1h);
        if (!result)
            return std::unexpected(result.error());
        if (result->exitCode != 0)
            return fail(ErrorCode::IoError, "disposable pin provisioning/configuration failed; see retained evidence");
        return {};
    };
    auto fetched = invoke("clone-fetch", {"fetch", "--only", "frostengine", "--keep-old", "--fallback-source", pinned,
                                          "--cpm-source", pinned + "/.cache/cpm"});
    if (!fetched)
        return std::unexpected(fetched.error());
    auto verified = invoke("clone-verify", {"verify", "--only", "frostengine"});
    if (!verified)
        return std::unexpected(verified.error());
    auto configured = invoke("clone-configure", {"engine", "--only", "frostengine", "--configure"});
    if (!configured)
        return std::unexpected(configured.error());
    const auto clone = io::joinPath(root, "frostengine-" + std::string(FROST_CORPUS_FROSTENGINE_SHA).substr(0, 12));
    if (!clone)
        return std::unexpected(clone.error());
    const auto head = io::realFileSystem().readFile(*clone + "/.git/HEAD");
    if (!head || head->substr(0, std::string_view(FROST_CORPUS_FROSTENGINE_SHA).size()) != FROST_CORPUS_FROSTENGINE_SHA)
        return fail(ErrorCode::Corrupt, "disposable clone does not have the actual pinned detached HEAD");
    return *clone;
}

inline Result<void> build(Evidence& evidence, const std::string& clone, bool tests) {
    std::vector<std::string> args{"-B", clone + "/Scripts/Build.py", "--preset", FROST_CORPUS_FROSTENGINE_PRESET};
    if (tests)
        args.push_back("--test"); // Preserve the complete real engine test suite, including embedded-Lua tests.
    else
        args.insert(args.end(), {"--target", "FrostEditor"});
    auto result = evidence.command(tests ? "reviewed-preview-build-tests" : "documented-fixture-baseline-build",
                                   FROST_TEST_PYTHON, std::move(args), clone, 3h);
    if (!result)
        return std::unexpected(result.error());
    if (result->exitCode != 0)
        return fail(ErrorCode::IoError, "actual disposable engine build/test oracle failed; inspect retained output");
    return {};
}

class Environment {
public:
    Environment() {
        auto reactor = io::Reactor::create(io::ReactorOptions{1, "fec13-corpus"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().message();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<sched::ThreadPool>(sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~Environment() {
        if (lua)
            run(lua->stop());
        if (native)
            run(native->stop());
        if (m_reactor)
            m_reactor->shutdown();
        if (m_pool)
            m_pool->shutdown();
    }
    bool ok() const { return m_reactor && m_pool; }
    io::AsyncContext context() { return {*m_reactor, *m_pool}; }
    template<class T>
    T run(sched::Task<T> task) {
        return m_pool->spawn(sched::Lane::UserVisible, std::move(task)).join();
    }
    std::unique_ptr<lang::ClangdService> native;
    std::unique_ptr<lang::LuaService> lua;

private:
    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<sched::ThreadPool> m_pool;
};

class Database final : public lang::ICompileCommandsProvider {
public:
    explicit Database(std::string file) : m_file(std::move(file)) {}
    Result<std::optional<lang::CompileCommandsLocation>> locate() override {
        return std::optional(lang::CompileCommandsLocation{.directory = std::string(io::parentPath(m_file)),
                                                           .file = m_file,
                                                           .source = lang::CompileCommandsSource::Existing});
    }

private:
    std::string m_file;
};

struct RecordedRequest {
    std::string method;
    Json params, result;
};
class RecordingProvider final : public lang::ILspRequester {
public:
    explicit RecordingProvider(lang::ILspRequester& server) : m_server(server) {}
    sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token stop = {}) override {
        auto result = co_await m_server.request(method, params, stop);
        if (result)
            records.push_back({std::move(method), std::move(params), *result});
        co_return result;
    }
    lang::PositionEncoding encoding() const override { return m_server.encoding(); }
    std::optional<lang::ServerCapabilities> serverCapabilities() const override {
        return m_server.serverCapabilities();
    }
    std::vector<RecordedRequest> records; // Queries are deliberately serialized by this acceptance test.
private:
    lang::ILspRequester& m_server;
};

inline Json parameters(const engine::BindingQuerySeed& seed) {
    return Json::object()
        .set("textDocument", Json::object().set("uri", lang::pathToUri(seed.anchor.path).value()))
        .set("position",
             lang::toJson(lang::toLspPosition(*seed.anchorSnapshot, seed.anchor.bytes.begin, seed.provider.encoding)));
}

} // namespace frost::test::binding
