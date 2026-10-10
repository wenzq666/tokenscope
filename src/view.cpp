#include "app.h"
#include "i18n.h"
#include "heatmap_layout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <wincodec.h>

// Native layout and token composition are informed by missuo/tokens profile
// components (MIT; see vendor/LICENSE.tokens). No web runtime is embedded.

namespace tokenscope {
namespace {

struct Palette {
    COLORREF background, surface, ink, muted, line, accent, soft;
    COLORREF grid, heatOutside, heatEmpty, good, noticeFill, noticeLine, noticeInk;
};
constexpr Palette kLight{RGB(245, 247, 251), RGB(255, 255, 255), RGB(24, 34, 53),    RGB(103, 116, 139),
                         RGB(226, 232, 241), RGB(53, 101, 223),  RGB(235, 241, 255), RGB(237, 241, 247),
                         RGB(245, 247, 250), RGB(232, 236, 242), RGB(30, 128, 93),   RGB(255, 249, 233),
                         RGB(241, 224, 179), RGB(132, 94, 26)};
constexpr Palette kDark{RGB(17, 20, 27),    RGB(27, 31, 40), RGB(229, 233, 240), RGB(150, 160, 178), RGB(50, 56, 69),
                        RGB(110, 150, 245), RGB(37, 46, 70), RGB(42, 48, 60),    RGB(22, 25, 33),    RGB(40, 45, 56),
                        RGB(88, 196, 150),  RGB(56, 46, 24), RGB(102, 82, 38),   RGB(240, 205, 140)};

// The active palette. Selected at the start of every paint from the effective theme; painting
// happens on the UI thread only.
COLORREF Background = kLight.background, Surface = kLight.surface, Ink = kLight.ink, Muted = kLight.muted,
         Line = kLight.line, Accent = kLight.accent, Soft = kLight.soft, Grid = kLight.grid,
         HeatOutside = kLight.heatOutside, HeatEmpty = kLight.heatEmpty, Good = kLight.good,
         NoticeFill = kLight.noticeFill, NoticeLine = kLight.noticeLine, NoticeInk = kLight.noticeInk;

void UsePalette(const Palette& p) {
    Background = p.background;
    Surface = p.surface;
    Ink = p.ink;
    Muted = p.muted;
    Line = p.line;
    Accent = p.accent;
    Soft = p.soft;
    Grid = p.grid;
    HeatOutside = p.heatOutside;
    HeatEmpty = p.heatEmpty;
    Good = p.good;
    NoticeFill = p.noticeFill;
    NoticeLine = p.noticeLine;
    NoticeInk = p.noticeInk;
}

struct Totals {
    int64_t tokens = 0, input = 0, output = 0, cached = 0, requests = 0, unclassified = 0;
    int64_t cacheWrite = 0, cacheWrite1h = 0, reasoning = 0, automaticRequests = 0;
    int64_t unknown = 0;
    std::set<std::string> priceSources;
    std::optional<int64_t> cost;
};

Totals Sum(const std::vector<UsageRow>& rows) {
    Totals result;
    for (const auto& row : rows) {
        result.tokens += row.total();
        result.input += row.input;
        result.output += row.output;
        result.cached += row.cachedInput;
        result.requests += row.requests;
        result.unclassified += row.unclassified;
        result.cacheWrite += row.cacheWrite;
        result.cacheWrite1h += row.cacheWrite1h;
        result.reasoning += row.reasoning;
        result.automaticRequests += row.automaticRequests;
        std::istringstream sources(row.priceSources);
        for (std::string source; std::getline(sources, source, ',');)
            if (!source.empty()) result.priceSources.insert(source);
        if (row.costUsdMicros) result.cost = result.cost.value_or(0) + *row.costUsdMicros;
        result.unknown += row.unpricedRequests;
        if (!row.costUsdMicros && !row.unpricedRequests) ++result.unknown;
    }
    return result;
}

std::wstring RangeName(Range range) {
    switch (range) {
    case Range::Today:
        return Tr(L"今日");
    case Range::Week:
        return Tr(L"近 7 天");
    case Range::Month:
        return Tr(L"本月");
    default:
        return Tr(L"全部");
    }
}

std::vector<UsageRow> FilterRows(const Snapshot& snapshot, Range range, const std::string& selectedDay = "") {
    const std::string today = LocalToday();
    std::string first, last = today;
    if (!selectedDay.empty())
        first = last = selectedDay;
    else
        switch (range) {
        case Range::Today:
            first = today;
            break;
        case Range::Week:
            first = ShiftDay(today, -6);
            break;
        case Range::Month:
            first = today.substr(0, 7) + "-01";
            break;
        default:
            first = "0000-00-00";
            break;
        }
    std::vector<UsageRow> rows;
    for (const auto& row : snapshot.rows)
        if (row.day >= first && row.day <= last && row.total() > 0) rows.push_back(row);
    return rows;
}

std::wstring SourceState(const std::string& status) {
    if (status == "ready") return Tr(L"已采集");
    if (status == "not-found") return Tr(L"未发现本地路径");
    if (status == "needs-export") return Tr(L"需真实用量导出");
    if (status == "pending") return Tr(L"等待采集");
    if (status == "empty") return Tr(L"已读取 · 暂无用量");
    if (status == "unsupported") return Tr(L"尚未接入");
    if (status == "error") return Tr(L"读取失败");
    return Utf8ToWide(status);
}

std::wstring CostText(const Totals& total, const Preferences& prefs) {
    auto text = FormatCost(total.cost, prefs);
    if (total.cost && total.unknown) text += Tr(L" + 未定价");
    return text;
}

std::wstring Percent(int64_t value, int64_t total) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << (total > 0 ? value * 100.0 / total : 0) << L"%";
    return out.str();
}

// Price sources are stored in the database in Chinese; translate known parts for display only.
std::wstring DisplayPriceSource(const std::string& stored) {
    std::wstring text = Utf8ToWide(stored);
    for (const wchar_t* part : {L"手动费率", L" · 首方参考", L"本地缓存"}) {
        const auto found = text.find(part);
        if (found != std::wstring::npos) text.replace(found, wcslen(part), Tr(part));
    }
    return text;
}

std::wstring PriceLabel(const Totals& total, bool includeSources = false) {
    if (!total.cost) return Tr(L"未定价");
    bool manual = false;
    for (const auto& source : total.priceSources)
        if (source.find("手动") != std::string::npos || source.find("manual") != std::string::npos) manual = true;
    std::wstring label = total.automaticRequests ? (manual ? Tr(L"API 估算 · 含手动费率") : Tr(L"API 估算"))
                         : manual                ? Tr(L"手动费率估算")
                                                 : Tr(L"已有估算");
    if (includeSources && !total.priceSources.empty()) {
        label += L" · ";
        bool first = true;
        for (const auto& source : total.priceSources) {
            if (!first) label += L" / ";
            label += DisplayPriceSource(source);
            first = false;
        }
    }
    if (total.unknown) label += Tr(L" · 含未定价用量");
    return label;
}

} // namespace

namespace {
// Parses an ISO 8601 timestamp with Z or ±hh:mm into UTC FILETIME ticks.
bool ParseIsoTicks(const std::string& timestamp, uint64_t& utcTicks) {
    if (timestamp.size() < 20 || timestamp[4] != '-' || timestamp[7] != '-' ||
        (timestamp[10] != 'T' && timestamp[10] != 't' && timestamp[10] != ' ') || timestamp[13] != ':' ||
        timestamp[16] != ':')
        return false;
    auto number = [&](size_t offset, size_t length) {
        int result = 0;
        for (size_t index = offset; index < offset + length; ++index) {
            if (timestamp[index] < '0' || timestamp[index] > '9') return -1;
            result = result * 10 + timestamp[index] - '0';
        }
        return result;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59)
        return false;
    size_t suffix = 19;
    if (timestamp[suffix] == '.') {
        const size_t first = ++suffix;
        while (suffix < timestamp.size() && timestamp[suffix] >= '0' && timestamp[suffix] <= '9') ++suffix;
        if (suffix == first) return false;
    }
    int offsetMinutes = 0;
    if (suffix + 1 == timestamp.size() && (timestamp[suffix] == 'Z' || timestamp[suffix] == 'z')) {
    } else if (suffix + 6 == timestamp.size() && (timestamp[suffix] == '+' || timestamp[suffix] == '-') &&
               timestamp[suffix + 3] == ':') {
        const int offsetHour = number(suffix + 1, 2), offsetMinute = number(suffix + 4, 2);
        if (offsetHour < 0 || offsetHour > 14 || offsetMinute < 0 || offsetMinute > 59 ||
            (offsetHour == 14 && offsetMinute))
            return false;
        offsetMinutes = (offsetHour * 60 + offsetMinute) * (timestamp[suffix] == '+' ? 1 : -1);
    } else
        return false;
    SYSTEMTIME wall{};
    wall.wYear = static_cast<WORD>(year);
    wall.wMonth = static_cast<WORD>(month);
    wall.wDay = static_cast<WORD>(day);
    wall.wHour = static_cast<WORD>(hour);
    wall.wMinute = static_cast<WORD>(minute);
    wall.wSecond = static_cast<WORD>(second);
    FILETIME file{};
    SYSTEMTIME checked{};
    if (!SystemTimeToFileTime(&wall, &file) || !FileTimeToSystemTime(&file, &checked) || checked.wYear != wall.wYear ||
        checked.wMonth != wall.wMonth || checked.wDay != wall.wDay)
        return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    const int64_t utc = static_cast<int64_t>(ticks.QuadPart) - static_cast<int64_t>(offsetMinutes) * 600000000LL;
    if (utc < 0) return false;
    utcTicks = static_cast<uint64_t>(utc);
    return true;
}
bool TicksToSystemTime(uint64_t ticks, SYSTEMTIME& out) {
    FILETIME file{static_cast<DWORD>(ticks & 0xFFFFFFFFULL), static_cast<DWORD>(ticks >> 32)};
    return FileTimeToSystemTime(&file, &out) != FALSE;
}
bool SystemTimeToTicks(const SYSTEMTIME& time, uint64_t& ticks) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) return false;
    ticks = (static_cast<uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    return true;
}
} // namespace

std::wstring FormatUpdateTime(const std::string& timestamp, bool compact, int displayOffsetMinutes) {
    if (timestamp.empty()) return Tr(L"尚未更新");
    const auto unknown = std::wstring(Tr(L"更新时间未知"));
    uint64_t utc = 0;
    if (!ParseIsoTicks(timestamp, utc)) return unknown;
    const int64_t adjusted = static_cast<int64_t>(utc) + static_cast<int64_t>(displayOffsetMinutes) * 600000000LL;
    SYSTEMTIME shown{};
    if (adjusted < 0 || !TicksToSystemTime(static_cast<uint64_t>(adjusted), shown) || shown.wYear > 9999)
        return unknown;
    wchar_t formatted[64]{};
    if (compact)
        std::swprintf(formatted, std::size(formatted), Tr(L"%d年%d月%d日 %02d:%02d:%02d"), shown.wYear, shown.wMonth,
                      shown.wDay, shown.wHour, shown.wMinute, shown.wSecond);
    else
        std::swprintf(formatted, std::size(formatted), L"%04d-%02d-%02d %02d:%02d:%02d", shown.wYear, shown.wMonth,
                      shown.wDay, shown.wHour, shown.wMinute, shown.wSecond);
    const int magnitude = displayOffsetMinutes < 0 ? -displayOffsetMinutes : displayOffsetMinutes;
    wchar_t zone[16]{};
    std::swprintf(zone, std::size(zone), L"UTC%lc%02d:%02d", displayOffsetMinutes < 0 ? L'-' : L'+', magnitude / 60,
                  magnitude % 60);
    return Tr(L"更新于 ") + std::wstring(formatted) + L" · " + zone;
}

int LocalUtcOffsetMinutes(const std::string& timestamp) {
    // Offset of the computer's time zone at that instant (daylight saving aware), the same
    // conversion LocalDay() uses to assign usage to calendar days.
    uint64_t utc = 0;
    SYSTEMTIME utcTime{}, localTime{};
    uint64_t local = 0;
    if (!ParseIsoTicks(timestamp, utc) || !TicksToSystemTime(utc, utcTime) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime) || !SystemTimeToTicks(localTime, local))
        return 0;
    return static_cast<int>((static_cast<int64_t>(local) - static_cast<int64_t>(utc)) / 600000000LL);
}

std::wstring LocalUpdateTime(const std::string& timestamp, bool compact) {
    return FormatUpdateTime(timestamp, compact, LocalUtcOffsetMinutes(timestamp));
}

namespace {

std::wstring RowModel(const UsageRow& row) {
    return row.model.empty() ? Tr(L"未知模型") : Utf8ToWide(row.model);
}

std::wstring MonthLabel(int month) {
    static const wchar_t* const english[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                             L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (CurrentLanguage() == Language::English && month >= 1 && month <= 12) return english[month - 1];
    return std::to_wstring(month) + L"月";
}

COLORREF Blend(COLORREF first, COLORREF second, double amount) {
    auto blend = [amount](int a, int b) {
        return static_cast<int>(a + (b - a) * amount);
    };
    return RGB(blend(GetRValue(first), GetRValue(second)), blend(GetGValue(first), GetGValue(second)),
               blend(GetBValue(first), GetBValue(second)));
}

struct ImageData {
    HBITMAP bitmap = nullptr;
    int width = 0, height = 0;
};
struct Images {
    std::map<int, ImageData> values;
    ~Images() {
        for (auto& image : values)
            if (image.second.bitmap) DeleteObject(image.second.bitmap);
    }
    ImageData Load(int id) {
        auto found = values.find(id);
        if (found != values.end()) return found->second;
        ImageData data;
        HRSRC resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource) {
            values[id] = data;
            return data;
        }
        HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
        DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
        BYTE* bytes = static_cast<BYTE*>(LockResource(loaded));
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                      reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(bytes, size);
        if (SUCCEEDED(hr))
            hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
            hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
        UINT width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
        if (SUCCEEDED(hr) && width && height && width <= 2048 && height <= 2048) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(width);
            info.bmiHeader.biHeight = -static_cast<LONG>(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* destination = nullptr;
            data.bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &destination, nullptr, 0);
            if (data.bitmap && SUCCEEDED(converter->CopyPixels(nullptr, width * 4, width * height * 4,
                                                               static_cast<BYTE*>(destination)))) {
                data.width = static_cast<int>(width);
                data.height = static_cast<int>(height);
            } else if (data.bitmap) {
                DeleteObject(data.bitmap);
                data.bitmap = nullptr;
            }
        }
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        values[id] = data;
        return data;
    }
};

Images brandImages;

int BrandId(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (lower.find("codex") != std::string::npos) return 201;
    if (lower.find("claude") != std::string::npos) return 202;
    if (lower.find("trae") != std::string::npos) return 203;
    if (lower.find("workbuddy") != std::string::npos) return 204;
    if (lower.find("deepseek") != std::string::npos) return 205;
    if (lower.find("opencodereview") != std::string::npos || lower.find("open code review") != std::string::npos)
        return 228;
    if (lower.find("opencode") != std::string::npos) return 206;
    if (lower.find("cursor") != std::string::npos) return 207;
    if (lower.find("copilot") != std::string::npos) return 208;
    if (lower.find("codebuddy") != std::string::npos) return 209;
    if (lower.find("gemini") != std::string::npos) return 210;
    if (lower.find("qwen") != std::string::npos) return 211;
    if (lower.find("kimi") != std::string::npos) return 212;
    if (lower.find("chatgpt") != std::string::npos) return 201;
    if (lower == "amp") return 213;
    if (lower.find("antigravity") != std::string::npos) return 214;
    if (lower == "cline") return 215;
    if (lower.find("roo code") != std::string::npos || lower.find("roo-code") != std::string::npos) return 216;
    if (lower.find("kilo") != std::string::npos) return 217;
    if (lower.find("mimo") != std::string::npos) return 218;
    if (lower.find("codebuff") != std::string::npos) return 219;
    if (lower.find("commandcode") != std::string::npos) return 220;
    if (lower == "pi" || lower == "oh my pi" || lower == "omp") return 221;
    if (lower.find("openclaw") != std::string::npos) return 222;
    if (lower.find("junie") != std::string::npos) return 223;
    if (lower.find("grok") != std::string::npos) return 224;
    if (lower == "jcode") return 225;
    if (lower.find("devin") != std::string::npos) return 226;
    if (lower == "zcode") return 227;
    if (lower == "crush") return 229;
    if (lower == "droid") return 230;
    if (lower == "fx") return 231;
    if (lower == "goose") return 232;
    if (lower.find("hermes") != std::string::npos) return 233;
    if (lower == "kiro") return 234;
    if (lower == "mux") return 235;
    if (lower == "orca") return 236;
    if (lower == "warp") return 237;
    if (lower.find("zed") == 0) return 238;
    return 0;
}

class Canvas {
public:
    Canvas(HDC context, ViewState& view, const RECT& client) : dc(context), state(view) {
        dpi = std::max(0.5f, state.dpi);
        width = static_cast<int>((client.right - client.left) / dpi);
        height = static_cast<int>((client.bottom - client.top) / dpi);
        saved = SaveDC(dc);
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, 1000, 1000, nullptr);
        SetViewportExtEx(dc, static_cast<int>(1000 * dpi), static_cast<int>(1000 * dpi), nullptr);
        SetBkMode(dc, TRANSPARENT);
        state.hits.clear();
    }
    ~Canvas() {
        RestoreDC(dc, saved);
        for (auto& font : fonts) DeleteObject(font.second);
    }
    void Rect(int x, int y, int w, int h, COLORREF color, COLORREF border = CLR_INVALID, int radius = 0) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? color : border);
        HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
        if (radius)
            RoundRect(dc, x, y, x + w, y + h, radius * 2, radius * 2);
        else
            Rectangle(dc, x, y, x + w, y + h);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
    void Text(const std::wstring& value, int x, int y, int w, int h = 24, int size = 14, COLORREF color = Ink,
              bool bold = false, UINT align = DT_LEFT, bool wrap = false) {
        const int key = size * 2 + (bold ? 1 : 0);
        auto found = fonts.find(key);
        if (found == fonts.end()) {
            HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
            found = fonts.emplace(key, font).first;
        }
        HGDIOBJ oldFont = SelectObject(dc, found->second);
        SetTextColor(dc, color);
        RECT rect{x, y, x + w, y + h};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect,
                  align | (wrap ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER) | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }
    void Rule(int x, int y, int w, COLORREF color = Line) { Rect(x, y, w, 1, color); }
    void Hit(int x, int y, int w, int h, int command, const std::string& value = "") {
        RECT target{static_cast<LONG>(std::lround(x * dpi)), static_cast<LONG>(std::lround(y * dpi)),
                    static_cast<LONG>(std::lround((x + w) * dpi)), static_cast<LONG>(std::lround((y + h) * dpi))};
        if (bodyClip) {
            target.top = std::max(target.top, static_cast<LONG>(std::lround(80 * dpi)));
            target.left = std::max(target.left, static_cast<LONG>(std::lround(sidebar * dpi)));
        }
        target.bottom = std::min(target.bottom, static_cast<LONG>(std::lround(height * dpi)));
        if (target.bottom > target.top && target.right > target.left) state.hits.push_back({target, command, value});
    }
    void Button(const std::wstring& label, int x, int y, int w, int command, bool active = false, int h = 34,
                const std::string& value = "") {
        Rect(x, y, w, h, active ? Soft : Surface, active ? Accent : Line, 7);
        Text(label, x + 8, y, w - 16, h, 13, active ? Accent : Ink, active, DT_CENTER);
        Hit(x, y, w, h, command, value);
    }
    void Brand(const std::string& name, int x, int y, int size = 28) {
        const auto image = brandImages.Load(BrandId(name));
        if (image.bitmap) {
            if (state.prefs.darkMode) {
                // Several brand marks are black; give them a light tile so they stay visible.
                Rect(x - 2, y - 2, size + 4, size + 4, RGB(236, 239, 244), CLR_INVALID, 6);
            }
            HDC memory = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(memory, image.bitmap);
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, x, y, size, size, memory, 0, 0, image.width, image.height, blend);
            SelectObject(memory, old);
            DeleteDC(memory);
        } else {
            Rect(x, y, size, size, Soft, Line, 6);
            const int gap = std::max(2, size / 12), tile = (size - gap * 3) / 2;
            Rect(x + gap, y + gap, tile, tile, Accent, CLR_INVALID, 2);
            Rect(x + gap * 2 + tile, y + gap, tile, tile, Blend(Accent, Surface, .4), CLR_INVALID, 2);
            Rect(x + gap, y + gap * 2 + tile, tile, tile, Blend(Accent, Surface, .4), CLR_INVALID, 2);
            Rect(x + gap * 2 + tile, y + gap * 2 + tile, tile, tile, Accent, CLR_INVALID, 2);
        }
    }
    HDC dc;
    ViewState& state;
    int width = 0, height = 0, sidebar = 174;
    float dpi = 1;
    bool bodyClip = false;

private:
    int saved = 0;
    std::map<int, HFONT> fonts;
};

std::map<std::string, std::vector<UsageRow>> GroupSoftware(const std::vector<UsageRow>& rows) {
    std::map<std::string, std::vector<UsageRow>> groups;
    for (const auto& row : rows)
        if (row.total() > 0) groups[row.software].push_back(row);
    return groups;
}

std::vector<std::pair<std::string, std::vector<UsageRow>>>
SortGroups(const std::map<std::string, std::vector<UsageRow>>& groups) {
    std::vector<std::pair<std::string, std::vector<UsageRow>>> result(groups.begin(), groups.end());
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return Sum(a.second).tokens > Sum(b.second).tokens; });
    return result;
}

std::wstring SelectionLabel(const ViewState& state) {
    return state.selectedDay.empty() ? RangeName(state.prefs.range) + Tr(L"汇总")
                                     : Utf8ToWide(state.selectedDay) + Tr(L" · 单日");
}

void ShareBar(Canvas& c, int x, int y, int w, int64_t value, int64_t total, COLORREF color = Accent) {
    c.Rect(x, y, w, 3, Line, CLR_INVALID, 1);
    if (value > 0 && total > 0)
        c.Rect(x, y, std::min(w, std::max(2, static_cast<int>(w * static_cast<double>(value) / total))), 3, color,
               CLR_INVALID, 1);
}

int DrawBreakdown(Canvas& c, int x, int y, int w, const Totals& total) {
    // Cached input and reasoning are subsets. The bar uses disjoint buckets so
    // it always represents input + output + unclassified exactly once.
    struct Part {
        const wchar_t* name;
        int64_t value;
        COLORREF color;
    };
    const Part parts[] = {
        {Tr(L"普通输入"), std::max<int64_t>(0, total.input - total.cached - total.cacheWrite), RGB(71, 120, 221)},
        {Tr(L"普通输出"), std::max<int64_t>(0, total.output - total.reasoning), RGB(145, 100, 214)},
        {Tr(L"缓存读取"), total.cached, RGB(42, 157, 122)},
        {Tr(L"缓存写入"), total.cacheWrite, RGB(216, 159, 52)},
        {Tr(L"推理"), total.reasoning, RGB(204, 99, 146)},
        {Tr(L"未分类"), total.unclassified, Muted}};
    int64_t cumulative = 0;
    int previous = 0, count = 0;
    for (const auto& part : parts)
        if (part.value > 0) ++count;
    c.Rect(x, y, w, 7, Line, CLR_INVALID, 2);
    for (const auto& part : parts)
        if (part.value > 0) {
            cumulative += part.value;
            const int end = static_cast<int>(w * static_cast<double>(cumulative) / std::max<int64_t>(1, total.tokens));
            if (end > previous) c.Rect(x + previous, y, end - previous, 7, part.color);
            previous = end;
        }
    int index = 0;
    for (const auto& part : parts)
        if (part.value > 0) {
            const int column = w / std::max(1, count), left = x + index * column;
            c.Rect(left, y + 19, 7, 7, part.color, CLR_INVALID, 2);
            c.Text(std::wstring(part.name) + L" " + FormatCount(part.value, true), left + 13, y + 12, column - 16, 22,
                   10, Muted);
            ++index;
        }
    return y + 47;
}

int DrawSoftware(Canvas& c, int x, int y, int w, const std::vector<UsageRow>& rows, bool modelPage = false) {
    auto groups = GroupSoftware(rows);
    const auto selectedTotal = Sum(rows);
    c.Text(modelPage ? Tr(L"模型用量") : Tr(L"软件用量"), x, y, w - 200, 30, 19, Ink, true);
    y += 34;
    c.Text(SelectionLabel(c.state) + L" · " + std::to_wstring(groups.size()) + Tr(L" 个有用量软件") +
               (selectedTotal.input ? Tr(L" · 缓存读取占输入 ") + Percent(selectedTotal.cached, selectedTotal.input)
                                    : L""),
           x, y, w - 180, 24, 12, Muted);
    if (!c.state.selectedDay.empty())
        c.Button(Tr(L"返回") + RangeName(c.state.prefs.range) + Tr(L"汇总"), x + w - 148, y - 4, 148, CmdClearDay);
    y += 38;
    if (rows.empty()) {
        c.Rect(x, y, w, 148, Surface, Line, 10);
        c.Text(Tr(L"所选时段暂无已采集用量"), x + 20, y + 24, w - 40, 28, 17, Ink, true, DT_CENTER);
        c.Text(Tr(L"仅展示可获取的真实记录；未采集日期的消耗未知。"), x + 20, y + 58, w - 40, 24, 12, Muted, false,
               DT_CENTER);
        c.Button(Tr(L"管理数据来源"), x + (w - 140) / 2, y + 98, 140, CmdSettings);
        return y + 168;
    }
    y = DrawBreakdown(c, x, y, w, selectedTotal);
    if (modelPage) {
        std::map<std::string, std::vector<UsageRow>> models;
        for (const auto& row : rows) {
            // Without a recorded provider, a same-named model from another app may be a different endpoint.
            const std::string identity = row.provider.empty() ? "unknown-provider:" + row.software : row.provider;
            models[identity + "\n" + row.model].push_back(row);
        }
        for (const auto& group : SortGroups(models)) {
            auto total = Sum(group.second);
            int h = 104 + static_cast<int>(GroupSoftware(group.second).size()) * 47;
            c.Rect(x, y, w, h, Surface, Line, 10);
            c.Text(RowModel(group.second.front()), x + 18, y + 10, w - 300, 28, 14, Ink, true);
            std::wstring provider = Utf8ToWide(group.second.front().provider);
            if (provider.empty()) provider = Tr(L"提供商未记录");
            c.Text(provider, x + 18, y + 37, w - 300, 20, 11, Muted);
            c.Text(FormatCount(total.tokens) + L" Token", x + w - 265, y + 10, 245, 24, 13, Ink, true, DT_RIGHT);
            c.Text(CostText(total, c.state.prefs), x + w - 265, y + 35, 245, 22, 12, Muted, false, DT_RIGHT);
            const int priceWidth = std::min(445, w - 220);
            c.Text(PriceLabel(total, true), x + w - priceWidth - 20, y + 57, priceWidth, 20, 10, Muted, false,
                   DT_RIGHT);
            c.Text(Tr(L"占所选用量 ") + Percent(total.tokens, selectedTotal.tokens), x + 18, y + 64, 170, 20, 10,
                   Muted);
            ShareBar(c, x + 188, y + 84, w - 208, total.tokens, selectedTotal.tokens);
            c.Rule(x + 18, y + 98, w - 36);
            int rowY = y + 105;
            for (const auto& software : SortGroups(GroupSoftware(group.second))) {
                const auto subtotal = Sum(software.second);
                c.Brand(software.first, x + 22, rowY + 5, 22);
                c.Text(Utf8ToWide(software.first), x + 54, rowY, w - 320, 34, 12);
                c.Text(FormatCount(subtotal.tokens) + L" Token · " + Percent(subtotal.tokens, total.tokens),
                       x + w - 300, rowY, 280, 34, 12, Muted, false, DT_RIGHT);
                ShareBar(c, x + 54, rowY + 37, w - 74, subtotal.tokens, total.tokens);
                rowY += 47;
            }
            y += h + 12;
        }
        return y + 12;
    }
    for (const auto& group : SortGroups(groups)) {
        auto total = Sum(group.second);
        std::map<std::string, std::vector<UsageRow>> models;
        for (const auto& row : group.second) models[row.provider + "\n" + row.model].push_back(row);
        int h = 96 + static_cast<int>(models.size()) * 62;
        c.Rect(x, y, w, h, Surface, Line, 10);
        c.Brand(group.first, x + 18, y + 18, 30);
        c.Text(Utf8ToWide(group.first), x + 60, y + 10, w - 340, 28, 15, Ink, true);
        c.Text(std::to_wstring(models.size()) + Tr(L" 个模型 · ") + FormatCount(total.requests) + Tr(L" 条记录") +
                   (total.input ? Tr(L" · 缓存 ") + Percent(total.cached, total.input) : L""),
               x + 60, y + 38, w - 340, 20, 11, Muted);
        c.Text(FormatCount(total.tokens) + L" Token", x + w - 265, y + 11, 245, 26, 15, Ink, true, DT_RIGHT);
        c.Text(CostText(total, c.state.prefs), x + w - 265, y + 39, 245, 21, 12, Muted, false, DT_RIGHT);
        const int priceWidth = std::min(445, w - 260);
        c.Text(PriceLabel(total, true), x + w - priceWidth - 20, y + 61, priceWidth, 20, 10, Muted, false, DT_RIGHT);
        c.Text(Tr(L"占所选用量 ") + Percent(total.tokens, selectedTotal.tokens), x + 60, y + 63, 170, 20, 10, Muted);
        ShareBar(c, x + 60, y + 86, w - 80, total.tokens, selectedTotal.tokens);
        int rowY = y + 96;
        for (const auto& model : SortGroups(models)) {
            const auto& row = model.second.front();
            auto subtotal = Sum(model.second);
            c.Text(RowModel(row), x + 60, rowY, w - 370, 24, 13);
            c.Text(Utf8ToWide(row.provider.empty() ? Tr8("提供商未记录") : row.provider), x + 60, rowY + 23,
                   (w - 100) / 3, 18, 10, Muted);
            c.Text(FormatCount(subtotal.tokens) + L" Token", x + w - 290, rowY, 135, 24, 13, Ink, false, DT_RIGHT);
            c.Text(CostText(subtotal, c.state.prefs), x + w - 148, rowY, 128, 24, 12, Muted, false, DT_RIGHT);
            const int detailWidth = std::min(435, w - (w - 100) / 3 - 100);
            c.Text(Percent(subtotal.tokens, total.tokens) + L" · " + PriceLabel(subtotal, true),
                   x + w - detailWidth - 20, rowY + 23, detailWidth, 18, 10, Muted, false, DT_RIGHT);
            ShareBar(c, x + 60, rowY + 48, w - 80, subtotal.tokens, total.tokens, Blend(Accent, Surface, .2));
            rowY += 62;
        }
        y += h + 12;
    }
    return y + 8;
}

std::map<std::string, Totals> DailyTotals(const Snapshot& snapshot) {
    std::map<std::string, std::vector<UsageRow>> rows;
    for (const auto& row : snapshot.rows) rows[row.day].push_back(row);
    std::map<std::string, Totals> result;
    for (const auto& day : rows) result[day.first] = Sum(day.second);
    return result;
}

double Metric(const Totals& value, const ViewState& state) {
    if (!state.costMetric) return static_cast<double>(value.tokens);
    if (!value.cost) return -1;
    const double rate = EffectiveUsdToCny(state.prefs);
    if (state.prefs.cny && rate <= 0) return -1;
    return *value.cost / 1000000.0 * (state.prefs.cny ? rate : 1);
}

int MondayWeekday(const std::string& day) {
    if (day.size() != 10) return 0;
    SYSTEMTIME date{};
    date.wYear = static_cast<WORD>(std::stoi(day.substr(0, 4)));
    date.wMonth = static_cast<WORD>(std::stoi(day.substr(5, 2)));
    date.wDay = static_cast<WORD>(std::stoi(day.substr(8, 2)));
    FILETIME file{};
    if (!SystemTimeToFileTime(&date, &file)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = file.dwLowDateTime;
    value.HighPart = file.dwHighDateTime;
    // FILETIME starts on Monday, 1601-01-01; this calculation uses calendar dates only.
    return static_cast<int>((value.QuadPart / 864000000000ULL) % 7);
}

int DrawChart(Canvas& c, int x, int y, int w) {
    auto& state = c.state;
    const auto daily = DailyTotals(state.snapshot);
    const std::string today = LocalToday();
    const bool heat = state.chart == Chart::Heatmap;
    // Selection filters the lists only. Trend navigation owns chartEnd; the
    // heatmap follows today. Both charts keep their date geometry on a click.
    const std::string end = heat || state.chartEnd.empty() ? today : state.chartEnd;
    const int h = heat ? 300 : 314;
    c.Rect(x, y, w, h, Surface, Line, 10);
    c.Button(Tr(L"趋势图"), x + 18, y + 17, 84, CmdTrend, !heat);
    c.Button(Tr(L"热力图"), x + 110, y + 17, 84, CmdHeatmap, heat);
    c.Button(L"Token", x + w - 156, y + 17, 65, CmdTokens, !state.costMetric);
    c.Button(Tr(L"费用"), x + w - 83, y + 17, 65, CmdCost, state.costMetric);
    std::string start;
    if (!heat) {
        start = state.trendMonth ? end.substr(0, 7) + "-01" : ShiftDay(end, -6);
        c.Button(Tr(L"近 7 天"), x + 18, y + 62, 76, CmdTrendWeek, !state.trendMonth, 28);
        c.Button(Tr(L"月"), x + 102, y + 62, 58, CmdTrendMonth, state.trendMonth, 28);
        c.Button(L"‹", x + w - 84, y + 62, 28, CmdPrevious, false, 28);
        c.Button(L"›", x + w - 46, y + 62, 28, CmdNext, false, 28);
        c.Text(Utf8ToWide(start + " — " + end), x + 172, y + 62, w - 270, 28, 11, Muted);
        std::vector<std::string> dates;
        for (auto day = start; day <= end && dates.size() < 32; day = ShiftDay(day, 1)) dates.push_back(day);
        const int plotX = x + 50, plotY = y + 110, plotW = w - 74, plotH = 130;
        double maxValue = 0;
        bool hasKnown = false;
        for (const auto& day : dates) {
            auto it = daily.find(day);
            if (it != daily.end() && Metric(it->second, state) >= 0) {
                maxValue = std::max(maxValue, Metric(it->second, state));
                hasKnown = true;
            }
        }
        if (maxValue <= 0) maxValue = 1;
        for (int n = 0; n <= 3; ++n) c.Rule(plotX, plotY + n * plotH / 3, plotW, Grid);
        std::wstring maximum;
        if (state.costMetric && !hasKnown)
            maximum = Tr(L"未知");
        else if (state.costMetric) {
            std::wostringstream out;
            out << std::fixed << std::setprecision(maxValue < 1 ? 2 : 1) << maxValue;
            maximum = out.str();
        } else
            maximum = FormatCount(static_cast<int64_t>(maxValue), true);
        c.Text(maximum, x + 6, plotY - 10, 39, 20, 10, Muted, false, DT_RIGHT);
        c.Text(L"0", x + 6, plotY + plotH - 10, 39, 20, 10, Muted, false, DT_RIGHT);
        const double step = dates.empty() ? plotW : static_cast<double>(plotW) / dates.size();
        const int labelEvery = dates.size() > 12 ? 5 : 1;
        for (size_t index = 0; index < dates.size(); ++index) {
            auto found = daily.find(dates[index]);
            const double value = found == daily.end() ? -1 : Metric(found->second, state);
            const int barX = plotX + static_cast<int>(index * step) + 4;
            const int barW = std::max(3, static_cast<int>(step) - 8);
            if (value >= 0) {
                const int barH = value > 0 ? std::max(2, static_cast<int>(plotH * value / maxValue)) : 2;
                c.Rect(barX, plotY + plotH - barH, barW, barH, state.prefs.trendColor, CLR_INVALID, 3);
            } else
                c.Rect(barX, plotY + plotH - 2, barW, 2, Line);
            if (dates[index] == state.selectedDay) c.Rect(barX - 2, plotY - 3, barW + 4, 3, Ink);
            c.Hit(plotX + static_cast<int>(index * step), plotY - 3, static_cast<int>(step), plotH + 30, CmdSelectDay,
                  dates[index]);
            if (index % labelEvery == 0 || index + 1 == dates.size())
                c.Text(Utf8ToWide(dates[index].substr(5)), barX - 10, plotY + plotH + 5, barW + 20, 21, 10, Muted,
                       false, DT_CENTER);
        }
    } else {
        const int gridX = x + 44, usable = std::max(140, w - 64);
        const int columns = std::max(10, std::min(53, usable / 14));
        const auto calendar = MakeHeatmapCalendar(today, columns);
        start = calendar.first;
        const std::string& gridStart = calendar.gridFirst;
        c.Text(Utf8ToWide(start + " — " + end) + L" · " + std::to_wstring(calendar.days) + Tr(L" 天"), x + 18, y + 59,
               w - 36, 25, 11, Muted);
        const int gridY = y + 116;
        c.Text(Tr(L"一"), x + 17, gridY - 4, 20, 20, 10, Muted);
        c.Text(Tr(L"三"), x + 17, gridY + 24, 20, 20, 10, Muted);
        c.Text(Tr(L"五"), x + 17, gridY + 52, 20, 20, 10, Muted);
        std::vector<double> positive;
        for (const auto& day : daily)
            if (day.first >= start && day.first <= end && Metric(day.second, state) > 0)
                positive.push_back(Metric(day.second, state));
        std::sort(positive.begin(), positive.end());
        const double spacing = columns > 1 ? static_cast<double>(usable - 11) / (columns - 1) : 14;
        int lastMonth = -1;
        for (int index = 0; index < columns * 7; ++index) {
            const auto day = ShiftDay(gridStart, index);
            const int column = index / 7, row = index % 7;
            const int cellX = gridX + static_cast<int>(column * spacing), cellY = gridY + row * 14;
            if (day > end) continue; // A new day adds one cell; future days stay empty.
            const int month = std::stoi(day.substr(5, 2));
            if (row == 0 && month != lastMonth) {
                c.Text(MonthLabel(month), cellX, gridY - 25, 44, 20, 10, Muted);
                lastMonth = month;
            }
            auto found = daily.find(day);
            COLORREF color = HeatOutside;
            if (found != daily.end() && day >= start && day <= end) {
                double value = Metric(found->second, state);
                if (value >= 0) {
                    int level = 0;
                    if (value > 0) {
                        level = 1;
                        for (int quarter = 1; quarter <= 3; ++quarter)
                            if (!positive.empty() && value >= positive[(positive.size() - 1) * quarter / 4]) ++level;
                    }
                    color = level ? Blend(Surface, state.prefs.heatColor, .2 + level * .2) : HeatEmpty;
                }
            }
            c.Rect(cellX, cellY, 11, 11, color, day == state.selectedDay ? Ink : CLR_INVALID, 2);
            if (day >= start && day <= end) c.Hit(cellX - 1, cellY - 1, 14, 14, CmdSelectDay, day);
        }
        c.Text(Tr(L"空白格：未采集"), x + 18, y + 220, 180, 22, 11, Muted);
        c.Text(Tr(L"低"), x + w - 182, y + 220, 54, 22, 10, Muted, false, DT_RIGHT);
        for (int n = 0; n < 5; ++n)
            c.Rect(x + w - 124 + n * 16, y + 225, 11, 11,
                   n ? Blend(Surface, state.prefs.heatColor, .2 + n * .2) : HeatEmpty, CLR_INVALID, 2);
        c.Text(Tr(L"高"), x + w - 38, y + 220, 40, 22, 10, Muted);
    }
    {
        std::wstring hint =
            heat ? Tr(L"点击小格查看当天软件与模型") : Tr(L"点击日期查看当天软件与模型 · 灰色基线表示未采集或费用未知");
        if (!state.selectedDay.empty()) {
            auto found = daily.find(state.selectedDay);
            hint = Utf8ToWide(state.selectedDay) + L" · " +
                   (found == daily.end()
                        ? Tr(L"未采集，Token 与费用未知")
                        : FormatCount(found->second.tokens) + L" Token · " + CostText(found->second, state.prefs));
        }
        c.Text(hint, x + 18, y + h - 36, w - 36, 24, 11, Muted);
    }
    return y + h + 24;
}

int DrawSettings(Canvas& c, int x, int y, int w) {
    auto& state = c.state;
    c.Text(Tr(L"显示与采集"), x, y, w, 32, 20, Ink, true);
    y += 43;
    c.Rect(x, y, w, 386, Surface, Line, 10);
    c.Text(Tr(L"费用币种"), x + 20, y + 17, 140, 34, 14);
    c.Button(state.prefs.cny ? Tr(L"人民币 CNY") : Tr(L"美元 USD"), x + w - 164, y + 17, 144, CmdCurrency, true);
    c.Rule(x + 20, y + 64, w - 40);
    c.Text(Tr(L"美元 / 人民币汇率"), x + 20, y + 76, 180, 28, 14);
    const double effectiveRate = EffectiveUsdToCny(state.prefs);
    const bool manualRate =
        state.prefs.usdToCny > 0 && state.prefs.usdToCny < 10000 && std::isfinite(state.prefs.usdToCny);
    c.Text(manualRate ? Tr(L"手动优先；输入空值或 0 恢复自动。") : Tr(L"未设置手动汇率时，自动采用公开日价。"), x + 20,
           y + 102, w - 215, 20, 10, Muted);
    std::wstring rateDetail;
    if (state.prefs.automaticUsdToCny > 0 && state.prefs.automaticUsdToCny < 10000 &&
        std::isfinite(state.prefs.automaticUsdToCny)) {
        rateDetail +=
            Tr(L"自动参考") +
            (state.prefs.exchangeRateDate.empty() ? std::wstring() : L" · " + Utf8ToWide(state.prefs.exchangeRateDate));
        if (!state.prefs.exchangeRateSource.empty()) rateDetail += L" · " + Utf8ToWide(state.prefs.exchangeRateSource);
    } else
        rateDetail = Tr(L"自动参考 · 暂未取得公开日价");
    c.Text(rateDetail, x + 20, y + 123, w - 40, 20, 10, Muted);
    c.Text(state.prefs.exchangeRateStatus.empty()
               ? (effectiveRate > 0 ? Tr(L"仅用于显示换算，不改写 USD 计费记录。")
                                    : Tr(L"自动汇率暂不可用；联网取得后才换算人民币费用。"))
               : Utf8ToWide(state.prefs.exchangeRateStatus),
           x + 20, y + 144, w - 40, 20, 10, Muted);
    std::wostringstream rate;
    rate << (manualRate ? Tr(L"手动 ") : Tr(L"自动 ")) << std::fixed << std::setprecision(4) << effectiveRate;
    c.Button(effectiveRate > 0 ? rate.str() : Tr(L"自动 · 暂不可用"), x + w - 164, y + 82, 144, CmdExchangeRate);
    c.Rule(x + 20, y + 169, w - 40);
    c.Text(Tr(L"自动刷新本地用量"), x + 20, y + 182, w - 210, 30, 14);
    c.Button(state.prefs.autoRefresh ? Tr(L"已开启") : Tr(L"已关闭"), x + w - 164, y + 182, 144, CmdAutoRefresh,
             state.prefs.autoRefresh);
    c.Text(Tr(L"本地来源有变化时立即读取；每 5 分钟兜底检查一次。"), x + 20, y + 216, w - 210, 20, 10, Muted);
    c.Rule(x + 20, y + 244, w - 40);
    c.Text(Tr(L"外观"), x + 20, y + 257, w - 210, 30, 14);
    c.Button(state.prefs.theme == 1   ? Tr(L"浅色")
             : state.prefs.theme == 2 ? Tr(L"深色")
                                      : Tr(L"跟随系统"),
             x + w - 164, y + 257, 144, CmdTheme);
    c.Rule(x + 20, y + 303, w - 40);
    c.Text(Tr(L"界面语言"), x + 20, y + 316, w - 210, 30, 14);
    c.Button(state.prefs.language == 1   ? Tr(L"中文")
             : state.prefs.language == 2 ? L"English"
                                         : Tr(L"跟随系统"),
             x + w - 164, y + 316, 144, CmdLanguage);
    c.Text(Tr(L"用量只从本地记录或已授权接口读取，不上传对话。"), x + 20, y + 356, w - 40, 22, 11, Muted);
    y += 406;
    c.Text(Tr(L"预算提醒"), x, y, w, 30, 18, Ink, true);
    y += 40;
    c.Rect(x, y, w, 128, Surface, Line, 10);
    const bool budgetSet = state.prefs.dailyBudget > 0 || state.prefs.monthlyBudget > 0;
    c.Text(Tr(L"每日 / 每月预估费用上限"), x + 20, y + 15, w - 210, 30, 14);
    c.Button(budgetSet ? Tr(L"修改预算") : Tr(L"设置预算"), x + w - 164, y + 15, 144, CmdBudget);
    std::wstring budgetLine;
    if (!budgetSet)
        budgetLine = Tr(L"尚未设置；达到 80% 和 100% 时在托盘提醒。");
    else {
        const auto budget = ComputeBudget(state.snapshot, state.prefs);
        const wchar_t* symbol = state.prefs.budgetCny ? L"¥" : L"$";
        auto money = [&](double value) {
            std::wostringstream out;
            out << symbol << std::fixed << std::setprecision(2) << value;
            return out.str();
        };
        if (!budget.available)
            budgetLine = Tr(L"人民币预算需要汇率；取得汇率后开始提醒。");
        else {
            if (state.prefs.dailyBudget > 0)
                budgetLine += TrF(L"今日 {0} / {1}", money(budget.today), money(state.prefs.dailyBudget));
            if (state.prefs.monthlyBudget > 0)
                budgetLine += (budgetLine.empty() ? L"" : L" · ") +
                              TrF(L"本月 {0} / {1}", money(budget.month), money(state.prefs.monthlyBudget));
        }
    }
    c.Text(budgetLine, x + 20, y + 60, w - 40, 24, 12, Ink);
    c.Text(Tr(L"按标准 API 价格估算，未定价用量不计入；不是订阅账单。"), x + 20, y + 90, w - 40, 22, 10, Muted);
    y += 148;
    c.Text(Tr(L"参考价目与费用估算"), x, y, w, 30, 18, Ink, true);
    y += 40;
    c.Rect(x, y, w, 190, Surface, Line, 10);
    c.Text(Tr(L"按实际记录的模型与 Token 分桶估算 API 费用。"), x + 20, y + 13, w - 40, 26, 12, Muted);
    c.Button(Tr(L"更新参考价目"), x + 20, y + 49, 144, CmdOnlineCheck);
    c.Button(Tr(L"手动模型价格"), x + 176, y + 49, 144, CmdPricing);
    c.Text(state.snapshot.pricingStatus.empty() ? Tr(L"尚未加载参考价目；未匹配模型保持未定价。")
                                                : Utf8ToWide(state.snapshot.pricingStatus),
           x + 20, y + 96, w - 40, 36, 11, Muted, false, DT_LEFT, true);
    if (!state.onlineStatus.empty())
        c.Text(state.onlineStatus, x + 336, y + 46, w - 356, 42, 11, Muted, false, DT_LEFT, true);
    c.Text(Tr(L"参考源：LiteLLM / models.dev / OpenRouter。手动费率优先用于尚未定价记录。"), x + 20, y + 137, w - 40,
           21, 10, Muted);
    c.Text(Tr(L"已定价历史保持原费率；估算不等同于订阅账单或实际付款。"), x + 20, y + 160, w - 40, 20, 10, Muted);
    y += 210;
    c.Text(Tr(L"图表颜色"), x, y, w, 30, 18, Ink, true);
    y += 40;
    c.Rect(x, y, w, 104, Surface, Line, 10);
    c.Rect(x + 20, y + 25, 20, 20, state.prefs.trendColor, CLR_INVALID, 5);
    c.Text(Tr(L"趋势图"), x + 52, y + 16, 94, 36, 13);
    c.Button(Tr(L"选择颜色"), x + 150, y + 17, 100, CmdTrendColor);
    c.Rect(x + 274, y + 25, 20, 20, state.prefs.heatColor, CLR_INVALID, 5);
    c.Text(Tr(L"热力图"), x + 306, y + 16, 94, 36, 13);
    c.Button(Tr(L"选择颜色"), x + 400, y + 17, 100, CmdHeatColor);
    c.Button(Tr(L"恢复默认"), x + 20, y + 61, 110, CmdResetColors, false, 28);
    y += 124;
    c.Text(Tr(L"数据与来源"), x, y, w, 30, 18, Ink, true);
    y += 40;
    c.Rect(x, y, w, 148, Surface, Line, 10);
    c.Text(Tr(L"自动发现可读取的本地记录；有真实用量的软件才进入统计列表。"), x + 20, y + 14, w - 40, 27, 12, Muted);
    c.Button(Tr(L"打开数据目录"), x + 20, y + 52, 140, CmdOpenData);
    c.Button(Tr(L"导出 CSV"), x + 172, y + 52, 118, CmdExport);
    c.Button(Tr(L"添加 / 导入来源"), x + 20, y + 99, 176, CmdImportSource);
    c.Text(Tr(L"选择软件与本地文件或文件夹"), x + 212, y + 99, w - 232, 34, 11, Muted);
    y += 168;
    c.Text(Tr(L"来源状态") + (state.snapshot.sources.empty()
                                  ? std::wstring()
                                  : L" · " + std::to_wstring(state.snapshot.sources.size()) + Tr(L" 项")),
           x, y, w, 30, 17, Ink, true);
    y += 38;
    for (const auto& source : state.snapshot.sources) {
        const bool connected = source.canAuthorize && source.connectionState == "connected";
        const bool connectionInfo = source.canAuthorize || !source.connectionDetail.empty();
        const int pathRows = std::max(1, static_cast<int>(source.paths.size()));
        const int h = 156 + pathRows * 26 + (connectionInfo ? 40 : 0);
        c.Rect(x, y, w, h, Surface, Line, 8);
        c.Brand(source.name, x + 16, y + 17, 26);
        c.Text(Utf8ToWide(source.name), x + 54, y + 8, w - 250, 27, 13, Ink, true);
        c.Text(SourceState(source.status), x + w - 190, y + 8, 172, 27, 12, Muted, false, DT_RIGHT);
        c.Text(Utf8ToWide(source.detail), x + 54, y + 37, w - 72, 22, 11, Muted);
        c.Text(std::to_wstring(source.records) + Tr(L" 条记录 · ") + std::to_wstring(source.files) + Tr(L" 个文件"),
               x + 54, y + 60, w - 72, 18, 10, Muted);
        c.Text(source.customPath      ? Tr(L"自定义本地路径 · 点击查看或更改")
               : source.paths.empty() ? Tr(L"本地用量路径 · 点击配置")
                                      : Tr(L"默认候选路径 · 点击查看或更改"),
               x + 20, y + 82, w - 40, 20, 11, Muted);
        int rowY = y + 104;
        if (source.paths.empty()) {
            c.Text(source.path.empty() ? Tr(L"未发现") : Tr(L"待核实"), x + 20, rowY, 70, 22, 10, Muted);
            c.Text(source.path.empty() ? Tr(L"尚无本地路径，可选择真实用量文件或文件夹") : source.path, x + 94, rowY,
                   w - 114, 22, 11, Muted);
            c.Hit(x + 18, rowY, w - 36, 24, CmdSourcePath, source.id);
            rowY += 26;
        } else
            for (const auto& path : source.paths) {
                c.Text(path.exists ? Tr(L"已发现") : Tr(L"未发现"), x + 20, rowY, 70, 22, 10,
                       path.exists ? Good : Muted);
                c.Text(path.path, x + 94, rowY, w - 114, 22, 11, path.exists ? Ink : Muted);
                c.Hit(x + 18, rowY, w - 36, 24, CmdSourcePath, source.id);
                rowY += 26;
            }
        rowY += 8;
        if (connectionInfo) {
            std::wstring label;
            if (source.canAuthorize) {
                label = connected                             ? Tr(L"已连接")
                        : source.connectionState == "syncing" ? Tr(L"正在同步…")
                        : source.connectionState == "saved"   ? Tr(L"凭据已保存 · 待验证")
                        : source.connectionState == "expired" ? Tr(L"授权已过期")
                        : source.connectionState == "error"   ? Tr(L"同步失败")
                                                              : Tr(L"尚未连接");
            }
            if (!source.connectionDetail.empty()) label += (label.empty() ? L"" : L" · ") + source.connectionDetail;
            c.Text(label, x + 20, rowY, w - 40, 34, 11, connected ? Good : Muted, false, DT_LEFT, true);
            rowY += 40;
        }
        c.Button(Tr(L"更改路径"), x + 20, rowY, 100, CmdSourcePath, false, 30, source.id);
        c.Button(Tr(L"恢复默认"), x + 132, rowY, 100, CmdSourceReset, false, 30, source.id);
        if (source.canAuthorize) {
            if (source.hasCredentials) {
                c.Button(source.connectionState == "syncing" ? Tr(L"同步中…") : Tr(L"同步用量"), x + 244, rowY, 84,
                         CmdSourceSync, false, 30, source.id);
                c.Button(Tr(L"解除连接"), x + 340, rowY, 84, CmdSourceDisconnect, false, 30, source.id);
                if (source.connectionState == "expired" || source.connectionState == "error")
                    c.Button(Tr(L"重新授权"), x + 436, rowY, 120, CmdSourceConnect, false, 30, source.id);
            } else
                c.Button(Tr(L"授权连接"), x + 244, rowY, 120, CmdSourceConnect, false, 30, source.id);
        } else {
            c.Button(Tr(L"接入说明"), x + 244, rowY, 120, CmdSourceHelp, false, 30, source.id);
        }
        y += h + 10;
    }
    if (state.snapshot.sources.empty()) {
        c.Text(Tr(L"尚未读取来源状态，请刷新。"), x, y, w, 32, 12, Muted);
        y += 42;
    }
    return y + 20;
}

} // namespace

HeatmapCalendar MakeHeatmapCalendar(const std::string& today, int columns) {
    HeatmapCalendar calendar;
    calendar.columns = std::max(1, std::min(53, columns));
    calendar.last = today;
    calendar.days = std::max(1, std::min(365, calendar.columns * 7 - (6 - MondayWeekday(today))));
    calendar.first = ShiftDay(today, -(calendar.days - 1));
    calendar.gridFirst = ShiftDay(calendar.first, -MondayWeekday(calendar.first));
    return calendar;
}

std::wstring FormatCount(int64_t count, bool compact) {
    if (compact && std::abs(static_cast<double>(count)) >= 1000) {
        const double magnitude = std::abs(static_cast<double>(count));
        const wchar_t* suffix = magnitude >= 1000000000 ? L"B" : magnitude >= 1000000 ? L"M" : L"K";
        const double divisor = magnitude >= 1000000000 ? 1000000000 : magnitude >= 1000000 ? 1000000 : 1000;
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << count / divisor << suffix;
        return out.str();
    }
    std::wstring value = std::to_wstring(count);
    const size_t first = count < 0 ? 1 : 0;
    for (size_t index = value.size(); index > first + 3;) {
        index -= 3;
        value.insert(index, L",");
    }
    return value;
}

std::wstring FormatCost(const std::optional<int64_t>& micros, const Preferences& prefs) {
    if (!micros) return Tr(L"费用未知");
    const double rate = EffectiveUsdToCny(prefs);
    if (prefs.cny && rate <= 0) return Tr(L"汇率暂不可用");
    double amount = *micros / 1000000.0 * (prefs.cny ? rate : 1);
    std::wostringstream out;
    out << (prefs.cny ? L"¥" : L"$") << std::fixed << std::setprecision(2) << amount;
    return out.str();
}

std::vector<UsageRow> SelectRows(const ViewState& state, bool tray) {
    return FilterRows(state.snapshot, tray ? (state.trayMonth ? Range::Month : Range::Today) : state.prefs.range,
                      tray ? std::string() : state.selectedDay);
}

BudgetStatus ComputeBudget(const Snapshot& snapshot, const Preferences& prefs) {
    BudgetStatus status;
    const double rate = prefs.budgetCny ? EffectiveUsdToCny(prefs) : 1;
    if (!(rate > 0) || !std::isfinite(rate)) return status;
    status.available = true;
    auto spend = [&](Range range, bool& unknown) {
        const auto total = Sum(FilterRows(snapshot, range));
        unknown = total.unknown > 0;
        return static_cast<double>(total.cost.value_or(0)) / 1000000.0 * rate;
    };
    status.today = spend(Range::Today, status.todayHasUnknown);
    status.month = spend(Range::Month, status.monthHasUnknown);
    return status;
}

std::wstring TraySummary(const ViewState& state) {
    const auto rows = FilterRows(state.snapshot, Range::Today);
    const auto total = Sum(rows);
    return Tr(L"今日 ") + FormatCount(total.tokens) + Tr(L" Token\n预估 ") + CostText(total, state.prefs) + L"\n" +
           std::to_wstring(GroupSoftware(rows).size()) + Tr(L" 个软件 · ") +
           LocalUpdateTime(state.snapshot.updatedAt, false);
}

void PaintView(HWND, HDC dc, const RECT& client, ViewState& state) {
    UsePalette(state.prefs.darkMode ? kDark : kLight);
    Canvas c(dc, state, client);
    c.Rect(0, 0, c.width, c.height, Background);
    c.sidebar = c.width < 800 ? 136 : 174;
    c.Rect(0, 0, c.sidebar, c.height, Surface);
    c.Rect(c.sidebar - 1, 0, 1, c.height, Line);
    c.Text(L"TokenScope", 18, 21, c.sidebar - 30, 30, c.width < 800 ? 17 : 21, Ink, true);
    c.Text(Tr(L"本地智能体用量"), 18, 54, c.sidebar - 30, 22, 11, Muted);
    struct Nav {
        const wchar_t* name;
        int command;
        Page page;
    };
    const Nav navs[] = {{Tr(L"用量总览"), CmdOverview, Page::Overview},
                        {Tr(L"软件用量"), CmdSoftware, Page::Software},
                        {Tr(L"模型用量"), CmdModels, Page::Models},
                        {Tr(L"设置"), CmdSettings, Page::Settings}};
    for (int i = 0; i < 4; ++i) {
        const bool active = state.page == navs[i].page;
        const int y = 108 + i * 48;
        if (active) c.Rect(10, y, c.sidebar - 20, 38, Soft, CLR_INVALID, 7);
        c.Text(navs[i].name, 24, y, c.sidebar - 42, 38, 14, active ? Accent : Muted, active);
        c.Hit(10, y, c.sidebar - 20, 38, navs[i].command);
    }
    c.Text(Tr(L"仅统计可获取的用量"), 16, c.height - 58, c.sidebar - 28, 22, 10, Muted);
    c.Text(Tr(L"关闭窗口后驻留托盘"), 16, c.height - 36, c.sidebar - 28, 20, 10, Muted);
    c.Rect(c.sidebar, 0, c.width - c.sidebar, 80, Surface);
    c.Rule(c.sidebar, 79, c.width - c.sidebar);
    const std::wstring title = state.page == Page::Overview   ? Tr(L"用量总览")
                               : state.page == Page::Software ? Tr(L"软件用量")
                               : state.page == Page::Models   ? Tr(L"模型用量")
                                                              : Tr(L"设置");
    c.Text(title, c.sidebar + 26, 14, c.width - c.sidebar - 200, 30, 22, Ink, true);
    c.Text(state.collecting                   ? Tr(L"正在增量采集本地记录…")
           : state.snapshot.updatedAt.empty() ? Tr(L"等待首次采集")
                                              : LocalUpdateTime(state.snapshot.updatedAt, true),
           c.sidebar + 26, 47, c.width - c.sidebar - 200, 21, 11, Muted);
    c.Button(state.collecting ? Tr(L"采集中…") : Tr(L"刷新"), c.width - 110, 23, 84, CmdRefresh);
    const int clipped = SaveDC(dc);
    IntersectClipRect(dc, c.sidebar + 1, 80, c.width, c.height);
    c.bodyClip = true;
    const int x = c.sidebar + 26, w = std::max(340, c.width - x - 26);
    const int scroll = static_cast<int>(state.scroll / c.dpi);
    int y = 101 - scroll;
    if (!state.notice.empty() || !state.snapshot.error.empty()) {
        c.Rect(x, y, w, 39, NoticeFill, NoticeLine, 7);
        c.Text(state.notice.empty() ? Utf8ToWide(state.snapshot.error) : state.notice, x + 12, y + 3, w - 24, 32, 12,
               NoticeInk);
        y += 53;
    }
    if (state.page == Page::Settings)
        y = DrawSettings(c, x, y, w);
    else {
        const wchar_t* labels[] = {Tr(L"今日"), Tr(L"近 7 天"), Tr(L"本月"), Tr(L"全部")};
        const int commands[] = {CmdToday, CmdWeek, CmdMonth, CmdAll};
        for (int i = 0; i < 4; ++i)
            c.Button(labels[i], x + i * 83, y, 75, commands[i], static_cast<int>(state.prefs.range) == i);
        y += 52;
        if (state.page == Page::Overview) {
            const auto rows = FilterRows(state.snapshot, state.prefs.range);
            const auto total = Sum(rows);
            const int gap = 12, cardW = (w - gap * 2) / 3;
            for (int i = 0; i < 3; ++i) c.Rect(x + i * (cardW + gap), y, cardW, 111, Surface, Line, 9);
            c.Text(RangeName(state.prefs.range) + L" Token", x + 16, y + 11, cardW - 30, 24, 12, Muted);
            c.Text(FormatCount(total.tokens, true), x + 16, y + 37, cardW - 30, 38, 28, Ink, true);
            c.Text(total.unclassified ? Tr(L"含未分类 ") + FormatCount(total.unclassified, true) + L" Token"
                                      : Tr(L"输入 ") + FormatCount(total.input, true) + Tr(L" / 输出 ") +
                                            FormatCount(total.output, true),
                   x + 16, y + 80, cardW - 30, 20, 10, Muted);
            c.Text(std::wstring(total.cost && total.unknown ? Tr(L"已定价部分 · ") : Tr(L"估算费用 · ")) +
                       (state.prefs.cny ? L"CNY" : L"USD"),
                   x + cardW + gap + 16, y + 11, cardW - 30, 24, 12, Muted);
            c.Text(FormatCost(total.cost, state.prefs), x + cardW + gap + 16, y + 37, cardW - 30, 38, 24, Ink, true);
            c.Text(PriceLabel(total), x + cardW + gap + 16, y + 80, cardW - 30, 20, 10, Muted);
            c.Text(Tr(L"有用量软件"), x + (cardW + gap) * 2 + 16, y + 11, cardW - 30, 24, 12, Muted);
            c.Text(std::to_wstring(GroupSoftware(rows).size()), x + (cardW + gap) * 2 + 16, y + 37, cardW - 30, 38, 28,
                   Ink, true);
            c.Text(FormatCount(total.requests) + Tr(L" 条记录 · 缓存 ") + Percent(total.cached, total.input),
                   x + (cardW + gap) * 2 + 16, y + 80, cardW - 30, 20, 10, Muted);
            y += 130;
            y = DrawChart(c, x, y, w);
        }
        y = DrawSoftware(c, x, y, w, SelectRows(state), state.page == Page::Models);
    }
    c.Text(Tr(L"费用为估算值，不等同于订阅账单。未记录的模型与费用保持未知。"), x, y, w, 24, 10, Muted);
    y += 42;
    state.contentHeight =
        std::max(static_cast<int>((y + scroll) * c.dpi), static_cast<int>(client.bottom - client.top));
    RestoreDC(dc, clipped);
}

} // namespace tokenscope
