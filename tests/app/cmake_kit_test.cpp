// SPDX-License-Identifier: Apache-2.0
#include "../../src/app/src/cmake_kit_dialog.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <gtest/gtest.h>

TEST(CMakeKit, PreservesUnknownJsonAndUserBytes) {
    const std::string original = R"({"version":3,"vendor":{"large":18446744073709551615,"decimal":1.0000000000000000001},"configurePresets":[{"name":"old","vendor":{"x":true}}]})";
    auto result = frost::app::appendCMakeKitPreset(original, R"({"name":"new","generator":"Ninja","binaryDir":"${sourceDir}/build-new"})");
    ASSERT_TRUE(result);
    EXPECT_NE(result->find(R"("large":18446744073709551615,"decimal":1.0000000000000000001)"), std::string::npos);
    EXPECT_NE(result->find(R"({"name":"old","vendor":{"x":true}})"), std::string::npos);
    const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(*result));
    EXPECT_EQ(parsed.object().value("configurePresets").toArray().size(), 2);
}

TEST(CMakeKit, CreatesAndAddsArrayWithoutChangingOtherMembers) {
    auto created = frost::app::appendCMakeKitPreset({}, R"({"name":"new"})");
    ASSERT_TRUE(created);
    EXPECT_EQ(QJsonDocument::fromJson(QByteArray::fromStdString(*created)).object().value("version").toInt(), 3);
    auto appended = frost::app::appendCMakeKitPreset("{\n  \"version\": 6, \"vendor\": {\"keep\": null}\n}\n", R"({"name":"new"})");
    ASSERT_TRUE(appended);
    EXPECT_NE(appended->find(R"("vendor": {"keep": null})"), std::string::npos);
}

TEST(CMakeKit, RefusesMalformedAndDuplicatePresets) {
    EXPECT_FALSE(frost::app::appendCMakeKitPreset("bad", R"({"name":"new"})"));
    EXPECT_FALSE(frost::app::appendCMakeKitPreset(R"({"version":3,"configurePresets":{}})", R"({"name":"new"})"));
    EXPECT_FALSE(frost::app::appendCMakeKitPreset(R"({"version":3,"configurePresets":[{"name":"new"}]})", R"({"name":"new"})"));
}
