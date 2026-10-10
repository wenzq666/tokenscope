#include "shell/shell.h"
#include "i18n.h"

using namespace tokenscope;
using namespace tokenscope::shell;

namespace {

int Diagnose(const std::wstring& exportPath, bool scan, const std::string& source, const std::wstring& folder,
             bool network, bool exchangeCheck) {
    std::string error;
    if (!store.Open((fs::path(dataDirectory) / L"usage.sqlite3").wstring(), error)) {
        fprintf(stderr, "Database: %s\n", error.c_str());
        return 1;
    }
    LoadPreferences();
    std::wstring networkStatus;
    if (network) networkStatus = CheckModelCatalogue();
    if (exchangeCheck && state.prefs.usdToCny <= 0) ApplyExchangeResult(FetchExchangeRate(dataDirectory));
    if (!store.ReloadPricing(error)) {
        fprintf(stderr, "Pricing: %s\n", error.c_str());
        return 1;
    }
    ScanResult result;
    if (scan) result = store.ScanSources(configuredImports, cancelled);
    if (!folder.empty()) {
        auto imported = store.ImportFolder(folder, source, cancelled);
        result.added += imported.added;
        result.invalid += imported.invalid;
        result.error += imported.error;
    }
    Snapshot snapshot = store.ReadSnapshot();
    if (!exportPath.empty() && !store.ExportCsv(exportPath, snapshot, error)) {
        fprintf(stderr, "Export: %s\n", error.c_str());
        return 1;
    }
    int64_t tokens = 0;
    std::set<std::string> software, models;
    for (const auto& row : snapshot.rows) {
        tokens += row.total();
        if (row.total() > 0) {
            software.insert(row.software);
            models.insert((row.provider.empty() ? "unknown-provider:" + row.software : row.provider) + "/" + row.model);
        }
    }
    printf(
        "{\"events\":%lld,\"added\":%lld,\"tokens\":%lld,\"software\":%zu,\"modelIdentities\":%zu,\"dailyModelRows\":%zu,\"invalid\":%lld}\n",
        static_cast<long long>(snapshot.events), static_cast<long long>(result.added), static_cast<long long>(tokens),
        software.size(), models.size(), snapshot.rows.size(), static_cast<long long>(result.invalid));
    if (!result.error.empty()) fprintf(stderr, "%s\n", result.error.c_str());
    if (!snapshot.error.empty()) fprintf(stderr, "%s\n", snapshot.error.c_str());
    if (network) printf("%s\n", WideToUtf8(networkStatus).c_str());
    if (exchangeCheck) {
        printf("{\"usdToCny\":%.8f,\"manual\":%s,\"publicationDate\":\"%s\",\"source\":\"%s\"}\n",
               EffectiveUsdToCny(state.prefs), state.prefs.usdToCny > 0 ? "true" : "false",
               state.prefs.usdToCny > 0 ? "" : state.prefs.exchangeRateDate.c_str(),
               state.prefs.usdToCny > 0 ? "manual" : state.prefs.exchangeRateSource.c_str());
        if (state.prefs.usdToCny <= 0 && !state.prefs.exchangeRateStatus.empty())
            printf("%s\n", state.prefs.exchangeRateStatus.c_str());
    }
    fflush(stdout);
    return snapshot.error.empty() ? 0 : 1;
}
} // namespace

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR, int show) {
    instance = appInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool diagnose = false, scan = true, network = false, hidden = false, exchangeCheck = false;
    int exitAfterSeconds = 0;
    std::wstring exportPath, folder;
    std::string source;
    dataDirectory = GetDataDirectory();
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--data-dir" && i + 1 < argc)
            dataDirectory = argv[++i];
        else if (arg == L"--diagnose")
            diagnose = true;
        else if (arg == L"--no-scan")
            scan = false;
        else if (arg == L"--network-check")
            network = true;
        else if (arg == L"--exchange-check")
            exchangeCheck = true;
        else if (arg == L"--tray")
            hidden = true;
        else if (arg == L"--exit-after" && i + 1 < argc)
            exitAfterSeconds = std::clamp(_wtoi(argv[++i]), 1, 3600);
        else if (arg == L"--export" && i + 1 < argc)
            exportPath = argv[++i];
        else if (arg == L"--import" && i + 2 < argc) {
            source = WideToUtf8(argv[++i]);
            folder = argv[++i];
        }
    }
    LocalFree(argv);
    try {
        dataDirectory = fs::absolute(dataDirectory).lexically_normal().wstring();
        fs::create_directories(dataDirectory);
    } catch (...) {
        MessageBoxW(nullptr, Tr(L"无法创建数据目录。"), L"TokenScope", MB_OK | MB_ICONERROR);
        return 1;
    }
    settingsPath = (fs::path(dataDirectory) / L"settings.ini").wstring();
    if (diagnose) {
        int result = Diagnose(exportPath, scan, source, folder, network, exchangeCheck);
        CoUninitialize();
        return result;
    }
    std::wstring mutexName = L"Local\\TokenScope-" + std::to_wstring(std::hash<std::wstring>{}(dataDirectory));
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TokenScopeMain", nullptr);
        if (other) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }
    std::string error;
    if (!store.Open((fs::path(dataDirectory) / L"usage.sqlite3").wstring(), error)) {
        MessageBoxW(nullptr, Utf8ToWide(error).c_str(), Tr(L"数据库初始化失败"), MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }
    LoadPreferences();
    state.snapshot = store.ReadSnapshot();
    DecorateSources();
    state.chartEnd = LocalToday();
    lastDay = Utf8ToWide(LocalToday());
    state.onlineStatus = Tr(L"内置参考价目离线可用；可按需更新三源价目，仅补未定价记录，不上传本地日志。");
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    klass.hIconSm = klass.hIcon;
    klass.lpszClassName = L"TokenScopeMain";
    RegisterClassExW(&klass);
    RegisterDialogClasses(klass);
    mainWindow =
        CreateWindowExW(0, L"TokenScopeMain", Tr(L"TokenScope · 本地智能体用量"), WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 850, nullptr, nullptr, instance, nullptr);
    if (!mainWindow) {
        CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }
    RefreshTheme();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    tooltipWindow =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, mainWindow, nullptr, instance, nullptr);
    if (tooltipWindow) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = mainWindow;
        tool.uId = 1;
        GetClientRect(mainWindow, &tool.rect);
        tool.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(tooltipWindow, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(tooltipWindow, TTM_SETMAXTIPWIDTH, 0, 440);
    }
    SetTimer(mainWindow, TIMER_REFRESH, 15000, nullptr);
    if (exitAfterSeconds) SetTimer(mainWindow, TIMER_EXIT, exitAfterSeconds * 1000, nullptr);
    if (!hidden) {
        ShowWindow(mainWindow, show);
        UpdateWindow(mainWindow);
    }
    StartCollection();
    StartExchangeRate();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
