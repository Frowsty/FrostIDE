// SPDX-License-Identifier: Apache-2.0
//
// OpDriver (text_ops.hpp): random operations against TextBuffer and TextOracle, with per-op and
// full checks.

#include "text/support/text_ops.hpp"

#include "frost/text/text_summary.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace frost::text::test {
namespace {

// The text alphabet: ASCII, tab, every line break, NUL, and characters at the 1/2/3/4-byte
// boundaries (U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF) plus common ones (é, €, 😀).
constexpr std::string_view kTokens[] = {
    "a",
    "b",
    " ",
    "\t",
    "\r",
    "\n",
    "\r\n",
    std::string_view("\0", 1),
    "\xC3\xA9",         // é U+00E9
    "\xC2\x80",         // U+0080
    "\xDF\xBF",         // U+07FF
    "\xE0\xA0\x80",     // U+0800
    "\xE2\x82\xAC",     // € U+20AC
    "\xEF\xBF\xBF",     // U+FFFF
    "\xF0\x90\x80\x80", // U+10000
    "\xF0\x9F\x98\x80", // 😀 U+1F600
    "\xF4\x8F\xBF\xBF", // U+10FFFF
};
constexpr std::uint64_t kTokenCount = std::size(kTokens);
constexpr std::size_t kRingSize = 16;

// A printable rendering of up to 24 bytes for reports.
std::string escape(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else if (b >= 0x20 && b < 0x7F && c != '\\' && c != '"') {
            out += c;
        } else {
            out += std::format("\\x{:02X}", b);
        }
    }
    return out;
}

std::string describe(const TextChange& c) {
    return std::format("{{start {}, oldEnd {}, newEnd {}, points ({},{}) ({},{}) ({},{})}}", c.start, c.oldEnd,
                       c.newEnd, c.startPoint.line, c.startPoint.column, c.oldEndPoint.line, c.oldEndPoint.column,
                       c.newEndPoint.line, c.newEndPoint.column);
}

std::string describe(Point p) { return std::format("({},{})", p.line, p.column); }
std::string describe(Utf16Point p) { return std::format("({},{})", p.line, p.column); }
std::string describe(const LineInfo& l) {
    return std::format("line {} [{}, {}) break {}", l.line, l.content.begin, l.content.end,
                       static_cast<int>(l.lineBreak));
}

std::string describe(const TextSummary& s) {
    return std::format("{{bytes {}, utf16 {}, chars {}, breaks {}, lf {}, first {}, last {}/{}/{}, longest {}@{}, "
                       "startsLf {}, endsCr {}}}",
                       s.bytes, s.utf16, s.chars, s.lineBreaks, s.lineFeeds, s.firstLineChars, s.lastLineChars,
                       s.lastLineBytes, s.lastLineUtf16, s.longestLineChars, s.longestLine, s.startsWithLf,
                       s.endsWithCr);
}

} // namespace

std::uint64_t FuzzSource::next(std::uint64_t bound) {
    if (bound <= 1) {
        return 0;
    }
    std::size_t bytes = 0;
    for (std::uint64_t v = bound - 1; v != 0; v >>= 8) {
        ++bytes;
    }
    if (m_data.size() - m_position < bytes) {
        m_exhausted = true;
        m_position = m_data.size();
        return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t k = 0; k < bytes; ++k) {
        value |= static_cast<std::uint64_t>(m_data[m_position + k]) << (8 * k);
    }
    m_position += bytes;
    return value % bound;
}

OpDriver::OpDriver(DriverOptions options, std::string label)
    : m_options(options), m_label(std::move(label)), m_buffer(options.buffer) {}

Error OpDriver::mismatch(const std::string& what) const {
    std::string report = std::format("{}: op {}: {}\nlast ops (oldest first):\n", m_label, m_stats.ops, what);
    const std::uint64_t last = m_stats.ops;
    const std::uint64_t first = last >= m_recent.size() ? last - m_recent.size() + 1 : 1;
    for (std::uint64_t i = first; i <= last; ++i) {
        const OpRecord& r = m_recent[i % m_recent.size()];
        if (r.index != i) {
            continue;
        }
        report += std::format("  #{} {} [{}, {}) text {} bytes \"{}\"{}{}\n", r.index, r.kind, r.a, r.b, r.textSize,
                              escape(r.text), r.outcome.empty() ? "" : " -> ", r.outcome);
    }
    return Error(ErrorCode::Internal, std::move(report));
}

void OpDriver::record(const char* kind, std::uint64_t a, std::uint64_t b, std::string_view text) {
    OpRecord& r = m_recent[m_stats.ops % m_recent.size()];
    r.index = m_stats.ops;
    r.kind = kind;
    r.a = a;
    r.b = b;
    r.text = std::string(text.substr(0, 24));
    r.textSize = text.size();
    r.outcome.clear();
}

void OpDriver::note(std::string outcome) { m_recent[m_stats.ops % m_recent.size()].outcome = std::move(outcome); }

std::uint64_t OpDriver::randomBoundary(ByteSource& source) const {
    return m_oracle.floorChar(source.next(m_oracle.size() + 1));
}

// 97.8 % 1-8 bytes, 2 % up to 4 KiB, 0.2 % up to 64 KiB. The mean (about 110 bytes) makes a
// 1000-op run grow by about 16 KiB, so the -Quick differential checks documents of a few KiB to a
// few tens of KiB; long runs (stress, 20k ops per seed) reach the 64 KiB cap and its erase bias.
std::uint64_t OpDriver::randomLength(ByteSource& source) const {
    const std::uint64_t roll = source.next(1000);
    if (roll < 978) {
        return 1 + source.next(8);
    }
    if (roll < 998) {
        return 1 + source.next(4096);
    }
    return 1 + source.next(65536);
}

std::string tokenText(ByteSource& source, std::uint64_t bytes) {
    std::string out;
    out.reserve(static_cast<std::size_t>(bytes));
    while (out.size() < bytes) {
        std::string_view token = kTokens[source.next(kTokenCount)];
        if (out.size() + token.size() > bytes) {
            token = "a";
        }
        out += token;
    }
    return out;
}

std::uint32_t seedPieceCap(std::uint64_t seed) noexcept {
    constexpr std::uint32_t kCaps[] = {4, 5, 7, 16, 64, 4096};
    return kCaps[seed % std::size(kCaps)];
}

std::string seedInitialText(std::uint64_t seed) {
    if ((seed / 6) % 4 == 0) {
        return {};
    }
    SplitMixSource source(seed ^ 0x5EED'7E47'0000'0000ULL);
    return tokenText(source, source.next(8 * 1024 + 1));
}

Result<DriverStats> runSeed(const SeedRun& run) {
    DriverOptions options;
    options.buffer.maxPieceBytes = seedPieceCap(run.seed);
    options.checkEvery = run.checkEvery;
    options.queriesPerCheck = run.queriesPerCheck;
    options.maxOps = run.ops;
    OpDriver driver(options, std::format("seed {}", run.seed));
    if (auto loaded = driver.load(seedInitialText(run.seed)); !loaded) {
        return std::unexpected(std::move(loaded).error());
    }
    SplitMixSource source(run.seed);
    while (!driver.done()) {
        if (auto stepped = driver.step(source); !stepped) {
            return std::unexpected(std::move(stepped).error());
        }
    }
    if (auto verified = driver.verifyFull(true); !verified) {
        return std::unexpected(std::move(verified).error());
    }
    return driver.stats();
}

std::string OpDriver::randomText(ByteSource& source, std::uint64_t maxBytes) const {
    if (maxBytes == 0) {
        return {};
    }
    return tokenText(source, std::min(randomLength(source), maxBytes));
}

Result<void> OpDriver::load(std::string initial) {
    m_stats.ops = 0;
    record("load", 0, initial.size(), initial);
    const std::size_t valid = oracleValidPrefix(initial);
    const bool tooLong = initial.size() > m_options.buffer.maxBufferBytes;
    std::string copy = initial;
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(std::move(copy), m_options.buffer);
    m_ring.clear();
    m_steps.clear();
    m_expectedVersion = 0;
    if (tooLong || valid != initial.size()) {
        const ErrorCode expected = tooLong ? ErrorCode::OutOfRange : ErrorCode::InvalidArgument;
        if (loaded) {
            return std::unexpected(
                mismatch(std::format("fromUtf8 accepted a text the oracle rejects ({})", toString(expected))));
        }
        if (loaded.error().code() != expected) {
            return std::unexpected(
                mismatch(std::format("fromUtf8 failed with {} ({}), expected {}", toString(loaded.error().code()),
                                     loaded.error().message(), toString(expected))));
        }
        if (!tooLong && loaded.error().message().find(std::format("at byte {}", valid)) == std::string::npos) {
            return std::unexpected(mismatch(
                std::format("fromUtf8's message '{}' does not name byte {}", loaded.error().message(), valid)));
        }
        m_buffer = TextBuffer(m_options.buffer);
        m_oracle.setText({});
        ++m_stats.rejected;
    } else {
        if (!loaded) {
            return std::unexpected(mismatch("fromUtf8 failed: " + loaded.error().describe()));
        }
        m_buffer = std::move(*loaded);
        m_oracle.setText(std::move(initial));
    }
    if (m_buffer.version() != 0 || m_buffer.current().size().value() != m_oracle.size()) {
        return std::unexpected(mismatch("the loaded buffer's version or size differs"));
    }
    return {};
}

Result<void> OpDriver::checkRejected(const Result<TextChange>& result, ErrorCode expected, std::uint64_t versionBefore,
                                     const TextSummary& summaryBefore) {
    ++m_stats.rejected;
    if (result) {
        return std::unexpected(mismatch(
            std::format("expected {}, but the edit succeeded with {}", toString(expected), describe(*result))));
    }
    note(std::string(toString(result.error().code())));
    if (result.error().code() != expected) {
        return std::unexpected(mismatch(std::format("expected {}, got {}: {}", toString(expected),
                                                    toString(result.error().code()), result.error().message())));
    }
    if (m_buffer.version() != versionBefore || !(m_buffer.current().summary() == summaryBefore)) {
        return std::unexpected(mismatch("a rejected edit changed the version or the summary"));
    }
    return {};
}

Result<void> OpDriver::checkChange(const TextChange& change, const ByteChange& expected, std::uint64_t a,
                                   std::uint64_t b, std::string_view before) {
    const ByteChange got{change.start.value(), change.oldEnd.value(), change.newEnd.value()};
    if (!(got == expected)) {
        return std::unexpected(
            mismatch(std::format("change {} differs from the oracle's [{}, {}) -> [{}, {})", describe(change),
                                 expected.start, expected.oldEnd, expected.start, expected.newEnd)));
    }
    if ((a > 0 && got.start < a - 1) || got.oldEnd > b + 1) {
        return std::unexpected(
            mismatch(std::format("change {} is wider than [begin - 1, end + 1) = [{}, {})", describe(change), a, b)));
    }
    if (crlfInterior(before, got.start) || crlfInterior(before, got.oldEnd)) {
        return std::unexpected(
            mismatch(std::format("change {} has a CRLF-interior bound in the old text", describe(change))));
    }
    return {};
}

Result<void> OpDriver::checkWindow(const TextChange& change) {
    const Snapshot& current = m_buffer.current();
    if (current.size().value() != m_oracle.size()) {
        return std::unexpected(
            mismatch(std::format("size {} differs from the oracle's {}", current.size(), m_oracle.size())));
    }
    if (m_buffer.version() != m_expectedVersion || current.version() != m_expectedVersion) {
        return std::unexpected(mismatch(std::format("version {} (snapshot {}) differs from the expected {}",
                                                    m_buffer.version(), current.version(), m_expectedVersion)));
    }
    const std::uint64_t start = change.start.value();
    const std::uint64_t lo = start >= 8 ? start - 8 : 0;
    const std::uint64_t hi = std::min<std::uint64_t>(change.newEnd.value() + 8, m_oracle.size());
    if (lo > hi) {
        return std::unexpected(mismatch(std::format("change {} lies beyond the text", describe(change))));
    }
    const std::string window = current.text(ByteRange{ByteOffset{lo}, ByteOffset{hi}});
    if (window !=
        std::string_view(m_oracle.text()).substr(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo))) {
        return std::unexpected(
            mismatch(std::format("text of [{}, {}) is \"{}\", the oracle's \"{}\"", lo, hi, escape(window),
                                 escape(std::string_view(m_oracle.text()).substr(lo, hi - lo)))));
    }
    if (crlfInterior(m_oracle.text(), start) || crlfInterior(m_oracle.text(), change.newEnd.value())) {
        return std::unexpected(
            mismatch(std::format("change {} has a CRLF-interior bound in the new text", describe(change))));
    }
    return {};
}

void OpDriver::pushSnapshot(ByteSource& source) {
    if (source.next(8) != 0) {
        return;
    }
    RingEntry entry{m_buffer.snapshot(), m_oracle.text()};
    if (m_ring.size() < kRingSize) {
        m_ring.push_back(std::move(entry));
    } else {
        m_ring[m_ringNext] = std::move(entry);
    }
    m_ringNext = (m_ringNext + 1) % kRingSize;
}

Result<void> OpDriver::edit(ByteSource& source, int api, std::uint64_t a, std::uint64_t b, std::string text) {
    static constexpr const char* kKinds[] = {"insert", "erase", "replace"};
    record(kKinds[api], a, b, text);
    const std::uint64_t versionBefore = m_buffer.version();
    const TextSummary summaryBefore = m_buffer.current().summary();
    std::optional<ErrorCode> expected = m_oracle.checkEdit(a, b, text);
    if (!expected && m_oracle.size() - (b - a) + text.size() > m_options.buffer.maxBufferBytes) {
        expected = ErrorCode::OutOfRange;
    }
    const ByteRange range{ByteOffset{a}, ByteOffset{b}};
    const Result<TextChange> result = api == 0   ? m_buffer.insert(ByteOffset{a}, text)
                                      : api == 1 ? m_buffer.erase(range)
                                                 : m_buffer.replace(range, text);
    if (expected) {
        return checkRejected(result, *expected, versionBefore, summaryBefore);
    }
    if (!result) {
        return std::unexpected(mismatch("the edit failed: " + result.error().describe()));
    }
    if (a == b && text.empty()) {
        ++m_stats.noOps;
        const TextChange& c = *result;
        const bool empty = c.start.value() == a && c.oldEnd.value() == a && c.newEnd.value() == a &&
                           c.startPoint == c.oldEndPoint && c.startPoint == c.newEndPoint;
        if (!empty || m_buffer.version() != versionBefore) {
            return std::unexpected(mismatch(std::format("no-op edit returned {} or changed the version", describe(c))));
        }
        return {};
    }
    (api == 0 ? m_stats.inserts : api == 1 ? m_stats.erases : m_stats.replaces) += 1;
    const ByteChange expectedChangeBytes = expectedChange(m_oracle.text(), a, b, text);
    if (auto checked = checkChange(*result, expectedChangeBytes, a, b, m_oracle.text()); !checked) {
        return checked;
    }
    std::string removed =
        m_oracle.text().substr(static_cast<std::size_t>(expectedChangeBytes.start),
                               static_cast<std::size_t>(expectedChangeBytes.oldEnd - expectedChangeBytes.start));
    m_oracle.replace(a, b, text);
    ++m_expectedVersion;
    if (auto window = checkWindow(*result); !window) {
        return window;
    }
    m_steps.clear();
    m_steps.push_back(Step{*result, std::move(removed)});
    pushSnapshot(source);
    return {};
}

Result<void> OpDriver::batch(ByteSource& source) {
    const std::uint64_t n = m_oracle.size();
    const std::size_t count = 1 + static_cast<std::size_t>(source.next(4));
    std::vector<std::uint64_t> cuts(2 * count);
    for (std::uint64_t& cut : cuts) {
        cut = randomBoundary(source);
    }
    std::sort(cuts.begin(), cuts.end());
    const std::uint64_t budget = m_options.maxDocBytes > n ? m_options.maxDocBytes - n : 0;
    std::vector<std::string> texts(count + 1);
    std::vector<Edit> edits;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t a = cuts[2 * i];
        const std::uint64_t b = source.next(3) == 0 ? a : cuts[2 * i + 1];
        texts[i] = (b > a && source.next(4) == 0) ? std::string() : randomText(source, budget / count);
        edits.push_back(Edit{ByteRange{ByteOffset{a}, ByteOffset{b}}, texts[i]});
    }
    // One batch in five breaks the order rule: unsorted, or an insert at a non-empty range's begin.
    if (source.next(5) == 0) {
        if (count >= 2 && source.next(2) == 0) {
            std::swap(edits.front(), edits.back());
        } else {
            for (std::size_t i = 0; i < edits.size(); ++i) {
                if (!edits[i].range.empty()) {
                    texts[count] = "x";
                    edits.insert(edits.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                 Edit{ByteRange{edits[i].range.begin, edits[i].range.begin}, texts[count]});
                    break;
                }
            }
        }
    }
    record("batch", edits.size(), edits.empty() ? 0 : edits.front().range.begin.value(),
           edits.empty() ? std::string_view() : edits.front().text);

    // The oracle's verdict, in the documented order.
    std::optional<ErrorCode> expected;
    std::uint64_t total = n;
    for (std::size_t i = 0; i < edits.size() && !expected; ++i) {
        const std::uint64_t a = edits[i].range.begin.value();
        const std::uint64_t b = edits[i].range.end.value();
        expected = m_oracle.checkEdit(a, b, edits[i].text);
        if (!expected && i > 0 && edits[i - 1].range.end > edits[i].range.begin) {
            expected = ErrorCode::InvalidArgument;
        }
        if (!expected) {
            total = total - (b - a) + edits[i].text.size();
        }
    }
    if (!expected && total > m_options.buffer.maxBufferBytes) {
        expected = ErrorCode::OutOfRange;
    }
    const std::uint64_t versionBefore = m_buffer.version();
    const TextSummary summaryBefore = m_buffer.current().summary();
    const Result<std::vector<TextChange>> result = m_buffer.applyEdits(edits);
    if (expected) {
        ++m_stats.rejected;
        if (result) {
            return std::unexpected(mismatch(std::format("expected {}, but the batch succeeded", toString(*expected))));
        }
        note(std::string(toString(result.error().code())));
        if (result.error().code() != *expected) {
            return std::unexpected(mismatch(std::format("batch: expected {}, got {}: {}", toString(*expected),
                                                        toString(result.error().code()), result.error().message())));
        }
        if (m_buffer.version() != versionBefore || !(m_buffer.current().summary() == summaryBefore)) {
            return std::unexpected(mismatch("a rejected batch changed the version or the summary"));
        }
        return {};
    }
    if (!result) {
        return std::unexpected(mismatch("the batch failed: " + result.error().describe()));
    }
    ++m_stats.batches;
    // Apply to the oracle last to first, checking each step in its own coordinates.
    std::vector<Step> steps;
    std::size_t k = 0;
    for (std::size_t i = edits.size(); i-- > 0;) {
        const std::uint64_t a = edits[i].range.begin.value();
        const std::uint64_t b = edits[i].range.end.value();
        if (a == b && edits[i].text.empty()) {
            continue;
        }
        if (k >= result->size()) {
            return std::unexpected(mismatch(std::format("the batch returned {} changes, too few", result->size())));
        }
        const TextChange& change = (*result)[k++];
        const ByteChange expectedBytes = expectedChange(m_oracle.text(), a, b, edits[i].text);
        if (auto checked = checkChange(change, expectedBytes, a, b, m_oracle.text()); !checked) {
            return checked;
        }
        std::string removed =
            m_oracle.text().substr(static_cast<std::size_t>(expectedBytes.start),
                                   static_cast<std::size_t>(expectedBytes.oldEnd - expectedBytes.start));
        m_oracle.replace(a, b, edits[i].text);
        if (crlfInterior(m_oracle.text(), expectedBytes.start) || crlfInterior(m_oracle.text(), expectedBytes.newEnd)) {
            return std::unexpected(mismatch(std::format("batch step {} has a CRLF-interior bound in its new text", k)));
        }
        steps.push_back(Step{change, std::move(removed)});
    }
    if (k != result->size()) {
        return std::unexpected(mismatch(std::format("the batch returned {} changes, expected {}", result->size(), k)));
    }
    if (!steps.empty()) {
        ++m_expectedVersion;
        if (auto window = checkWindow(steps.back().change); !window) {
            return window;
        }
        m_steps = std::move(steps);
        pushSnapshot(source);
    } else if (m_buffer.version() != versionBefore) {
        return std::unexpected(mismatch("a batch of no-op edits changed the version"));
    }
    return {};
}

Result<void> OpDriver::restore(ByteSource& source) {
    const bool self = m_ring.empty() || source.next(8) == 0;
    const std::size_t index = self ? 0 : static_cast<std::size_t>(source.next(m_ring.size()));
    record(self ? "restore-current" : "restore", index, 0, {});
    // Copy what restore() reads before it runs: restoring current() must work through an alias.
    std::string removed = m_oracle.text();
    std::string target = self ? m_oracle.text() : m_ring[index].text;
    const Result<TextChange> result = m_buffer.restore(self ? m_buffer.current() : m_ring[index].snapshot);
    if (!result) {
        return std::unexpected(mismatch("restore failed: " + result.error().describe()));
    }
    ++m_stats.restores;
    const TextChange& c = *result;
    if (c.start.value() != 0 || c.oldEnd.value() != removed.size() || c.newEnd.value() != target.size() ||
        !(c.startPoint == Point{})) {
        return std::unexpected(mismatch(std::format("restore returned {}", describe(c))));
    }
    m_oracle.setText(std::move(target));
    ++m_expectedVersion;
    if (auto window = checkWindow(TextChange{ByteOffset{0}, ByteOffset{0}, ByteOffset{0}, {}, {}, {}}); !window) {
        return window;
    }
    m_steps.clear();
    m_steps.push_back(Step{c, std::move(removed)});
    return {};
}

Result<void> OpDriver::reload() {
    record("reload", 0, m_oracle.size(), {});
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(m_oracle.text(), m_options.buffer);
    if (!loaded) {
        return std::unexpected(mismatch("reload failed: " + loaded.error().describe()));
    }
    ++m_stats.reloads;
    const Snapshot old = m_buffer.snapshot();
    m_buffer = std::move(*loaded);
    m_expectedVersion = 0;
    m_ring.clear();
    m_ringNext = 0;
    m_steps.clear();
    if (!m_foreignChecked) {
        // The old buffer's snapshots are foreign to the new buffer: once per run.
        m_foreignChecked = true;
        const TextSummary summaryBefore = m_buffer.current().summary();
        const Result<TextChange> foreign = m_buffer.restore(old);
        if (foreign || foreign.error().code() != ErrorCode::InvalidArgument || m_buffer.version() != 0 ||
            !(m_buffer.current().summary() == summaryBefore)) {
            return std::unexpected(mismatch("restoring a snapshot of the previous buffer did not fail cleanly"));
        }
    }
    return checkWindow(TextChange{});
}

Result<void> OpDriver::invalid(ByteSource& source) {
    const std::uint64_t n = m_oracle.size();
    std::uint64_t variant = source.next(6);
    // An offset inside a multi-byte character, searched forward from a random position.
    std::optional<std::uint64_t> inside;
    if (variant == 0 || variant == 4) {
        for (std::uint64_t x = source.next(n + 1), stop = std::min(n, x + 64); x < stop; ++x) {
            if (!m_oracle.isCharBoundary(x)) {
                inside = x;
                break;
            }
        }
        if (!inside) {
            variant = 1;
        }
    }
    const auto rawBytes = [&]() {
        std::string raw(static_cast<std::size_t>(1 + source.next(8)), '\0');
        for (char& c : raw) {
            c = static_cast<char>(source.next(256));
        }
        return raw;
    };
    switch (variant) {
    case 0: // Inside a character: insert there, or a range ending or starting there.
        if (source.next(2) == 0) {
            return edit(source, 0, *inside, *inside, randomText(source, 4));
        }
        return source.next(2) == 0 ? edit(source, 1, m_oracle.floorChar(*inside), *inside, {})
                                   : edit(source, 2, *inside, n, randomText(source, 4));
    case 1: { // Beyond the size.
        const std::uint64_t beyond = n + 1 + source.next(8);
        if (source.next(2) == 0) {
            return edit(source, 0, beyond, beyond, randomText(source, 4));
        }
        return edit(source, 1, randomBoundary(source), beyond, {});
    }
    case 2: { // Reversed, possibly also beyond the size (check 1 comes first).
        const std::uint64_t a = randomBoundary(source);
        const std::uint64_t b = a + 1 + source.next(8);
        return source.next(2) == 0 ? edit(source, 1, b, a, {}) : edit(source, 2, b, a, rawBytes());
    }
    case 3: { // Raw bytes as text; accepted exactly when the oracle's validator accepts them.
        const std::uint64_t a = randomBoundary(source);
        if (source.next(2) == 0) {
            return edit(source, 0, a, a, rawBytes());
        }
        const std::uint64_t b = m_oracle.floorChar(std::min(n, a + source.next(8)));
        return edit(source, 2, a, b, rawBytes());
    }
    case 4: // A bad offset and bad text: the offset is reported first.
        return source.next(2) == 0 ? edit(source, 2, *inside, *inside, rawBytes())
                                   : edit(source, 2, randomBoundary(source), n + 1 + source.next(4), rawBytes());
    default: { // A result longer than maxBufferBytes, when the limit is near.
        const std::uint64_t limit = m_options.buffer.maxBufferBytes;
        if (limit >= n && limit - n < 70'000) {
            const std::uint64_t at = randomBoundary(source);
            return edit(source, 0, at, at, std::string(static_cast<std::size_t>(limit - n + 1), 'a'));
        }
        const std::uint64_t a = randomBoundary(source);
        return edit(source, 0, a, a, rawBytes());
    }
    }
}

Result<void> OpDriver::step(ByteSource& source) {
    ++m_stats.ops;
    const std::uint64_t n = m_oracle.size();
    const std::uint64_t cap = m_options.maxDocBytes;
    const std::uint64_t room = cap > n ? cap - n : 0;
    std::uint64_t roll = source.next(100);
    if (roll < 40 && n >= cap / 4 * 3 && source.next(2) == 0) {
        roll = 40; // Near the cap, half the inserts become erases.
    }
    Result<void> result;
    if (roll < 40) {
        const std::uint64_t at = randomBoundary(source);
        result = edit(source, 0, at, at, randomText(source, room));
    } else if (roll < 65) {
        const std::uint64_t a = randomBoundary(source);
        const std::uint64_t b = m_oracle.floorChar(std::min(n, a + randomLength(source)));
        result = edit(source, 1, a, b, {});
    } else if (roll < 80) {
        const std::uint64_t a = randomBoundary(source);
        const std::uint64_t b = m_oracle.floorChar(std::min(n, a + randomLength(source)));
        result = edit(source, 2, a, b, randomText(source, room + (b - a)));
    } else if (roll < 85) {
        result = batch(source);
    } else if (roll < 88) {
        result = restore(source);
    } else if (roll < 89) {
        result = reload();
    } else if (roll < 95) {
        result = invalid(source);
    } else {
        record("check", 0, 0, {});
        result = verifyFull(false);
    }
    if (!result) {
        return result;
    }
    m_stats.maxBytes = std::max<std::uint64_t>(m_stats.maxBytes, m_oracle.size());
    if (m_options.checkEvery != 0 && m_stats.ops % m_options.checkEvery == 0) {
        return verifyFull(false);
    }
    return {};
}

Result<void> OpDriver::checkQueries(sched::detail::SplitMix64& rng) {
    const Snapshot& s = m_buffer.current();
    const std::string& text = m_oracle.text();
    const std::uint64_t n = text.size();
    const std::uint32_t lines = m_oracle.lineCount();
    const auto below = [&](std::uint64_t bound) { return bound <= 1 ? 0 : rng.below(bound); };
    for (std::uint32_t q = 0; q < m_options.queriesPerCheck; ++q) {
        const std::uint64_t x = below(n + 1);
        const ByteOffset bx{x};
        if (s.pointAt(bx) != m_oracle.pointAt(x) || s.lineAt(bx) != m_oracle.pointAt(x).line) {
            return std::unexpected(mismatch(std::format("pointAt({}) is {}, the oracle's {}", x,
                                                        describe(s.pointAt(bx)), describe(m_oracle.pointAt(x)))));
        }
        if (s.utf16At(bx).value() != m_oracle.utf16At(x)) {
            return std::unexpected(
                mismatch(std::format("utf16At({}) is {}, the oracle's {}", x, s.utf16At(bx), m_oracle.utf16At(x))));
        }
        if (s.utf16PointAt(bx) != m_oracle.utf16PointAt(x)) {
            return std::unexpected(
                mismatch(std::format("utf16PointAt({}) is {}, the oracle's {}", x, describe(s.utf16PointAt(bx)),
                                     describe(m_oracle.utf16PointAt(x)))));
        }
        const std::uint64_t y = below(n + 3);
        if (s.isCharBoundary(ByteOffset{y}) != m_oracle.isCharBoundary(y)) {
            return std::unexpected(mismatch(std::format("isCharBoundary({}) differs", y)));
        }
        const std::uint64_t z = below(n + 10);
        if (s.clip(ByteOffset{z}).value() != m_oracle.clip(z)) {
            return std::unexpected(
                mismatch(std::format("clip({}) is {}, the oracle's {}", z, s.clip(ByteOffset{z}), m_oracle.clip(z))));
        }
        if (n > 0) {
            const std::uint64_t i = below(n);
            if (s.byteAt(ByteOffset{i}) != text[static_cast<std::size_t>(i)]) {
                return std::unexpected(mismatch(std::format("byteAt({}) differs", i)));
            }
        }
        // offsetAt(Point), including lines past the end and columns past the content.
        const std::uint32_t line = static_cast<std::uint32_t>(below(std::uint64_t{lines} + 2));
        const std::optional<LineInfo> info = m_oracle.lineInfo(line);
        const std::uint64_t contentBytes = info ? info->content.size() : 0;
        const Point point{LineIndex{line}, ByteColumn{static_cast<std::uint32_t>(below(contentBytes + 6))}};
        const Result<ByteOffset> byPoint = s.offsetAt(point);
        const std::optional<std::uint64_t> expectedByPoint = m_oracle.offsetAt(point);
        if (byPoint.has_value() != expectedByPoint.has_value() || (byPoint && byPoint->value() != *expectedByPoint) ||
            (!byPoint && byPoint.error().code() != ErrorCode::OutOfRange)) {
            return std::unexpected(mismatch(std::format("offsetAt{} differs from the oracle", describe(point))));
        }
        // offsetAt(Utf16Point).
        const std::uint64_t contentUnits =
            info ? m_oracle.utf16At(info->content.end.value()) - m_oracle.utf16At(info->content.begin.value()) : 0;
        const Utf16Point upoint{LineIndex{line}, Utf16Column{static_cast<std::uint32_t>(below(contentUnits + 6))}};
        const Result<ByteOffset> byUtf16Point = s.offsetAt(upoint);
        const std::optional<std::uint64_t> expectedByUtf16Point = m_oracle.offsetAt(upoint);
        if (byUtf16Point.has_value() != expectedByUtf16Point.has_value() ||
            (byUtf16Point && byUtf16Point->value() != *expectedByUtf16Point) ||
            (!byUtf16Point && byUtf16Point.error().code() != ErrorCode::OutOfRange)) {
            return std::unexpected(
                mismatch(std::format("offsetAt(utf16 {}) differs from the oracle", describe(upoint))));
        }
        // offsetAtUtf16.
        const std::uint64_t units = below(m_oracle.utf16Size() + 3);
        const Result<ByteOffset> byUnits = s.offsetAtUtf16(Utf16Offset{units});
        const std::optional<std::uint64_t> expectedByUnits = m_oracle.offsetAtUtf16(units);
        if (byUnits.has_value() != expectedByUnits.has_value() || (byUnits && byUnits->value() != *expectedByUnits) ||
            (!byUnits && byUnits.error().code() != ErrorCode::OutOfRange)) {
            return std::unexpected(mismatch(std::format("offsetAtUtf16({}) differs from the oracle", units)));
        }
        // lineInfo.
        const Result<LineInfo> gotInfo = s.lineInfo(LineIndex{line});
        if (gotInfo.has_value() != info.has_value() || (gotInfo && !(*gotInfo == *info)) ||
            (!gotInfo && gotInfo.error().code() != ErrorCode::OutOfRange)) {
            return std::unexpected(mismatch(std::format("lineInfo({}) is {}, the oracle's {}", line,
                                                        gotInfo ? describe(*gotInfo) : gotInfo.error().message(),
                                                        info ? describe(*info) : "OutOfRange")));
        }
        // Round-trip rules at x.
        const std::uint64_t floored = m_oracle.crlfFloor(m_oracle.clip(x));
        const Result<ByteOffset> back1 = s.offsetAt(s.pointAt(bx));
        const Result<ByteOffset> back2 = s.offsetAt(s.utf16PointAt(bx));
        const Result<ByteOffset> back3 = s.offsetAtUtf16(s.utf16At(bx));
        if (!back1 || back1->value() != floored || !back2 || back2->value() != floored || !back3 ||
            back3->value() != m_oracle.clip(x)) {
            return std::unexpected(mismatch(std::format("a round-trip rule fails at {}", x)));
        }
        // pointAt(offsetAt(p)) == p for a canonical p.
        const std::uint32_t canonicalLine = static_cast<std::uint32_t>(below(lines));
        const LineInfo canonical = *m_oracle.lineInfo(canonicalLine);
        const std::uint64_t column =
            m_oracle.floorChar(canonical.content.begin.value() + below(canonical.content.size() + 1)) -
            canonical.content.begin.value();
        const Point p{LineIndex{canonicalLine}, ByteColumn{static_cast<std::uint32_t>(column)}};
        const Result<ByteOffset> at = s.offsetAt(p);
        if (!at || s.pointAt(*at) != p) {
            return std::unexpected(mismatch(std::format("pointAt(offsetAt{}) is not the point", describe(p))));
        }
    }

    // summaryOf, chunks and chunkAt on random char-boundary ranges.
    std::uint64_t a = m_oracle.floorChar(below(n + 1));
    std::uint64_t b = m_oracle.floorChar(below(n + 1));
    if (a > b) {
        std::swap(a, b);
    }
    const ByteRange range{ByteOffset{a}, ByteOffset{b}};
    const std::string_view expectedBytes =
        std::string_view(text).substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
    const TextSummary rangeSummary = s.summaryOf(range);
    const TextSummary expectedSummary = oracleSummary(expectedBytes);
    if (!(rangeSummary == expectedSummary)) {
        return std::unexpected(mismatch(std::format("summaryOf([{}, {})) is {}, the oracle's {}", a, b,
                                                    describe(rangeSummary), describe(expectedSummary))));
    }
    std::uint64_t c = m_oracle.floorChar(below(n + 1));
    std::uint64_t d = m_oracle.floorChar(below(n + 1));
    if (c > d) {
        std::swap(c, d);
    }
    std::string joined;
    for (const std::string_view chunk : s.chunks(ByteRange{ByteOffset{c}, ByteOffset{d}})) {
        if (chunk.empty() || !oracleValid(chunk)) {
            return std::unexpected(mismatch(std::format("chunks([{}, {})) yields an empty or ill-formed chunk", c, d)));
        }
        joined += chunk;
    }
    if (joined != std::string_view(text).substr(static_cast<std::size_t>(c), static_cast<std::size_t>(d - c))) {
        return std::unexpected(mismatch(std::format("chunks([{}, {})) do not cover the range exactly", c, d)));
    }
    const std::uint64_t e = below(n + 1);
    const std::string_view rest = s.chunkAt(ByteOffset{e});
    const bool restOk =
        e == n ? rest.empty()
               : !rest.empty() && std::string_view(text).substr(static_cast<std::size_t>(e)).starts_with(rest) &&
                     (!m_oracle.isCharBoundary(e) || (static_cast<unsigned char>(rest[0]) & 0xC0u) != 0x80u);
    if (!restOk) {
        return std::unexpected(mismatch(std::format("chunkAt({}) is wrong", e)));
    }
    return {};
}

Result<void> OpDriver::checkLines(sched::detail::SplitMix64& rng, bool sweepAllLines) {
    const Snapshot& s = m_buffer.current();
    const std::uint32_t lines = m_oracle.lineCount();
    const std::uint32_t first = sweepAllLines || lines <= 1 ? 0 : static_cast<std::uint32_t>(rng.below(lines));
    const std::uint32_t count = sweepAllLines ? lines : std::min<std::uint32_t>(64, lines - first);
    Result<LineCursor> cursor = s.linesFrom(LineIndex{first});
    if (!cursor) {
        return std::unexpected(mismatch("linesFrom failed: " + cursor.error().describe()));
    }
    LineInfo line;
    for (std::uint32_t i = 0; i < count; ++i) {
        const LineInfo expected = *m_oracle.lineInfo(first + i);
        if (!cursor->next(line) || !(line == expected)) {
            return std::unexpected(mismatch(
                std::format("LineCursor from {} gives {}, the oracle {}", first, describe(line), describe(expected))));
        }
    }
    if (first + count == lines && cursor->next(line)) {
        return std::unexpected(mismatch("LineCursor goes past the last line"));
    }
    // The last line on its own, and the first line past the end.
    Result<LineCursor> last = s.linesFrom(LineIndex{lines - 1});
    if (!last || !last->next(line) || !(line == *m_oracle.lineInfo(lines - 1)) || last->next(line)) {
        return std::unexpected(mismatch("LineCursor over the last line is wrong"));
    }
    const Result<LineCursor> beyond = s.linesFrom(LineIndex{lines});
    if (beyond || beyond.error().code() != ErrorCode::OutOfRange) {
        return std::unexpected(mismatch("linesFrom(lineCount()) is not OutOfRange"));
    }
    return {};
}

Result<void> OpDriver::checkSteps() {
    if (m_steps.empty()) {
        return {};
    }
    // Rebuild each step's old and new texts backward from the current text.
    std::vector<std::string> before(m_steps.size());
    std::vector<std::string> after(m_steps.size());
    std::string current = m_oracle.text();
    for (std::size_t k = m_steps.size(); k-- > 0;) {
        const TextChange& c = m_steps[k].change;
        after[k] = current;
        before[k] = current.substr(0, static_cast<std::size_t>(c.start.value())) + m_steps[k].removed +
                    current.substr(static_cast<std::size_t>(c.newEnd.value()));
        current = before[k];
    }
    for (std::size_t k = 0; k < m_steps.size(); ++k) {
        const TextChange& c = m_steps[k].change;
        // Linear scans rather than TextOracle tables: two table builds per step would cost more
        // than the rest of the full check.
        const Point startPoint = linearPointAt(before[k], c.start.value());
        const Point oldEndPoint = linearPointAt(before[k], c.oldEnd.value());
        const Point newEndPoint = linearPointAt(after[k], c.newEnd.value());
        if (c.startPoint != startPoint || c.oldEndPoint != oldEndPoint || c.newEndPoint != newEndPoint) {
            return std::unexpected(
                mismatch(std::format("step {} change {} has points other than the oracle's {} {} {}", k, describe(c),
                                     describe(startPoint), describe(oldEndPoint), describe(newEndPoint))));
        }
        if (crlfInterior(before[k], c.start.value()) || crlfInterior(before[k], c.oldEnd.value()) ||
            crlfInterior(after[k], c.start.value()) || crlfInterior(after[k], c.newEnd.value())) {
            return std::unexpected(
                mismatch(std::format("step {} change {} has a CRLF-interior bound", k, describe(c))));
        }
        // LSP replay: map the points to offsets in the old text with LSP's rules and splice.
        const std::uint64_t from = linearLspOffset(before[k], c.startPoint);
        const std::uint64_t to = linearLspOffset(before[k], c.oldEndPoint);
        const std::string replayed = before[k].substr(0, static_cast<std::size_t>(from)) +
                                     after[k].substr(static_cast<std::size_t>(c.start.value()),
                                                     static_cast<std::size_t>(c.newEnd.value() - c.start.value())) +
                                     before[k].substr(static_cast<std::size_t>(to));
        if (replayed != after[k]) {
            return std::unexpected(
                mismatch(std::format("step {} change {} does not replay as an LSP edit", k, describe(c))));
        }
    }
    m_steps.clear();
    return {};
}

Result<void> OpDriver::verifyFull(bool sweepAllLines) {
    ++m_stats.fullChecks;
    m_stats.checkedBytes += m_oracle.size();
    m_oracle.rebuild();
    const Snapshot& s = m_buffer.current();
    const std::string& text = m_oracle.text();
    if (s.size().value() != text.size()) {
        return std::unexpected(mismatch(std::format("size {} differs from the oracle's {}", s.size(), text.size())));
    }
    if (const std::string got = s.text(); got != text) {
        const auto diff = std::mismatch(got.begin(), got.end(), text.begin(), text.end());
        return std::unexpected(mismatch(std::format("the text differs from byte {}", diff.first - got.begin())));
    }
    if (!(s.summary() == m_oracle.summary())) {
        return std::unexpected(mismatch(std::format("summary() {} differs from the oracle's independent summary {}",
                                                    describe(s.summary()), describe(m_oracle.summary()))));
    }
    if (!(s.summary() == summarize(text))) {
        return std::unexpected(mismatch("summary() differs from text::summarize of the text (consistency check)"));
    }
    // TextBuffer::checkInvariants runs Snapshot::checkInvariants on current() first (P1-P4, S1-S4).
    if (auto invariants = m_buffer.checkInvariants(); !invariants) {
        return std::unexpected(mismatch("TextBuffer::checkInvariants: " + invariants.error().message()));
    }
    if (m_buffer.version() != m_expectedVersion) {
        return std::unexpected(mismatch("the version differs"));
    }
    sched::detail::SplitMix64 rng(m_stats.ops * 0x9E37'79B9'7F4A'7C15ULL + text.size());
    if (auto queries = checkQueries(rng); !queries) {
        return queries;
    }
    if (auto lines = checkLines(rng, sweepAllLines); !lines) {
        return lines;
    }
    if (!m_ring.empty()) {
        const RingEntry& entry = m_ring[static_cast<std::size_t>(rng.below(m_ring.size()))];
        if (entry.snapshot.text() != entry.text) {
            return std::unexpected(mismatch("an older snapshot's text changed"));
        }
    }
    return checkSteps();
}

} // namespace frost::text::test
