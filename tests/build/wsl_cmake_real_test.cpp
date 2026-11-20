// SPDX-License-Identifier: Apache-2.0
// Deferred CMK-16 actual Windows->named WSL configure/build/run/CDB oracle; no tool is executed in authoring.
#include "frost/build/remote_process.hpp"
#include "frost/build/wsl_cmake.hpp"
#include "frost/io/file_system.hpp"
#include "frost/rpc/json.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_environment_value.hpp"
#include <gtest/gtest.h>
namespace frost::build {
namespace {
using namespace std::chrono_literals;
[[maybe_unused]] std::string trim(std::string text){while(!text.empty()&&(text.back()=='\r'||text.back()=='\n'))text.pop_back();return text;}
[[maybe_unused]] io::ProcessSpec python(const RemoteTarget& target,std::string code,std::vector<std::string> arguments={}) {
    io::ProcessSpec spec;spec.executable=target.remotePython;spec.arguments={"-I","-u","-c",std::move(code)};
    spec.arguments.insert(spec.arguments.end(),arguments.begin(),arguments.end());spec.workingDirectory="/tmp";return spec;
}
struct OwnedWslBuild {
    RemoteProcessLauncher* launcher;RemoteTarget endpoint;std::string path;
    ~OwnedWslBuild(){auto spec=python(endpoint,"import os,pathlib,shutil,sys; p=pathlib.Path(sys.argv[1]); assert p.is_absolute() and p.parent==pathlib.Path('/tmp') and p.name.startswith('frost-cmk16-'); assert (p/'fixture-owner').read_text()=='FrostIDE-CMK16'; shutil.rmtree(p)",{path});
        auto cleaned=io::runToCompletion(*launcher,spec,"",30s);EXPECT_TRUE(cleaned);if(cleaned)EXPECT_EQ(cleaned->exitCode,0)<<cleaned->stderrBytes;}
};
TEST(WslCMakeReal, ActualMappedSourceBuildAndGeneratedCompilationDatabase) {
#ifndef _WIN32
    GTEST_SKIP()<<"CMK-16 is the Windows IDE driving a named WSL distribution; Linux guardian has separate real coverage";
#else
    RemoteTarget endpoint;endpoint.id="wsl-acceptance";endpoint.name="Explicit WSL build acceptance";endpoint.executionApproved=true;
    endpoint.transportExecutable = test::environmentValue("FROST_REMOTE_TEST_WSL").value_or("");
    endpoint.distribution = test::environmentValue("FROST_REMOTE_TEST_DISTRIBUTION").value_or("");
    const auto cmake = test::environmentValue("FROST_REMOTE_TEST_CMAKE").value_or("");
    const auto ninja = test::environmentValue("FROST_REMOTE_TEST_NINJA").value_or("");
    const auto compiler = test::environmentValue("FROST_REMOTE_TEST_CXX").value_or("");
    if(endpoint.transportExecutable.empty()||endpoint.distribution.empty()||cmake.empty()||ninja.empty()||compiler.empty())
        GTEST_SKIP()<<"No explicitly selected/provisioned WSL CMake/Ninja/Clang endpoint; not counted as WSL build acceptance";
    if (auto python = test::environmentValue("FROST_REMOTE_TEST_LINUX_PYTHON"); python && !python->empty())
        endpoint.remotePython = *python;
    RemoteProcessLauncher initial(io::realProcessLauncher(),endpoint,{.admit=[]()->Result<void>{return {};}});
    frost::test::ScratchDir scratch("cmk16-wsl");auto& fs=io::realFileSystem();
    const auto source=io::joinPath(scratch.path(),"source with space").value();ASSERT_TRUE(fs.createDirectories(source));
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(source,"CMakeLists.txt").value(),"cmake_minimum_required(VERSION 3.25)\nproject(WslFixture LANGUAGES CXX)\nadd_executable(remote_fixture main.cpp)\n"));
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(source,"main.cpp").value(),"int main() { return 27; }\n"));
    io::ProcessSpec map;map.executable="/usr/bin/wslpath";map.arguments={"-u",source};map.workingDirectory="/tmp";
    auto mapped=io::runToCompletion(initial,map,"",30s);ASSERT_TRUE(mapped);ASSERT_EQ(mapped->exitCode,0)<<mapped->stderrBytes;
    const auto remoteSource=trim(mapped->stdoutBytes);ASSERT_TRUE(io::isAbsolutePath(remoteSource,io::PathStyle::Posix));
    endpoint.mappings={{source,remoteSource}};
    RemoteProcessLauncher launcher(io::realProcessLauncher(),endpoint,{.admit=[]()->Result<void>{return {};}});
    auto create=python(endpoint,"import pathlib,tempfile; p=pathlib.Path(tempfile.mkdtemp(prefix='frost-cmk16-',dir='/tmp')); (p/'fixture-owner').write_text('FrostIDE-CMK16'); print(p)");
    auto created=io::runToCompletion(launcher,create,"",30s);ASSERT_TRUE(created);ASSERT_EQ(created->exitCode,0)<<created->stderrBytes;
    const auto build=trim(created->stdoutBytes);ASSERT_TRUE(build.starts_with("/tmp/frost-cmk16-"));
    OwnedWslBuild owned{&launcher,endpoint,build};
    WslCMakeProfile profile;profile.id="actual";profile.name="Actual WSL fixture";profile.targetId=endpoint.id;
    profile.localSourceDirectory=source;profile.cmakeProgram=cmake;profile.ninjaProgram=ninja;profile.buildDirectory=build;
    profile.cacheDefinitions={"CMAKE_CXX_COMPILER:FILEPATH="+compiler,"CMAKE_BUILD_TYPE:STRING=Debug"};
    auto configure=wslCMakeCommand(profile,endpoint,WslCMakeOperation::Configure,2);ASSERT_TRUE(configure);
    auto probe=*configure;probe.arguments={"--version"};auto cmakeVersion=io::runToCompletion(launcher,probe,"",30s);ASSERT_TRUE(cmakeVersion);ASSERT_EQ(cmakeVersion->exitCode,0);
    probe.executable=ninja;auto ninjaVersion=io::runToCompletion(launcher,probe,"",30s);ASSERT_TRUE(ninjaVersion);ASSERT_EQ(ninjaVersion->exitCode,0);
    ASSERT_TRUE(validateWslCMakeToolVersions(cmakeVersion->stdoutBytes,ninjaVersion->stdoutBytes));
    auto configured=io::runToCompletion(launcher,*configure,"",120s);ASSERT_TRUE(configured);ASSERT_EQ(configured->exitCode,0)<<configured->stderrBytes;
    auto command=wslCMakeCommand(profile,endpoint,WslCMakeOperation::Build,2);ASSERT_TRUE(command);
    auto built=io::runToCompletion(launcher,*command,"",120s);ASSERT_TRUE(built);ASSERT_EQ(built->exitCode,0)<<built->stderrBytes;
    auto database=io::runToCompletion(launcher,python(endpoint,"import pathlib,sys; print((pathlib.Path(sys.argv[1])/'compile_commands.json').read_text())",{build}),"",30s);
    ASSERT_TRUE(database);ASSERT_EQ(database->exitCode,0);auto actual=rpc::parseJson(database->stdoutBytes);ASSERT_TRUE(actual);ASSERT_TRUE(actual->isArray());
    bool mappedActualSource=false;for(const auto& row:actual->items())if(auto local=mapRemoteToLocal(endpoint,row.stringAt("file"));local&&*local==io::joinPath(source,"main.cpp").value())mappedActualSource=true;
    EXPECT_TRUE(mappedActualSource)<<"Compilation database/source relationship comes from actual remote CMake output";
    io::ProcessSpec program;program.executable=build+"/remote_fixture";program.workingDirectory=build;
    auto ran=io::runToCompletion(launcher,program,"",30s);ASSERT_TRUE(ran);EXPECT_EQ(ran->exitCode,27);
#endif
}
} // namespace
} // namespace frost::build
