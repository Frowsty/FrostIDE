// SPDX-License-Identifier: Apache-2.0
//
// Unit tests of document_features.hpp (CPP-5) and save_actions.hpp's wait (REF-12): document
// links, folding ranges and clangd's AST parsed from answers shaped like clangd 23's (the shapes
// were recorded from the pinned clangd on the FrostEngine clone; the real-clangd checks are in
// clangd_features_test.cpp), the capability flags and client capabilities, and waitForResult's
// result, its timeout and the cancellation that timeout sends.

#include "frost/lang/document_features.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/save_actions.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace fl = frost::lang;
using fl::Json;
using namespace std::chrono_literals;

Json parse(std::string_view text) {
    auto json = fl::parseJson(text);
    EXPECT_TRUE(json.has_value()) << text;
    return json.value_or(Json());
}

// An absolute path in the native style (never touched on disk).
std::string anyFile(std::string_view name) {
#ifdef _WIN32
    std::string path = "C:\\work\\features\\" + std::string(name);
    std::ranges::replace(path, '/', '\\');
    return path;
#else
    return "/work/features/" + std::string(name);
#endif
}

TEST(DocumentLinks, IncludeLinksBecomePathsAndOtherTargetsStayUris) {
    const std::string header = anyFile("Editor/EditorApplication.h");
    const std::string uri = fl::pathToUri(header).value();
    const Json answer = parse(
        R"([{"range":{"start":{"line":2,"character":9},"end":{"line":2,"character":37}},"target":")" + uri +
        R"("},{"range":{"start":{"line":3,"character":9},"end":{"line":3,"character":20}},"target":"https://example.invalid/x"},)"
        R"({"range":{"start":{"line":4}},"target":"file:///nowhere"},{"range":{"start":{"line":5,"character":1},"end":{"line":5,"character":4}},"tooltip":"lazy"}])");
    const auto links = fl::parseDocumentLinks(answer);
    ASSERT_TRUE(links.has_value()) << links.error().describe();
    ASSERT_EQ(links->size(), 3U); // The one without a valid range is dropped.
    EXPECT_EQ((*links)[0].range, (fl::LspRange{{2, 9}, {2, 37}}));
    EXPECT_EQ(fl::documentKey((*links)[0].path), fl::documentKey(header));
    EXPECT_EQ((*links)[0].uri, uri);
    EXPECT_TRUE((*links)[1].path.empty());
    EXPECT_EQ((*links)[1].uri, "https://example.invalid/x");
    EXPECT_TRUE((*links)[2].uri.empty());
    EXPECT_EQ((*links)[2].tooltip, "lazy");

    EXPECT_TRUE(fl::parseDocumentLinks(Json())->empty());
    EXPECT_FALSE(fl::parseDocumentLinks(parse(R"({"range":1})")).has_value());
}

TEST(FoldingRanges, LinesCharactersAndKindsAreReadAndBackwardRangesDropped) {
    // clangd 23's shape: "region" for braces, endCharacter left out when the range ends a line.
    const Json answer =
        parse(R"([{"startLine":26,"startCharacter":11,"endLine":36,"kind":"region"},)"
              R"({"startLine":30,"startCharacter":2,"endLine":34,"endCharacter":1,"kind":"region"},)"
              R"({"startLine":1,"endLine":4,"kind":"comment"},{"startLine":7,"endLine":9,"kind":"imports"},)"
              R"({"startLine":12,"endLine":13},{"startLine":9,"endLine":3},{"endLine":3},)"
              R"({"startLine":5,"startCharacter":9,"endLine":5,"endCharacter":2},{"startLine":-1,"endLine":2}])");
    const auto ranges = fl::parseFoldingRanges(answer);
    ASSERT_TRUE(ranges.has_value()) << ranges.error().describe();
    ASSERT_EQ(ranges->size(), 5U);
    EXPECT_EQ((*ranges)[0], (fl::FoldingRange{26, 11, 36, std::nullopt, fl::FoldingRangeKind::Region}));
    EXPECT_EQ((*ranges)[1], (fl::FoldingRange{30, 2, 34, 1, fl::FoldingRangeKind::Region}));
    EXPECT_EQ((*ranges)[2].kind, fl::FoldingRangeKind::Comment);
    EXPECT_EQ((*ranges)[3].kind, fl::FoldingRangeKind::Imports);
    EXPECT_EQ((*ranges)[4].kind, fl::FoldingRangeKind::Other);
    EXPECT_TRUE(fl::parseFoldingRanges(Json())->empty());
    EXPECT_FALSE(fl::parseFoldingRanges(Json::object()).has_value());
}

TEST(ClangdAst, NodesKeepTheirFieldsAndChildrenAndDeepTreesAreCut) {
    const Json answer = parse(
        R"({"role":"expression","kind":"BinaryOperator","detail":"+","arcana":"BinaryOperator <col:12, col:16> 'int' '+'",)"
        R"("range":{"start":{"line":3,"character":11},"end":{"line":3,"character":16}},"children":[)"
        R"({"role":"expression","kind":"ImplicitCast","children":[{"role":"expression","kind":"DeclRef","detail":"a"}]},)"
        R"({"role":"expression","kind":"DeclRef","detail":"b"}]})");
    const auto node = fl::parseAstNode(answer);
    ASSERT_TRUE(node.has_value()) << node.error().describe();
    ASSERT_TRUE(node->has_value());
    const fl::AstNode& root = **node;
    EXPECT_EQ(root.role, "expression");
    EXPECT_EQ(root.kind, "BinaryOperator");
    EXPECT_EQ(root.detail, "+");
    EXPECT_NE(root.arcana.find("'int'"), std::string::npos);
    EXPECT_EQ(root.range, (fl::LspRange{{3, 11}, {3, 16}}));
    ASSERT_EQ(root.children.size(), 2U);
    EXPECT_EQ(root.children[0].children.at(0).detail, "a");
    EXPECT_FALSE(root.children[1].range.has_value());
    EXPECT_EQ(fl::astNodeCount(root), 4U);
    EXPECT_FALSE(root.truncated);

    // null: no node encloses the range.
    const auto none = fl::parseAstNode(Json());
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
    EXPECT_FALSE(fl::parseAstNode(parse(R"({"detail":"x"})")).has_value());
    EXPECT_FALSE(fl::parseAstNode(parse(R"({"role":"x","children":[3]})")).has_value());

    /*
     * A chain deeper than kMaxAstDepth keeps kMaxAstDepth levels and marks the cut. It is built as a
     * value: the wire parser already refuses JSON nested deeper than 256 levels (two per AST level),
     * and parseAstNode must not depend on that.
     */
    Json deep = Json::object().set("role", "statement").set("kind", "Null");
    for (std::size_t i = 0; i < fl::kMaxAstDepth + 10; ++i) {
        deep = Json::object()
                   .set("role", "statement")
                   .set("kind", "Compound")
                   .set("children", Json::array({std::move(deep)}));
    }
    const auto cut = fl::parseAstNode(deep);
    ASSERT_TRUE(cut.has_value()) << cut.error().describe();
    EXPECT_EQ(fl::astNodeCount(**cut), fl::kMaxAstDepth);
    const fl::AstNode* last = &**cut;
    while (!last->children.empty()) {
        last = &last->children.front();
    }
    EXPECT_TRUE(last->truncated);
}

TEST(DocumentFeatureCapabilities, TheServerFlagsAreReadAndTheClientAnnouncesLinksAndFolding) {
    // The providers as clangd 23 announces them.
    const Json result = parse(R"({"capabilities":{"documentLinkProvider":{"resolveProvider":false},)"
                              R"("foldingRangeProvider":true,"astProvider":true}})");
    const fl::ServerCapabilities capabilities = fl::parseInitializeResult(result);
    EXPECT_TRUE(capabilities.documentLink);
    EXPECT_TRUE(capabilities.foldingRange);
    EXPECT_TRUE(capabilities.ast);
    const fl::ServerCapabilities none = fl::parseInitializeResult(parse(R"({"capabilities":{}})"));
    EXPECT_FALSE(none.documentLink || none.foldingRange || none.ast);

    const Json client = fl::clientCapabilities();
    const Json* textDocument = client.find("textDocument");
    ASSERT_NE(textDocument, nullptr);
    const Json* folding = textDocument->find("foldingRange");
    ASSERT_NE(folding, nullptr);
    EXPECT_EQ(folding->boolAt("lineFoldingOnly"), false);
    EXPECT_NE(textDocument->find("documentLink"), nullptr);
}

TEST(SaveActionWait, TheResultArrivesAndATimeoutCancelsTheRequest) {
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto value = fl::waitForResult<int>(
        pool, [](std::stop_token) -> frost::sched::Task<frost::Result<int>> { co_return 42; }, 5s);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);

    // A request that waits until it is cancelled: the wait gives up with Timeout and stops it.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto begin = std::chrono::steady_clock::now();
    auto late = fl::waitForResult<int>(
        pool,
        [&pool, cancelled](std::stop_token token) -> frost::sched::Task<frost::Result<int>> {
            return [](frost::sched::ThreadPool& on, std::stop_token stop,
                      std::shared_ptr<std::atomic<bool>> flag) -> frost::sched::Task<frost::Result<int>> {
                (void)co_await on.sleepFor(std::chrono::minutes(10), stop);
                flag->store(stop.stop_requested());
                co_return frost::fail(frost::ErrorCode::Cancelled, "stopped");
            }(pool, std::move(token), cancelled);
        },
        50ms);
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), frost::ErrorCode::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 30s);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!cancelled->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(cancelled->load()) << "the timed-out request was not stopped";
    pool.shutdown();
}

TEST(SaveActionWait, CapturingFactorySurvivesTimeoutUntilDeferredCoroutineCompletes) {
    std::weak_ptr<std::string> payloadLifetime;
    std::string capturedBytes;
    bool sawCancellation = false;
    frost::sched::SimExecutor executor;
    {
        auto payload = std::make_shared<std::string>("unsaved document bytes");
        payloadLifetime = payload;
        auto coroutine = [payload = std::move(payload), &capturedBytes,
                          &sawCancellation](std::weak_ptr<std::string> lifetime,
                                            std::stop_token token) -> frost::sched::Task<frost::Result<int>> {
            // This frame-owned guard lets the unfixed path drain safely without reading dead captures.
            if (lifetime.expired()) {
                co_return frost::fail(frost::ErrorCode::Internal, "capturing factory was destroyed");
            }
            capturedBytes = *payload;
            sawCancellation = token.stop_requested();
            co_return 42;
        };
        auto factory = std::bind_front(std::move(coroutine), payloadLifetime);
        auto result = fl::waitForResult<int>(executor, std::move(factory), 0ms);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    }
    // Only waitForResult's retained factory can own the payload after the caller's scope ends.
    EXPECT_FALSE(payloadLifetime.expired());
    EXPECT_TRUE(capturedBytes.empty());
    EXPECT_EQ(executor.dispatches(), 0U);
    EXPECT_GT(executor.runUntilIdle(), 0U);
    EXPECT_EQ(capturedBytes, "unsaved document bytes");
    EXPECT_TRUE(sawCancellation);
    EXPECT_TRUE(payloadLifetime.expired());
}

} // namespace
