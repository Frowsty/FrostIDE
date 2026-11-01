// SPDX-License-Identifier: Apache-2.0
//
// frost_ts_unicode.h: frost::syntax's UTF-8 and UTF-16 decoders for the tree-sitter runtime
// (ADR 0023, D2). They replace the bodies of upstream's lib/src/unicode.h, which wraps ICU
// headers that we do not vendor (their licence is not on the allowlist).
/*
 * frost_tree_sitter.c defines TREE_SITTER_UNICODE_H_ (upstream's include guard) and includes this
 * header before the runtime, so the runtime's `#include "./unicode.h"` resolves to the vendored
 * file but skips its body. If upstream ever renames the guard, both sets of definitions collide
 * and the build fails loudly instead of silently mixing decoders.
 *
 * The contract is upstream's (TSDecodeFunction in api.h, as the lexer uses it):
 *   - the return value is the number of bytes consumed, at least 1 when length >= 1 (0 only for
 *     length 0);
 *   - *code_point receives the scalar value, or TS_DECODE_ERROR for ill-formed input;
 *   - ill-formed UTF-8 consumes its maximal subpart (Unicode 16.0, section 3.9, "U+FFFD
 *     Substitution of Maximal Subparts"), so the lexer resynchronises on the next possible lead
 *     byte;
 *   - a sequence cut off by the end of the buffer is an error that consumed fewer than 4 bytes,
 *     which makes the lexer fetch a fresh chunk and retry.
 * Written from the Unicode Standard, Table 3-7 ("Well-Formed UTF-8 Byte Sequences") and section
 * 3.9's UTF-16 definition; no ICU or upstream decoder code was used. Pure functions, any thread.
 * Also compiles as C++ so tests can call it directly.
 */
#ifndef FROST_TS_UNICODE_H
#define FROST_TS_UNICODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The value a decoder stores for ill-formed input (upstream uses ICU's U_SENTINEL, -1).
static const int32_t TS_DECODE_ERROR = -1;

/*
 * Decodes one UTF-8 scalar value from string[0, length). Table 3-7: the lead byte selects the
 * sequence length and the allowed range of the second byte (E0: A0..BF, ED: 80..9F, F0: 90..BF,
 * F4: 80..8F, otherwise 80..BF); later bytes are 80..BF. C0, C1 and F5..FF never start a sequence.
 * O(1).
 */
static inline uint32_t ts_decode_utf8(const uint8_t* string, uint32_t length, int32_t* code_point) {
    if (length == 0) {
        *code_point = TS_DECODE_ERROR;
        return 0;
    }
    const uint8_t lead = string[0];
    if (lead < 0x80) {
        *code_point = (int32_t)lead;
        return 1;
    }
    uint32_t trail = 0;
    uint32_t value = 0;
    uint8_t low = 0x80;
    uint8_t high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trail = 1;
        value = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trail = 2;
        value = lead & 0x0Fu;
        if (lead == 0xE0) {
            low = 0xA0;
        } else if (lead == 0xED) {
            high = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trail = 3;
        value = lead & 0x07u;
        if (lead == 0xF0) {
            low = 0x90;
        } else if (lead == 0xF4) {
            high = 0x8F;
        }
    } else {
        // A continuation byte, C0, C1 or F5..FF: a maximal subpart of one byte.
        *code_point = TS_DECODE_ERROR;
        return 1;
    }
    for (uint32_t i = 1; i <= trail; ++i) {
        if (i >= length) {
            // Truncated by the end of the buffer: i < 4 bytes consumed, so the lexer re-reads.
            *code_point = TS_DECODE_ERROR;
            return i;
        }
        const uint8_t byte = string[i];
        if (byte < low || byte > high) {
            // The maximal subpart ends before the offending byte, which is decoded next.
            *code_point = TS_DECODE_ERROR;
            return i;
        }
        low = 0x80;
        high = 0xBF;
        value = (value << 6) | (byte & 0x3Fu);
    }
    *code_point = (int32_t)value;
    return trail + 1;
}

// Decodes one UTF-16 scalar value from two-byte units in the byte order given by `big_endian`.
// A lone surrogate is an error that consumes its unit; an odd trailing byte is an error that
// consumes it. O(1).
static inline uint32_t frost_ts_decode_utf16(const uint8_t* string, uint32_t length, int32_t* code_point,
                                             int big_endian) {
    if (length < 2) {
        *code_point = TS_DECODE_ERROR;
        return length;
    }
    const uint32_t first = big_endian ? ((uint32_t)string[0] << 8 | string[1]) : ((uint32_t)string[1] << 8 | string[0]);
    if (first < 0xD800 || first > 0xDFFF) {
        *code_point = (int32_t)first;
        return 2;
    }
    if (first <= 0xDBFF && length >= 4) {
        const uint32_t second =
            big_endian ? ((uint32_t)string[2] << 8 | string[3]) : ((uint32_t)string[3] << 8 | string[2]);
        if (second >= 0xDC00 && second <= 0xDFFF) {
            *code_point = (int32_t)(0x10000u + ((first - 0xD800u) << 10) + (second - 0xDC00u));
            return 4;
        }
    }
    *code_point = TS_DECODE_ERROR;
    return 2;
}

// UTF-16 little-endian (TSInputEncodingUTF16LE). O(1).
static inline uint32_t ts_decode_utf16_le(const uint8_t* string, uint32_t length, int32_t* code_point) {
    return frost_ts_decode_utf16(string, length, code_point, 0);
}

// UTF-16 big-endian (TSInputEncodingUTF16BE). O(1).
static inline uint32_t ts_decode_utf16_be(const uint8_t* string, uint32_t length, int32_t* code_point) {
    return frost_ts_decode_utf16(string, length, code_point, 1);
}

#ifdef __cplusplus
}
#endif

#endif // FROST_TS_UNICODE_H
