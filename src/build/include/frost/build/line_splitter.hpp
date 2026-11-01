// SPDX-License-Identifier: Apache-2.0
//
// LineSplitter: turns the chunks a build tool writes to a pipe into whole lines of valid UTF-8,
// for the build output window and the diagnostic parser.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace frost::build {

/*
 * Lines end at LF; a CR before the LF is dropped, so CRLF and LF output read the same. Bytes that
 * are not valid UTF-8 become U+FFFD, one per invalid byte, so a tool that ignores the requested
 * encoding still yields readable lines. Once more than kMaxLineBytes of a line are buffered across
 * feed() calls, the buffered part is passed on as a line of its own (cut at a character boundary),
 * which bounds memory for output without line breaks. Not thread-safe; use one splitter per stream.
 */
class LineSplitter {
public:
    using Sink = std::function<void(std::string_view line)>;

    static constexpr std::size_t kMaxLineBytes = 64 * 1024;

    // Appends `bytes` and passes every line it completes to `sink`, in order. O(bytes).
    void feed(std::string_view bytes, const Sink& sink);
    // Passes the unterminated rest, if any, to `sink` (call once at end of stream).
    void finish(const Sink& sink);

private:
    void emit(std::string_view line, const Sink& sink);

    std::string m_pending;
};

// `bytes` with each byte that is not part of a valid UTF-8 sequence replaced by U+FFFD.
[[nodiscard]] std::string sanitizeUtf8(std::string_view bytes);

} // namespace frost::build
