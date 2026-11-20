// SPDX-License-Identifier: Apache-2.0
// Checks allowlisted Unreal Editor requests and strict validation of project replies.
#include "frost/lang/unreal_partner.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
namespace frost::lang {
namespace {
std::string project(){return io::joinPath(io::kNativePathStyle==io::PathStyle::Windows?"C:/Synthetic":"/Synthetic","Oracle.uproject").value();}
std::string wire(){auto value=project();std::ranges::replace(value,'\\','/');return value;}
Json envelope(std::string_view kind){return Json::object().set("schema",1).set("project",wire()).set("kind",kind);}
}
TEST(UnrealPartner, RequestsAreLiteralAllowlistedAndStrictProjectScoped) {
    const std::string token(64,'a');auto request=unrealPartnerRequest(1,token,project(),"blueprints.loadedCalls");ASSERT_TRUE(request);
    auto parsed=rpc::parseJsonStrict(*request);ASSERT_TRUE(parsed);EXPECT_EQ(parsed->stringAt("op"),"blueprints.loadedCalls");EXPECT_EQ(parsed->stringAt("project"),wire());
    EXPECT_FALSE(unrealPartnerRequest(1,token,project(),"console.exec"));EXPECT_FALSE(unrealPartnerRequest(0,token,project(),"state"));
    EXPECT_FALSE(unrealPartnerRequest(1,"short",project(),"state"));
    auto state=envelope("state").set("playing",true).set("paused",false);
    auto event=parseUnrealPartnerEvent(state.dump(),project());ASSERT_TRUE(event);EXPECT_TRUE(event->playing);EXPECT_FALSE(event->paused);
    EXPECT_FALSE(parseUnrealPartnerEvent(Json(state).set("project","/other.uproject").dump(),project()));
}
TEST(UnrealPartner, ActualLoadedGraphNodesKeepScopeAndRejectAmbiguousOrMalformedRows) {
    auto call=Json::object().set("blueprint","/Game/Original.Blueprint").set("function","/Script/Original.Oracle:Add").set("node",std::string(32,'b'));
    auto reply=envelope("reply").set("id",7).set("status","ok").set("scope","loaded-blueprint-graphs").set("calls",Json::array({call}));
    auto event=parseUnrealPartnerEvent(reply.dump(),project());ASSERT_TRUE(event);ASSERT_EQ(event->calls.size(),1U);EXPECT_EQ(event->scope,"loaded-blueprint-graphs");
    EXPECT_FALSE(parseUnrealPartnerEvent(Json(reply).set("calls",Json::array({call,call})).dump(),project()));
    EXPECT_FALSE(parseUnrealPartnerEvent(Json(reply).set("scope","whole-project-guess").dump(),project()));
    auto bad=Json(call).set("node","not-a-guid");EXPECT_FALSE(parseUnrealPartnerEvent(Json(reply).set("calls",Json::array({bad})).dump(),project()));
}
TEST(UnrealPartner, StrictBoundaryRejectsDuplicatesAndInvalidUtf8WithoutChangingOrdinaryRpcParsing) {
    const std::string duplicate=R"({"schema":1,"schema":2})";
    EXPECT_FALSE(rpc::parseJsonStrict(duplicate));auto normal=parseJson(duplicate);ASSERT_TRUE(normal);EXPECT_EQ(normal->find("schema")->asUnsigned(),2U);
    std::string invalid="{\"message\":\"";invalid.push_back(char(0xff));invalid+="\"}";
    EXPECT_FALSE(rpc::parseJsonStrict(invalid));EXPECT_TRUE(parseJson(invalid));
}
} // namespace frost::lang
