// SPDX-License-Identifier: Apache-2.0
//
// The theme blob v1 encoder, decoder and file loaders (frost/ui/model/theme_blob.hpp, which
// documents the layout).

#include "frost/ui/model/theme_blob.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/model/fnv1a.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace frost::ui {

static_assert(std::endian::native == std::endian::little,
              "the theme blob is little-endian and read with memcpy; a big-endian port needs byte swaps");

namespace {

constexpr std::string_view kMagic = "FROSTTHM";
constexpr std::size_t kChecksumOffset = 40;
constexpr std::size_t kChecksumEnd = 48;
constexpr std::size_t kColorsOffset = kThemeBlobHeaderSize;
constexpr std::size_t kMetricsOffset = kColorsOffset + kColorKeyCount * 4;
constexpr std::size_t kRolesOffset = kMetricsOffset + kMetricKeyCount * 8;
constexpr std::size_t kStringsOffset = kRolesOffset + kIconRoleCount * 4;
constexpr std::uint16_t kFlagDark = 1;
constexpr std::uint16_t kFlagHighContrast = 2;

template<class T>
void put(std::vector<std::byte>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

template<class T>
T get(std::span<const std::byte> in, std::size_t offset) {
    T value;
    std::memcpy(&value, in.data() + offset, sizeof(T));
    return value;
}

// FNV-1a over [0,40) then [48,end): everything but the checksum field itself.
std::uint64_t blobChecksum(std::span<const std::byte> blob) {
    return fnv1a64(blob.subspan(kChecksumEnd), fnv1a64(blob.first(kChecksumOffset)));
}

Result<Theme> corrupt(std::string message) { return fail(ErrorCode::Corrupt, "theme blob: " + std::move(message)); }

} // namespace

std::vector<std::byte> encodeThemeBlob(const ThemeData& data) {
    FROST_PRE(Theme::fromData(data).has_value());
    const std::size_t total = kStringsOffset + data.id.size() + data.name.size();
    std::vector<std::byte> out(total, std::byte{0});
    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    put<std::uint16_t>(out, 8, kThemeBlobVersion);
    put<std::uint16_t>(
        out, 10, static_cast<std::uint16_t>((data.dark ? kFlagDark : 0) | (data.highContrast ? kFlagHighContrast : 0)));
    put<std::uint64_t>(out, 16, kRegistryHash);
    put<std::uint16_t>(out, 24, static_cast<std::uint16_t>(kColorKeyCount));
    put<std::uint16_t>(out, 26, static_cast<std::uint16_t>(kMetricKeyCount));
    put<std::uint16_t>(out, 28, static_cast<std::uint16_t>(kIconRoleCount));
    put<std::uint16_t>(out, 30, static_cast<std::uint16_t>(data.id.size()));
    put<std::uint16_t>(out, 32, static_cast<std::uint16_t>(data.name.size()));
    put<std::uint32_t>(out, 36, static_cast<std::uint32_t>(total));
    for (std::size_t i = 0; i < kColorKeyCount; ++i) {
        put<std::uint32_t>(out, kColorsOffset + i * 4, data.colors[i].argb);
    }
    for (std::size_t i = 0; i < kMetricKeyCount; ++i) {
        put<float>(out, kMetricsOffset + i * 8, data.metrics[i]);
        put<float>(out, kMetricsOffset + i * 8 + 4, data.compactMetrics[i]);
    }
    for (std::size_t i = 0; i < kIconRoleCount; ++i) {
        put<std::uint32_t>(out, kRolesOffset + i * 4, data.iconColors[i].argb);
    }
    std::memcpy(out.data() + kStringsOffset, data.id.data(), data.id.size());
    std::memcpy(out.data() + kStringsOffset + data.id.size(), data.name.data(), data.name.size());
    put<std::uint64_t>(out, kChecksumOffset, blobChecksum(out));
    return out;
}

Result<Theme> decodeThemeBlob(std::span<const std::byte> blob) {
    if (blob.size() < kThemeBlobHeaderSize) {
        return corrupt(std::format("{} bytes is shorter than the {}-byte header", blob.size(), kThemeBlobHeaderSize));
    }
    if (std::memcmp(blob.data(), kMagic.data(), kMagic.size()) != 0) {
        return corrupt("bad magic");
    }
    if (const auto total = get<std::uint32_t>(blob, 36); total != blob.size()) {
        return corrupt(std::format("totalSize {} differs from the size {}", total, blob.size()));
    }
    // Checked before the version so that a damaged version field reads as damage.
    if (get<std::uint64_t>(blob, kChecksumOffset) != blobChecksum(blob)) {
        return corrupt("checksum mismatch");
    }
    if (const auto version = get<std::uint16_t>(blob, 8); version != kThemeBlobVersion) {
        return fail(ErrorCode::Unsupported,
                    std::format("theme blob format version {} (expected {})", version, kThemeBlobVersion));
    }
    if (get<std::uint64_t>(blob, 16) != kRegistryHash) {
        return fail(ErrorCode::Unsupported, "theme blob compiled against another key registry; rebuild");
    }
    const auto flags = get<std::uint16_t>(blob, 10);
    if ((flags & ~(kFlagDark | kFlagHighContrast)) != 0 || get<std::uint32_t>(blob, 12) != 0 ||
        get<std::uint16_t>(blob, 34) != 0) {
        return corrupt("reserved bits or fields are not zero");
    }
    if (get<std::uint16_t>(blob, 24) != kColorKeyCount || get<std::uint16_t>(blob, 26) != kMetricKeyCount ||
        get<std::uint16_t>(blob, 28) != kIconRoleCount) {
        return corrupt("key counts differ from the registry");
    }
    const std::size_t idLength = get<std::uint16_t>(blob, 30);
    const std::size_t nameLength = get<std::uint16_t>(blob, 32);
    if (kStringsOffset + idLength + nameLength != blob.size()) {
        return corrupt("string lengths do not match the size");
    }
    ThemeData data;
    data.dark = (flags & kFlagDark) != 0;
    data.highContrast = (flags & kFlagHighContrast) != 0;
    for (std::size_t i = 0; i < kColorKeyCount; ++i) {
        data.colors[i] = Rgba{get<std::uint32_t>(blob, kColorsOffset + i * 4)};
    }
    for (std::size_t i = 0; i < kMetricKeyCount; ++i) {
        data.metrics[i] = get<float>(blob, kMetricsOffset + i * 8);
        data.compactMetrics[i] = get<float>(blob, kMetricsOffset + i * 8 + 4);
    }
    for (std::size_t i = 0; i < kIconRoleCount; ++i) {
        data.iconColors[i] = Rgba{get<std::uint32_t>(blob, kRolesOffset + i * 4)};
    }
    const auto* strings = reinterpret_cast<const char*>(blob.data() + kStringsOffset);
    data.id.assign(strings, idLength);
    data.name.assign(strings + idLength, nameLength);
    auto theme = Theme::fromData(data);
    if (!theme) {
        return corrupt(theme.error().message());
    }
    return theme;
}

Result<Theme> loadThemeFile(io::IFileSystem& fs, std::string_view absolutePath, std::string_view expectedId) {
    const auto context = [&](Error error) {
        return std::unexpected(std::move(error).withContext(std::format("while loading theme {}", absolutePath)));
    };
    auto status = fs.status(absolutePath);
    if (!status) {
        return context(std::move(status.error()));
    }
    if (status->type != io::FileType::Regular) {
        return context(Error(ErrorCode::InvalidArgument, "not a regular file"));
    }
    if (status->size > kMaxThemeFileBytes) {
        return context(Error(ErrorCode::Corrupt, std::format("{} bytes is over the {}-byte theme file limit",
                                                             status->size, kMaxThemeFileBytes)));
    }
    auto bytes = fs.readFile(absolutePath);
    if (!bytes) {
        return context(std::move(bytes.error()));
    }
    auto theme = decodeThemeBlob(std::as_bytes(std::span(*bytes)));
    if (!theme) {
        return context(std::move(theme.error()));
    }
    if (theme->id() != expectedId) {
        return context(
            Error(ErrorCode::Corrupt, std::format("embedded id '{}' differs from '{}'", theme->id(), expectedId)));
    }
    return theme;
}

Result<std::vector<ThemeInfo>> listInstalledThemes(io::IFileSystem& fs, std::string_view themesDir,
                                                   std::vector<Error>* skipped) {
    auto entries = fs.listDirectory(themesDir);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    std::vector<ThemeInfo> themes;
    for (const io::DirEntry& entry : *entries) {
        if (entry.type != io::FileType::Regular || !entry.name.ends_with(kThemeFileSuffix)) {
            continue;
        }
        const std::string_view id = std::string_view(entry.name).substr(0, entry.name.size() - kThemeFileSuffix.size());
        auto path = io::joinPath(themesDir, entry.name);
        auto theme = path ? loadThemeFile(fs, *path, id) : Result<Theme>(std::unexpected(path.error()));
        if (!theme) {
            if (skipped != nullptr) {
                skipped->push_back(std::move(theme.error()));
            }
            continue;
        }
        themes.push_back(
            {std::string(theme->id()), std::string(theme->name()), theme->isDark(), theme->isHighContrast()});
    }
    std::ranges::sort(themes, {}, &ThemeInfo::id);
    return themes;
}

} // namespace frost::ui
