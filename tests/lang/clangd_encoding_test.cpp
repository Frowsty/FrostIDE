// SPDX-License-Identifier: Apache-2.0
//
// Position-encoding negotiation (LSP 3.17 general.positionEncodings, FND-9a) against the real clangd
// of the pinned LLVM: FrostIDE offers ["utf-8", "utf-16"] (or only "utf-16") and no longer clangd's
// deprecated offsetEncoding extension, clangd answers capabilities.positionEncoding, and columns on
// a line with non-ASCII text (U+1F600, a UTF-16 surrogate pair, and U+00E9) are exchanged in the
// negotiated units both ways: a definition request sent at the column of `target` on that line, and
// the references answer that points back into it. Each test starts one clangd over LspClient and
// shuts it down before returning (label "clangd").

#include "frost/io/async_process.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/jsonrpc.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;
using frost::lang::Json;
using frost::lang::LspClient;
using frost::lang::PositionEncoding;

// Line 1 holds U+1F600 (4 UTF-8 bytes, 2 UTF-16 units) and U+00E9 (2 bytes, 1 unit) before `target`.
constexpr std::string_view kSource = "int target = 0;\n"
                                     "int main() { const char* s = \"\xF0\x9F\x98\x80\xC3\xA9\"; return target; }\n";
// The column of `target` on line 1: 42 code points, 43 UTF-16 units, 46 UTF-8 bytes.
constexpr std::int64_t kUtf16Column = 43;
constexpr std::int64_t kUtf8Column = 46;

struct Outcome {
    std::string advertised;           // capabilities.positionEncoding as clangd sent it.
    PositionEncoding encoding{};      // What LspClient negotiated.
    bool legacyAnswer = false;        // clangd still answered offsetEncoding.
    std::int64_t definitionLine = -1; // Where the definition request at the line-1 column landed.
    std::int64_t definitionColumn = -1;
    std::int64_t referenceColumn = -1; // The line-1 reference's start column from clangd.
    std::string failure;
};

frost::sched::Task<Outcome> exchange(LspClient& client, std::string uri, std::int64_t column) {
    Outcome out;
    auto initialized = co_await client.initialize();
    if (!initialized) {
        out.failure = "initialize: " + initialized.error().describe();
        co_return out;
    }
    out.advertised = std::string(initialized->raw.stringAt("positionEncoding"));
    out.legacyAnswer = initialized->raw.find("offsetEncoding") != nullptr;
    out.encoding = client.encoding();
    if (auto opened = client.didOpen(uri, "cpp", 1, kSource); !opened) {
        out.failure = "didOpen: " + opened.error().describe();
        co_return out;
    }
    const auto at = [&](std::int64_t line, std::int64_t character) {
        return Json::object()
            .set("textDocument", Json::object().set("uri", uri))
            .set("position", Json::object().set("line", line).set("character", character));
    };
    auto definition = co_await client.request("textDocument/definition", at(1, column));
    if (!definition) {
        out.failure = "definition: " + definition.error().describe();
        co_return out;
    }
    const Json* first =
        definition->isArray() && !definition->items().empty() ? &definition->items().front() : &*definition;
    if (const Json* range = first->find("range"); range != nullptr) {
        out.definitionLine = range->find("start")->integerAt("line").value_or(-1);
        out.definitionColumn = range->find("start")->integerAt("character").value_or(-1);
    }
    Json references = at(0, 4);
    references.set("context", Json::object().set("includeDeclaration", false));
    auto found = co_await client.request("textDocument/references", std::move(references));
    if (!found) {
        out.failure = "references: " + found.error().describe();
        co_return out;
    }
    if (found->isArray()) {
        for (const Json& location : found->items()) {
            const Json* start = location.find("range") ? location.find("range")->find("start") : nullptr;
            if (start != nullptr && start->integerAt("line") == 1)
                out.referenceColumn = start->integerAt("character").value_or(-1);
        }
    }
    if (auto down = co_await client.shutdown(10s); !down)
        out.failure = "shutdown: " + down.error().describe();
    co_return out;
}

// Ends the child's process tree (a no-op after a clean exit) and waits for it.
frost::sched::Task<void> reap(std::shared_ptr<frost::io::IAsyncProcess> child) {
    (void)child->terminate();
    (void)co_await child->exited();
}

Outcome failed(std::string why) {
    Outcome out;
    out.failure = std::move(why);
    return out;
}

// Runs one clangd over LspClient with the given offer and returns what it negotiated and answered.
Outcome run(bool utf16Only, std::int64_t column) {
    frost::test::disableCrashDialogs();
    auto clangd = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    if (!clangd) {
        return failed("the pinned clangd is missing: " + clangd.error().describe());
    }
    frost::test::ScratchDir scratch("clangd-encoding");
    const std::string file = scratch.join("main.cpp");
    if (auto written = frost::io::realFileSystem().writeFileAtomic(file, kSource); !written)
        return failed(written.error().describe());
    auto uri = frost::lang::pathToUri(file);
    if (!uri)
        return failed(uri.error().describe());
    auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-encoding"});
    if (!reactor)
        return failed(reactor.error().describe());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    Outcome out;
    {
        frost::io::ProcessSpec spec;
        spec.executable = clangd->path;
        spec.arguments = {"--log=error", "--background-index=false", "-j=1"};
        spec.workingDirectory = scratch.path();
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.stdoutMode = frost::io::Stdio::Pipe;
        spec.stderrMode = frost::io::Stdio::Null;
        spec.killOnClose = true;
        auto launched = frost::io::realProcessLauncher().launchAsync(spec, {**reactor, pool});
        if (!launched) {
            out.failure = launched.error().describe();
        } else {
            std::shared_ptr<frost::io::IAsyncProcess> process(std::move(*launched));
            frost::lang::InitializeOptions init;
            init.rootPath = scratch.path();
            init.utf16Only = utf16Only;
            {
                LspClient client(frost::lang::processByteStream(process), pool, init, {});
                out = pool.spawn(frost::sched::Lane::UserVisible, exchange(client, *uri, column)).join();
            }
            // The client sent exit; make sure no process outlives the test either way.
            pool.spawn(frost::sched::Lane::UserVisible, reap(std::move(process))).join();
        }
    }
    (*reactor)->shutdown();
    pool.shutdown();
    return out;
}

TEST(ClangdEncoding, Utf16OfferExchangesUtf16ColumnsOnANonAsciiLine) {
    const Outcome out = run(true, kUtf16Column);
    ASSERT_TRUE(out.failure.empty()) << out.failure;
    EXPECT_EQ(out.advertised, "utf-16");
    EXPECT_FALSE(out.legacyAnswer) << "offsetEncoding was not offered, so clangd must not answer it";
    EXPECT_EQ(out.encoding, PositionEncoding::Utf16);
    // The request column (UTF-16 units) reached `target`, whose definition is on line 0.
    EXPECT_EQ(out.definitionLine, 0);
    EXPECT_EQ(out.definitionColumn, 4);
    // The reference on the non-ASCII line comes back in UTF-16 units, not bytes or code points.
    EXPECT_EQ(out.referenceColumn, kUtf16Column);
}

TEST(ClangdEncoding, DefaultOfferNegotiatesUtf8Columns) {
    const Outcome out = run(false, kUtf8Column);
    ASSERT_TRUE(out.failure.empty()) << out.failure;
    EXPECT_EQ(out.advertised, "utf-8");
    EXPECT_FALSE(out.legacyAnswer);
    EXPECT_EQ(out.encoding, PositionEncoding::Utf8);
    EXPECT_EQ(out.definitionLine, 0);
    EXPECT_EQ(out.definitionColumn, 4);
    EXPECT_EQ(out.referenceColumn, kUtf8Column);
}

} // namespace
