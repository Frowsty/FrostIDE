// SPDX-License-Identifier: Apache-2.0
//
// Tests for SolutionModel (frost/project/solution_model.hpp): the Solution Explorer tree of the
// SDK-shaped fixture from .sln and .slnx, unsupported and unavailable projects, lookups,
// dependencies, configuration mapping, stable keys and nesting problems.
#include "frost/io/file_system.hpp"
#include "frost/project/solution_model.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;
using test::sdk;

// "Name" for a leaf, "Name[child,child]" otherwise, with a kind marker for projects.
std::string dump(const SolutionModel& model, NodeId id) {
    const Node& node = model.node(id);
    std::string out = node.displayName;
    if (node.kind == NodeKind::UnsupportedProject) {
        out += "(unsupported)";
    } else if (node.kind == NodeKind::UnavailableProject) {
        out += "(unavailable)";
    }
    if (!node.children.empty()) {
        out += '[';
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            out += (i ? "," : "") + dump(model, node.children[i]);
        }
        out += ']';
    }
    return out;
}

// The child of `parent` with this display name.
std::optional<NodeId> child(const SolutionModel& model, NodeId parent, std::string_view name) {
    for (NodeId id : model.children(parent)) {
        if (model.node(id).displayName == name) {
            return id;
        }
    }
    return std::nullopt;
}

ProjectId projectNamed(const SolutionModel& model, std::string_view name) {
    for (std::size_t i = 0; i < model.projects().size(); ++i) {
        if (model.projects()[i].name == name) {
            return ProjectId{static_cast<std::uint32_t>(i)};
        }
    }
    ADD_FAILURE() << "no project " << name;
    return ProjectId{};
}

constexpr std::string_view kExpectedTree =
    "SdkApp["
    "Libraries["
    "Core[Empty Filter,Generated[Config[config.h]],Header Files[Shared[common.h],core.h],"
    "Source Files[Detail[strings.cpp],core.cpp],core.natvis,gen.py,version.h],"
    "Engine[engine.cpp,engine.h]],"
    "Tools[Gone(unavailable),Packager,README.md],"
    "App[Header Files[app.h],Resource Files[app.ico,app.rc],Source Files[app.cpp,main.cpp],README.md]]";

TEST(SolutionModel, EvaluatedFilesOverlayKeepsFiltersAndOtherItems) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model);
    const ProjectId core = projectNamed(*model, "Core");
    const std::string oldFile = sdk("Libraries/Core/src/core.cpp");
    const std::string header = sdk("Libraries/Core/include/core/core.h");
    const std::string generated = sdk("Libraries/Core/generated.cpp");
    ASSERT_TRUE(model->findFileNode(oldFile));
    ASSERT_TRUE(model->findFileNode(header));
    const auto oldNode = model->findFileNode(oldFile);
    ASSERT_TRUE(model->parent(*oldNode));
    const auto filterKey = model->node(*model->parent(*oldNode)).key;
    const std::vector<std::string> files{generated};
    const auto overlay = model->withEvaluatedFiles(core, ItemType::ClCompile, files);
    EXPECT_TRUE(model->findFileNode(oldFile));
    EXPECT_FALSE(overlay.findFileNode(oldFile));
    EXPECT_TRUE(overlay.findFileNode(header));
    ASSERT_TRUE(overlay.findFileNode(generated));
    EXPECT_EQ(overlay.owningProject(generated), core);
    EXPECT_TRUE(overlay.findNode(filterKey));
}

TEST(SolutionModel, EvaluatedHeadersReturnAfterAnAuthoritativeEmptyResultInTheirOriginalFilter) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model);
    const ProjectId core = projectNamed(*model, "Core");
    const std::string header = sdk("Libraries/Core/include/core/core.h");
    const auto original = model->findFileNode(header);
    ASSERT_TRUE(original);
    ASSERT_TRUE(model->parent(*original));
    const std::string originalParent = model->node(*model->parent(*original)).key;
    const std::vector<std::string> none;
    const auto absent = model->withEvaluatedFiles(core, ItemType::ClInclude, none);
    EXPECT_FALSE(absent.findFileNode(header));
    const std::vector<std::string> returned{header};
    const auto restored = absent.withEvaluatedFiles(core, ItemType::ClInclude, returned);
    const auto found = restored.findFileNode(header);
    ASSERT_TRUE(found) << "a detached arena node must not suppress a newly authoritative header";
    ASSERT_TRUE(restored.parent(*found));
    EXPECT_EQ(restored.node(*restored.parent(*found)).key, originalParent);
    EXPECT_EQ(restored.node(*found).itemType, ItemType::ClInclude);
    const auto reachable = restored.projectFilePaths(core);
    EXPECT_NE(std::ranges::find(reachable, header), reachable.end());
    EXPECT_TRUE(model->findFileNode(header)); // The immutable original still contains its header.
    EXPECT_FALSE(absent.findFileNode(header));
    auto repeated = restored;
    for (int cycle = 0; cycle != 3; ++cycle) {
        repeated = repeated.withEvaluatedFiles(core, ItemType::ClInclude, returned);
        EXPECT_EQ(repeated.nodeCount(), restored.nodeCount());
        const auto id = repeated.findFileNode(header);
        ASSERT_TRUE(id);
        ASSERT_TRUE(repeated.parent(*id));
        EXPECT_EQ(std::ranges::count(repeated.children(*repeated.parent(*id)), *id), 1);
        EXPECT_EQ(repeated.projectsContaining(header), std::vector<ProjectId>{core});
        repeated = repeated.withEvaluatedFiles(core, ItemType::ClInclude, none);
        EXPECT_FALSE(repeated.findFileNode(header));
        repeated = repeated.withEvaluatedFiles(core, ItemType::ClInclude, returned);
        EXPECT_EQ(repeated.nodeCount(), restored.nodeCount());
    }
}

TEST(SolutionModel, NewlyEvaluatedHeadersUseDeclaredFiltersOrPhysicalHierarchyWithoutCompileMembership) {
    MemTree tree;
    tree.write("Headers.slnx", R"(<Solution><Project Path="App/App.vcxproj" /></Solution>)");
    tree.write("App/App.vcxproj", R"(<Project><Import Project="headers.props" />
      <ItemGroup><ClCompile Include="main.cpp" /></ItemGroup></Project>)");
    tree.write("App/headers.props", R"(<Project><ItemGroup>
      <ClInclude Include="include/deep/imported.hpp" /></ItemGroup></Project>)");
    tree.write("App/App.vcxproj.filters", R"(<Project><ItemGroup>
      <Filter Include="Header Files\API" />
      <ClInclude Include="include/deep/imported.hpp"><Filter>Header Files\API</Filter></ClInclude>
      </ItemGroup></Project>)");
    tree.write("App/main.cpp", "int main() {}\n");
    tree.write("App/include/deep/imported.hpp", "#pragma once\n");
    tree.write("App/include/loose/unassigned.hpp", "#pragma once\n");
    const std::string header = MemTree::path("App/include/deep/imported.hpp");
    const std::string unassigned = MemTree::path("App/include/loose/unassigned.hpp");
    for (const auto mode :
         {CppHierarchyMode::Filters, CppHierarchyMode::FiltersAndFolders, CppHierarchyMode::PhysicalFolders}) {
        SCOPED_TRACE(static_cast<int>(mode));
        auto model = SolutionModel::load(tree.fs, MemTree::path("Headers.slnx"), {.cppHierarchy = mode});
        ASSERT_TRUE(model);
        EXPECT_FALSE(model->findFileNode(header)); // Imports require real evaluation, never a guessed scan.
        const auto project = projectNamed(*model, "App");
        const std::vector<std::string> evaluated{header, unassigned};
        const auto overlay = model->withEvaluatedFiles(project, ItemType::ClInclude, evaluated);
        const auto found = overlay.findFileNode(header);
        ASSERT_TRUE(found);
        ASSERT_TRUE(overlay.parent(*found));
        EXPECT_EQ(overlay.node(*overlay.parent(*found)).displayName,
                  mode == CppHierarchyMode::PhysicalFolders ? "deep" : "API");
        EXPECT_EQ(overlay.node(*found).itemType, ItemType::ClInclude);
        EXPECT_TRUE(overlay.findFileNode(MemTree::path("App/main.cpp")));
        const auto loose = overlay.findFileNode(unassigned);
        ASSERT_TRUE(loose);
        ASSERT_TRUE(overlay.parent(*loose));
        EXPECT_EQ(overlay.node(*overlay.parent(*loose)).displayName,
                  mode == CppHierarchyMode::Filters ? "App" : "loose");
        const auto repeated = overlay.withEvaluatedFiles(project, ItemType::ClInclude, evaluated);
        EXPECT_EQ(repeated.nodeCount(), overlay.nodeCount());
        EXPECT_EQ(std::ranges::count(repeated.children(*repeated.parent(*found)), *found), 1);
        EXPECT_EQ(std::ranges::count(repeated.children(*repeated.parent(*loose)), *loose), 1);
    }
}

TEST(SolutionModel, HeaderOverlaysPreserveOtherItemKindsAndIndependentSharedHeaderMembership) {
    MemTree tree;
    tree.write("Shared.slnx", R"(<Solution><Project Path="One/One.vcxproj" />
      <Project Path="Two/Two.vcxproj" /></Solution>)");
    tree.write("One/One.vcxproj", R"(<Project><ItemGroup>
      <ClInclude Include="../shared/linked.hpp" /><None Include="notes.h" />
      <CustomBuild Include="generated.h" /></ItemGroup></Project>)");
    tree.write("Two/Two.vcxproj", R"(<Project><ItemGroup>
      <ClInclude Include="../shared/linked.hpp" /></ItemGroup></Project>)");
    tree.write("shared/linked.hpp", "#pragma once\n");
    tree.write("One/notes.h", "#pragma once\n");
    tree.write("One/generated.h", "#pragma once\n");
    auto model = SolutionModel::load(tree.fs, MemTree::path("Shared.slnx"));
    ASSERT_TRUE(model);
    const auto one = projectNamed(*model, "One");
    const auto two = projectNamed(*model, "Two");
    const std::string linked = MemTree::path("shared/linked.hpp");
    std::optional<NodeId> twoNode;
    for (NodeId id : model->children(model->project(two).node)) {
        if (model->node(id).path == linked)
            twoNode = id;
    }
    ASSERT_TRUE(twoNode);
    const auto twoParent = model->parent(*twoNode);
    const std::string twoKey = model->node(*twoNode).key;
    const std::vector<std::string> none;
    const auto removed = model->withEvaluatedFiles(one, ItemType::ClInclude, none);
    EXPECT_EQ(removed.owningProject(linked), two);
    const std::vector<std::string> returned{linked};
    const auto restored = removed.withEvaluatedFiles(one, ItemType::ClInclude, returned);
    const auto oneFiles = restored.projectFilePaths(one);
    const auto twoFiles = restored.projectFilePaths(two);
    EXPECT_NE(std::ranges::find(oneFiles, linked), oneFiles.end());
    EXPECT_NE(std::ranges::find(twoFiles, linked), twoFiles.end());
    EXPECT_EQ(restored.owningProject(linked), one);
    EXPECT_EQ(restored.projectsContaining(linked), (std::vector<ProjectId>{one, two}));
    EXPECT_EQ(restored.parent(*twoNode), twoParent);
    EXPECT_EQ(restored.node(*twoNode).key, twoKey);
    EXPECT_EQ(restored.findNode(twoKey), twoNode);
    EXPECT_EQ(std::ranges::count(restored.children(*twoParent), *twoNode), 1);
    for (const auto& [file, type] :
         {std::pair{"One/notes.h", ItemType::None}, std::pair{"One/generated.h", ItemType::CustomBuild}}) {
        const auto found = restored.findFileNode(MemTree::path(file));
        ASSERT_TRUE(found);
        EXPECT_EQ(restored.node(*found).itemType, type);
    }
}

TEST(SolutionModel, ReturningHeaderDoesNotOverwriteAnotherActiveItemKindAtTheSamePath) {
    MemTree tree;
    tree.write("Kinds.slnx", R"(<Solution><Project Path="App.vcxproj" /></Solution>)");
    tree.write("App.vcxproj", R"(<Project><ItemGroup><ClInclude Include="api.h" /></ItemGroup></Project>)");
    tree.write("api.h", "#pragma once\n");
    auto initial = SolutionModel::load(tree.fs, MemTree::path("Kinds.slnx"));
    ASSERT_TRUE(initial);
    const auto project = projectNamed(*initial, "App");
    const std::vector<std::string> none;
    const std::vector<std::string> file{MemTree::path("api.h")};
    auto absent = initial->withEvaluatedFiles(project, ItemType::ClInclude, none);
    auto other = absent.withEvaluatedFiles(project, ItemType::CustomBuild, file);
    const auto otherNode = other.findFileNode(file.front());
    ASSERT_TRUE(otherNode);
    const std::string otherKey = other.node(*otherNode).key;
    auto stillAbsent = other.withEvaluatedFiles(project, ItemType::ClInclude, none);
    EXPECT_EQ(stillAbsent.findNode(otherKey), otherNode);
    auto returned = stillAbsent.withEvaluatedFiles(project, ItemType::ClInclude, file);
    EXPECT_EQ(returned.findNode(otherKey), otherNode);
    EXPECT_EQ(returned.node(*otherNode).itemType, ItemType::CustomBuild);
    int headers = 0;
    int custom = 0;
    for (const auto id : returned.children(returned.project(project).node)) {
        if (returned.node(id).path == file.front()) {
            headers += returned.node(id).itemType == ItemType::ClInclude ? 1 : 0;
            custom += returned.node(id).itemType == ItemType::CustomBuild ? 1 : 0;
        }
    }
    EXPECT_EQ(headers, 1);
    EXPECT_EQ(custom, 1);
}

TEST(SolutionModel, BuildsTheSdkTreeFromSln) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    EXPECT_EQ(model->format(), SolutionFormat::Sln);
    EXPECT_EQ(model->name(), "SdkApp");
    EXPECT_EQ(model->directory(), sdk());
    EXPECT_EQ(dump(*model, SolutionModel::root()), kExpectedTree);
    EXPECT_EQ(model->node(SolutionModel::root()).kind, NodeKind::Solution);
    EXPECT_FALSE(model->node(SolutionModel::root()).parent.has_value());

    // Kinds, projects and paths on the nodes a tree view binds to.
    const NodeId libraries = *child(*model, SolutionModel::root(), "Libraries");
    EXPECT_EQ(model->node(libraries).kind, NodeKind::SolutionFolder);
    const NodeId core = *child(*model, libraries, "Core");
    EXPECT_EQ(model->node(core).kind, NodeKind::Project);
    EXPECT_EQ(model->node(core).path, sdk("Libraries/Core/Core.vcxproj"));
    EXPECT_EQ(*model->parent(core), libraries);
    const NodeId sources = *child(*model, core, "Source Files");
    EXPECT_EQ(model->node(sources).kind, NodeKind::Filter);
    EXPECT_EQ(model->node(sources).project, projectNamed(*model, "Core"));
    const NodeId cpp = *child(*model, sources, "core.cpp");
    EXPECT_EQ(model->node(cpp).kind, NodeKind::File);
    EXPECT_EQ(model->node(cpp).itemType, ItemType::ClCompile);
    EXPECT_EQ(model->node(cpp).path, sdk("Libraries/Core/src/core.cpp"));
    const NodeId readme = *child(*model, *child(*model, SolutionModel::root(), "Tools"), "README.md");
    EXPECT_FALSE(model->node(readme).project.has_value()); // A solution item.
}

TEST(SolutionModel, ReportsUnsupportedAndUnavailableProjects) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const ProjectInfo& packager = model->project(projectNamed(*model, "Packager"));
    EXPECT_EQ(packager.kind, ProjectKind::CSharp);
    EXPECT_EQ(packager.nodeKind, NodeKind::Project);
    EXPECT_FALSE(packager.vcxproj.has_value());
    EXPECT_EQ(toString(packager.kind), "C#");

    const ProjectInfo& gone = model->project(projectNamed(*model, "Gone"));
    EXPECT_EQ(gone.kind, ProjectKind::Cpp);
    EXPECT_EQ(gone.nodeKind, NodeKind::UnavailableProject);
    ASSERT_TRUE(gone.error.has_value());
    EXPECT_EQ(gone.error->code(), ErrorCode::NotFound);
    EXPECT_EQ(model->node(gone.node).kind, NodeKind::UnavailableProject);
    EXPECT_TRUE(model->children(gone.node).empty());

    const ProjectInfo& core = model->project(projectNamed(*model, "Core"));
    ASSERT_TRUE(core.vcxproj.has_value());
    EXPECT_EQ(core.vcxproj->configurationType, "StaticLibrary");
    // The unknown $(SdkDocs) reaches the log.
    bool reported = false;
    for (const std::string& warning : model->warnings()) {
        reported = reported || warning.find("$(SdkDocs)") != std::string::npos;
    }
    EXPECT_TRUE(reported);
}

TEST(SolutionModel, ResolvesDependenciesAndLookups) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const ProjectId app = projectNamed(*model, "App");
    const ProjectId core = projectNamed(*model, "Core");
    const ProjectId engine = projectNamed(*model, "Engine");
    const ProjectId packager = projectNamed(*model, "Packager");
    EXPECT_EQ(model->project(app).dependencies, (std::vector<ProjectId>{packager, engine, core}));
    EXPECT_EQ(model->project(engine).dependencies, std::vector<ProjectId>{core});
    EXPECT_TRUE(model->project(core).dependencies.empty());
    EXPECT_EQ(model->project(app).externalReferences,
              std::vector<std::string>{test::fixture("Elsewhere/Outside.vcxproj")});

    EXPECT_EQ(model->owningProject(sdk("Libraries/Core/src/detail/strings.cpp")), core);
    EXPECT_EQ(model->owningProject(sdk("shared/common.h")), core);
    EXPECT_EQ(model->owningProject(sdk("Libraries/Core/src/legacy/old.cpp")), std::nullopt);
    EXPECT_EQ(model->owningProject("relative.cpp"), std::nullopt);
    // README.md is a solution item and an App item: only the project counts.
    EXPECT_EQ(model->projectsContaining(sdk("README.md")), std::vector<ProjectId>{app});
    ASSERT_TRUE(model->findFileNode(sdk("README.md")).has_value());

    EXPECT_EQ(model->findProjectByPath(sdk("Libraries/Engine/Engine.vcxproj")), engine);
    EXPECT_EQ(model->findProjectByGuid("e0e0e0e0-0003-4000-8000-000000000003"), engine);
    EXPECT_EQ(model->findProjectByGuid("{00000000-0000-0000-0000-000000000000}"), std::nullopt);
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        // NTFS compares names case-insensitively, and so do the lookups.
        std::string upper = sdk("App/SRC/MAIN.CPP");
        EXPECT_EQ(model->owningProject(upper), app);
    }
}

TEST(SolutionModel, MapsConfigurations) {
    auto model = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    ASSERT_EQ(model->configurations().size(), 4u);
    const auto release86 = model->findConfiguration("release|X86");
    ASSERT_TRUE(release86.has_value());
    const auto& engine = model->projectConfiguration(projectNamed(*model, "Engine"), *release86);
    ASSERT_TRUE(engine.has_value());
    EXPECT_EQ(engine->platform, "Win32");
    EXPECT_FALSE(engine->build);
    const auto& app = model->projectConfiguration(projectNamed(*model, "App"), *release86);
    ASSERT_TRUE(app.has_value());
    EXPECT_TRUE(app->build);
    EXPECT_EQ(model->findConfiguration("Profile|x64"), std::nullopt);
}

TEST(SolutionModel, SlnxGivesTheSameTreeAndMapping) {
    auto sln = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    auto slnx = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.slnx"));
    ASSERT_TRUE(sln.has_value()) << sln.error().describe();
    ASSERT_TRUE(slnx.has_value()) << slnx.error().describe();
    EXPECT_EQ(slnx->format(), SolutionFormat::Slnx);
    EXPECT_EQ(dump(*slnx, SolutionModel::root()), kExpectedTree);
    ASSERT_EQ(slnx->configurations().size(), sln->configurations().size());

    // The C++ projects build the same project configurations from both files.
    for (std::string_view name : {"App", "Core", "Engine"}) {
        for (std::size_t c = 0; c < sln->configurations().size(); ++c) {
            EXPECT_EQ(sln->configurations()[c], slnx->configurations()[c]);
            const auto& a = sln->projectConfiguration(projectNamed(*sln, name), c);
            const auto& b = slnx->projectConfiguration(projectNamed(*slnx, name), c);
            ASSERT_TRUE(a.has_value() && b.has_value()) << name;
            EXPECT_EQ(*a, *b) << name << " " << sln->configurations()[c].name();
        }
    }
    const ProjectId app = projectNamed(*slnx, "App");
    EXPECT_EQ(slnx->project(app).dependencies,
              (std::vector<ProjectId>{projectNamed(*slnx, "Packager"), projectNamed(*slnx, "Engine"),
                                      projectNamed(*slnx, "Core")}));
    // Packager's <Build Solution="Debug|*" Project="false"/>.
    EXPECT_FALSE(slnx->projectConfiguration(projectNamed(*slnx, "Packager"), 0)->build);
    EXPECT_TRUE(slnx->projectConfiguration(projectNamed(*slnx, "Packager"), 2)->build);
    // App's GUID comes from its .vcxproj when the .slnx has no Id.
    EXPECT_EQ(slnx->project(app).projectGuid, "{A1A1A1A1-0001-4000-8000-000000000001}");
}

TEST(SolutionModel, KeysAreStableAcrossReloads) {
    auto first = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    auto second = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(first.has_value() && second.has_value());
    ASSERT_EQ(first->nodeCount(), second->nodeCount());
    for (std::uint32_t i = 0; i < first->nodeCount(); ++i) {
        const Node& node = first->node(NodeId{i});
        auto found = second->findNode(node.key);
        ASSERT_TRUE(found.has_value()) << node.key;
        EXPECT_EQ(second->node(*found).displayName, node.displayName);
    }
    // Project keys come from paths, so the .slnx model names the same projects.
    auto slnx = SolutionModel::load(io::realFileSystem(), sdk("SdkApp.slnx"));
    ASSERT_TRUE(slnx.has_value());
    const ProjectInfo& core = first->project(projectNamed(*first, "Core"));
    EXPECT_TRUE(slnx->findNode(first->node(core.node).key).has_value());
    EXPECT_EQ(first->findNode("no such key"), std::nullopt);
}

TEST(SolutionModel, UnreadableAndCorruptProjectsAreUnavailable) {
    MemTree tree;
    tree.write("Demo.slnx", R"(<Solution>
  <Project Path="Good/Good.vcxproj" />
  <Project Path="Locked/Locked.vcxproj" />
  <Project Path="Broken/Broken.vcxproj" />
  <Project Path="Shared/Shared.vcxitems" />
</Solution>)");
    tree.write("Good/Good.vcxproj", "<Project><ItemGroup><ClCompile Include=\"g.cpp\" /></ItemGroup></Project>");
    tree.write("Locked/Locked.vcxproj", "<Project />");
    tree.write("Broken/Broken.vcxproj", "<Project><ItemGroup>");
    tree.write("Broken/Broken.vcxproj.filters", "<Project />");
    tree.write("Good/Good.vcxproj.filters", "not xml");
    tree.fs.injectFault(io::FsOp::Read, MemTree::path("Locked/Locked.vcxproj"), ErrorCode::PermissionDenied);

    auto model = SolutionModel::load(tree.fs, MemTree::path("Demo.slnx"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    EXPECT_EQ(dump(*model, SolutionModel::root()),
              "Demo[Broken(unavailable),Good[g.cpp],Locked(unavailable),Shared(unsupported)]");
    EXPECT_EQ(model->project(ProjectId{1}).error->code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(model->project(ProjectId{2}).error->code(), ErrorCode::Corrupt);
    EXPECT_EQ(model->project(ProjectId{3}).kind, ProjectKind::SharedItems);
    // Good's unreadable .filters file is a warning; its file sits at the project root.
    bool filtersWarning = false;
    for (const std::string& warning : model->warnings()) {
        filtersWarning = filtersWarning || warning.starts_with("Good: filters ignored");
    }
    EXPECT_TRUE(filtersWarning);
}

TEST(SolutionModel, NestingCyclesFallBackToTheRoot) {
    MemTree tree;
    tree.write("Demo.sln", "Microsoft Visual Studio Solution File, Format Version 12.00\r\n"
                           "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"A\", \"A\", "
                           "\"{00000000-0000-4000-8000-00000000000A}\"\r\nEndProject\r\n"
                           "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"B\", \"B\", "
                           "\"{00000000-0000-4000-8000-00000000000B}\"\r\nEndProject\r\n"
                           "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"C\", \"C\", "
                           "\"{00000000-0000-4000-8000-00000000000C}\"\r\nEndProject\r\n"
                           "Global\r\n\tGlobalSection(NestedProjects) = preSolution\r\n"
                           "\t\t{00000000-0000-4000-8000-00000000000A} = {00000000-0000-4000-8000-00000000000B}\r\n"
                           "\t\t{00000000-0000-4000-8000-00000000000B} = {00000000-0000-4000-8000-00000000000A}\r\n"
                           "\t\t{00000000-0000-4000-8000-00000000000C} = {00000000-0000-4000-8000-00000000000A}\r\n"
                           "\tEndGlobalSection\r\nEndGlobal\r\n");
    auto model = SolutionModel::load(tree.fs, MemTree::path("Demo.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    // A and B nest in each other: both go to the root; C stays in A.
    EXPECT_EQ(dump(*model, SolutionModel::root()), "Demo[A[C],B]");
    EXPECT_EQ(model->warnings().size(), 2u);
}

TEST(SolutionModel, FailsOnlyForTheSolutionFile) {
    MemTree tree;
    auto missing = SolutionModel::load(tree.fs, MemTree::path("Missing.sln"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    tree.write("Bad.sln", "garbage\n");
    auto bad = SolutionModel::load(tree.fs, MemTree::path("Bad.sln"));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::Corrupt);
}

TEST(SolutionModel, LoadsCSharpCsprojHierarchyWithSubfoldersAndProjectReferences) {
    MemTree tree;
    tree.write("ManagedSuite.slnx", R"(<Solution>
  <Project Path="Core/Core.csproj" />
  <Project Path="App/App.csproj" />
</Solution>)");
    tree.write("Core/Core.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <ProjectGuid>{11111111-2222-3333-4444-555555555555}</ProjectGuid>
  </PropertyGroup>
  <ItemGroup>
    <Folder Include="EmptyFolder\" />
  </ItemGroup>
</Project>)");
    tree.write("Core/Greeter.cs", "namespace Core; public class Greeter {}\n");
    tree.write("Core/Models/User.cs", "namespace Core.Models; public record User(string Name);\n");
    tree.write("Core/bin/Debug/net10.0/Generated.cs", "// ignored build output\n");
    tree.write("Core/obj/Debug/net10.0/Temporary.cs", "// ignored intermediate\n");
    tree.write("Core/.vs/hidden.cs", "// ignored hidden folder\n");

    tree.write("App/App.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="..\Core\Core.csproj" />
    <Compile Remove="Excluded.cs" />
  </ItemGroup>
</Project>)");
    tree.write("App/Program.cs", "using Core;\n");
    tree.write("App/Excluded.cs", "// removed via Compile Remove\n");
    tree.write("App/Services/AuthService.cs", "namespace App.Services; public class AuthService {}\n");
    tree.write("App/appsettings.json", "{}\n");

    auto model = SolutionModel::load(tree.fs, MemTree::path("ManagedSuite.slnx"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    EXPECT_EQ(dump(*model, SolutionModel::root()),
              "ManagedSuite[App[Services[AuthService.cs],appsettings.json,Program.cs],"
              "Core[EmptyFolder,Models[User.cs],Greeter.cs]]");

    const ProjectId coreId = projectNamed(*model, "Core");
    const ProjectId appId = projectNamed(*model, "App");
    EXPECT_EQ(model->project(coreId).kind, ProjectKind::CSharp);
    EXPECT_EQ(model->project(coreId).nodeKind, NodeKind::Project);
    EXPECT_EQ(model->project(appId).kind, ProjectKind::CSharp);
    EXPECT_EQ(model->project(appId).nodeKind, NodeKind::Project);
    EXPECT_EQ(model->project(appId).dependencies, std::vector<ProjectId>{coreId});
    EXPECT_EQ(model->findProjectByGuid("11111111-2222-3333-4444-555555555555"), coreId);

    const std::string authPath = MemTree::path("App/Services/AuthService.cs");
    EXPECT_EQ(model->owningProject(authPath), appId);
    const auto authNode = model->findFileNode(authPath);
    ASSERT_TRUE(authNode.has_value());
    EXPECT_EQ(model->node(*authNode).itemType, ItemType::ClCompile);
    const auto servicesFolder = model->parent(*authNode);
    ASSERT_TRUE(servicesFolder.has_value());
    EXPECT_EQ(model->node(*servicesFolder).kind, NodeKind::Filter);
    EXPECT_EQ(model->node(*servicesFolder).path, MemTree::path("App/Services"));
    EXPECT_FALSE(model->findFileNode(MemTree::path("App/Excluded.cs")).has_value());
    EXPECT_FALSE(model->findFileNode(MemTree::path("Core/bin/Debug/net10.0/Generated.cs")).has_value());
    EXPECT_FALSE(model->findFileNode(MemTree::path("Core/obj/Debug/net10.0/Temporary.cs")).has_value());
}

TEST(SolutionModel, SupportsCppHierarchyModesFiltersAndFoldersAndPhysicalFolders) {
    MemTree tree;
    tree.write("Hierarchy.slnx", R"(<Solution>
  <Project Path="App/App.vcxproj" />
</Solution>)");
    tree.write("App/App.vcxproj", R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="src\core\engine.cpp" />
    <ClInclude Include="include\engine.h" />
  </ItemGroup>
</Project>)");
    tree.write("App/App.vcxproj.filters", R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
    <Filter Include="Source Files" />
  </ItemGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
  </ItemGroup>
</Project>)");
    tree.write("App/main.cpp", "int main() { return 0; }\n");
    tree.write("App/src/core/engine.cpp", "void run() {}\n");
    tree.write("App/include/engine.h", "#pragma once\n");

    auto filtersOnly =
        SolutionModel::load(tree.fs, MemTree::path("Hierarchy.slnx"), {.cppHierarchy = CppHierarchyMode::Filters});
    ASSERT_TRUE(filtersOnly.has_value());
    EXPECT_EQ(dump(*filtersOnly, SolutionModel::root()), "Hierarchy[App[Source Files[main.cpp],engine.cpp,engine.h]]");

    auto filtersAndFolders = SolutionModel::load(tree.fs, MemTree::path("Hierarchy.slnx"),
                                                 {.cppHierarchy = CppHierarchyMode::FiltersAndFolders});
    ASSERT_TRUE(filtersAndFolders.has_value());
    EXPECT_EQ(dump(*filtersAndFolders, SolutionModel::root()),
              "Hierarchy[App[include[engine.h],Source Files[main.cpp],src[core[engine.cpp]]]]");

    auto physicalOnly = SolutionModel::load(tree.fs, MemTree::path("Hierarchy.slnx"),
                                            {.cppHierarchy = CppHierarchyMode::PhysicalFolders});
    ASSERT_TRUE(physicalOnly.has_value());
    EXPECT_EQ(dump(*physicalOnly, SolutionModel::root()),
              "Hierarchy[App[include[engine.h],src[core[engine.cpp]],main.cpp]]");
}

// Generated scale fixture asserts actual model work/counts, not an unmeasured timing claim.
void writeScaleSolution(MemTree& tree, std::size_t count) {
    std::string solution = R"(<Solution><Configurations><BuildType Name="Debug" />
<BuildType Name="Release" /><Platform Name="x64" /></Configurations>)";
    for (std::size_t i = 0; i < count; ++i) {
        const auto directory = "p" + std::to_string(i);
        solution += "<Project Path=\"" + directory + "/p.vcxproj\" />";
        std::string project = R"(<Project><ItemGroup><ClCompile Include="main.cpp" />)";
        if (i)
            project += "<ProjectReference Include=\"../p" + std::to_string(i - 1) + "/p.vcxproj\" />";
        project += "</ItemGroup></Project>";
        tree.write(directory + "/p.vcxproj", project);
    }
    tree.write("Scale.slnx", solution + "</Solution>");
}

TEST(SolutionScale, FiveHundredProjectsPublishCompleteIndexedTreeAndConfigurationProfile) {
    MemTree tree;
    writeScaleSolution(tree, 500);
    std::vector<SolutionLoadProgress> progress;
    auto model =
        SolutionModel::load(tree.fs, MemTree::path("Scale.slnx"),
                            {.progress = [&](const SolutionLoadProgress& value) { progress.push_back(value); }});
    ASSERT_TRUE(model) << model.error().describe();
    ASSERT_EQ(model->projects().size(), 500U);
    EXPECT_EQ(model->configurations().size(), 2U);
    EXPECT_EQ(model->loadProfile().projects, 500U);
    EXPECT_EQ(model->loadProfile().cppProjectsRead, 500U);
    EXPECT_EQ(model->loadProfile().managedDirectoryLists, 0U);
    ASSERT_EQ(progress.size(), 501U);
    EXPECT_EQ(progress.front().completedProjects, 0U);
    EXPECT_EQ(progress.back().completedProjects, 500U);
    for (std::size_t i = 0; i < 500; ++i) {
        const auto directory = "p" + std::to_string(i);
        const auto id = model->findProjectByPath(MemTree::path(directory + "/p.vcxproj"));
        ASSERT_TRUE(id);
        EXPECT_EQ(id->value(), i);
        EXPECT_TRUE(model->findFileNode(MemTree::path(directory + "/main.cpp")));
        EXPECT_TRUE(model->projectConfiguration(*id, 0));
        if (i) {
            ASSERT_EQ(model->project(*id).dependencies.size(), 1U);
            EXPECT_EQ(model->project(*id).dependencies.front().value(), i - 1);
        }
    }
}

TEST(SolutionScale, UnloadedProjectsKeepTheirIdentityWithoutReadingTheirProjectFiles) {
    MemTree tree;
    writeScaleSolution(tree, 500);
    SolutionLoadOptions options;
    for (std::size_t i = 0; i < 50; ++i)
        options.unloadedProjectPaths.push_back(MemTree::path("p" + std::to_string(i) + "/p.vcxproj"));
    auto model = SolutionModel::load(tree.fs, MemTree::path("Scale.slnx"), std::move(options));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->projects().size(), 500U);
    EXPECT_EQ(model->loadProfile().cppProjectsRead, 450U);
    EXPECT_EQ(model->project(ProjectId{0}).nodeKind, NodeKind::UnavailableProject);
    EXPECT_TRUE(model->findProjectByPath(MemTree::path("p0/p.vcxproj")));
    EXPECT_FALSE(model->findFileNode(MemTree::path("p0/main.cpp")));
}

TEST(SolutionScale, CancellationDiscardsPartialTreeAndLaterLoadIsIndependent) {
    MemTree tree;
    writeScaleSolution(tree, 500);
    std::stop_source stop;
    std::size_t completed = 0;
    auto cancelled = SolutionModel::load(tree.fs, MemTree::path("Scale.slnx"),
                                         {.stop = stop.get_token(), .progress = [&](const SolutionLoadProgress& value) {
                                              completed = value.completedProjects;
                                              if (completed == 37)
                                                  stop.request_stop();
                                          }});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(completed, 37U);
    auto retry = SolutionModel::load(tree.fs, MemTree::path("Scale.slnx"));
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->projects().size(), 500U);
    // An already-stopped request wins even over an invalid/nonexistent input.
    auto beforeRead = SolutionModel::load(tree.fs, "not-an-absolute-solution", {.stop = stop.get_token()});
    ASSERT_FALSE(beforeRead);
    EXPECT_EQ(beforeRead.error().code(), ErrorCode::Cancelled);
}

TEST(SolutionModel, ExplicitApproximationIsReadOnlyEvenWhenEvaluatedItemsAreOffered) {
    MemTree tree;
    tree.write("Approx.slnx", R"(<Solution><Project Path="p/p.vcxproj" /></Solution>)");
    tree.write("p/p.vcxproj", R"(<Project><Import Project="unavailable.props" />
<ItemGroup Condition="false"><ClCompile Include="literal.cpp" /></ItemGroup></Project>)");
    auto model =
        SolutionModel::load(tree.fs, MemTree::path("Approx.slnx"), {.mode = SolutionModelMode::ApproximateReadOnly});
    ASSERT_TRUE(model);
    EXPECT_TRUE(model->approximate());
    EXPECT_FALSE(model->approximationNotice().empty());
    EXPECT_TRUE(model->findFileNode(MemTree::path("p/literal.cpp"))); // Literal, not condition evaluation.
    auto mutation = model->requireProjectMutation();
    auto execution = model->requireExecution();
    ASSERT_FALSE(mutation);
    ASSERT_FALSE(execution);
    EXPECT_EQ(mutation.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(execution.error().code(), ErrorCode::Unsupported);
    auto evaluated =
        SolutionModel::load(tree.fs, MemTree::path("Approx.slnx"),
                            {.evaluated = {EvaluatedProjectItems{}}, .mode = SolutionModelMode::ApproximateReadOnly});
    ASSERT_FALSE(evaluated);
    EXPECT_EQ(evaluated.error().code(), ErrorCode::Unsupported);
#ifndef _WIN32
    auto defaultModel = SolutionModel::load(tree.fs, MemTree::path("Approx.slnx"));
    ASSERT_TRUE(defaultModel);
    EXPECT_TRUE(defaultModel->approximate());
#endif
}

} // namespace
} // namespace frost::project
