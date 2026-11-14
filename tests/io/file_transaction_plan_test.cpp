// SPDX-License-Identifier: Apache-2.0
// Structural plan declarations only: these synthetic values do not claim live IO or recovery proof.
#include "frost/core/transaction_id.hpp"
#include "frost/io/file_transaction_plan.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace {
using namespace frost::io;
using frost::ErrorCode;

template<class T>
concept AddableIdentity = requires(T value) { value + value; };

template<class Id>
Id id(unsigned char value) {
    std::array<std::byte, 16> bytes{};
    bytes.back() = static_cast<std::byte>(value);
    return Id(bytes);
}
FilePlanImage emptyImage() {
    // Present synthetic digests with size zero describe an empty file, not a missing file.
    return {0, FilePlanDigest{}, "test-metadata/v1", FilePlanDigest{}};
}
FilePlanEndpoint sourceEndpoint() {
    return {"/workspace/src", {"volume", "src-parent"}, "old.cpp", "old.cpp", FilePlanCaseRule::Sensitive};
}
FilePlanEndpoint destinationEndpoint() {
    return {"/workspace/include", {"volume", "include-parent"}, "new.cpp", "new.cpp", FilePlanCaseRule::Sensitive};
}
FilePlanExisting beforeFile() { return {sourceEndpoint(), {"volume", "old-object"}, emptyImage()}; }
template<class Effect>
FilePlanOperation operation(Effect effect, unsigned char value = 1) {
    return {id<frost::TransactionOperationId>(value), id<frost::TransactionArtifactId>(value), std::move(effect)};
}
FileTransactionPlan plan(FilePlanOperation effect) {
    return {id<frost::TransactionId>(1), PathStyle::Posix, {std::move(effect)}};
}
void invalid(const FileTransactionPlan& value, ErrorCode code = ErrorCode::InvalidArgument) {
    const auto checked = validateFileTransactionPlan(value);
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error().code(), code) << checked.error().describe();
}

TEST(FileTransactionPlan, OpaqueIdsHaveNoArithmeticOrCrossKindConversion) {
    static_assert(!AddableIdentity<frost::TransactionId>);
    static_assert(!std::is_convertible_v<frost::TransactionId, frost::TransactionOperationId>);
    static_assert(!std::is_convertible_v<std::array<std::byte, 16>, frost::TransactionId>);
    EXPECT_FALSE(frost::TransactionId{}.assigned());
    EXPECT_TRUE(id<frost::TransactionId>(1).assigned());
    EXPECT_EQ(id<frost::TransactionId>(1), id<frost::TransactionId>(1));
    EXPECT_NE(id<frost::TransactionId>(1), id<frost::TransactionId>(2));
}

TEST(FileTransactionPlan, AllFourVariantsRepresentExistingEmptyFilesAndActualAbsenceSeparately) {
    static_assert(!std::is_convertible_v<FilePlanAbsent, FilePlanExisting>);
    const auto existing = beforeFile();
    const FilePlanAbsent absent{destinationEndpoint()};
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(FilePlanReplace{existing, emptyImage()}))));
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(FilePlanCreate{absent, emptyImage()}))));
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(FilePlanDelete{existing}))));
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(FilePlanRename{existing, absent}))));
    EXPECT_EQ(existing.image.size, 0U);
    EXPECT_TRUE(existing.image.bytesDigest.has_value());
    // Missing bytes evidence is not excused by size zero, nor replaced by an empty-file assumption.
    auto missing = emptyImage();
    missing.bytesDigest.reset();
    invalid(plan(operation(FilePlanCreate{absent, missing})));
}

TEST(FileTransactionPlan, RenameChecksBothParentsAndTheExplicitDestination) {
    auto source = beforeFile();
    auto destination = destinationEndpoint();
    destination.parentPath = "relative";
    invalid(plan(operation(FilePlanRename{source, FilePlanAbsent{destination}})));
    destination = destinationEndpoint();
    source.endpoint.parent.object.clear();
    invalid(plan(operation(FilePlanRename{source, FilePlanAbsent{destination}})));
    source = beforeFile();
    destination.lookupKey.clear();
    invalid(plan(operation(FilePlanRename{source, FilePlanAbsent{destination}})));
    destination = destinationEndpoint();
    destination.parent.volume = "other-volume";
    invalid(plan(operation(FilePlanRename{source, FilePlanAbsent{destination}})), ErrorCode::Unsupported);
}

TEST(FileTransactionPlan, IdentityAndMetadataAreMandatoryOnExistingAndReplacementImages) {
    auto existing = beforeFile();
    existing.object.volume = "another-volume";
    invalid(plan(operation(FilePlanDelete{existing})));
    existing = beforeFile();
    existing.object = existing.endpoint.parent;
    invalid(plan(operation(FilePlanDelete{existing})));
    existing = beforeFile();
    existing.image.metadataDigest.reset();
    invalid(plan(operation(FilePlanDelete{existing})));
    auto after = emptyImage();
    after.metadataProfile.clear();
    invalid(plan(operation(FilePlanReplace{beforeFile(), after})));
}

TEST(FileTransactionPlan, CaseOnlyRenameNeedsSameSourceParentAndDeclaredNativeLookup) {
    auto source = beforeFile();
    source.endpoint.leaf = "Old.cpp";
    source.endpoint.caseRule = FilePlanCaseRule::Insensitive;
    auto destination = source.endpoint;
    destination.leaf = "old.cpp";
    auto rename = FilePlanRename{source, FilePlanCaseOnly{destination, source.object}};
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(rename))));
    std::get<FilePlanCaseOnly>(rename.destination).expectedSource.object = "another-object";
    invalid(plan(operation(rename)));
    rename.destination = FilePlanCaseOnly{destination, source.object};
    std::get<FilePlanCaseOnly>(rename.destination).endpoint.parent.object = "another-parent";
    invalid(plan(operation(rename)));
    rename.destination = FilePlanCaseOnly{destination, source.object};
    std::get<FilePlanCaseOnly>(rename.destination).endpoint.caseRule = FilePlanCaseRule::Sensitive;
    invalid(plan(operation(rename)));
    // An equivalent lookup cannot be smuggled in as an absent destination.
    invalid(plan(operation(FilePlanRename{source, FilePlanAbsent{destination}})));
}

TEST(FileTransactionPlan, DuplicateIdsArtifactWritersAndPhysicalAliasesAreRejected) {
    auto first = operation(FilePlanDelete{beforeFile()});
    auto other = beforeFile();
    other.endpoint = destinationEndpoint();
    other.object.object = "other-object";
    auto second = operation(FilePlanDelete{other}, 2);
    auto value = plan(first);
    value.operations.push_back(second);
    ASSERT_TRUE(validateFileTransactionPlan(value));
    value.operations.back().id = first.id;
    invalid(value);
    value.operations.back() = second;
    value.operations.back().artifact = first.artifact;
    invalid(value);
    value.operations.back() = second;
    std::get<FilePlanDelete>(value.operations.back().effect).before.object = beforeFile().object;
    invalid(value);
}

TEST(FileTransactionPlan, AnotherOperationCannotWriteEitherRenameEndpointEvenThroughAlias) {
    auto value = plan(operation(FilePlanRename{beforeFile(), FilePlanAbsent{destinationEndpoint()}}));
    auto alias = destinationEndpoint();
    alias.parentPath = "/alias/to/include"; // Same declared native parent and lookup, different spelling.
    value.operations.push_back(operation(FilePlanCreate{FilePlanAbsent{alias}, emptyImage()}, 2));
    invalid(value);
    alias = sourceEndpoint();
    value.operations.back() = operation(FilePlanCreate{FilePlanAbsent{alias}, emptyImage()}, 2);
    invalid(value);
    // Exact lexical collision is invalid even when declarations disagree about native parent IDs.
    alias.parent.object = "inconsistent-parent";
    value.operations.back() = operation(FilePlanCreate{FilePlanAbsent{alias}, emptyImage()}, 2);
    invalid(value);
}

TEST(FileTransactionPlan, RejectsMissingIdentitiesMalformedLeavesAndInvalidPathStyle) {
    auto value = plan(operation(FilePlanDelete{beforeFile()}));
    value.id = {};
    invalid(value);
    value.id = id<frost::TransactionId>(1);
    value.operations.front().id = {};
    invalid(value);
    value.operations.front().id = id<frost::TransactionOperationId>(1);
    value.operations.front().artifact = {};
    invalid(value);
    value.operations.clear();
    invalid(value);
    for (const auto* leaf : {".", "..", "child/file.cpp"}) {
        auto file = beforeFile();
        file.endpoint.leaf = leaf;
        invalid(plan(operation(FilePlanDelete{file})));
    }
    auto file = beforeFile();
    file.endpoint.leaf = std::string("a\0b", 3);
    invalid(plan(operation(FilePlanDelete{file})));
    value = plan(operation(FilePlanDelete{beforeFile()}));
    value.pathStyle = static_cast<PathStyle>(255);
    invalid(value);
}

TEST(FileTransactionPlan, PureChecksUseExplicitWindowsRulesOnEitherPlatform) {
    auto file = beforeFile();
    file.endpoint.parentPath = "C:\\workspace\\src";
    file.endpoint.leaf = "file.cpp";
    auto value = plan(operation(FilePlanDelete{file}));
    value.pathStyle = PathStyle::Windows;
    ASSERT_TRUE(validateFileTransactionPlan(value));
    std::get<FilePlanDelete>(value.operations.front().effect).before.endpoint.leaf = "folder\\file.cpp";
    invalid(value);
}

TEST(FileTransactionPlan, FullWindowsLeavesRejectStreamsReservedNamesAndRewrittenOrInvalidSpellings) {
    for (const auto* leaf : {"file.cpp:stream", "CON", "name.", "name ", "?", "has*wildcard.cpp", "pipe|name",
                             "less<name", "quote\"name"}) {
        SCOPED_TRACE(leaf);
        auto file = beforeFile();
        file.endpoint.parentPath = R"(C:\workspace\src)";
        file.endpoint.leaf = leaf;
        file.endpoint.lookupKey = leaf;
        auto value = plan(operation(FilePlanDelete{file}));
        value.pathStyle = PathStyle::Windows;
        invalid(value);
        auto source = beforeFile();
        source.endpoint.parentPath = R"(C:\workspace\src)";
        auto destination = file.endpoint;
        destination.parentPath = R"(C:\workspace\include)";
        destination.parent.object = "include-parent";
        value = plan(operation(FilePlanRename{source, FilePlanAbsent{destination}}));
        value.pathStyle = PathStyle::Windows;
        invalid(value);
    }
    // Preserve the existing deliberate verbatim escape for ordinary Win32 name rewriting.
    for (const auto* leaf : {"CON", "name.", "name "}) {
        auto file = beforeFile();
        file.endpoint.parentPath = R"(\\?\C:\workspace\src)";
        file.endpoint.leaf = leaf;
        file.endpoint.lookupKey = leaf;
        auto value = plan(operation(FilePlanDelete{file}));
        value.pathStyle = PathStyle::Windows;
        ASSERT_TRUE(validateFileTransactionPlan(value));
    }
    auto posix = beforeFile();
    posix.endpoint.leaf = "file.cpp:stream";
    posix.endpoint.lookupKey = posix.endpoint.leaf;
    ASSERT_TRUE(validateFileTransactionPlan(plan(operation(FilePlanDelete{posix}))));
}

TEST(FileTransactionPlan, ExactNativeLeafCollisionCannotBeHiddenByAliasAndConflictingLookupKey) {
    auto alias = sourceEndpoint();
    alias.parentPath = "/alias/src";
    alias.lookupKey = "contradictory-key"; // Exact parent+leaf remains the same declared namespace endpoint.
    invalid(plan(operation(FilePlanRename{beforeFile(), FilePlanAbsent{alias}})));
    auto value = plan(operation(FilePlanDelete{beforeFile()}));
    value.operations.push_back(operation(FilePlanCreate{FilePlanAbsent{alias}, emptyImage()}, 2));
    invalid(value);
}

TEST(FileTransactionPlan, ParentDeclarationsMustAgreeWithinRenameAndAcrossIndependentOperations) {
    auto destination = sourceEndpoint();
    destination.leaf = "different.cpp";
    destination.lookupKey = destination.leaf;
    destination.parentPath = "/alias/src";
    destination.caseRule = FilePlanCaseRule::Insensitive;
    invalid(plan(operation(FilePlanRename{beforeFile(), FilePlanAbsent{destination}})));
    auto second = beforeFile();
    second.endpoint = destination;
    second.object.object = "different-object";
    auto value = plan(operation(FilePlanDelete{beforeFile()}));
    value.operations.push_back(operation(FilePlanDelete{second}, 2));
    invalid(value);
    // One exact lexical parent cannot simultaneously declare two native directory identities,
    // even for distinct, noncolliding leaves and lookup keys.
    second.endpoint.caseRule = FilePlanCaseRule::Sensitive;
    second.endpoint.parentPath = sourceEndpoint().parentPath;
    second.endpoint.parent.object = "contradictory-parent";
    value.operations.back() = operation(FilePlanDelete{second}, 2);
    invalid(value);
    invalid(plan(operation(FilePlanRename{beforeFile(), FilePlanAbsent{second.endpoint}})));
}

TEST(FileTransactionPlan, LegitimateParentAliasesWithDistinctLeavesRemainStructurallyValid) {
    auto other = beforeFile();
    other.endpoint.parentPath = "/alias/src";
    other.endpoint.leaf = "different.cpp";
    other.endpoint.lookupKey = other.endpoint.leaf;
    other.object.object = "different-object";
    auto value = plan(operation(FilePlanDelete{beforeFile()}));
    value.operations.push_back(operation(FilePlanDelete{other}, 2));
    ASSERT_TRUE(validateFileTransactionPlan(value));
    ASSERT_TRUE(
        validateFileTransactionPlan(plan(operation(FilePlanRename{beforeFile(), FilePlanAbsent{other.endpoint}}))));
}

TEST(FileTransactionPlan, UnknownDecisionNeverBecomesUncommittedFromBeforeOrEffectsObservation) {
    EXPECT_EQ(classifyFilePlanRecovery({FilePlanDecision::Unknown, false, false}), FilePlanRecovery::Unresolved);
    EXPECT_EQ(classifyFilePlanRecovery({FilePlanDecision::Unknown, true, false}), FilePlanRecovery::Unresolved);
    // A valid participant COMMIT is stronger evidence; it never permits preparation cleanup.
    EXPECT_EQ(classifyFilePlanRecovery({FilePlanDecision::Unknown, false, true}), FilePlanRecovery::CommittedForward);
    EXPECT_EQ(classifyFilePlanRecovery({FilePlanDecision::VerifiedCommit, false, false}),
              FilePlanRecovery::CommittedForward);
}

TEST(FileTransactionPlan, OnlyPositiveUncommittedEvidenceClassifiesPreparationAndContradictionsConflict) {
    for (const auto decision : {FilePlanDecision::ProvenNoCommit, FilePlanDecision::VerifiedAbort}) {
        EXPECT_EQ(classifyFilePlanRecovery({decision, false, false}), FilePlanRecovery::UncommittedPreparation);
        EXPECT_EQ(classifyFilePlanRecovery({decision, true, false}), FilePlanRecovery::Conflict);
        EXPECT_EQ(classifyFilePlanRecovery({decision, false, true}), FilePlanRecovery::Conflict);
    }
    EXPECT_EQ(classifyFilePlanRecovery({static_cast<FilePlanDecision>(255), false, false}), FilePlanRecovery::Conflict);
}
} // namespace
