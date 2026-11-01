// SPDX-License-Identifier: Apache-2.0
//
// The encoding fixture table (encoding_fixtures.hpp). Hex arrays are the source of the committed
// files in tests/text/fixtures/encodings; CPython-produced bytes say so beside them.

#include "text/support/encoding_fixtures.hpp"

#include "text/support/encoding_oracle.hpp"

#include <cstdio>
#include <cstdlib>

namespace frost::text::test {
namespace {

using Form = EncodingForm;

Encoding unicode(Form form) { return Encoding::unicode(form); }

EncodingFixture make(std::string name, std::string file, std::string bytes) {
    EncodingFixture f;
    f.name = std::move(name);
    f.file = std::move(file);
    f.bytes = std::move(bytes);
    return f;
}

std::vector<EncodingFixture> buildFixtures() {
    std::vector<EncodingFixture> all;
    const std::string codeText = fromU8(u8"int main() {\n    return 0; // \u00e9 \u2192\n}\n");
    const std::string utf16Text = fromU8(u8"G clef \U0001D11E, grin \U0001F600, \u00e4\r\nline 2\r\n");

    { // 1. LF, UTF-8 without a BOM; two multi-byte sequences (m = 2: confidence 90).
        EncodingFixture f = make("lf-utf8", "lf-utf8.txt",
                                 fromHex("69 6e 74 20 6d 61 69 6e 28 29 20 7b 0a 20 20 20 20 72 65 74 75 72 6e 20 30 "
                                         "3b 20 2f 2f 20 c3 a9 20 e2 86 92 0a 7d 0a"));
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::Utf8Valid;
        f.minConfidence = 90;
        f.eol = {3, 0, 0};
        f.separator = LineSeparator::Lf;
        f.text = codeText;
        all.push_back(std::move(f));
    }
    { // 2. The same text with CRLF.
        EncodingFixture f = make("crlf-utf8", "crlf-utf8.txt",
                                 fromHex("69 6e 74 20 6d 61 69 6e 28 29 20 7b 0d 0a 20 20 20 20 72 65 74 75 72 6e 20 "
                                         "30 3b 20 2f 2f 20 c3 a9 20 e2 86 92 0d 0a 7d 0d 0a"));
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::Utf8Valid;
        f.minConfidence = 90;
        f.eol = {0, 3, 0};
        f.separator = LineSeparator::CrLf;
        f.text = fromU8(u8"int main() {\r\n    return 0; // \u00e9 \u2192\r\n}\r\n");
        all.push_back(std::move(f));
    }
    { // 3. Lone CRs only.
        EncodingFixture f = make("cr-only", "cr-only.txt", "a\rb\rc");
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::AsciiOnly;
        f.minConfidence = 100;
        f.eol = {0, 0, 2};
        f.separator = LineSeparator::Cr;
        f.text = "a\rb\rc";
        all.push_back(std::move(f));
    }
    { // 4. Mixed breaks; CRLF dominates.
        EncodingFixture f = make("mixed-eol", "mixed-eol.txt", "a\r\nb\r\nc\nd\re\r\n");
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::AsciiOnly;
        f.minConfidence = 100;
        f.eol = {1, 3, 1};
        f.separator = LineSeparator::CrLf;
        f.text = "a\r\nb\r\nc\nd\re\r\n";
        all.push_back(std::move(f));
    }
    { // 5. UTF-8 with a BOM and CRLF.
        EncodingFixture f =
            make("utf8-bom-crlf", "utf8-bom-crlf.txt", fromHex("ef bb bf 2f 2f 20 c3 bc 0d 0a 78 0d 0a"));
        f.encoding = Encoding::utf8();
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = fromU8(u8"// \u00fc\r\nx\r\n");
        all.push_back(std::move(f));
    }
    { // 6. A UTF-8 BOM and nothing else: the separator comes from the default (LF).
        EncodingFixture f = make("utf8-bom-only", "utf8-bom-only.txt", fromHex("ef bb bf"));
        f.encoding = Encoding::utf8();
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.separator = LineSeparator::Lf;
        all.push_back(std::move(f));
    }
    { // 7. An empty file.
        EncodingFixture f = make("empty", "empty.txt", "");
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::Empty;
        f.minConfidence = 100;
        f.separator = LineSeparator::Lf;
        all.push_back(std::move(f));
    }
    { // 8. UTF-16LE with a BOM and two surrogate pairs. CPython: "...".encode("utf-16-le").
        EncodingFixture f =
            make("utf16le-bom", "utf16le-bom.txt",
                 fromHex("ff fe 47 00 20 00 63 00 6c 00 65 00 66 00 20 00 34 d8 1e dd 2c 00 20 00 67 00 "
                         "72 00 69 00 6e 00 20 00 3d d8 00 de 2c 00 20 00 e4 00 0d 00 0a 00 6c 00 69 00 "
                         "6e 00 65 00 20 00 32 00 0d 00 0a 00"));
        f.encoding = unicode(Form::Utf16Le);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = utf16Text;
        all.push_back(std::move(f));
    }
    { // 9. The same text in UTF-16BE with a BOM. CPython: "...".encode("utf-16-be").
        EncodingFixture f =
            make("utf16be-bom", "utf16be-bom.txt",
                 fromHex("fe ff 00 47 00 20 00 63 00 6c 00 65 00 66 00 20 d8 34 dd 1e 00 2c 00 20 00 67 "
                         "00 72 00 69 00 6e 00 20 d8 3d de 00 00 2c 00 20 00 e4 00 0d 00 0a 00 6c 00 69 "
                         "00 6e 00 65 00 20 00 32 00 0d 00 0a"));
        f.encoding = unicode(Form::Utf16Be);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = utf16Text;
        all.push_back(std::move(f));
    }
    { // 10. UTF-16LE without a BOM, detected by its zero bytes (zHi = h: confidence 95).
        EncodingFixture f =
            make("utf16le-nobom", "utf16le-nobom.txt",
                 fromHex("23 00 69 00 6e 00 63 00 6c 00 75 00 64 00 65 00 20 00 3c 00 63 00 73 00 74 00 "
                         "64 00 69 00 6f 00 3e 00 0d 00 0a 00 69 00 6e 00 74 00 20 00 6d 00 61 00 69 00 "
                         "6e 00 28 00 29 00 20 00 7b 00 20 00 72 00 65 00 74 00 75 00 72 00 6e 00 20 00 "
                         "30 00 3b 00 20 00 7d 00 0d 00 0a 00"));
        f.encoding = unicode(Form::Utf16Le);
        f.source = DetectionSource::NulPattern;
        f.minConfidence = 95;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = "#include <cstdio>\r\nint main() { return 0; }\r\n";
        all.push_back(std::move(f));
    }
    { // 11. UTF-32LE with a BOM (20 bytes, a multiple of 4). CPython: "...".encode("utf-32-le").
        EncodingFixture f = make("utf32le-bom", "utf32le-bom.txt",
                                 fromHex("ff fe 00 00 78 00 00 00 20 00 00 00 00 f6 01 00 0a 00 00 00"));
        f.encoding = unicode(Form::Utf32Le);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.eol = {1, 0, 0};
        f.separator = LineSeparator::Lf;
        f.text = fromU8(u8"x \U0001F600\n");
        all.push_back(std::move(f));
    }
    { // 12. FF FE 00 00 at size 6: a UTF-16LE BOM followed by U+0000 'A' (D8).
        EncodingFixture f = make("utf16le-bom-nul", "utf16le-bom-nul.txt", fromHex("ff fe 00 00 41 00"));
        f.encoding = unicode(Form::Utf16Le);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.separator = LineSeparator::Lf;
        f.text = std::string("\0A", 2);
        all.push_back(std::move(f));
    }
    { // 13. windows-1252. CPython: "...".encode("cp1252") (re-checked, ADR 0018 F7).
        EncodingFixture f =
            make("cp1252", "cp1252.txt",
                 fromHex("2f 2f 20 43 61 66 e9 20 96 20 93 71 75 6f 74 65 73 94 20 80 35 20 6e 61 ef 76 "
                         "65 0d 0a 69 6e 74 20 78 20 3d 20 30 3b 0d 0a"));
        f.legacyCodePage = 1252;
        f.encoding = Encoding::legacy(1252);
        f.source = DetectionSource::LegacyFallback;
        f.minConfidence = 40;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = fromU8(u8"// Caf\u00e9 \u2013 \u201cquotes\u201d \u20ac5 na\u00efve\r\nint x = 0;\r\n");
        all.push_back(std::move(f));
    }
    { // 14. windows-1252's C1 bytes 81 and 9D: lossless on both OSes by D4 (expectations from
        // the probe; CPython rejects these bytes).
        EncodingFixture f = make("cp1252-c1", "cp1252-c1.txt", fromHex("61 81 62 9d 63"));
        f.legacyCodePage = 1252;
        f.encoding = Encoding::legacy(1252);
        f.source = DetectionSource::LegacyFallback;
        f.minConfidence = 40;
        f.separator = LineSeparator::Lf;
        f.text = fromU8(u8"a\u0081b\u009dc");
        all.push_back(std::move(f));
    }
    { // 15. Shift_JIS with 14 double-byte characters (confidence 50 + 14). CPython:
        // "...".encode("cp932"); every character agrees across CPython, Windows and glibc.
        EncodingFixture f =
            make("cp932", "cp932.txt",
                 fromHex("2f 2f 20 93 fa 96 7b 8c ea 82 cc 83 52 83 81 83 93 83 67 3a 20 87 40 87 41 87 "
                         "42 20 b6 c0 b6 c5 20 81 60 0d 0a 69 6e 74 20 95 cf 90 94 20 3d 20 30 3b 20 2f "
                         "2f 20 c3 bd c4 0d 0a"));
        f.legacyCodePage = 932;
        f.encoding = Encoding::legacy(932);
        f.source = DetectionSource::LegacyFallback;
        f.minConfidence = 64;
        f.eol = {0, 2, 0};
        f.separator = LineSeparator::CrLf;
        f.text = fromU8(u8"// \u65e5\u672c\u8a9e\u306e\u30b3\u30e1\u30f3\u30c8: \u2460\u2461\u2462 "
                        u8"\uff76\uff80\uff76\uff85 \uff5e\r\nint \u5909\u6570 = 0; // \uff83\uff7d\uff84\r\n");
        all.push_back(std::move(f));
    }
    { // 16. 932's NEC/IBM duplicate: 87 90 and 81 E0 both decode to U+2252, which encodes as
        // 81 E0 on both OSes (probe F1). Valid-first: accepted as NotRoundTrip (D6).
        EncodingFixture f = make("cp932-nec-dup", "cp932-nec-dup.txt", fromHex("87 90 81 e0"));
        f.legacyCodePage = 932;
        f.encoding = Encoding::legacy(932);
        f.source = DetectionSource::LegacyFallback;
        f.minConfidence = 52;
        f.separator = LineSeparator::Lf;
        f.loss = LossKind::NotRoundTrip;
        f.firstLossFileOffset = 0;
        f.firstLossTextOffset = 0;
        f.text = fromU8(u8"\u2252\u2252");
        f.consentBytes = fromHex("81 e0 81 e0");
        all.push_back(std::move(f));
    }
    // 17. The Unicode standard's five examples (section 3.9, tables 3-8 to 3-11 and the
    // section's own example), each followed by LF: 8 + 8 + 7 + 4 + 6 = 33 replacements.
    const std::string invalid =
        fromHex("c0 af e0 80 bf f0 81 82 41 0a ed a0 80 ed bf bf ed af 41 0a f4 91 92 93 ff 41 "
                "80 bf 42 0a e1 80 e2 f0 91 92 f1 bf 41 0a 61 f1 80 80 e1 80 c2 62 80 63 80 bf 64 0a");
    {
        EncodingFixture f = make("invalid-utf8", "invalid-utf8.txt", invalid);
        f.override = Encoding::utf8();
        f.encoding = Encoding::utf8();
        f.source = DetectionSource::UserOverride;
        f.minConfidence = 100;
        f.eol = {5, 0, 0};
        f.separator = LineSeparator::Lf;
        f.loss = LossKind::Replaced;
        f.replacements = 33;
        f.firstLossFileOffset = 0;
        f.firstLossTextOffset = 0;
        f.text = fromU8(u8"\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffdA\n"
                        u8"\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffdA\n"
                        u8"\ufffd\ufffd\ufffd\ufffd\ufffdA\ufffd\ufffdB\n"
                        u8"\ufffd\ufffd\ufffd\ufffdA\n"
                        u8"a\ufffd\ufffd\ufffdb\ufffdc\ufffd\ufffdd\n");
        f.consentBytes = f.text;
        all.push_back(std::move(f));
    }
    { // The same file without an override: every byte decodes in windows-1252 (D4 for 81).
        EncodingFixture f = make("invalid-utf8-as-1252", "invalid-utf8.txt", invalid);
        f.legacyCodePage = 1252;
        f.encoding = Encoding::legacy(1252);
        f.source = DetectionSource::LegacyFallback;
        f.minConfidence = 40;
        f.eol = {5, 0, 0};
        f.separator = LineSeparator::Lf;
        f.text = fromHex("c3 80 c2 af c3 a0 e2 82 ac c2 bf c3 b0 c2 81 e2 80 9a 41 0a c3 ad c2 a0 e2 82 ac c3 ad c2 bf "
                         "c2 bf c3 ad c2 af 41 0a c3 b4 e2 80 98 e2 80 99 e2 80 9c c3 bf 41 e2 82 ac c2 bf 42 0a c3 a1 "
                         "e2 82 ac c3 a2 c3 b0 e2 80 98 e2 80 99 c3 b1 c2 bf 41 0a 61 c3 b1 e2 82 ac e2 82 ac c3 a1 e2 "
                         "82 ac c3 82 62 e2 82 ac 63 e2 82 ac c2 bf 64 0a");
        all.push_back(std::move(f));
    }
    { // 18. A lone high surrogate in UTF-16LE: one U+FFFD at file offset 4.
        EncodingFixture f =
            make("utf16le-lone-surrogate", "utf16le-lone-surrogate.txt", fromHex("ff fe 41 00 00 d8 42 00"));
        f.encoding = unicode(Form::Utf16Le);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.separator = LineSeparator::Lf;
        f.loss = LossKind::Replaced;
        f.replacements = 1;
        f.firstLossFileOffset = 4;
        f.firstLossTextOffset = 1;
        f.text = fromU8(u8"A\ufffdB");
        f.consentBytes = fromHex("ff fe 41 00 fd ff 42 00");
        all.push_back(std::move(f));
    }
    { // 19. UTF-16BE with an odd trailing byte.
        EncodingFixture f = make("utf16be-odd", "utf16be-odd.txt", fromHex("fe ff 00 41 00"));
        f.encoding = unicode(Form::Utf16Be);
        f.bom = true;
        f.source = DetectionSource::Bom;
        f.minConfidence = 100;
        f.separator = LineSeparator::Lf;
        f.loss = LossKind::Replaced;
        f.replacements = 1;
        f.firstLossFileOffset = 4;
        f.firstLossTextOffset = 1;
        f.text = fromU8(u8"A\ufffd");
        f.consentBytes = fromHex("fe ff 00 41 ff fd");
        all.push_back(std::move(f));
    }
    { // 20. A Replaced legacy load: windows-1253 leaves AA undefined on both OSes (probe F1), so
        // the override gives U+FFFD, and even AcceptLoss fails (U+FFFD has no mapping, D6).
        EncodingFixture f = make("cp1253-undefined", "cp1253-undefined.txt", fromHex("61 aa 62 0a"));
        f.override = Encoding::legacy(1253);
        f.encoding = Encoding::legacy(1253);
        f.source = DetectionSource::UserOverride;
        f.minConfidence = 100;
        f.eol = {1, 0, 0};
        f.separator = LineSeparator::Lf;
        f.loss = LossKind::Replaced;
        f.replacements = 1;
        f.firstLossFileOffset = 1;
        f.firstLossTextOffset = 1;
        f.text = fromU8(u8"a\ufffdb\n");
        f.consentUnencodable = true;
        all.push_back(std::move(f));
    }
    return all;
}

} // namespace

const std::vector<EncodingFixture>& encodingFixtures() {
    static const std::vector<EncodingFixture> all = buildFixtures();
    return all;
}

const EncodingFixture& fixture(std::string_view name) {
    for (const EncodingFixture& f : encodingFixtures()) {
        if (f.name == name) {
            return f;
        }
    }
    std::fprintf(stderr, "no encoding fixture named %.*s\n", static_cast<int>(name.size()), name.data());
    std::abort();
}

LoadOptions fixtureOptions(const EncodingFixture& f) {
    LoadOptions options;
    options.encoding = f.override;
    options.detect.legacyCodePage = f.legacyCodePage;
    options.detect.defaultEncoding = Encoding::utf8();
    options.defaultSeparator = LineSeparator::Lf;
    return options;
}

std::string fuzzSeedHeader(const EncodingFixture& f) {
    /*
     * Byte 0: bits 0-2 the override (0 none, 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE, 4 UTF-32LE,
     * 5 UTF-32BE, 6 cp1252, 7 cp932); bit 3 clear: platform codecs; bits 4-5 the legacy code page
     * (0 1252, 1 932, 2 1251, 3 437); bits 6-7 clear: no buffer limit. Byte 1: the fake seed.
     * Byte 2: 255, the default sample size.
     */
    unsigned overrideBits = 0;
    if (f.override) {
        switch (f.override->form) {
        case EncodingForm::Utf8: overrideBits = 1; break;
        case EncodingForm::Utf16Le: overrideBits = 2; break;
        case EncodingForm::Utf16Be: overrideBits = 3; break;
        case EncodingForm::Utf32Le: overrideBits = 4; break;
        case EncodingForm::Utf32Be: overrideBits = 5; break;
        case EncodingForm::CodePage: overrideBits = f.override->codePage == 932 ? 7u : 6u; break;
        }
    }
    const unsigned legacyBits = f.legacyCodePage == 932 ? 1u : 0u;
    std::string header;
    header.push_back(static_cast<char>(overrideBits | (legacyBits << 4)));
    header.push_back('\0');
    header.push_back(static_cast<char>(255));
    return header;
}

} // namespace frost::text::test
