# ADR 0018: Text encodings, BOMs and line separators (frost::text)

- Status: accepted for `m1/encodings`. No budget, test or warning is weakened; `tests/perf/budgets.json` is
  unchanged. A B15 sub-budget for in-memory loads is recorded here (D15).
- Date: 2026-11-01
- Amended: 2026-11-01 by ADR 0029 (m1/large-file is m15/large-file, so its to-dos here are `TODO(M15)`;
  nothing else changes)
- Plan reference: PLAN.md section 3's "Text buffer" row (UTF-8 storage); section 2.2 "Data safety" (SaveManager
  preserves BOM, EOL and encoding); section 7 M1 scope ("encodings") and exit ("byte-exact CRLF/BOM"); section 6.1
  (fuzz targets, golden and differential tests); budgets B5 and B15; feature matrix row 60.

## Context
frost::text stores well-formed UTF-8 only (ADR 0013, D1). Files on disk are UTF-8 with or without a BOM,
UTF-16/32 in either byte order, or a legacy Windows code page, and the M1 exit asks for byte-exact round trips of
CRLF and BOM files. Every load therefore needs a decision (which encoding, how sure, was anything lost) and every
save the inverse, with a guard so that a lossy load is never written back silently. The same decoders serve M4's
build and terminal output, which arrives in blocks.

Constraints: no new dependency (ICU, libiconv on Windows and uchardet are each excluded below); Windows uses wide
Win32 APIs with explicit code page numbers (the UTF-8 manifest makes `GetACP()` return 65001 in our processes,
so `CP_ACP` is never used); Linux uses glibc's iconv; both platforms must agree byte for byte, which the probe
below shows they do only after two normalisation rules (D4).

## Decision
1. **D1, placement.** Encodings live in frost::text (`encoding.hpp`, `unicode_codec.hpp`, `text_codec.hpp`,
   `line_separator.hpp`, `encoding_detection.hpp`, `text_file.hpp`). The Win32 calls are confined to
   `codec_win.cpp` and the iconv calls to `codec_posix.cpp`. M4's build drivers and the terminal may use
   `ITextCodec`, `decodeLegacy` with `InputEnd::More` and `Encoding::fromCodePageId`. `TODO(M4)`: move the seam to
   frost::io if a module below text needs it.
2. **D2, identity and the table.** `Encoding = {form, Windows code page id}`, the id 0 for Unicode forms and a
   table entry for `CodePage`; external ids go through `fromCodePageId`. The 16 M1 code pages: 437, 850, 866, 874,
   932, 1250, 1251, 1252, 1253, 1254, 1256, 1257, 20866 (KOI8-R), 28591, 28592 and 28605, each with a name, lower
   case aliases, its iconv name, DBCS lead-byte ranges (932: 81-9F, E0-FC) and its `windowsOnlyC1` list. Names
   follow the WHATWG Encoding Standard where it has the encoding, with one divergence: "ISO-8859-1" is the real
   ISO-8859-1 (28591), not WHATWG's label for windows-1252. Aliases are WHATWG labels minus those that would name
   a different page ("latin1", which EditorConfig reads as ISO-8859-1 and WHATWG as windows-1252, and "ascii"),
   plus the common iconv spellings (`sjis`, `ms932`, `iso8859-n`, `koi8r`); WHATWG has neither 437 nor 850, so
   their aliases are the IANA character-set registry's (`cspc8codepage437`, `cspc850multilingual`, `437`, `850`).
   `findEncoding` also accepts `cp<N>` for table entries. Excluded, with the probe's evidence (F4): 1255 and 1258 (glibc's converters compose base characters with combining marks), 936, 949 and 950
   (2149, 189 and 5817 Windows-only EUDC/PUA mappings), KOI8-U (2 mapping differences) and every stateful encoding
   (ISO-2022, UTF-7, EBCDIC).
3. **D3, strict platform codecs and a narrow seam.** `ITextCodec` decodes and encodes strictly: Windows with
   `MB_ERR_INVALID_CHARS` and `WC_NO_BEST_FIT_CHARS` plus `lpUsedDefaultChar`, Linux with plain iconv. The shared
   driver (`decodeLegacy`, `encodeLegacy`) owns the replacement policy, so both OSes replace identically: one
   U+FFFD per invalid unit; a lead byte followed by a byte >= 0x80 consumes 2; a lead byte followed by an ASCII
   byte consumes 1 and the ASCII byte is read again (WHATWG's Shift_JIS rule); units come from the driver's
   lead-byte table, not the codec. The driver validates codec output (well-formed UTF-8, no overconsumption) and
   reports a misbehaving codec as `Internal`. Codec errors are `Internal` (with the OS message) or `Unsupported`;
   the driver reports any other code a codec returns as `Internal` too, so an OS `InvalidArgument` can never pass
   for D10's unencodable character. Opening a page is `Unsupported` when it is not in the table, `IsValidCodePage`
   fails or `iconv_open` reports `EINVAL` (with the gconv hint); any other `iconv_open` failure (`EMFILE`,
   `ENFILE`, `ENOMEM`) is `Internal`.
4. **D4, normalisation to Windows' tables.** The `windowsOnlyC1` bytes (F1: bytes Windows maps to the identical C1
   control and glibc rejects) are decoded and encoded by the driver on every platform, and encoding accepts only
   reversible mappings (`enc(c) = b` and `dec(b) = c`, F2). With both rules, per-unit decoding, BMP encoding and
   valid-stream decoding agree exactly on both OSes for all 16 pages (F1-F3), and the differential's junk-stream
   line (2% invalid units) agrees too (measured below).
5. **D5, lossy Unicode decoding.** UTF-8 replaces maximal subparts (Unicode 16.0 section 3.9, tables 3-8 to 3-11);
   UTF-16 replaces each unpaired surrogate and an odd trailing byte; UTF-32 replaces each unit above U+10FFFF or in
   the surrogate range and a trailing partial unit. Decoding never fails on ill-formed input. Encoders are exact
   and total over well-formed UTF-8.
6. **D6, lossless and the save guard.** A load is lossless iff `encode(decode(bytes)) == bytes`. Acceptance is
   valid-first: the first candidate that decodes without replacements wins. A valid decode that does not round
   trip (`NotRoundTrip`: 932's 398 NEC/IBM duplicate pairs) is accepted, because its text is right, and is guarded;
   rejecting it would let windows-1252, which decodes every byte, win with mojibake. `encodeForSave` refuses a
   lossy document with `Corrupt` unless `SaveConsent::AcceptLoss`; `afterSave` clears the record. A `Replaced`
   legacy document can only be saved after a conversion to a Unicode form, because U+FFFD has no mapping in any
   of the 16 pages (`InvalidArgument`, D10).
7. **D7, detection** (`encoding_detection.cpp`; every constant in one block). The sample is the first 64 KiB
   (`kDetectSampleBytes`, B15); a sequence that the sample's end cuts is not counted as invalid.
   - BOM: confidence 100 (`Bom`); FF FE 00 00 gives UTF-32LE 100 and then UTF-16LE 90 (UTF-16LE text starting
     with U+0000 is possible, and the size decides, D8). Empty file: the default encoding, 100.
   - UTF-16 without a BOM by zero bytes (`NulPattern`), sample >= 4 bytes, h = sample / 2 units: zHi >=
     ceil(0.4 h), zLo <= floor(0.05 h) and a clean UTF-16 decode give `60 + 35 * zHi / h`, at most 95, with no
     fallback chain (a UTF-16 file never becomes UTF-8 with NULs).
   - Valid UTF-8: ASCII only gives the default encoding at 100 (`AsciiOnly`; 90 when the sample was cut, since
     later bytes may be non-ASCII); otherwise UTF-8 at `80 + 5 * min(m, 3)` for m lead bytes >= 0xC2. Both are
     followed by the fallback chain [UTF-8, legacy, 1252] at 30, minus duplicates and pages reported
     `Unsupported`, which lets the loader recover when bytes after the sample are not UTF-8.
   - Invalid UTF-8: each of [legacy, 1252] decodes the sample; clean single-byte pages score 40, clean DBCS pages
     `50 + min(20, d)` for d double-byte characters, and dirty ones 10 (`Lossy`). Order: the clean pages, then
     UTF-8 at 10, then the dirty pages. At most 4 candidates; `tried` records each attempt and its outcome.
   - Detection opens each distinct page once (at most 3 opens, none for a BOM). Only `Unsupported` (a missing
     converter) removes a page; a page whose open failed otherwise stays a candidate (dirty, 10, when the sample
     needed decoding), and the loader reports that error ("while decoding as X") when it reaches the page
     instead of skipping to a lossy fallback.
   - **Invalid UTF-8 falls back losslessly to the legacy page** (the system ANSI code page, else 1252) instead of
     showing U+FFFD. The reason is data safety: a legacy load round-trips, the status bar shows the guess and its
     confidence (30 or 40), `tried` records where UTF-8 failed, and "Reload in UTF-8" stays one click away. This
     is an open question for the user (progress notes): VS Code-style UTF-8 with U+FFFD is the alternative.
   - Unusable defaults (a UTF-16/32 default, a page reported `Unsupported`) fall back to UTF-8; an unusable
     legacy page is skipped and 1252 remains. These values come from settings, so they are never preconditions (D18).
   - No UTF-32 detection without a BOM, and mostly-CJK UTF-16 without a BOM (few zero bytes) is not detected:
     both are override-only, which keeps M1 small and avoids false positives on binary files.
   - 932 is never a candidate unless it is the legacy page: Western 1252 text such as E9 61 is valid Shift_JIS.
8. **D8, BOMs.** `sniffBom` tests UTF-8, then UTF-32 before UTF-16. FF FE 00 00 is UTF-32LE only when the
   file's size is a multiple of 4, otherwise UTF-16LE (a BOM followed by U+0000), so it needs the total size, and
   detection adds UTF-16LE as the second candidate in the UTF-32LE case. A BOM is preserved through `format.bom`
   and written back by `encodeForSave`. Code pages never carry a BOM (`TextFileFormat::isValid`). An
   override strips only a BOM of its own form; other leading bytes are content.
9. **D9, line separators.** Existing breaks are never rewritten on save, unlike editors that normalise on save.
   `EolStats` counts in the LSP model (LF, CRLF and lone CR each end a line, ADR 0013 D2), with a mixed flag. The
   dominant separator (ties: the fallback if among the largest, else Lf, CrLf, Cr) is kept for new lines; files
   without breaks use the platform default (CRLF on Windows, LF on Linux) until `m1/settings`. Conversions to
   positions.hpp's `LineBreak` are total.
10. **D10, unencodable characters** are `InvalidArgument` naming the scalar and its byte offset; never a best-fit
    mapping, '?' or the default character (Windows' 932 default would be U+30FB). `firstUnencodable` finds the
    offset up front.
11. **D11, status-bar fields** (`formatStatus`, O(1), B5) are values and untranslated tokens: the encoding name,
    the BOM flag, the `LossKind`, the detection source and confidence, the separator name and the mixed flag. The
    UI renders them and translates tooltips.
12. **D12, the differential test.** Canonical tables (per-unit decode, an encode sample, junk streams; the whole
    BMP in the stress test) are fingerprinted with FNV-1a 64 (public domain, cited in the code), and only the
    fingerprints are committed, as a licence precaution for Microsoft's and Unicode's mapping data. Goldens are
    generated on one OS and must match on the other unchanged; a mismatch dumps the tables to
    `<build>/encoding-tables` for diffing. The probe counts are pinned as constants, independent of the goldens.
13. **D13, the fuzz target `text_decode`.** Header bytes choose the override, platform or fake codecs (one random
    DBCS page under four ids), the legacy page, a 256-byte buffer limit and the sample size. Checks: the three
    Unicode decoders against independent oracles with split invariance and encode identity, and the whole
    load/save pipeline through `checkLoadAndSave` (the lossless implications, `decodeBytes` agreement, the sink,
    `AcceptLoss` against `firstUnencodable`), then `checkBlockInvariance`: `decodeBytes` in blocks of 16-79 bytes
    (from the seed byte) equals one block, so fuzzed inputs cross many block boundaries.
14. **D14, the default legacy code page.** Windows: `GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
    LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER)`, read once; 0 (Unicode-only locales) or a page outside
    the table gives 1252. Linux: always 1252, because the locale's charset is UTF-8 on every supported setup and
    says nothing about legacy files. Both are open questions for `m1/settings`.
15. **D15, a B15 sub-budget for in-memory loads.** `decodeTextFile` on valid UTF-8 takes at most 1.5x
    `TextBuffer::fromUtf8` on the same input (min of 5 runs, 64 MiB, optimised unsanitised builds;
    `EncodingPerf.Utf8LoadOverhead`). The UTF-8 path is zero-copy (the caller's string moves into the buffer, a
    BOM is erased in place) and validates twice (detection plus `decodeBytes`, then `fromUtf8`); `decodeTextFile`
    counts line breaks on the built buffer (`eolStats(Snapshot)`: one CR pass over `TextSummary`'s counts) rather
    than the string. Measured below.
16. **D16, streaming.** Decoders take `InputEnd::More` or `Final` and return the bytes consumed; an incomplete
    sequence at a `More` end is left for the next call. `DecodeStats` offsets are absolute over the stream, and
    every split gives the same text and stats as one call (tested and fuzzed). Loads decode in blocks of
    `LoadOptions::blockBytes`, 1 MiB by default (`kDecodeBlockBytes`); like `DetectOptions::sampleBytes` it is a
    test and fuzz knob (at least 16 bytes, so every block holds a whole character), and the result never depends
    on it (`TextFileBlocks.SmallBlocksMatchOneBlock` and the fuzz target's check (d)). Detection's cut sample and
    M4's child output use the same rule.
17. **D17, load limits and memory.** `maxFileBytes` is checked before reading (allocation failure terminates, ADR
    0013); the output cap is `maxBufferBytes`, checked after each decode block, so a candidate that would exceed
    it is abandoned early (`TooLarge`). No lossy copy is retained. The round-trip check is incremental (64 KiB
    chunks re-encoded and compared), never a full re-encoding. One decoded text exists at a time: each candidate
    starts from an empty string; a block is decoded straight into the text while its spare capacity covers the
    block's worst case, else into a staging string (one block's output) after which the text is regrown by hand
    to the size projected from the ratio so far, plus 1/32 and 64 KiB (at least 9/8 of the old capacity, at most
    the cap). So the peak is the input, the text plus about 1/32 and one staged block when the first block is
    representative; a later regrowth briefly holds the old and the new buffer. An accepted text with more than
    1/16 + 64 KiB unused is shrunk, so a document never retains the payload-sized reservation of the first build
    (2x its text for UTF-16 ASCII, 4x for UTF-32). Windows API calls take at most 64 KiB; decoding starts each
    call with a one-character chunk and doubles it after every success, bisecting a failing chunk at character
    boundaries, so dense errors cost one failing call per byte and stay linear. `encodeLegacy` resumes after a
    stop.
18. **D18, persisted state.** Every persisted enum has stable, append-only values and a name function with an exact
    inverse (`encodingName`/`findEncoding`, `lossKindName`, `detectionSourceName`, `separatorName`, ...). The
    persisted `DocumentEncoding` fields are the format (encoding, bom, separator), the fidelity record and the
    detection guess. Values from settings, files or the journal are validated (`TextFileFormat::isValid`,
    `find*`) and never reach a precondition. `encodeForSaveTo` streams to a sink in 64 KiB pieces for big saves.

### Interop notes for later slices
- **SaveManager** (`m1/save-manager`): `encodeForSave` or `encodeForSaveTo`, then `writeFileAtomic` (or a
  temporary-file stream once io has one), then `afterSave`. The journal persists every `DocumentEncoding` field
  by name and validates on restore. Reloads after external changes use detection, unless
  `detection.source == UserOverride`, in which case they reuse that override.
- **`m1/large-file`:** `decodeBytes` on a mapped view (`payloadIsText` means map `[payloadOffset, n)`),
  `detectEncoding` on the view, `eolStats(string_view)` and its own single-validation path. `eolStats` on a
  string costs about 15-25 ms per 64 MiB (three passes, measured below); `TODO(M15)` there fuses them if B15 needs
  it.
- **`m1/settings`:** EditorConfig `charset` (utf-8, utf-8-bom, utf-16le, utf-16be, latin1 = 28591) becomes
  `LoadOptions::encoding` or `convertedTo`; `end_of_line` goes through `findSeparator`; a default-encoding and a
  legacy-code-page setting (unusable values fall back, D7); code page ids go through `Encoding::fromCodePageId`.
- **`m1/editor-model`:** Enter inserts `separatorBytes(format.lineSeparator)`; "change line separator" is an edit
  list owned by that slice.
- **`m1/shell-frame`:** `formatStatus` (with its `LossKind` marker); the Reload in / Convert to popups and Add or
  Remove BOM through `convertedTo`; `tried` for "opened as X because Y failed at byte N"; `eolStats(Snapshot)` on
  a background lane when idle.
- **M4:** build and terminal output through `decodeLegacy` or `appendUtf8Lossy` with `InputEnd::More`, and code
  page ids through `fromCodePageId`.

### Deviations from the slice spec
- **Static providers:** `platformCodecs()` returns a `constinit` namespace-scope provider (trivially destructible,
  no state) instead of a function-local static; `defaultLegacyCodePage` is a function-local static integer as
  specified. Both satisfy `-Wglobal-constructors` and `-Wexit-time-destructors`.
- **A fourth support file:** `tests/text/support/encoding_checks.{hpp,cpp}` holds the shared property checks
  (`checkLoadAndSave`), the golden mechanism and the seed helpers, used by `text_file_test`, the stress test and
  the fuzz harness, instead of repeating them.
- **Support link:** `frost_text_encoding_test_support` also links `frost_text_test_support` for `oracleDecode`,
  the UTF-8 definition oracle that `m1/piece-tree` already has.
- **`TextFile.LossyLegacyNeedsConversion`** produces its `Replaced` legacy document with 1253's AA (undefined on
  both OSes, F1), because windows-1252 decodes every byte after D4; the 1252 half of the case converts a lossy
  UTF-8 document with `convertedTo`.
- **Aliases:** the spec named only 932's aliases and the ISO ones; the table adds the WHATWG labels of each page
  (`csibm866`, `dos-874`, `x-cp125x`, `cskoi8r`, ...), the IANA aliases of 437 and 850 (which WHATWG lacks) and
  common iconv spellings, minus the conflicting labels (D2).
- **`payloadOffset`** is the accepted candidate's BOM length (0, 2, 3 or 4) for every accepted candidate, as the
  header documents, not only when `payloadIsText`.
- **gconv loads under a mutex** (not in the spec): the first `verify -Full` failed two cases under WSL TSan
  (`EncodingConcurrency.PlatformCodecsInParallel`, `EncodingStress.LosslessProperty`). TSan reported glibc's
  dynamic loader (`_dl_new_object` in one thread's `iconv_open`, `_dl_close_worker` in another's `iconv_close`) as
  a data race, because glibc's own loader lock is invisible to it. `codec_posix.cpp` now also runs `iconv_open`
  and `iconv_close` under a constant-initialised `std::mutex` (TSan sees it; conversions run outside it), and
  both cases are clean under TSan (five runs of the concurrency cases, the stress binary once). No suppression
  was added. It is a workaround for a tool limitation (LLVM 23.1.4's TSan with glibc 2.39-0ubuntu8.9) and guards
  nothing of ours; no upstream issue was found on 2026-11-01 (filing one is a question for the user), and it is
  removed when both cases pass under TSan without it. The comment in `codec_posix.cpp` says the same.
- **The line-break pass of `decodeTextFile`:** it runs `decodeBytes`' steps without the string `eolStats` and
  counts on the built buffer instead (D15). The result equals `decodeBytes`' (the fuzz harness and
  `TextFile.LosslessIffRoundTrip` compare them). The first build ran the spec's order and failed D15 with ratios
  of 1.54-1.59 on Windows Release; `eolStats(string_view)` was also rewritten (two `std::count` passes and a
  branch-free pair loop instead of one `memchr` per CR).

### Review follow-up (two independent reviews of 22cbc80)
Neither review found a correctness or data-safety bug. The changes, folded into D3, D7 and D17 above:
- **Retained capacity (medium).** The first build reserved the payload's size for every decoded text, so a
  UTF-16 document kept 2x its text in commit for its lifetime, UTF-32 4x, and legacy or lossy texts kept
  std::string's growth (1.5x or 2x). It also held two payload-sized buffers when nothing decoded cleanly. D17 now
  has one text at a time with a projected reservation, and a shrink when more than 1/16 + 64 KiB is unused. The
  reviewer's probe (Release, a 32 MiB text, commit held by the loaded document): UTF-16LE 65 -> 34 MiB,
  windows-1252 with 1% e-acute 49 -> 35 MiB, zero-copy UTF-8 32 MiB before and after.
- **Codec error contract (low).** Codecs passed io's OS mappings through (`ERROR_INVALID_PARAMETER` and `EINVAL`
  became `InvalidArgument`, which `encodeForSave` uses for an unencodable character). Now they are `Internal`
  with the OS message. `iconv_open` reports `Unsupported` only for `EINVAL`, and detection and the loader skip a
  page only when its open is `Unsupported` (D3, D7).
- **Detection opens (low):** each distinct page once, at most 3, and none after a BOM (`DetectionCodecs`).
- **Shared table 3-7 rows:** `isProperUtf8Prefix` and the lossy decoder's maximal subparts read one
  `multiByteShape`.
- **Comments:** the candidates and opens of `detectEncoding`, `encodeLegacy`'s precondition, the BOM erase's
  O(n) cost, D15's ratio label, the 437/850 aliases (WHATWG lacks both; their aliases are now IANA's) and the
  gconv mutex's workaround fields (tool versions, issue, removal condition).
- **The block size became a knob (`LoadOptions::blockBytes`, D16).** The first version of the block tests used
  only 1 MiB inputs and took 2.9 s of WSL's ASan + UBSan Debug run. That pushed `text_encoding_tests` past its
  15 s ASan budget (13.0 s before). The real 1 MiB block is still crossed by the cases where that is cheap; the
  others use 64 B-128 KiB blocks. Block-size invariance is now a property test and fuzz check (d), so fuzzed
  inputs cross block boundaries too.
- **Tests:**
  - `TextFileBlocks` (12 cases, 0.25 s in Windows Debug):
    - sequences across a block end: UTF-16 (both orders, with and without a BOM), Shift_JIS, lossy UTF-8 and a
      UTF-8 BOM file;
    - the buffer limit after the crossing block and on the last block;
    - the capacity bound, including the decoded-again path;
    - `SmallBlocksMatchOneBlock`;
    - `NotRoundTrip` offsets past the first 64 KiB round-trip chunk;
    - the error contract at the loader.
  - Detection: `SampleCutTailUsesTable37`, `FailedOpenKeepsPage` and `OpensEachPageOnce`.
  - The `cp1253-undefined` fixture, which gives `consentUnencodable` its first reader.
  - Before the knob, seven of the new checks failed on 22cbc80's sources (the knob's tests cannot build there).
  - Mutants of `text_file.cpp`, each run against `TextFileBlocks` and the corpus replay:
    - always passing `Final`, or `pos = blockEnd`: three or four block cases and the property fail, and so do 3
      of the 30 replayed seeds;
    - checking the limit only after the last block: `TooLargeStopsAfterTheCrossingBlock` fails;
    - 22cbc80's capacity code (a payload-sized reservation, no shrink), or dropping either shrink: the capacity
      test fails.
    - The payload-sized reservation alone survives, because the shrink then hides it. That is the transient
      peak, which no unit test observes; the probe above measures it.

## Measurements
Machine: REF-W1 (AMD Ryzen 9 5900X, 12 cores / 24 threads, 32 GB, Windows 11 10.0.26300; WSL Ubuntu 24.04 with
glibc 2.39-0ubuntu8.9). LLVM 23.1.4; MSVC STL 14.44.35207 on Windows, libstdc++ 14 on Linux. All Windows runs
went through `run_hidden_nobox.py` (a private desktop).

### Probe (2026-11-01, before the slice; scratch scripts, not committed)
- **F1, per-unit decoding.** Each single byte and each DBCS pair decoded strictly. The only differences are bytes
  Windows maps to the identical C1 control and glibc rejects: 874 81-84 86-8F 90 98-9F; 932 80; 1250 81 83 88
  90 98; 1251 98; 1252 81 8D 8F 90 9D; 1253 81 88 8A 8C-90 98 9A 9C-9F; 1254 81 8D-90 9D 9E; 1257 81 83 88 8A 8C
  90 98 9A 9C 9F; the other eight pages are identical. Invalid on both: 874 8 bytes (DB-DE, FC-FF), 932 64 single
  bytes (60 lone leads plus A0, FD, FE, FF), 1253 AA D2 FF, 1257 A1 A5. 932: 192 valid single bytes and 9604 valid
  pairs (9796 units); 398 pairs do not round-trip, identically on both OSes (lead rows 87: 9, ED: 188, EE: 186,
  FA: 15; e.g. 87 90 -> U+2252 -> 81 E0); EUDC F040-F9FC maps to the PUA and round-trips; without
  `MB_ERR_INVALID_CHARS` Windows turns invalid input into U+30FB.
- **F2, BMP encoding.** Both sides have one-way mappings (Windows: 932 U+F8F0-F8F3, 1253 U+F8F9-F8FB, 874
  U+F8C1-F8C8, 1257 U+F8FC-F8FD to bytes it refuses to decode; glibc: nine 932 mappings such as U+00A5 -> 5C and
  U+203E -> 7E). With D4's two rules both agree for all 16 pages; reversible counts 932 9398, 874 248, 1253 253,
  1257 254, every other page 256.
- **F3, valid streams:** 20000 random valid units per page decode identically on both OSes (no stateful or
  composing page).
- **F4, exclusions:** CP1258 41 DE is U+00C3 on Linux and U+0041 U+0303 on Windows; CP1255 F9 D1 is U+FB2A on
  Linux and U+05E9 U+05C1 on Windows; 936/949/950 have 2149/189/5817 Windows-only mappings; KOI8-U differs at AE
  and BE.
- **F5, environment:** all 16 iconv names open in WSL; `GetLocaleInfoEx` returns 1252 on REF-W1; Unicode-only
  locales (hi-IN) return 0.
- **F6:** CPython's `decode('utf-8', 'replace')` reproduces every Unicode golden (33 replacements), as a
  development cross-check only.
- **F7, review cross-checks:** 81 41 is valid 932 (U+3001); 85 41, 81 7F, 81 0A, 81 FF and 85 80 are invalid on
  Windows (error 1113). CPython's cp932 is not Windows' table (it decodes A0/FD/FE/FF, and 379 BMP scalars encode
  differently), so it was used only where CPython, Windows and glibc agree: the cp1252, cp932 and UTF-16LE fixture
  bytes.

### The differential (D12)
The goldens were generated once on Windows with `FROST_UPDATE_GOLDENS=1` (that run fails by design: "goldens
rewritten; rerun without FROST_UPDATE_GOLDENS"), checked, and committed: `fingerprints.txt` (per page: decode
units/valid/nort, an encode sample, 4 junk streams) and `fingerprints-bmp.txt` (every BMP scalar through
`encodeLegacy` per page). Unchanged, both match in WSL (`EncodingDifferential.FingerprintsMatchGolden` and
`EncodingDifferentialStress.FullBmpEncode` pass on the mirror without an update), so Windows and glibc agree after
D4 on per-unit decoding, the BMP encode tables and, beyond the probe, streams with 2% invalid units. The pinned
counts match the probe: 932 units 11776, valid 9796, nort 398, reversible 9398; 874 248, 1253 253, 1257 254
valid units and encodable scalars, 256 elsewhere.

### Test times
| Configuration | `text_encoding_tests` | integration | stress (FullBmpEncode + LosslessProperty) |
| --- | --- | --- | --- |
| Windows Debug | 4.3 s (127 cases; budget 5 s) | 0.2 s (2) | 7.4-9.1 s (not run by `verify` in Debug) |
| WSL Debug | 3.4 s (126 cases) | 0.2 s (2) | 2.5 s |
| Windows Release | 0.85 s | not measured | 0.63 s |
| Windows ASan (RelWithDebInfo) | 11.5 s (budget 15 s) | 0.4 s | 5.9 s |
| WSL ASan + UBSan (Debug) | 13.0 s (budget 15 s) | 0.5 s | 8.4 s |
| WSL TSan (RelWithDebInfo) | 3.6 s | 0.5 s | 3.6 s |

Times are gtest's totals for the binary run directly (ctest runs each case as its own process, which adds the
start-ups: the Windows ASan ctest sum of the unit cases plus the replay is 14.1 s). Windows has one more case than
Linux (the platform tests differ: `GetCPInfoExW`/`GetLocaleInfoEx` checks on
Windows, the raw iconv and fixed-1252 checks on Linux). The slowest unit cases in WSL Debug:
`TextFile.LosslessIffRoundTrip` 0.65 s, `Utf8Lossy.MatchesDefinitionOracle` 0.60 s,
`TextFile.EncodeForSaveToMatchesString` 0.41 s, `LegacyDriver.RandomTablesMatchOracle` 0.34 s.

### Perf (`text_encoding_perf_tests`, Release, min of 5 runs, three runs each)
| | Windows | WSL |
| --- | --- | --- |
| D15 ratio, 64 MiB (decodeTextFile / fromUtf8) | **1.21-1.26** (73-82 / 60 ms) | **1.22-1.31** (62-77 / 51-63 ms) |
| ... the parts: one validation / string `eolStats` | 7-8 ms / 15-19 ms | 6-8 ms / 21-27 ms |
| Detection, 256 MiB file: UTF-8 / 1252 / dense junk with 932 | 0.11 / 0.37 / 2.2 ms | 0.06 / 0.14-0.26 / 1.4-1.6 ms |
| Decode, 32 MiB: UTF-8 lossy / UTF-16LE / 1252 / Shift_JIS | 3.0-4.2 GB/s / 1.06 GB/s / 230 MB/s / 118 MB/s | 5.7-7.0 GB/s / 0.8-1.0 GB/s / 340-390 MB/s / 175-189 MB/s |
| Dense invalid: 4 MiB of FD through Shift_JIS (bound 2 MB/s, D17) | 29-35 MB/s | 16-18 MB/s |

Before the D15 change (the string `eolStats` in `decodeTextFile`), Windows measured 1.54-1.59; the deferred count
brought it to 1.21-1.26. Detection reads only the sample, so its cost does not grow with the file.

### Fuzzing (`text_decode`)
Target: at least 200 exec/s in the sanitised fuzz build on each OS. The 60 s `fuzz.text_decode` smokes of the first
`verify -Full`, each from an empty work corpus plus the 22 hand seeds, found no crash, leak or timeout:
- Windows (`win-clangcl-fuzz`, /MT, ASan): 88929 runs, **1457 exec/s**, 1387 -> 1936 edges, 2429 -> 6158
  features, peak RSS 230 MB.
- Linux (`linux-clang-fuzz`, ASan + UBSan): 129216 runs, **2118 exec/s**, 1271 -> 1860 edges, peak RSS 256 MB.
- Corpus: the 22 hand seeds (13 fixtures with their header bytes, the five Unicode examples, two FF FE 00 00 sizes,
  Shift_JIS's invalid pairs, `hand-ascii`) plus 8 picked inputs. `-set_cover_merge=1 -use_counters=0 -use_cmp=0`
  over each smoke's work corpus (838 files on Windows, 877 on Linux) offered 31 and 33 new files; a greedy cover
  over the merge control files picked 6 Windows inputs (396 edges beyond the seeds' 1390) and then 2 Linux inputs
  (20 edges beyond those 28 files' 1729). The committed 30 files (661 bytes) cover 1812 edges on Windows
  (`-runs=0`). Every merge and replay on Windows ran through `run_hidden_nobox.py`.
- The final `verify -Full`, from the committed 30 files: Windows 121170 runs, **1986 exec/s**, 1812 -> 1941 edges,
  peak RSS 234 MB; Linux 137285 runs, **2250 exec/s**, 1757 -> 1861 edges, peak RSS 255 MB. No crash, leak or
  timeout, and TSan 545/545 in WSL after the gconv mutex.

## Consequences
- Every load reports what it did: the encoding, a confidence, the attempts and an exact fidelity record; a lossy
  document cannot be saved by accident.
- Fedora and other hosts without glibc's CP932 gconv module report `Unsupported` for 932; the M5 AppImage relies
  on the host's gconv modules and must not bundle them. Linux behaviour is verified on glibc 2.39 only.
- The UTF-8 path validates twice for in-memory loads (about 7 ms per 64 MiB); files of 128 MB and more take
  `m1/large-file`'s mapped path.
- Unchanged regions of a `NotRoundTrip` file are not preserved on an accepted lossy save. `TODO(M13)`: byte reuse
  for unchanged pieces.
- A file that grows between `status` and `readFile` can still be read whole (the size is checked again after the
  read; `TODO(M15)`, `m15/large-file`).
- Without a BOM, CJK UTF-16 and all UTF-32 files are not detected (override only).
- frost::text grows by two platform files; text, core and io changes need an independent review before merge
  (CLAUDE.md), which is not part of this slice.
- Dependencies: none new. Windows uses kernel32 (`MultiByteToWideChar`, `WideCharToMultiByte`, `IsValidCodePage`,
  `GetLocaleInfoEx`; `GetCPInfoExW` in tests). Linux uses glibc's iconv: glibc (LGPL-2.1-or-later) is the
  platform C library that every Linux build already links dynamically, iconv adds no library, and the host
  provides the gconv modules, so nothing is bundled. `tools/licensecheck.py` checks vcpkg ports and Qt and is
  unaffected (it passes unchanged); no SPDX entry and no NOTICE entry are needed. `frost::function_ref` (compat,
  through core) types `SaveSink`.

## Alternatives considered
- **ICU:** licence Unicode-3.0, not on the allowlist, and large.
- **GNU libiconv on Windows:** LGPL, and it would be bundled.
- **uchardet** (statistical detection): tri-licensed MPL/GPL/LGPL, not on the allowlist; not opened.
- **Google compact_enc_det:** Apache-2.0; possible later for statistical detection behind the same
  `detectEncoding` interface.
- **Our own mapping tables:** size, and the licence of copied tables (Microsoft's and Unicode's data).
- **Runtime-built per-codec tables for single-byte pages** (256 platform calls at open): considered as a speed-up;
  not needed after the chunking and resume fixes, kept as a fallback if `DecodeThroughput` ever needs it.
- **UTF-8 with U+FFFD for invalid files** (VS Code's default): loses data on save unless the user notices; the
  lossless legacy fallback was chosen (D7), pending the user's answer.
- **Rejecting `NotRoundTrip` decodes:** would make 1252 win on Shift_JIS files with NEC/IBM duplicates (D6).
- **Normalising line breaks on save:** silently rewrites lines the user never touched (D9).

## Sources
- Unicode 16.0 core specification, chapter 3, section 3.9 (U+FFFD substitution of maximal subparts) and table
  3-7 (well-formed byte sequences).
- WHATWG Encoding Standard (names and labels, the Shift_JIS decoder's ASCII-trail rule; CC BY 4.0).
- IANA Character Sets registry (the aliases of IBM437 and IBM850, which WHATWG does not list).
- Microsoft Learn: `MultiByteToWideChar`, `WideCharToMultiByte`, `IsValidCodePage`, `GetCPInfoExW`,
  `GetLocaleInfoEx`.
- The glibc `iconv(3)` manual page.
- FNV hash (http://www.isthe.com/chongo/tech/comp/fnv/, public domain), cited in `encoding_oracle.cpp`.
- CPython's codecs, used only to produce fixture bytes and cross-check goldens on characters where CPython,
  Windows and glibc agree (F7); never copied.

No IDE source was consulted. In particular, neither IntelliJ's CharsetToolkit or LoadTextUtil (JetBrains code,
banned even where Apache-2.0) nor VS Code's encoding detection (jschardet, LGPL) was opened, and no glibc source
was read.
