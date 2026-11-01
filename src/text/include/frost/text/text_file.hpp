// SPDX-License-Identifier: Apache-2.0
//
// Loading and saving text files in frost::text (ADR 0018): decoding a file's bytes to the UTF-8 a
// TextBuffer holds, with detection, BOM and line-separator handling and a fidelity record; the
// save guard that stops a lossy load from being written back silently; encoding for save; and the
// values the status bar shows.
/*
 * Data safety (PLAN.md section 2.2): a load is "lossless" when encoding the loaded text with the
 * loaded format reproduces the file byte for byte (D6). A load that is not lossless (ill-formed
 * bytes became U+FFFD, or a valid legacy decode does not re-encode to the same bytes) marks the
 * document, and encodeForSave refuses it with Corrupt until the user consents or converts. BOMs are
 * kept through TextFileFormat::bom (D8), and existing line breaks are never rewritten (D9).
 *
 * The flow, for SaveManager (m1/save-manager) and the large-file loader (m1/large-file):
 *   load:  loadTextFile, or decodeBytes on a mapped view, then a TextBuffer;
 *   save:  encodeForSave / encodeForSaveTo, write atomically, then DocumentEncoding::afterSave.
 */
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/io/file_system.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text {

// The input bytes decodeBytes decodes per block by default (LoadOptions::blockBytes; D16, D17).
inline constexpr std::size_t kDecodeBlockBytes = std::size_t{1} << 20;

// How a document is written back. A plain value.
struct TextFileFormat {
    Encoding encoding;
    bool bom = false;
    LineSeparator lineSeparator = LineSeparator::Lf; // For new lines. Existing breaks are never rewritten (D9).

    // Encoding's invariant, a separator enumerator, and !bom || encoding.isUnicode(). Validate
    // formats read from the journal or settings with it; encodeForSave's precondition. O(1).
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return encoding.isValid() && lineSeparator <= LineSeparator::Cr && (!bom || encoding.isUnicode());
    }

    friend constexpr bool operator==(const TextFileFormat&, const TextFileFormat&) noexcept = default;
};

// How a load lost information. The enumerator values are stable and append-only (persisted by name).
enum class LossKind : std::uint8_t {
    None,         // encode(decode(bytes)) == bytes.
    Replaced,     // Ill-formed input became U+FFFD.
    NotRoundTrip, // The decode was valid but re-encoding differs (932's NEC/IBM duplicates).
};

// Persistence names "none", "replaced", "not-round-trip"; "" outside the enumeration. O(1).
[[nodiscard]] std::string_view lossKindName(LossKind kind) noexcept;
// The exact (case-sensitive) inverse of lossKindName. O(1).
[[nodiscard]] std::optional<LossKind> findLossKind(std::string_view name) noexcept;

// What the load found (D6). Lossless means that encoding the loaded text with the loaded format
// reproduces the file byte for byte. A plain value.
struct LoadFidelity {
    LossKind loss = LossKind::None;
    std::uint64_t replacements = 0;
    // The file byte offset of the first replaced byte, or of the first byte the re-encoding does
    // not reproduce.
    std::optional<std::uint64_t> firstLossFileOffset;
    // The text offset of the first U+FFFD (Replaced), or of the first character whose re-encoding
    // differs (NotRoundTrip).
    std::optional<ByteOffset> firstLossTextOffset;

    // True when saving with the loaded format reproduces the file. O(1).
    [[nodiscard]] constexpr bool lossless() const noexcept { return loss == LossKind::None; }

    friend constexpr bool operator==(const LoadFidelity&, const LoadFidelity&) noexcept = default;
};

/*
 * Everything a document keeps about its file encoding. A plain value.
 * Persisted fields (m1/save-manager's journal, D18): all of them, so that a lossy document is
 * still guarded after a crash:
 *   - format: encodingName, bom, separatorName;
 *   - fidelity: lossKindName, replacements, both offsets;
 *   - detection: encodingName, bom, confidence, detectionSourceName.
 * Restoring code validates with the find* functions and TextFileFormat::isValid. An entry that
 * fails is m1/save-manager's to handle; the conservative choice is to keep the text and treat the
 * document as lossy.
 */
struct DocumentEncoding {
    TextFileFormat format;
    LoadFidelity fidelity;
    EncodingGuess detection;

    // "Convert to": the format takes `encoding` and `bom`, detection becomes {encoding, bom, 100,
    // UserOverride}, and fidelity is unchanged, so a lossy load stays guarded. Precondition
    // (FROST_PRE): encoding.isValid() && (!bom || encoding.isUnicode()). O(1).
    [[nodiscard]] DocumentEncoding convertedTo(Encoding encoding, bool bom) const noexcept;
    // Call only after the encodeForSave bytes reached the disk: fidelity becomes lossless, because
    // the file now equals the encoded text. O(1).
    [[nodiscard]] DocumentEncoding afterSave() const noexcept;

    friend bool operator==(const DocumentEncoding&, const DocumentEncoding&) noexcept = default;
};

// Load options. The defaults suit production; tests set detect.legacyCodePage and
// defaultSeparator explicitly so results do not depend on the machine.
struct LoadOptions {
    // User override ("Reload in"): no detection, and a BOM of the override's own form is
    // stripped. A value that breaks Encoding's invariant is Unsupported.
    std::optional<Encoding> encoding;
    DetectOptions detect;
    // For files without line breaks.
    LineSeparator defaultSeparator = platformDefaultSeparator();
    // maxBufferBytes also caps the decoded text while decoding (D17).
    TextBufferOptions buffer;
    // loadTextFile refuses larger files with OutOfRange before reading them (D17).
    std::uint64_t maxFileBytes = 0xFFFF'FFFE;
    // Input bytes per decode block (D16, D17); the result does not depend on it. FROST_PRE: >= 16,
    // so a block always holds a whole character. A test and fuzz knob, like detect.sampleBytes.
    std::size_t blockBytes = kDecodeBlockBytes;
};

// What happened to one candidate the load tried (in order). Exactly one attempt is Accepted.
enum class AttemptOutcome : std::uint8_t {
    Accepted,         // Its text was taken (valid, or the lossy fallback when nothing was valid).
    Lossy,            // It decoded with replacements and a later candidate was tried.
    CodecUnavailable, // `codecs` reported Unsupported for its code page (detected candidates only).
    TooLarge,         // Its text grew beyond options.buffer.maxBufferBytes.
};

// One tried candidate, for the UI: "opened as windows-1252 because UTF-8 failed at byte N".
struct CandidateAttempt {
    EncodingGuess guess;
    AttemptOutcome outcome = AttemptOutcome::Accepted;
    LoadFidelity fidelity; // For Accepted and Lossy.

    friend bool operator==(const CandidateAttempt&, const CandidateAttempt&) noexcept = default;
};

/*
 * The result of decodeBytes.
 * Zero copy: when the accepted candidate is UTF-8 and the payload (the bytes after a BOM) is
 * well-formed, payloadIsText is true, `text` is empty, and the text is bytes.substr(payloadOffset).
 * Otherwise `text` holds the decoded UTF-8.
 */
struct DecodedText {
    bool payloadIsText = false;
    std::size_t payloadOffset = 0; // The accepted candidate's BOM length (0, 2, 3 or 4).
    std::string text;
    DocumentEncoding encoding;
    EolStats eol; // Of the text.
    bool binaryLikely = false;
    std::vector<CandidateAttempt> tried;
};

/*
 * Decodes the bytes of a whole file without building a buffer (ADR 0018, D6, D7 and D17), for
 * m1/large-file's mapped views and for decodeTextFile.
 *   - With an override: decodes with it.
 *   - Otherwise: tries the detection's candidates in order and accepts the first one that decodes
 *     without replacements. If that one does not round-trip (NotRoundTrip), it is still accepted
 *     and the document is marked. If no candidate is valid, it takes the first lossy candidate,
 *     re-decoded, and the document is marked Replaced.
 * Decoding runs in blocks of options.blockBytes (1 MiB by default), and the result is the same for
 * every block size; a candidate whose text exceeds options.buffer.maxBufferBytes is abandoned
 * (TooLarge) after the block that crossed the limit. A detected candidate whose code page the
 * provider reports Unsupported is skipped (CodecUnavailable). Reads `bytes` only during the call.
 * Precondition (FROST_PRE): options.blockBytes >= 16 and options.detect.sampleBytes >= 16.
 * Errors:
 *   - Unsupported: the override names an unsupported or unavailable code page;
 *   - OutOfRange: every candidate's text exceeds options.buffer.maxBufferBytes;
 *   - Internal: a codec produced ill-formed UTF-8 or broke its contract, or a platform failure
 *     (codecs report those as Internal; the drivers turn any other code from a codec into
 *     Internal);
 *   - any other error from opening a candidate's codec, with the context "while decoding as
 *     <name>": only Unsupported is skipped.
 * Ill-formed input is never an error.
 * Memory: one decoded text exists at a time. Its capacity is projected from the first block's
 * ratio of text to input, so the peak is the input, the text plus about 1/32 and one block's
 * staged text when the first block is representative of the file; otherwise a later block regrows
 * the text (old and new buffer together, at least 9/8 apart) or the projection overshoots (at most
 * by the ratio range of the form, transiently). The returned `text` keeps at most 1/16 of its size
 * plus 64 KiB of spare capacity.
 * Cost: O(n) per candidate tried (at most 4), typically one validation or decode. Thread-safe
 * given a thread-safe provider. Budget: B15 (the UTF-8 path).
 */
[[nodiscard]] Result<DecodedText> decodeBytes(std::string_view bytes, const LoadOptions& options = {},
                                              ITextCodecProvider& codecs = platformCodecs()) noexcept;

// A loaded document: the buffer and what the load found.
struct LoadedText {
    TextBuffer buffer; // Version 0, the decoded text.
    DocumentEncoding encoding;
    EolStats eol; // Of the loaded text.
    bool binaryLikely = false;
    std::vector<CandidateAttempt> tried;
};

/*
 * decodeBytes, then TextBuffer::fromUtf8. Takes ownership of `bytes`. Valid UTF-8 without a BOM
 * moves into the buffer without a copy; with a BOM, the bytes after it move down in place (one
 * O(n) memmove, no second buffer). Other texts move into the buffer with decodeBytes' capacity
 * bound. The result equals decodeBytes' (eol included), but line breaks are counted on the built
 * buffer.
 * Errors: decodeBytes' errors; OutOfRange from fromUtf8; InvalidArgument from fromUtf8 becomes
 * Internal ("a decoder produced ill-formed UTF-8 for <name>"). Thread-safe given a thread-safe
 * provider. O(n). Budget: B15 (the UTF-8 path).
 */
[[nodiscard]] Result<LoadedText> decodeTextFile(std::string bytes, const LoadOptions& options = {},
                                                ITextCodecProvider& codecs = platformCodecs()) noexcept;

/*
 * fs.status(path), then fs.readFile(path), then decodeTextFile. A file larger than
 * options.maxFileBytes is refused with OutOfRange before it is read ("the file has N bytes, more
 * than the limit M"), and the size is checked again after the read. fs errors keep their code,
 * with the context "while loading <path>". A file that grows between status and read can still be
 * read whole: TODO(M1): m1/large-file's mapping API bounds that. Thread safety: as `fs` and
 * `codecs`. O(n).
 */
[[nodiscard]] Result<LoadedText> loadTextFile(io::IFileSystem& fs, std::string_view path,
                                              const LoadOptions& options = {},
                                              ITextCodecProvider& codecs = platformCodecs()) noexcept;

// Whether the user accepted that saving does not restore the loaded bytes.
enum class SaveConsent : std::uint8_t { None, AcceptLoss };

// Receives encoded bytes in order, in pieces of at most 64 KiB. It must not throw. An error it
// returns stops the encode and is returned with the context "while saving".
using SaveSink = frost::function_ref<Result<void>(std::string_view bytes)>;

/*
 * Streams the bytes to write for `text`: the BOM if format.bom, then the encoded text. Line breaks
 * are written exactly as they are in the text. Precondition (FROST_PRE): state.format.isValid();
 * the journal validates restored formats first.
 * Errors. Before the first sink call:
 *   - Corrupt: !state.fidelity.lossless() && consent == None. The message gives the kind, the
 *     replacement count and the first offsets ("saving would not restore the original bytes");
 *   - Unsupported: the code page has no codec here.
 * After some bytes were delivered (the caller discards them; SaveManager writes to a temporary
 * file):
 *   - InvalidArgument: a character has no reversible mapping in the encoding. The message gives
 *     U+XXXX, the document byte offset and the encoding's name. A Replaced legacy document hits
 *     this for its U+FFFD, so it can only be saved after convertedTo(a Unicode form);
 *   - the sink's error;
 *   - Internal: a codec broke its contract.
 * UI mapping: Corrupt asks for consent, InvalidArgument offers a conversion (with firstUnencodable
 * for the position), and Unsupported names the missing codec. Never substitutes a character and
 * never applies best fit (D10). Thread-safe given a thread-safe provider. O(n) over
 * snapshot.chunks().
 */
[[nodiscard]] Result<void> encodeForSaveTo(const Snapshot& text, const DocumentEncoding& state, SaveConsent consent,
                                           SaveSink sink, ITextCodecProvider& codecs = platformCodecs()) noexcept;
// encodeForSaveTo into one string, reserved from O(1) summaries (bytes, UTF-16 units, chars). The
// same precondition and errors; no partial result. O(n).
[[nodiscard]] Result<std::string> encodeForSave(const Snapshot& text, const DocumentEncoding& state,
                                                SaveConsent consent = SaveConsent::None,
                                                ITextCodecProvider& codecs = platformCodecs()) noexcept;
// The document byte offset of the first character `encoding` cannot represent reversibly, or
// nullopt. Always nullopt for Unicode forms. Errors: Unsupported (no codec, or an invalid
// encoding value). O(n).
[[nodiscard]] Result<std::optional<ByteOffset>>
firstUnencodable(const Snapshot& text, Encoding encoding, ITextCodecProvider& codecs = platformCodecs()) noexcept;

/*
 * The values the status bar shows (row 242; m1/shell-frame draws them). The string views point at
 * static storage. Nothing here is translated: encoding names and LF/CRLF/CR are technical tokens,
 * and tooltip sentences go through frost::i18n::tr in the UI (D11). A plain value.
 */
struct FormatStatus {
    std::string_view encodingName;  // encodingName(format.encoding), e.g. "UTF-8", "Shift_JIS".
    bool bom = false;               // The UI renders it ("UTF-8 BOM").
    LossKind loss = LossKind::None; // Replaced: saving needs consent (U+FFFD present); NotRoundTrip: saving re-encodes
                                    // some characters.
    DetectionSource source = DetectionSource::Empty;
    std::uint8_t confidence = 0;
    std::string_view lineSeparatorName; // separatorName(format.lineSeparator), the separator for new lines.
    bool mixedLineSeparators = false;   // From `eol` (the caller refreshes it when idle).

    friend constexpr bool operator==(const FormatStatus&, const FormatStatus&) noexcept = default;
};

// The status-bar values of a document. O(1). Budget: B5.
[[nodiscard]] FormatStatus formatStatus(const DocumentEncoding& state, const EolStats& eol) noexcept;

} // namespace frost::text
