// SPDX-License-Identifier: Apache-2.0
//
// The Linux text codecs (frost/text/text_codec.hpp; ADR 0018, D3 and D17): strict glibc iconv
// conversions for the M1 code pages, the platform provider, and the fixed legacy default (D14).
// The only file of frost::text that calls iconv. iconv is part of glibc (libc.so.6), so nothing
// extra is linked and nothing is bundled: the host provides the gconv modules.
/*
 * Strictness: plain converter names from CodePageInfo::iconvName, never //TRANSLIT or //IGNORE,
 * and GCONV_PATH is never set. iconv_open only ever receives a name from the static table, never a
 * string from settings, EditorConfig or file content: glibc's converters have had memory-safety
 * bugs (CVE-2024-2961 in ISO-2022-CN-EXT), and the table limits the exposure to 16 audited names.
 *
 * Each call resets the converter state, converts the input in windows that start at 64 bytes and
 * double after each fully converted window up to 64 KiB (so a call that stops early costs O(bytes
 * consumed + 64), and dense invalid input stays linear), stops at EILSEQ (an undefined character)
 * or EINVAL (an incomplete character at the end of the input), and flushes the converter so that a
 * buffered character is never lost. Output is reserved at 3 bytes per input byte when decoding
 * (the most these pages need) and grows by doubling on E2BIG as a fallback.
 */

#include "frost/io/os_error.hpp"
#include "frost/text/text_codec.hpp"

#include "unicode_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <iconv.h>

namespace frost::text {
namespace {

constexpr std::size_t kFirstWindow = 64;
constexpr std::size_t kMaxWindow = 64 * 1024;
// iconv_open's documented failure value, (iconv_t)-1. A function, not a constant: the cast is not a
// constant expression.
bool isInvalid(iconv_t cd) noexcept { return cd == reinterpret_cast<iconv_t>(-1); }

/*
 * Workaround (ThreadSanitizer, not correctness): serialises iconv_open and iconv_close.
 *   - Why: they load and unload gconv modules through glibc's dynamic loader, which has its own
 *     lock, but ThreadSanitizer cannot see that lock (glibc is not instrumented, and gconv's
 *     internal loads do not go through TSan's dlopen/dlclose interceptors). It reports the
 *     loader's allocations on two threads (_dl_new_object in one thread's iconv_open,
 *     _dl_close_worker in another's iconv_close) as a data race:
 *     EncodingConcurrency.PlatformCodecsInParallel and EncodingStress.LosslessProperty.
 *   - Tool versions: LLVM 23.1.4 TSan with glibc 2.39-0ubuntu8.9 (Ubuntu 24.04, WSL).
 *   - Issue link: none; no upstream report was found on 2026-11-01 (ADR 0018 lists filing one as
 *     a question for the user). No suppression file is used instead.
 *   - Removal condition: drop the mutex once both tests pass under TSan without it on the pinned
 *     toolchain (a TSan that sees glibc's loader lock, or a glibc that loads gconv modules through
 *     the public dlopen), or if a profile shows it contended.
 * It guards nothing of ours. With it, every module load and unload is ordered in a way TSan sees.
 * Each codec opens and closes once, so it is rarely contended; conversions (iconv) run outside it.
 * Constant-initialised and trivially destructible (libstdc++ defines __GTHREAD_MUTEX_INIT on
 * Linux): no global constructor, no exit-time destructor.
 */
constinit std::mutex gGconvMutex;

// A conversion failure other than EILSEQ, EINVAL or E2BIG (ITextCodec: platform failures are
// Internal): io's mapping of errno would read as invalid input, so only its message is kept.
std::unexpected<Error> platformFailure(int error, std::string_view operation) {
    return fail(ErrorCode::Internal,
                std::format("the iconv converter failed: {}", io::errnoError(error, operation).message()));
}

class IconvCodec final : public ITextCodec {
public:
    IconvCodec(const CodePageInfo& info, iconv_t toUtf8, iconv_t fromUtf8) noexcept
        : m_info(&info), m_toUtf8(toUtf8), m_fromUtf8(fromUtf8) {}
    ~IconvCodec() override {
        const std::lock_guard lock(gGconvMutex);
        iconv_close(m_toUtf8);
        iconv_close(m_fromUtf8);
    }
    IconvCodec(const IconvCodec&) = delete ("owns two iconv descriptors");
    IconvCodec& operator=(const IconvCodec&) = delete ("owns two iconv descriptors");

    [[nodiscard]] const CodePageInfo& info() const noexcept override { return *m_info; }

    [[nodiscard]] Result<std::size_t> decodePrefix(std::string_view bytes, std::string& utf8) noexcept override {
        return run(m_toUtf8, bytes, utf8, 3, false);
    }

    [[nodiscard]] Result<std::size_t> encodePrefix(std::string_view utf8, std::string& bytes) noexcept override {
        return run(m_fromUtf8, utf8, bytes, 2, true);
    }

private:
    /*
     * Converts the longest convertible prefix of `in` with `cd`, appending to `out`; returns the
     * input consumed. `perByte` is the output reserve per input byte. `utf8Input` keeps windows
     * at UTF-8 character boundaries (the encode direction), so a window never ends mid-character.
     */
    [[nodiscard]] static Result<std::size_t> run(iconv_t cd, std::string_view in, std::string& out, std::size_t perByte,
                                                 bool utf8Input) noexcept {
        iconv(cd, nullptr, nullptr, nullptr, nullptr); // Reset to the initial state.
        const std::size_t start = out.size();
        std::size_t used = start; // Bytes of `out` holding output.
        std::size_t pos = 0;
        std::size_t window = kFirstWindow;
        bool stopped = false;
        while (pos < in.size() && !stopped) {
            std::size_t end = std::min(in.size(), pos + window);
            if (utf8Input && end < in.size()) {
                const std::size_t floored = detail::floorCharBoundary(in, end);
                end = floored > pos ? floored : pos + detail::utf8SequenceLength(detail::octetAt(in, pos));
            }
            const bool last = end == in.size();
            // glibc's iconv takes char** input but does not write through it.
            char* inPtr = const_cast<char*>(in.data() + pos);
            std::size_t inLeft = end - pos;
            out.resize(used + perByte * inLeft + 16);
            while (true) {
                char* outPtr = out.data() + used;
                std::size_t outLeft = out.size() - used;
                const std::size_t result = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
                used = static_cast<std::size_t>(outPtr - out.data());
                if (result != static_cast<std::size_t>(-1)) {
                    break;
                }
                const int error = errno;
                if (error == E2BIG) {
                    out.resize(out.size() * 2);
                    continue;
                }
                if (error == EILSEQ) {
                    stopped = true; // An undefined character starts at inPtr.
                    break;
                }
                if (error == EINVAL) {
                    // An incomplete character at the window's end. Inside the input, the next
                    // window starts with it; at the input's end, the conversion stops there.
                    stopped = last;
                    break;
                }
                out.resize(start);
                return platformFailure(error, "iconv");
            }
            const std::size_t consumed = static_cast<std::size_t>(inPtr - in.data()) - pos;
            pos += consumed;
            if (!stopped && consumed == 0 && !last) {
                // EINVAL with nothing converted: widen the window so the character fits.
                window = std::min(window * 2, kMaxWindow);
                continue;
            }
            if (!stopped) {
                window = std::min(window * 2, kMaxWindow);
            }
        }
        // Flush: a stateless converter emits nothing, but a buffered character is never lost.
        while (true) {
            out.resize(std::max(out.size(), used + 16));
            char* outPtr = out.data() + used;
            std::size_t outLeft = out.size() - used;
            const std::size_t result = iconv(cd, nullptr, nullptr, &outPtr, &outLeft);
            used = static_cast<std::size_t>(outPtr - out.data());
            if (result != static_cast<std::size_t>(-1)) {
                break;
            }
            const int error = errno;
            if (error == E2BIG) {
                out.resize(out.size() * 2);
                continue;
            }
            out.resize(start);
            return platformFailure(error, "iconv");
        }
        out.resize(used);
        return pos;
    }

    const CodePageInfo* m_info;
    iconv_t m_toUtf8;
    iconv_t m_fromUtf8;
};

class IconvProvider final : public ITextCodecProvider {
public:
    constexpr IconvProvider() noexcept = default;

    [[nodiscard]] Result<std::unique_ptr<ITextCodec>> open(std::uint16_t codePage) noexcept override {
        const CodePageInfo* info = findCodePage(codePage);
        if (info == nullptr) {
            return fail(ErrorCode::Unsupported, std::format("code page {} is not supported", codePage));
        }
        // iconvName is a NUL-terminated literal from the static table (string_view over a literal).
        const std::string name(info->iconvName);
        iconv_t toUtf8 = nullptr;
        iconv_t fromUtf8 = nullptr;
        int error = 0;
        {
            const std::lock_guard lock(gGconvMutex);
            toUtf8 = iconv_open("UTF-8", name.c_str());
            error = errno;
            if (!isInvalid(toUtf8)) {
                fromUtf8 = iconv_open(name.c_str(), "UTF-8");
                error = errno;
                if (isInvalid(fromUtf8)) {
                    iconv_close(toUtf8);
                }
            }
        }
        if (isInvalid(toUtf8) || isInvalid(fromUtf8)) {
            return openFailure(*info, error);
        }
        return std::unique_ptr<ITextCodec>(std::make_unique<IconvCodec>(*info, toUtf8, fromUtf8));
    }

private:
    // iconv_open's EINVAL means the conversion is not available here: Unsupported with the gconv
    // hint. Any other errno (EMFILE, ENFILE, ENOMEM) is a platform failure: Internal, so callers
    // report it instead of skipping the page.
    static std::unexpected<Error> openFailure(const CodePageInfo& info, int error) {
        const Error base = io::errnoError(error, "iconv_open", info.iconvName);
        if (error != EINVAL) {
            return fail(ErrorCode::Internal, std::format("iconv could not open its {} converter ({}): {}",
                                                         info.iconvName, info.name, base.message()));
        }
        return fail(ErrorCode::Unsupported,
                    std::format("iconv has no converter for {} ({}); install the glibc gconv modules "
                                "(glibc-gconv-extra on Fedora): {}",
                                info.iconvName, info.name, base.message()));
    }
};

// Constant-initialised and trivially destructible: no global constructor, no exit-time destructor.
constinit IconvProvider gProvider;

} // namespace

ITextCodecProvider& platformCodecs() noexcept { return gProvider; }

std::uint16_t defaultLegacyCodePage() noexcept {
    // Linux has no system ANSI code page; windows-1252 is the documented default (D14).
    return 1252;
}

} // namespace frost::text
