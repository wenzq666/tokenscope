#include "shell/shell.h"

namespace tokenscope::shell {

std::wstring GetDataDirectory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) {
        std::wstring result = std::wstring(value) + L"\\TokenScope";
        CoTaskMemFree(value);
        return result;
    }
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"TokenScope";
}

std::wstring IniString(const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"TokenScope", key, fallback, buffer, 32768, settingsPath.c_str());
    return buffer;
}
bool IniWrite(const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(L"TokenScope", key, value.c_str(), settingsPath.c_str()) != FALSE;
}
bool EnsureUnicodeSettings() {
    // Profile APIs otherwise use the system ANSI code page when saving paths.
    std::error_code error;
    const auto exists = fs::exists(settingsPath, error);
    if (error) return false;
    std::string bytes;
    if (exists) {
        const auto size = fs::file_size(settingsPath, error);
        if (error || size > 4 * 1024 * 1024) return false;
        std::ifstream input(fs::path(settingsPath), std::ios::binary);
        bytes.resize(static_cast<size_t>(size));
        if (size && !input.read(bytes.data(), static_cast<std::streamsize>(size))) return false;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE)
        return true;
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        if (bytes.size() % 2) return false;
        for (size_t i = 2; i < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                                static_cast<unsigned char>(bytes[i + 1])));
    } else if (!bytes.empty()) {
        UINT page = CP_ACP;
        size_t offset = 0;
        if (bytes.starts_with("\xEF\xBB\xBF")) {
            page = CP_UTF8;
            offset = 3;
        }
        const auto length = static_cast<int>(bytes.size() - offset);
        const auto count = MultiByteToWideChar(page, page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data() + offset,
                                               length, nullptr, 0);
        if (length && !count) return false;
        text.resize(count);
        if (count)
            MultiByteToWideChar(page, page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data() + offset, length,
                                text.data(), count);
    }
    const auto pending =
        settingsPath + L".unicode-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    auto file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool okay = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    const auto length = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (okay && length) okay = WriteFile(file, text.data(), length, &written, nullptr) && written == length;
    if (okay) okay = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (okay)
        okay = MoveFileExW(pending.c_str(), settingsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
               FALSE;
    if (!okay) DeleteFileW(pending.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, settingsPath.c_str());
    return okay;
}
void LoadPreferences() {
    if (!EnsureUnicodeSettings()) state.notice = Tr(L"设置文件无法升级为 Unicode；请检查数据目录写入权限。");
    int range = GetPrivateProfileIntW(L"TokenScope", L"Range", 0, settingsPath.c_str());
    state.prefs.range = static_cast<Range>(std::clamp(range, 0, 3));
    state.prefs.cny = GetPrivateProfileIntW(L"TokenScope", L"CNY", 0, settingsPath.c_str()) != 0;
    state.prefs.autoRefresh = GetPrivateProfileIntW(L"TokenScope", L"AutoRefresh", 1, settingsPath.c_str()) != 0;
    state.prefs.trendColor =
        GetPrivateProfileIntW(L"TokenScope", L"TrendColor", RGB(53, 101, 223), settingsPath.c_str()) & 0xFFFFFF;
    state.prefs.heatColor =
        GetPrivateProfileIntW(L"TokenScope", L"HeatColor", RGB(105, 157, 47), settingsPath.c_str()) & 0xFFFFFF;
    wchar_t* end = nullptr;
    const auto manualRate = IniString(L"USDCNY", L"0");
    state.prefs.usdToCny = 0;
    double rate = wcstod(manualRate.c_str(), &end);
    while (end && iswspace(*end)) ++end;
    if (end != manualRate.c_str() && end && !*end && std::isfinite(rate) && rate > 0 && rate < 10000)
        state.prefs.usdToCny = rate;
    state.prefs.theme =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"TokenScope", L"Theme", 0, settingsPath.c_str())), 0, 2);
    state.prefs.language =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"TokenScope", L"Language", 0, settingsPath.c_str())), 0, 2);
    SetLanguage(ResolveLanguage(static_cast<LanguagePreference>(state.prefs.language)));
    auto amount = [](const std::wstring& text) {
        wchar_t* stop = nullptr;
        const double value = wcstod(text.c_str(), &stop);
        return stop != text.c_str() && std::isfinite(value) && value > 0 && value < 1e9 ? value : 0.0;
    };
    state.prefs.dailyBudget = amount(IniString(L"DailyBudget", L"0"));
    state.prefs.monthlyBudget = amount(IniString(L"MonthlyBudget", L"0"));
    state.prefs.budgetCny = GetPrivateProfileIntW(L"TokenScope", L"BudgetCNY", 0, settingsPath.c_str()) != 0;
    LoadExchangeCache();
    importCodex = IniString(L"ImportCodex");
    importClaude = IniString(L"ImportClaude");
    configuredImports.clear();
    for (const auto& source : CatalogSources()) {
        const auto key = L"Import." + Utf8ToWide(source.id);
        auto path = IniString(key.c_str());
        if (!path.empty()) configuredImports.emplace_back(source.id, std::move(path));
    }
    for (const auto& legacy :
         std::vector<std::pair<std::string, std::wstring>>{{"codex", importCodex}, {"claude", importClaude}}) {
        if (!legacy.second.empty() && std::none_of(configuredImports.begin(), configuredImports.end(),
                                                   [&](const auto& entry) { return entry.first == legacy.first; }))
            configuredImports.push_back(legacy);
    }
}
bool SavePreferences() {
    if (!EnsureUnicodeSettings()) {
        state.notice = Tr(L"设置保存失败：无法写入 Unicode 设置文件。");
        return false;
    }
    bool okay = true;
    okay = IniWrite(L"Range", std::to_wstring(static_cast<int>(state.prefs.range))) && okay;
    okay = IniWrite(L"CNY", state.prefs.cny ? L"1" : L"0") && okay;
    okay = IniWrite(L"AutoRefresh", state.prefs.autoRefresh ? L"1" : L"0") && okay;
    okay = IniWrite(L"TrendColor", std::to_wstring(state.prefs.trendColor)) && okay;
    okay = IniWrite(L"HeatColor", std::to_wstring(state.prefs.heatColor)) && okay;
    okay = IniWrite(L"USDCNY", std::to_wstring(state.prefs.usdToCny)) && okay;
    okay = IniWrite(L"Theme", std::to_wstring(state.prefs.theme)) && okay;
    okay = IniWrite(L"Language", std::to_wstring(state.prefs.language)) && okay;
    okay = IniWrite(L"DailyBudget", std::to_wstring(state.prefs.dailyBudget)) && okay;
    okay = IniWrite(L"MonthlyBudget", std::to_wstring(state.prefs.monthlyBudget)) && okay;
    okay = IniWrite(L"BudgetCNY", state.prefs.budgetCny ? L"1" : L"0") && okay;
    okay = WritePrivateProfileStringW(L"TokenScope", L"ImportCodex", nullptr, settingsPath.c_str()) && okay;
    okay = WritePrivateProfileStringW(L"TokenScope", L"ImportClaude", nullptr, settingsPath.c_str()) && okay;
    for (const auto& source : CatalogSources()) {
        auto found = std::find_if(configuredImports.begin(), configuredImports.end(),
                                  [&](const auto& entry) { return entry.first == source.id; });
        const auto key = L"Import." + Utf8ToWide(source.id);
        okay = WritePrivateProfileStringW(L"TokenScope", key.c_str(),
                                          found == configuredImports.end() ? nullptr : found->second.c_str(),
                                          settingsPath.c_str()) &&
               okay;
    }
    if (!okay) state.notice = Tr(L"设置保存失败，请检查数据目录写入权限。");
    return okay;
}

} // namespace tokenscope::shell
