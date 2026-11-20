// SPDX-License-Identifier: Apache-2.0
// Exact WSL CMake/Ninja command construction and bounded application profile persistence.
#include "frost/build/wsl_cmake.hpp"
#include "frost/rpc/json.hpp"
#include <algorithm>
#include <cctype>
#include <set>
namespace frost::build {
namespace {
bool id(std::string_view value){return !value.empty()&&value.size()<=128&&std::ranges::all_of(value,[](unsigned char c){return std::isalnum(c)||c=='_'||c=='.'||c=='-';});}
bool contains(std::string_view root,std::string_view value){return root==value || (value.starts_with(root)&&!root.empty()&&(root.back()=='/'||(value.size()>root.size()&&value[root.size()]=='/')));}
}
Result<io::ProcessSpec> wslCMakeCommand(const WslCMakeProfile& profile,const RemoteTarget& target,WslCMakeOperation operation,unsigned jobs) {
    if(auto valid=validateRemoteTarget(target);!valid)return std::unexpected(valid.error());
    if(target.transport!=RemoteTransport::Wsl||profile.targetId!=target.id||!id(profile.id)||profile.name.empty()||jobs==0||jobs>1024)
        return fail(ErrorCode::InvalidArgument,"WSL CMake needs a named WSL target/profile and a governed nonzero job count");
    if(!io::isAbsolutePath(profile.cmakeProgram,io::PathStyle::Posix)||!io::isAbsolutePath(profile.ninjaProgram,io::PathStyle::Posix)||
        !io::isAbsolutePath(profile.buildDirectory,io::PathStyle::Posix)||profile.buildDirectory.find_first_of("\r\n\0",0,3)!=profile.buildDirectory.npos)
        return fail(ErrorCode::InvalidArgument,"WSL CMake/Ninja/build paths must be explicit absolute Linux paths");
    auto source=mapLocalToRemote(target,profile.localSourceDirectory);if(!source)return std::unexpected(source.error());
    const auto build=io::normalizePath(profile.buildDirectory,io::PathStyle::Posix).value();
    // Dot traversal cannot silently change which configured directory owns outputs.
    for(std::size_t start=0;start<=build.size();) {
        const auto end=build.find('/',start);const auto part=std::string_view(build).substr(start,end==build.npos?build.size()-start:end-start);
        if(part=="."||part=="..")return fail(ErrorCode::InvalidArgument,"WSL build directory contains dot traversal");
        if(end==build.npos)break;start=end+1;
    }
    if(contains(*source,build)||contains(build,*source))return fail(ErrorCode::InvalidArgument,"WSL build output must be outside and not contain the mapped source tree");
    io::ProcessSpec spec;spec.executable=profile.cmakeProgram;spec.workingDirectory=*source;
    spec.stdinMode=io::Stdio::Null;spec.stdoutMode=io::Stdio::Pipe;spec.stderrMode=io::Stdio::Pipe;
    spec.killOnClose=true;spec.belowNormalPriority=true;
    if(operation==WslCMakeOperation::Configure) {
        spec.arguments={"-S",*source,"-B",build,"-G","Ninja","-DCMAKE_MAKE_PROGRAM="+profile.ninjaProgram,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
        for(const auto& definition:profile.cacheDefinitions) {
            const auto equals=definition.find('=');
            if(equals==definition.npos||equals==0||definition.find('\0')!=definition.npos ||
                !std::ranges::all_of(std::string_view(definition).substr(0,equals),[](unsigned char c){return std::isalnum(c)||c=='_'||c==':'||c=='.'||c=='-';}))
                return fail(ErrorCode::InvalidArgument,"WSL cache definitions must be KEY[:TYPE]=VALUE structured arguments");
            spec.arguments.push_back("-D"+definition);
        }
    } else {
        spec.arguments={"--build",build,"--parallel",std::to_string(jobs)};
        const auto targetName=operation==WslCMakeOperation::Clean?"clean":operation==WslCMakeOperation::ListTargets?"help":profile.buildTarget;
        if(!targetName.empty()) {
            if(targetName.starts_with('-')||targetName.find_first_of("\r\n\0",0,3)!=targetName.npos)
                return fail(ErrorCode::InvalidArgument,"WSL build target cannot be an option or contain control characters");
            spec.arguments.insert(spec.arguments.end(),{"--target",targetName});
        }
    }
    if(auto valid=validateRemoteWorkload(spec);!valid)return std::unexpected(valid.error());
    return spec;
}
Result<void> validateWslCMakeToolVersions(std::string_view cmakeOutput,std::string_view ninjaOutput) {
    const std::string cmake="cmake version " FROST_PINNED_CMAKE_VERSION;
    if(!cmakeOutput.starts_with(cmake)||(cmakeOutput.size()>cmake.size()&&cmakeOutput[cmake.size()]!='\r'&&cmakeOutput[cmake.size()]!='\n'))
        return fail(ErrorCode::Unsupported,"Remote CMake must match the pinned " FROST_PINNED_CMAKE_VERSION " toolchain");
    const auto end=ninjaOutput.find_first_of("\r\n");
    if(ninjaOutput.substr(0,end)!=FROST_PINNED_NINJA_VERSION)
        return fail(ErrorCode::Unsupported,"Remote Ninja must match the pinned " FROST_PINNED_NINJA_VERSION " toolchain");
    return {};
}
Result<std::vector<WslCMakeProfile>> parseWslCMakeProfiles(std::string_view bytes) {
    if(bytes.size()>1024U*1024U)return fail(ErrorCode::OutOfRange,"WSL profile registry exceeds 1 MiB");
    auto parsed=rpc::parseJson(bytes);if(!parsed)return std::unexpected(parsed.error());
    const auto* rows=parsed->find("profiles");if(parsed->integerAt("version")!=1||!rows||!rows->isArray()||rows->size()>128)
        return fail(ErrorCode::Corrupt,"Unsupported or malformed application WSL CMake profile registry");
    std::vector<WslCMakeProfile> profiles;std::set<std::string> ids;
    for(const auto& row:rows->items()) {
        WslCMakeProfile profile;
        for(auto [name,field]:{std::pair{"id",&profile.id},{"name",&profile.name},{"target",&profile.targetId},{"source",&profile.localSourceDirectory},
            {"cmake",&profile.cmakeProgram},{"ninja",&profile.ninjaProgram},{"build",&profile.buildDirectory},{"buildTarget",&profile.buildTarget}}) {
            const auto* value=row.find(name);if(!value||!value->asString())return fail(ErrorCode::Corrupt,"Missing/wrong-typed WSL profile field");*field=*value->asString();
        }
        const auto* definitions=row.find("definitions");if(!definitions||!definitions->isArray()||definitions->size()>256)return fail(ErrorCode::Corrupt,"Malformed WSL cache definitions");
        for(const auto& value:definitions->items()){if(!value.asString())return fail(ErrorCode::Corrupt,"Malformed WSL cache definition");profile.cacheDefinitions.push_back(*value.asString());}
        if(!id(profile.id)||!ids.insert(profile.id).second)return fail(ErrorCode::Corrupt,"Invalid/duplicate WSL profile identity");
        profiles.push_back(std::move(profile));
    }
    return profiles;
}
std::string wslCMakeProfilesJson(const std::vector<WslCMakeProfile>& profiles) {
    auto rows=rpc::Json::array();for(const auto& profile:profiles) {
        auto definitions=rpc::Json::array();for(const auto& definition:profile.cacheDefinitions)definitions.push(definition);
        rows.push(rpc::Json::object().set("id",profile.id).set("name",profile.name).set("target",profile.targetId).set("source",profile.localSourceDirectory)
            .set("cmake",profile.cmakeProgram).set("ninja",profile.ninjaProgram).set("build",profile.buildDirectory).set("buildTarget",profile.buildTarget).set("definitions",std::move(definitions)));
    }
    return rpc::Json::object().set("version",1).set("profiles",std::move(rows)).dump();
}
} // namespace frost::build
