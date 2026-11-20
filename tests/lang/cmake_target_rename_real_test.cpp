// SPDX-License-Identifier: Apache-2.0
// Deferred CMK-9 real configure oracle: target rename proposal changes actual File API graph.
#include "frost/lang/cmake_refactoring.hpp"
#include "frost/io/process.hpp"
#include "frost/io/path.hpp"
#include "frost/text/text_buffer.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <algorithm>
namespace frost::lang {
TEST(CMakeTargetRenameReal, ReconfiguredFileApiReportsRenamedTargetAndDependency) {
    frost::test::ScratchDir scratch("cmk9-configure");
    auto& fs=io::realFileSystem();
    const auto root=io::joinPath(scratch.path(),"source").value();
    const auto build=io::joinPath(scratch.path(),"build").value();
    const auto path=io::joinPath(root,"CMakeLists.txt").value();
    const std::string source="cmake_minimum_required(VERSION 3.25)\nproject(RenameFixture LANGUAGES NONE)\n"
        "add_custom_target(alpha)\nadd_custom_target(consumer)\nadd_dependencies(consumer alpha)\n";
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(path,source));
    ASSERT_TRUE(project::writeCMakeFileApiQuery(fs,build));
    io::ProcessSpec spec;
    spec.executable=FROST_TEST_CMAKE; spec.arguments={"-S",root,"-B",build,"-G","Ninja"};
    auto configured=io::runToCompletion(io::realProcessLauncher(),spec,"",std::chrono::seconds(90));
    ASSERT_TRUE(configured);
    ASSERT_EQ(configured->exitCode,0) << configured->stderrBytes;
    auto before=project::readCMakeFileApiReply(fs,build,"");
    ASSERT_TRUE(before);
    ASSERT_TRUE(*before);
    CMakeLanguageInput input;
    input.path=path; input.source=text::TextBuffer::fromUtf8(source).value().snapshot();
    input.model=std::make_shared<project::CMakeProject>(**before);
    for (const auto& file:input.model->inputs)
        if (!file.cmake && file.path!=path) {
            auto bytes=fs.readFile(file.path); ASSERT_TRUE(bytes); input.inputs.emplace_back(file.path,*bytes);
        }
    auto proposal=planCMakeTargetRename(input,{2,19},"bravo");
    ASSERT_TRUE(proposal) << proposal.error().describe();
    for (const auto& file:proposal->edit.files) {
        auto bytes=fs.readFile(file.path); ASSERT_TRUE(bytes);
        auto changed=applyTextEdits(*bytes,file.edits,PositionEncoding::Utf16); ASSERT_TRUE(changed);
        ASSERT_TRUE(fs.writeFileAtomic(file.path,*changed));
    }
    auto reconfigured=io::runToCompletion(io::realProcessLauncher(),spec,"",std::chrono::seconds(90));
    ASSERT_TRUE(reconfigured);
    ASSERT_EQ(reconfigured->exitCode,0) << reconfigured->stderrBytes;
    auto after=project::readCMakeFileApiReply(fs,build,"");
    ASSERT_TRUE(after); ASSERT_TRUE(*after);
    const auto* alpha=(**before).findTarget("alpha");
    const auto* bravo=(**after).findTarget("bravo");
    const auto* consumer=(**after).findTarget("consumer");
    ASSERT_NE(alpha,nullptr); ASSERT_NE(bravo,nullptr); ASSERT_NE(consumer,nullptr);
    EXPECT_EQ((**after).findTarget("alpha"),nullptr);
    EXPECT_NE(std::find(consumer->dependencies.begin(),consumer->dependencies.end(),bravo->id),consumer->dependencies.end());
    EXPECT_EQ(inspectCMakeSource(path,text::TextBuffer::fromUtf8(fs.readFile(path).value()).value().snapshot()).size(),0U);
}
} // namespace frost::lang
