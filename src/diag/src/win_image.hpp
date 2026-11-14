// SPDX-License-Identifier: Apache-2.0
//
// Scoped mapped-image ownership and metadata bounds for Windows diagnostics. Acquiring or releasing
// a reference may enter the loader: callers must first resume every thread they suspended.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include <windows.h>

namespace frost::diag::detail {

class ImageReference {
public:
    [[nodiscard]] static std::optional<ImageReference> acquire(std::uintptr_t address) noexcept {
        HMODULE handle = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &handle)) {
            return std::nullopt;
        }
        return ImageReference(handle);
    }

    ImageReference(const ImageReference&) = delete;
    ImageReference& operator=(const ImageReference&) = delete;
    ImageReference(ImageReference&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    ImageReference& operator=(ImageReference&&) = delete;
    ~ImageReference() {
        if (m_handle != nullptr) {
            FreeLibrary(m_handle);
        }
    }
    [[nodiscard]] HMODULE get() const noexcept { return m_handle; }

private:
    explicit ImageReference(HMODULE handle) noexcept : m_handle(handle) {}
    HMODULE m_handle;
};

// Valid only while the corresponding ImageReference remains alive.
struct ImageView {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    IMAGE_DATA_DIRECTORY exceptions{};

    [[nodiscard]] bool contains(std::uintptr_t address, std::size_t bytes) const noexcept {
        return address >= base && address - base <= size && bytes <= size - (address - base);
    }

    [[nodiscard]] bool ownsFunctionEntry(const RUNTIME_FUNCTION* entry, DWORD64 imageBase) const noexcept {
        if (imageBase != base || exceptions.VirtualAddress == 0 || exceptions.Size < sizeof(RUNTIME_FUNCTION) ||
            exceptions.VirtualAddress > size || exceptions.Size > size - exceptions.VirtualAddress) {
            return false;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(entry);
        const std::uintptr_t table = base + exceptions.VirtualAddress;
        return address >= table && address - table <= exceptions.Size - sizeof(RUNTIME_FUNCTION) &&
               (address - table) % sizeof(RUNTIME_FUNCTION) == 0;
    }
};

[[nodiscard]] inline std::optional<ImageView> imageView(const ImageReference& reference) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(reference.get());
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reference.get(), &region, sizeof(region)) == 0 || region.AllocationBase != reference.get() ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || region.RegionSize < sizeof(IMAGE_DOS_HEADER)) {
        return std::nullopt;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || region.RegionSize < sizeof(IMAGE_NT_HEADERS64) ||
        static_cast<std::size_t>(dos->e_lfanew) > region.RegionSize - sizeof(IMAGE_NT_HEADERS64)) {
        return std::nullopt;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt->OptionalHeader.SizeOfImage == 0) {
        return std::nullopt;
    }
    ImageView view{.base = base, .size = nt->OptionalHeader.SizeOfImage, .exceptions = {}};
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        view.exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    }
    return view;
}

[[nodiscard]] inline DWORD64 unwindRegister(const CONTEXT& context, unsigned index) noexcept {
    switch (index) {
    case 0: return context.Rax;
    case 1: return context.Rcx;
    case 2: return context.Rdx;
    case 3: return context.Rbx;
    case 4: return context.Rsp;
    case 5: return context.Rbp;
    case 6: return context.Rsi;
    case 7: return context.Rdi;
    case 8: return context.R8;
    case 9: return context.R9;
    case 10: return context.R10;
    case 11: return context.R11;
    case 12: return context.R12;
    case 13: return context.R13;
    case 14: return context.R14;
    case 15: return context.R15;
    default: return 0;
    }
}

// Preflight only metadata ownership and frame-register bounds, never execute unwind codes. The
// PE/COFF x64 UNWIND_INFO layout is documented at https://learn.microsoft.com/cpp/build/exception-handling-x64.
// Native RtlVirtualUnwind2 performs the unwind with explicit stack limits. Indirect/dynamic tables
// and unbounded/cyclic chains are unsupported rather than dereferenced without a lifetime owner.
[[nodiscard]] inline bool imageUnwindSupported(const ImageView& image, const RUNTIME_FUNCTION* function,
                                               DWORD64 imageBase, const CONTEXT& context, DWORD64 low, DWORD64 high,
                                               const char** rejection = nullptr) noexcept {
    const auto reject = [rejection](const char* reason) {
        if (rejection != nullptr) {
            *rejection = reason;
        }
        return false;
    };
    if (!image.ownsFunctionEntry(function, imageBase) || context.Rsp < low || context.Rsp > high ||
        high - context.Rsp < sizeof(DWORD64)) {
        return reject("function ownership or initial stack bounds");
    }
    RUNTIME_FUNCTION entry = *function;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (entry.BeginAddress >= entry.EndAddress || entry.EndAddress > image.size ||
            (entry.UnwindData & RUNTIME_FUNCTION_INDIRECT) != 0 || entry.UnwindData > image.size ||
            !image.contains(image.base + entry.UnwindData, 4)) {
            return reject("function range or indirect metadata");
        }
        const auto* info = reinterpret_cast<const unsigned char*>(image.base + entry.UnwindData);
        const unsigned version = info[0] & 7U;
        const unsigned flags = info[0] >> 3U;
        if (version != 1 && version != 2) {
            return reject("unwind version");
        }
        const std::size_t codesEnd = (4 + static_cast<std::size_t>(info[2]) * 2 + 3) & ~std::size_t{3};
        const unsigned frameRegister = info[3] & 15U;
        if (frameRegister != 0) {
            const DWORD64 frame = unwindRegister(context, frameRegister);
            const DWORD64 offset = static_cast<DWORD64>(info[3] >> 4U) * 16;
            if (frame < low || frame > high || frame - low < offset || high - (frame - offset) < sizeof(DWORD64)) {
                return reject("frame register outside copied stack");
            }
        }
        if (!image.contains(reinterpret_cast<std::uintptr_t>(info), codesEnd)) {
            return reject("unwind code bounds");
        }
        if ((flags & UNW_FLAG_CHAININFO) == 0) {
            if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) != 0 &&
                !image.contains(reinterpret_cast<std::uintptr_t>(info), codesEnd + sizeof(DWORD))) {
                return reject("handler metadata bounds");
            }
            return true;
        }
        if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) != 0 ||
            !image.contains(reinterpret_cast<std::uintptr_t>(info), codesEnd + sizeof(RUNTIME_FUNCTION))) {
            return reject("chain metadata flags or bounds");
        }
        std::memcpy(&entry, info + codesEnd, sizeof(entry));
    }
    return reject("chain depth");
}

// Public Windows 11 API, dynamically resolved because the pinned SDK has no declaration/import.
// Exact signature and bounds/status contract: https://learn.microsoft.com/windows/win32/devnotes/rtlvirtualunwind2.
using VirtualUnwind2 = LONG(NTAPI*)(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PBOOLEAN, PVOID*, PDWORD64,
                                    PKNONVOLATILE_CONTEXT_POINTERS, PDWORD64, PDWORD64, PEXCEPTION_ROUTINE*, DWORD);

[[nodiscard]] inline VirtualUnwind2 virtualUnwind2() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll == nullptr ? nullptr : reinterpret_cast<VirtualUnwind2>(GetProcAddress(ntdll, "RtlVirtualUnwind2"));
}

} // namespace frost::diag::detail
