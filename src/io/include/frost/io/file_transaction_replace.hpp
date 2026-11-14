// SPDX-License-Identifier: Apache-2.0
// Native retained-original replacement primitives; not a multi-file Apply/Undo coordinator.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_transaction_prepare.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::io {

enum class FileReplacementPhase : unsigned char {
    Prepared,
    OriginalRetiredUnverified,
    OriginalRetained,
    AfterInstalledUnverified,
    AfterInstalled,
    AfterRetiredUnverified,
    AfterRetained,
    BeforeRestoredUnverified,
    BeforeRestored,
    OriginalMovedUnverified,
    OriginalMoved,
    MoveRetainedForInverse,
    MoveRestoredUnverified,
    MoveRestored
};

struct FileReplacementReceipt {
    FileImageReceipt images;
    std::string replacementPath;
    std::string retainedBeforePath;
    std::string metadataProfile;
    std::string moveDestination;
    FileImageObservation moveParent;
    std::string beforeMetadataSha256;
    std::string privateAfterMetadataSha256;
    std::string installedAfterMetadataSha256;
    FileReplacementPhase phase = FileReplacementPhase::Prepared;
};

// Always carries known progress, including when a native effect succeeded but verification failed.
// An error never means that the workspace was unchanged: inspect receipt.phase and retained paths.
struct FileReplacementOutcome {
    FileReplacementReceipt receipt;
    std::optional<Error> error;
    [[nodiscard]] bool complete() const noexcept { return !error.has_value(); }
};

/*
 * Move-only, single-owner-thread native participant. Each operation is O(Before+After), bounded by
 * preparation limits. The private same-volume store has the byte-Prepare ownership contract.
 * Endpoint operations are no-replace; they can expose an absent name between retirement/install.
 * External processes may observe gaps or ordered effects. No hostile same-user namespace exclusion,
 * whole-workspace atomicity, durable COMMIT, power-loss recovery or global Undo is provided here.
 *
 * Keep receipt() before destruction for recovery. Destruction/move assignment ONLY close handles;
 * no namespace mutation, recursive cleanup or implicit rollback occurs. Retained Before is mutable
 * recovery material, not permanent authority: inverse revalidates it and current After first.
 * Read/rename-induced access/change times and a replacement's inode/file ID are not preserved.
 * First limited metadata profile (NOT metadata-complete or global-Apply-ready):
 * Windows: regular single-link NTFS; basic attributes/creation/write times and readable
 * owner/group/DACL. EA/ADS, special attributes, object IDs and workspace short names refuse on
 * positive observation; failed inspections are errors. SACL is UNOBSERVED/UNPROVEN, not claimed
 * preserved. No privilege changes. Unpublished After/store DACLs allow only owner/system/admin;
 * retained previously published objects keep their own DACL, so the store name alone does not
 * hide them from identities already authorized on those objects (traverse bypass can apply).
 * original DACL restoration is installation-dependent and any failure reports published progress.
 * POSIX: ext4 regular single-link, ordinary 0777 mode/uid/gid/mtime, only zero/extent inode flags;
 * complete xattr-name set must be empty (including ACL/security attrs). Query failures refuse.
 * Immutable/append-only/other flags and set-id/sticky mode refuse. Private store is euid/0700.
 * Access/change times and new inode identity differ by design. No claim of complete historical
 * metadata/security equivalence: later coordinated Apply must upgrade this deliberately limited type.
 */
class PreparedFileReplacement {
public:
    ~PreparedFileReplacement();
    PreparedFileReplacement(PreparedFileReplacement&&) noexcept;
    PreparedFileReplacement& operator=(PreparedFileReplacement&&) noexcept;
    PreparedFileReplacement(const PreparedFileReplacement&) = delete;
    PreparedFileReplacement& operator=(const PreparedFileReplacement&) = delete;

    [[nodiscard]] const FileReplacementReceipt* receipt() const noexcept;
    // Prepared -> OriginalRetained. Failed postchecks retain OriginalRetiredUnverified progress.
    [[nodiscard]] FileReplacementOutcome retireOriginal();
    // OriginalRetained -> AfterInstalled; refuses any occupied destination, including foreign data.
    [[nodiscard]] FileReplacementOutcome installAfter();
    // AfterInstalled -> AfterRetained -> BeforeRestored. Every endpoint is no-replace; conflicts
    // preserve both objects. AfterRetained can be retried only after revalidating both owned images.
    [[nodiscard]] FileReplacementOutcome restoreBefore();
    // Repeat the frozen forward/inverse after a reported partial effect. Revalidates every owned
    // object; foreign/tampered objects remain conflicts. No new preview is accepted on recovery.
    [[nodiscard]] FileReplacementOutcome settle();
    [[nodiscard]] FileReplacementOutcome reapplyAfter();
    // Same-volume reversible source-file movement, including case-only names via private retirement.
    // Prepared/OriginalRetained only; unchanged bytes+metadata. Destination never overwritten.
    [[nodiscard]] Result<void> prepareMoveDestination(std::string_view destination);
    [[nodiscard]] FileReplacementOutcome moveOriginalTo(std::string_view destination);
    [[nodiscard]] FileReplacementOutcome restoreMove();
    [[nodiscard]] FileReplacementOutcome reapplyMove();
    // Fresh all-participant preflight used before coordinated Undo/Redo; no namespace changes.
    [[nodiscard]] Result<void> validateCurrent(bool after) const;

private:
    struct State;
    explicit PreparedFileReplacement(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> m_state;
    friend Result<PreparedFileReplacement> recoverFileReplacement(const FileReplacementReceipt&, std::string_view,
                                                                  std::string_view, bool, std::size_t);
    friend Result<PreparedFileReplacement> prepareFileReplacement(std::string_view, std::string_view, std::string_view,
                                                                  std::size_t, std::size_t);
};

// Explicit NEW preparation, not an upgrade of an earlier partially observed metadata receipt.
// Native metadata is captured before byte preparation and revalidated after it. Stage metadata is
// copied/verified before retirement except explicitly installation-dependent DACL publication.
// Query/copy failure never means absent metadata. Errors
// retain any created private paths in their description. No workspace namespace write occurs.
[[nodiscard]] Result<PreparedFileReplacement> prepareFileReplacement(std::string_view target,
                                                                     std::string_view privateStore,
                                                                     std::string_view after, std::size_t maxBeforeBytes,
                                                                     std::size_t maxAfterBytes);

// Reacquires native objects from an immutable private manifest. IDs, payload hashes and admitted
// metadata must match; reported phase is freshly observed, never trusted from disk. desiredBefore
// selects recovery direction only after the caller verified the durable decision. Foreign data is
// never overwritten. This does not fabricate a PreparedFileImages capability.
[[nodiscard]] Result<PreparedFileReplacement> recoverFileReplacement(const FileReplacementReceipt& receipt,
                                                                     std::string_view before, std::string_view after,
                                                                     bool desiredBefore, std::size_t maxBytes);

// Cooperating coordinators hold one live OS lock per private recovery root. Destruction releases
// the lock, never removes recovery records. No claim against hostile same-user directory rewrites.
class TransactionStoreLease {
public:
    ~TransactionStoreLease();
    TransactionStoreLease(TransactionStoreLease&&) noexcept;
    TransactionStoreLease& operator=(TransactionStoreLease&&) noexcept;

private:
    struct State;
    explicit TransactionStoreLease(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> m_state;
    friend Result<TransactionStoreLease> acquireTransactionStoreLease(std::string_view);
};
[[nodiscard]] Result<TransactionStoreLease> acquireTransactionStoreLease(std::string_view store);
// Atomic exclusive subdirectory creation, restricted at creation; existing names are refused.
[[nodiscard]] Result<std::string> claimTransactionDirectory(std::string_view store, std::string_view leaf);

// Creates/reuses a current-owner restricted transaction store, refusing unsafe existing objects.
// Prefer recoveryRoot when on target volume; otherwise use a per-user sibling of workspaceRoot.
// With no workspaceRoot the fallback is adjacent to target (caller must exclude it from project
// indexing/VCS). No privileges or existing unsafe directory ACL/mode are changed. O(path depth).
[[nodiscard]] Result<std::string> ensurePrivateTransactionStore(std::string_view target,
                                                                std::string_view preferredRecoveryRoot,
                                                                std::string_view workspaceRoot = {});

} // namespace frost::io
