// SPDX-License-Identifier: Apache-2.0
//
// Tests of frost-icon-baker through its command line (realProcessLauncher): baking the synthetic
// fixture manifest (tests/ui/fixtures/icons, FrostIDE-authored SVGs), known-answer pixels for
// fills, colours and the strokePx normalisation, determinism and restat, the depfile, the source
// hash, and every rejection path (ADR 0015 decision 7).

#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/model/icon_atlas_format.hpp"

#include "model/model_test_support.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui {
namespace {

const std::filesystem::path& fixturesDir() {
    static const std::filesystem::path path =
        std::filesystem::path(FROST_SOURCE_DIR) / "tests" / "ui" / "fixtures" / "icons";
    return path;
}

io::ProcessResult runBaker(std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = FROST_ICON_BAKER;
    spec.arguments = std::move(arguments);
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(120));
    EXPECT_TRUE(result) << result.error().describe();
    return result.value_or(io::ProcessResult{});
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path.string();
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << path.string();
}

// A scratch copy of the good and bad fixture SVGs in one flat directory, plus an output directory.
struct Workspace {
    frost::test::ScratchDir scratch{"baker"};
    std::filesystem::path src = std::filesystem::path(scratch.join("src"));
    std::filesystem::path out = std::filesystem::path(scratch.join("out"));

    Workspace() {
        std::filesystem::create_directories(src);
        std::filesystem::create_directories(out);
        for (const char* group : {"good", "bad"}) {
            for (const auto& entry : std::filesystem::directory_iterator(fixturesDir() / group)) {
                std::filesystem::copy_file(entry.path(), src / entry.path().filename());
            }
        }
    }

    io::ProcessResult bake(const std::filesystem::path& outDir, std::vector<std::string> extra = {}) const {
        std::vector<std::string> args{
            "--manifest", (src / "icons.json").string(), "--source-dir", src.string(), "--out-dir", outDir.string()};
        args.insert(args.end(), extra.begin(), extra.end());
        return runBaker(std::move(args));
    }

    std::size_t atlasCount() const {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(out)) {
            count += entry.path().extension() == ".fia" ? 1U : 0U;
        }
        return count;
    }
};

// Sum of a cell column's alpha coverage, in pixels (a horizontal line's stroke width).
double columnCoverage(const IconAtlasView& atlas, const AtlasEntryView& entry, std::uint16_t column) {
    const AtlasPageView page = atlas.page(entry.page);
    double sum = 0;
    for (std::uint16_t y = 0; y < entry.h; ++y) {
        const std::size_t at = std::size_t{static_cast<std::uint16_t>(entry.y + y)} * page.stride + entry.x + column;
        sum += static_cast<double>(std::to_integer<unsigned>(page.pixels[at])) / 255.0;
    }
    return sum;
}

// The premultiplied ARGB pixel (x, y) of a cell on an ARGB page, as 0xAARRGGBB.
std::uint32_t argbAt(const IconAtlasView& atlas, std::uint16_t pageIndex, std::uint16_t x, std::uint16_t y) {
    const AtlasPageView page = atlas.page(pageIndex);
    const std::size_t at = std::size_t{y} * page.stride + std::size_t{x} * 4;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(page.pixels[at + i]) << (8 * i);
    }
    return value;
}

TEST(IconBakerCli, BakesFixtureManifest) {
    Workspace ws;
    const auto depfile = ws.out / "icons.d";
    const io::ProcessResult result = ws.bake(ws.out, {"--scales", "100,150,300", "--depfile", depfile.string()});
    ASSERT_EQ(result.exitCode, 0) << result.stderrBytes;
    EXPECT_EQ(ws.atlasCount(), 3U) << "only the requested scales";

    std::uint64_t sourceHash = 0;
    for (const std::uint16_t scale : {std::uint16_t{100}, std::uint16_t{150}, std::uint16_t{300}}) {
        SCOPED_TRACE(scale);
        const std::string bytes = readBytes(ws.out / ("icons-" + std::to_string(scale) + ".fia"));
        auto parsed = IconAtlasView::parse(test::asBytes(bytes));
        ASSERT_TRUE(parsed) << parsed.error().describe();
        const IconAtlasView& atlas = *parsed;
        EXPECT_EQ(atlas.scalePercent(), scale);
        EXPECT_EQ(atlas.entryCount(), 7U);
        if (sourceHash == 0) {
            sourceHash = atlas.sourceHash();
        }
        EXPECT_EQ(atlas.sourceHash(), sourceHash) << "one source hash for every scale";
        EXPECT_EQ(atlas.designSizesOf("test.line"), (std::vector<std::uint8_t>{14, 16, 20}));

        // A full-cell fill covers every pixel completely.
        const AtlasEntryView full = atlas.entry(*atlas.find("test.full", 16));
        EXPECT_TRUE(full.tinted);
        EXPECT_EQ(full.role, IconRole::Default);
        EXPECT_EQ(full.w, iconDevicePx(16, scale));
        for (std::uint16_t x = 0; x < full.w; ++x) {
            EXPECT_NEAR(columnCoverage(atlas, full, x), full.h, 1e-9) << "column " << x;
        }

        // strokePx: the stroke is strokePx x devicePx / designSize device pixels wide.
        for (const auto& [size, strokePx] : {std::pair<std::uint8_t, double>{14, 1.0}, {16, 1.0}, {20, 2.0}}) {
            const AtlasEntryView line = atlas.entry(*atlas.find("test.line", size));
            EXPECT_EQ(line.role, IconRole::Accent);
            const double expected = strokePx * line.w / size;
            EXPECT_NEAR(columnCoverage(atlas, line, static_cast<std::uint16_t>(line.w / 2)), expected, 0.05)
                << "test.line@" << int{size};
        }
        EXPECT_EQ(atlas.entry(*atlas.find("test.ring", 20)).role, IconRole::Error);

        // Untinted: premultiplied ARGB in its own colours, with the dark variant beside it.
        const AtlasEntryView swatch = atlas.entry(*atlas.find("test.swatch", 16));
        EXPECT_FALSE(swatch.tinted);
        ASSERT_TRUE(swatch.hasDark);
        const auto inset = static_cast<std::uint16_t>(swatch.w / 4);
        EXPECT_EQ(argbAt(atlas, swatch.page, swatch.x + inset, swatch.y + inset), 0xffff0000U);
        EXPECT_EQ(argbAt(atlas, swatch.page, swatch.x + swatch.w - 1 - inset, swatch.y + inset), 0xff0000ffU);
        EXPECT_EQ(argbAt(atlas, swatch.darkPage, swatch.darkX + inset, swatch.darkY + inset), 0xff00ff00U);
    }

    // The depfile names the first output and lists the manifest and every SVG read.
    const std::string deps = readBytes(depfile);
    EXPECT_NE(deps.find("icons-100.fia: "), std::string::npos) << deps;
    for (const char* name : {"icons.json", "full.svg", "line.svg", "ring.svg", "swatch.svg", "swatch_dark.svg"}) {
        EXPECT_NE(deps.find(name), std::string::npos) << name << "\n" << deps;
    }
    EXPECT_EQ(deps.find("text.svg"), std::string::npos) << "only files that were read";
    EXPECT_EQ(deps.find('\\'), std::string::npos) << "forward slashes only";
}

TEST(IconBakerCli, DeterministicAndRestat) {
    Workspace ws;
    const auto second = ws.out / "second";
    std::filesystem::create_directories(second);
    ASSERT_EQ(ws.bake(ws.out).exitCode, 0);
    ASSERT_EQ(ws.bake(second).exitCode, 0);
    for (const std::uint16_t scale : kAtlasScales) {
        const std::string name = "icons-" + std::to_string(scale) + ".fia";
        EXPECT_EQ(readBytes(ws.out / name), readBytes(second / name)) << name;
    }
    const std::string first = (ws.out / "icons-100.fia").string();
    const auto before = io::realFileSystem().status(first);
    ASSERT_TRUE(before);
    ASSERT_EQ(ws.bake(ws.out).exitCode, 0);
    const auto after = io::realFileSystem().status(first);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->mtime, after->mtime) << "an unchanged atlas is not rewritten";
}

TEST(IconBakerCli, SourceHashTracksEveryInput) {
    Workspace ws;
    const auto hashOf = [&ws]() -> std::uint64_t {
        const io::ProcessResult result = ws.bake(ws.out, {"--scales", "100"});
        EXPECT_EQ(result.exitCode, 0) << result.stderrBytes;
        const std::string bytes = readBytes(ws.out / "icons-100.fia");
        auto atlas = IconAtlasView::parse(test::asBytes(bytes));
        return atlas ? atlas->sourceHash() : 0;
    };
    std::vector<std::uint64_t> hashes{hashOf()};
    // A comment changes no pixel but is a different input.
    writeBytes(ws.src / "ring.svg", readBytes(ws.src / "ring.svg") + "<!-- edited -->\n");
    hashes.push_back(hashOf());
    writeBytes(ws.src / "swatch_dark.svg", readBytes(ws.src / "swatch_dark.svg") + "\n");
    hashes.push_back(hashOf());
    writeBytes(ws.src / "icons.json", readBytes(ws.src / "icons.json") + "\n");
    hashes.push_back(hashOf());
    for (std::size_t i = 1; i < hashes.size(); ++i) {
        EXPECT_NE(hashes[i], hashes[i - 1]) << "edit " << i;
    }
}

TEST(IconBakerCli, RejectsInvalidInputs) {
    Workspace ws;
    std::string oversized = readBytes(ws.src / "full.svg");
    oversized.insert(oversized.find("<rect"), "<!--" + std::string(64 * 1024, 'x') + "-->");
    writeBytes(ws.src / "oversized.svg", oversized);
    const auto icon = [](std::string_view fields) {
        return R"({"schemaVersion": 1, "icons": [{"id": "test.icon", "sizes": [16], )" + std::string(fields) + "}]}";
    };
    const std::pair<std::string, std::string_view> cases[] = {
        {icon(R"("source": "text.svg", "tint": "Icon.Default")"), "<text> is not allowed"},
        {icon(R"("source": "no-viewbox.svg", "tint": "Icon.Default")"), "square viewBox"},
        {icon(R"("source": "not-square.svg", "tint": "Icon.Default")"), "square viewBox"},
        {icon(R"("source": "child-stroke.svg", "tint": "Icon.Default", "strokePx": {"16": 1})"), "own stroke-width"},
        {icon(R"("source": "root-no-stroke.svg", "tint": "Icon.Default", "strokePx": {"16": 1})"), "no stroke-width"},
        {icon(R"("source": "unresolved-paint.svg", "tint": "Icon.Default")"), "Qt Svg reported"},
        {icon(R"("source": "dtd.svg", "tint": "Icon.Default")"), "DTDs are not allowed"},
        {icon(R"("source": "malformed.svg", "tint": "Icon.Default")"), "malformed SVG"},
        {icon(R"("source": "not-svg.svg", "tint": "Icon.Default")"), "root element must be <svg>"},
        {icon(R"("source": "oversized.svg", "tint": "Icon.Default")"), "64 KiB"},
        {icon(R"("source": "absent.svg", "tint": "Icon.Default")"), "cannot read"},
        {icon(R"("source": "../src/full.svg", "tint": "Icon.Default")"), "without '..'"},
        {icon(R"("source": "full.png", "tint": "Icon.Default")"), "relative .svg path"},
        {icon(R"("source": "full.svg", "tint": "Icon.Grey")"), "must be an icon role"},
        {icon(R"("source": "full.svg", "tint": 3)"), "icon role name or null"},
        {icon(R"("source": "full.svg", "tint": "Icon.Default", "dark": "swatch_dark.svg")"), "only untinted"},
        {icon(R"("source": "full.svg", "tint": null, "dark": "/abs.svg")"), "dark must be a relative"},
        {icon(R"("source": "full.svg", "tint": "Icon.Default", "strokePx": {"20": 1})"), "strokePx maps"},
        {icon(R"("source": "full.svg", "tint": "Icon.Default", "strokePx": {"16": 9})"), "strokePx maps"},
        {icon(R"("source": "full.svg", "tint": "Icon.Default", "colour": 1)"), "unknown field colour"},
        {R"({"schemaVersion": 1, "icons": [{"id": "Test.Icon", "source": "full.svg", "sizes": [16], "tint": null}]})",
         "invalid or duplicate icon id"},
        {R"({"schemaVersion": 1, "icons": [{"id": "test.a", "source": "full.svg", "sizes": [16], "tint": null},)"
         R"( {"id": "test.a", "source": "full.svg", "sizes": [20], "tint": null}]})",
         "invalid or duplicate icon id"},
        {R"({"schemaVersion": 1, "icons": [{"id": "test.a", "source": "full.svg", "sizes": [18], "tint": null}]})",
         "sizes must be distinct"},
        {R"({"schemaVersion": 1, "icons": [{"id": "test.a", "source": "full.svg", "sizes": [16, 16], "tint": null}]})",
         "sizes must be distinct"},
        {R"({"schemaVersion": 1, "icons": [{"id": "test.a", "source": "full.svg", "sizes": [], "tint": null}]})",
         "sizes is empty"},
        {R"({"schemaVersion": 2, "icons": []})", "schemaVersion must be 1"},
        {R"({"schemaVersion": 1, "icons": {}})", "must be an array"},
        {R"({"schemaVersion": 1, "icons": [], "extra": 1})", "unknown manifest field"},
        {R"([1, 2])", "not a JSON object"},
    };
    for (const auto& [manifest, expected] : cases) {
        SCOPED_TRACE(manifest);
        writeBytes(ws.src / "icons.json", manifest);
        const io::ProcessResult result = ws.bake(ws.out);
        EXPECT_EQ(result.exitCode, 1);
        EXPECT_NE(result.stderrBytes.find(expected), std::string::npos) << result.stderrBytes;
        EXPECT_EQ(ws.atlasCount(), 0U) << "nothing is written on failure";
    }
    // A missing output directory is reported after baking, still before writing anything.
    std::filesystem::copy_file(fixturesDir() / "good" / "icons.json", ws.src / "icons.json",
                               std::filesystem::copy_options::overwrite_existing);
    EXPECT_EQ(ws.bake(ws.out / "missing").exitCode, 1);
}

TEST(IconBakerCli, UsageErrors) {
    Workspace ws;
    const std::string manifest = (fixturesDir() / "good" / "icons.json").string();
    const std::string src = (fixturesDir() / "good").string();
    const std::string out = ws.out.string();
    EXPECT_EQ(runBaker({}).exitCode, 2);
    EXPECT_EQ(runBaker({"--manifest", manifest, "--source-dir", src}).exitCode, 2) << "no --out-dir";
    EXPECT_EQ(runBaker({"--manifest", manifest, "--source-dir", src, "--out-dir"}).exitCode, 2);
    EXPECT_EQ(runBaker({"--manifest", manifest, "--source-dir", src, "--out-dir", out, "--frobnicate"}).exitCode, 2);
    for (const char* scales : {"100,101", "100,100", "", "abc", "0"}) {
        EXPECT_EQ(
            runBaker({"--manifest", manifest, "--source-dir", src, "--out-dir", out, "--scales", scales}).exitCode, 2)
            << scales;
    }
    EXPECT_EQ(
        runBaker({"--manifest", (ws.src / "absent.json").string(), "--source-dir", src, "--out-dir", out}).exitCode, 1);
    EXPECT_EQ(ws.atlasCount(), 0U);
}

} // namespace
} // namespace frost::ui
