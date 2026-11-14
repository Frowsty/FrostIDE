// SPDX-License-Identifier: Apache-2.0
// Lossless replacement-group DTOs: DiagnosticsYaml's normalized map would swallow overlap errors.
#include "tidy_fixes_decode.hpp"

#include "clang/Tooling/ReplacementsYaml.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <vector>
namespace frost::clang_engine::tidy_yaml {
struct Message {
    std::string text, path;
    std::uint64_t offset = 0;
    std::vector<clang::tooling::Replacement> replacements;
};
struct Diagnostic {
    std::string name, level = "Warning", directory;
    Message message;
    std::vector<Message> notes;
};
struct Document {
    std::string source;
    std::vector<Diagnostic> diagnostics;
};
} // namespace frost::clang_engine::tidy_yaml
LLVM_YAML_IS_SEQUENCE_VECTOR(frost::clang_engine::tidy_yaml::Message)
LLVM_YAML_IS_SEQUENCE_VECTOR(frost::clang_engine::tidy_yaml::Diagnostic)
namespace llvm::yaml {
template<>
struct MappingTraits<frost::clang_engine::tidy_yaml::Message> {
    static void mapping(IO& io, frost::clang_engine::tidy_yaml::Message& value) {
        io.mapRequired("Message", value.text);
        io.mapOptional("FilePath", value.path);
        io.mapOptional("FileOffset", value.offset);
        io.mapRequired("Replacements", value.replacements);
    }
};
template<>
struct MappingTraits<frost::clang_engine::tidy_yaml::Diagnostic> {
    static void mapping(IO& io, frost::clang_engine::tidy_yaml::Diagnostic& value) {
        io.mapRequired("DiagnosticName", value.name);
        io.mapRequired("DiagnosticMessage", value.message);
        io.mapOptional("Notes", value.notes);
        io.mapOptional("Level", value.level);
        io.mapOptional("BuildDirectory", value.directory);
    }
};
template<>
struct MappingTraits<frost::clang_engine::tidy_yaml::Document> {
    static void mapping(IO& io, frost::clang_engine::tidy_yaml::Document& value) {
        io.mapRequired("MainSourceFile", value.source);
        io.mapRequired("Diagnostics", value.diagnostics);
    }
};
} // namespace llvm::yaml
namespace frost::clang_engine {
int runTidyFixesDecode() {
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    std::string bytes;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (bytes.size() > limit - count) {
            llvm::errs() << "clang-tidy export exceeds 64 MiB\n";
            return 1;
        }
        bytes.append(chunk, count);
    }
    tidy_yaml::Document document;
    llvm::yaml::Input input(bytes);
    input.setAllowUnknownKeys(true); // Ranges/diagnostic metadata do not change the complete replacement vectors.
    input >> document;
    if (input.error() || document.diagnostics.size() > 8192) {
        llvm::errs() << "Invalid or oversized actual clang-tidy export\n";
        return 1;
    }
    std::size_t replacements = 0, outputCharge = 0;
    bool valid = true;
    const auto charge = [&](std::size_t amount) {
        if (amount > (limit - outputCharge) / 6) {
            valid = false;
            return;
        }
        outputCharge += amount * 6;
    };
    const auto message = [&](const tidy_yaml::Message& value) {
        llvm::json::Array edits;
        charge(value.text.size() + value.path.size() + 256);
        for (const auto& replacement : value.replacements) {
            if (++replacements > 100000)
                valid = false;
            charge(replacement.getFilePath().size() + replacement.getReplacementText().size() + 160);
            edits.push_back(llvm::json::Object{{"path", replacement.getFilePath()},
                                               {"offset", replacement.getOffset()},
                                               {"length", replacement.getLength()},
                                               {"text", replacement.getReplacementText()}});
        }
        return llvm::json::Object{{"message", value.text},
                                  {"path", value.path},
                                  {"offset", value.offset},
                                  {"replacements", std::move(edits)}};
    };
    llvm::json::Array diagnostics;
    for (const auto& diagnostic : document.diagnostics) {
        charge(diagnostic.name.size() + diagnostic.directory.size() + 256);
        llvm::json::Array notes;
        for (const auto& note : diagnostic.notes)
            notes.push_back(message(note));
        diagnostics.push_back(llvm::json::Object{{"name", diagnostic.name},
                                                 {"level", diagnostic.level},
                                                 {"directory", diagnostic.directory},
                                                 {"primary", message(diagnostic.message)},
                                                 {"notes", std::move(notes)}});
    }
    if (!valid) {
        llvm::errs() << "clang-tidy replacement output exceeds budget\n";
        return 1;
    }
    llvm::outs() << llvm::json::Value(llvm::json::Object{{"kind", "clang-tidy-fixes"},
                                                         {"schemaVersion", 1},
                                                         {"source", document.source},
                                                         {"diagnostics", std::move(diagnostics)}})
                 << '\n';
    return 0;
}
} // namespace frost::clang_engine
