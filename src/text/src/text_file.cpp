// SPDX-License-Identifier: Apache-2.0
//
// Decoding and loading text files, the save guard, encoding for save and the status-bar values
// (frost/text/text_file.hpp; ADR 0018, D6, D7, D8, D10, D11, D17 and D18).
/*
 * decodeBytes tries candidates in order (an override, or the detection's ranking) and accepts the
 * first one that decodes without replacements ("valid-first", D6/D7). A valid legacy decode is
 * then checked incrementally: the text is re-encoded in 64 KiB chunks and compared with the file
 * bytes, so a NotRoundTrip load (932's NEC/IBM duplicates) is accepted but marked, without ever
 * holding a full re-encoding. Decoding runs in blocks of options.blockBytes (1 MiB by default)
 * with InputEnd::More (D16), and a candidate whose text exceeds the buffer limit is abandoned
 * after the block that crossed it (D17). The text of a lossy candidate is discarded unless it is
 * the last candidate; when nothing decodes cleanly, the first lossy candidate is decoded again.
 * One decoded text exists at a time, reserved from the first block's ratio (decodeBlocks), so the
 * peak is the input, that text plus about 1/32, and one block's staged text when the first block
 * is representative; the accepted text keeps at most 1/16 + 64 KiB of spare capacity
 * (releaseSlack). The block size never changes the result (TextFileBlocks, and the fuzz target's
 * check (d)).
 *
 * The UTF-8 path is zero-copy: a well-formed payload is reported as payloadIsText, and
 * decodeTextFile moves the caller's string into the buffer (after erasing a BOM in place). The
 * text is validated twice on that path (here and in TextBuffer::fromUtf8), about 15 ms for a
 * 128 MiB file; decodeTextFile counts line breaks on the built buffer rather than the string, which
 * saves two of eolStats' three passes. TODO(M1): m1/large-file adds a single-validation path for mapped originals if
 * its B15 measurement needs one; EncodingPerf.Utf8LoadOverhead bounds the overhead here (D15).
 */

#include "frost/text/text_file.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "unicode_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

namespace frost::text {
namespace {

constexpr std::size_t kRoundTripChunkBytes = 64u * 1024u; // Incremental round-trip check (D17).
constexpr std::size_t kSinkPieceBytes = 64u * 1024u;      // The largest piece a SaveSink receives.
constexpr std::size_t kEncodeSliceBytes = 16u * 1024u;    // UTF-8 per encode step on save.
constexpr std::size_t kFlushBytes = 32u * 1024u;          // Flush threshold of the save scratch.
constexpr std::size_t kTextSlackBytes = 64u * 1024u;      // Fixed slack of a decoded text's capacity.
constexpr std::uint64_t kMaxBufferBytes = 0xFFFF'FFFEULL; // TextBufferOptions' limit (ADR 0013, D6).
constexpr std::uint8_t kOverrideConfidence = 100;

// The name used in messages: encodingName, or "code page N" for pages outside the table.
std::string displayName(Encoding encoding) {
    const std::string_view name = encodingName(encoding);
    if (!name.empty()) {
        return std::string(name);
    }
    return encoding.form == EncodingForm::CodePage ? std::format("code page {}", encoding.codePage)
                                                   : std::string("an invalid encoding");
}

bool isUtf16Or32(EncodingForm form) noexcept {
    return form == EncodingForm::Utf16Le || form == EncodingForm::Utf16Be || form == EncodingForm::Utf32Le ||
           form == EncodingForm::Utf32Be;
}

// The end of the next piece of `utf8` starting at `pos`: at most `limit` bytes, at a character
// boundary, and at least one character.
std::size_t sliceEnd(std::string_view utf8, std::size_t pos, std::size_t limit) noexcept {
    if (utf8.size() - pos <= limit) {
        return utf8.size();
    }
    std::size_t end = detail::floorCharBoundary(utf8, pos + limit);
    if (end == pos) {
        end = pos + detail::utf8SequenceLength(detail::octetAt(utf8, pos));
    }
    return end;
}

/*
 * The most UTF-8 the decoders append for `bytes` input bytes of `form` (bytes): UTF-8, 3 per byte
 * (U+FFFD for each invalid byte); UTF-16, 3 per unit (a surrogate pair gives 4 for 4) plus an odd
 * byte's U+FFFD; UTF-32, 4 per unit plus a partial unit's U+FFFD; code pages, 4 per byte (a unit
 * maps to one scalar or U+FFFD). A memory heuristic, not a contract: a codec that exceeds it only
 * costs a std::string growth.
 */
std::size_t worstCaseText(EncodingForm form, std::size_t bytes) noexcept {
    switch (form) {
    case EncodingForm::Utf16Le:
    case EncodingForm::Utf16Be: return 3 * (bytes / 2) + 3;
    case EncodingForm::Utf32Le:
    case EncodingForm::Utf32Be: return bytes + 3;
    case EncodingForm::CodePage: return 4 * bytes;
    case EncodingForm::Utf8: break;
    }
    return 3 * bytes;
}

/*
 * The capacity to give the text when `needed` bytes of it came from the first `consumed` of
 * `payloadSize` input bytes and the current capacity `capacity` is too small: exactly `needed`
 * once the input is consumed; otherwise the text so far scaled to the whole payload by the ratio so
 * far, plus 1/32 and kTextSlackBytes, at least 9/8 of the current capacity (so a file whose ratio
 * keeps rising regrows O(log n) times), and at most `cap` (beyond it the candidate is abandoned).
 */
std::size_t projectedCapacity(std::size_t needed, std::size_t consumed, std::size_t payloadSize, std::size_t capacity,
                              std::uint64_t cap) noexcept {
    if (consumed >= payloadSize) {
        return needed;
    }
    const double ratio = static_cast<double>(needed) / static_cast<double>(std::max<std::size_t>(consumed, 1));
    const double rest = ratio * static_cast<double>(payloadSize - consumed);
    const double projected = static_cast<double>(needed) + rest + rest / 32.0 + static_cast<double>(kTextSlackBytes);
    std::uint64_t target = projected >= static_cast<double>(cap) ? cap : static_cast<std::uint64_t>(projected);
    target = std::max<std::uint64_t>(target, capacity + capacity / 8);
    target = std::min<std::uint64_t>(target, cap);
    return static_cast<std::size_t>(std::max<std::uint64_t>(target, needed));
}

// Gives an accepted text back its spare capacity when that exceeds 1/16 of the text plus
// kTextSlackBytes (a first block that was not representative), so the buffer that takes the
// string keeps at most that much unused for the document's lifetime. O(n) when it shrinks.
void releaseSlack(std::string& text) noexcept {
    if (text.capacity() - text.size() > text.size() / 16 + kTextSlackBytes) {
        text.shrink_to_fit();
    }
}

enum class BlockOutcome : std::uint8_t { Done, TooLarge };

// Decodes one block of `form` (through `codec` for code pages), appending to `out`; returns the
// bytes consumed.
Result<std::size_t> decodeBlock(EncodingForm form, ITextCodec* codec, std::string_view block, std::string& out,
                                DecodeStats& stats, InputEnd end) noexcept {
    switch (form) {
    case EncodingForm::Utf8: return appendUtf8Lossy(block, out, stats, end);
    case EncodingForm::Utf16Le: return appendUtf16Lossy(block, Endian::Little, out, stats, end);
    case EncodingForm::Utf16Be: return appendUtf16Lossy(block, Endian::Big, out, stats, end);
    case EncodingForm::Utf32Le: return appendUtf32Lossy(block, Endian::Little, out, stats, end);
    case EncodingForm::Utf32Be: return appendUtf32Lossy(block, Endian::Big, out, stats, end);
    case EncodingForm::CodePage: break;
    }
    FROST_ASSERT(codec != nullptr);
    return decodeLegacy(*codec, block, out, stats, end);
}

/*
 * Decodes `payload` as `form` (through `codec` for code pages) into `text` in blocks of
 * `blockBytes` (>= 16, so each block makes progress: a decoder holds back at most 3 bytes at More),
 * one DecodeStats for the stream; stops with TooLarge once the text exceeds `cap` (D17). The buffer
 * `text` held (an earlier candidate's) is released first, so one decoded text exists at a time.
 * Capacity (Budget: B15). std::string's own growth (1.5x in the MSVC STL, 2x in libstdc++) would
 * leave up to half of the capacity unused for the document's lifetime, and a reserve of the
 * payload's size would hold 2x a UTF-16 text and 4x a UTF-32 one. So a block whose worst case
 * (worstCaseText) fits in the spare capacity decodes straight into `text`; any other block decodes
 * into `staged` (O(block)) and is appended after `text` moved, when needed, to a string reserved
 * by hand to projectedCapacity. A file whose first block is representative is therefore reserved
 * once, after its first block, at its text plus about 1/32; only the first block and the last few
 * blocks (while the spare capacity is below a block's worst case) pass through `staged`.
 */
Result<BlockOutcome> decodeBlocks(EncodingForm form, ITextCodec* codec, std::string_view payload,
                                  std::size_t blockBytes, std::uint64_t cap, std::string& text,
                                  DecodeStats& stats) noexcept {
    std::string().swap(text);
    stats = {};
    std::string staged;
    std::size_t pos = 0;
    while (true) {
        const std::size_t blockEnd = pos + std::min(payload.size() - pos, blockBytes);
        const bool last = blockEnd == payload.size();
        const std::string_view block = payload.substr(pos, blockEnd - pos);
        const bool direct = text.capacity() - text.size() >= worstCaseText(form, block.size());
        staged.clear();
        auto consumed =
            decodeBlock(form, codec, block, direct ? text : staged, stats, last ? InputEnd::Final : InputEnd::More);
        if (!consumed) {
            return std::unexpected(std::move(consumed).error());
        }
        pos += *consumed;
        if (!direct) {
            const std::size_t needed = text.size() + staged.size();
            if (needed > cap) {
                return BlockOutcome::TooLarge;
            }
            if (text.capacity() < needed) {
                std::string grown;
                grown.reserve(projectedCapacity(needed, pos, payload.size(), text.capacity(), cap));
                grown.append(text);
                text.swap(grown);
            }
            text.append(staged);
        }
        if (text.size() > cap) {
            return BlockOutcome::TooLarge;
        }
        if (last) {
            return BlockOutcome::Done;
        }
    }
}

// Where a valid legacy decode stops reproducing the file (NotRoundTrip), as file and text offsets
// relative to the payload and the text.
struct Mismatch {
    std::uint64_t file = 0;
    std::uint64_t text = 0;
};

// The character of chunk `c` whose re-encoding covers byte `j` of the chunk's encoding: c is
// re-encoded character by character, once (only for a NotRoundTrip load).
Result<std::size_t> characterCovering(ITextCodec& codec, std::string_view c, std::size_t j) noexcept {
    std::string one;
    std::size_t encodedBefore = 0;
    std::size_t i = 0;
    while (i < c.size()) {
        const std::size_t length = detail::utf8SequenceLength(detail::octetAt(c, i));
        one.clear();
        auto stop = encodeLegacy(codec, c.substr(i, length), one);
        if (!stop) {
            return std::unexpected(std::move(stop).error());
        }
        if (*stop || j < encodedBefore + one.size()) {
            return i;
        }
        encodedBefore += one.size();
        i += length;
    }
    return i;
}

// The incremental round-trip check of a valid legacy decode (D6, D17): nullopt when encoding
// `text` reproduces `payload` exactly.
Result<std::optional<Mismatch>> roundTripMismatch(ITextCodec& codec, std::string_view text,
                                                  std::string_view payload) noexcept {
    std::string encoded;
    std::size_t textPos = 0;
    std::size_t filePos = 0;
    while (textPos < text.size()) {
        const std::size_t end = sliceEnd(text, textPos, kRoundTripChunkBytes);
        const std::string_view c = text.substr(textPos, end - textPos);
        encoded.clear();
        auto stop = encodeLegacy(codec, c, encoded);
        if (!stop) {
            return std::unexpected(std::move(stop).error());
        }
        const std::string_view expected = payload.substr(filePos, std::min(encoded.size(), payload.size() - filePos));
        const std::size_t same = static_cast<std::size_t>(
            std::mismatch(expected.begin(), expected.end(), encoded.begin()).first - expected.begin());
        if (same < encoded.size()) {
            auto at = characterCovering(codec, c, same);
            if (!at) {
                return std::unexpected(std::move(at).error());
            }
            return std::optional<Mismatch>(Mismatch{filePos + same, textPos + *at});
        }
        if (*stop) {
            return std::optional<Mismatch>(Mismatch{filePos + encoded.size(), textPos + **stop});
        }
        filePos += encoded.size();
        textPos = end;
    }
    if (filePos != payload.size()) {
        return std::optional<Mismatch>(Mismatch{filePos, text.size()});
    }
    return std::optional<Mismatch>();
}

std::string lossDescription(const LoadFidelity& fidelity) {
    std::string text = std::format("{} ({} replacements", lossKindName(fidelity.loss), fidelity.replacements);
    if (fidelity.firstLossFileOffset) {
        text += std::format(", first loss at file byte {}", *fidelity.firstLossFileOffset);
    }
    if (fidelity.firstLossTextOffset) {
        text += std::format(", text byte {}", *fidelity.firstLossTextOffset);
    }
    text += ')';
    return text;
}

// A character a code page cannot represent: its document offset and scalar value.
struct Unencodable {
    std::uint64_t offset = 0;
    char32_t scalar = 0;
};

using Deliver = frost::function_ref<Result<void>(std::string_view)>;

// Encodes `text` through `codec` (reversible mappings only), handing the bytes to `deliver` in
// pieces of at most 64 KiB; returns the first unencodable character, if any.
Result<std::optional<Unencodable>> encodeCodePage(const Snapshot& text, ITextCodec& codec, Deliver deliver) noexcept {
    std::string scratch;
    scratch.reserve(kFlushBytes + 2 * kEncodeSliceBytes);
    std::uint64_t base = 0;
    for (const std::string_view chunk : text.chunks(ByteRange{ByteOffset{0}, text.size()})) {
        for (std::size_t pos = 0; pos < chunk.size();) {
            const std::size_t end = sliceEnd(chunk, pos, kEncodeSliceBytes);
            const std::string_view slice = chunk.substr(pos, end - pos);
            auto stop = encodeLegacy(codec, slice, scratch);
            if (!stop) {
                return std::unexpected(std::move(stop).error());
            }
            if (*stop) {
                std::size_t length = 0;
                const char32_t scalar = detail::decodeValidUtf8Scalar(slice, static_cast<std::size_t>(**stop), length);
                return std::optional<Unencodable>(Unencodable{base + pos + **stop, scalar});
            }
            if (scratch.size() >= kFlushBytes) {
                if (auto delivered = deliver(scratch); !delivered) {
                    return std::unexpected(std::move(delivered).error());
                }
                scratch.clear();
            }
            pos = end;
        }
        base += chunk.size();
    }
    if (!scratch.empty()) {
        if (auto delivered = deliver(scratch); !delivered) {
            return std::unexpected(std::move(delivered).error());
        }
    }
    return std::optional<Unencodable>();
}

} // namespace

std::string_view lossKindName(LossKind kind) noexcept {
    switch (kind) {
    case LossKind::None: return "none";
    case LossKind::Replaced: return "replaced";
    case LossKind::NotRoundTrip: return "not-round-trip";
    }
    return {};
}

std::optional<LossKind> findLossKind(std::string_view name) noexcept {
    for (const LossKind kind : {LossKind::None, LossKind::Replaced, LossKind::NotRoundTrip}) {
        if (name == lossKindName(kind)) {
            return kind;
        }
    }
    return std::nullopt;
}

DocumentEncoding DocumentEncoding::convertedTo(Encoding encoding, bool bom) const noexcept {
    FROST_PRE(encoding.isValid() && (!bom || encoding.isUnicode()));
    DocumentEncoding converted = *this;
    converted.format.encoding = encoding;
    converted.format.bom = bom;
    converted.detection = EncodingGuess{encoding, bom, kOverrideConfidence, DetectionSource::UserOverride};
    return converted;
}

DocumentEncoding DocumentEncoding::afterSave() const noexcept {
    DocumentEncoding saved = *this;
    saved.fidelity = LoadFidelity{};
    return saved;
}

namespace {

/*
 * decodeBytes, without its line-break pass when `countEol` is false (eol stays zero and the
 * separator is the default): decodeTextFile counts line breaks on the buffer it builds instead,
 * where eolStats(const Snapshot&) adds one CR pass to TextSummary's counts. Budget: B15 (D15).
 */
Result<DecodedText> decodeImpl(std::string_view bytes, const LoadOptions& options, ITextCodecProvider& codecs,
                               bool countEol) noexcept {
    FROST_PRE(options.buffer.maxBufferBytes <= kMaxBufferBytes);
    FROST_PRE(options.blockBytes >= 16); // A block holds a whole character, so every block progresses.
    const std::uint64_t cap = options.buffer.maxBufferBytes;
    DecodedText result;
    std::vector<EncodingGuess> candidates;
    std::unique_ptr<ITextCodec> overrideCodec;

    // 1. The candidate list.
    if (options.encoding) {
        const Encoding encoding = *options.encoding;
        if (!encoding.isValid()) {
            return fail(ErrorCode::Unsupported, std::format("the override (form {}, code page {}) is not an encoding",
                                                            static_cast<int>(encoding.form), encoding.codePage));
        }
        if (!encoding.isUnicode()) {
            auto opened = codecs.open(encoding.codePage);
            if (!opened) {
                return std::unexpected(
                    std::move(opened).error().withContext(std::format("while decoding as {}", displayName(encoding))));
            }
            overrideCodec = std::move(*opened);
        }
        const bool bom = encoding.isUnicode() && bytes.starts_with(bomBytes(encoding.form));
        candidates.push_back({encoding, bom, kOverrideConfidence, DetectionSource::UserOverride});
        FROST_PRE(options.detect.sampleBytes >= 16);
        const std::string_view sample = bytes.substr(0, std::min(bytes.size(), options.detect.sampleBytes));
        result.binaryLikely = sample.find('\0') != std::string_view::npos && !isUtf16Or32(encoding.form);
    } else {
        EncodingDetection detection = detectEncoding(bytes, options.detect, codecs);
        candidates = std::move(detection.candidates);
        result.binaryLikely = detection.binaryLikely;
    }

    // 2. Valid-first iteration.
    std::optional<std::size_t> accepted;
    std::optional<std::size_t> firstLossy;
    bool lossyTextKept = false;
    std::string text;
    DecodeStats stats;
    for (std::size_t i = 0; i < candidates.size() && !accepted; ++i) {
        const EncodingGuess& guess = candidates[i];
        const std::size_t bomLength = guess.bom ? bomBytes(guess.encoding.form).size() : 0;
        const std::string_view payload = bytes.substr(std::min(bomLength, bytes.size()));
        CandidateAttempt& attempt = result.tried.emplace_back(CandidateAttempt{guess, AttemptOutcome::Accepted, {}});

        // Budget: B15. Valid UTF-8 is accepted without a copy.
        if (guess.encoding.form == EncodingForm::Utf8 && io::validUtf8Prefix(payload) == payload.size()) {
            if (payload.size() > cap) {
                attempt.outcome = AttemptOutcome::TooLarge;
                continue;
            }
            result.payloadIsText = true;
            result.payloadOffset = bomLength;
            accepted = i;
            break;
        }
        std::unique_ptr<ITextCodec> opened;
        ITextCodec* codec = overrideCodec.get();
        if (guess.encoding.form == EncodingForm::CodePage && codec == nullptr) {
            auto open = codecs.open(guess.encoding.codePage);
            if (!open) {
                // Only a missing converter is skipped; any other failure is reported, never hidden.
                if (open.error().code() != ErrorCode::Unsupported) {
                    return std::unexpected(std::move(open).error().withContext(
                        std::format("while decoding as {}", displayName(guess.encoding))));
                }
                attempt.outcome = AttemptOutcome::CodecUnavailable;
                continue;
            }
            opened = std::move(*open);
            codec = opened.get();
        }
        auto decoded = decodeBlocks(guess.encoding.form, codec, payload, options.blockBytes, cap, text, stats);
        if (!decoded) {
            return std::unexpected(std::move(decoded).error());
        }
        if (*decoded == BlockOutcome::TooLarge) {
            attempt.outcome = AttemptOutcome::TooLarge;
            continue;
        }
        if (stats.replacements > 0) {
            attempt.outcome = AttemptOutcome::Lossy;
            attempt.fidelity =
                LoadFidelity{LossKind::Replaced, stats.replacements, bomLength + stats.firstInvalidInput.value_or(0),
                             ByteOffset{stats.firstReplacementOutput.value_or(0)}};
            if (!firstLossy) {
                firstLossy = i;
                if (i + 1 == candidates.size()) {
                    releaseSlack(text);
                    result.text = std::move(text); // The last candidate: no decode again (D17).
                    lossyTextKept = true;
                }
            }
            continue;
        }
        if (guess.encoding.form == EncodingForm::CodePage) {
            auto mismatch = roundTripMismatch(*codec, text, payload);
            if (!mismatch) {
                return std::unexpected(std::move(mismatch).error());
            }
            if (*mismatch) {
                attempt.fidelity = LoadFidelity{LossKind::NotRoundTrip, 0, bomLength + (*mismatch)->file,
                                                ByteOffset{(*mismatch)->text}};
            }
        }
        releaseSlack(text);
        result.text = std::move(text);
        accepted = i;
    }

    // 3. Nothing valid: the first lossy candidate, decoded again unless its text was kept. It
    // decodes into `text`, so the last candidate's buffer is released first (D17).
    if (!accepted) {
        if (!firstLossy) {
            return fail(ErrorCode::OutOfRange,
                        std::format("the decoded text of every candidate encoding exceeds the buffer limit of {} "
                                    "bytes (the file has {} bytes)",
                                    cap, bytes.size()));
        }
        const std::size_t j = *firstLossy;
        const EncodingGuess& guess = candidates[j];
        if (!lossyTextKept) {
            const std::size_t bomLength = guess.bom ? bomBytes(guess.encoding.form).size() : 0;
            const std::string_view payload = bytes.substr(std::min(bomLength, bytes.size()));
            std::unique_ptr<ITextCodec> opened;
            ITextCodec* codec = overrideCodec.get();
            if (guess.encoding.form == EncodingForm::CodePage && codec == nullptr) {
                auto open = codecs.open(guess.encoding.codePage);
                if (!open) {
                    return std::unexpected(std::move(open).error().withContext(
                        std::format("while decoding as {}", displayName(guess.encoding))));
                }
                opened = std::move(*open);
                codec = opened.get();
            }
            auto decoded = decodeBlocks(guess.encoding.form, codec, payload, options.blockBytes, cap, text, stats);
            if (!decoded) {
                return std::unexpected(std::move(decoded).error());
            }
            if (*decoded == BlockOutcome::TooLarge) {
                return fail(ErrorCode::Internal, "a lossy candidate grew beyond the buffer limit when decoded again");
            }
            releaseSlack(text);
            result.text = std::move(text);
        }
        result.tried[j].outcome = AttemptOutcome::Accepted;
        accepted = j;
    }

    // 4-5. Line breaks, the format and the detection record.
    const CandidateAttempt& winner = result.tried[*accepted];
    result.payloadOffset = winner.guess.bom ? bomBytes(winner.guess.encoding.form).size() : 0;
    if (countEol) {
        const std::string_view decodedText =
            result.payloadIsText ? bytes.substr(result.payloadOffset) : std::string_view(result.text);
        result.eol = eolStats(decodedText);
    }
    result.encoding.format = TextFileFormat{winner.guess.encoding, winner.guess.bom,
                                            dominantSeparator(result.eol, options.defaultSeparator)};
    result.encoding.fidelity = winner.fidelity;
    result.encoding.detection = winner.guess;
    return result;
}

} // namespace

Result<DecodedText> decodeBytes(std::string_view bytes, const LoadOptions& options,
                                ITextCodecProvider& codecs) noexcept {
    return decodeImpl(bytes, options, codecs, true);
}

Result<LoadedText> decodeTextFile(std::string bytes, const LoadOptions& options, ITextCodecProvider& codecs) noexcept {
    auto decoded = decodeImpl(bytes, options, codecs, false);
    if (!decoded) {
        return std::unexpected(std::move(decoded).error());
    }
    DecodedText& d = *decoded;
    Result<TextBuffer> buffer = [&]() -> Result<TextBuffer> {
        if (d.payloadIsText) {
            // Budget: B15. The caller's string moves into the buffer; a BOM is erased in place.
            bytes.erase(0, d.payloadOffset);
            return TextBuffer::fromUtf8(std::move(bytes), options.buffer);
        }
        return TextBuffer::fromUtf8(std::move(d.text), options.buffer);
    }();
    if (!buffer) {
        if (buffer.error().code() == ErrorCode::InvalidArgument) {
            return fail(ErrorCode::Internal,
                        std::format("a decoder produced ill-formed UTF-8 for {}: {}",
                                    displayName(d.encoding.format.encoding), buffer.error().message()));
        }
        return std::unexpected(std::move(buffer).error());
    }
    // Budget: B15 (D15). One CR pass: the buffer's summary already counts line feeds and breaks.
    d.eol = eolStats(buffer->current());
    d.encoding.format.lineSeparator = dominantSeparator(d.eol, options.defaultSeparator);
    return LoadedText{std::move(*buffer), d.encoding, d.eol, d.binaryLikely, std::move(d.tried)};
}

Result<LoadedText> loadTextFile(io::IFileSystem& fs, std::string_view path, const LoadOptions& options,
                                ITextCodecProvider& codecs) noexcept {
    const std::string context = std::format("while loading {}", path);
    auto status = fs.status(path);
    if (!status) {
        return std::unexpected(std::move(status).error().withContext(context));
    }
    const auto tooLarge = [&](std::uint64_t size) {
        return fail(ErrorCode::OutOfRange,
                    std::format("the file has {} bytes, more than the limit {}", size, options.maxFileBytes))
            .error()
            .withContext(context);
    };
    if (status->size > options.maxFileBytes) {
        return std::unexpected(tooLarge(status->size));
    }
    auto bytes = fs.readFile(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().withContext(context));
    }
    if (bytes->size() > options.maxFileBytes) {
        return std::unexpected(tooLarge(bytes->size()));
    }
    auto loaded = decodeTextFile(std::move(*bytes), options, codecs);
    if (!loaded) {
        return std::unexpected(std::move(loaded).error().withContext(context));
    }
    return loaded;
}

Result<void> encodeForSaveTo(const Snapshot& text, const DocumentEncoding& state, SaveConsent consent, SaveSink sink,
                             ITextCodecProvider& codecs) noexcept {
    FROST_PRE(state.format.isValid());
    const Encoding encoding = state.format.encoding;
    if (!state.fidelity.lossless() && consent == SaveConsent::None) {
        return fail(ErrorCode::Corrupt,
                    std::format("saving would not restore the original bytes: the file was loaded as {} with loss {}; "
                                "save with consent (SaveConsent::AcceptLoss) or convert it to a Unicode encoding",
                                displayName(encoding), lossDescription(state.fidelity)));
    }
    std::unique_ptr<ITextCodec> codec;
    if (!encoding.isUnicode()) {
        auto opened = codecs.open(encoding.codePage);
        if (!opened) {
            return std::unexpected(
                std::move(opened).error().withContext(std::format("while saving as {}", displayName(encoding))));
        }
        codec = std::move(*opened);
    }
    // Hands bytes to the sink in pieces of at most 64 KiB; sink errors gain "while saving".
    const auto deliverImpl = [&sink](std::string_view bytes) -> Result<void> {
        for (std::size_t pos = 0; pos < bytes.size(); pos += kSinkPieceBytes) {
            auto delivered = sink(bytes.substr(pos, kSinkPieceBytes));
            if (!delivered) {
                return std::unexpected(std::move(delivered).error().withContext("while saving"));
            }
        }
        return {};
    };
    const Deliver deliver = deliverImpl;
    if (state.format.bom) {
        if (auto delivered = deliver(bomBytes(encoding.form)); !delivered) {
            return delivered;
        }
    }
    const ByteRange whole{ByteOffset{0}, text.size()};
    switch (encoding.form) {
    case EncodingForm::Utf8:
        for (const std::string_view chunk : text.chunks(whole)) {
            if (auto delivered = deliver(chunk); !delivered) {
                return delivered;
            }
        }
        return {};
    case EncodingForm::Utf16Le:
    case EncodingForm::Utf16Be:
    case EncodingForm::Utf32Le:
    case EncodingForm::Utf32Be: {
        const bool wide = encoding.form == EncodingForm::Utf32Le || encoding.form == EncodingForm::Utf32Be;
        const Endian order = encoding.form == EncodingForm::Utf16Le || encoding.form == EncodingForm::Utf32Le
                                 ? Endian::Little
                                 : Endian::Big;
        std::string scratch;
        scratch.reserve(kFlushBytes + 4 * kEncodeSliceBytes);
        for (const std::string_view chunk : text.chunks(whole)) {
            for (std::size_t pos = 0; pos < chunk.size();) {
                const std::size_t end = sliceEnd(chunk, pos, kEncodeSliceBytes / 2);
                if (wide) {
                    appendUtf32(chunk.substr(pos, end - pos), order, scratch);
                } else {
                    appendUtf16(chunk.substr(pos, end - pos), order, scratch);
                }
                if (scratch.size() >= kFlushBytes) {
                    if (auto delivered = deliver(scratch); !delivered) {
                        return delivered;
                    }
                    scratch.clear();
                }
                pos = end;
            }
        }
        if (!scratch.empty()) {
            return deliver(scratch);
        }
        return {};
    }
    case EncodingForm::CodePage: break;
    }
    auto stop = encodeCodePage(text, *codec, deliver);
    if (!stop) {
        return std::unexpected(std::move(stop).error());
    }
    if (*stop) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("U+{:04X} at byte {} has no reversible mapping in {}",
                                static_cast<std::uint32_t>((*stop)->scalar), (*stop)->offset, displayName(encoding)));
    }
    return {};
}

Result<std::string> encodeForSave(const Snapshot& text, const DocumentEncoding& state, SaveConsent consent,
                                  ITextCodecProvider& codecs) noexcept {
    std::string out;
    const TextSummary& summary = text.summary();
    switch (state.format.encoding.form) {
    case EncodingForm::Utf8: out.reserve(static_cast<std::size_t>(summary.bytes) + 3); break;
    case EncodingForm::Utf16Le:
    case EncodingForm::Utf16Be: out.reserve(static_cast<std::size_t>(2 * summary.utf16) + 2); break;
    case EncodingForm::Utf32Le:
    case EncodingForm::Utf32Be: out.reserve(static_cast<std::size_t>(4 * summary.chars) + 4); break;
    case EncodingForm::CodePage: out.reserve(static_cast<std::size_t>(summary.bytes)); break;
    }
    const auto append = [&out](std::string_view bytes) -> Result<void> {
        out.append(bytes);
        return {};
    };
    if (auto encoded = encodeForSaveTo(text, state, consent, append, codecs); !encoded) {
        return std::unexpected(std::move(encoded).error());
    }
    return out;
}

Result<std::optional<ByteOffset>> firstUnencodable(const Snapshot& text, Encoding encoding,
                                                   ITextCodecProvider& codecs) noexcept {
    if (!encoding.isValid()) {
        return fail(ErrorCode::Unsupported, "firstUnencodable: not a valid encoding");
    }
    if (encoding.isUnicode()) {
        return std::optional<ByteOffset>();
    }
    auto opened = codecs.open(encoding.codePage);
    if (!opened) {
        return std::unexpected(std::move(opened).error());
    }
    const auto discard = [](std::string_view) -> Result<void> { return {}; };
    auto stop = encodeCodePage(text, **opened, discard);
    if (!stop) {
        return std::unexpected(std::move(stop).error());
    }
    if (*stop) {
        return std::optional<ByteOffset>(ByteOffset{(*stop)->offset});
    }
    return std::optional<ByteOffset>();
}

FormatStatus formatStatus(const DocumentEncoding& state, const EolStats& eol) noexcept {
    return FormatStatus{encodingName(state.format.encoding),
                        state.format.bom,
                        state.fidelity.loss,
                        state.detection.source,
                        state.detection.confidence,
                        separatorName(state.format.lineSeparator),
                        eol.mixed()};
}

} // namespace frost::text
