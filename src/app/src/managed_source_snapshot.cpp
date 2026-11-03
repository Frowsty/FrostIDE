// SPDX-License-Identifier: Apache-2.0
// Capture immutable source bytes that match the verifier's manifest.
#include "managed_source_snapshot.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/json.hpp"

#include <QByteArray>
#include <QCryptographicHash>

#include <limits>
#include <set>

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

static Result<ManagedSourceSnapshot> captureSources(io::IFileSystem& fs, const project::TrustStore& trust,
                                                    std::string_view root, const PreparedManagedDebug& prepared,
                                                    const std::function<bool()>& cancelled, std::uint64_t moduleLimit) {
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
    auto assembly = capture(fs, trust, prepared.target.assembly, prepared.assemblySha256, rootKey, moduleLimit);
    if (!assembly)
        return std::unexpected(std::move(assembly.error()));
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed source capture was cancelled");
    const std::string pdb = prepared.target.assembly.substr(0, prepared.target.assembly.size() - 4) + ".pdb";
    auto symbols = capture(fs, trust, pdb, prepared.pdbSha256, rootKey, moduleLimit);
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

Result<ManagedSourceSnapshot> captureManagedSources(io::IFileSystem& fs, const project::TrustStore& trust,
                                                    std::string_view root, const PreparedManagedDebug& prepared,
                                                    const std::function<bool()>& cancelled) {
    return captureSources(fs, trust, root, prepared, cancelled, kMaxBytes);
}

Result<ManagedModuleSnapshot> captureManagedModule(io::IFileSystem& fs, const project::TrustStore& trust,
                                                   std::string_view root, const PreparedManagedDebug& prepared,
                                                   const std::function<bool()>& cancelled) {
    constexpr std::uint64_t maxBlob = 1024 * 1024 - 15;
    auto source = captureSources(fs, trust, root, prepared, cancelled, maxBlob);
    if (!source)
        return std::unexpected(std::move(source.error()));
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed module capture was cancelled");
    auto canonical = fs.canonicalPath(root);
    auto normalized = io::normalizePath(root);
    if (!canonical || !normalized || *canonical != *normalized || !managedPathHasNoLinks(fs, root) ||
        !trust.isTrusted(*canonical))
        return fail(ErrorCode::Untrusted, "managed workspace is no longer trusted or uses an alias");
    auto image = capture(fs, trust, prepared.target.assembly, prepared.assemblySha256, *canonical, maxBlob);
    if (!image)
        return std::unexpected(std::move(image.error()));
    if (image->empty())
        return fail(ErrorCode::Corrupt, "verified managed assembly is empty");
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed module capture was cancelled");
    const auto pdb = prepared.target.assembly.substr(0, prepared.target.assembly.size() - 4) + ".pdb";
    auto symbols = capture(fs, trust, pdb, prepared.pdbSha256, *canonical, maxBlob);
    if (!symbols)
        return std::unexpected(std::move(symbols.error()));
    if (symbols->empty())
        return fail(ErrorCode::Corrupt, "verified managed symbols are empty");
    if (cancelled && cancelled())
        return fail(ErrorCode::Cancelled, "managed module capture was cancelled");
    if (!trust.isTrusted(*canonical))
        return fail(ErrorCode::Untrusted, "managed workspace is no longer trusted");
    return ManagedModuleSnapshot{std::move(*source), std::move(*image), std::move(*symbols)};
}

namespace {
void appendMonoFrame(std::string& out, std::string_view bytes) {
    const auto length = static_cast<std::uint32_t>(bytes.size());
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((length >> shift) & 0xffU));
    out.append(bytes);
}
} // namespace

Result<std::string> monoDocumentsInput(std::string_view assembly, std::string_view pdb) {
    constexpr std::size_t maxBlob = 1024 * 1024 - 15;
    if (assembly.empty() || pdb.empty() || assembly.size() > maxBlob || pdb.size() > maxBlob)
        return fail(ErrorCode::OutOfRange, "Mono module snapshot exceeds the packet limit");
    std::string result;
    result.reserve(8 + assembly.size() + pdb.size());
    appendMonoFrame(result, assembly);
    appendMonoFrame(result, pdb);
    return result;
}
Result<std::string> monoMapInput(const ManagedModuleSnapshot& snapshot, const std::vector<std::string>& documentOrder) {
    auto result = monoDocumentsInput(snapshot.assemblyBytes, snapshot.pdbBytes);
    if (!result)
        return std::unexpected(std::move(result.error()));
    if (documentOrder.empty() || documentOrder.size() > 16384 || documentOrder.size() != snapshot.source.sources.size())
        return fail(ErrorCode::Corrupt, "Mono document order is incomplete");
    std::set<std::string> seen;
    std::size_t total = 0;
    for (const auto& path : documentOrder) {
        const auto found = snapshot.source.sources.find(path);
        if (path.size() > 4096 || found == snapshot.source.sources.end() || !seen.insert(path).second ||
            found->second.size() > kMaxSourceBytes || found->second.size() > kMaxSourceTotal - total)
            return fail(ErrorCode::Corrupt, "Mono source bytes or document order are invalid");
        total += found->second.size();
        appendMonoFrame(*result, found->second);
    }
    return result;
}

Result<MonoSourceMap> parseMonoSourceMap(std::string_view text, const PreparedManagedDebug& prepared,
                                         const ManagedModuleSnapshot& snapshot) {
    if (text.size() > 8 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Mono source map exceeds the output limit");
    auto digest = [](std::string_view bytes) {
        return QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                        QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    };
    if (snapshot.assemblyBytes.empty() || snapshot.pdbBytes.empty() ||
        snapshot.assemblyBytes.size() > 1024 * 1024 - 15 || snapshot.pdbBytes.size() > 1024 * 1024 - 15)
        return fail(ErrorCode::OutOfRange, "Mono module snapshot exceeds the packet limit");
    if (digest(snapshot.assemblyBytes) != prepared.assemblySha256 || digest(snapshot.pdbBytes) != prepared.pdbSha256)
        return fail(ErrorCode::Corrupt, "Mono source map module snapshot changed");
    auto json = rpc::parseJson(text);
    if (!json || !json->isObject() || json->stringAt("moduleId") != prepared.moduleId ||
        snapshot.source.moduleId != prepared.moduleId || json->stringAt("assemblySha256") != prepared.assemblySha256 ||
        json->stringAt("pdbSha256") != prepared.pdbSha256)
        return fail(ErrorCode::Corrupt, "Mono source map names a different module");
    const auto* sources = json->find("sources");
    const auto* points = json->find("sequencePoints");
    if (!sources || !sources->isArray() || sources->size() == 0 || sources->size() != prepared.sources.size() ||
        sources->size() > 16384 || !points || !points->isArray() || points->size() > 100000 ||
        snapshot.source.sources.size() != prepared.sources.size())
        return fail(ErrorCode::Corrupt, "invalid Mono source map lists");
    std::vector<std::string> paths;
    paths.reserve(sources->size());
    std::uint64_t totalBytes = 0;
    for (std::size_t i = 0; i < sources->size(); ++i) {
        const auto& source = sources->items()[i];
        if (!source.isObject() || source.stringAt("path") != prepared.sources[i].path ||
            source.stringAt("sha256") != prepared.sources[i].sha256 ||
            !snapshot.source.sources.contains(prepared.sources[i].path))
            return fail(ErrorCode::Corrupt, "Mono source map source is not verified");
        const auto& bytes = snapshot.source.sources.at(prepared.sources[i].path);
        if (prepared.sources[i].path.size() > 4096 || bytes.size() > kMaxSourceBytes ||
            bytes.size() > kMaxSourceTotal - totalBytes || digest(bytes) != prepared.sources[i].sha256)
            return fail(ErrorCode::Corrupt, "Mono source map snapshot changed");
        totalBytes += bytes.size();
        paths.push_back(prepared.sources[i].path);
    }
    std::vector<MonoSourcePoint> result;
    result.reserve(points->size());
    for (const auto& point : points->items()) {
        const auto token = point.integerAt("methodToken");
        const auto index = point.integerAt("sourceIndex");
        const auto line = point.integerAt("line");
        const auto offset = point.integerAt("ilOffset");
        if (!point.isObject() || !token || *token < 0x06000001 || *token > 0x06ffffff || !index || *index < 0 ||
            static_cast<std::uint64_t>(*index) >= paths.size() || !line || *line <= 0 ||
            *line > std::numeric_limits<std::int32_t>::max() || !offset || *offset < 0 ||
            *offset > std::numeric_limits<std::int32_t>::max())
            return fail(ErrorCode::Corrupt, "invalid Mono source map location");
        result.push_back({static_cast<std::uint32_t>(*token), static_cast<std::uint32_t>(*index),
                          static_cast<std::int32_t>(*line), static_cast<std::int32_t>(*offset)});
    }
    return MonoSourceMap{std::move(paths), std::move(result)};
}

} // namespace frost::app
