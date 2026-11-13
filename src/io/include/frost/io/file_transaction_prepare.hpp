// SPDX-License-Identifier: Apache-2.0
// Native, bounded Before capture and private After byte staging for REF-1 preparation.
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace frost::io {

enum class FileImageCheck : unsigned char { Matches, TargetChanged, StageChanged };
enum class FileImageDiscard : unsigned char { Removed, AlreadyAbsent, CleanupPending };
// Actual native observations, not a portable wire schema or permission to mutate by identity/path.
struct FileImageObservation {
    std::string volume;
    std::string object;
    std::string metadata;
    std::uint64_t size = 0;
    friend bool operator==(const FileImageObservation&, const FileImageObservation&) = default;
};
struct FileImageReceipt {
    std::string targetPath;
    std::string stagePath;
    std::string beforeSha256;
    std::string afterSha256;
    std::string metadataObservationProfile;
    FileImageObservation target;
    FileImageObservation stage;
    FileImageObservation targetParent;
    FileImageObservation store;
};

/*
 * Move-only ownership of captured bytes and native target/store/stage handles, NOT an Apply-ready
 * replacement or transaction decision. No workspace namespace write, commit or Undo API exists here.
 * Receipt copies survive destruction; paths/hashes are evidence, never deletion authority.
 *
 * The caller owns and exclusively manages the private store (outside the workspace), on the target's
 * volume. POSIX requires current-user ownership and mode0700. Windows uses the store's ACL, which the
 * caller must restrict to its recovery owner; handles retain the observed store object. No
 * universal exclusion of hostile equivalent-rights processes or ancestor relocation is promised.
 * Methods on one instance must be serialized. Distinct instances may be used concurrently.
 *
 * Destructor/move assignment close handles ONLY; forgotten discard retains the private artifact.
 * Keep receipt() before abandonment for recovery. No recursive deletion or original-file cleanup.
 * Discard checks fresh stage ownership; POSIX unlinkat additionally relies on the private-store
 * cooperative ownership contract (there is no inode-conditional unlink). Foreign occupants remain
 * untouched when observed. This is not an arbitrary hostile same-user namespace exclusion promise.
 */
class PreparedFileImages {
public:
    ~PreparedFileImages();
    PreparedFileImages(PreparedFileImages&&) noexcept;
    PreparedFileImages& operator=(PreparedFileImages&&) noexcept;
    PreparedFileImages(const PreparedFileImages&) = delete;
    PreparedFileImages& operator=(const PreparedFileImages&) = delete;

    [[nodiscard]] const FileImageReceipt* receipt() const noexcept;
    [[nodiscard]] std::string_view beforeBytes() const noexcept;
    [[nodiscard]] std::string_view afterBytes() const noexcept;
    // O(Before+After), bounded by preparation limits. A successful observation is not a future
    // mutation permit. Compares target/stage IDs, bytes and the metadata categories named below;
    // directories use object IDs plus POSIX private-store ownership/mode, not their changing mtimes.
    // NotFound/name replacement is a Changed result; other OS failures remain errors.
    [[nodiscard]] Result<FileImageCheck> revalidate() const;
    // O(After). A successful discard is idempotent. Unknown ownership/errors retain the receipt.
    [[nodiscard]] Result<FileImageDiscard> discard();

private:
    struct State;
    explicit PreparedFileImages(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> m_state;
    friend Result<PreparedFileImages> prepareFileImages(std::string_view, std::string_view, std::string_view,
                                                        std::size_t, std::size_t);
};

/*
 * Worker-side real IO: bounded regular single-link Before capture; exclusive private After file;
 * flush and verify actual After bytes/identity. Both limits are checked before owning payload copies;
 * reads use fixed-size chunks and refuse growth beyond the limit. Empty files are supported.
 * O(Before+After) time/memory. Caller keeps path/store arguments alive only for the call.
 *
 * First byte-preparation profile: final symlink/reparse and nonregular/multilink targets refuse;
 * Windows also refuses the existing beta save profile's EFS/object-ID cases. Same-volume store is
 * required. No target permission or metadata setter runs (ordinary reads can update access time).
 * Observed: Windows volume/file ID, link count, attributes, creation/write/change times;
 * POSIX device/inode, mode, uid/gid, link count, mtime/ctime. Unobserved: ACL/security descriptors,
 * xattrs/EAs, alternate streams, short names and other platform metadata. Revalidation does NOT
 * claim those categories unchanged. Stage has private-store permissions, not target metadata.
 * Future Apply must acquire authority and copy/revalidate its COMPLETE admitted metadata profile;
 * this deliberately separate type cannot stand in for that capability.
 *
 * Errors: InvalidArgument (paths/private store), Unsupported (named profile/volume), OutOfRange
 * (payload limit), and native IO errors. Stage failures never change the target; any partial private
 * artifact is named in the error and retained, rather than silently deleted without ownership proof.
 */
[[nodiscard]] Result<PreparedFileImages> prepareFileImages(std::string_view target, std::string_view privateStore,
                                                           std::string_view after, std::size_t maxBeforeBytes,
                                                           std::size_t maxAfterBytes);

} // namespace frost::io
