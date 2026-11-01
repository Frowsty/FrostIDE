// SPDX-License-Identifier: Apache-2.0
//
// Find and replace (frost/text/editor/search.hpp, ADR 0025): the streamed byte search over chunks
// and reverse chunks, RE2 line-by-line and whole-text matching, whole-word checks, replacement
// expansion and the find/replace commands.

#include "frost/text/editor/search.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"
#include "frost/text/editor/unicode.hpp"

#include "command_support.hpp"
#include <re2/re2.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <utility>

namespace frost::text {

namespace detail {

// The compiled form of a query.
struct SearchProgram {
    enum class Mode : std::uint8_t {
        Bytes, // Plain and case-sensitive: streamed byte search.
        Lines, // RE2 on each line's content.
        Whole, // RE2 on the whole text.
    };

    std::string pattern;
    SearchOptions options;
    Mode mode = Mode::Bytes;
    std::unique_ptr<RE2> re; // Lines and Whole.
};

} // namespace detail

namespace {

using detail::SearchProgram;
using Mode = SearchProgram::Mode;

// How a match visitor wants the scan to go on.
enum class Next : std::uint8_t {
    Stop,   // Done.
    After,  // Continue after the match (non-overlapping; one character on after an empty match).
    Inside, // Continue one character after the match's start (it was rejected).
};

std::size_t charBytes(std::string_view text, std::size_t at) noexcept {
    return at < text.size() ? decodeUtf8(text.substr(at)).bytes : 1;
}

bool isWord(std::optional<DecodedChar> c) noexcept { return c && charClass(c->cp) == CharClass::Word; }

// search.hpp's whole-word rule.
bool wordBounded(const Snapshot& text, ByteRange range) noexcept {
    if (isWord(charAt(text, range.begin)) && isWord(charBefore(text, range.begin))) {
        return false;
    }
    return !(isWord(charBefore(text, range.end)) && isWord(charAt(text, range.end)));
}

bool acceptable(const SearchProgram& program, const Snapshot& text, ByteRange range) noexcept {
    return !program.options.wholeWord || wordBounded(text, range);
}

/*
 * Calls visit(start) for each occurrence of `needle` that starts in [from, size), in increasing
 * order of start; visit returns the smallest start still wanted, or nullopt to stop. Occurrences
 * that span chunks are found in a window of the previous chunks' last needle-1 bytes and the
 * current chunk's first ones.
 */
template<class Visit>
void bytesForward(const Snapshot& text, ByteOffset from, std::string_view needle, Visit&& visit) noexcept {
    const std::size_t n = needle.size();
    std::uint64_t allowed = from.value();
    std::string carry; // Up to n - 1 bytes that end where the current chunk starts.
    std::uint64_t carryStart = from.value();
    std::string window;
    std::uint64_t base = from.value();
    const ChunkRange chunks = text.chunks(ByteRange{from, text.size()});
    for (const std::string_view chunk : chunks) {
        if (!carry.empty()) {
            window.assign(carry);
            window.append(chunk.substr(0, std::min(n - 1, chunk.size())));
            for (std::size_t p = window.find(needle); p != std::string::npos && p < carry.size();
                 p = window.find(needle, p + 1)) {
                const std::uint64_t start = carryStart + p;
                if (start < allowed) {
                    continue;
                }
                const std::optional<std::uint64_t> next = visit(start);
                if (!next) {
                    return;
                }
                allowed = *next;
            }
        }
        const std::size_t skip =
            allowed > base ? static_cast<std::size_t>(std::min<std::uint64_t>(allowed - base, chunk.size())) : 0;
        for (std::size_t p = chunk.find(needle, skip); p != std::string_view::npos; p = chunk.find(needle, p + 1)) {
            const std::uint64_t start = base + p;
            if (start < allowed) {
                continue;
            }
            const std::optional<std::uint64_t> next = visit(start);
            if (!next) {
                return;
            }
            allowed = *next;
        }
        if (n > 1) {
            if (chunk.size() >= n - 1) {
                carry.assign(chunk.substr(chunk.size() - (n - 1)));
                carryStart = base + chunk.size() - (n - 1);
            } else {
                if (carry.empty()) {
                    carryStart = base;
                }
                carry.append(chunk);
                if (carry.size() > n - 1) {
                    carryStart += carry.size() - (n - 1);
                    carry.erase(0, carry.size() - (n - 1));
                }
            }
        }
        base += chunk.size();
    }
}

/*
 * Calls visit(start) for each occurrence of `needle` inside [0, before), in decreasing order of
 * start, until visit returns false. Walks the chunks backwards (Snapshot::reversedChunks);
 * occurrences that span chunks are found in a window of the chunk's last needle-1 bytes and the
 * first ones of the chunks after it.
 */
template<class Visit>
void bytesBackward(const Snapshot& text, ByteOffset before, std::string_view needle, Visit&& visit) noexcept {
    const std::size_t n = needle.size();
    std::string carry; // Up to n - 1 bytes that start where the current chunk ends.
    std::string window;
    const ReverseChunkRange chunks = text.reversedChunks(ByteRange{ByteOffset{0}, before});
    for (auto it = chunks.begin(); it != std::default_sentinel; ++it) {
        const std::string_view chunk = *it;
        const std::uint64_t base = it.offset().value();
        if (!carry.empty()) {
            const std::size_t tail = std::min(n - 1, chunk.size());
            window.assign(chunk.substr(chunk.size() - tail));
            window.append(carry);
            // Only occurrences that start in this chunk and end in the carry.
            for (std::size_t p = window.rfind(needle); p != std::string::npos;
                 p = p == 0 ? std::string::npos : window.rfind(needle, p - 1)) {
                if (p >= tail || p + n <= tail) {
                    continue;
                }
                if (!visit(base + chunk.size() - tail + p)) {
                    return;
                }
            }
        }
        for (std::size_t p = chunk.rfind(needle); p != std::string_view::npos;
             p = p == 0 ? std::string_view::npos : chunk.rfind(needle, p - 1)) {
            if (!visit(base + p)) {
                return;
            }
        }
        if (n > 1) {
            if (chunk.size() >= n - 1) {
                carry.assign(chunk.substr(0, n - 1));
            } else {
                carry.insert(0, chunk);
                carry.resize(std::min(carry.size(), n - 1));
            }
        }
    }
}

/*
 * Runs the program over `subject` (the text from `subjectStart`) from byte `from` to `limit`,
 * calling visit(range, groups) for each match (groups[0] is the match).
 */
template<class Visit>
Next scanSubject(const SearchProgram& program, std::string_view subject, ByteOffset subjectStart, std::size_t from,
                 std::size_t limit, std::vector<absl::string_view>& groups, Visit&& visit) noexcept {
    std::size_t pos = from;
    while (pos <= limit) {
        if (!program.re->Match(absl::string_view(subject.data(), subject.size()), pos, limit, RE2::UNANCHORED,
                               groups.data(), static_cast<int>(groups.size()))) {
            return Next::After;
        }
        const auto begin = static_cast<std::size_t>(groups[0].data() - subject.data());
        const std::size_t end = begin + groups[0].size();
        const Next next = visit(ByteRange{subjectStart + begin, subjectStart + end}, std::span(groups));
        if (next == Next::Stop) {
            return next;
        }
        pos = next == Next::After && end > begin ? end : begin + charBytes(subject, begin);
    }
    return Next::After;
}

// Visits the matches that start at or after `from`, in order (Lines and Whole modes).
template<class Visit>
void regexForward(const SearchProgram& program, const Snapshot& text, ByteOffset from, Visit&& visit) noexcept {
    std::vector<absl::string_view> groups(static_cast<std::size_t>(program.re->NumberOfCapturingGroups() + 1));
    if (program.mode == Mode::Whole) {
        const std::string whole = text.text();
        scanSubject(program, whole, ByteOffset{0}, static_cast<std::size_t>(from.value()), whole.size(), groups, visit);
        return;
    }
    Result<LineCursor> cursor = text.linesFrom(text.lineAt(from));
    LineInfo line;
    std::string buffer;
    while (cursor->next(line)) {
        if (line.content.end < from) {
            continue;
        }
        buffer = text.text(line.content);
        const std::size_t start = from > line.content.begin ? static_cast<std::size_t>(from - line.content.begin) : 0;
        if (scanSubject(program, buffer, line.content.begin, start, buffer.size(), groups, visit) == Next::Stop) {
            return;
        }
    }
}

// The last acceptable non-empty match that ends at or before `before` (Lines and Whole modes).
std::optional<ByteRange> regexBackward(const SearchProgram& program, const Snapshot& text, ByteOffset before) noexcept {
    std::vector<absl::string_view> groups(static_cast<std::size_t>(program.re->NumberOfCapturingGroups() + 1));
    std::optional<ByteRange> last;
    const auto keep = [&](ByteRange range, std::span<absl::string_view>) {
        if (range.empty() || !acceptable(program, text, range)) {
            return Next::Inside;
        }
        last = range;
        return Next::After;
    };
    if (program.mode == Mode::Whole) {
        const std::string whole = text.text();
        scanSubject(program, whole, ByteOffset{0}, 0, static_cast<std::size_t>(before.value()), groups, keep);
        return last;
    }
    for (std::uint32_t l = text.lineAt(before).value() + 1; l-- > 0;) {
        const LineInfo line = *text.lineInfo(LineIndex{l});
        const std::string buffer = text.text(line.content);
        const std::size_t limit = static_cast<std::size_t>(
            std::min(line.content.end, std::max(before, line.content.begin)) - line.content.begin);
        scanSubject(program, buffer, line.content.begin, 0, limit, groups, keep);
        if (last) {
            return last;
        }
    }
    return last;
}

// search.hpp's replacement expansion for regex mode.
std::string expand(std::string_view replacement, std::span<const absl::string_view> groups) {
    std::string out;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        const char c = replacement[i];
        if (c == '$' && i + 1 < replacement.size()) {
            const char d = replacement[i + 1];
            if (d == '$') {
                out += '$';
                ++i;
                continue;
            }
            if (d == '&') {
                out.append(groups[0].data(), groups[0].size());
                ++i;
                continue;
            }
            if (digit(d)) {
                std::size_t group = static_cast<std::size_t>(d - '0');
                std::size_t next = i + 2;
                if (next < replacement.size() && digit(replacement[next])) {
                    const std::size_t two = group * 10 + static_cast<std::size_t>(replacement[next] - '0');
                    if (two < groups.size()) {
                        group = two;
                        ++next;
                    }
                }
                if (group < groups.size()) {
                    out.append(groups[group].data(), groups[group].size());
                }
                i = next - 1;
                continue;
            }
        }
        if (c == '\\' && i + 1 < replacement.size()) {
            const char d = replacement[i + 1];
            const char escaped = d == 'n' ? '\n' : d == 'r' ? '\r' : d == 't' ? '\t' : d == '\\' ? '\\' : 0;
            if (escaped != 0) {
                out += escaped;
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// True when a regex can match a line break: a literal break, or \n or \r.
bool mentionsLineBreak(std::string_view pattern, bool regex) noexcept {
    if (pattern.find_first_of("\r\n") != std::string_view::npos) {
        return true;
    }
    if (!regex) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            if (pattern[i + 1] == 'n' || pattern[i + 1] == 'r') {
                return true;
            }
            ++i; // Skip the escaped character ("\\n" is a backslash and an n).
        }
    }
    return false;
}

// Runs `program` anchored on exactly `range`, filling `groups`; false when it does not match there.
bool matchExactly(const SearchProgram& program, const Snapshot& text, ByteRange range,
                  std::vector<absl::string_view>& groups, std::string& buffer) noexcept {
    std::size_t begin = 0;
    if (program.mode == Mode::Whole) {
        buffer = text.text();
        begin = static_cast<std::size_t>(range.begin.value());
    } else {
        const LineInfo line = *text.lineInfo(text.lineAt(range.begin));
        if (range.end > line.content.end || range.begin < line.content.begin) {
            return false;
        }
        buffer = text.text(line.content);
        begin = static_cast<std::size_t>(range.begin - line.content.begin);
    }
    return program.re->Match(absl::string_view(buffer.data(), buffer.size()), begin,
                             begin + static_cast<std::size_t>(range.size()), RE2::ANCHOR_BOTH, groups.data(),
                             static_cast<int>(groups.size()));
}

} // namespace

SearchPattern::SearchPattern(std::shared_ptr<const detail::SearchProgram> program) noexcept
    : m_program(std::move(program)) {}

Result<SearchPattern> SearchPattern::compile(std::string_view pattern, SearchOptions options) noexcept {
    if (pattern.empty()) {
        return fail(ErrorCode::InvalidArgument, "the search pattern is empty");
    }
    if (io::validUtf8Prefix(pattern) != pattern.size()) {
        return fail(ErrorCode::InvalidArgument, "the search pattern is not valid UTF-8");
    }
    auto program = std::make_shared<detail::SearchProgram>();
    program->pattern = std::string(pattern);
    program->options = options;
    const bool multiline = mentionsLineBreak(pattern, options.regex);
    if (!options.regex && options.caseSensitive) {
        program->mode = Mode::Bytes;
        return SearchPattern(std::move(program));
    }
    program->mode = multiline ? Mode::Whole : Mode::Lines;
    RE2::Options re2Options;
    re2Options.set_encoding(RE2::Options::EncodingUTF8);
    re2Options.set_case_sensitive(options.caseSensitive);
    re2Options.set_literal(!options.regex);
    re2Options.set_log_errors(false);
    const std::string source = options.regex && multiline ? "(?m)" + program->pattern : program->pattern;
    program->re = std::make_unique<RE2>(source, re2Options);
    if (!program->re->ok()) {
        return fail(ErrorCode::InvalidArgument, std::format("invalid regular expression: {}", program->re->error()));
    }
    return SearchPattern(std::move(program));
}

const std::string& SearchPattern::pattern() const noexcept { return m_program->pattern; }

SearchOptions SearchPattern::options() const noexcept { return m_program->options; }

std::optional<ByteRange> SearchPattern::findNext(const Snapshot& text, ByteOffset from) const noexcept {
    const SearchProgram& program = *m_program;
    from = text.clip(from);
    std::optional<ByteRange> found;
    if (program.mode == Mode::Bytes) {
        const std::uint64_t n = program.pattern.size();
        bytesForward(text, from, program.pattern, [&](std::uint64_t start) -> std::optional<std::uint64_t> {
            const ByteRange range{ByteOffset{start}, ByteOffset{start + n}};
            if (!acceptable(program, text, range)) {
                return start + 1;
            }
            found = range;
            return std::nullopt;
        });
        return found;
    }
    regexForward(program, text, from, [&](ByteRange range, std::span<absl::string_view>) {
        if (range.empty() || !acceptable(program, text, range)) {
            return Next::Inside;
        }
        found = range;
        return Next::Stop;
    });
    return found;
}

std::optional<ByteRange> SearchPattern::findPrevious(const Snapshot& text, ByteOffset before) const noexcept {
    const SearchProgram& program = *m_program;
    before = text.clip(before);
    if (program.mode != Mode::Bytes) {
        return regexBackward(program, text, before);
    }
    const std::uint64_t n = program.pattern.size();
    std::optional<ByteRange> found;
    bytesBackward(text, before, program.pattern, [&](std::uint64_t start) {
        const ByteRange range{ByteOffset{start}, ByteOffset{start + n}};
        if (!acceptable(program, text, range)) {
            return true;
        }
        found = range;
        return false;
    });
    return found;
}

std::vector<ByteRange> SearchPattern::findAll(const Snapshot& text) const noexcept {
    const SearchProgram& program = *m_program;
    std::vector<ByteRange> all;
    if (program.mode == Mode::Bytes) {
        const std::uint64_t n = program.pattern.size();
        bytesForward(text, ByteOffset{0}, program.pattern, [&](std::uint64_t start) -> std::optional<std::uint64_t> {
            const ByteRange range{ByteOffset{start}, ByteOffset{start + n}};
            if (!acceptable(program, text, range)) {
                return start + 1;
            }
            all.push_back(range);
            return start + n;
        });
        return all;
    }
    regexForward(program, text, ByteOffset{0}, [&](ByteRange range, std::span<absl::string_view>) {
        if (!acceptable(program, text, range)) {
            return Next::Inside;
        }
        all.push_back(range);
        return Next::After;
    });
    return all;
}

bool SearchPattern::matches(const Snapshot& text, ByteRange range) const noexcept {
    const SearchProgram& program = *m_program;
    if (range.empty() || range.end > text.size()) {
        return false;
    }
    if (program.mode == Mode::Bytes) {
        return range.size() == program.pattern.size() && text.text(range) == program.pattern &&
               acceptable(program, text, range);
    }
    std::vector<absl::string_view> groups(1);
    std::string buffer;
    return matchExactly(program, text, range, groups, buffer) && acceptable(program, text, range);
}

std::string SearchPattern::replacementFor(const Snapshot& text, ByteRange match,
                                          std::string_view replacement) const noexcept {
    const SearchProgram& program = *m_program;
    if (!program.options.regex) {
        return std::string(replacement);
    }
    std::vector<absl::string_view> groups(static_cast<std::size_t>(program.re->NumberOfCapturingGroups() + 1));
    std::string buffer;
    if (!matchExactly(program, text, match, groups, buffer)) {
        const std::string whole = text.text(match);
        groups.assign(1, absl::string_view(whole.data(), whole.size()));
        return expand(replacement, groups);
    }
    return expand(replacement, groups);
}

std::vector<TextEdit> SearchPattern::replacements(const Snapshot& text, std::string_view replacement) const noexcept {
    const SearchProgram& program = *m_program;
    std::vector<TextEdit> edits;
    if (!program.options.regex) {
        for (const ByteRange& range : findAll(text)) {
            edits.push_back(TextEdit{range, std::string(replacement)});
        }
        return edits;
    }
    regexForward(program, text, ByteOffset{0}, [&](ByteRange range, std::span<absl::string_view> groups) {
        if (!acceptable(program, text, range)) {
            return Next::Inside;
        }
        edits.push_back(TextEdit{range, expand(replacement, groups)});
        return Next::After;
    });
    return edits;
}

namespace commands {

namespace {

EditorState selecting(ByteRange range) noexcept {
    const Selection selection{range.begin, range.end};
    return EditorState::from(SelectionSet::fromSelections(std::span<const Selection>(&selection, 1), 0));
}

} // namespace

FindResult findNext(const CommandInput& in, const SearchPattern& pattern, bool wrap) noexcept {
    const ByteOffset from = in.text.clip(in.state.primary().range().end);
    std::optional<ByteRange> match = pattern.findNext(in.text, from);
    FindResult result;
    if (!match && wrap && from > ByteOffset{0}) {
        match = pattern.findNext(in.text, ByteOffset{0});
        result.wrapped = match.has_value();
    }
    result.command.state = in.state;
    if (match) {
        result.found = true;
        result.command.state = selecting(*match);
    }
    return result;
}

FindResult findPrevious(const CommandInput& in, const SearchPattern& pattern, bool wrap) noexcept {
    const ByteOffset before = in.text.clip(in.state.primary().range().begin);
    std::optional<ByteRange> match = pattern.findPrevious(in.text, before);
    FindResult result;
    if (!match && wrap && before < in.text.size()) {
        match = pattern.findPrevious(in.text, in.text.size());
        result.wrapped = match.has_value();
    }
    result.command.state = in.state;
    if (match) {
        result.found = true;
        result.command.state = selecting(*match);
    }
    return result;
}

FindResult replaceNext(const CommandInput& in, const SearchPattern& pattern, std::string_view replacement,
                       bool wrap) noexcept {
    const ByteRange current = in.state.primary().range();
    if (current.empty() || !pattern.matches(in.text, current)) {
        return findNext(in, pattern, wrap);
    }
    std::string text = pattern.replacementFor(in.text, current, replacement);
    const std::uint64_t size = text.size();
    FindResult result;
    result.replaced = 1;
    std::optional<ByteRange> next = pattern.findNext(in.text, current.end);
    if (!next && wrap) {
        next = pattern.findNext(in.text, ByteOffset{0});
        if (next && next->end > current.begin) {
            next.reset(); // Only the replaced match (or one overlapping it) is left.
        }
        result.wrapped = next.has_value();
    }
    if (next) {
        result.found = true;
        if (next->begin >= current.end) {
            const ByteOffset begin = current.begin + size + (next->begin - current.end);
            next = ByteRange{begin, begin + next->size()};
        }
        result.command.state = selecting(*next);
    } else {
        result.command.state = EditorState::caretAt(current.begin + size);
    }
    result.command.edits.push_back(TextEdit{current, std::move(text)});
    result.command.kind = EditKind::Other;
    return result;
}

FindResult replaceAll(const CommandInput& in, const SearchPattern& pattern, std::string_view replacement) noexcept {
    FindResult result;
    std::vector<TextEdit> edits = pattern.replacements(in.text, replacement);
    std::vector<RawEdit> raw;
    raw.reserve(edits.size());
    for (const TextEdit& edit : edits) {
        raw.push_back(RawEdit{edit.range, edit.text.size()});
    }
    result.replaced = edits.size();
    result.found = !edits.empty();
    result.command.state = detail::mapState(in.state, raw);
    result.command.edits = std::move(edits);
    result.command.kind = EditKind::Other;
    return result;
}

} // namespace commands

} // namespace frost::text
