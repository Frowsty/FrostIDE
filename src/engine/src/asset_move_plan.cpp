// SPDX-License-Identifier: Apache-2.0
// Fail-closed consumer of the actual engine assets.plan-move wire profile.
#include "frost/engine/asset_move_plan.hpp"

#include "frost/lang/uri.hpp"

#include <algorithm>
#include <charconv>
#include <set>
namespace frost::engine {
namespace {
bool safe(std::string_view p) {
    if (p.empty() || p.front() == '/' || p.find_first_of("\\:\r\n") != p.npos)
        return false;
    for (std::size_t at = 0; at < p.size();) {
        auto end = p.find('/', at);
        auto part = p.substr(at, end == p.npos ? p.size() - at : end - at);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        for (char byte : part) {
            const auto c = static_cast<unsigned char>(byte);
            if (c < 32 || c == 127)
                return false;
        }
        if (end == p.npos)
            return true;
        at = end + 1;
        if (at == p.size())
            return false;
    }
    return false;
}
bool decimal(std::string_view p) {
    std::uint64_t value{};
    auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
    return !p.empty() && ec == std::errc{} && end == p.data() + p.size() && std::to_string(value) == p;
}
} // namespace
std::string assetMoveHash(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return std::to_string(hash);
}
Result<AssetMovePlan> parseAssetMovePlan(const rpc::Json& r, std::string_view source, std::string_view target,
                                         EngineHandle handle) {
    auto bad = [] { return fail(ErrorCode::Corrupt, "Invalid engine asset move receipt"); };
    const auto* version = r.find("version");
    if (!version || version->kind() != rpc::Json::Kind::Integer || r.integerAt("version") != 1 ||
        r.stringAt("oldPath") != source || r.stringAt("path") != target ||
        engineHandleAt(r, "handle") != std::optional(handle) || !handle.value || !source.starts_with("Assets/") ||
        !target.starts_with("Assets/") || !safe(source) || !safe(target) ||
        lang::documentKey(source) == lang::documentKey(target))
        return bad();
    AssetMovePlan out;
    out.lease = r.stringAt("lease");
    out.receipt = r.stringAt("receipt");
    out.oldPath = source;
    out.path = target;
    out.handle = handle;
    if (out.lease.empty() || out.lease.size() > 256 || out.receipt.empty() || out.receipt.size() > 256)
        return bad();
    const auto* moves = r.find("moves");
    const auto* files = r.find("files");
    const auto* guards = r.find("guards");
    if (!moves || !moves->isArray() || moves->size() != 2 || !files || !files->isArray() || files->size() > 4094 ||
        !guards || !guards->isArray() || guards->size() > 32768)
        return bad();
    std::set<std::string> participants, guarded;
    for (const auto& m : moves->items()) {
        AssetMove move{std::string(m.stringAt("from")), std::string(m.stringAt("to"))};
        const bool primary = move.from == source && move.to == target;
        const bool sidecar = move.from == std::string(source) + ".fmeta" && move.to == std::string(target) + ".fmeta";
        if ((!primary && !sidecar) || !participants.insert(lang::documentKey(move.from)).second ||
            !participants.insert(lang::documentKey(move.to)).second)
            return bad();
        out.moves.push_back(std::move(move));
    }
    std::size_t total = 0;
    for (const auto& f : files->items()) {
        auto* before = f.find("before");
        auto* after = f.find("after");
        AssetMoveEdit edit{std::string(f.stringAt("path")), {}, {}};
        if (!safe(edit.path) || !before || !before->isString() || !after || !after->isString())
            return bad();
        if (!participants.insert(lang::documentKey(edit.path)).second)
            return fail(ErrorCode::Unsupported,
                        "Engine move overlaps a file rewrite; this native transaction cannot preserve both");
        edit.before = *before->asString();
        edit.after = *after->asString();
        total += edit.before.size() + edit.after.size();
        if (edit.before.size() > 64 * 1024 * 1024 || edit.after.size() > 64 * 1024 * 1024 ||
            total > 256 * 1024 * 1024 || edit.before == edit.after)
            return bad();
        out.files.push_back(std::move(edit));
    }
    for (const auto& g : guards->items()) {
        auto* size = g.find("size");
        AssetMoveGuard guard{std::string(g.stringAt("path")), std::string(g.stringAt("hash")), 0};
        if (!safe(guard.path) || !decimal(guard.hash) || !size || size->kind() != rpc::Json::Kind::Integer ||
            !size->asUnsigned() || *size->asUnsigned() > 64 * 1024 * 1024 ||
            !guarded.insert(lang::documentKey(guard.path)).second)
            return bad();
        guard.size = *size->asUnsigned();
        out.guards.push_back(std::move(guard));
    }
    for (const auto& m : out.moves)
        if (!guarded.contains(lang::documentKey(m.from)))
            return bad();
    for (const auto& f : out.files)
        if (!guarded.contains(lang::documentKey(f.path)))
            return bad();
    return out;
}
} // namespace frost::engine
