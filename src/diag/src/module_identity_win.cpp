// SPDX-License-Identifier: Apache-2.0
//
// Module identity on Windows (frost/diag/module_identity.hpp): the loaded image containing an
// address and its CodeView RSDS record, read from the mapped PE headers as the PE/COFF
// specification describes them (the debug directory, IMAGE_DEBUG_TYPE_CODEVIEW).

#include "frost/diag/module_identity.hpp"
#include "frost/io/utf.hpp"

#include "win_image.hpp"

#include <cstring>
#include <format>

#include <windows.h>

namespace frost::diag {
namespace {

// The CodeView 7.0 record ("RSDS") the linker points the debug directory at.
struct RsdsRecord {
    DWORD signature;
    GUID guid;
    DWORD age;
    char pdbPath[1];
};

constexpr DWORD kRsdsSignature = 0x53445352; // 'RSDS'

std::string guidText(const GUID& guid) {
    return std::format("{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", guid.Data1, guid.Data2,
                       guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
                       guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

// Reads the identity of the image mapped at `base` (a loaded module, so the headers and the
// debug data are mapped at their RVAs).
void readImage(const unsigned char* base, const detail::ImageView& image, ModuleIdentity& module) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }
    module.size = image.size;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) {
        return;
    }
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        !image.contains(image.base + directory.VirtualAddress, directory.Size)) {
        return;
    }
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t i = 0; i < count; ++i) {
        const IMAGE_DEBUG_DIRECTORY& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.AddressOfRawData == 0 ||
            entry.SizeOfData < offsetof(RsdsRecord, pdbPath) + 1 ||
            !image.contains(image.base + entry.AddressOfRawData, entry.SizeOfData)) {
            continue;
        }
        const auto* record = reinterpret_cast<const RsdsRecord*>(base + entry.AddressOfRawData);
        if (record->signature != kRsdsSignature) {
            continue;
        }
        module.kind = ModuleIdentity::Kind::Pdb70;
        module.guid = guidText(record->guid);
        module.age = record->age;
        const std::size_t maxPath = entry.SizeOfData - offsetof(RsdsRecord, pdbPath);
        module.pdb.assign(record->pdbPath, strnlen(record->pdbPath, maxPath));
        return;
    }
}

} // namespace

Result<ModuleIdentity> moduleForAddress(std::uintptr_t address) {
    // A handle with UNCHANGED_REFCOUNT can be unmapped as soon as the loader lookup returns.
    // Keep an ordinary temporary reference until every PE/PDB byte and the path have been copied.
    auto reference = detail::ImageReference::acquire(address);
    if (!reference) {
        return fail(ErrorCode::NotFound, std::format("no module contains 0x{:x}", address));
    }
    const HMODULE handle = reference->get();
    const auto image = detail::imageView(*reference);
    if (!image || !image->contains(address, 1)) {
        return fail(ErrorCode::Unsupported, "module has unsupported PE headers");
    }
    ModuleIdentity module;
    module.base = reinterpret_cast<std::uintptr_t>(handle);
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            path.clear();
            break;
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    module.path = io::fromWide(path).value_or(std::string());
    readImage(reinterpret_cast<const unsigned char*>(handle), *image, module);
    return module;
}

} // namespace frost::diag
