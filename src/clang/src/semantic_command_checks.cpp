// SPDX-License-Identifier: Apache-2.0
// Authored CPP-8 shared admission checks using its actual constructor/adjusters, not policy copies.
#include "semantic_command.hpp"

#include <algorithm>

namespace frost::clang_engine {
int checkSemanticCommand(std::string_view resourceDirectory) {
#ifdef _WIN32
    const std::string root = "C:/frost-semantic-admission";
#else
    const std::string root = "/frost-semantic-admission";
#endif
    const auto request = [&](std::vector<std::string> arguments) {
        llvm::json::Array argv;
        for (const auto& argument : arguments)
            argv.push_back(argument);
        llvm::json::Array rows;
        rows.push_back(
            llvm::json::Object{{"directory", root}, {"file", root + "/unit.cpp"}, {"arguments", std::move(argv)}});
        std::string database;
        llvm::raw_string_ostream stream(database);
        stream << llvm::json::Value(std::move(rows));
        stream.flush();
        return llvm::json::Object{{"projectRoot", root},
                                  {"compilationDatabase", database},
                                  {"commandIndex", 0},
                                  {"configuration", "same-display-label"}};
    };
    const auto fail = [](llvm::StringRef message) {
        llvm::errs() << message << '\n';
        return 1;
    };
    std::string error;
    auto before = semanticCommand(
        request({"clang++", "-DVARIANT=1", "-c", root + "/unit.cpp", "-o", "output.o", "-MMD", "-MF", "output.d"}),
        resourceDirectory, error);
    if (!before)
        return fail(error);
    const auto& arguments = before->command.CommandLine;
    for (const auto forbidden : {"-c", "-o", "output.o", "-MMD", "-MF", "output.d"})
        if (std::ranges::find(arguments, forbidden) != arguments.end())
            return fail("Output/dependency action survived semantic admission");
    if (std::ranges::find(arguments, "-fsyntax-only") == arguments.end())
        return fail("Semantic command is not syntax-only");
    auto variant = semanticCommand(
        request({"clang++", "-DVARIANT=2", "-c", root + "/unit.cpp", "-o", "output.o", "-MMD", "-MF", "output.d"}),
        resourceDirectory, error);
    if (!variant || variant->identity == before->identity)
        return fail("Distinct macro configurations collapsed");
    for (const auto& unsafe : std::vector<std::vector<std::string>>{{"-Xclang", "-load", "-Xclang", "plugin"},
                                                                    {"-Xclang=-load", "-Xclang=plugin"},
                                                                    {"--serialize-diagnostics", "diagnostics.dia"},
                                                                    {"-serialize-diagnostics=diagnostics.dia"},
                                                                    {"-ftime-trace=trace.json"},
                                                                    {"-save-temps"},
                                                                    {"@arguments.rsp"}}) {
        std::vector<std::string> args{"clang++", "-c", root + "/unit.cpp"};
        args.insert(args.end(), unsafe.begin(), unsafe.end());
        error.clear();
        if (semanticCommand(request(std::move(args)), resourceDirectory, error) || error.empty())
            return fail("Unsafe semantic command was admitted");
    }
    llvm::outs() << "semantic command admission checks passed\n";
    return 0;
}
} // namespace frost::clang_engine
