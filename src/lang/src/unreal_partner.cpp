// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unreal_partner.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include <algorithm>
#include <array>
#include <set>
namespace frost::lang {
namespace {
bool hex(char c){return(c>='a'&&c<='f')||(c>='A'&&c<='F')||(c>='0'&&c<='9');}
bool text(std::string_view value,std::size_t max){return value.size()<=max&&std::ranges::none_of(value,[](unsigned char c){return c<32;});}
std::string wireProject(std::string_view project){std::string path(project);std::ranges::replace(path,'\\','/');return path;}
bool objectPath(std::string_view value){return value.starts_with('/')&&text(value,4096);}
bool guid(std::string_view value){return value.size()==32&&std::ranges::all_of(value,hex);}
}
Result<std::string> unrealPartnerRequest(std::uint64_t id,std::string_view token,std::string_view projectFile,std::string_view operation) {
    if(!id||id>9007199254740991ULL||token.size()!=64||!std::ranges::all_of(token,hex)||
        !io::isAbsolutePath(projectFile)||!projectFile.ends_with(".uproject")||!io::isValidUtf8(projectFile)||!text(projectFile,32768))return fail(ErrorCode::InvalidArgument,"invalid Unreal partner request identity");
    constexpr std::array<std::string_view,6> operations{"state","blueprints.loadedCalls","pie.play","pie.stop","pie.pause","pie.resume"};
    if(std::ranges::find(operations,operation)==operations.end())return fail(ErrorCode::Unsupported,"unsupported Unreal partner operation");
    auto request=Json::object().set("schema",1).set("id",id).set("token",token).set("project",wireProject(projectFile)).set("op",operation).dump()+"\n";
    if(request.size()>65536)return fail(ErrorCode::OutOfRange,"Unreal partner request exceeds frame limit");
    return request;
}
Result<UnrealPartnerEvent> parseUnrealPartnerEvent(std::string_view frame,std::string_view projectFile) {
    if(frame.size()>65536||!io::isAbsolutePath(projectFile))return fail(ErrorCode::OutOfRange,"Unreal partner frame exceeds bounds or project is invalid");
    auto parsed=rpc::parseJsonStrict(frame);if(!parsed)return std::unexpected(parsed.error());
    if(!parsed->isObject()||!parsed->find("schema")||parsed->find("schema")->asUnsigned()!=1||parsed->stringAt("project")!=wireProject(projectFile))
        return fail(ErrorCode::Corrupt,"Unreal partner schema/project mismatch");
    UnrealPartnerEvent result;result.kind=std::string(parsed->stringAt("kind"));
    if(result.kind=="state"){
        const auto playing=parsed->boolAt("playing"),paused=parsed->boolAt("paused");
        if(!playing||!paused||(*paused&&!*playing))return fail(ErrorCode::Corrupt,"invalid Unreal partner state");
        result.playing=*playing;result.paused=*paused;
    }else if(result.kind=="log"){
        const auto* message=parsed->find("message");if(!message||!message->asString()||message->asString()->size()>32768)return fail(ErrorCode::Corrupt,"invalid Unreal partner log");
        result.message=*message->asString();
    }else if(result.kind=="droppedLogs"){
        const auto* count=parsed->find("count");if(!count||!count->asUnsigned())return fail(ErrorCode::Corrupt,"invalid Unreal dropped-log count");result.droppedLogs=*count->asUnsigned();
    }else if(result.kind=="reply"){
        const auto* id=parsed->find("id");if(!id||!id->asUnsigned()||!*id->asUnsigned()||*id->asUnsigned()>9007199254740991ULL)return fail(ErrorCode::Corrupt,"invalid Unreal reply id");
        result.id=*id->asUnsigned();result.status=std::string(parsed->stringAt("status"));
        if(result.status!="ok"&&result.status!="requested"&&result.status!="unsupported"&&result.status!="unavailable"&&result.status!="limit-exceeded")return fail(ErrorCode::Corrupt,"invalid Unreal reply status");
        if(const auto* calls=parsed->find("calls")){
            result.scope=std::string(parsed->stringAt("scope"));
            if(result.status!="ok"||result.scope!="loaded-blueprint-graphs"||!calls->isArray()||calls->items().size()>10000)return fail(ErrorCode::Corrupt,"invalid Unreal Blueprint call scope");
            std::set<std::string> identities;
            for(const auto& call:calls->items()){
                const auto blueprint=call.stringAt("blueprint"),function=call.stringAt("function"),node=call.stringAt("node");
                if(!call.isObject()||!objectPath(blueprint)||!objectPath(function)||!guid(node)||!identities.insert(std::string(blueprint)+"\n"+std::string(node)).second)return fail(ErrorCode::Corrupt,"invalid/duplicate Unreal Blueprint call node");
                result.calls.push_back({std::string(blueprint),std::string(function),std::string(node)});
            }
        }
    }else return fail(ErrorCode::Unsupported,"unknown Unreal partner event");
    return result;
}
} // namespace frost::lang
