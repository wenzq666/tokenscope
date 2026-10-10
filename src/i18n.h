#pragma once
// User interface language. Chinese literals in the source are the message keys; English text
// comes from i18n_en.inc. A key without an English entry falls back to the Chinese original
// (scripts/check-translations.ps1 keeps the table complete).
#include <string>
#include <string_view>
#include <vector>

namespace tokenscope {

enum class Language { Chinese, English };
// Setting stored in settings.ini: follow Windows, or a fixed language.
enum class LanguagePreference { System, Chinese, English };

Language CurrentLanguage();
void SetLanguage(Language language);
Language SystemLanguage(); // Chinese for any zh-* Windows display language, English otherwise.
Language ResolveLanguage(LanguagePreference preference);

// Translate a whole Chinese message. Returned pointers stay valid for the process lifetime.
const wchar_t* Tr(const wchar_t* chinese);
std::wstring Tr(const std::wstring& chinese);
const char* Tr8(const char* chineseUtf8);
std::string Tr8(const std::string& chineseUtf8);

// Replace {0}, {1}, … after translating the pattern.
std::wstring FormatText(const std::wstring& pattern, const std::vector<std::wstring>& arguments);
std::string FormatText8(const std::string& pattern, const std::vector<std::string>& arguments);

inline std::wstring ToText(const std::wstring& value) {
    return value;
}
inline std::wstring ToText(const wchar_t* value) {
    return value;
}
inline std::wstring ToText(long long value) {
    return std::to_wstring(value);
}
inline std::wstring ToText(long value) {
    return std::to_wstring(value);
}
inline std::wstring ToText(int value) {
    return std::to_wstring(value);
}
inline std::wstring ToText(unsigned long long value) {
    return std::to_wstring(value);
}
inline std::wstring ToText(unsigned long value) {
    return std::to_wstring(value);
}
inline std::wstring ToText(unsigned value) {
    return std::to_wstring(value);
}
inline std::string ToText8(const std::string& value) {
    return value;
}
inline std::string ToText8(const char* value) {
    return value;
}
inline std::string ToText8(long long value) {
    return std::to_string(value);
}
inline std::string ToText8(long value) {
    return std::to_string(value);
}
inline std::string ToText8(int value) {
    return std::to_string(value);
}
inline std::string ToText8(unsigned long long value) {
    return std::to_string(value);
}
inline std::string ToText8(unsigned long value) {
    return std::to_string(value);
}
inline std::string ToText8(unsigned value) {
    return std::to_string(value);
}

// TrF(L"已更新 · 新增 {0} 条用量记录", count): translate, then substitute.
template <class... Arguments> std::wstring TrF(const wchar_t* pattern, const Arguments&... arguments) {
    return FormatText(Tr(pattern), {ToText(arguments)...});
}
template <class... Arguments> std::string TrF8(const char* pattern, const Arguments&... arguments) {
    return FormatText8(Tr8(pattern), {ToText8(arguments)...});
}

// For the translation completeness check and tests.
size_t TranslationCount();
bool HasTranslation(std::wstring_view chinese);

} // namespace tokenscope
