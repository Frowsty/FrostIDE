// SPDX-License-Identifier: Apache-2.0
// FEC-12 real compiler/registry oracle: generated files enter a disposable pinned engine executable.
#include "frost/lang/file_templates.hpp"
#include "binding_corpus_support.hpp"
#include <algorithm>
namespace frost::engine {
namespace {
TEST(ComponentTemplateCorpus, GeneratedExecutableMemberBuildsAndAppearsInActualSchema) {
    test::RealEngineBuild pinned;auto ready=pinned.prepare();ASSERT_TRUE(ready)<<ready.error().message();
    test::binding::Evidence evidence;auto clone=test::binding::disposableClone(evidence,pinned.corpus);
    ASSERT_TRUE(clone)<<clone.error().message();
    auto files=lang::renderFileTemplate({lang::FileTemplateKind::FrostComponent,"Fec12GeneratedComponent","Corpus","Generated component acceptance fixture"});
    ASSERT_TRUE(files);auto& fs=io::realFileSystem();
    const auto directory=*clone+"/Frost-Editor/src/Fec12Generated";ASSERT_TRUE(fs.createDirectories(directory));
    for(const auto& file:*files)ASSERT_TRUE(fs.writeFileAtomic(directory+"/"+file.name,file.contents));
    // Direct executable source membership is the production template's required profile.
    // Do not put a registration-only TU in FrostEditorCore's discardable static archive.
    auto lists=fs.readFile(*clone+"/CMakeLists.txt");ASSERT_TRUE(lists);
    lists->append("\ntarget_sources(FrostEditor PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/Frost-Editor/src/Fec12Generated/Fec12GeneratedComponent.cpp\")\n");
    ASSERT_TRUE(fs.writeFileAtomic(*clone+"/CMakeLists.txt",*lists));
    auto built=test::binding::build(evidence,*clone,false);ASSERT_TRUE(built)<<built.error().message();
    auto program=findEngineEditorProgram(fs,*clone,FROST_CORPUS_FROSTENGINE_PRESET);ASSERT_TRUE(program);
    const auto project=evidence.root+"/ComponentProject";ASSERT_TRUE(pinned.copyProject(project));
    auto output=evidence.command("generated-component-schema",*program,{"schema.export","--project",project,"--log-level","error"},project);
    ASSERT_TRUE(output)<<output.error().message();ASSERT_EQ(output->exitCode,0);
    auto envelope=rpc::parseJson(output->stdoutBytes);ASSERT_TRUE(envelope);ASSERT_EQ(envelope->boolAt("ok"),true);
    const auto* result=envelope->find("result");ASSERT_NE(result,nullptr);
    auto schema=parseSchemaDocument(*result);ASSERT_TRUE(schema)<<schema.error().message();
    const auto component=std::ranges::find(schema->components,"Fec12GeneratedComponent",&SchemaComponent::name);
    ASSERT_NE(component,schema->components.end());
    EXPECT_NE(std::ranges::find(component->fields,"Value",&SchemaField::name),component->fields.end());
    EXPECT_NE(component->source.file.find("Fec12GeneratedComponent.cpp"),std::string::npos);
}
}
}
