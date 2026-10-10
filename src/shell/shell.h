#pragma once
// Internal interface of the native Windows shell (main window, tray, dialogs and background workers).
// Everything here runs on the UI thread unless a comment says otherwise.
#include "app.h"
#include "adapters.h"
#include "pricing.h"
#include "exchange.h"
#include "connections.h"
#include "credentials.h"
#include "i18n.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tokenscope::shell {
namespace fs = std::filesystem;

constexpr UINT WM_TRAY = WM_APP + 1, WM_IMPORTED = WM_APP + 2, WM_ONLINE = WM_APP + 3, WM_EXCHANGE = WM_APP + 4,
               WM_CONNECTION = WM_APP + 5, WM_SOURCES_CHANGED = WM_APP + 6;
constexpr UINT TIMER_REFRESH = 1;
constexpr UINT TIMER_EXIT = 2;
constexpr UINT TIMER_WATCH = 3;

struct ConnectionState {
    std::string state;
    std::wstring detail;
    ULONGLONG lastAttempt = 0;
};
struct ScanPacket {
    Snapshot snapshot;
    ScanResult result;
    std::wstring notice;
};
struct ExchangePacket {
    std::optional<ExchangeRateQuote> quote;
    std::string fetchedDay, error;
};
struct ConnectionPacket {
    std::string id;
    ConnectionSyncResult result;
};

// Process-wide shell state (defined in state.cpp).
extern HINSTANCE instance;
extern HWND mainWindow, tooltipWindow;
extern UINT taskbarCreated;
extern ViewState state;
extern Store store;
extern std::wstring dataDirectory, settingsPath, importCodex, importClaude;
extern std::vector<std::pair<std::string, std::wstring>> configuredImports;
extern std::thread collector, networkWorker, exchangeWorker, connectionWorker;
extern std::atomic_bool cancelled, networkBusy, exchangeBusy, connectionBusy;
extern std::map<std::string, ConnectionState> connectionStates;
extern std::string exchangeFetchedDay;
extern ULONGLONG exchangeLastAttempt;
extern NOTIFYICONDATAW trayIcon;
extern int keyboardHit;
extern bool shuttingDown, pauseCollection, priceDialogActive, importRefreshPending;
extern std::wstring lastDay;
extern ULONGLONG lastCollectionStart;
extern bool watchRefreshPending;

// settings.cpp
std::wstring GetDataDirectory();
std::wstring IniString(const wchar_t* key, const wchar_t* fallback = L"");
bool IniWrite(const wchar_t* key, const std::wstring& value);
bool EnsureUnicodeSettings();
void LoadPreferences();
bool SavePreferences();

// workers.cpp
void StartCollection();
std::wstring CheckModelCatalogue();
void ApplyExchangeQuote(const ExchangeRateQuote& quote);
void LoadExchangeCache();
ExchangePacket FetchExchangeRate(const std::wstring& directory);
void ApplyExchangeResult(const ExchangePacket& packet);
void StartExchangeRate(bool force = false);
void StartOnlineCheck();
// Signals cancellation, aborts in-flight HTTP calls and joins every worker.
void StopWorkers();
// Writes bytes to path via a pending file and an atomic replace. Returns false on any failure.
bool WriteFileAtomically(const fs::path& path, const std::string& bytes);
bool WriteTextFile(const fs::path& path, const std::string& text);

// watcher.cpp
// Watches the existing directories behind every source and posts WM_SOURCES_CHANGED to the
// main window (debounced) when something changes. Restarted whenever the paths change.
void RestartSourceWatcher();
void StopSourceWatcher();
bool SourceWatcherActive();
// Debounces change notifications and starts at most one collection per kMinimumScanSpacing.
void OnSourcesChanged();
void OnWatchTimer();
constexpr ULONGLONG kMinimumScanSpacing = 10 * 1000, kFallbackPollInterval = 5 * 60 * 1000;

// budget.cpp
// Shows a tray balloon once per day / month when estimated spending reaches 80% and 100% of a limit.
void CheckBudgetAlerts();

// theme.cpp
bool SystemUsesDarkTheme();
// Resolves the effective theme from the preference and Windows, then repaints.
void RefreshTheme();
// Applies the main window caption text and dark/light title bar for the current language/theme.
void ApplyWindowChrome(HWND window);

// dialogs.cpp
void RegisterDialogClasses(WNDCLASSEXW base);
bool RunModal(const wchar_t* klass, const std::wstring& title, int logicalWidth, int logicalHeight, void* dialog,
              bool& done);
std::wstring EditText(HWND edit);
bool ParsePrice(std::wstring text, int64_t& result);
std::wstring ChooseFolder(HWND owner = nullptr);
std::wstring ChooseUsageFile(const SourceSpec& source, HWND owner = nullptr);
void EditExchangeRate();
void EditBudget();
void ChooseChartColor(bool heat);
void EditModelPrice();
LRESULT CALLBACK RateDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK BudgetDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PriceDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ImportDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SourcePathDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ConnectionDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// sources.cpp
const SourceSpec* FindSource(const std::string& id);
void RefreshConfiguredSource(const std::string& id, const std::wstring& path);
void ConfigureSourcePath(const std::string& id);
void DecorateSources();
void SyncSource(const std::string& id);
void ConnectSource(const std::string& id);
void DisconnectSource(const std::string& id);
void SourceHelp(const std::string& id);
void AddImportSource();
void ExportData();

// window.cpp
void UpdateTray();
void AddTray();
void ShowTrayBalloon(const std::wstring& title, const std::wstring& text);
void Redraw();
void OpenMain(bool settings = false);
void Execute(int command, const std::string& value = {});
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

} // namespace tokenscope::shell
