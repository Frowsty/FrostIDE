// SPDX-License-Identifier: Apache-2.0
//
// MSVC throw decoding (msvc_throw.hpp, DBG-2b): PE header and ThrowInfo field parsing, the RTTI type
// name undecorator and the type filter's matching.

#include "frost/debug/msvc_throw.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace frost::debug::msvc {

namespace {

std::optional<std::uint16_t> readU16(std::string_view bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset]) |
                                      (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

/*
 * A recursive-descent reader over the subset of the Microsoft type-name mangling that RTTI type
 * descriptors of thrown types use (msvc_throw.hpp). The grammar follows LLVM's
 * llvm/lib/Demangle/MicrosoftDemangle.cpp (Apache-2.0 WITH LLVM-exception): demangleFullyQualifiedTypeName,
 * demangleSimpleName with its 10-entry back-reference table, demanglePrimitiveType and
 * demanglePointerType. No code was copied; unsupported forms make the whole name fail.
 */
class TypeNameReader {
public:
    explicit TypeNameReader(std::string_view text) noexcept : m_text(text) {}

    // The type at the cursor; nullopt for an unsupported or malformed encoding.
    std::optional<std::string> type() {
        if (m_text.empty()) {
            return std::nullopt;
        }
        const char c = take();
        switch (c) {
        case 'V':
        case 'U':
        case 'T': return qualifiedName();
        case 'W':
            // Enums: W then the underlying type's digit ('4' is int).
            if (m_text.empty() || std::isdigit(static_cast<unsigned char>(m_text.front())) == 0) {
                return std::nullopt;
            }
            take();
            return qualifiedName();
        case 'P':
        case 'Q':
        case 'R':
        case 'S': return pointer(c);
        case '_': return extendedPrimitive();
        default: return primitive(c);
        }
    }

    [[nodiscard]] bool atEnd() const noexcept { return m_text.empty(); }

private:
    char take() {
        const char c = m_text.front();
        m_text.remove_prefix(1);
        return c;
    }

    static std::optional<std::string> primitive(char c) {
        switch (c) {
        case 'C': return "signed char";
        case 'D': return "char";
        case 'E': return "unsigned char";
        case 'F': return "short";
        case 'G': return "unsigned short";
        case 'H': return "int";
        case 'I': return "unsigned int";
        case 'J': return "long";
        case 'K': return "unsigned long";
        case 'M': return "float";
        case 'N': return "double";
        case 'O': return "long double";
        case 'X': return "void";
        default: return std::nullopt;
        }
    }

    std::optional<std::string> extendedPrimitive() {
        if (m_text.empty()) {
            return std::nullopt;
        }
        switch (take()) {
        case 'J': return "__int64";
        case 'K': return "unsigned __int64";
        case 'N': return "bool";
        case 'W': return "wchar_t";
        case 'S': return "char16_t";
        case 'U': return "char32_t";
        case 'Q': return "char8_t";
        default: return std::nullopt;
        }
    }

    // P/Q/R/S (the pointer's own cv), an optional E (__ptr64), the pointee's cv (A-D), the pointee.
    std::optional<std::string> pointer(char kind) {
        if (!m_text.empty() && m_text.front() == 'E') {
            take();
        }
        if (m_text.empty()) {
            return std::nullopt;
        }
        const char cv = take();
        if (cv < 'A' || cv > 'D') {
            return std::nullopt;
        }
        auto pointee = type();
        if (!pointee) {
            return std::nullopt;
        }
        std::string out;
        if (cv == 'B' || cv == 'D') {
            out += "const ";
        }
        if (cv == 'C' || cv == 'D') {
            out += "volatile ";
        }
        out += *pointee;
        out += " *";
        if (kind == 'Q' || kind == 'S') {
            out += " const";
        }
        if (kind == 'R' || kind == 'S') {
            out += " volatile";
        }
        return out;
    }

    // Fragments innermost first, each ended by '@', the list ended by one more '@'.
    std::optional<std::string> qualifiedName() {
        std::vector<std::string> parts;
        while (true) {
            if (m_text.empty()) {
                return std::nullopt;
            }
            if (m_text.front() == '@') {
                take();
                break;
            }
            auto part = fragment();
            if (!part) {
                return std::nullopt;
            }
            parts.push_back(std::move(*part));
        }
        if (parts.empty()) {
            return std::nullopt;
        }
        std::string out;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (!out.empty()) {
                out += "::";
            }
            out += *it;
        }
        return out;
    }

    std::optional<std::string> fragment() {
        const char c = m_text.front();
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            take();
            const auto index = static_cast<std::size_t>(c - '0');
            if (index >= m_memorized) {
                return std::nullopt;
            }
            return m_names[index];
        }
        if (c == '?') {
            // Only the anonymous namespace ("?A0x1234abcd@"); templates ("?$") and the rest fail.
            if (m_text.size() < 2 || m_text[1] != 'A') {
                return std::nullopt;
            }
            const std::size_t end = m_text.find('@');
            if (end == std::string_view::npos) {
                return std::nullopt;
            }
            m_text.remove_prefix(end + 1);
            std::string name = "`anonymous namespace'";
            memorize(name);
            return name;
        }
        const std::size_t end = m_text.find('@');
        if (end == std::string_view::npos || end == 0) {
            return std::nullopt;
        }
        std::string name(m_text.substr(0, end));
        for (const char ch : name) {
            if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_' && ch != '$') {
                return std::nullopt;
            }
        }
        m_text.remove_prefix(end + 1);
        memorize(name);
        return name;
    }

    void memorize(const std::string& name) {
        for (std::size_t i = 0; i < m_memorized; ++i) {
            if (m_names[i] == name) {
                return;
            }
        }
        if (m_memorized < m_names.size()) {
            m_names[m_memorized++] = name;
        }
    }

    std::string_view m_text;
    std::array<std::string, 10> m_names{};
    std::size_t m_memorized = 0;
};

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::optional<std::uint32_t> readU32(std::string_view bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

Result<ImageHeader> parseImageHeader(std::string_view bytes) {
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') {
        return fail(ErrorCode::Corrupt, "the module's base holds no DOS header");
    }
    const auto peOffset = readU32(bytes, 0x3C);
    if (!peOffset || bytes.substr(*peOffset, 4) != std::string_view("PE\0\0", 4)) {
        return fail(ErrorCode::Corrupt, "the module's base holds no PE header");
    }
    // The COFF header (20 bytes) follows the signature; SizeOfImage is at +56 of the optional header.
    const auto machine = readU16(bytes, *peOffset + 4);
    const auto size = readU32(bytes, *peOffset + 4 + 20 + 56);
    if (!machine || !size) {
        return fail(ErrorCode::Corrupt, "the module's PE header is cut short");
    }
    return ImageHeader{.machine = *machine, .sizeOfImage = *size};
}

std::optional<std::uint64_t> parseAddress(std::string_view value) {
    value = trim(value);
    std::uint64_t out = 0;
    std::size_t digits = 0;
    if (value.starts_with("0x") || value.starts_with("0X")) {
        for (const char c : value.substr(2)) {
            int nibble = -1;
            if (c >= '0' && c <= '9') {
                nibble = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                nibble = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                nibble = c - 'A' + 10;
            }
            if (nibble < 0) {
                break;
            }
            if (++digits > 16) {
                return std::nullopt;
            }
            out = (out << 4) | static_cast<std::uint64_t>(nibble);
        }
        return digits > 0 ? std::optional(out) : std::nullopt;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            break;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (out > (UINT64_MAX - digit) / 10) {
            return std::nullopt;
        }
        out = out * 10 + digit;
        ++digits;
    }
    return digits > 0 ? std::optional(out) : std::nullopt;
}

std::string_view cString(std::string_view bytes) {
    const std::size_t end = bytes.find('\0');
    return end == std::string_view::npos ? bytes : bytes.substr(0, end);
}

std::optional<std::string> undecorateTypeName(std::string_view decorated) {
    if (!decorated.starts_with('.')) {
        return std::nullopt;
    }
    decorated.remove_prefix(1);
    // Class types carry the RTTI "?A" prefix; built-in and pointer types do not.
    if (decorated.starts_with("?A")) {
        decorated.remove_prefix(2);
    }
    TypeNameReader reader(decorated);
    auto out = reader.type();
    if (!out || !reader.atEnd()) {
        return std::nullopt;
    }
    return out;
}

std::string normalizeTypeName(std::string_view name) {
    name = trim(name);
    for (const std::string_view keyword : {"class ", "struct ", "union ", "enum "}) {
        if (name.starts_with(keyword)) {
            name.remove_prefix(keyword.size());
            break;
        }
    }
    name = trim(name);
    if (name.starts_with("::")) {
        name.remove_prefix(2);
    }
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            out.push_back(c);
        }
    }
    return out;
}

bool throwTypeMatches(const std::vector<std::string>& filters, std::string_view undecorated,
                      std::string_view decorated) {
    if (filters.empty()) {
        return true;
    }
    const std::string thrown = normalizeTypeName(undecorated);
    for (const std::string& filter : filters) {
        const std::string wanted = normalizeTypeName(filter);
        if (wanted.empty()) {
            continue;
        }
        if ((!thrown.empty() && wanted == thrown) || (!decorated.empty() && wanted == decorated)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> splitTypeList(std::string_view text) {
    std::vector<std::string> out;
    std::vector<std::string> seen;
    const auto flush = [&](std::string_view piece) {
        piece = trim(piece);
        if (piece.empty()) {
            return;
        }
        std::string key = normalizeTypeName(piece);
        for (const std::string& known : seen) {
            if (known == key) {
                return;
            }
        }
        seen.push_back(std::move(key));
        out.emplace_back(piece);
    };
    // Commas inside template arguments ("std::pair<int, int>") do not separate names.
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>' && depth > 0) {
            --depth;
        } else if ((c == ',' && depth == 0) || c == ';' || c == '\n' || c == '\r') {
            flush(text.substr(start, i - start));
            start = i + 1;
            depth = c == ',' ? depth : 0;
        }
    }
    flush(text.substr(start));
    return out;
}

} // namespace frost::debug::msvc
