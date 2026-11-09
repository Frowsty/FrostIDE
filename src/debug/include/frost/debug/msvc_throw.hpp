// SPDX-License-Identifier: Apache-2.0
//
// MSVC C++ throw decoding for type-filtered exception breakpoints (DBG-2b): the pure parts of reading
// which type a program built for the MSVC ABI throws, from the ThrowInfo that _CxxThrowException gets.
/*
 * The MSVC ABI (x64 and ARM64; as LLVM's clang/lib/CodeGen/MicrosoftCXXABI.cpp emits it, Apache-2.0
 * WITH LLVM-exception, getThrowInfo, getCatchableTypeArray, getCatchableType). A throw calls
 *     _CxxThrowException(void* object, const ThrowInfo* info)
 * (x64: info in rdx; ARM64: in x1). Every "pointer" below is a 32-bit image-relative offset (RVA)
 * from the base of the module that holds the ThrowInfo:
 *     ThrowInfo           { u32 attributes; i32 unwind; i32 forwardCompat; i32 catchableTypeArray; }
 *     CatchableTypeArray  { i32 count; i32 catchableTypes[count]; }   // [0] is the thrown type itself,
 *                                                                     // then its public bases.
 *     CatchableType       { u32 properties; i32 typeDescriptor; ... }
 *     TypeDescriptor      { void* vftable; void* spare; char name[]; } // name at +16 on 64-bit targets
 * The name is the type's decorated RTTI name: ".?AVruntime_error@std@@" for class std::runtime_error,
 * ".?AU" for a struct, ".?AT" a union, ".?AW4" an enum, ".H" for int, ".PEBD" for const char*.
 *
 * The session reads these structures with DAP readMemory (DebugSession, "Exception filters"); this
 * header holds the parsing and matching, so they are testable without an adapter. All functions are
 * pure and thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::debug::msvc {

// Byte offsets in the structures of the header comment.
inline constexpr std::uint64_t kThrowInfoSize = 16;
inline constexpr std::uint64_t kThrowInfoCatchableTypeArray = 12;
inline constexpr std::uint64_t kCatchableTypeTypeDescriptor = 4;
inline constexpr std::uint64_t kTypeDescriptorName64 = 16;
// The longest decorated name read; longer ones are cut there and fail to undecorate.
inline constexpr std::uint64_t kMaxTypeNameBytes = 512;
// The PE headers read from a module's base (the DOS header and e_lfanew's NT headers fit).
inline constexpr std::uint64_t kImageHeaderBytes = 0x400;

// IMAGE_FILE_MACHINE_* values that use the 64-bit layout of the header comment.
inline constexpr std::uint16_t kMachineAmd64 = 0x8664;
inline constexpr std::uint16_t kMachineArm64 = 0xAA64;

// What the PE headers at a module's base say.
struct ImageHeader {
    std::uint16_t machine = 0;
    std::uint32_t sizeOfImage = 0;
};

/*
 * Parses the DOS and NT headers from the first bytes of a mapped image (at least the bytes up to
 * e_lfanew + 84). Errors: Corrupt (no "MZ", no "PE\0\0", or the bytes end before SizeOfImage).
 */
[[nodiscard]] Result<ImageHeader> parseImageHeader(std::string_view bytes);

// Little-endian 32-bit values at `offset`; nullopt when `bytes` is too short.
[[nodiscard]] std::optional<std::uint32_t> readU32(std::string_view bytes, std::size_t offset);

/*
 * The address a debugger shows as a value: "0x00007FF7DFCB2198", "0x000000000015fbe0 -> ..."
 * or a decimal number; leading spaces are skipped. nullopt when it starts with neither.
 */
[[nodiscard]] std::optional<std::uint64_t> parseAddress(std::string_view value);

// The NUL-terminated name at the start of `bytes` (all of it when there is no NUL).
[[nodiscard]] std::string_view cString(std::string_view bytes);

/*
 * The C++ spelling of a decorated RTTI type name (header comment): ".?AVruntime_error@std@@" ->
 * "std::runtime_error", ".?AUUserError@@" -> "UserError", ".H" -> "int", ".PEBD" -> "const char *".
 * Handles classes, structs, unions and enums with namespace and class qualifiers (and their
 * back-references), the built-in types and pointers to any of these. Templates and other rare
 * forms give nullopt; callers then show and match the decorated name.
 */
[[nodiscard]] std::optional<std::string> undecorateTypeName(std::string_view decorated);

// A type name as matching compares it: spaces dropped, a leading "class ", "struct ", "union "
// or "enum " and a leading "::" removed ("class std::runtime_error" -> "std::runtime_error").
[[nodiscard]] std::string normalizeTypeName(std::string_view name);

/*
 * Whether a throw of the type `undecorated` (decorated RTTI name `decorated`) is one of `filters`.
 * A filter matches the thrown type exactly, by its C++ name or by its decorated name, never a base
 * or derived class: a filter for std::runtime_error does not stop for std::range_error. An empty
 * list matches every throw.
 */
[[nodiscard]] bool throwTypeMatches(const std::vector<std::string>& filters, std::string_view undecorated,
                                    std::string_view decorated);

/*
 * The filters a user typed: names separated by ',' or ';' (or new lines), trimmed, empty ones
 * dropped, duplicates (after normalizeTypeName) removed, in their first order.
 */
[[nodiscard]] std::vector<std::string> splitTypeList(std::string_view text);

} // namespace frost::debug::msvc
