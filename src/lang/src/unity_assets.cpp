// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unity_assets.hpp"
#include <algorithm>
#include <set>
#include <map>
#include "frost/lang/document_sync.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"
namespace frost::lang {
namespace {
std::string normalizedGuid(std::string_view value) {
    if (value.size() != 32) return {};
    std::string result(value);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        else if (!(c >= 'a' && c <= 'f') && !(c >= '0' && c <= '9')) return {};
    }
    return result;
}
bool yamlAsset(std::string_view path) {
    return path.ends_with(".unity") || path.ends_with(".prefab") || path.ends_with(".asset")
        || path.ends_with(".mat") || path.ends_with(".controller") || path.ends_with(".anim");
}
std::string_view trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}
bool consume(std::string_view& s, std::string_view token) {
    s = trim(s); if (!s.starts_with(token)) return false;
    s.remove_prefix(token.size()); return true;
}
std::optional<std::string> number(std::string_view& s) {
    s = trim(s); std::size_t end = s.starts_with('-') ? 1 : 0;
    const auto start = end;
    while (end < s.size() && s[end] >= '0' && s[end] <= '9') ++end;
    if (start == end || end > 20) return {};
    std::string result(s.substr(0, end)); s.remove_prefix(end); return result;
}
}
bool isUnityScriptAssociation(std::string_view path, std::string_view declaringType) {
    const auto slash = path.find_last_of("/\\");
    const auto name = slash == path.npos ? path : path.substr(slash + 1);
    return !declaringType.empty() && name.ends_with(".cs") && name.substr(0, name.size() - 3) == declaringType;
}
bool isUnityAssetsPath(std::string_view root, std::string_view path, io::PathStyle style) {
    const auto assets = documentKey(std::string(root) + "/Assets", style), key = documentKey(path, style);
    return key == assets || (key.size() > assets.size() && key.starts_with(assets) &&
        (key[assets.size()] == '/' || key[assets.size()] == '\\'));
}
void UnityAssetIndex::reset(std::span<const UnityAssetSnapshot> snapshots) {
    m_targets.clear(); m_usages.clear(); m_diagnostics.clear();
    std::size_t total = 0, files = 0;
    for (const auto& snapshot : snapshots) {
        if (++files > 4096 || snapshot.contents.size() > 16 * 1024 * 1024 ||
            snapshot.contents.size() > 64 * 1024 * 1024 - total) {
            m_diagnostics.push_back("Unity asset index incomplete: snapshot budget exceeded"); break;
        }
        total += snapshot.contents.size();
        const bool meta = snapshot.path.ends_with(".meta");
        if (!meta && !yamlAsset(snapshot.path)) continue;
        std::string_view contents(snapshot.contents);
        if (contents.starts_with("\xef\xbb\xbf")) contents.remove_prefix(3);
        if (!text::TextBuffer::fromUtf8(std::string(contents)) || contents.find('\0') != contents.npos || (!meta && !contents.starts_with("%YAML 1.1"))) {
            m_diagnostics.push_back(snapshot.path + ": binary or unsupported serialized encoding; index incomplete"); continue;
        }
        std::vector<std::string> guids;
        std::size_t begin = snapshot.contents.size() - contents.size();
        std::optional<std::size_t> scalarIndent;
        char quote = 0;
        while (begin < snapshot.contents.size()) {
            const auto end = snapshot.contents.find('\n', begin);
            const auto count = end == std::string::npos ? snapshot.contents.size() - begin : end - begin;
            const auto line = std::string_view(snapshot.contents).substr(begin, count);
            if (count > 16384) {
                m_diagnostics.push_back(snapshot.path + ": oversized YAML line; index incomplete"); break;
            }
            const auto indent = line.find_first_not_of(' ');
            const auto stripped = trim(line);
            if (scalarIndent && !stripped.empty() && indent <= *scalarIndent) scalarIndent.reset();
            const bool scalarBody = scalarIndent.has_value();
            if (meta && line.starts_with("guid:")) {
                auto value = trim(line.substr(5));
                const auto guid = normalizedGuid(value);
                if (!guid.empty()) guids.push_back(guid);
                else guids.emplace_back(); // malformed duplicate must not make a valid row look unique
            } else if (!meta && !scalarBody && !stripped.starts_with('#')) {
                auto value = stripped;
                if (value.starts_with("- ")) value = trim(value.substr(2));
                if (!value.starts_with('{')) {
                    const auto colon = value.find(':');
                    const auto key = value.substr(0, colon);
                    if (colon != value.npos && !key.empty() &&
                        key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-[]") == key.npos)
                        value = trim(value.substr(colon + 1));
                    else value = {};
                }
                if (!quote && (value.starts_with('|') || value.starts_with('>'))) scalarIndent = indent;
                if (!quote && value.starts_with('{')) {
                    auto remaining = value;
                    if (consume(remaining, "{fileID:")) {
                        const auto id = number(remaining);
                        if (id && consume(remaining, ",") && consume(remaining, "guid:")) {
                            remaining = trim(remaining);
                            const auto guid = normalizedGuid(remaining.substr(0, std::min<std::size_t>(32, remaining.size())));
                            const auto offset = static_cast<std::size_t>(remaining.data() - snapshot.contents.data());
                            if (!guid.empty()) {
                                remaining.remove_prefix(32);
                                if (consume(remaining, ",") && consume(remaining, "type:") && number(remaining) && consume(remaining, "}")) {
                                    remaining = trim(remaining);
                                    if ((remaining.empty() || remaining.starts_with('#')) && guid != std::string(32, '0'))
                                        m_usages.push_back({snapshot.path, guid, *id, offset});
                                }
                            }
                        }
                    }
                }
                // Track only scalar quotation, never quote characters inside plain values/comments.
                // A continued quoted scalar owns following apparent property lines until it closes.
                if (quote || value.starts_with('\'') || value.starts_with('"')) {
                    std::size_t i = 0;
                    if (!quote) { quote = value.front(); i = 1; }
                    else value = stripped;
                    for (; i < value.size(); ++i) {
                        if (quote == '"' && value[i] == '\\') { ++i; continue; }
                        if (value[i] != quote) continue;
                        if (quote == '\'' && i + 1 < value.size() && value[i + 1] == '\'') { ++i; continue; }
                        quote = 0; break;
                    }
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (meta) {
            if (guids.size() == 1 && !guids.front().empty() && guids.front() != std::string(32, '0'))
                m_targets.push_back({snapshot.path.substr(0, snapshot.path.size() - 5), snapshot.path, guids.front()});
            else m_diagnostics.push_back(snapshot.path + ": missing or ambiguous GUID");
        }
    }
}
std::optional<UnityAssetTarget> UnityAssetIndex::resolve(std::string_view guid) const {
    const auto key = normalizedGuid(guid);
    std::optional<UnityAssetTarget> result;
    for (const auto& target : m_targets) if (target.guid == key) {
        if (result) return std::nullopt;
        result = target;
    }
    return result;
}
std::vector<UnityAssetUsage> UnityAssetIndex::usages(std::string_view guid) const {
    const auto key = normalizedGuid(guid);
    std::vector<UnityAssetUsage> result;
    for (const auto& usage : m_usages) if (usage.guid == key) result.push_back(usage);
    return result;
}
std::optional<std::vector<UnityAssetMove>> planUnityAssetMove(
    std::span<const UnityAssetSnapshot> snapshots, std::string_view from, std::string_view to) {
    if (from.empty() || to.empty() || from == to || from.ends_with(".meta") || to.ends_with(".meta")) return std::nullopt;
    const std::string sidecar = std::string(from) + ".meta", targetMeta = std::string(to) + ".meta";
    const UnityAssetSnapshot* asset = nullptr; const UnityAssetSnapshot* meta = nullptr;
    for (const auto& file : snapshots) {
        if (file.path == to || file.path == targetMeta) return std::nullopt;
        if (file.path == from) { if (asset) return std::nullopt; asset = &file; }
        if (file.path == sidecar) { if (meta) return std::nullopt; meta = &file; }
    }
    if (!asset || !meta) return std::nullopt;
    UnityAssetIndex index; index.reset(std::span(meta, 1));
    if (!index.diagnostics().empty()) return std::nullopt;
    return std::vector<UnityAssetMove>{{asset->path, std::string(to), asset->contents}, {meta->path, targetMeta, meta->contents}};
}
std::optional<UnityAssetUsage> UnityAssetIndex::usageAt(std::string_view path, std::size_t offset) const {
    for (const auto& usage : m_usages)
        if (documentKey(usage.path) == documentKey(path) && offset >= usage.offset && offset <= usage.offset + 32) return usage;
    return {};
}
std::optional<UnityAssetTarget> UnityAssetIndex::asset(std::string_view path) const {
    for (const auto& target : m_targets)
        if (documentKey(target.path) == documentKey(path) || documentKey(target.metaPath) == documentKey(path))
            return resolve(target.guid);
    return {};
}
// Joins original document-local object IDs to their m_Script GUID. Caller supplies a Roslyn-
// validated field or unique public parameterless method. Never searches arbitrary scalar text.
std::vector<UnityAssetUsage> memberUsages(std::span<const UnityAssetSnapshot> snapshots, std::string_view guid,
    std::string_view name, std::string_view kind) {
    std::vector<UnityAssetUsage> result;
    for (const auto& snapshot : snapshots) {
        if (!yamlAsset(snapshot.path) || snapshot.contents.size() > 16 * 1024 * 1024) continue;
        struct Value { std::string object, name, text; std::size_t offset; };
        struct Call { std::string object, name; std::size_t offset; };
        std::vector<Value> values; std::vector<Call> calls;
        std::map<std::string, std::string> scripts;
        std::set<std::string> duplicateObjects;
        std::string object, target, method;
        std::size_t targetIndent = 0, methodOffset = 0;
        std::optional<std::size_t> scalarIndent, persistentIndent;
        char quote = 0;
        UnityAssetIndex fileIndex; fileIndex.reset(std::span(&snapshot, 1));
        if (!fileIndex.diagnostics().empty()) continue;
        std::size_t begin = 0;
        while (begin < snapshot.contents.size()) {
            const auto end = snapshot.contents.find('\n', begin);
            const auto line = std::string_view(snapshot.contents).substr(begin, end == std::string::npos ? end : end - begin);
            auto text = trim(line);
            const auto indent = line.find_first_not_of(' ');
            if (text.starts_with("--- !u!")) {
                object.clear(); target.clear(); method.clear(); persistentIndent.reset(); scalarIndent.reset(); quote = 0;
                const auto anchor = text.find(" &");
                if (anchor != text.npos) {
                    auto id = text.substr(anchor + 2); const auto parsed = number(id);
                    if (parsed && trim(id).empty()) {
                        object = *parsed;
                        if (scripts.contains(object)) duplicateObjects.insert(object);
                        else scripts.emplace(object, "");
                    }
                }
            } else if (!text.empty() && !text.starts_with('#') && !object.empty()) {
                if (scalarIndent && indent <= *scalarIndent) scalarIndent.reset();
                if (persistentIndent && indent <= *persistentIndent) { persistentIndent.reset(); target.clear(); method.clear(); }
                if (!scalarIndent) {
                    if (quote) {
                        for (std::size_t i = 0; i < text.size(); ++i) {
                            if (quote == '"' && text[i] == '\\') { ++i; continue; }
                            if (text[i] != quote) continue;
                            if (quote == '\'' && i + 1 < text.size() && text[i + 1] == '\'') { ++i; continue; }
                            quote = 0; break;
                        }
                    } else {
                        const bool sequence = text.starts_with("- ");
                        if (sequence) text = trim(text.substr(2));
                        const auto colon = text.find(':');
                        const auto key = text.substr(0, colon);
                        if (colon != text.npos && key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == key.npos) {
                            auto value = trim(text.substr(colon + 1));
                            const auto valueOffset = value.empty() ? begin : static_cast<std::size_t>(value.data() - snapshot.contents.data());
                            if (indent == 2 && key == name && kind == "field")
                                values.push_back({object, std::string(key), std::string(value), begin + static_cast<std::size_t>(key.data() - line.data())});
                            if (value.starts_with('|') || value.starts_with('>')) scalarIndent = indent;
                            else {
                                bool complete = true;
                                if (value.starts_with('\'') || value.starts_with('"')) {
                                    quote = value.front(); bool closed = false;
                                    for (std::size_t i = 1; i < value.size(); ++i) {
                                        if (quote == '"' && value[i] == '\\') { ++i; continue; }
                                        if (value[i] != quote) continue;
                                        if (quote == '\'' && i + 1 < value.size() && value[i + 1] == '\'') { ++i; continue; }
                                        closed = true; quote = 0; break;
                                    }
                                    complete = closed;
                                }
                                if (complete && key == "m_Script" && indent == 2) {
                                    for (const auto& usage : fileIndex.usages(guid))
                                        if (usage.offset >= begin && usage.offset < begin + line.size()) scripts[object] = usage.guid;
                                }
                                if (key == "m_PersistentCalls" && value.empty()) persistentIndent = indent;
                                if (persistentIndent) {
                                    if (key == "m_Target") {
                                        target.clear(); method.clear(); auto ref = value;
                                        if (consume(ref, "{fileID:")) {
                                            const auto id = number(ref);
                                            if (id && consume(ref, "}") && trim(ref).empty()) { target = *id; targetIndent = indent + (sequence ? 2 : 0); }
                                        }
                                    } else if (key == "m_MethodName" && indent == targetIndent && !target.empty() &&
                                        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == value.npos) {
                                        method = value; methodOffset = valueOffset;
                                    } else if (key == "m_Mode" && indent == targetIndent && value == "1" && !method.empty()) {
                                        calls.push_back({target, method, methodOffset}); target.clear(); method.clear();
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        auto matches = [&](const std::string& id) { const auto found = scripts.find(id); return found != scripts.end() && !duplicateObjects.contains(id) && found->second == guid; };
        for (const auto& value : values) if (matches(value.object)) result.push_back({snapshot.path, std::string(guid), value.object, value.offset});
        for (const auto& call : calls) if (kind == "eventMethod" && call.name == name && matches(call.object)) result.push_back({snapshot.path, std::string(guid), call.object, call.offset});
    }
    return result;
}
UnityAssetQuery queryUnityAssets(io::IFileSystem& fs, std::span<const std::string> projectFiles,
    std::span<const UnityAssetSnapshot> overlays, std::string_view path, std::size_t byteOffset, std::stop_token stop, std::string_view memberName, std::string_view memberKind) {
    UnityAssetQuery result;
    if (projectFiles.empty() || (!yamlAsset(path) && !path.ends_with(".cs") && !path.ends_with(".meta"))) return result;
    std::set<std::string> candidates;
    auto include = [&](const std::string& file) {
        if (yamlAsset(file) || file.ends_with(".meta")) candidates.insert(file);
        if (yamlAsset(file) || file.ends_with(".cs")) candidates.insert(file + ".meta");
    };
    for (const auto& file : projectFiles) {
        if (candidates.size() >= 8192) { result.diagnostics.push_back("Unity asset inventory exceeds limit; index incomplete"); break; }
        include(file);
    }
    include(std::string(path));
    std::vector<UnityAssetSnapshot> snapshots;
    std::size_t total = 0;
    for (const auto& candidate : candidates) {
        if (stop.stop_requested()) return {};
        if (snapshots.size() >= 4096 || total >= 64 * 1024 * 1024) {
            result.diagnostics.push_back("Unity asset index incomplete: snapshot budget exceeded"); break;
        }
        const auto open = std::ranges::find_if(overlays, [&](const auto& file) { return documentKey(file.path) == documentKey(candidate); });
        Result<std::string> bytes = open == overlays.end() ? fs.readFileSnapshot(candidate, std::min<std::size_t>(16 * 1024 * 1024, 64 * 1024 * 1024 - total)) : Result<std::string>(open->contents);
        if (!bytes) {
            if (bytes.error().code() != ErrorCode::NotFound) result.diagnostics.push_back(candidate + ": unreadable Unity input; index incomplete");
            continue;
        }
        if (bytes->size() > 16 * 1024 * 1024 || bytes->size() > 64 * 1024 * 1024 - total) {
            result.diagnostics.push_back(candidate + ": Unity snapshot budget exceeded; index incomplete"); continue;
        }
        total += bytes->size(); snapshots.push_back({candidate, std::move(*bytes)});
    }
    UnityAssetIndex index; index.reset(snapshots);
    result.diagnostics.insert(result.diagnostics.end(), index.diagnostics().begin(), index.diagnostics().end());
    auto target = index.asset(path);
    if (const auto usage = index.usageAt(path, byteOffset)) target = index.resolve(usage->guid);
    if (!target) return result;
    result.recognized = true;
    // GUID resolves to the original asset (script, scene, prefab, material), never a generated copy.
    result.declarations.push_back({target->path, {}, {{0, 0}, {0, 0}}});
    std::vector<Location> locations;
    const auto references = memberName.empty() ? index.usages(target->guid) : memberUsages(snapshots, target->guid, memberName, memberKind);
    for (const auto& usage : references) {
        const auto file = std::ranges::find_if(snapshots, [&](const auto& item) { return item.path == usage.path; });
        if (file == snapshots.end()) continue;
        const auto buffer = text::TextBuffer::fromUtf8(file->contents);
        if (!buffer) { result.diagnostics.push_back(file->path + ": invalid UTF-8; index incomplete"); continue; }
        const auto snapshot = buffer->current();
        const auto begin = toLspPosition(snapshot, ByteOffset{usage.offset}, PositionEncoding::Utf16);
        const auto end = toLspPosition(snapshot, ByteOffset{usage.offset + (memberName.empty() ? 32 : memberName.size())}, PositionEncoding::Utf16);
        locations.push_back({usage.path, {}, {begin, end}});
    }
    auto line = [&snapshots](std::string_view file, std::uint32_t row) -> std::optional<std::string> {
        const auto found = std::ranges::find_if(snapshots, [&](const auto& item) { return item.path == file; });
        if (found == snapshots.end()) return {};
        std::size_t begin = 0;
        for (std::uint32_t i = 0; i < row; ++i) {
            const auto end = found->contents.find('\n', begin); if (end == std::string::npos) return {}; begin = end + 1;
        }
        const auto end = found->contents.find('\n', begin);
        return found->contents.substr(begin, end == std::string::npos ? end : end - begin);
    };
    result.usages = groupUsages(locations, line, PositionEncoding::Utf16);
    return result;
}
} // namespace frost::lang
