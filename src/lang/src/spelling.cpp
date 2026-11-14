// SPDX-License-Identifier: Apache-2.0
// Installed Windows dictionaries supply all results; missing providers remain unavailable.
#include "frost/lang/spelling.hpp"
#include "frost/io/utf.hpp"
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <spellcheck.h>
#include <cwchar>
namespace frost::lang {
namespace {
template<class T> struct Com {
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
    Com() = default; Com(const Com&) = delete; Com& operator=(const Com&) = delete;
    T* operator->() const { return value; }
};
struct Apartment {
    HRESULT status = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(status) || status == RPC_E_CHANGED_MODE; }
};
Result<std::string> fromWide(const wchar_t* value) {
    if (!value) return fail(ErrorCode::Corrupt,"Spelling provider returned no string");
    const auto length = wcsnlen_s(value,4097);
    if (length > 4096) return fail(ErrorCode::OutOfRange,"Spelling provider string limit exceeded");
    return io::fromWide(std::wstring_view(value,length));
}
Result<void> factory(Com<ISpellCheckerFactory>& out) {
    const auto result = CoCreateInstance(__uuidof(SpellCheckerFactory),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&out.value));
    if (FAILED(result)) return fail(ErrorCode::NotFound,"Installed Windows spelling dictionaries are unavailable");
    return {};
}
Result<void> checker(ISpellCheckerFactory& source, std::string_view language, Com<ISpellChecker>& out) {
    if (language.empty() || language.size()>128 || language.find('\0') != language.npos)
        return fail(ErrorCode::InvalidArgument,"Choose an installed spelling language");
    auto wide = io::toWide(language); if (!wide) return std::unexpected(wide.error());
    BOOL supported = FALSE;
    if (FAILED(source.IsSupported(wide->c_str(),&supported)) || !supported)
        return fail(ErrorCode::Unsupported,"This spelling language is not installed");
    if (FAILED(source.CreateSpellChecker(wide->c_str(),&out.value))) return fail(ErrorCode::NotFound,"Spelling provider could not open the selected language");
    return {};
}
}
Result<std::vector<std::string>> spellingLanguages() {
    Apartment apartment; if (!apartment.usable()) return fail(ErrorCode::IoError,"COM spelling initialization failed");
    Com<ISpellCheckerFactory> source; if (auto opened=factory(source); !opened) return std::unexpected(opened.error());
    Com<IEnumString> languages;
    if (FAILED(source->get_SupportedLanguages(&languages.value))) return fail(ErrorCode::IoError,"Cannot enumerate installed spelling dictionaries");
    std::vector<std::string> result;
    for (;;) {
        wchar_t* value = nullptr; ULONG count = 0;
        const auto next = languages->Next(1,&value,&count);
        if (next == S_FALSE) break;
        if (FAILED(next) || count != 1) return fail(ErrorCode::IoError,"Spelling dictionary enumeration failed");
        auto converted = fromWide(value); CoTaskMemFree(value);
        if (!converted) return std::unexpected(converted.error());
        if (result.size() == 256) return fail(ErrorCode::OutOfRange,"Too many installed spelling dictionaries");
        result.push_back(std::move(*converted));
    }
    std::ranges::sort(result); return result;
}
Result<std::vector<SpellingIssue>> checkSpelling(std::string_view text, std::string_view language) {
    if (text.size()>65536 || text.find('\0') != text.npos) return fail(ErrorCode::OutOfRange,"Select at most64KiB of prose without NULs");
    auto wide = io::toWide(text); if (!wide) return std::unexpected(wide.error());
    Apartment apartment; if (!apartment.usable()) return fail(ErrorCode::IoError,"COM spelling initialization failed");
    Com<ISpellCheckerFactory> source; if (auto opened=factory(source); !opened) return std::unexpected(opened.error());
    Com<ISpellChecker> engine; if (auto opened=checker(*source.value,language,engine); !opened) return std::unexpected(opened.error());
    Com<IEnumSpellingError> errors;
    if (FAILED(engine->Check(wide->c_str(),&errors.value))) return fail(ErrorCode::IoError,"Installed dictionary check failed");
    std::vector<SpellingIssue> result;
    for (;;) {
        Com<ISpellingError> error;
        const auto next = errors->Next(&error.value);
        if (next == S_FALSE) break;
        if (FAILED(next) || !error.value) return fail(ErrorCode::IoError,"Spelling error enumeration failed");
        if (result.size() == 256) return fail(ErrorCode::OutOfRange,"Select less prose (more than256 spelling issues)");
        ULONG begin=0,length=0;
        if (FAILED(error->get_StartIndex(&begin)) || FAILED(error->get_Length(&length)) || begin>wide->size() || length>wide->size()-begin || !length)
            return fail(ErrorCode::Corrupt,"Spelling provider returned an invalid text range");
        auto prefix=io::fromWide(std::wstring_view(*wide).substr(0,begin));
        auto word=io::fromWide(std::wstring_view(*wide).substr(begin,length));
        if (!prefix || !word) return fail(ErrorCode::Corrupt,"Spelling provider split a Unicode character");
        SpellingIssue issue{static_cast<std::uint32_t>(prefix->size()),static_cast<std::uint32_t>(prefix->size()+word->size()),*word,{}};
        CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
        if (FAILED(error->get_CorrectiveAction(&action))) return fail(ErrorCode::IoError,"Spelling action unavailable");
        if (action == CORRECTIVE_ACTION_NONE) continue;
        if (action == CORRECTIVE_ACTION_REPLACE) {
            wchar_t* replacement=nullptr;
            if (FAILED(error->get_Replacement(&replacement))) return fail(ErrorCode::IoError,"Spelling replacement unavailable");
            auto converted=fromWide(replacement); CoTaskMemFree(replacement);
            if (!converted) return std::unexpected(converted.error()); issue.suggestions.push_back(std::move(*converted));
        } else if (action == CORRECTIVE_ACTION_DELETE) issue.suggestions.emplace_back();
        else {
            Com<IEnumString> suggestions;
            const std::wstring selected(wide->data()+begin,length);
            if (FAILED(engine->Suggest(selected.c_str(),&suggestions.value))) return fail(ErrorCode::IoError,"Spelling suggestions unavailable");
            for (std::size_t index=0;index<16;++index) {
                wchar_t* value=nullptr; ULONG count=0;
                const auto suggested=suggestions->Next(1,&value,&count);
                if (suggested==S_FALSE) break;
                if (FAILED(suggested) || count!=1) return fail(ErrorCode::IoError,"Spelling suggestions failed");
                auto converted=fromWide(value); CoTaskMemFree(value);
                if (!converted) return std::unexpected(converted.error()); issue.suggestions.push_back(std::move(*converted));
            }
        }
        result.push_back(std::move(issue));
    }
    std::ranges::sort(result,{},&SpellingIssue::begin);
    std::uint32_t previous=0;
    for (const auto& issue:result) {
        if (issue.begin<previous) return fail(ErrorCode::Corrupt,"Spelling provider returned overlapping issues");
        previous=issue.end;
    }
    return result;
}
Result<void> addSpellingWord(std::string_view word, std::string_view language) {
    if (word.empty() || word.size()>256 || word.find_first_of("\0\r\n\t ",0,5) != word.npos) return fail(ErrorCode::InvalidArgument,"Choose one bounded dictionary word");
    auto wide=io::toWide(word); if (!wide) return std::unexpected(wide.error());
    Apartment apartment; if (!apartment.usable()) return fail(ErrorCode::IoError,"COM spelling initialization failed");
    Com<ISpellCheckerFactory> source; if (auto opened=factory(source); !opened) return std::unexpected(opened.error());
    Com<ISpellChecker> engine; if (auto opened=checker(*source.value,language,engine); !opened) return std::unexpected(opened.error());
    if (FAILED(engine->Add(wide->c_str()))) return fail(ErrorCode::PermissionDenied,"Installed dictionary rejected this word");
    return {};
}
} // namespace frost::lang
#else
namespace frost::lang {
Result<std::vector<std::string>> spellingLanguages() { return fail(ErrorCode::Unsupported,"No installed spelling provider is configured on this platform"); }
Result<std::vector<SpellingIssue>> checkSpelling(std::string_view, std::string_view) { return fail(ErrorCode::Unsupported,"No installed spelling provider is configured on this platform"); }
Result<void> addSpellingWord(std::string_view, std::string_view) { return fail(ErrorCode::Unsupported,"No installed spelling provider is configured on this platform"); }
}
#endif
