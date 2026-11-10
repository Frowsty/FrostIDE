// SPDX-License-Identifier: Apache-2.0
//
// Self-contained IEEE 802.3 CRC-32 and uncompressed llvm::compression::* shim for frost-clang
// (CLANG-0, ADR 0083). The pinned LLVM 23.1.4 SDK archives were built on a release builder with
// external zlib/zstd; providing the !LLVM_ENABLE_ZLIB / !LLVM_ENABLE_ZSTD definitions here keeps
// LLVMSupport's Compression.cpp.obj out of the link and satisfies CRC.cpp.obj without requiring
// external compression libraries in /MT or fuzz trees.

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::array<std::uint32_t, 256> makeCrc32Table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrc32Table = makeCrc32Table();

} // namespace

// Called by LLVMSupport's CRC.cpp.obj (zlib's crc32 signature: uLong crc32(uLong, const Bytef*, uInt)).
extern "C" unsigned long crc32(unsigned long crc, const unsigned char* buf, unsigned int len) {
    if (buf == nullptr) {
        return 0UL;
    }
    auto state = static_cast<std::uint32_t>(crc) ^ 0xFFFFFFFFU;
    for (unsigned int i = 0; i < len; ++i) {
        const std::uint32_t idx = (state ^ buf[i]) & 0xFFU;
        state = kCrc32Table[idx] ^ (state >> 8U);
    }
    return static_cast<unsigned long>(state ^ 0xFFFFFFFFU);
}

namespace llvm::compression {

namespace zlib {

bool isAvailable() { return false; }

void compress(ArrayRef<std::uint8_t> /*input*/, SmallVectorImpl<std::uint8_t>& /*compressedBuffer*/, int /*level*/) {
    llvm_unreachable("zlib::compress is unavailable in frost-clang");
}

Error decompress(ArrayRef<std::uint8_t> /*input*/, std::uint8_t* /*output*/, std::size_t& /*uncompressedSize*/) {
    return createStringError(inconvertibleErrorCode(), "zlib is unavailable in frost-clang");
}

Error decompress(ArrayRef<std::uint8_t> /*input*/, SmallVectorImpl<std::uint8_t>& /*output*/,
                 std::size_t /*uncompressedSize*/) {
    return createStringError(inconvertibleErrorCode(), "zlib is unavailable in frost-clang");
}

} // namespace zlib

namespace zstd {

bool isAvailable() { return false; }

void compress(ArrayRef<std::uint8_t> /*input*/, SmallVectorImpl<std::uint8_t>& /*compressedBuffer*/, int /*level*/,
              bool /*enableLdm*/) {
    llvm_unreachable("zstd::compress is unavailable in frost-clang");
}

Error decompress(ArrayRef<std::uint8_t> /*input*/, std::uint8_t* /*output*/, std::size_t& /*uncompressedSize*/) {
    return createStringError(inconvertibleErrorCode(), "zstd is unavailable in frost-clang");
}

Error decompress(ArrayRef<std::uint8_t> /*input*/, SmallVectorImpl<std::uint8_t>& /*output*/,
                 std::size_t /*uncompressedSize*/) {
    return createStringError(inconvertibleErrorCode(), "zstd is unavailable in frost-clang");
}

} // namespace zstd

const char* getReasonIfUnsupported(Format format) {
    switch (format) {
    case Format::Zlib: return "LLVM_ENABLE_ZLIB is disabled in frost-clang";
    case Format::Zstd: return "LLVM_ENABLE_ZSTD is disabled in frost-clang";
    }
    return "unsupported compression format";
}

void compress(Params params, ArrayRef<std::uint8_t> input, SmallVectorImpl<std::uint8_t>& output) {
    switch (params.format) {
    case Format::Zlib: zlib::compress(input, output, params.level); return;
    case Format::Zstd: zstd::compress(input, output, params.level, params.zstdEnableLdm); return;
    }
}

Error decompress(DebugCompressionType type, ArrayRef<std::uint8_t> input, std::uint8_t* output,
                 std::size_t uncompressedSize) {
    switch (formatFor(type)) {
    case Format::Zlib: return zlib::decompress(input, output, uncompressedSize);
    case Format::Zstd: return zstd::decompress(input, output, uncompressedSize);
    }
    return createStringError(inconvertibleErrorCode(), "unsupported compression format");
}

Error decompress(Format format, ArrayRef<std::uint8_t> input, SmallVectorImpl<std::uint8_t>& output,
                 std::size_t uncompressedSize) {
    switch (format) {
    case Format::Zlib: return zlib::decompress(input, output, uncompressedSize);
    case Format::Zstd: return zstd::decompress(input, output, uncompressedSize);
    }
    return createStringError(inconvertibleErrorCode(), "unsupported compression format");
}

Error decompress(DebugCompressionType type, ArrayRef<std::uint8_t> input, SmallVectorImpl<std::uint8_t>& output,
                 std::size_t uncompressedSize) {
    return decompress(formatFor(type), input, output, uncompressedSize);
}

} // namespace llvm::compression
