// SPDX-License-Identifier: Apache-2.0
// FER-3 policy decisions, opt-in parsing and zero-dispatch refusal at the caller boundary.
#include "frost/engine/command_policy.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <utility>

namespace frost::engine {
namespace {
using rpc::Json;

Json policyDocument() {
    auto parsed = rpc::parseJson(R"json({
        "schema":"frostengine.schema-export","schemaVersion":1,"engine":{"version":"test"},
        "costTags":[],"bindingKinds":[],"components":[],
        "lua":{"version":"Lua 5.4","scriptsRoot":"Scripts","modulePathPattern":"Scripts/?.lua","entries":[],"aliases":[]},
        "events":[],"commands":[
            {"name":"scene.inspect","category":"Scene","flags":["RequiresProject","RequiresEditor"],
             "idePolicy":{"allowed":true,"states":["Edit","Paused"],"requiresMutationPermission":false,
                          "requiredCapabilities":["session.v2"]}},
            {"name":"scene.change","category":"Scene","flags":["Mutating"],
             "idePolicy":{"allowed":true,"states":["Edit"],"requiresMutationPermission":false,
                          "requiredCapabilities":["session.v2"]}},
            {"name":"script.execute","category":"Script","flags":[],
             "idePolicy":{"allowed":true,"states":["Edit"],"requiresMutationPermission":true,
                          "requiredCapabilities":["session.v2"]}},
            {"name":"server.shutdown","category":"System","flags":["Mutating"]}
        ]
    })json");
    EXPECT_TRUE(parsed);
    return parsed.value_or(Json());
}

SchemaDocument policySchema() {
    auto parsed = parseSchemaDocument(policyDocument());
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().message());
    return parsed.value_or(SchemaDocument{});
}

CommandPolicyContext context() {
    CommandPolicyContext out;
    out.connected = true;
    out.paired = true;
    out.trusted = true;
    out.editorAvailable = true;
    out.selectedProject = "physical-project-identity";
    out.sessionProject = out.selectedProject;
    out.selectedSession = "selected-authenticated-session";
    out.sessionId = out.selectedSession;
    out.state = "Edit";
    out.availableCommands = {"scene.inspect", "scene.change", "script.execute", "server.shutdown"};
    out.capabilities = {"session.v2"};
    return out;
}

TEST(CommandPolicy, ExplicitReadOptInCanSendWhileUnknownAndUnoptedCommandsNeverSend) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    const auto observed = context();
    std::vector<std::string> sent;
    const auto dispatch = [&](std::string_view command) -> Result<void> {
        auto decision = policy.admit(command, Json::object(), observed);
        if (!decision)
            return std::unexpected(std::move(decision.error()));
        sent.emplace_back(command);
        return {};
    };
    EXPECT_TRUE(dispatch("scene.inspect"));
    EXPECT_FALSE(dispatch("unknown.command"));
    EXPECT_FALSE(dispatch("server.shutdown"));
    EXPECT_EQ(sent, std::vector<std::string>{"scene.inspect"});
}

TEST(CommandPolicy, DefaultContextAndEachMissingIdentityOrPermissionRefuse) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object(), CommandPolicyContext{}));
    const std::vector<std::function<void(CommandPolicyContext&)>> revoke{
        [](auto& c) { c.connected = false; },
        [](auto& c) { c.paired = false; },
        [](auto& c) { c.trusted = false; },
        [](auto& c) { c.selectedProject.clear(); },
        [](auto& c) { c.sessionProject = "other-project"; },
        [](auto& c) { c.selectedSession.clear(); },
        [](auto& c) { c.sessionId = "restarted-session"; },
        [](auto& c) { c.editorAvailable = false; },
        [](auto& c) { c.availableCommands.clear(); },
        [](auto& c) { c.capabilities.clear(); },
        [](auto& c) { c.state = "Play"; },
        [](auto& c) { c.state = "unknown-future-state"; }};
    for (std::size_t i = 0; i < revoke.size(); ++i) {
        SCOPED_TRACE(i);
        auto observed = context();
        revoke[i](observed);
        EXPECT_FALSE(policy.admit("scene.inspect", Json::object(), observed));
    }
}

TEST(CommandPolicy, RechecksTrustAndExactSessionAfterPriorAdmission) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    auto observed = context();
    EXPECT_TRUE(policy.admit("scene.inspect", Json::object(), observed));
    observed.trusted = false;
    auto denied = policy.admit("scene.inspect", Json::object(), observed);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::Untrusted);
    observed.trusted = true;
    observed.sessionId = "replacement-session";
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object(), observed));
}

TEST(CommandPolicy, SchemaRemainsBoundToTheExportingSessionAndPhysicalProject) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    auto observed = context();
    observed.selectedSession = "new-session";
    observed.sessionId = observed.selectedSession;
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object(), observed));
    observed = context();
    observed.selectedProject = "another-project";
    observed.sessionProject = observed.selectedProject;
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object(), observed));
    EXPECT_FALSE(CommandPolicy(policySchema(), "", "").admit("scene.inspect", Json::object(), context()));
}

TEST(CommandPolicy, MutationFlagAndSeparateExecutionPermissionBothRequireExplicitConsent) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    auto observed = context();
    EXPECT_FALSE(policy.admit("scene.change", Json::object(), observed));
    EXPECT_FALSE(policy.admit("script.execute", Json::object(), observed));
    observed.mutationAllowed = true;
    EXPECT_TRUE(policy.admit("scene.change", Json::object(), observed));
    EXPECT_TRUE(policy.admit("script.execute", Json::object(), observed));
    observed.trusted = false;
    EXPECT_FALSE(policy.admit("scene.change", Json::object(), observed));
}

TEST(CommandPolicy, CategoryOrFamiliarCommandNameNeverGrantsPermission) {
    auto schema = policySchema();
    schema.commands.front().name = "play.start";
    schema.commands.front().category = "ReadOnly";
    schema.commands.front().idePolicy.reset();
    auto observed = context();
    observed.availableCommands.push_back("play.start");
    observed.mutationAllowed = true;
    EXPECT_FALSE(CommandPolicy(std::move(schema), context().selectedProject, context().selectedSession)
                     .admit("play.start", Json::object(), observed));
}

TEST(CommandPolicy, UnsupportedFlagsAndAmbiguousRegistrationsFailClosed) {
    auto schema = policySchema();
    schema.commands.front().flags.push_back("FuturePrivilege");
    EXPECT_FALSE(CommandPolicy(schema, context().selectedProject, context().selectedSession)
                     .admit("scene.inspect", Json::object(), context()));
    schema.commands.front().flags.pop_back();
    schema.commands.push_back(schema.commands.front());
    EXPECT_FALSE(CommandPolicy(schema, context().selectedProject, context().selectedSession)
                     .admit("scene.inspect", Json::object(), context()));
}

TEST(CommandPolicy, RejectsCredentialInjectionAndInvalidParameterShape) {
    CommandPolicy policy(policySchema(), context().selectedProject, context().selectedSession);
    EXPECT_FALSE(policy.admit("scene.inspect", Json::array(), context()));
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object().set("token", "caller-token"), context()));
    EXPECT_FALSE(policy.admit("scene.inspect", Json::object().set("secret", "caller-secret"), context()));
}

TEST(CommandPolicySchema, MissingMetadataStaysAbsentAndOptInFieldsReadExactly) {
    auto parsed = parseSchemaDocument(policyDocument());
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->commands.size(), 4u);
    ASSERT_TRUE(parsed->commands.front().idePolicy);
    EXPECT_TRUE(parsed->commands.front().idePolicy->allowed);
    EXPECT_FALSE(parsed->commands.front().idePolicy->requiresMutationPermission);
    EXPECT_EQ(parsed->commands.front().idePolicy->states, (std::vector<std::string>{"Edit", "Paused"}));
    EXPECT_EQ(parsed->commands.front().idePolicy->requiredCapabilities, std::vector<std::string>{"session.v2"});
    EXPECT_FALSE(parsed->commands.back().idePolicy);
}

TEST(CommandPolicySchema, MalformedMetadataNamesTheFaultAndNeverGrantsPermission) {
    const std::vector<std::pair<std::string, Json>> invalid{{"allowed", Json("true")},
                                                            {"states", Json::array()},
                                                            {"states", Json::array().push("playing")},
                                                            {"requiresMutationPermission", Json("false")},
                                                            {"requiredCapabilities", Json::array().push(1)},
                                                            {"requiredCapabilities", Json::array().push("")}};
    for (const auto& [field, value] : invalid) {
        auto document = policyDocument();
        auto command = document.find("commands")->items().front();
        command.find("idePolicy")->set(field, value);
        document.set("commands", Json::array().push(std::move(command)));
        auto parsed = parseSchemaDocument(document);
        ASSERT_FALSE(parsed) << field;
        EXPECT_EQ(parsed.error().code(), ErrorCode::Corrupt);
        EXPECT_NE(parsed.error().message().find("commands[0].idePolicy." + field), std::string::npos);
    }
    auto document = policyDocument();
    auto command = document.find("commands")->items().front();
    command.set("idePolicy", true);
    document.set("commands", Json::array().push(std::move(command)));
    auto parsed = parseSchemaDocument(document);
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().message().find("commands[0].idePolicy"), std::string::npos);
}
} // namespace
} // namespace frost::engine
