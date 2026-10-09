#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tokenscope {

struct UsageRow {
    std::string day;       // Windows local calendar date, YYYY-MM-DD.
    std::string software;
    std::string model;     // Actual model ID from the record; empty means unknown.
    std::string provider;
    int64_t input = 0;     // Includes cache reads/writes when the source does.
    int64_t output = 0;
    int64_t cachedInput = 0;
    int64_t cacheWrite = 0;
    int64_t reasoning = 0; // Subset of output, never added to total again.
    int64_t unclassified = 0; // Official total-only usage, with no invented input/output split.
    int64_t requests = 0;
    std::optional<int64_t> costUsdMicros; // Missing is unknown, never zero.
    int64_t unpricedRequests = 0;
    int64_t cacheWrite1h = 0;
    std::string priceSources; // Applied price provenance; model identity is never rewritten.
    int64_t automaticRequests = 0;
    int64_t total() const { return input + output + unclassified; }
};

struct SourcePathStatus { std::wstring path; bool exists = false; };
struct SourceStatus {
    std::string id;
    std::string name;
    std::wstring path;
    std::string status;
    int64_t records = 0;
    int64_t files = 0;
    std::string detail;
    std::vector<SourcePathStatus> paths;
    bool customPath = false;
    std::string connectionState;
    std::wstring connectionDetail;
    bool canAuthorize = false, hasCredentials = false;
};

struct Snapshot {
    std::vector<UsageRow> rows; // Aggregated by local day + software + model + provider.
    std::vector<SourceStatus> sources;
    std::string updatedAt;
    int64_t events = 0;
    int64_t invalidRecords = 0;
    std::string error;
    std::string pricingStatus;
};

struct ScanResult {
    int64_t added = 0;
    int64_t files = 0;
    int64_t invalid = 0;
    std::string error;
};

struct PriceRule {
    std::string software, model, provider;
    // USD micro-dollars per million tokens, entered explicitly by the user.
    int64_t inputMicrosPerMillion = 0, outputMicrosPerMillion = 0;
    int64_t cachedMicrosPerMillion = 0, writeMicrosPerMillion = 0;
};

class Store {
public:
    Store();
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    bool Open(const std::wstring& path, std::string& error);
    ScanResult ScanDefaultSources(const std::atomic_bool& cancelled);
    ScanResult ScanSources(const std::vector<std::pair<std::string,std::wstring>>& overrides,
                           const std::atomic_bool& cancelled);
    ScanResult ImportFolder(const std::wstring& folder, const std::string& source,
                            const std::atomic_bool& cancelled);
    Snapshot ReadSnapshot();
    bool SetPriceRule(const PriceRule& rule, std::string& error);
    bool ReloadPricing(std::string& error); // Load public local caches, price only unpriced records.
    bool ExportCsv(const std::wstring& path, const Snapshot& snapshot, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string LocalToday();
std::string ShiftDay(const std::string& day, int delta);
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);

enum class Page { Overview, Software, Models, Settings };
enum class Range { Today, Week, Month, All };
enum class Chart { Trend, Heatmap };

struct Preferences {
    Range range = Range::Today;
    bool cny = false;
    double usdToCny = 0; // Explicit manual rate; zero selects the latest public rate.
    COLORREF trendColor = RGB(53, 101, 223);
    COLORREF heatColor = RGB(105, 157, 47);
    bool autoRefresh = true;
    double automaticUsdToCny = 0;
    std::string exchangeRateDate, exchangeRateSource, exchangeRateStatus;
};

double EffectiveUsdToCny(const Preferences& prefs);

struct HitTarget {
    RECT rect{}; // Device pixels, matches the Win32 client area.
    int command = 0;
    std::string value;
};

enum Command {
    CmdOverview = 100, CmdSoftware, CmdModels, CmdSettings,
    CmdToday, CmdWeek, CmdMonth, CmdAll,
    CmdTrend, CmdHeatmap, CmdTokens, CmdCost,
    CmdSelectDay, CmdClearDay, CmdTrendWeek, CmdTrendMonth, CmdPrevious, CmdNext,
    CmdRefresh, CmdCurrency, CmdExchangeRate, CmdTrendColor, CmdHeatColor,
    CmdResetColors, CmdOpenData, CmdExport, CmdImportCodex, CmdImportClaude,
    CmdAutoRefresh, CmdOnlineCheck, CmdTrayMonth, CmdTrayToday,
    CmdOpenMain, CmdExit, CmdPricing, CmdImportSource,
    CmdSourcePath, CmdSourceReset, CmdSourceConnect, CmdSourceSync, CmdSourceDisconnect, CmdSourceHelp
};

struct ViewState {
    Snapshot snapshot;
    Preferences prefs;
    Page page = Page::Overview;
    Chart chart = Chart::Trend;
    bool costMetric = false;
    bool trendMonth = false;
    bool collecting = false;
    bool trayMonth = false;
    std::string selectedDay;
    std::string chartEnd;
    std::wstring notice;
    std::wstring onlineStatus;
    int scroll = 0;
    int contentHeight = 0;
    float dpi = 1;
    std::vector<HitTarget> hits;
};

void PaintView(HWND window, HDC dc, const RECT& client, ViewState& state);
std::wstring FormatCount(int64_t count, bool compact = false);
std::wstring FormatCost(const std::optional<int64_t>& micros, const Preferences& prefs);
std::vector<UsageRow> SelectRows(const ViewState& state, bool tray = false);
std::wstring TraySummary(const ViewState& state);

} // namespace tokenscope
