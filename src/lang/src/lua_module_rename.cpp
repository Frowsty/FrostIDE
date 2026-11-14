// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/lua_module_rename.hpp"
#include "frost/lang/uri.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace frost::lang {
namespace {
bool moduleName(std::string_view name) {
    if(name.empty()||name.size()>256||name.front()=='.'||name.back()=='.') return false;
    char previous=0;
    for(char c:name) { if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.')||
        (c=='.'&&previous=='.'))return false;previous=c; } return true;
}
}
Result<LuaModuleRenameProposal> planLuaModuleRename(io::IFileSystem& fs,const LuaModuleRenameRequest& request,std::stop_token stop) {
    if(!io::isAbsolutePath(request.workspaceRoot)||!io::isAbsolutePath(request.fromPath)||!io::isAbsolutePath(request.toPath)||
       io::parentPath(request.fromPath)!=io::parentPath(request.toPath)||request.runtimePaths.empty()||request.runtimePaths.size()>32||request.sources.size()>10000)
        return fail(ErrorCode::Unsupported,"Module rename requires same-directory Lua endpoints and an explicit bounded runtime.path/source inventory");
    const auto rootKey=documentKey(request.workspaceRoot),sourceKey=documentKey(request.fromPath);
    if(!sourceKey.starts_with(rootKey)||sourceKey.size()<=rootKey.size()||
       (sourceKey[rootKey.size()]!='/'&&sourceKey[rootKey.size()]!='\\'))
        return fail(ErrorCode::Unsupported,"Selected Lua module must be inside the captured workspace");
    const auto leaf=io::fileName(request.toPath);
    if(!leaf.ends_with(".lua")||!moduleName(leaf.substr(0,leaf.size()-4))||leaf.substr(0,leaf.size()-4).find('.')!=std::string_view::npos)
        return fail(ErrorCode::InvalidArgument,"Choose a simple Lua module filename ending in .lua");
    auto from=fs.physicalPath(request.fromPath);if(!from)return std::unexpected(from.error());
    auto toExists=fs.exists(request.toPath);if(!toExists)return std::unexpected(toExists.error());
    if(*toExists)return fail(ErrorCode::AlreadyExists,"The new Lua module endpoint already exists");
    LuaModuleRenameProposal result;std::map<std::string,std::size_t> probes;std::size_t probeBytes=0,pathBytes=0,editBytes=0;
    const auto probe=[&](const std::string& path)->Result<std::string> {
        const auto key=documentKey(path);if(auto found=probes.find(key);found!=probes.end())return result.probes[found->second].physicalPath;
        if(probes.size()>=20000)return fail(ErrorCode::OutOfRange,"Lua module lookup probe budget exceeded");
        pathBytes+=path.size();if(pathBytes>16*1024*1024)return fail(ErrorCode::OutOfRange,"Lua lookup path budget exceeded");
        auto exists=fs.exists(path);if(!exists)return std::unexpected(exists.error());
        LuaModuleProbe receipt;receipt.path=path;
        if(*exists) {
            auto status=fs.status(path);if(!status||status->type!=io::FileType::Regular)return fail(ErrorCode::Unsupported,"Lua lookup candidate is not a regular file: "+path);
            auto bytes=fs.readFileSnapshot(path,8*1024*1024);if(!bytes)return std::unexpected(bytes.error());
            probeBytes+=bytes->size();if(probeBytes>128*1024*1024)return fail(ErrorCode::OutOfRange,"Lua lookup before-images exceed 128 MiB");
            auto physical=fs.physicalPath(path);if(!physical)return std::unexpected(physical.error());
            receipt.bytes=std::move(*bytes);receipt.physicalPath=std::move(*physical);
        }
        probes.emplace(key,result.probes.size());result.probes.push_back(std::move(receipt));return result.probes.back().physicalPath;
    };
    const auto resolve=[&](std::string name,bool after)->Result<std::string> {
        std::ranges::replace(name,'.','/');std::set<std::string> matches;
        for(const auto& pattern:request.runtimePaths) {
            if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Module rename cancelled");
            const auto marker=pattern.find('?');
            if(pattern.size()>4096||marker==pattern.npos||pattern.find('?',marker+1)!=pattern.npos||pattern.find_first_of("$;\r\n")!=pattern.npos)
                return fail(ErrorCode::Unsupported,"runtime.path contains a dynamic or unsupported search pattern");
            auto spelling=pattern.substr(0,marker)+name+pattern.substr(marker+1);
            auto path=io::isAbsolutePath(spelling)?io::normalizePath(spelling):io::joinPath(request.workspaceRoot,spelling);
            if(!path)return std::unexpected(path.error());
            if(after&&documentKey(*path)==documentKey(request.fromPath))continue;
            if(after&&documentKey(*path)==documentKey(request.toPath)) {matches.insert(documentKey(request.toPath));continue;}
            auto physical=probe(*path);if(!physical)return std::unexpected(physical.error());
            if(!physical->empty())matches.insert(documentKey(*physical));
        }
        if(matches.size()>1)return fail(ErrorCode::Unsupported,"Lua module resolution is ambiguous across runtime.path");
        return matches.empty()?std::string{}:*matches.begin();
    };
    std::size_t retained=0;const auto started=systemClock().steadyNow();
    for(const auto& source:request.sources) {
        if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Module rename cancelled");
        if(systemClock().steadyNow()-started>std::chrono::seconds(30))return fail(ErrorCode::Timeout,"Module rename source analysis budget exceeded");
        retained+=source.snapshot.size().value();if(retained>64*1024*1024)return fail(ErrorCode::OutOfRange,"Lua workspace snapshots exceed 64 MiB");
        syntax::LuaAstLimits limits;limits.stop=stop;auto ast=syntax::projectLuaSnapshot(source.snapshot,limits);if(!ast)return std::unexpected(ast.error());
        LuaModuleFileEdits file{source.path,source.snapshot,{}};
        auto sourcePhysical=fs.physicalPath(source.path);if(!sourcePhysical)return std::unexpected(sourcePhysical.error());
        for(std::uint32_t i=0;i<ast->nodes.size();++i) {
            if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Module rename cancelled");
            const auto& node=ast->nodes[i];if(node.kind!="identifier")continue;
            const auto name=source.snapshot.text(node.range);
            if(name=="_ENV"||name=="_G"||name=="package"||name=="debug"||name=="module"||name=="getfenv"||name=="setfenv"||name=="load"||name=="loadstring"||name=="loadfile"||name=="dofile")
                return fail(ErrorCode::Unsupported,"Lua environment/package/searcher changes make module users unprovable: "+source.path);
            if(name!="require")continue;
            const auto parent=node.parent;
            if(parent!=syntax::kNoLuaNode&&((ast->nodes[parent].kind=="dot_index_expression"&&node.field=="field")||
                (ast->nodes[parent].kind=="method_index_expression"&&node.field=="method")))continue;
            if(parent==syntax::kNoLuaNode||ast->nodes[parent].kind!="function_call"||node.field!="name")
                return fail(ErrorCode::Unsupported,"A require alias/shadowing/write cannot be renamed safely: "+source.path);
            std::uint32_t arguments=syntax::kNoLuaNode;
            for(auto child:ast->nodes[parent].children)if(ast->nodes[child].kind=="arguments")arguments=child;
            if(arguments==syntax::kNoLuaNode||ast->nodes[arguments].children.size()!=1)return fail(ErrorCode::Unsupported,"Dynamic/multiple require arguments are unsupported");
            const auto literal=ast->nodes[arguments].children.front();const auto& string=ast->nodes[literal];
            auto spelling=source.snapshot.text(string.range);
            if(string.kind!="string"||spelling.size()<2||(spelling.front()!='\''&&spelling.front()!='"')||spelling.back()!=spelling.front()||spelling.find('\\')!=spelling.npos)
                return fail(ErrorCode::Unsupported,"Module rename requires a plain quoted literal require argument");
            auto module=spelling.substr(1,spelling.size()-2);if(!moduleName(module))return fail(ErrorCode::Unsupported,"Unsupported Lua module literal");
            auto oldTarget=resolve(module,false);if(!oldTarget)return std::unexpected(oldTarget.error());
            if(*oldTarget!=documentKey(*from)) {
                auto unchanged=resolve(module,true);if(!unchanged)return std::unexpected(unchanged.error());
                if(*unchanged!=*oldTarget)return fail(ErrorCode::Unsupported,"Renaming would change an existing require binding (including unresolved users): "+module);
                continue;
            }
            const auto dot=module.rfind('.');auto renamed=(dot==module.npos?std::string{}:module.substr(0,dot+1))+std::string(leaf.substr(0,leaf.size()-4));
            auto newTarget=resolve(renamed,true);if(!newTarget)return std::unexpected(newTarget.error());
            if(*newTarget!=documentKey(request.toPath))return fail(ErrorCode::Unsupported,"New module name does not resolve uniquely to the renamed file");
            if(documentKey(*sourcePhysical)==documentKey(*from))return fail(ErrorCode::Unsupported,"Self-requiring module needs overlapping content+move support; no partial rename is applied");
            editBytes+=renamed.size()+2;if(editBytes>8*1024*1024)return fail(ErrorCode::OutOfRange,"Lua module edit output exceeds 8 MiB");
            file.edits.push_back({string.range,std::string(1,spelling.front())+renamed+spelling.back()});
        }
        if(!file.edits.empty()) {std::ranges::sort(file.edits,{},[](const auto& e){return e.range.begin;});result.files.push_back(std::move(file));}
    }
    return result;
}
}
