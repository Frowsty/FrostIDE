// SPDX-License-Identifier: Apache-2.0
//
// The independent encoding oracles (encoding_oracle.hpp). Nothing here calls src/text or
// frost::io; UTF-8 sequences are recognised with text::test::oracleDecode (bit patterns).

#include "text/support/encoding_oracle.hpp"

#include "text/support/fake_codec.hpp"
#include "text/support/text_oracle.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace frost::text::test {
namespace {

// Bitmaps of every proper prefix of every well-formed sequence, by length (1, 2 and 3 bytes),
// built once by encoding each scalar value with oracleAppendUtf8 and checking the encoding with
// oracleDecode. 2 MiB for the 3-byte map; a few milliseconds.
struct PrefixSet {
    std::vector<bool> one = std::vector<bool>(1u << 8);
    std::vector<bool> two = std::vector<bool>(1u << 16);
    std::vector<bool> three = std::vector<bool>(1u << 24);

    PrefixSet() {
        std::string bytes;
        for (std::uint32_t c = 0; c <= 0x10FFFF; ++c) {
            if (c >= 0xD800 && c <= 0xDFFF) {
                continue;
            }
            bytes.clear();
            oracleAppendUtf8(static_cast<char32_t>(c), bytes);
            char32_t back = 0;
            if (oracleDecode(bytes, 0, back) != bytes.size() || back != static_cast<char32_t>(c)) {
                std::fprintf(stderr, "oracle encoder and oracleDecode disagree on U+%04X\n", c);
                std::abort();
            }
            const auto b = [&](std::size_t k) {
                return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[k]));
            };
            if (bytes.size() >= 2) {
                one[b(0)] = true;
            }
            if (bytes.size() >= 3) {
                two[(b(0) << 8) | b(1)] = true;
            }
            if (bytes.size() == 4) {
                three[(b(0) << 16) | (b(1) << 8) | b(2)] = true;
            }
        }
    }
};

const PrefixSet& prefixes() {
    static const PrefixSet set;
    return set;
}

void replaceAt(OracleDecode& out, std::uint64_t inputOffset) {
    if (!out.firstInvalid) {
        out.firstInvalid = inputOffset;
        out.firstReplacementOutput = out.utf8.size();
    }
    ++out.replacements;
    oracleAppendUtf8(0xFFFD, out.utf8);
}

std::uint32_t readUnit(std::string_view bytes, std::size_t at, std::size_t width, bool bigEndian) {
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < width; ++k) {
        const std::size_t index = bigEndian ? at + k : at + width - 1 - k;
        value = (value << 8) | static_cast<std::uint8_t>(bytes[index]);
    }
    return value;
}

void writeUnit(std::uint32_t value, std::size_t width, bool bigEndian, std::string& out) {
    for (std::size_t k = 0; k < width; ++k) {
        const std::size_t shift = bigEndian ? 8 * (width - 1 - k) : 8 * k;
        out.push_back(static_cast<char>(static_cast<std::uint8_t>((value >> shift) & 0xFF)));
    }
}

template<class Fn>
void forEachScalar(std::string_view utf8, Fn&& fn) {
    for (std::size_t i = 0; i < utf8.size();) {
        char32_t c = 0;
        const std::size_t length = oracleDecode(utf8, i, c);
        if (length == 0) {
            std::fprintf(stderr, "oracle encoder given ill-formed UTF-8 at %zu\n", i);
            std::abort();
        }
        fn(c, i, length);
        i += length;
    }
}

bool isC1Byte(const FakeCodePage& page, std::uint8_t b) {
    const auto& c1 = page.info().windowsOnlyC1;
    for (const std::uint8_t x : c1) {
        if (x == b) {
            return true;
        }
    }
    return false;
}

// The scalar `bytes` (one unit) decodes to under the driver policy, if it is exactly one unit.
std::optional<char32_t> decodeOneUnit(const FakeCodePage& page, std::string_view bytes) {
    if (bytes.size() == 1) {
        const auto b = static_cast<std::uint8_t>(bytes[0]);
        if (page.isLead(b)) {
            return std::nullopt;
        }
        if (const auto s = page.single(b)) {
            return s;
        }
        if (isC1Byte(page, b)) {
            return static_cast<char32_t>(b);
        }
        return std::nullopt;
    }
    if (bytes.size() == 2 && page.isLead(static_cast<std::uint8_t>(bytes[0]))) {
        return page.pair(static_cast<std::uint8_t>(bytes[0]), static_cast<std::uint8_t>(bytes[1]));
    }
    return std::nullopt;
}

} // namespace

void oracleAppendUtf8(char32_t scalar, std::string& out) {
    const auto c = static_cast<std::uint32_t>(scalar);
    const auto put = [&](std::uint32_t v) { out.push_back(static_cast<char>(static_cast<std::uint8_t>(v))); };
    if (c <= 0x7F) {
        put(c);
    } else if (c <= 0x7FF) {
        put(0b1100'0000 | (c >> 6));
        put(0b1000'0000 | (c & 0b11'1111));
    } else if (c <= 0xFFFF) {
        put(0b1110'0000 | (c >> 12));
        put(0b1000'0000 | ((c >> 6) & 0b11'1111));
        put(0b1000'0000 | (c & 0b11'1111));
    } else {
        put(0b1111'0000 | (c >> 18));
        put(0b1000'0000 | ((c >> 12) & 0b11'1111));
        put(0b1000'0000 | ((c >> 6) & 0b11'1111));
        put(0b1000'0000 | (c & 0b11'1111));
    }
}

bool oracleIsProperPrefix(std::string_view prefix) {
    const PrefixSet& set = prefixes();
    const auto b = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(prefix[k])); };
    switch (prefix.size()) {
    case 1: return set.one[b(0)];
    case 2: return set.two[(b(0) << 8) | b(1)];
    case 3: return set.three[(b(0) << 16) | (b(1) << 8) | b(2)];
    default: return false;
    }
}

OracleDecode oracleUtf8Lossy(std::string_view bytes) {
    OracleDecode out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        char32_t c = 0;
        if (const std::size_t length = oracleDecode(bytes, i, c); length != 0) {
            out.utf8.append(bytes.substr(i, length));
            i += length;
            continue;
        }
        // The maximal subpart: the longest proper prefix of a well-formed sequence, or one byte.
        std::size_t k = 1;
        for (std::size_t len = std::min<std::size_t>(3, bytes.size() - i); len >= 1; --len) {
            if (oracleIsProperPrefix(bytes.substr(i, len))) {
                k = len;
                break;
            }
        }
        replaceAt(out, i);
        i += k;
    }
    return out;
}

OracleDecode oracleUtf16Lossy(std::string_view bytes, bool bigEndian) {
    OracleDecode out;
    const std::size_t units = bytes.size() / 2;
    std::size_t u = 0;
    while (u < units) {
        const std::uint32_t a = readUnit(bytes, 2 * u, 2, bigEndian);
        const bool high = a >= 0xD800 && a <= 0xDBFF;
        const bool low = a >= 0xDC00 && a <= 0xDFFF;
        if (high && u + 1 < units) {
            const std::uint32_t b = readUnit(bytes, 2 * (u + 1), 2, bigEndian);
            if (b >= 0xDC00 && b <= 0xDFFF) {
                oracleAppendUtf8(static_cast<char32_t>(0x10000 + ((a - 0xD800) << 10) + (b - 0xDC00)), out.utf8);
                u += 2;
                continue;
            }
        }
        if (high || low) {
            replaceAt(out, 2 * u);
        } else {
            oracleAppendUtf8(static_cast<char32_t>(a), out.utf8);
        }
        u += 1;
    }
    if (bytes.size() % 2 != 0) {
        replaceAt(out, bytes.size() - 1);
    }
    return out;
}

OracleDecode oracleUtf32Lossy(std::string_view bytes, bool bigEndian) {
    OracleDecode out;
    const std::size_t units = bytes.size() / 4;
    for (std::size_t u = 0; u < units; ++u) {
        const std::uint32_t a = readUnit(bytes, 4 * u, 4, bigEndian);
        if (a > 0x10FFFF || (a >= 0xD800 && a <= 0xDFFF)) {
            replaceAt(out, 4 * u);
        } else {
            oracleAppendUtf8(static_cast<char32_t>(a), out.utf8);
        }
    }
    if (bytes.size() % 4 != 0) {
        replaceAt(out, 4 * units);
    }
    return out;
}

std::string oracleEncodeUtf16(std::string_view utf8, bool bigEndian) {
    std::string out;
    forEachScalar(utf8, [&](char32_t c, std::size_t, std::size_t) {
        const auto v = static_cast<std::uint32_t>(c);
        if (v >= 0x10000) {
            writeUnit(0xD800 + ((v - 0x10000) >> 10), 2, bigEndian, out);
            writeUnit(0xDC00 + ((v - 0x10000) & 0x3FF), 2, bigEndian, out);
        } else {
            writeUnit(v, 2, bigEndian, out);
        }
    });
    return out;
}

std::string oracleEncodeUtf32(std::string_view utf8, bool bigEndian) {
    std::string out;
    forEachScalar(utf8, [&](char32_t c, std::size_t, std::size_t) {
        writeUnit(static_cast<std::uint32_t>(c), 4, bigEndian, out);
    });
    return out;
}

EolStats oracleEolStats(std::string_view text) {
    EolStats stats;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++stats.crlf;
                ++i;
            } else {
                ++stats.cr;
            }
        } else if (text[i] == '\n') {
            ++stats.lf;
        }
    }
    return stats;
}

OracleDecode legacyOracle(const FakeCodePage& page, std::string_view bytes) {
    OracleDecode out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto b = static_cast<std::uint8_t>(bytes[i]);
        if (page.isLead(b)) {
            if (i + 1 < bytes.size()) {
                const auto trail = static_cast<std::uint8_t>(bytes[i + 1]);
                if (const auto scalar = page.pair(b, trail)) {
                    oracleAppendUtf8(*scalar, out.utf8);
                    i += 2;
                    continue;
                }
                replaceAt(out, i);
                i += trail >= 0x80 ? 2 : 1; // An ASCII trail is read again (WHATWG Shift_JIS rule).
                continue;
            }
            replaceAt(out, i); // A lead byte at the end (Final).
            i += 1;
            continue;
        }
        if (const auto scalar = page.single(b)) {
            oracleAppendUtf8(*scalar, out.utf8);
        } else if (isC1Byte(page, b)) {
            oracleAppendUtf8(static_cast<char32_t>(b), out.utf8);
        } else {
            replaceAt(out, i);
        }
        i += 1;
    }
    return out;
}

OracleEncode oracleEncodeLegacy(const FakeCodePage& page, std::string_view utf8) {
    OracleEncode out;
    bool stopped = false;
    forEachScalar(utf8, [&](char32_t c, std::size_t at, std::size_t) {
        if (stopped) {
            return;
        }
        if (const auto bytes = page.encode(c); bytes && decodeOneUnit(page, *bytes) == c) {
            out.bytes.append(*bytes);
            return;
        }
        if (c >= 0x80 && c <= 0x9F && !page.isLead(static_cast<std::uint8_t>(c)) &&
            isC1Byte(page, static_cast<std::uint8_t>(c))) {
            out.bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(c)));
            return;
        }
        out.stop = at;
        stopped = true;
    });
    return out;
}

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t hash) {
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= kPrime;
    }
    return hash;
}

std::string fromU8(std::u8string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char8_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string fromHex(std::string_view hex) {
    std::string out;
    int pending = -1;
    for (const char c : hex) {
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue;
        }
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<char>(static_cast<std::uint8_t>(pending * 16 + v)));
            pending = -1;
        }
    }
    return out;
}

std::string toHex(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        const auto b = static_cast<std::uint8_t>(bytes[i]);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

} // namespace frost::text::test
