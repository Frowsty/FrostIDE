// SPDX-License-Identifier: Apache-2.0
//
// Tests for SolutionModel (frost/project/solution_model.hpp): the Solution Explorer tree of the
// SDK-shaped fixture from .sln and .slnx, unsupported and unavailable projects, lookups,
// dependencies, configuration mapping, stable keys and nesting problems.
#include "frost/io/file_system.hpp"
#include "frost/project/solution_model.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
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
    "Tools[Gone(unavailable),Packager(unsupported),README.md],"
    "App[Header Files[app.h],Resource Files[app.ico,app.rc],Source Files[app.cpp,main.cpp],README.md]]";

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
    EXPECT_EQ(packager.nodeKind, NodeKind::UnsupportedProject);
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

} // namespace
} // namespace frost::project
