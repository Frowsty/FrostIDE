// SPDX-License-Identifier: Apache-2.0
// Qt-free proposed file-operation plan values and pure structural checks (R8 REF1-0).
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/transaction_id.hpp"
#include "frost/io/path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace frost::io {

// Owning declarations, not verified native observations, handles, digests computed here or authority.
// Concurrent const access is safe. Callers synchronize mutation. No serialized schema is frozen.
using FilePlanDigest = std::array<std::byte, 32>;
struct FilePlanObjectIdentity {
    std::string volume;
    std::string object;
    friend bool operator==(const FilePlanObjectIdentity&, const FilePlanObjectIdentity&) = default;
};
enum class FilePlanCaseRule : unsigned char { Sensitive, Insensitive };
struct FilePlanEndpoint {
    std::string parentPath; // Absolute normalized spelling in the plan's explicit PathStyle.
    FilePlanObjectIdentity parent;
    std::string leaf;
    std::string lookupKey; // Declared native comparison key; validation does not derive/verify it.
    FilePlanCaseRule caseRule = FilePlanCaseRule::Sensitive;
};
struct FilePlanImage {
    std::uint64_t size = 0; // Zero is an existing empty file, never absence.
    std::optional<FilePlanDigest> bytesDigest;
    std::string metadataProfile;
    std::optional<FilePlanDigest> metadataDigest;
};
struct FilePlanExisting {
    FilePlanEndpoint endpoint;
    FilePlanObjectIdentity object;
    FilePlanImage image;
};
struct FilePlanAbsent {
    FilePlanEndpoint endpoint; // An absence declaration is not a held namespace reservation.
};
struct FilePlanCaseOnly {
    FilePlanEndpoint endpoint;
    FilePlanObjectIdentity expectedSource;
};
struct FilePlanReplace {
    FilePlanExisting before;
    FilePlanImage after; // Same namespace endpoint; replacement object identity is not predicted.
};
struct FilePlanCreate {
    FilePlanAbsent before;
    FilePlanImage after;
};
struct FilePlanDelete {
    FilePlanExisting before;
};
struct FilePlanRename {
    FilePlanExisting before;
    std::variant<FilePlanAbsent, FilePlanCaseOnly> destination;
    // Rename carries unchanged disk bytes/metadata. Content+rename chains are a future schema slice.
};
struct FilePlanOperation {
    TransactionOperationId id;
    TransactionArtifactId artifact;
    std::variant<FilePlanReplace, FilePlanCreate, FilePlanDelete, FilePlanRename> effect;
};
struct FileTransactionPlan {
    TransactionId id;
    PathStyle pathStyle = kNativePathStyle;
    std::vector<FilePlanOperation> operations;
};

/*
 * Pure structural validation, O(n^2 * m + total field bytes), m = largest field length, with temporary O(n) storage.
 * Borrows the immutable plan for the call; thread-safe for concurrent const callers.
 * Errors: InvalidArgument for malformed/inconsistent/duplicate identities, images or endpoints;
 * Unsupported for cross-volume rename. No IO, semantic proof, live absence, metadata support,
 * mutability, admission, decision verification, native Apply or cleanup permission is established.
 * This initial one-operation-per-artifact plan rejects collisions/chains rather than scheduling them.
 */
[[nodiscard]] Result<void> validateFileTransactionPlan(const FileTransactionPlan& plan);

// Pure interpretation of DECLARED evidence only, never validation of a journal or cleanup authority.
enum class FilePlanDecision : unsigned char { Unknown, ProvenNoCommit, VerifiedAbort, VerifiedCommit };
enum class FilePlanRecovery : unsigned char { Unresolved, UncommittedPreparation, CommittedForward, Conflict };
struct FilePlanDecisionEvidence {
    FilePlanDecision decision = FilePlanDecision::Unknown;
    bool managedEffectsObserved = false;
    bool participantVerifiedCommit = false;
};
// O(1), pure/thread-safe. Unknown never implies abort; even UncommittedPreparation needs separate
// fresh ownership/native checks before cleanup. Contradictory declarations classify Conflict.
[[nodiscard]] FilePlanRecovery classifyFilePlanRecovery(FilePlanDecisionEvidence evidence) noexcept;

} // namespace frost::io
