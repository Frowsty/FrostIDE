// SPDX-License-Identifier: Apache-2.0
//
// The navigation models' path helpers (frost/ui/nav/paths.hpp).

#include "frost/ui/nav/paths.hpp"

#include <algorithm>

namespace frost::ui::nav {
namespace {

[[nodiscard]] constexpr char fold(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

io::PathStyle styleOf(std::string_view path) noexcept {
    constexpr io::PathStyle kOther =
        io::kNativePathStyle == io::PathStyle::Windows ? io::PathStyle::Posix : io::PathStyle::Windows;
    if (io::isAbsolutePath(path, io::kNativePathStyle) || !io::isAbsolutePath(path, kOther)) {
        return io::kNativePathStyle;
    }
    return kOther;
}

std::string joinIn(std::string_view base, std::string_view name) {
    auto joined = io::joinPath(base, name, styleOf(base));
    return joined ? std::move(*joined) : std::string();
}

std::string_view baseName(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view parentOf(std::string_view path) noexcept { return io::parentPath(path, styleOf(path)); }

std::string relativeTo(std::string_view root, std::string_view path) {
    if (root.empty() || path.size() <= root.size()) {
        return std::string(path);
    }
    const bool windows = styleOf(root) == io::PathStyle::Windows;
    const std::string_view head = path.substr(0, root.size());
    const bool same =
        windows ? std::ranges::equal(head, root, [](char a, char b) { return fold(a) == fold(b); }) : head == root;
    if (!same) {
        return std::string(path);
    }
    std::string_view rest = path.substr(root.size());
    const bool rootEndsWithSeparator = root.back() == '/' || root.back() == '\\';
    if (!rootEndsWithSeparator) {
        if (rest.front() != '/' && rest.front() != '\\') {
            return std::string(path); // "/a/bc" is not below "/a/b".
        }
        rest.remove_prefix(1);
    }
    std::string out(rest);
    std::ranges::replace(out, '\\', '/');
    return out;
}

} // namespace frost::ui::nav
