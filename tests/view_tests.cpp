#ifdef NDEBUG
#undef NDEBUG
#endif
#include "app.h"
#include "../src/heatmap_layout.h"
#include <wincodec.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>

using namespace tokenscope;

namespace {

UsageRow Row(const std::string& day, const std::string& app, int64_t input, int64_t output,
             const std::string& provider = "", const std::string& model = "test-model") {
    UsageRow row;
    row.day = day; row.software = app; row.input = input; row.output = output;
    row.provider = provider; row.model = model; row.requests = 1;
    return row;
}

SourceStatus Source(const std::string& id, const std::string& name, const std::string& status) {
    SourceStatus source;
    source.id = id; source.name = name; source.status = status;
    return source;
}

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    RECT rect{};
    Surface(int width = 1200, int height = 820) {
        rect = {0, 0, width, height};
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        assert(bitmap && dc); previous = SelectObject(dc, bitmap);
    }
    ~Surface() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
    void Save(const std::filesystem::path& path) {
        IWICImagingFactory* factory = nullptr; IWICBitmap* source = nullptr;
        IWICStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory, reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source);
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(rect.right, rect.bottom);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (frame) frame->Release(); if (encoder) encoder->Release(); if (stream) stream->Release();
        if (source) source->Release(); if (factory) factory->Release();
        assert(SUCCEEDED(hr));
    }
};

HitTarget FindDay(const ViewState& state, const std::string& day) {
    for (const auto& hit : state.hits) if (hit.command == CmdSelectDay && hit.value == day) return hit;
    assert(false && "date hit target absent"); return {};
}

std::map<std::string, RECT> DayTargets(const ViewState& state) {
    std::map<std::string, RECT> result;
    for (const auto& hit : state.hits) if (hit.command == CmdSelectDay) result.emplace(hit.value, hit.rect);
    return result;
}

void AssertSameCalendarTargets(const std::map<std::string, RECT>& expected, const ViewState& state) {
    const auto actual = DayTargets(state);
    assert(expected.size() == actual.size());
    for (const auto& day : expected) {
        const auto found = actual.find(day.first);
        assert(found != actual.end());
        assert(EqualRect(&day.second, &found->second));
    }
}

void TestCalendarRollover() {
    const auto thursday = MakeHeatmapCalendar("2026-10-08", 53);
    const auto friday = MakeHeatmapCalendar("2026-10-09", 53);
    assert(thursday.last == "2026-10-08" && friday.last == "2026-10-09");
    assert(thursday.days == 365 && friday.days == 365);
    assert(friday.first == ShiftDay(thursday.first, 1));
    assert(friday.gridFirst == thursday.gridFirst);
    // In a compact calendar, advancing one day within a week appends one cell
    // without changing the existing dates or the Monday grid origin.
    const auto compactThursday = MakeHeatmapCalendar("2026-10-08", 20);
    const auto compactFriday = MakeHeatmapCalendar("2026-10-09", 20);
    assert(compactFriday.days == compactThursday.days + 1);
    assert(compactFriday.first == compactThursday.first);
    assert(compactFriday.gridFirst == compactThursday.gridFirst);
    const auto sunday = MakeHeatmapCalendar("2026-10-11", 53);
    const auto monday = MakeHeatmapCalendar("2026-10-12", 53);
    assert(monday.days == 365 && monday.first == ShiftDay(sunday.first, 1));
    assert(monday.gridFirst == ShiftDay(sunday.gridFirst, 7));
    const auto leapDay = MakeHeatmapCalendar("2024-02-29", 53);
    assert(leapDay.last == "2024-02-29" && leapDay.first == "2023-03-02");
}

void TestRangesAndCurrency() {
    ViewState state;
    const std::string today = LocalToday(), yesterday = ShiftDay(today, -1);
    state.snapshot.rows = {Row(today, "Codex", 70, 30), Row(yesterday, "Claude Code", 90, 10),
                           Row(ShiftDay(today, -7), "Codex", 20, 10), Row(today, "Unused", 0, 0)};
    assert(SelectRows(state).size() == 1);
    state.prefs.range = Range::Week; assert(SelectRows(state).size() == 2);
    state.prefs.range = Range::All; assert(SelectRows(state).size() == 3);
    state.selectedDay = yesterday; assert(SelectRows(state).size() == 1 && SelectRows(state)[0].software == "Claude Code");
    state.selectedDay = ShiftDay(today, -2); assert(SelectRows(state).empty());
    assert(FormatCost(std::nullopt, state.prefs) == L"费用未知");
    assert(FormatCost(0, state.prefs) == L"$0.00");
    assert(FormatCost(1250000, state.prefs) == L"$1.25");
    const auto before = state.snapshot.rows;
    for (int index = 0; index < 10; ++index) {
        state.prefs.cny = true; state.prefs.usdToCny = 0;
        assert(FormatCost(1250000, state.prefs) == L"汇率暂不可用");
        state.prefs.usdToCny = 7.2; assert(FormatCost(1250000, state.prefs) == L"¥9.00");
        state.prefs.cny = false; assert(FormatCost(1250000, state.prefs) == L"$1.25");
    }
    state.prefs.cny = true; state.prefs.usdToCny = 0; state.prefs.automaticUsdToCny = 7.2;
    assert(FormatCost(1250000, state.prefs) == L"¥9.00");
    state.prefs.usdToCny = 8.0;
    assert(FormatCost(1250000, state.prefs) == L"¥10.00"); // Explicit manual rate wins.
    state.prefs.usdToCny = 0;
    assert(FormatCost(1250000, state.prefs) == L"¥9.00"); // Clearing manual resumes the public rate.
    state.prefs.usdToCny = std::numeric_limits<double>::infinity();
    assert(FormatCost(1250000, state.prefs) == L"¥9.00"); // Invalid persisted manual values cannot defeat a valid public rate.
    state.prefs.usdToCny = 10000;
    assert(FormatCost(1250000, state.prefs) == L"¥9.00");
    state.prefs.usdToCny = 0;
    state.prefs.automaticUsdToCny = std::numeric_limits<double>::quiet_NaN();
    assert(FormatCost(1250000, state.prefs) == L"汇率暂不可用");
    state.prefs.automaticUsdToCny = 0;
    assert(FormatCost(1250000, state.prefs) == L"汇率暂不可用");
    state.prefs.cny = false;
    assert(FormatCost(1250000, state.prefs) == L"$1.25");
    for (size_t index = 0; index < before.size(); ++index) {
        assert(before[index].input == state.snapshot.rows[index].input);
        assert(before[index].output == state.snapshot.rows[index].output);
        assert(before[index].costUsdMicros == state.snapshot.rows[index].costUsdMicros);
    }
    assert(FormatCount(1234567) == L"1,234,567");
    assert(FormatCount(-1234567) == L"-1,234,567");
    state.selectedDay = yesterday; state.prefs.range = Range::All;
    assert(TraySummary(state).find(L"今日 100 Token") != std::wstring::npos);
}

void TestBeijingUpdateTimes() {
    ViewState state;
    const std::pair<const char*, const wchar_t*> cases[] = {
        {"2026-10-09T02:50:16Z", L"2026-10-09 10:50:16 · 北京时间"},
        {"2026-10-09T18:50:16Z", L"2026-10-10 02:50:16 · 北京时间"},
        {"2026-12-31T20:00:00Z", L"2027-01-01 04:00:00 · 北京时间"},
        {"2024-02-28T18:00:00.123Z", L"2024-02-29 02:00:00 · 北京时间"},
        {"2026-10-09T03:00:00+09:00", L"2026-10-09 02:00:00 · 北京时间"}
    };
    for (const auto& item : cases) {
        state.snapshot.updatedAt = item.first;
        const auto summary = TraySummary(state);
        assert(summary.find(item.second) != std::wstring::npos);
        assert(summary.find(Utf8ToWide(item.first)) == std::wstring::npos);
    }
    for (const auto& invalid : {"not-a-date", "2026-02-30T02:50:16Z", "2026-10-09T24:00:00Z", "2026-10-09T02:50:16", "2026-10-09T02:50:16.Z", "9999-12-31T23:59:59Z"}) {
        state.snapshot.updatedAt = invalid;
        const auto summary = TraySummary(state);
        assert(summary.find(L"更新时间未知") != std::wstring::npos);
        assert(summary.find(Utf8ToWide(invalid)) == std::wstring::npos);
    }
    state.snapshot.updatedAt.clear();
    assert(TraySummary(state).find(L"尚未更新") != std::wstring::npos);
}

void TestAutomaticCurrencyChart(const std::filesystem::path& output) {
    ViewState state;
    state.prefs.cny = true; state.prefs.usdToCny = 0; state.prefs.automaticUsdToCny = 7.2;
    state.prefs.exchangeRateDate = "2026-10-09"; state.prefs.exchangeRateSource = "Frankfurter / ECB";
    state.snapshot.updatedAt = "2026-10-09T02:50:16Z";
    state.costMetric = true;
    const auto today = LocalToday(), yesterday = ShiftDay(today, -1);
    state.snapshot.rows = {Row(today, "Codex", 100, 50), Row(yesterday, "Claude Code", 100, 50)};
    state.snapshot.rows[0].costUsdMicros = 1000000; state.snapshot.rows[1].costUsdMicros = 2000000;
    Surface surface(800, 700);
    PaintView(nullptr, surface.dc, surface.rect, state);
    const auto targets = DayTargets(state);
    const auto hit = FindDay(state, today);
    const int pixelX = (hit.rect.left + hit.rect.right) / 2, pixelY = hit.rect.top + 80;
    assert(GetPixel(surface.dc, pixelX, pixelY) == state.prefs.trendColor);
    assert(FormatCost(1000000, state.prefs) == L"¥7.20");
    surface.Save(output / L"ui-beijing-auto-cny.png");
    state.prefs.usdToCny = 8.0;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(targets, state);
    assert(GetPixel(surface.dc, pixelX, pixelY) == state.prefs.trendColor);
    assert(FormatCost(1000000, state.prefs) == L"¥8.00");
    state.prefs.usdToCny = 0; state.prefs.automaticUsdToCny = 0;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(targets, state);
    assert(GetPixel(surface.dc, pixelX, pixelY) != state.prefs.trendColor);
    assert(FormatCost(1000000, state.prefs) == L"汇率暂不可用");
    state.prefs.cny = false;
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(GetPixel(surface.dc, pixelX, pixelY) == state.prefs.trendColor);
    assert(FormatCost(1000000, state.prefs) == L"$1.00");
    state.page = Page::Settings; state.prefs.automaticUsdToCny = 7.2;
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(std::any_of(state.hits.begin(), state.hits.end(), [](const HitTarget& target) { return target.command == CmdExchangeRate; }));
    surface.Save(output / L"ui-settings-auto-rate.png");
    state.prefs.usdToCny = 8.0; state.prefs.exchangeRateStatus = "更新失败，沿用最近公布汇率；当前手动设置优先。";
    PaintView(nullptr, surface.dc, surface.rect, state);
    surface.Save(output / L"ui-settings-manual-rate.png");
    state.prefs.usdToCny = 0; state.prefs.automaticUsdToCny = 0;
    state.prefs.exchangeRateDate.clear(); state.prefs.exchangeRateSource.clear();
    state.prefs.exchangeRateStatus = "获取失败；离线暂无有效缓存，人民币费用保持未知。";
    PaintView(nullptr, surface.dc, surface.rect, state);
    surface.Save(output / L"ui-settings-no-rate.png");
}

void TestPaint(const std::filesystem::path& output) {
    ViewState state;
    state.prefs.range = Range::All;
    const auto today = LocalToday(), zero = ShiftDay(today, -1), missing = ShiftDay(today, -2);
    state.snapshot.rows = {Row(today, "Codex", 120000, 40000, "openai", "test-model-a"),
                          Row(today, "Claude Code", 90000, 22000, "anthropic", "test-model-b"),
                          Row(zero, "Codex", 0, 0, "openai", "test-model-a")};
    state.snapshot.rows[2].costUsdMicros = 0;
    Surface surface;
    PaintView(nullptr, surface.dc, surface.rect, state);
    auto zeroHit = FindDay(state, zero), missingHit = FindDay(state, missing);
    COLORREF zeroColor = GetPixel(surface.dc, (zeroHit.rect.left + zeroHit.rect.right) / 2, zeroHit.rect.bottom - 29);
    COLORREF missingColor = GetPixel(surface.dc, (missingHit.rect.left + missingHit.rect.right) / 2, missingHit.rect.bottom - 29);
    assert(zeroColor == state.prefs.trendColor && zeroColor != missingColor);
    surface.Save(output / L"ui-overview.png");
    const auto weekTargets = DayTargets(state);
    const auto navigationEnd = state.chartEnd;
    state.selectedDay = zero;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(weekTargets, state);
    assert(state.chartEnd == navigationEnd);
    state.selectedDay = ShiftDay(today, -80); // Even an off-chart selection cannot load another range.
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(weekTargets, state);
    assert(state.chartEnd == navigationEnd);
    state.trendMonth = true; state.chartEnd = ShiftDay(today, -35);
    state.selectedDay.clear();
    PaintView(nullptr, surface.dc, surface.rect, state);
    const auto monthTargets = DayTargets(state);
    const auto monthEnd = state.chartEnd;
    state.selectedDay = monthTargets.begin()->first;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(monthTargets, state);
    assert(state.chartEnd == monthEnd);
    state.trendMonth = false; state.selectedDay.clear(); state.chartEnd.clear();
    state.chart = Chart::Heatmap;
    PaintView(nullptr, surface.dc, surface.rect, state);
    zeroHit = FindDay(state, zero); missingHit = FindDay(state, missing);
    zeroColor = GetPixel(surface.dc, (zeroHit.rect.left + zeroHit.rect.right) / 2, (zeroHit.rect.top + zeroHit.rect.bottom) / 2);
    missingColor = GetPixel(surface.dc, (missingHit.rect.left + missingHit.rect.right) / 2, (missingHit.rect.top + missingHit.rect.bottom) / 2);
    assert(zeroColor == RGB(232, 236, 242));
    assert(missingColor == RGB(245, 247, 250));
    const auto calendarTargets = DayTargets(state);
    const auto calendar = MakeHeatmapCalendar(today, 53);
    assert(calendarTargets.size() == static_cast<size_t>(calendar.days));
    assert(calendarTargets.begin()->first == calendar.first);
    assert(calendarTargets.rbegin()->first == today);
    surface.Save(output / L"ui-heatmap.png");
    state.selectedDay = zero;
    state.chartEnd = zero;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(calendarTargets, state);
    assert(SelectRows(state).empty());
    state.selectedDay = ShiftDay(today, -80);
    state.chartEnd = state.selectedDay;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(calendarTargets, state);
    // Trend navigation may change the trend range, never the heatmap range.
    state.chart = Chart::Trend;
    state.chartEnd = ShiftDay(today, -120);
    PaintView(nullptr, surface.dc, surface.rect, state);
    state.chart = Chart::Heatmap;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(calendarTargets, state);
    state.selectedDay.clear(); state.chartEnd = today;
    PaintView(nullptr, surface.dc, surface.rect, state);
    AssertSameCalendarTargets(calendarTargets, state);
    state.selectedDay.clear(); state.page = Page::Settings;
    state.snapshot.sources = {Source("codex", "Codex", "ready"), Source("claude", "Claude Code", "not-found")};
    state.snapshot.sources[0].records = state.snapshot.sources[0].files = 1;
    state.snapshot.sources[0].detail = "Test fixture";
    state.snapshot.sources[1].detail = "No records in test fixture";
    state.snapshot.pricingStatus = "已加载 LiteLLM / models.dev / OpenRouter 参考价目；未匹配模型保持未定价，历史费率已固定。";
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(state.contentHeight > surface.rect.bottom);
    surface.Save(output / L"ui-settings.png");
    const int twoSourceHeight = state.contentHeight;
    assert(std::any_of(state.hits.begin(), state.hits.end(), [](const HitTarget& hit) { return hit.command == CmdOnlineCheck; }));
    state.scroll = 300;
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(std::any_of(state.hits.begin(), state.hits.end(), [](const HitTarget& hit) { return hit.command == CmdImportSource; }));
    surface.Save(output / L"ui-settings-sources.png");
    state.snapshot.sources.pop_back();
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(state.contentHeight < twoSourceHeight - 160);
    // New catalog sources use the same settings layout; no source count or
    // special name is compiled into the page.
    state.snapshot.sources.push_back(Source("future-agent", "Future Agent", "needs-export"));
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(state.contentHeight == twoSourceHeight);
    state.page = Page::Models; state.selectedDay.clear(); state.scroll = 0;
    state.snapshot.rows = {Row(today, "Codex", 1, 1, "", "same-id"), Row(today, "Claude Code", 1, 1, "", "same-id")};
    Surface small(1200, 300);
    PaintView(nullptr, small.dc, small.rect, state); const auto separateHeight = state.contentHeight;
    state.snapshot.rows[0].provider = "verified-provider"; state.snapshot.rows[1].provider = "verified-provider";
    PaintView(nullptr, small.dc, small.rect, state); assert(separateHeight > state.contentHeight + 50);
    state.dpi = 1.5f; state.page = Page::Overview; state.chart = Chart::Trend;
    Surface highDpi(1800, 1230);
    PaintView(nullptr, highDpi.dc, highDpi.rect, state);
    auto dayHit = FindDay(state, today); assert(dayHit.rect.right > 1700);
    highDpi.Save(output / L"ui-high-dpi.png");
}

void TestTokenBuckets(const std::filesystem::path& output) {
    ViewState state;
    state.page = Page::Software;
    auto row = Row(LocalToday(), "Codex", 100, 50, "openai", "bucket-fixture");
    row.cachedInput = 40; row.cacheWrite = 20; row.cacheWrite1h = 10;
    row.reasoning = 10; row.unclassified = 50;
    row.costUsdMicros = 123000; row.automaticRequests = 1; row.priceSources = "OpenRouter 首方参考";
    state.snapshot.rows = {row};
    Surface surface(1200, 700);
    PaintView(nullptr, surface.dc, surface.rect, state);
    // Input includes both cache buckets; output includes reasoning. These
    // exact segment positions prove none of those subsets are counted twice.
    const int left = 200, barY = 228;
    assert(GetPixel(surface.dc, left + 50, barY) == RGB(71, 120, 221));
    assert(GetPixel(surface.dc, left + 240, barY) == RGB(145, 100, 214));
    assert(GetPixel(surface.dc, left + 440, barY) == RGB(42, 157, 122));
    assert(GetPixel(surface.dc, left + 630, barY) == RGB(216, 159, 52));
    assert(GetPixel(surface.dc, left + 700, barY) == RGB(204, 99, 146));
    assert(GetPixel(surface.dc, left + 850, barY) == RGB(103, 116, 139));
    surface.Save(output / L"ui-token-buckets.png");
    Surface narrow(800, 700);
    PaintView(nullptr, narrow.dc, narrow.rect, state);
    narrow.Save(output / L"ui-narrow-software.png");
    state.snapshot.rows[0].input = state.snapshot.rows[0].output = 0;
    state.snapshot.rows[0].cachedInput = state.snapshot.rows[0].cacheWrite = 0;
    state.snapshot.rows[0].cacheWrite1h = state.snapshot.rows[0].reasoning = 0;
    state.snapshot.rows[0].unclassified = 200;
    state.snapshot.rows[0].costUsdMicros.reset(); state.snapshot.rows[0].unpricedRequests = 1;
    state.snapshot.rows[0].automaticRequests = 0; state.snapshot.rows[0].priceSources.clear();
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(GetPixel(surface.dc, left + 50, barY) == RGB(103, 116, 139));
    assert(GetPixel(surface.dc, left + 850, barY) == RGB(103, 116, 139));
    assert(!state.snapshot.rows[0].costUsdMicros && state.snapshot.rows[0].input == 0 && state.snapshot.rows[0].output == 0);
    surface.Save(output / L"ui-total-only.png");
}

void TestBrandResources() {
    IWICImagingFactory* factory = nullptr;
    assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))));
    for (int id = 201; id <= 238; ++id) {
        const HRSRC resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), RT_RCDATA);
        assert(resource);
        const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
        auto* data = static_cast<BYTE*>(LockResource(loaded));
        const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
        IWICStream* stream = nullptr; IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr;
        assert(SUCCEEDED(factory->CreateStream(&stream)));
        assert(SUCCEEDED(stream->InitializeFromMemory(data, size)));
        assert(SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
        assert(SUCCEEDED(decoder->GetFrame(0, &frame)));
        UINT width = 0, height = 0;
        assert(SUCCEEDED(frame->GetSize(&width, &height)) && width == 64 && height == 64);
        frame->Release(); decoder->Release(); stream->Release();
    }
    factory->Release();
}

void TestSourcePathCards(const std::filesystem::path& output) {
    ViewState state;
    state.page = Page::Settings; state.scroll = 900;
    auto local = Source("codex", "Codex", "ready");
    local.paths = {{L"C:\\Users\\使用者\\.codex\\sessions", true}, {L"E:\\用量归档\\Codex\\archived_sessions", false}};
    local.detail = "本地实际用量记录"; local.customPath = true;
    local.files = 2; local.records = 8;
    auto cursor = Source("cursor", "Cursor", "pending");
    cursor.paths = {{L"C:\\Users\\使用者\\AppData\\Local\\TokenScope\\.cache\\cursor\\usage.json", false}};
    cursor.canAuthorize = true; cursor.connectionState = "unconfigured";
    cursor.connectionDetail = L"使用官方账户的只读用量接口，真实同步后写入本地缓存。";
    auto cloud = Source("coze", "Coze", "needs-export");
    cloud.paths = {{L"E:\\真实导出\\Coze\\usage.jsonl", false}};
    cloud.connectionState = "instructions";
    cloud.connectionDetail = L"需要真实逐请求用量导出，未实现账户授权连接。";
    state.snapshot.sources = {local, cursor, cloud};
    Surface surface(1200, 1200);
    auto count = [&](int command, const std::string& id) {
        return std::count_if(state.hits.begin(), state.hits.end(), [&](const HitTarget& target) { return target.command == command && target.value == id; });
    };
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(count(CmdSourcePath, "codex") == 3); // Two candidate rows and the explicit change action.
    assert(count(CmdSourceReset, "codex") == 1);
    assert(count(CmdSourceConnect, "codex") == 0 && count(CmdSourceSync, "codex") == 0);
    assert(count(CmdSourceHelp, "codex") == 1);
    assert(count(CmdSourceConnect, "cursor") == 1 && count(CmdSourceDisconnect, "cursor") == 0);
    assert(count(CmdSourceHelp, "coze") == 1 && count(CmdSourceConnect, "coze") == 0);
    assert(count(CmdSourcePath, "coze") == 2 && count(CmdSourceReset, "coze") == 1);
    surface.Save(output / L"ui-source-paths.png");
    const int beforeExtraPath = state.contentHeight;
    state.snapshot.sources[0].paths.push_back({L"E:\\非常长的目录名称\\" + std::wstring(120, L'长') + L"\\session-2026-10-09.jsonl", true});
    PaintView(nullptr, surface.dc, surface.rect, state);
    assert(state.contentHeight == beforeExtraPath + 26);
    assert(count(CmdSourcePath, "codex") == 4);
    state.snapshot.sources[1].hasCredentials = true;
    for (const auto& connection : {"saved", "syncing", "connected", "error", "expired"}) {
        state.snapshot.sources[1].connectionState = connection;
        PaintView(nullptr, surface.dc, surface.rect, state);
        assert(count(CmdSourceSync, "cursor") == 1 && count(CmdSourceDisconnect, "cursor") == 1);
        const bool reauthorize = std::string(connection) == "error" || std::string(connection) == "expired";
        assert(count(CmdSourceConnect, "cursor") == (reauthorize ? 1 : 0));
        assert(count(CmdSourceConnect, "coze") == 0);
    }
    state.snapshot.sources[1].connectionState = "expired";
    state.snapshot.sources[1].connectionDetail = L"授权过期，请重新授权；可解除本地保存的凭据。";
    Surface narrow(800, 1200);
    PaintView(nullptr, narrow.dc, narrow.rect, state);
    assert(count(CmdSourceConnect, "cursor") == 1 && count(CmdSourceSync, "cursor") == 1 && count(CmdSourceDisconnect, "cursor") == 1);
    for (const auto& target : state.hits) if (target.command >= CmdSourcePath && target.command <= CmdSourceHelp)
        assert(target.rect.left >= 174 && target.rect.right <= narrow.rect.right - 18);
    narrow.Save(output / L"ui-source-paths-narrow.png");
    // Missing or future sources remain configuration-only and cannot create a
    // zero-token software row merely by gaining a local path or connection.
    state.page = Page::Software; state.scroll = 0;
    assert(SelectRows(state).empty());
}

bool RenderRealDatabase(const std::filesystem::path& database, const std::filesystem::path& output) {
    if (!std::filesystem::is_regular_file(database)) {
        std::fputs("Real database does not exist.\n", stderr); return false;
    }
    Store store;
    std::string error;
    if (!store.Open(database.wstring(), error)) { std::fprintf(stderr, "Database open failed: %s\n", error.c_str()); return false; }
    ViewState state;
    state.snapshot = store.ReadSnapshot();
    if (!state.snapshot.error.empty()) { std::fprintf(stderr, "Read failed: %s\n", state.snapshot.error.c_str()); return false; }
    if (state.snapshot.rows.empty()) { std::fputs("Real database has no usage rows; no fixture will be substituted.\n", stderr); return false; }
    state.prefs.range = Range::All;
    state.chartEnd = LocalToday();
    state.notice = L"本机真实采集记录 · " + FormatCount(state.snapshot.events) + L" 条用量记录";
    std::filesystem::create_directories(output);
    Surface window(1200, 820);
    PaintView(nullptr, window.dc, window.rect, state);
    window.Save(output / L"real-overview.png");
    const int fullHeight = std::max(820, std::min(2200, state.contentHeight));
    Surface full(1200, fullHeight);
    PaintView(nullptr, full.dc, full.rect, state);
    full.Save(output / L"real-overview-full.png");
    state.chart = Chart::Heatmap;
    PaintView(nullptr, full.dc, full.rect, state);
    full.Save(output / L"real-heatmap.png");
    state.page = Page::Settings;
    PaintView(nullptr, window.dc, window.rect, state);
    window.Save(output / L"real-settings.png");
    state.page = Page::Models;
    PaintView(nullptr, full.dc, full.rect, state);
    full.Save(output / L"real-models.png");
    std::printf("Real database rendered: %llu day/software/model rows; %lld collected usage records. No synthetic fixtures executed.\n",
        static_cast<unsigned long long>(state.snapshot.rows.size()), static_cast<long long>(state.snapshot.events));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    assert(SUCCEEDED(hr));
    if (argc >= 2 && std::string(argv[1]) == "--real-db") {
        if (argc < 3) { std::fputs("Usage: view_tests.exe --real-db <path> [output-directory]\n", stderr); CoUninitialize(); return 2; }
        const std::filesystem::path output = argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path("build/real-view-output");
        const bool rendered = RenderRealDatabase(std::filesystem::path(argv[2]), output);
        CoUninitialize(); return rendered ? 0 : 1;
    }
    std::filesystem::path output = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("build/view-test-output");
    std::filesystem::create_directories(output);
    TestCalendarRollover(); TestRangesAndCurrency(); TestBeijingUpdateTimes(); TestAutomaticCurrencyChart(output);
    TestPaint(output); TestTokenBuckets(output); TestBrandResources(); TestSourcePathCards(output);
    CoUninitialize();
    std::puts("View tests passed: source paths/dynamic cards/auth states/instructions/narrow actions, Beijing timestamps, automatic/manual FX, fixed charts, ranges, disjoint tokens, pricing settings, tray hover, zero vs missing, model identity, DPI, resources, and offscreen rendering.");
    return 0;
}
