// SPDX-License-Identifier: Apache-2.0
// Strict authenticated manifest reader; unsigned discovery data never enters policy.
#include <frost/update/manifest.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <string_view>

#include <yyjson.h>

namespace frost::update {
namespace {
struct Deleter {
    void operator()(yyjson_doc* doc) const noexcept { yyjson_doc_free(doc); }
};
using Document = std::unique_ptr<yyjson_doc, Deleter>;
std::string_view stringOf(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}
bool fields(yyjson_val* object, std::initializer_list<std::string_view> names) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != names.size())
        return false;
    std::set<std::string_view> seen;
    yyjson_obj_iter it;
    yyjson_obj_iter_init(object, &it);
    while (auto* key = yyjson_obj_iter_next(&it)) {
        auto name = stringOf(key);
        if (std::find(names.begin(), names.end(), name) == names.end() || !seen.insert(name).second)
            return false;
    }
    return true;
}
bool unsignedInteger(yyjson_val* v, std::uint64_t& out) {
    if (!yyjson_is_uint(v))
        return false;
    out = yyjson_get_uint(v);
    return true;
}
bool semver(std::string_view v) {
    // SemVer core (3 or 4 numeric parts) plus optional prerelease/build identifiers,
    // including the no-leading-zero rule.
    std::size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        const auto begin = pos;
        while (pos < v.size() && v[pos] >= '0' && v[pos] <= '9')
            ++pos;
        if (begin == pos || (pos - begin > 1 && v[begin] == '0'))
            return false;
        if (part < 2) {
            if (pos == v.size() || v[pos] != '.')
                return false;
            ++pos;
        } else if (part == 2) {
            if (pos < v.size() && v[pos] == '.') {
                ++pos;
            } else {
                break;
            }
        }
    }
    for (char marker : {'-', '+'}) {
        if (pos < v.size() && v[pos] == marker) {
            ++pos;
            do {
                const auto begin = pos;
                bool numeric = true;
                while (pos < v.size() && ((v[pos] >= '0' && v[pos] <= '9') || (v[pos] >= 'A' && v[pos] <= 'Z') ||
                                          (v[pos] >= 'a' && v[pos] <= 'z') || v[pos] == '-')) {
                    numeric = numeric && v[pos] >= '0' && v[pos] <= '9';
                    ++pos;
                }
                if (begin == pos || (marker == '-' && numeric && pos - begin > 1 && v[begin] == '0'))
                    return false;
                if (pos == v.size() || v[pos] != '.')
                    break;
                ++pos;
            } while (true);
        }
    }
    return pos == v.size() && v.size() <= 128;
}
// Both versions must already pass semver(). Compare numeric identifiers by
// length and lexically, avoiding arithmetic overflow for arbitrary input.
int compareNumeric(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return a == b ? 0 : (a < b ? -1 : 1);
}
int compareVersion(std::string_view a, std::string_view b) {
    auto releasePart = [](std::string_view& v) {
        auto end = v.find_first_of("-+");
        auto core = v.substr(0, end);
        v = end == std::string_view::npos ? std::string_view{} : v.substr(end);
        return core;
    };
    auto ac = releasePart(a), bc = releasePart(b);
    for (int i = 0; i < 4; ++i) {
        auto ad = ac.find('.'), bd = bc.find('.');
        auto av = ac.empty() ? std::string_view("0") : ac.substr(0, ad);
        auto bv = bc.empty() ? std::string_view("0") : bc.substr(0, bd);
        if (int c = compareNumeric(av, bv); c != 0)
            return c;
        ac = ad == std::string_view::npos ? std::string_view{} : ac.substr(ad + 1);
        bc = bd == std::string_view::npos ? std::string_view{} : bc.substr(bd + 1);
    }
    bool ap = a.starts_with('-'), bp = b.starts_with('-');
    if (ap != bp)
        return ap ? -1 : 1;
    if (!ap)
        return 0;
    a.remove_prefix(1);
    b.remove_prefix(1);
    a = a.substr(0, a.find('+'));
    b = b.substr(0, b.find('+'));
    auto numeric = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    while (true) {
        auto ad = a.find('.'), bd = b.find('.');
        auto av = a.substr(0, ad), bv = b.substr(0, bd);
        bool an = numeric(av), bn = numeric(bv);
        int c = an != bn ? (an ? -1 : 1) : an ? compareNumeric(av, bv) : (av == bv ? 0 : (av < bv ? -1 : 1));
        if (c != 0)
            return c;
        if (ad == std::string_view::npos || bd == std::string_view::npos)
            return ad == bd ? 0 : (ad == std::string_view::npos ? -1 : 1);
        a.remove_prefix(ad + 1);
        b.remove_prefix(bd + 1);
    }
}
bool digestFromHex(std::string_view hex, Sha256& out) {
    if (hex.size() != out.size() * 2)
        return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}
bool assetUrl(std::string_view url) {
    constexpr std::string_view prefix = "https://github.com/Frowsty/FrostIDE/releases/download/";
    if (!url.starts_with(prefix) || url.size() > 2048)
        return false;
    auto rest = url.substr(prefix.size());
    // Restrict to simple tag/asset segments: no percent escapes, query, fragment or traversal.
    auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == rest.size() ||
        rest.find('/', slash + 1) != std::string_view::npos)
        return false;
    for (auto segment : {rest.substr(0, slash), rest.substr(slash + 1)}) {
        if (segment == "." || segment == "..")
            return false;
        for (char c : segment)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == '_'))
                return false;
    }
    return true;
}
} // namespace
Result<ReleaseFloor> parseReleaseFloor(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > 1024)
        return fail(ErrorCode::Corrupt, "Invalid saved update state size");
    Document doc(yyjson_read(bytes.data(), bytes.size(), 0));
    auto* root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    std::uint64_t schema{};
    ReleaseFloor floor;
    Sha256 digest{};
    if (!fields(root, {"schema", "version", "build", "sha256"}) ||
        !unsignedInteger(yyjson_obj_get(root, "schema"), schema) || schema != 1 ||
        !unsignedInteger(yyjson_obj_get(root, "build"), floor.build) || floor.build == 0)
        return fail(ErrorCode::Corrupt, "Invalid saved update state");
    floor.version = stringOf(yyjson_obj_get(root, "version"));
    floor.digest = stringOf(yyjson_obj_get(root, "sha256"));
    if (!semver(floor.version) || !digestFromHex(floor.digest, digest))
        return fail(ErrorCode::Corrupt, "Invalid saved update identity");
    return floor;
}
Result<std::string> encodeReleaseFloor(const ReleaseFloor& floor) {
    Sha256 digest{};
    if (floor.build == 0 || !semver(floor.version) || !digestFromHex(floor.digest, digest))
        return fail(ErrorCode::InvalidArgument, "Invalid update identity to save");
    // SemVer validation restricts strings to ASCII characters requiring no JSON escaping.
    return "{\"schema\":1,\"version\":\"" + floor.version + "\",\"build\":" + std::to_string(floor.build) +
           ",\"sha256\":\"" + floor.digest + "\"}";
}
Result<std::optional<Release>> eligibleRelease(std::span<const std::byte> manifest, const Signature& signature,
                                               const PublicKey& key, const ReleasePolicy& policy) {
    if (auto verified = verifyManifest(manifest, signature, key); !verified)
        return std::unexpected(verified.error());
    Document doc(yyjson_read(reinterpret_cast<const char*>(manifest.data()), manifest.size(), 0));
    auto* root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    if (!fields(root, {"schema", "channel", "platform", "arch", "version", "build", "expires", "asset"}))
        return fail(ErrorCode::Corrupt, "Malformed release manifest");
    auto* asset = yyjson_obj_get(root, "asset");
    std::uint64_t schema{}, expires{};
    Release result;
    if (!fields(asset, {"url", "size", "sha256"}) || !unsignedInteger(yyjson_obj_get(root, "schema"), schema) ||
        schema != 1 || !unsignedInteger(yyjson_obj_get(root, "expires"), expires) || expires <= policy.nowUnix ||
        !unsignedInteger(yyjson_obj_get(root, "build"), result.build) || result.build == 0 ||
        !unsignedInteger(yyjson_obj_get(asset, "size"), result.size) || result.size == 0 ||
        result.size > 2ULL * 1024 * 1024 * 1024)
        return fail(ErrorCode::Corrupt, "Release manifest policy rejected");
    auto channel = stringOf(yyjson_obj_get(root, "channel"));
    auto platform = stringOf(yyjson_obj_get(root, "platform"));
    auto arch = stringOf(yyjson_obj_get(root, "arch"));
    auto version = stringOf(yyjson_obj_get(root, "version"));
    auto url = stringOf(yyjson_obj_get(asset, "url"));
    if ((channel != "stable" && channel != "eap" && channel != "nightly") || channel != policy.channel ||
        platform != policy.platform || arch != policy.architecture || !semver(version) ||
        !semver(policy.installedVersion) || (!policy.acceptedVersion.empty() && !semver(policy.acceptedVersion)) ||
        (channel == "stable" && version.substr(0, version.find('+')).find('-') != std::string_view::npos) ||
        !assetUrl(url) || !digestFromHex(stringOf(yyjson_obj_get(asset, "sha256")), result.digest))
        return fail(ErrorCode::Corrupt, "Release manifest fields rejected");
    if (result.build == policy.acceptedBuild && policy.acceptedBuild != 0) {
        constexpr char digits[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(result.digest.size() * 2);
        for (auto byte : result.digest) {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 15]);
        }
        if (version != policy.acceptedVersion || encoded != policy.acceptedDigest)
            return fail(ErrorCode::Corrupt, "Accepted release identity differs");
    }
    result.channel = channel;
    result.version = version;
    result.url = url;
    if (result.build <= policy.installedBuild || result.build < policy.acceptedBuild ||
        compareVersion(version, policy.installedVersion) < 0 ||
        (!policy.acceptedVersion.empty() && compareVersion(version, policy.acceptedVersion) < 0))
        return std::optional<Release>{};
    return std::optional<Release>(std::move(result));
}
Result<Release> verifyRelease(std::span<const std::byte> manifest, const Signature& signature, const PublicKey& key,
                              const ReleasePolicy& policy) {
    auto candidate = eligibleRelease(manifest, signature, key, policy);
    if (!candidate)
        return std::unexpected(candidate.error());
    if (!*candidate)
        return fail(ErrorCode::Corrupt, "Release is not newer than the installed or accepted floor");
    return std::move(**candidate);
}
} // namespace frost::update
