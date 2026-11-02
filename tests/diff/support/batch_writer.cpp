// SPDX-License-Identifier: Apache-2.0
//
// The git batch writer (batch_writer.hpp).

#include "diff/support/batch_writer.hpp"

#include "diff/support/files.hpp"

#include <format>

namespace frost::diff::test {

std::string caseName(std::size_t index) { return std::format("c{:06}", index); }

Result<void> writeBatch(std::string_view directory, std::span<const GitCase> cases) {
    const std::string a = joinUtf8(directory, "a");
    const std::string b = joinUtf8(directory, "b");
    if (auto made = makeDirectories(a); !made) {
        return made;
    }
    if (auto made = makeDirectories(b); !made) {
        return made;
    }
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const std::string name = caseName(k);
        if (auto written = writeBinaryFile(joinUtf8(a, name), cases[k].a); !written) {
            return written;
        }
        if (auto written = writeBinaryFile(joinUtf8(b, name), cases[k].b); !written) {
            return written;
        }
    }
    return {};
}

Result<void> writeSideA(std::string_view directory, std::span<const GitCase> cases) {
    if (auto made = makeDirectories(directory); !made) {
        return made;
    }
    for (std::size_t k = 0; k < cases.size(); ++k) {
        if (auto written = writeBinaryFile(joinUtf8(directory, caseName(k)), cases[k].a); !written) {
            return written;
        }
    }
    return {};
}

} // namespace frost::diff::test
