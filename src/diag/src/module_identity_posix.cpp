// SPDX-License-Identifier: Apache-2.0
//
// Module identity on Linux (frost/diag/module_identity.hpp): the loaded object containing an
// address (dl_iterate_phdr) and its GNU build-id, from the PT_NOTE segments as the ELF gABI
// and the GNU build-id note (NT_GNU_BUILD_ID, name "GNU") define them.

#include "frost/diag/module_identity.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include <elf.h>
#include <link.h>
#include <unistd.h>

namespace frost::diag {
namespace {

struct Search {
    std::uintptr_t address;
    bool found;
    ModuleIdentity module;
};

std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        text.push_back(kDigits[bytes[i] >> 4]);
        text.push_back(kDigits[bytes[i] & 0xF]);
    }
    return text;
}

std::size_t align4(std::size_t value) noexcept { return (value + 3) & ~std::size_t{3}; }

// The build-id note among the notes in [data, data + size), or "".
std::string buildIdFromNotes(const unsigned char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset + sizeof(ElfW(Nhdr)) <= size) {
        ElfW(Nhdr) header;
        std::memcpy(&header, data + offset, sizeof(header));
        const std::size_t nameOffset = offset + sizeof(header);
        const std::size_t descOffset = nameOffset + align4(header.n_namesz);
        const std::size_t next = descOffset + align4(header.n_descsz);
        if (next > size || descOffset > size) {
            break;
        }
        if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4 && std::memcmp(data + nameOffset, "GNU", 4) == 0 &&
            header.n_descsz > 0) {
            return hex(data + descOffset, header.n_descsz);
        }
        offset = next;
    }
    return {};
}

std::string selfExecutablePath() {
    std::string path(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
    if (length <= 0) {
        return {};
    }
    path.resize(static_cast<std::size_t>(length));
    return path;
}

int visit(dl_phdr_info* info, std::size_t, void* argument) {
    auto& search = *static_cast<Search*>(argument);
    std::uintptr_t end = 0;
    bool contains = false;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD) {
            continue;
        }
        const std::uintptr_t begin = info->dlpi_addr + segment.p_vaddr;
        const std::uintptr_t segmentEnd = begin + segment.p_memsz;
        end = std::max<std::uintptr_t>(end, segment.p_vaddr + segment.p_memsz);
        if (search.address >= begin && search.address < segmentEnd) {
            contains = true;
        }
    }
    if (!contains) {
        return 0;
    }
    ModuleIdentity& module = search.module;
    // Offsets are reported from the load bias, so they equal the object's virtual addresses,
    // which is what llvm-symbolizer and addr2line expect for any ELF object.
    module.base = info->dlpi_addr;
    module.size = end;
    module.path =
        info->dlpi_name != nullptr && info->dlpi_name[0] != '\0' ? std::string(info->dlpi_name) : selfExecutablePath();
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE) {
            continue;
        }
        std::string id = buildIdFromNotes(reinterpret_cast<const unsigned char*>(info->dlpi_addr + segment.p_vaddr),
                                          segment.p_memsz);
        if (!id.empty()) {
            module.kind = ModuleIdentity::Kind::GnuBuildId;
            module.buildId = std::move(id);
            break;
        }
    }
    search.found = true;
    return 1;
}

} // namespace

Result<ModuleIdentity> moduleForAddress(std::uintptr_t address) {
    Search search{address, false, {}};
    (void)dl_iterate_phdr(&visit, &search);
    if (!search.found) {
        return fail(ErrorCode::NotFound, std::format("no module contains 0x{:x}", address));
    }
    return std::move(search.module);
}

} // namespace frost::diag
