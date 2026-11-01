// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::diff (ADR 0019, D16): line diffs, refinement and three-way merges of
// texts read from the input, checked with the support library's independent appliers and
// invariants (tests/diff/support/checks.hpp).
/*
 * Input format (at most 16384 bytes, the fuzz trees' -max_len):
 *   byte 0   mode: bits 0-1 select diff, merge, refine or all; bit 2 the split (Lsp or Lf); bit 3
 *            Algorithm::Myers instead of Histogram;
 *   byte 1   limits: a value below 32 sets maxWork = value * 16, anything else keeps the default;
 *   byte 2   content: an even value reads the texts as raw bytes; an odd one maps each byte to one
 *            of 16 short lines (repeats, every break kind, marker-like lines, invalid UTF-8);
 *   the rest the texts, separated by 0xFF bytes: A and B for diffs and refinement, O, A and B for
 *            merges (missing texts are empty).
 * Checks: checkDiff (H1-H4) and the three appliers (TextBuffer only for valid UTF-8); refinement
 * order and apply, and the windowed refinement equal to the full scan; checkMerge;
 * merge(O, A, A) == A and merge(O, O, B) == B under the input's cap; editsForOurs applied to a
 * TextBuffer equals resolvedText; the diff3 marker round trip, before and after custom
 * resolutions with marker-like lines. A failure returns Internal; nothing is kept between inputs.
 */

#include "frost/diff/line_diff.hpp"
#include "frost/diff/merge.hpp"
#include "frost/diff/refine.hpp"
#include "frost/text/text_buffer.hpp"

#include "diff/support/checks.hpp"
#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace frost::fuzz {
namespace {

using namespace frost::diff;
using frost::diff::test::checkApplies;
using frost::diff::test::checkMarkersRoundTrip;
using frost::diff::test::checkRefinement;
using frost::diff::test::checkRefineScansAgree;
using frost::diff::test::hasLoneCr;
using frost::diff::test::validUtf8;

constexpr std::string_view kLines[16] = {"x\n",        "y\n",       "\r\n",       "a\r",       "}\n",       "",
                                         "\xC3\xA9\n", "<<<<<<<\n", "=======\n",  ">>>>>>>\n", "|||||||\n", "\n",
                                         "x",          "\r",        "int a;\r\n", "\xFF\n"};

std::vector<std::string> readTexts(std::span<const std::uint8_t> bytes, bool mapped) {
    std::vector<std::string> texts(1);
    for (const std::uint8_t byte : bytes) {
        if (byte == 0xFF) {
            texts.emplace_back();
        } else if (mapped) {
            texts.back() += kLines[byte % 16];
        } else {
            texts.back() += static_cast<char>(byte);
        }
    }
    texts.resize(std::max<std::size_t>(texts.size(), 3));
    return texts;
}

Result<LineSequence> sequence(std::string text, LineSplit split) {
    return LineSequence::fromBytes(std::move(text), split);
}

Result<void> internal(std::string_view what, const Error& error) {
    return fail(ErrorCode::Internal, std::format("{}: {}", what, error.describe()));
}

Result<void> fuzzDiff(const LineSequence& a, const LineSequence& b, const DiffOptions& options, bool refine) {
    auto diff = diffLines(a, b, options);
    if (!diff) {
        return internal("diffLines", diff.error());
    }
    if (diff->work > options.limits.maxWork) {
        return fail(ErrorCode::Internal, "diffLines worked beyond its cap");
    }
    if (auto checked = checkApplies(a, b, *diff, options); !checked) {
        return checked;
    }
    if (!refine) {
        return {};
    }
    RefineLimits limits;
    limits.maxWork = options.limits.maxWork;
    for (std::size_t k = 0; k < diff->hunks.size() && k < 32; ++k) {
        for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
            auto refined = refineHunk(a, b, diff->hunks[k], granularity, limits);
            if (!refined) {
                return internal("refineHunk", refined.error());
            }
            if (auto checked = checkRefinement(a, b, diff->hunks[k], *refined); !checked) {
                return checked;
            }
            if (auto agreed = checkRefineScansAgree(a, b, diff->hunks[k], granularity, limits); !agreed) {
                return agreed;
            }
        }
    }
    return {};
}

Result<std::string> resolvedText(const MergeModel& model) {
    auto output = model.resolvedText();
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    return std::move(output->text);
}

Result<void> fuzzMerge(const LineSequence& base, const LineSequence& ours, const LineSequence& theirs,
                       const MergeOptions& options, std::uint8_t takeSeed) {
    auto model = MergeModel::create(base, ours, theirs, options);
    if (!model) {
        return internal("MergeModel::create", model.error());
    }
    if (auto checked = checkMerge(base, ours, theirs, model->merge()); !checked) {
        return internal("checkMerge", checked.error());
    }
    // The identities hold under every cap.
    for (const auto& [o, a, b, expected] :
         {std::tuple{&base, &ours, &ours, &ours}, std::tuple{&base, &base, &theirs, &theirs}}) {
        auto identity = MergeModel::create(*o, *a, *b, options);
        if (!identity) {
            return internal("identity merge", identity.error());
        }
        auto text = resolvedText(*identity);
        if (!text || *text != expected->bytes()) {
            return fail(ErrorCode::Internal, "merge(O, A, A) != A or merge(O, O, B) != B");
        }
    }
    // The marker round trip (Lsp only without lone CRs: the documented CR + LF fusion).
    const bool loneCr = hasLoneCr(base.bytes()) || hasLoneCr(ours.bytes()) || hasLoneCr(theirs.bytes());
    if (base.split() == LineSplit::Lf || !loneCr) {
        MarkerOptions labels;
        labels.oursLabel = "ours\nlabel";
        labels.theirsLabel = "theirs\\";
        if (auto checked = checkMarkersRoundTrip(*model, labels); !checked) {
            return checked;
        }
        // Again with every other conflict resolved by a custom text holding marker-like lines.
        auto partly = MergeModel::create(base, ours, theirs, options);
        if (!partly) {
            return internal("MergeModel::create", partly.error());
        }
        static constexpr std::string_view kCustom[] = {"=======\n", "<<<<<<<< x\n|||||||", "kept\n>>>>>>> y\n"};
        std::uint32_t seen = 0;
        for (std::size_t k = 0; k < partly->merge().chunks.size(); ++k) {
            if (partly->merge().chunks[k].kind == ChunkKind::Conflict && seen++ % 2 == 0) {
                const std::string_view custom = kCustom[(takeSeed + k) % std::size(kCustom)];
                if (auto resolved = partly->resolve(k, Take::Custom, std::string(custom)); !resolved) {
                    return internal("resolve", resolved.error());
                }
            }
        }
        if (auto checked = checkMarkersRoundTrip(*partly, labels); !checked) {
            return checked;
        }
    }
    // Resolve every conflict and apply the patch for ours.
    static constexpr Take kTakes[] = {Take::Ours,           Take::Theirs,         Take::Base,
                                      Take::OursThenTheirs, Take::TheirsThenOurs, Take::Custom};
    for (std::size_t k = 0; k < model->merge().chunks.size(); ++k) {
        if (model->merge().chunks[k].kind == ChunkKind::Conflict) {
            const Take take = kTakes[(takeSeed + k) % std::size(kTakes)];
            if (auto resolved = model->resolve(k, take, take == Take::Custom ? std::string("custom") : std::string());
                !resolved) {
                return internal("resolve", resolved.error());
            }
        }
    }
    auto text = resolvedText(*model);
    if (!text) {
        return internal("resolvedText", text.error());
    }
    auto patch = model->editsForOurs();
    if (!patch) {
        return internal("editsForOurs", patch.error());
    }
    if (validUtf8(ours.bytes()) && validUtf8(*text)) {
        auto buffer = text::TextBuffer::fromUtf8(std::string(ours.bytes()));
        if (!buffer) {
            return internal("TextBuffer", buffer.error());
        }
        if (auto applied = buffer->applyEdits(patch->edits()); !applied) {
            return internal("applyEdits", applied.error());
        }
        if (buffer->snapshot().text() != *text) {
            return fail(ErrorCode::Internal, "the OursPatch does not produce resolvedText");
        }
    }
    return {};
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::uint8_t mode = data.size() > 0 ? data[0] : 0;
    const std::uint8_t limit = data.size() > 1 ? data[1] : 0xFF;
    const std::uint8_t content = data.size() > 2 ? data[2] : 0;
    const std::vector<std::string> texts =
        readTexts(data.size() > 3 ? data.subspan(3) : std::span<const std::uint8_t>(), content % 2 == 1);
    const LineSplit split = (mode & 4) != 0 ? LineSplit::Lf : LineSplit::Lsp;
    DiffOptions options;
    if ((mode & 8) != 0) {
        options.algorithm = Algorithm::Myers;
    }
    if (limit < 32) {
        options.limits.maxWork = std::uint64_t{limit} * 16;
    }
    auto o = sequence(texts[0], split);
    auto a = sequence(texts[1], split);
    auto b = sequence(texts[2], split);
    if (!o || !a || !b) {
        return fail(ErrorCode::Internal, "a fuzz text exceeds the sequence limits");
    }
    const unsigned selector = mode & 3;
    if (selector == 0 || selector == 2 || selector == 3) {
        // Diffs and refinement read A and B from the first two texts.
        if (auto checked = fuzzDiff(*o, *a, options, selector != 0); !checked) {
            return checked;
        }
    }
    if (selector == 1 || selector == 3) {
        MergeOptions merge;
        merge.diff = options;
        if (auto checked = fuzzMerge(*o, *a, *b, merge, content); !checked) {
            return checked;
        }
    }
    return {};
}

} // namespace frost::fuzz
