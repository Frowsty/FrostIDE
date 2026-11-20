// SPDX-License-Identifier: Apache-2.0
// Validation replies preserve exact engine messages and refuse unsafe or inconsistent envelopes.
#include "frost/engine/document_tools.hpp"
#include <gtest/gtest.h>
namespace frost::engine {
namespace {
rpc::Json documentReply() {
    return rpc::parseJson(R"({"protocol":"frostengine.document-validation","version":1,"path":"Assets/test.fmat","valid":false,"errors":1,"warnings":0,"issues":[{"severity":"error","code":"deserialize","message":"Engine serializer message","location":{"asset":"Assets/test.fmat","jsonPath":"$.Program","byte":12}}]})").value();
}
TEST(DocumentTools, KeepsActualMessageAndCapturedByte) {
    auto parsed=parseDocumentValidation(documentReply(),"Assets/test.fmat");
    ASSERT_TRUE(parsed);ASSERT_EQ(parsed->issues.size(),1U);EXPECT_FALSE(parsed->valid);
    EXPECT_EQ(parsed->issues[0].message,"Engine serializer message");EXPECT_EQ(parsed->issues[0].byte,12U);
    EXPECT_EQ(parsed->issues[0].location,"$.Program");
}
TEST(DocumentTools, RefusesOtherImageAndDisagreeingCounters) {
    EXPECT_FALSE(parseDocumentValidation(documentReply(),"Assets/other.fmat"));
    auto reply=documentReply();reply.set("errors",0);EXPECT_FALSE(parseDocumentValidation(reply,"Assets/test.fmat"));
    reply=documentReply();reply.set("valid",true);EXPECT_FALSE(parseDocumentValidation(reply,"Assets/test.fmat"));
}
TEST(DocumentTools, ContentLocationsCannotEscapeProject) {
    for(const auto* path:{"../outside.fscene","Assets/../../outside.fscene","C:/outside.fscene","Assets\\outside.fscene","Assets//outside.fscene"}) {
        auto issue=rpc::Json::object().set("severity","Error").set("message","actual error")
            .set("location",rpc::Json::object().set("asset",path));
        auto reply=rpc::Json::object().set("errors",1).set("warnings",0).set("issues",rpc::Json::array({issue}));
        EXPECT_FALSE(parseDocumentValidation(reply))<<path;
    }
}
TEST(DocumentTools, ContentSeverityAndUnknownCoordinatesArePreserved) {
    auto issue=rpc::Json::object().set("severity","Warning").set("message","engine warning")
        .set("location",rpc::Json::object().set("asset","Assets/test.fscene").set("jsonPath","$.Entities"));
    auto reply=rpc::Json::object().set("errors",0).set("warnings",1).set("issues",rpc::Json::array({issue}));
    auto parsed=parseDocumentValidation(reply);ASSERT_TRUE(parsed);EXPECT_TRUE(parsed->valid);
    ASSERT_EQ(parsed->issues.size(),1U);EXPECT_EQ(parsed->issues[0].severity,"warning");EXPECT_EQ(parsed->issues[0].line,0U);
}
TEST(DocumentTools, ActualDataRowAndFieldLocationsRemainVisible) {
    auto issue=rpc::Json::object().set("severity","error").set("code","D016").set("message","actual data error")
        .set("location",rpc::Json::object().set("asset","Assets/test.ftable").set("rowKey","Sword").set("field","Damage"));
    auto reply=rpc::Json::object().set("protocol","frostengine.document-validation").set("version",1)
        .set("path","Assets/test.ftable").set("valid",false).set("errors",1).set("warnings",0).set("issues",rpc::Json::array({issue}));
    auto result=parseDocumentValidation(reply,"Assets/test.ftable");ASSERT_TRUE(result);ASSERT_EQ(result->issues.size(),1U);
    EXPECT_NE(result->issues[0].location.find("row Sword"),std::string::npos);
    EXPECT_NE(result->issues[0].location.find("field Damage"),std::string::npos);
}
}
}
