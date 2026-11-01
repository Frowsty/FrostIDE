// SPDX-License-Identifier: Apache-2.0
//
// LineSplitter and sanitizeUtf8 (see frost/build/line_splitter.hpp).

#include "frost/build/line_splitter.hpp"

#include "frost/io/utf.hpp"

namespace frost::build {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD

constexpr bool isContinuation(char c) noexcept { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

} // namespace

std::string sanitizeUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    while (!bytes.empty()) {
        const std::size_t valid = io::validUtf8Prefix(bytes);
        out.append(bytes.substr(0, valid));
        bytes.remove_prefix(valid);
        if (!bytes.empty()) {
            out.append(kReplacement);
            bytes.remove_prefix(1);
        }
    }
    return out;
}

void LineSplitter::emit(std::string_view line, const Sink& sink) {
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    if (io::validUtf8Prefix(line) == line.size()) {
        sink(line);
    } else {
        sink(sanitizeUtf8(line));
    }
}

void LineSplitter::feed(std::string_view bytes, const Sink& sink) {
    while (!bytes.empty()) {
        const std::size_t newline = bytes.find('\n');
        const std::string_view piece = bytes.substr(0, newline);
        if (newline != std::string_view::npos && m_pending.empty()) {
            emit(piece, sink); // The common case: a whole line inside one chunk, no copy.
        } else {
            m_pending.append(piece);
            if (newline != std::string_view::npos) {
                emit(m_pending, sink);
                m_pending.clear();
            }
        }
        bytes.remove_prefix(newline == std::string_view::npos ? bytes.size() : newline + 1);
        // Cut an overlong line at a UTF-8 character boundary (at most three bytes back).
        while (m_pending.size() > kMaxLineBytes) {
            std::size_t cut = kMaxLineBytes;
            for (int back = 0; back < 3 && cut > 0 && isContinuation(m_pending[cut]); ++back) {
                --cut;
            }
            emit(std::string_view(m_pending).substr(0, cut), sink);
            m_pending.erase(0, cut);
        }
    }
}

void LineSplitter::finish(const Sink& sink) {
    if (!m_pending.empty()) {
        emit(m_pending, sink);
        m_pending.clear();
    }
}

} // namespace frost::build
