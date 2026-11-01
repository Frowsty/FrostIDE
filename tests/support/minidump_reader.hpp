// SPDX-License-Identifier: Apache-2.0
//
// A minimal reader for the minidumps crashpad writes, for tests: the header and stream
// directory, the exception stream, the module list with each module's CodeView record, the
// thread list with each thread's x86-64 context and stack, and crashpad's own info stream (the
// process's simple annotations and every module's annotation objects). Layouts follow
// Microsoft's public minidump documentation (minidumpapiset.h: MINIDUMP_HEADER,
// MINIDUMP_DIRECTORY, MINIDUMP_EXCEPTION_STREAM, MINIDUMP_MODULE, MINIDUMP_THREAD) and
// crashpad's minidump_extensions.h (Apache-2.0: MinidumpCrashpadInfo, MinidumpModuleCrashpadInfo,
// MinidumpAnnotation, the "BpEL" ELF CodeView record). Every offset is bounds-checked; a dump
// that does not parse yields an error string, never undefined behaviour.
//
// Header-only; test code only; not thread-safe (each reader owns its bytes).
#pragma once

#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::test {

struct MinidumpModule {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::string name; // UTF-8 (converted from the dump's UTF-16).
    // CodeView identity: "RSDS" (PDB 7.0) or "BpEL" (crashpad's ELF build-id record).
    std::string cvSignature;
    std::string pdbGuid; // "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" for RSDS.
    std::uint32_t pdbAge = 0;
    std::string pdbPath;
    std::string buildId; // Lower-case hex, for BpEL.
};

struct MinidumpThread {
    std::uint32_t id = 0;
    std::uint64_t rip = 0;
    std::uint64_t rsp = 0;
    std::uint64_t stackStart = 0;
    std::string stack; // The captured stack bytes from stackStart.
};

struct MinidumpException {
    std::uint32_t threadId = 0;
    std::uint32_t code = 0;
    std::uint32_t flags = 0;
    std::uint64_t address = 0;
    std::vector<std::uint64_t> information;
    std::uint64_t contextRip = 0;
};

class MinidumpReader {
public:
    // Parses `bytes`; error() is empty on success.
    explicit MinidumpReader(std::string bytes) : m_bytes(std::move(bytes)) { m_error = parse(); }

    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] const std::optional<MinidumpException>& exception() const noexcept { return m_exception; }
    [[nodiscard]] const std::vector<MinidumpModule>& modules() const noexcept { return m_modules; }
    [[nodiscard]] const std::vector<MinidumpThread>& threads() const noexcept { return m_threads; }
    // The process annotations (the handler's --annotation=key=value).
    [[nodiscard]] const std::map<std::string, std::string>& simpleAnnotations() const noexcept { return m_simple; }
    // Every module's annotation objects (crashpad::Annotation), by name; string values only.
    [[nodiscard]] const std::map<std::string, std::string>& annotations() const noexcept { return m_annotations; }

    // The module containing `address`, or nullptr.
    [[nodiscard]] const MinidumpModule* moduleAt(std::uint64_t address) const noexcept {
        for (const MinidumpModule& module : m_modules) {
            if (address >= module.base && address - module.base < module.size) {
                return &module;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const MinidumpThread* thread(std::uint32_t id) const noexcept {
        for (const MinidumpThread& candidate : m_threads) {
            if (candidate.id == id) {
                return &candidate;
            }
        }
        return nullptr;
    }

private:
    static constexpr std::uint32_t kSignature = 0x504d444d; // "MDMP"
    static constexpr std::uint32_t kThreadList = 3;
    static constexpr std::uint32_t kModuleList = 4;
    static constexpr std::uint32_t kException = 6;
    static constexpr std::uint32_t kCrashpadInfo = 0x43500001;
    static constexpr std::size_t kRipOffset = 0xF8; // CONTEXT / MinidumpContextAMD64.
    static constexpr std::size_t kRspOffset = 0x98;

    template<class T>
    bool read(std::uint64_t offset, T& out) const noexcept {
        if (offset > m_bytes.size() || m_bytes.size() - offset < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, m_bytes.data() + offset, sizeof(T));
        return true;
    }

    [[nodiscard]] std::uint32_t u32(std::uint64_t offset) const noexcept {
        std::uint32_t value = 0;
        return read(offset, value) ? value : 0;
    }
    [[nodiscard]] std::uint64_t u64(std::uint64_t offset) const noexcept {
        std::uint64_t value = 0;
        return read(offset, value) ? value : 0;
    }

    [[nodiscard]] bool span(std::uint64_t offset, std::uint64_t size) const noexcept {
        return offset <= m_bytes.size() && m_bytes.size() - offset >= size;
    }

    // MINIDUMP_STRING: a byte length, then UTF-16LE.
    [[nodiscard]] std::string utf16String(std::uint32_t rva) const {
        const std::uint32_t length = u32(rva);
        if (!span(rva + 4ULL, length)) {
            return {};
        }
        std::string out;
        for (std::uint32_t i = 0; i + 1 < length; i += 2) {
            std::uint32_t unit = static_cast<unsigned char>(m_bytes[rva + 4 + i]) |
                                 (static_cast<std::uint32_t>(static_cast<unsigned char>(m_bytes[rva + 5 + i])) << 8);
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < length) {
                const std::uint32_t low =
                    static_cast<unsigned char>(m_bytes[rva + 6 + i]) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(m_bytes[rva + 7 + i])) << 8);
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
            if (unit < 0x80) {
                out.push_back(static_cast<char>(unit));
            } else if (unit < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            } else if (unit < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (unit >> 18)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            }
        }
        return out;
    }

    // crashpad's MinidumpUTF8String / MinidumpByteArray: a byte length, then the bytes.
    [[nodiscard]] std::string byteString(std::uint32_t rva) const {
        const std::uint32_t length = u32(rva);
        if (!span(rva + 4ULL, length)) {
            return {};
        }
        return m_bytes.substr(rva + 4ULL, length);
    }

    void readCodeView(std::uint32_t size, std::uint32_t rva, MinidumpModule& module) const {
        if (size < 4 || !span(rva, size)) {
            return;
        }
        // The signatures are 32-bit values: 'RSDS' is stored as the bytes "RSDS", crashpad's
        // 'BpEL' (0x4270454c) as "LEpB".
        const std::uint32_t signature = u32(rva);
        module.cvSignature = signature == 0x53445352u ? "RSDS" : signature == 0x4270454cu ? "BpEL" : "";
        if (module.cvSignature == "RSDS" && size >= 24) {
            const std::uint32_t data1 = u32(rva + 4ULL);
            std::uint16_t data2 = 0;
            std::uint16_t data3 = 0;
            (void)read(rva + 8ULL, data2);
            (void)read(rva + 10ULL, data3);
            const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(m_bytes[rva + 12 + k]); };
            module.pdbGuid =
                std::format("{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", data1, data2,
                            data3, byte(0), byte(1), byte(2), byte(3), byte(4), byte(5), byte(6), byte(7));
            module.pdbAge = u32(rva + 20ULL);
            const std::string path = m_bytes.substr(rva + 24ULL, size - 24);
            module.pdbPath = path.substr(0, path.find('\0'));
        } else if (module.cvSignature == "BpEL") {
            static constexpr char kDigits[] = "0123456789abcdef";
            for (std::uint32_t i = 4; i < size; ++i) {
                const auto b = static_cast<unsigned char>(m_bytes[rva + i]);
                module.buildId.push_back(kDigits[b >> 4]);
                module.buildId.push_back(kDigits[b & 0xF]);
            }
        }
    }

    void readAnnotationObjects(std::uint32_t size, std::uint32_t rva) {
        if (size < 4 || !span(rva, size)) {
            return;
        }
        const std::uint32_t count = u32(rva);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t entry = rva + 4ULL + i * 12ULL;
            if (!span(entry, 12)) {
                return;
            }
            const std::string name = byteString(u32(entry));
            std::uint16_t type = 0;
            (void)read(entry + 4, type);
            if (type == 1) { // crashpad::Annotation::Type::kString
                m_annotations[name.substr(0, name.find('\0'))] = byteString(u32(entry + 8));
            }
        }
    }

    void readCrashpadInfo(std::uint32_t rva) {
        // version, report_id (16), client_id (16), simple_annotations {size, rva}, module_list {size, rva}.
        const std::uint32_t simpleSize = u32(rva + 36ULL);
        const std::uint32_t simpleRva = u32(rva + 40ULL);
        if (simpleSize >= 4 && span(simpleRva, simpleSize)) {
            const std::uint32_t count = u32(simpleRva);
            for (std::uint32_t i = 0; i < count && span(simpleRva + 4ULL + i * 8ULL, 8); ++i) {
                const std::string key = byteString(u32(simpleRva + 4ULL + i * 8ULL));
                const std::string value = byteString(u32(simpleRva + 8ULL + i * 8ULL));
                m_simple[key.substr(0, key.find('\0'))] = value.substr(0, value.find('\0'));
            }
        }
        const std::uint32_t listSize = u32(rva + 44ULL);
        const std::uint32_t listRva = u32(rva + 48ULL);
        if (listSize < 4 || !span(listRva, listSize)) {
            return;
        }
        const std::uint32_t count = u32(listRva);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t link = listRva + 4ULL + i * 12ULL; // index, {size, rva}
            if (!span(link, 12)) {
                return;
            }
            const std::uint32_t infoRva = u32(link + 8);
            // MinidumpModuleCrashpadInfo: version, list_annotations, simple_annotations, annotation_objects.
            readAnnotationObjects(u32(infoRva + 20ULL), u32(infoRva + 24ULL));
        }
    }

    std::string parse() {
        if (u32(0) != kSignature) {
            return "not a minidump (no MDMP signature)";
        }
        const std::uint32_t streams = u32(8);
        const std::uint32_t directory = u32(12);
        if (!span(directory, streams * 12ULL)) {
            return "truncated stream directory";
        }
        for (std::uint32_t i = 0; i < streams; ++i) {
            const std::uint64_t entry = directory + i * 12ULL;
            const std::uint32_t type = u32(entry);
            const std::uint32_t size = u32(entry + 4);
            const std::uint32_t rva = u32(entry + 8);
            if (!span(rva, size)) {
                return std::format("stream {} out of range", type);
            }
            if (type == kException && size >= 168) {
                MinidumpException exception;
                exception.threadId = u32(rva);
                exception.code = u32(rva + 8ULL);
                exception.flags = u32(rva + 12ULL);
                exception.address = u64(rva + 24ULL);
                const std::uint32_t parameters = std::min<std::uint32_t>(u32(rva + 32ULL), 15);
                for (std::uint32_t p = 0; p < parameters; ++p) {
                    exception.information.push_back(u64(rva + 40ULL + p * 8ULL));
                }
                exception.contextRip = u64(u32(rva + 164ULL) + kRipOffset);
                m_exception = exception;
            } else if (type == kModuleList && size >= 4) {
                const std::uint32_t count = u32(rva);
                for (std::uint32_t m = 0; m < count; ++m) {
                    const std::uint64_t at = rva + 4ULL + m * 108ULL;
                    if (!span(at, 108)) {
                        return "truncated module list";
                    }
                    MinidumpModule module;
                    module.base = u64(at);
                    module.size = u32(at + 8);
                    module.name = utf16String(u32(at + 20));
                    readCodeView(u32(at + 76), u32(at + 80), module);
                    m_modules.push_back(std::move(module));
                }
            } else if (type == kThreadList && size >= 4) {
                const std::uint32_t count = u32(rva);
                for (std::uint32_t t = 0; t < count; ++t) {
                    const std::uint64_t at = rva + 4ULL + t * 48ULL;
                    if (!span(at, 48)) {
                        return "truncated thread list";
                    }
                    MinidumpThread thread;
                    thread.id = u32(at);
                    thread.stackStart = u64(at + 24);
                    const std::uint32_t stackSize = u32(at + 32);
                    const std::uint32_t stackRva = u32(at + 36);
                    if (span(stackRva, stackSize)) {
                        thread.stack = m_bytes.substr(stackRva, stackSize);
                    }
                    const std::uint32_t contextRva = u32(at + 44);
                    thread.rip = u64(contextRva + kRipOffset);
                    thread.rsp = u64(contextRva + kRspOffset);
                    m_threads.push_back(std::move(thread));
                }
            } else if (type == kCrashpadInfo && size >= 52) {
                readCrashpadInfo(rva);
            }
        }
        return {};
    }

    std::string m_bytes;
    std::string m_error;
    std::optional<MinidumpException> m_exception;
    std::vector<MinidumpModule> m_modules;
    std::vector<MinidumpThread> m_threads;
    std::map<std::string, std::string> m_simple;
    std::map<std::string, std::string> m_annotations;
};

} // namespace frost::test
