// SPDX-License-Identifier: Apache-2.0
// Capture immutable source bytes that match the verifier's manifest.
#include "managed_source_snapshot.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"

#include <QByteArray>
#include <QCryptographicHash>

namespace frost::app {
namespace {
constexpr std::uint64_t kMaxBytes = 256ULL * 1024 * 1024;
constexpr std::uint64_t kMaxSourceBytes = 4ULL * 1024 * 1024;
constexpr std::uint64_t kMaxSourceTotal = 16ULL * 1024 * 1024;

Result<std::string> capture(io::IFileSystem& fs, const project::TrustStore& trust, std::string_view path,
                            std::string_view expected, std::string_view rootKey, std::uint64_t limit) {
    auto canonical = fs.canonicalPath(path);
    auto normalized = io::normalizePath(path);
    const auto canonicalKey = canonical ? *canonical : std::string();
    if (!canonical || !normalized || !managedPathHasNoLinks(fs, path) || canonicalKey != *normalized ||
        !trust.isTrusted(*canonical) || !canonicalKey.starts_with(rootKey) || canonicalKey.size() <= rootKey.size() ||
        (rootKey.back() != '/' && rootKey.back() != '\\' && canonicalKey[rootKey.size()] != '/' &&
         canonicalKey[rootKey.size()] != '\\'))
        return fail(ErrorCode::Untrusted, "managed input is untrusted or uses a filesystem alias");
    auto status = fs.status(path);
    if (!status || status->type != io::FileType::Regular || status->size > limit)
        return fail(ErrorCode::IoError, "managed input is missing or too large");
    auto bytes = fs.readFile(path);
    if (!bytes || bytes->size() > limit)
        return fail(ErrorCode::IoError, "managed input could not be read within the size limit");
    const auto digest =
        QCryptographicHash::hash(QByteArray::fromRawData(bytes->data(), static_cast<qsizetype>(bytes->size())),
                                 QCryptographicHash::Sha256)
            .toHex();
    if (std::string_view(digest.constData(), static_cast<std::size_t>(digest.size())) != expected)
        return fail(ErrorCode::Corrupt, "managed input changed after symbol verification");
    return bytes;
}
} // namespace

bool managedPathHasNoLinks(io::IFileSystem& fs, std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized)
        return false;
    std::string current = *normalized;
    while (!io::parentPath(current).empty()) {
        const std::string parent(io::parentPath(current));
        auto entries = fs.listDirectory(parent);
        if (!entries)
            return false;
        bool found = false;
        std::size_t matches = 0;
        for (const auto& entry : *entries) {
            auto child = io::joinPath(parent, entry.name);
            if (child && lang::documentKey(*child) == lang::documentKey(current)) {
                ++matches;
                if (*child != current)
                    continue;
                if (entry.type == io::FileType::Symlink)
                    return false;
                found = true;
            }
        }
        if (!found || matches != 1)
            return false;
        current = parent;
    }
    return true;
}

Result<ManagedSourceSnapshot> captureManagedSources(io::IFileSystem& fs, const project::TrustStore& trust,
                                                    std::string_view root, const PreparedManagedDebug& prepared,
                                                    const std::function<bool()>& cancelled) {
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed source capture was cancelled");
    auto canonical = fs.canonicalPath(root);
    auto normalized = io::normalizePath(root);
    if (!canonical || !normalized || !managedPathHasNoLinks(fs, root) || *canonical != *normalized ||
        !trust.isTrusted(*canonical))
        return fail(ErrorCode::Untrusted, "managed workspace is untrusted or uses a filesystem alias");
    if (!std::string_view(prepared.target.assembly).ends_with(".dll"))
        return fail(ErrorCode::InvalidArgument, "managed target is not a DLL");
    const auto rootKey = *canonical;
    auto assembly = capture(fs, trust, prepared.target.assembly, prepared.assemblySha256, rootKey, kMaxBytes);
    if (!assembly)
        return std::unexpected(std::move(assembly.error()));
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed source capture was cancelled");
    const std::string pdb = prepared.target.assembly.substr(0, prepared.target.assembly.size() - 4) + ".pdb";
    auto symbols = capture(fs, trust, pdb, prepared.pdbSha256, rootKey, kMaxBytes);
    if (!symbols)
        return std::unexpected(std::move(symbols.error()));
    ManagedSourceSnapshot result{.moduleId = prepared.moduleId, .sources = {}};
    std::uint64_t total = 0;
    for (const auto& source : prepared.sources) {
        if (cancelled && cancelled())
            return fail(ErrorCode::Cancelled, "managed source capture was cancelled");
        auto bytes = capture(fs, trust, source.path, source.sha256, rootKey, kMaxSourceBytes);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        if (bytes->size() > kMaxSourceTotal - total)
            return fail(ErrorCode::OutOfRange, "verified source documents are too large");
        total += bytes->size();
        if (!result.sources.emplace(source.path, std::move(*bytes)).second)
            return fail(ErrorCode::Corrupt, "duplicate verified source document");
    }
    return result;
}

} // namespace frost::app
