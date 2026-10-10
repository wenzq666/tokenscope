#include "i18n.h"
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>

namespace tokenscope {
namespace {

struct Entry {
    const wchar_t* chinese;
    const wchar_t* english;
};
constexpr Entry kEnglish[] = {
#include "i18n_en.inc"
};

std::atomic<Language> current{Language::Chinese};

const std::unordered_map<std::wstring_view, const wchar_t*>& WideTable() {
    static const auto table = [] {
        std::unordered_map<std::wstring_view, const wchar_t*> result;
        result.reserve(std::size(kEnglish));
        for (const auto& entry : kEnglish) result.emplace(entry.chinese, entry.english);
        return result;
    }();
    return table;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
// UTF-8 view of the same table, keyed by the UTF-8 Chinese text. Built once, then read-only.
const std::map<std::string, std::string, std::less<>>& NarrowTable() {
    static const auto table = [] {
        std::map<std::string, std::string, std::less<>> result;
        for (const auto& entry : kEnglish) result.emplace(Narrow(entry.chinese), Narrow(entry.english));
        return result;
    }();
    return table;
}

template <class String> String Substitute(const String& pattern, const std::vector<String>& arguments) {
    using Char = typename String::value_type;
    String result;
    result.reserve(pattern.size());
    for (size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == Char('{')) {
            const size_t close = pattern.find(Char('}'), index);
            if (close != String::npos && close > index + 1 && close - index <= 3) {
                size_t number = 0;
                bool digits = true;
                for (size_t digit = index + 1; digit < close; ++digit) {
                    if (pattern[digit] < Char('0') || pattern[digit] > Char('9')) digits = false;
                    number = number * 10 + static_cast<size_t>(pattern[digit] - Char('0'));
                }
                if (digits && number < arguments.size()) {
                    result += arguments[number];
                    index = close;
                    continue;
                }
            }
        }
        result += pattern[index];
    }
    return result;
}

} // namespace

Language CurrentLanguage() {
    return current.load();
}
void SetLanguage(Language language) {
    current.store(language);
}
Language SystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::Chinese : Language::English;
}
Language ResolveLanguage(LanguagePreference preference) {
    switch (preference) {
    case LanguagePreference::Chinese:
        return Language::Chinese;
    case LanguagePreference::English:
        return Language::English;
    default:
        return SystemLanguage();
    }
}

const wchar_t* Tr(const wchar_t* chinese) {
    if (!chinese || current.load() == Language::Chinese) return chinese;
    const auto& table = WideTable();
    const auto found = table.find(chinese);
    return found == table.end() ? chinese : found->second;
}
std::wstring Tr(const std::wstring& chinese) {
    if (current.load() == Language::Chinese) return chinese;
    const auto& table = WideTable();
    const auto found = table.find(chinese);
    return found == table.end() ? chinese : std::wstring(found->second);
}
const char* Tr8(const char* chineseUtf8) {
    if (!chineseUtf8 || current.load() == Language::Chinese) return chineseUtf8;
    const auto& table = NarrowTable();
    const auto found = table.find(std::string_view(chineseUtf8));
    return found == table.end() ? chineseUtf8 : found->second.c_str();
}
std::string Tr8(const std::string& chineseUtf8) {
    return Tr8(chineseUtf8.c_str());
}

std::wstring FormatText(const std::wstring& pattern, const std::vector<std::wstring>& arguments) {
    return Substitute(pattern, arguments);
}
std::string FormatText8(const std::string& pattern, const std::vector<std::string>& arguments) {
    return Substitute(pattern, arguments);
}

size_t TranslationCount() {
    return std::size(kEnglish);
}
bool HasTranslation(std::wstring_view chinese) {
    return WideTable().count(chinese) != 0;
}

} // namespace tokenscope
