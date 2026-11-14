// SPDX-License-Identifier: Apache-2.0
//
// Which loaded module an address belongs to, and the identity a symbol server or debuginfod
// keys that module's symbols by: the CodeView RSDS record (PDB GUID and age) of a PE image,
// from the PE/COFF specification's debug directory; the GNU build-id note of an ELF object
// (ELF gABI, PT_NOTE / NT_GNU_BUILD_ID). Used by freeze reports (ADR 0022, D-I).
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace frost::diag {

struct ModuleIdentity {
    enum class Kind : std::uint8_t { None, Pdb70, GnuBuildId };

    std::string path;        // UTF-8 path of the image file.
    std::uintptr_t base = 0; // Load address.
    std::uint64_t size = 0;  // Bytes mapped from base (SizeOfImage / the highest PT_LOAD end).
    Kind kind = Kind::None;
    std::string guid;      // Pdb70: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", upper case.
    std::uint32_t age = 0; // Pdb70.
    std::string pdb;       // Pdb70: the PDB path recorded by the linker.
    std::string buildId;   // GnuBuildId: lower-case hex.
};

/*
 * The module containing `address`. Errors: NotFound (no loaded module contains it), Unsupported
 * (Windows: unsupported PE headers). Windows retains a module reference until its data is copied. Takes the
 * loader lock (GetModuleHandleExW / dl_iterate_phdr), so never call it while another thread of
 * this process is suspended.
 */
[[nodiscard]] Result<ModuleIdentity> moduleForAddress(std::uintptr_t address);

// Every address mapped to the index of its module in `modules` (appending new modules), or -1.
class ModuleTable {
public:
    // The index of the module containing `address`, resolving and caching it on first use.
    [[nodiscard]] int indexOf(std::uintptr_t address);
    [[nodiscard]] const std::vector<ModuleIdentity>& modules() const noexcept { return m_modules; }

private:
    std::vector<ModuleIdentity> m_modules;
};

} // namespace frost::diag
