#include "app.h"
#include "adapters.h"
#include "pricing.h"
#include "exchange.h"
#include "connections.h"
#include "credentials.h"
#include "yyjson.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace tokenscope;
namespace fs = std::filesystem;
namespace {
constexpr UINT WM_TRAY = WM_APP + 1, WM_IMPORTED = WM_APP + 2, WM_ONLINE = WM_APP + 3, WM_EXCHANGE = WM_APP + 4, WM_CONNECTION = WM_APP + 5;
constexpr UINT TIMER_REFRESH = 1;
constexpr UINT TIMER_EXIT = 2;
HINSTANCE instance;
HWND mainWindow = nullptr, tooltipWindow = nullptr;
UINT taskbarCreated = 0;
ViewState state;
Store store;
std::wstring dataDirectory, settingsPath, importCodex, importClaude;
std::vector<std::pair<std::string,std::wstring>> configuredImports;
std::thread collector, networkWorker, exchangeWorker, connectionWorker;
std::atomic_bool cancelled{false}, networkBusy{false}, exchangeBusy{false}, connectionBusy{false};
struct ConnectionState { std::string state; std::wstring detail; ULONGLONG lastAttempt=0; };
std::map<std::string,ConnectionState> connectionStates;
std::string exchangeFetchedDay;
ULONGLONG exchangeLastAttempt = 0;
NOTIFYICONDATAW trayIcon{};
int keyboardHit = -1;
bool shuttingDown = false, pauseCollection = false, priceDialogActive = false;
bool importRefreshPending = false;
std::wstring lastDay;

struct ScanPacket { Snapshot snapshot; ScanResult result; std::wstring notice; };
struct ExchangePacket { std::optional<ExchangeRateQuote> quote; std::string fetchedDay, error; };
struct ConnectionPacket { std::string id; ConnectionSyncResult result; };
void LoadExchangeCache();
void StartExchangeRate(bool force = false);
void DecorateSources();
void SyncSource(const std::string& id);
void RefreshConfiguredSource(const std::string& id, const std::wstring& path);

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

std::wstring IniString(const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"TokenScope", key, fallback, buffer, 32768, settingsPath.c_str());
    return buffer;
}
bool IniWrite(const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(L"TokenScope", key, value.c_str(), settingsPath.c_str())!=FALSE;
}
bool EnsureUnicodeSettings() {
    // Profile APIs otherwise use the system ANSI code page when saving paths.
    std::error_code error;const auto exists=fs::exists(settingsPath,error);if(error)return false;
    std::string bytes;
    if(exists) {
        const auto size=fs::file_size(settingsPath,error);if(error||size>4*1024*1024)return false;
        std::ifstream input(fs::path(settingsPath),std::ios::binary);bytes.resize(static_cast<size_t>(size));
        if(size&&!input.read(bytes.data(),static_cast<std::streamsize>(size)))return false;
    }
    if(bytes.size()>=2&&static_cast<unsigned char>(bytes[0])==0xFF&&static_cast<unsigned char>(bytes[1])==0xFE)return true;
    std::wstring text;
    if(bytes.size()>=2&&static_cast<unsigned char>(bytes[0])==0xFE&&static_cast<unsigned char>(bytes[1])==0xFF) {
        if(bytes.size()%2)return false;
        for(size_t i=2;i<bytes.size();i+=2)text.push_back(static_cast<wchar_t>((static_cast<unsigned char>(bytes[i])<<8)|static_cast<unsigned char>(bytes[i+1])));
    } else if(!bytes.empty()) {
        UINT page=CP_ACP;size_t offset=0;
        if(bytes.starts_with("\xEF\xBB\xBF")){page=CP_UTF8;offset=3;}
        const auto length=static_cast<int>(bytes.size()-offset);
        const auto count=MultiByteToWideChar(page,page==CP_UTF8?MB_ERR_INVALID_CHARS:0,bytes.data()+offset,length,nullptr,0);
        if(length&&!count)return false;
        text.resize(count);if(count)MultiByteToWideChar(page,page==CP_UTF8?MB_ERR_INVALID_CHARS:0,bytes.data()+offset,length,text.data(),count);
    }
    const auto pending=settingsPath+L".unicode-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    auto file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const wchar_t bom=0xFEFF;DWORD written=0;
    bool okay=WriteFile(file,&bom,sizeof(bom),&written,nullptr)&&written==sizeof(bom);
    const auto length=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    if(okay&&length)okay=WriteFile(file,text.data(),length,&written,nullptr)&&written==length;
    if(okay)okay=FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    if(okay)okay=MoveFileExW(pending.c_str(),settingsPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!okay)DeleteFileW(pending.c_str());
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,settingsPath.c_str());
    return okay;
}
void LoadPreferences() {
    if(!EnsureUnicodeSettings())state.notice=L"设置文件无法升级为 Unicode；请检查数据目录写入权限。";
    int range = GetPrivateProfileIntW(L"TokenScope", L"Range", 0, settingsPath.c_str());
    state.prefs.range = static_cast<Range>(std::clamp(range, 0, 3));
    state.prefs.cny = GetPrivateProfileIntW(L"TokenScope", L"CNY", 0, settingsPath.c_str()) != 0;
    state.prefs.autoRefresh = GetPrivateProfileIntW(L"TokenScope", L"AutoRefresh", 1, settingsPath.c_str()) != 0;
    state.prefs.trendColor = GetPrivateProfileIntW(L"TokenScope", L"TrendColor", RGB(53,101,223), settingsPath.c_str()) & 0xFFFFFF;
    state.prefs.heatColor = GetPrivateProfileIntW(L"TokenScope", L"HeatColor", RGB(105,157,47), settingsPath.c_str()) & 0xFFFFFF;
    wchar_t* end = nullptr;
    const auto manualRate = IniString(L"USDCNY", L"0");
    state.prefs.usdToCny=0;
    double rate = wcstod(manualRate.c_str(), &end);
    while(end && iswspace(*end))++end;
    if (end!=manualRate.c_str() && end && !*end && std::isfinite(rate) && rate > 0 && rate < 10000) state.prefs.usdToCny = rate;
    LoadExchangeCache();
    importCodex = IniString(L"ImportCodex");
    importClaude = IniString(L"ImportClaude");
    configuredImports.clear();
    for(const auto& source:CatalogSources()) {
        const auto key=L"Import."+Utf8ToWide(source.id);
        auto path=IniString(key.c_str());
        if(!path.empty())configuredImports.emplace_back(source.id,std::move(path));
    }
    for(const auto& legacy:std::vector<std::pair<std::string,std::wstring>>{{"codex",importCodex},{"claude",importClaude}}) {
        if(!legacy.second.empty() && std::none_of(configuredImports.begin(),configuredImports.end(),[&](const auto& entry){return entry.first==legacy.first;}))configuredImports.push_back(legacy);
    }
}
bool SavePreferences() {
    if(!EnsureUnicodeSettings()){state.notice=L"设置保存失败：无法写入 Unicode 设置文件。";return false;}
    bool okay=true;
    okay=IniWrite(L"Range", std::to_wstring(static_cast<int>(state.prefs.range)))&&okay;
    okay=IniWrite(L"CNY", state.prefs.cny ? L"1" : L"0")&&okay;
    okay=IniWrite(L"AutoRefresh", state.prefs.autoRefresh ? L"1" : L"0")&&okay;
    okay=IniWrite(L"TrendColor", std::to_wstring(state.prefs.trendColor))&&okay;
    okay=IniWrite(L"HeatColor", std::to_wstring(state.prefs.heatColor))&&okay;
    okay=IniWrite(L"USDCNY", std::to_wstring(state.prefs.usdToCny))&&okay;
    okay=WritePrivateProfileStringW(L"TokenScope",L"ImportCodex",nullptr,settingsPath.c_str())&&okay;
    okay=WritePrivateProfileStringW(L"TokenScope",L"ImportClaude",nullptr,settingsPath.c_str())&&okay;
    for(const auto& source:CatalogSources()) {
        auto found=std::find_if(configuredImports.begin(),configuredImports.end(),[&](const auto& entry){return entry.first==source.id;});
        const auto key=L"Import."+Utf8ToWide(source.id);
        okay=WritePrivateProfileStringW(L"TokenScope",key.c_str(),found==configuredImports.end()?nullptr:found->second.c_str(),settingsPath.c_str())&&okay;
    }
    if(!okay)state.notice=L"设置保存失败，请检查数据目录写入权限。";
    return okay;
}

void UpdateTray() {
    std::wstring tip = TraySummary(state);
    wcsncpy_s(trayIcon.szTip, tip.c_str(), _TRUNCATE);
    trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &trayIcon);
}
void AddTray() {
    trayIcon = {};
    trayIcon.cbSize = sizeof(trayIcon);
    trayIcon.hWnd = mainWindow;
    trayIcon.uID = 1;
    trayIcon.uCallbackMessage = WM_TRAY;
    trayIcon.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32,32, LR_DEFAULTCOLOR));
    if (!trayIcon.hIcon) trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = TraySummary(state);
    wcsncpy_s(trayIcon.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &trayIcon);
    trayIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &trayIcon);
}
void Redraw() {
    keyboardHit = -1;
    InvalidateRect(mainWindow, nullptr, FALSE);
    UpdateTray();
}
void StartCollection() {
    if (state.collecting || shuttingDown || priceDialogActive) return;
    if (collector.joinable()) collector.join();
    state.collecting = true;
    state.notice = L"正在读取用量记录…";
    const auto extraImports=configuredImports;
    collector = std::thread([extraImports] {
        auto packet = std::make_unique<ScanPacket>();
        try {
            std::string pricingError;
            store.ReloadPricing(pricingError);
            packet->result = store.ScanSources(extraImports,cancelled);
            if(!pricingError.empty())packet->result.error+=pricingError;
            packet->snapshot = store.ReadSnapshot();
        } catch (const std::exception& error) { packet->snapshot.error = error.what(); }
        if (!PostMessageW(mainWindow, WM_IMPORTED, 0, reinterpret_cast<LPARAM>(packet.get()))) return;
        packet.release();
    });
    Redraw();
}

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
bool DownloadPublicJson(const std::string& resource,size_t maximumBytes,std::string& body,std::wstring& error) {
    if(cancelled){error=L"已取消";return false;}
    const std::wstring url=Utf8ToWide(resource);const auto slash=url.find(L'/',8);
    const auto host=url.substr(8,slash-8),path=slash==std::wstring::npos?L"/":url.substr(slash);
    InternetHandle session{WinHttpOpen(L"TokenScope/0.4.0 (public reference data)",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.value){error=L"网络初始化失败";return false;}
    WinHttpSetTimeouts(session.value,5000,5000,8000,10000);
    InternetHandle connection{WinHttpConnect(session.value,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0)};
    InternetHandle request{connection.value?WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr};
    // Public constant URLs only. No local identities, prompts or credentials are sent.
    if(!request.value||!WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr)){error=L"网络或证书错误";return false;}
    DWORD status=0,size=sizeof(status);WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
    if(status!=200){error=L"HTTP "+std::to_wstring(status);return false;}
    for(;;) {
        if(cancelled){error=L"已取消";return false;}
        DWORD available=0;if(!WinHttpQueryDataAvailable(request.value,&available)){error=L"读取中断";return false;}
        if(!available)break;
        if(body.size()+available>maximumBytes){error=L"超过响应大小限制";return false;}
        const auto offset=body.size();body.resize(offset+available);DWORD read=0;
        if(!WinHttpReadData(request.value,body.data()+offset,available,&read)){error=L"下载中断";return false;}
        body.resize(offset+read);
    }
    return true;
}
std::wstring CheckModelCatalogue() {
    SYSTEMTIME now{};GetSystemTime(&now);char date[32];
    std::snprintf(date,sizeof(date),"%04u-%02u-%02uT%02u:%02u:%02uZ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    int updated=0;std::wstring errors;PricingCatalog validator(false);
    for(const auto& feed:PricingFeeds()) {
        if(cancelled)break;
        std::string body,validationError;std::wstring error;
        if(!DownloadPublicJson(feed.url,32*1024*1024,body,error)||!validator.LoadJson(feed.feed,body,date,validationError)) {
            errors+=Utf8ToWide(feed.name)+L"："+(error.empty()?L"价目格式校验失败":error)+L"；";continue;
        }
        const fs::path cache=fs::path(dataDirectory)/feed.cacheFile,temp=fs::path(cache.wstring()+L".pending");
        std::ofstream output(temp,std::ios::binary|std::ios::trunc);output.write(body.data(),static_cast<std::streamsize>(body.size()));output.close();
        if(!output||!MoveFileExW(temp.c_str(),cache.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){errors+=Utf8ToWide(feed.name)+L"：写入缓存失败；";continue;}
        std::ofstream metadata(fs::path(cache.wstring()+L".date"),std::ios::binary|std::ios::trunc);metadata<<date;
        ++updated;
    }
    return L"已更新 "+std::to_wstring(updated)+L" / 3 个参考价目源，匹配后估算未定价用量；已定价历史保持原价。"+(errors.empty()?L"":L" 未完成："+errors);
}
std::string BeijingToday() {
    FILETIME time{};GetSystemTimeAsFileTime(&time);ULARGE_INTEGER ticks{};
    ticks.LowPart=time.dwLowDateTime;ticks.HighPart=time.dwHighDateTime;ticks.QuadPart+=8ULL*60*60*10000000;
    time={ticks.LowPart,ticks.HighPart};SYSTEMTIME local{};FileTimeToSystemTime(&time,&local);char day[16];
    std::snprintf(day,sizeof(day),"%04u-%02u-%02u",local.wYear,local.wMonth,local.wDay);return day;
}
void ApplyExchangeQuote(const ExchangeRateQuote& quote) {
    state.prefs.automaticUsdToCny=quote.usdToCny;
    state.prefs.exchangeRateDate=quote.date;state.prefs.exchangeRateSource=quote.source;
}
void LoadExchangeCache() {
    const fs::path path=fs::path(dataDirectory)/L"exchange-rate.json";std::error_code ec;
    const auto bytes=fs::file_size(path,ec);if(ec||bytes>64*1024)return;
    std::ifstream input(path,std::ios::binary);std::string body(static_cast<size_t>(bytes),'\0');
    if(!input.read(body.data(),static_cast<std::streamsize>(body.size())))return;
    std::string error;auto quote=ParseExchangeRateJson(body,error);
    if(!quote||quote->date>BeijingToday())return;
    ApplyExchangeQuote(*quote);state.prefs.exchangeRateStatus="使用已缓存的公开日参考价";
    std::ifstream metadata(fs::path(path.wstring()+L".date"),std::ios::binary);char fetched[12]{};
    metadata.read(fetched,10);if(metadata.gcount()==10)exchangeFetchedDay.assign(fetched,10);
    // A cache with an old publication date must be refreshed even if its metadata is fresh.
    if(quote->date<ShiftDay(BeijingToday(),-7))exchangeFetchedDay.clear();
}
ExchangePacket FetchExchangeRate(const std::wstring& directory) {
    ExchangePacket packet;std::string body,parseError;std::wstring networkError;
    if(!DownloadPublicJson(ExchangeRateUrl(),64*1024,body,networkError)) {
        packet.error=WideToUtf8(networkError);return packet;
    }
    packet.quote=ParseExchangeRateJson(body,parseError);
    const auto today=BeijingToday();
    if(!packet.quote||packet.quote->date>today||packet.quote->date<ShiftDay(today,-31)) {
        packet.quote.reset();packet.error="公开汇率格式或发布日期无效";return packet;
    }
    const fs::path cache=fs::path(directory)/L"exchange-rate.json",pending=fs::path(cache.wstring()+L".pending");
    std::ofstream output(pending,std::ios::binary|std::ios::trunc);output.write(body.data(),static_cast<std::streamsize>(body.size()));output.close();
    packet.fetchedDay=today;
    if(!output||!MoveFileExW(pending.c_str(),cache.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        packet.error="已取得公开日参考价，缓存写入失败";return packet;
    }
    std::ofstream metadata(fs::path(cache.wstring()+L".date"),std::ios::binary|std::ios::trunc);metadata<<today;
    if(!metadata)packet.error="已取得公开日参考价，获取日期写入失败";
    return packet;
}
void ApplyExchangeResult(const ExchangePacket& packet) {
    if(packet.quote) {
        ApplyExchangeQuote(*packet.quote);exchangeFetchedDay=packet.fetchedDay;
        state.prefs.exchangeRateStatus=packet.error.empty()?"最新公布的日参考价；非实时成交价":packet.error;
    } else {
        state.prefs.exchangeRateStatus="公开汇率更新失败："+packet.error+
            (state.prefs.automaticUsdToCny>0?"；沿用缓存，见公布日期":"；尚无可用汇率");
    }
}
void StartExchangeRate(bool force) {
    if(shuttingDown||state.prefs.usdToCny>0)return;
    const auto now=GetTickCount64();
    if(!force&&((state.prefs.automaticUsdToCny>0&&exchangeFetchedDay==BeijingToday())||
       (exchangeLastAttempt&&now-exchangeLastAttempt<30*60*1000)))return;
    if(exchangeBusy.exchange(true))return;
    if(exchangeWorker.joinable())exchangeWorker.join();
    exchangeLastAttempt=now;state.prefs.exchangeRateStatus=state.prefs.automaticUsdToCny>0?
        "正在更新公开日参考价，暂用缓存":"正在获取公开日参考价…";
    const auto directory=dataDirectory;
    exchangeWorker=std::thread([directory] {
        auto packet=std::make_unique<ExchangePacket>();
        try {*packet=FetchExchangeRate(directory);}catch(const std::exception& error){packet->error=error.what();}
        if(PostMessageW(mainWindow,WM_EXCHANGE,0,reinterpret_cast<LPARAM>(packet.get())))packet.release();
    });
    Redraw();
}
void StartOnlineCheck() {
    if (networkBusy.exchange(true)) return;
    if (networkWorker.joinable()) networkWorker.join();
    state.onlineStatus = L"正在更新 LiteLLM、OpenRouter 与 models.dev 公开价目…";
    networkWorker = std::thread([] {
        std::unique_ptr<std::wstring> message;
        try {message=std::make_unique<std::wstring>(CheckModelCatalogue());}
        catch(const std::exception& e){message=std::make_unique<std::wstring>(L"价目更新失败，保留原有数据："+Utf8ToWide(e.what()));}
        if (PostMessageW(mainWindow, WM_ONLINE, 0, reinterpret_cast<LPARAM>(message.get()))) message.release();
        networkBusy = false;
    });
    Redraw();
}

struct RateDialogState { HWND edit = nullptr; bool done = false, saved = false; double value = 0; };
LRESULT CALLBACK RateDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<RateDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<RateDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        auto label = CreateWindowW(L"STATIC", L"1 USD 等于多少 CNY？留空或填 0 使用公开汇率。\n手动值优先；仅用于显示换算。", WS_CHILD|WS_VISIBLE, 20,18,370,42, window,nullptr,instance,nullptr);
        dialog->edit = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT", dialog->value > 0 ? std::to_wstring(dialog->value).c_str() : L"", WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,20,70,365,28,window,reinterpret_cast<HMENU>(10),instance,nullptr);
        auto save = CreateWindowW(L"BUTTON", L"保存", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,215,116,80,30,window,reinterpret_cast<HMENU>(IDOK),instance,nullptr);
        auto close = CreateWindowW(L"BUTTON", L"取消", WS_CHILD|WS_VISIBLE|WS_TABSTOP,305,116,80,30,window,reinterpret_cast<HMENU>(IDCANCEL),instance,nullptr);
        for (HWND child : {label,dialog->edit,save,close}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SetFocus(dialog->edit);
        SendMessageW(dialog->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buffer[128]{}; GetWindowTextW(dialog->edit, buffer, 128);
            wchar_t* start=buffer;while(iswspace(*start))++start;
            wchar_t* end = nullptr; double value = *start?wcstod(start, &end):0;
            if(!*start)end=start;
            while (end && iswspace(*end)) ++end;
            if (!std::isfinite(value) || value < 0 || value >= 10000 || !end || *end || (*start&&end==start)) {
                MessageBoxW(window,L"留空或填 0 使用公开汇率；手动汇率须大于 0、小于 10000。",L"汇率",MB_OK|MB_ICONINFORMATION); return 0;
            }
            dialog->value = value; dialog->saved = true; DestroyWindow(window); return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: dialog->done = true; return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
void EditExchangeRate() {
    RateDialogState dialog; dialog.value = state.prefs.usdToCny;
    RECT rect{}; GetWindowRect(mainWindow, &rect);
    EnableWindow(mainWindow, FALSE);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,L"TokenScopeRate",L"设置人民币汇率",WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_VISIBLE,
                                rect.left+150,rect.top+150,425,200,mainWindow,nullptr,instance,&dialog);
    if (!window) { EnableWindow(mainWindow,TRUE); return; }
    MSG message;
    while (!dialog.done && GetMessageW(&message,nullptr,0,0)>0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(mainWindow,TRUE); SetForegroundWindow(mainWindow);
    if (dialog.saved) { state.prefs.usdToCny=dialog.value; state.notice=dialog.value>0?L"已使用手动汇率。":L"已切换为公开汇率。"; SavePreferences();StartExchangeRate(true); }
}
void ChooseChartColor(bool heat) {
    static COLORREF custom[16]{};
    CHOOSECOLORW choose{}; choose.lStructSize=sizeof(choose); choose.hwndOwner=mainWindow;
    choose.rgbResult=heat ? state.prefs.heatColor : state.prefs.trendColor;
    choose.lpCustColors=custom; choose.Flags=CC_FULLOPEN|CC_RGBINIT;
    if (ChooseColorW(&choose)) {
        COLORREF color=choose.rgbResult;
        const double luminance=.2126*GetRValue(color)+.7152*GetGValue(color)+.0722*GetBValue(color);
        if(luminance>210) {
            const double scale=210.0/luminance;
            color=RGB(static_cast<int>(GetRValue(color)*scale),static_cast<int>(GetGValue(color)*scale),static_cast<int>(GetBValue(color)*scale));
            state.notice=L"所选颜色较浅，已适当加深以便在图表中辨认。";
        }
        if (heat) state.prefs.heatColor=color; else state.prefs.trendColor=color;
        SavePreferences();
    }
}

struct PriceDialogState {
    HWND combo=nullptr;
    HWND fields[4]{};
    std::vector<PriceRule> models;
    PriceRule selected;
    bool done=false, saved=false;
};
bool ParsePrice(std::wstring text, int64_t& result) {
    auto first=text.find_first_not_of(L" \t\r\n"), last=text.find_last_not_of(L" \t\r\n");
    if(first==std::wstring::npos)return false;
    text=text.substr(first,last-first+1);
    int64_t integer=0, fraction=0, scale=1;
    bool decimal=false, digit=false;
    for(wchar_t ch:text) {
        if(ch==L'.'&&!decimal){decimal=true;continue;}
        if(ch<L'0'||ch>L'9')return false;
        digit=true;
        if(!decimal){integer=integer*10+(ch-L'0');if(integer>10000)return false;}
        else {if(scale>=1000000)return false;fraction=fraction*10+(ch-L'0');scale*=10;}
    }
    if(!digit)return false;
    result=integer*1000000+fraction*(1000000/scale);
    return result<=10000LL*1000000;
}
LRESULT CALLBACK PriceDialogProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* dialog=reinterpret_cast<PriceDialogState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){dialog=static_cast<PriceDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(dialog));}
    switch(message) {
    case WM_CREATE: {
        std::vector<HWND> controls;
        controls.push_back(CreateWindowW(L"STATIC",L"选择记录中的实际软件 / 模型 / 提供商：",WS_CHILD|WS_VISIBLE,20,18,570,24,window,nullptr,instance,nullptr));
        dialog->combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,20,48,590,260,window,reinterpret_cast<HMENU>(20),instance,nullptr);
        controls.push_back(dialog->combo);
        for(const auto& model:dialog->models) {
            auto label=Utf8ToWide(model.software+" / "+model.model+" / "+(model.provider.empty()?"提供商未记录":model.provider));
            SendMessageW(dialog->combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(dialog->combo,CB_SETCURSEL,0,0);
        const wchar_t* labels[]={L"普通输入",L"输出",L"缓存读取",L"缓存写入"};
        for(int i=0;i<4;++i) {
            int y=101+i*43;
            controls.push_back(CreateWindowW(L"STATIC",labels[i],WS_CHILD|WS_VISIBLE,20,y,140,26,window,nullptr,instance,nullptr));
            dialog->fields[i]=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,170,y,225,28,window,reinterpret_cast<HMENU>(30+i),instance,nullptr);
            controls.push_back(dialog->fields[i]);
            controls.push_back(CreateWindowW(L"STATIC",L"USD / 百万 Token",WS_CHILD|WS_VISIBLE,410,y,195,26,window,nullptr,instance,nullptr));
        }
        controls.push_back(CreateWindowW(L"STATIC",L"四项均需填写，可填 0；最多六位小数。\n会补算该身份尚未定价的历史记录，并用于未来记录。\n已定价历史沿用旧版本；估算不等于实际扣费。",WS_CHILD|WS_VISIBLE,20,285,590,64,window,nullptr,instance,nullptr));
        controls.push_back(CreateWindowW(L"BUTTON",L"保存并估算",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,370,365,130,32,window,reinterpret_cast<HMENU>(IDOK),instance,nullptr));
        controls.push_back(CreateWindowW(L"BUTTON",L"取消",WS_CHILD|WS_VISIBLE|WS_TABSTOP,515,365,95,32,window,reinterpret_cast<HMENU>(IDCANCEL),instance,nullptr));
        for(HWND child:controls)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        SetFocus(dialog->fields[0]);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wParam)==IDOK) {
            auto index=static_cast<int>(SendMessageW(dialog->combo,CB_GETCURSEL,0,0));
            if(index<0||index>=static_cast<int>(dialog->models.size()))return 0;
            int64_t rates[4]{};
            for(int i=0;i<4;++i){wchar_t text[100]{};GetWindowTextW(dialog->fields[i],text,100);if(!ParsePrice(text,rates[i])){MessageBoxW(window,L"每项请输入 0–10000 USD / 百万 Token，最多六位小数。",L"模型价格",MB_OK|MB_ICONINFORMATION);SetFocus(dialog->fields[i]);return 0;}}
            dialog->selected=dialog->models[index];
            dialog->selected.inputMicrosPerMillion=rates[0];dialog->selected.outputMicrosPerMillion=rates[1];
            dialog->selected.cachedMicrosPerMillion=rates[2];dialog->selected.writeMicrosPerMillion=rates[3];
            dialog->saved=true;DestroyWindow(window);return 0;
        }
        if(LOWORD(wParam)==IDCANCEL){DestroyWindow(window);return 0;}break;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:dialog->done=true;return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
void EditModelPrice() {
    if(state.collecting){state.notice=L"首次采集完成后即可设置模型价格。";return;}
    PriceDialogState dialog;
    std::set<std::string> identities;
    for(const auto& row:state.snapshot.rows) {
        if(row.model.empty())continue;
        if(identities.insert(row.software+"\n"+row.model+"\n"+row.provider).second)
            dialog.models.push_back({row.software,row.model,row.provider});
    }
    if(dialog.models.empty()){state.notice=L"尚未采集到有实际模型 ID 的记录；未知模型保持未定价。";return;}
    RECT rect{};GetWindowRect(mainWindow,&rect);
    priceDialogActive=true;EnableWindow(mainWindow,FALSE);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,L"TokenScopePrice",L"手动模型价格 · USD / 百万 Token",WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_VISIBLE,
                              rect.left+90,rect.top+90,650,455,mainWindow,nullptr,instance,&dialog);
    if(window){MSG message{};while(!dialog.done&&GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}}
    priceDialogActive=false;EnableWindow(mainWindow,TRUE);SetForegroundWindow(mainWindow);
    if(shuttingDown)return;
    if(!dialog.saved){if(importRefreshPending){importRefreshPending=false;StartCollection();}return;}
    if(collector.joinable())collector.join();
    state.collecting=true;state.notice=L"正在按手动价格估算尚未定价记录…";
    collector=std::thread([rule=dialog.selected] {
        auto packet=std::make_unique<ScanPacket>();
        std::string error;
        try {
            if(store.SetPriceRule(rule,error))packet->notice=L"手动价格已保存；未定价记录已估算，已有历史价格未改写。";
            else packet->notice=L"价格保存失败："+Utf8ToWide(error);
            packet->snapshot=store.ReadSnapshot();
        }catch(const std::exception& e){packet->notice=L"计价失败："+Utf8ToWide(e.what());}
        if(PostMessageW(mainWindow,WM_IMPORTED,0,reinterpret_cast<LPARAM>(packet.get())))packet.release();
    });
}
std::wstring ChooseFolder(HWND owner=nullptr) {
    IFileOpenDialog* dialog=nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags=0; dialog->GetOptions(&flags); dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"选择用量日志文件夹（只读导入）");
    std::wstring folder;
    if (SUCCEEDED(dialog->Show(owner?owner:mainWindow))) {
        IShellItem* item=nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path=nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) { folder=path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release(); return folder;
}

struct ImportDialogState {
    std::vector<SourceSpec> sources;
    HWND combo = nullptr, description = nullptr, currentPath = nullptr;
    HFONT font = nullptr;
    int selected = -1;
    bool folder = false, done = false;
};
void UpdateImportDescription(ImportDialogState& dialog) {
    const auto index = SendMessageW(dialog.combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<size_t>(index) >= dialog.sources.size()) return;
    const auto& selected = dialog.sources[static_cast<size_t>(index)];
    SetWindowTextW(dialog.description, selected.note.c_str());
    std::wstring text = L"尚未添加自定义用量路径";
    for (const auto& imported : configuredImports) if (imported.first == selected.id) {
        text = L"当前路径：" + imported.second; break;
    }
    SetWindowTextW(dialog.currentPath, text.c_str());
}
LRESULT CALLBACK ImportDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<ImportDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<ImportDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        const UINT dpi = GetDpiForWindow(window);
        const auto px = [dpi](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
        dialog->font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        std::vector<HWND> controls;
        auto add = [&](const wchar_t* klass, const wchar_t* label, DWORD style, int x, int y, int w, int h, int id = 0) {
            HWND control = CreateWindowW(klass, label, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            controls.push_back(control); return control;
        };
        add(L"STATIC", L"选择记录所属的软件", 0, 20, 18, 560, 25);
        dialog->combo = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 49, 560, 280, 70);
        for (const auto& source : dialog->sources) {
            const auto label = Utf8ToWide(source.name);
            SendMessageW(dialog->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(dialog->combo, CB_SETMINVISIBLE, 12, 0);
        SendMessageW(dialog->combo, CB_SETCURSEL, 0, 0);
        dialog->description = add(L"STATIC", L"", 0, 20, 92, 560, 58);
        dialog->currentPath = add(L"STATIC", L"", SS_ENDELLIPSIS, 20, 157, 560, 40);
        add(L"STATIC", L"只读实际 Token 记录，不修改原文件。\n普通聊天导出与积分记录不会生成 Token 用量。", 0, 20, 205, 560, 48);
        add(L"BUTTON", L"选择文件", WS_TABSTOP | BS_DEFPUSHBUTTON, 215, 272, 110, 34, IDOK);
        add(L"BUTTON", L"选择文件夹", WS_TABSTOP, 337, 272, 128, 34, 71);
        add(L"BUTTON", L"取消", WS_TABSTOP, 477, 272, 103, 34, IDCANCEL);
        for (HWND control : controls) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font ? dialog->font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        UpdateImportDescription(*dialog); SetFocus(dialog->combo); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 70 && HIWORD(wParam) == CBN_SELCHANGE) { UpdateImportDescription(*dialog); return 0; }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == 71) {
            const auto selected = SendMessageW(dialog->combo, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR && static_cast<size_t>(selected) < dialog->sources.size()) {
                dialog->selected = static_cast<int>(selected); dialog->folder = LOWORD(wParam) == 71;
                DestroyWindow(window);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: dialog->done = true; return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
std::wstring ChooseUsageFile(const SourceSpec& source, HWND owner=nullptr) {
    wchar_t path[32768]{};
    std::wstring initial;
    for (const auto& configured : configuredImports) if (configured.first == source.id) {
        std::error_code error; fs::path previous(configured.second);
        initial = fs::is_directory(previous, error) ? previous.wstring() : previous.parent_path().wstring();
        break;
    }
    const auto title = Utf8ToWide(source.name) + L" · 选择真实用量文件（只读）";
    OPENFILENAMEW open{}; open.lStructSize = sizeof(open); open.hwndOwner = owner?owner:mainWindow;
    open.lpstrTitle = title.c_str();
    open.lpstrFilter = L"用量文件\0*.json;*.jsonl;*.csv;*.db;*.sqlite;*.sqlite3;*.zstd\0JSON / JSONL\0*.json;*.jsonl\0CSV\0*.csv\0SQLite\0*.db;*.sqlite;*.sqlite3\0压缩会话\0*.zstd\0所有文件\0*.*\0\0";
    open.lpstrFile = path; open.nMaxFile = 32768;
    open.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_EXPLORER;
    if (GetOpenFileNameW(&open)) return path;
    if (const DWORD error = CommDlgExtendedError()) state.notice = L"文件选择失败，错误码 " + std::to_wstring(error);
    return {};
}

const SourceSpec* FindSource(const std::string& id) {
    static const auto sources=CatalogSources();
    auto found=std::find_if(sources.begin(),sources.end(),[&](const auto& source){return source.id==id;});
    return found==sources.end()?nullptr:&*found;
}
void RefreshConfiguredSource(const std::string& id,const std::wstring& path) {
    if(!FindSource(id))return;
    auto connection=connectionStates.find(id);
    if(connection!=connectionStates.end()&&connection->second.state=="syncing"){state.notice=L"请等待本来源同步结束后再切换路径。";return;}
    const auto previous=configuredImports;
    auto existing=std::find_if(configuredImports.begin(),configuredImports.end(),[&](const auto& imported){return imported.first==id;});
    if(path.empty()) {
        if(existing!=configuredImports.end())configuredImports.erase(existing);
    } else if(existing==configuredImports.end())configuredImports.emplace_back(id,path);
    else existing->second=path;
    if(!SavePreferences()){configuredImports=previous;return;}
    if(state.collecting||priceDialogActive) {importRefreshPending=true;state.notice=L"路径设置已保存，当前操作结束后读取；保留已有历史用量。";}
    else StartCollection();
}
struct SourcePathDialogState {
    SourceSpec source;
    std::wstring candidates,value;
    HWND edit=nullptr;
    HFONT font=nullptr;
    bool saved=false,reset=false,done=false;
};
std::wstring EditText(HWND edit) {
    int length=GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length)+1,L'\0');
    GetWindowTextW(edit,text.data(),length+1);text.resize(length);return text;
}
LRESULT CALLBACK SourcePathDialogProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* dialog=reinterpret_cast<SourcePathDialogState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){dialog=static_cast<SourcePathDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(dialog));}
    switch(message) {
    case WM_CREATE: {
        const UINT dpi=GetDpiForWindow(window);const auto px=[dpi](int n){return MulDiv(n,dpi?dpi:96,96);};
        dialog->font=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        std::vector<HWND> controls;
        auto add=[&](const wchar_t* klass,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0) {
            auto control=CreateWindowExW(wcscmp(klass,L"EDIT")==0?WS_EX_CLIENTEDGE:0,klass,text,WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            controls.push_back(control);return control;
        };
        add(L"STATIC",L"当前采集路径（可选中复制；发现状态以最近一次采集为准）",0,20,16,640,25);
        add(L"EDIT",dialog->candidates.c_str(),WS_TABSTOP|WS_VSCROLL|WS_HSCROLL|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL,20,47,640,105);
        add(L"STATIC",dialog->source.note.c_str(),0,20,165,640,50);
        add(L"STATIC",L"自定义文件或文件夹路径",0,20,224,640,23);
        dialog->edit=add(L"EDIT",dialog->value.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,20,253,640,31,80);
        SendMessageW(dialog->edit,EM_SETLIMITTEXT,32767,0);
        add(L"BUTTON",L"选择文件",WS_TABSTOP,20,300,112,32,81);
        add(L"BUTTON",L"选择文件夹",WS_TABSTOP,145,300,125,32,82);
        add(L"STATIC",L"保存后仅读取此路径；尚不存在的路径会显示“未发现”。\n恢复默认会重新自动发现，已采集的历史数据会保留。",0,20,347,640,48);
        add(L"BUTTON",L"恢复默认",WS_TABSTOP,20,411,118,34,83);
        add(L"BUTTON",L"保存路径",WS_TABSTOP|BS_DEFPUSHBUTTON,420,411,114,34,IDOK);
        add(L"BUTTON",L"取消",WS_TABSTOP,546,411,114,34,IDCANCEL);
        for(auto child:controls)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(dialog->font?dialog->font:GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        SetFocus(dialog->edit);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wParam)==81||LOWORD(wParam)==82) {
            auto path=LOWORD(wParam)==81?ChooseUsageFile(dialog->source,window):ChooseFolder(window);
            if(!path.empty())SetWindowTextW(dialog->edit,path.c_str());return 0;
        }
        if(LOWORD(wParam)==IDOK) {
            auto text=EditText(dialog->edit);
            const auto first=text.find_first_not_of(L" \t\r\n"),last=text.find_last_not_of(L" \t\r\n");
            if(first==std::wstring::npos){MessageBoxW(window,L"请输入路径；使用“恢复默认”重新自动发现。",L"采集路径",MB_OK|MB_ICONINFORMATION);return 0;}
            text=text.substr(first,last-first+1);
            if(text.size()>1&&text.front()==L'"'&&text.back()==L'"')text=text.substr(1,text.size()-2);
            DWORD length=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);
            if(!length||length>32768){MessageBoxW(window,L"路径过长或无效。",L"采集路径",MB_OK|MB_ICONINFORMATION);return 0;}
            std::wstring expanded(length,L'\0');ExpandEnvironmentStringsW(text.c_str(),expanded.data(),length);expanded.resize(length-1);
            std::error_code error;auto absolute=fs::absolute(fs::path(expanded),error).lexically_normal();
            if(error||expanded.empty()||expanded.find_first_of(L"\r\n\t")!=std::wstring::npos){MessageBoxW(window,L"请输入有效文件或文件夹路径。",L"采集路径",MB_OK|MB_ICONINFORMATION);return 0;}
            dialog->value=absolute.wstring();dialog->saved=true;DestroyWindow(window);return 0;
        }
        if(LOWORD(wParam)==83){dialog->reset=true;DestroyWindow(window);return 0;}
        if(LOWORD(wParam)==IDCANCEL){DestroyWindow(window);return 0;}
        break;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:dialog->done=true;return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
bool RunModal(const wchar_t* klass,const std::wstring& title,int logicalWidth,int logicalHeight,void* dialog,bool& done) {
    const UINT dpi=GetDpiForWindow(mainWindow);RECT owner{};GetWindowRect(mainWindow,&owner);
    RECT size{0,0,MulDiv(logicalWidth,dpi?dpi:96,96),MulDiv(logicalHeight,dpi?dpi:96,96)};
    const DWORD style=WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_VISIBLE;
    AdjustWindowRectExForDpi(&size,style,FALSE,WS_EX_DLGMODALFRAME,dpi?dpi:96);
    EnableWindow(mainWindow,FALSE);
    auto window=CreateWindowExW(WS_EX_DLGMODALFRAME,klass,title.c_str(),style,owner.left+(owner.right-owner.left-(size.right-size.left))/2,owner.top+(owner.bottom-owner.top-(size.bottom-size.top))/2,size.right-size.left,size.bottom-size.top,mainWindow,nullptr,instance,dialog);
    if(!window){EnableWindow(mainWindow,TRUE);return false;}
    MSG message{};BOOL received=1;
    while(!done&&(received=GetMessageW(&message,nullptr,0,0))>0)if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    if(!received)PostQuitMessage(static_cast<int>(message.wParam));
    if(IsWindow(window))DestroyWindow(window);
    if(IsWindow(mainWindow)){EnableWindow(mainWindow,TRUE);SetForegroundWindow(mainWindow);}
    return !shuttingDown;
}
void ConfigureSourcePath(const std::string& id) {
    auto* source=FindSource(id);if(!source)return;
    auto connection=connectionStates.find(id);
    if(connection!=connectionStates.end()&&connection->second.state=="syncing"){state.notice=L"请等待本来源同步结束后再切换路径。";return;}
    SourcePathDialogState dialog;dialog.source=*source;
    for(const auto& entry:configuredImports)if(entry.first==id)dialog.value=entry.second;
    for(const auto& status:state.snapshot.sources)if(status.id==id) {
        for(const auto& path:status.paths)dialog.candidates+=(path.exists?L"已发现  ":L"未发现  ")+path.path+L"\r\n";
    }
    if(dialog.candidates.empty())dialog.candidates=L"未配置本地路径，可选择真实用量文件或文件夹。";
    if(RunModal(L"TokenScopeSourcePath",Utf8ToWide(source->name)+L" · 采集路径",680,468,&dialog,dialog.done)) {
        if(dialog.saved||dialog.reset)RefreshConfiguredSource(id,dialog.reset?L"":dialog.value);
    }
    if(dialog.font)DeleteObject(dialog.font);
}

void DecorateSources() {
    for(auto& source:state.snapshot.sources) {
        const auto* profile=FindConnectionProfile(source.id);
        source.canAuthorize=profile!=nullptr;
        if(!profile)continue;
        source.hasCredentials=HasConnectionCredential(dataDirectory,source.id);
        const auto found=connectionStates.find(source.id);
        source.connectionState=found!=connectionStates.end()?found->second.state:(source.hasCredentials?"saved":"unconfigured");
        source.connectionDetail=found!=connectionStates.end()?found->second.detail:L"实验性账号用量同步；不读取浏览器登录信息";
    }
}
void SyncSource(const std::string& id) {
    if(!FindConnectionProfile(id)||shuttingDown)return;
    if(connectionBusy){state.notice=L"当前账号正在同步，请完成后再操作。";return;}
    std::string token,error;
    if(!LoadConnectionCredential(dataDirectory,id,token,error)){state.notice=L"尚无可用授权，请重新连接。";return;}
    if(connectionWorker.joinable())connectionWorker.join();
    connectionBusy=true;connectionStates[id]={"syncing",L"正在读取最近 30 天实际用量",GetTickCount64()};DecorateSources();
    const auto directory=(fs::path(dataDirectory)/L"connections"/Utf8ToWide(id)).wstring();
    connectionWorker=std::thread([id,directory,secret=std::move(token)]() mutable {
        auto packet=std::make_unique<ConnectionPacket>();packet->id=id;
        try {packet->result=SyncConnection(id,secret,directory,cancelled);}
        catch(...){packet->result.error="同步未完成";}
        if(!secret.empty())SecureZeroMemory(secret.data(),secret.size());secret.clear();
        if(PostMessageW(mainWindow,WM_CONNECTION,0,reinterpret_cast<LPARAM>(packet.get())))packet.release();
    });
    Redraw();
}
struct ConnectionDialogState {
    ConnectionProfile profile;
    HWND edit=nullptr;
    HFONT font=nullptr;
    std::string token;
    bool done=false,saved=false;
};
LRESULT CALLBACK ConnectionDialogProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* dialog=reinterpret_cast<ConnectionDialogState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){dialog=static_cast<ConnectionDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(dialog));}
    switch(message) {
    case WM_CREATE: {
        const UINT dpi=GetDpiForWindow(window);const auto px=[dpi](int n){return MulDiv(n,dpi?dpi:96,96);};
        dialog->font=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        std::vector<HWND> controls;
        auto add=[&](const wchar_t* klass,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0){auto child=CreateWindowExW(wcscmp(klass,L"EDIT")==0?WS_EX_CLIENTEDGE:0,klass,text,WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);controls.push_back(child);return child;};
        add(L"STATIC",L"连接自己的账号，读取最近 30 天用量",0,20,16,620,27);
        add(L"STATIC",Utf8ToWide(dialog->profile.credentialHint).c_str(),0,20,52,620,72);
        add(L"BUTTON",L"打开官方用量页面",WS_TABSTOP,20,135,190,32,90);
        add(L"STATIC",L"在系统默认浏览器打开；网页登录不会自动传回凭据。",0,225,139,415,27);
        add(L"STATIC",L"账号用量凭据（遮罩输入）",0,20,185,620,24);
        dialog->edit=add(L"EDIT",L"",WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,20,218,620,32,91);SendMessageW(dialog->edit,EM_SETLIMITTEXT,16384,0);
        add(L"STATIC",L"实验性 Dashboard 接口，服务方变更时可能需要重新连接。\n凭据由 Windows 在本机加密保存，仅发送到对应服务方。\n成功取得实际 Token 记录后才统计；不会把积分换算为 Token。\n连接成功后，本来源切换到该账号的本地同步缓存。",0,20,266,620,88);
        add(L"BUTTON",L"授权并同步",WS_TABSTOP|BS_DEFPUSHBUTTON,390,375,130,34,IDOK);
        add(L"BUTTON",L"取消",WS_TABSTOP,534,375,106,34,IDCANCEL);
        for(auto child:controls)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(dialog->font?dialog->font:GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        SetFocus(dialog->edit);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wParam)==90){auto url=Utf8ToWide(dialog->profile.websiteUrl);ShellExecuteW(window,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return 0;}
        if(LOWORD(wParam)==IDOK) {
            auto input=EditText(dialog->edit);
            const auto first=input.find_first_not_of(L" \t\r\n"),last=input.find_last_not_of(L" \t\r\n");
            std::string token=first==std::wstring::npos?"":WideToUtf8(input.substr(first,last-first+1));
            SecureZeroMemory(input.data(),input.size()*sizeof(wchar_t));
            if(dialog->profile.sourceId=="cursor"&&token.starts_with("WorkosCursorSessionToken="))token.erase(0,25);
            if(dialog->profile.sourceId=="trae-code"&&token.starts_with("Cloud-IDE-JWT "))token.erase(0,14);
            bool invalid=token.empty()||token.size()>16384;
            for(unsigned char ch:token)if(ch<=32||ch>=127||ch==';'||ch=='\r'||ch=='\n')invalid=true;
            if(!invalid) {
                FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER ticks{};ticks.LowPart=now.dwLowDateTime;ticks.HighPart=now.dwHighDateTime;
                const auto milliseconds=static_cast<int64_t>((ticks.QuadPart-116444736000000000ULL)/10000ULL);
                std::string error;invalid=!BuildConnectionRequest(dialog->profile.sourceId,token,milliseconds-30LL*86400000,milliseconds,1,error).has_value();
            }
            if(invalid){if(!token.empty())SecureZeroMemory(token.data(),token.size());MessageBoxW(window,L"请输入单独的有效凭据值，不包含 Cookie 列表、换行或其它请求头。",L"连接账号",MB_OK|MB_ICONINFORMATION);return 0;}
            dialog->token=std::move(token);dialog->saved=true;SetWindowTextW(dialog->edit,L"");DestroyWindow(window);return 0;
        }
        if(LOWORD(wParam)==IDCANCEL){SetWindowTextW(dialog->edit,L"");DestroyWindow(window);return 0;}
        break;
    case WM_CLOSE:SetWindowTextW(dialog->edit,L"");DestroyWindow(window);return 0;
    case WM_DESTROY:dialog->done=true;return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
void ConnectSource(const std::string& id) {
    const auto* profile=FindConnectionProfile(id);if(!profile)return;
    if(connectionBusy){state.notice=L"当前账号正在同步，请完成后再连接。";return;}
    ConnectionDialogState dialog;dialog.profile=*profile;
    RunModal(L"TokenScopeConnection",Utf8ToWide(profile->name)+L" · 授权连接",660,430,&dialog,dialog.done);
    if(dialog.font)DeleteObject(dialog.font);
    if(dialog.saved&&!shuttingDown) {
        std::string error;
        if(SaveConnectionCredential(dataDirectory,id,dialog.token,error)) {
            connectionStates[id]={"saved",L"凭据已加密保存，尚待验证",0};
            if(!dialog.token.empty())SecureZeroMemory(dialog.token.data(),dialog.token.size());dialog.token.clear();
            SyncSource(id);
        } else state.notice=L"凭据保存失败："+Utf8ToWide(error);
    }
    if(!dialog.token.empty())SecureZeroMemory(dialog.token.data(),dialog.token.size());
    DecorateSources();
}
void DisconnectSource(const std::string& id) {
    if(connectionBusy){state.notice=L"请等待账号同步结束后解除连接。";return;}
    std::string error;
    if(!RemoveConnectionCredential(dataDirectory,id,error)){state.notice=L"解除连接失败："+Utf8ToWide(error);return;}
    connectionStates[id]={"unconfigured",L"已删除本机凭据；保留同步缓存及历史用量",0};
    state.notice=L"已解除本机连接；如需撤销服务端登录会话，请在服务方账号设置中操作。";DecorateSources();
}
void SourceHelp(const std::string& id) {
    const auto* source=FindSource(id);if(!source)return;
    auto text=source->note+L"\n\n";
    if(source->format==SourceFormat::UsageExport||source->paths.empty())
        text+=L"当前接入方式：导入包含真实 Token、模型与调用时间的用量文件。点击“更改路径”选择文件或文件夹。普通聊天导出、积分和请求次数不能生成 Token 统计。\n\n此来源暂未提供账号直连按钮；网页登录本身不会使统计出现。";
    else text+=L"当前接入方式：只读本地用量记录，通常无需额外账号授权。点击“更改路径”可查看完整路径或指定目录；“恢复默认”重新自动发现。\n\n若已发现路径仍无用量，需要该软件实际记录 Token；仅有程序安装目录或普通对话文本不够。";
    if(id=="trae-cn"||id=="trae-work")text+=L"\n\nTRAE CN / TraeWork 与国际版 IDE 账号接口分开，当前不复用国际版凭据。";
    MessageBoxW(mainWindow,text.c_str(),(Utf8ToWide(source->name)+L" · 接入说明").c_str(),MB_OK|MB_ICONINFORMATION);
}
void AddImportSource() {
    ImportDialogState dialog; dialog.sources = CatalogSources();
    if (dialog.sources.empty()) { state.notice = L"暂无可选择的来源。"; return; }
    const UINT dpi = GetDpiForWindow(mainWindow);
    RECT owner{}; GetWindowRect(mainWindow, &owner);
    RECT size{0, 0, MulDiv(600, dpi ? dpi : 96, 96), MulDiv(326, dpi ? dpi : 96, 96)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE;
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_DLGMODALFRAME, dpi ? dpi : 96);
    const int width = size.right - size.left, height = size.bottom - size.top;
    EnableWindow(mainWindow, FALSE);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TokenScopeImport", L"添加 / 导入用量来源", style,
        owner.left + (owner.right - owner.left - width) / 2, owner.top + (owner.bottom - owner.top - height) / 2,
        width, height, mainWindow, nullptr, instance, &dialog);
    if (!window) { EnableWindow(mainWindow, TRUE); if (dialog.font) DeleteObject(dialog.font); state.notice = L"无法打开来源选择窗口。"; return; }
    MSG message{}; BOOL received = 1;
    while (!dialog.done && (received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (!received) PostQuitMessage(static_cast<int>(message.wParam));
    if (IsWindow(window)) DestroyWindow(window);
    if (IsWindow(mainWindow)) { EnableWindow(mainWindow, TRUE); SetForegroundWindow(mainWindow); }
    if (dialog.font) DeleteObject(dialog.font);
    if (dialog.selected < 0 || shuttingDown) return;
    const auto& source = dialog.sources[static_cast<size_t>(dialog.selected)];
    const auto path = dialog.folder ? ChooseFolder() : ChooseUsageFile(source);
    if (path.empty()) return;
    RefreshConfiguredSource(source.id,path);
}
void ExportData() {
    // Use the cached immutable snapshot; never access the database from the UI thread.
    wchar_t path[32768]=L"TokenScope-usage.csv";
    OPENFILENAMEW save{}; save.lStructSize=sizeof(save); save.hwndOwner=mainWindow;
    save.lpstrFilter=L"CSV 文件\0*.csv\0\0"; save.lpstrFile=path; save.nMaxFile=32768;
    save.lpstrDefExt=L"csv"; save.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&save)) {
        Snapshot snapshot=state.snapshot; snapshot.rows=SelectRows(state);
        std::string error;
        if (!store.ExportCsv(path,snapshot,error)) state.notice=L"导出失败："+Utf8ToWide(error);
        else state.notice=L"已导出当前日期范围的软件 / 模型用量。";
    }
}
void OpenMain(bool settings=false) {
    state.chart=Chart::Trend; state.selectedDay.clear(); state.chartEnd=LocalToday();
    state.page=settings ? Page::Settings : Page::Overview; state.scroll=0;
    ShowWindow(mainWindow,SW_RESTORE); SetForegroundWindow(mainWindow); Redraw();
}
void Execute(int command, const std::string& value={}) {
    switch(command) {
    case CmdOverview: state.page=Page::Overview; state.scroll=0; break;
    case CmdSoftware: state.page=Page::Software; state.scroll=0; break;
    case CmdModels: state.page=Page::Models; state.scroll=0; break;
    case CmdSettings: state.page=Page::Settings; state.scroll=0; break;
    case CmdToday: case CmdWeek: case CmdMonth: case CmdAll:
        state.prefs.range=static_cast<Range>(command-CmdToday); state.selectedDay.clear(); state.scroll=0; SavePreferences(); break;
    case CmdTrend: state.chart=Chart::Trend; break;
    case CmdHeatmap: state.chart=Chart::Heatmap; break;
    case CmdTokens: state.costMetric=false; break;
    case CmdCost: state.costMetric=true; break;
    case CmdSelectDay:
        state.selectedDay=value;
        break;
    case CmdClearDay: state.selectedDay.clear(); break;
    case CmdTrendWeek: state.trendMonth=false; break;
    case CmdTrendMonth: state.trendMonth=true; break;
    case CmdPrevious: state.chartEnd=ShiftDay(state.chartEnd.empty()?LocalToday():state.chartEnd,state.trendMonth?-30:-7); break;
    case CmdNext: state.chartEnd=std::min(LocalToday(),ShiftDay(state.chartEnd.empty()?LocalToday():state.chartEnd,state.trendMonth?30:7)); break;
    case CmdRefresh: StartCollection();StartExchangeRate(true); break;
    case CmdCurrency: state.prefs.cny=!state.prefs.cny; SavePreferences();StartExchangeRate(); break;
    case CmdExchangeRate: EditExchangeRate(); break;
    case CmdTrendColor: ChooseChartColor(false); break;
    case CmdHeatColor: ChooseChartColor(true); break;
    case CmdResetColors: state.prefs.trendColor=RGB(53,101,223); state.prefs.heatColor=RGB(105,157,47); SavePreferences(); break;
    case CmdAutoRefresh: state.prefs.autoRefresh=!state.prefs.autoRefresh; SavePreferences(); break;
    case CmdOpenData: ShellExecuteW(mainWindow,L"open",dataDirectory.c_str(),nullptr,nullptr,SW_SHOWNORMAL); break;
    case CmdExport: ExportData(); break;
    case CmdImportSource: AddImportSource(); break;
    case CmdSourcePath: ConfigureSourcePath(value); break;
    case CmdSourceReset: RefreshConfiguredSource(value,L""); break;
    case CmdSourceConnect: ConnectSource(value); break;
    case CmdSourceSync: SyncSource(value); break;
    case CmdSourceDisconnect: DisconnectSource(value); break;
    case CmdSourceHelp: SourceHelp(value); break;
    case CmdImportCodex: case CmdImportClaude: {
        std::wstring folder=ChooseFolder();
        if(!folder.empty())RefreshConfiguredSource(command==CmdImportCodex?"codex":"claude",folder);
        break;
    }
    case CmdOnlineCheck: StartOnlineCheck();StartExchangeRate(true); break;
    case CmdPricing: EditModelPrice(); break;
    case CmdTrayMonth: state.trayMonth=true; break;
    case CmdTrayToday: state.trayMonth=false; break;
    case CmdOpenMain: OpenMain(); return;
    case CmdExit: DestroyWindow(mainWindow); return;
    }
    Redraw();
}

void UpdateScroll(HWND window) {
    RECT rect{}; GetClientRect(window,&rect);
    SCROLLINFO info{}; info.cbSize=sizeof(info); info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
    info.nMin=0; info.nMax=std::max(0,state.contentHeight-1); info.nPage=rect.bottom;
    state.scroll=std::clamp(state.scroll,0,std::max<int>(0,state.contentHeight-rect.bottom));
    info.nPos=state.scroll; SetScrollInfo(window,SB_VERT,&info,TRUE);
}
void ScrollBy(int pixels) { state.scroll+=pixels; UpdateScroll(mainWindow); Redraw(); }
void ShowTrayMenu() {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,CmdOpenMain,L"打开 TokenScope");
    AppendMenuW(menu,MF_STRING,CmdRefresh,L"刷新用量");
    AppendMenuW(menu,MF_STRING,900,pauseCollection?L"恢复自动采集":L"暂停自动采集");
    AppendMenuW(menu,MF_STRING,CmdSettings,L"设置");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,CmdExit,L"退出");
    POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(mainWindow);
    int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,cursor.x,cursor.y,0,mainWindow,nullptr);
    DestroyMenu(menu); PostMessageW(mainWindow,WM_NULL,0,0);
    if(choice==900) {pauseCollection=!pauseCollection;state.notice=pauseCollection?L"自动采集已暂停。":L"自动采集已恢复。";Redraw();}
    else if(choice==CmdSettings) OpenMain(true);
    else if(choice) Execute(choice);
}
void PaintBuffered(HWND window) {
    PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); RECT rect{};GetClientRect(window,&rect);
    HDC memory=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,rect.right),std::max(1L,rect.bottom));
    HGDIOBJ previous=SelectObject(memory,bitmap);
    state.dpi=GetDpiForWindow(window)/96.0f;
    PaintView(window,memory,rect,state);
    if(keyboardHit>=0 && keyboardHit<static_cast<int>(state.hits.size())) {RECT focus=state.hits[keyboardHit].rect;DrawFocusRect(memory,&focus);}
    BitBlt(dc,0,0,rect.right,rect.bottom,memory,0,0,SRCCOPY);
    SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);EndPaint(window,&paint);
    UpdateScroll(window);
}
void HitClick(LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
    const auto hits=state.hits;
    for(auto it=hits.rbegin();it!=hits.rend();++it) if(PtInRect(&it->rect,point)) {Execute(it->command,it->value);return;}
}
void Hover(HWND window, LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
    bool interactive=false; std::wstring text;
    for(auto it=state.hits.rbegin();it!=state.hits.rend();++it) if(PtInRect(&it->rect,point)) {
        interactive=true;
        if(it->command==CmdSelectDay) {
            int64_t total=0; bool known=false;
            for(const auto& row:state.snapshot.rows) if(row.day==it->value) {total+=row.total();known=true;}
            text=Utf8ToWide(it->value)+L" · "+(known?FormatCount(total)+L" Token":L"未采集 · 用量未知");
        }
        break;
    }
    SetCursor(LoadCursorW(nullptr,interactive?IDC_HAND:IDC_ARROW));
    if(tooltipWindow) {
        TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.hwnd=window;tool.uId=1;tool.lpszText=text.empty()?const_cast<wchar_t*>(L""):text.data();
        SendMessageW(tooltipWindow,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
        SendMessageW(tooltipWindow,TTM_ACTIVATE,!text.empty(),0);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if(taskbarCreated && message==taskbarCreated) {AddTray();return 0;}
    switch(message) {
    case WM_PAINT: PaintBuffered(window);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SIZE: UpdateScroll(window);InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_GETMINMAXINFO: {auto* info=reinterpret_cast<MINMAXINFO*>(lParam);UINT dpi=GetDpiForWindow(window);info->ptMinTrackSize={MulDiv(800,dpi?dpi:96,96),MulDiv(600,dpi?dpi:96,96)};return 0;}
    case WM_DPICHANGED: {const RECT* rect=reinterpret_cast<RECT*>(lParam);SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_LBUTTONUP: HitClick(lParam);return 0;
    case WM_MOUSEMOVE: Hover(window,lParam);return 0;
    case WM_MOUSEWHEEL: ScrollBy(-GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA*90);return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{};info.cbSize=sizeof(info);info.fMask=SIF_ALL;GetScrollInfo(window,SB_VERT,&info);
        switch(LOWORD(wParam)) {case SB_LINEUP:state.scroll-=40;break;case SB_LINEDOWN:state.scroll+=40;break;case SB_PAGEUP:state.scroll-=static_cast<int>(info.nPage)*3/4;break;case SB_PAGEDOWN:state.scroll+=static_cast<int>(info.nPage)*3/4;break;case SB_THUMBTRACK:state.scroll=info.nTrackPos;break;}
        UpdateScroll(window);Redraw();
    }return 0;
    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE) {ShowWindow(window,SW_HIDE);return 0;}
        if(wParam==VK_TAB && !state.hits.empty()) {
            keyboardHit=(keyboardHit+(GetKeyState(VK_SHIFT)<0?-1:1)+static_cast<int>(state.hits.size()))%static_cast<int>(state.hits.size());
            InvalidateRect(window,nullptr,FALSE);return 0;
        }
        if((wParam==VK_RETURN||wParam==VK_SPACE)&&keyboardHit>=0&&keyboardHit<static_cast<int>(state.hits.size())) {auto hit=state.hits[keyboardHit];Execute(hit.command,hit.value);return 0;}
        if(wParam==VK_F5 || (wParam=='R'&&GetKeyState(VK_CONTROL)<0)) Execute(CmdRefresh);
        if(wParam=='T') Execute(CmdTrend);
        if(wParam=='H') Execute(CmdHeatmap);
        if(wParam>='1'&&wParam<='4') Execute(CmdToday+static_cast<int>(wParam-'1'));
        if(wParam==VK_DOWN) ScrollBy(50); if(wParam==VK_UP) ScrollBy(-50);
        if(wParam==VK_NEXT) ScrollBy(400); if(wParam==VK_PRIOR) ScrollBy(-400);
        return 0;
    case WM_CLOSE: ShowWindow(window,SW_HIDE);state.notice=L"已隐藏到托盘，继续采集。";return 0;
    case WM_TIMER:
        if(wParam==TIMER_EXIT){DestroyWindow(window);return 0;}
        if(wParam==TIMER_REFRESH) {
        std::wstring today=Utf8ToWide(LocalToday());
        if(today!=lastDay) {lastDay=today;state.chartEnd=LocalToday();Redraw();}
        if(state.prefs.autoRefresh&&!pauseCollection) StartCollection();
        StartExchangeRate();
    }return 0;
    case WM_POWERBROADCAST: if(wParam==PBT_APMRESUMEAUTOMATIC) {AddTray();if(!pauseCollection)StartCollection();}return TRUE;
    case WM_TRAY: {
        UINT event=LOWORD(lParam);
        if(event==NIN_SELECT||event==NIN_KEYSELECT) OpenMain();
        if(event==WM_CONTEXTMENU) ShowTrayMenu();
        if(event==WM_LBUTTONDBLCLK) OpenMain();
        return 0;
    }
    case WM_IMPORTED: {
        std::unique_ptr<ScanPacket> packet(reinterpret_cast<ScanPacket*>(lParam));
        if(collector.joinable())collector.join();
        state.collecting=false;state.snapshot=std::move(packet->snapshot);
        DecorateSources();
        if(!packet->notice.empty())state.notice=packet->notice;
        else if(!state.snapshot.error.empty()) state.notice=L"采集异常："+Utf8ToWide(state.snapshot.error);
        else if(!packet->result.error.empty())state.notice=L"部分来源未完成："+Utf8ToWide(packet->result.error);
        else state.notice=L"已更新 · 新增 "+FormatCount(packet->result.added)+L" 条用量记录"+(packet->result.invalid?L" · 跳过 "+FormatCount(packet->result.invalid)+L" 条无效记录":L"");
        if(importRefreshPending) {importRefreshPending=false;StartCollection();}
        Redraw();return 0;
    }
    case WM_ONLINE: {std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));state.onlineStatus=*result;
        if(state.collecting||priceDialogActive)importRefreshPending=true;else StartCollection();Redraw();return 0;}
    case WM_EXCHANGE: {
        std::unique_ptr<ExchangePacket> packet(reinterpret_cast<ExchangePacket*>(lParam));
        if(exchangeWorker.joinable())exchangeWorker.join();exchangeBusy=false;
        ApplyExchangeResult(*packet);Redraw();return 0;
    }
    case WM_CONNECTION: {
        std::unique_ptr<ConnectionPacket> packet(reinterpret_cast<ConnectionPacket*>(lParam));
        if(connectionWorker.joinable())connectionWorker.join();connectionBusy=false;
        auto& connection=connectionStates[packet->id];
        if(packet->result.success) {
            connection.state="connected";
            connection.detail=packet->result.usageRecords?L"最近 30 天 · 已同步 "+std::to_wstring(packet->result.usageRecords)+L" 条真实用量到本地缓存":L"连接成功，最近 30 天暂无可采集 Token 用量";
            RefreshConfiguredSource(packet->id,packet->result.cachePath);
        } else {
            connection.state=packet->result.authenticationFailed?"expired":"error";
            connection.detail=packet->result.authenticationFailed?L"凭据无效或已过期，请重新授权":Utf8ToWide(packet->result.error.empty()?"同步未完成，请稍后重试":packet->result.error);
            state.notice=Utf8ToWide(packet->id)+L"："+connection.detail+L"；保留之前的缓存和历史用量。";
        }
        DecorateSources();Redraw();return 0;
    }
    case WM_DESTROY:
        shuttingDown=true;cancelled=true;KillTimer(window,TIMER_REFRESH);
        Shell_NotifyIconW(NIM_DELETE,&trayIcon);
        if(collector.joinable())collector.join();
        if(networkWorker.joinable())networkWorker.join();
        if(exchangeWorker.joinable())exchangeWorker.join();
        if(connectionWorker.joinable())connectionWorker.join();
        MSG pending{};
        while(PeekMessageW(&pending,window,WM_IMPORTED,WM_IMPORTED,PM_REMOVE))delete reinterpret_cast<ScanPacket*>(pending.lParam);
        while(PeekMessageW(&pending,window,WM_ONLINE,WM_ONLINE,PM_REMOVE))delete reinterpret_cast<std::wstring*>(pending.lParam);
        while(PeekMessageW(&pending,window,WM_EXCHANGE,WM_EXCHANGE,PM_REMOVE))delete reinterpret_cast<ExchangePacket*>(pending.lParam);
        while(PeekMessageW(&pending,window,WM_CONNECTION,WM_CONNECTION,PM_REMOVE))delete reinterpret_cast<ConnectionPacket*>(pending.lParam);
        SavePreferences();PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

int Diagnose(const std::wstring& exportPath,bool scan,const std::string& source,const std::wstring& folder,bool network,bool exchangeCheck) {
    std::string error;
    if(!store.Open((fs::path(dataDirectory)/L"usage.sqlite3").wstring(),error)) {fprintf(stderr,"Database: %s\n",error.c_str());return 1;}
    LoadPreferences();
    std::wstring networkStatus;if(network)networkStatus=CheckModelCatalogue();
    if(exchangeCheck&&state.prefs.usdToCny<=0)ApplyExchangeResult(FetchExchangeRate(dataDirectory));
    if(!store.ReloadPricing(error)){fprintf(stderr,"Pricing: %s\n",error.c_str());return 1;}
    ScanResult result;
    if(scan)result=store.ScanSources(configuredImports,cancelled);
    if(!folder.empty()) {auto imported=store.ImportFolder(folder,source,cancelled);result.added+=imported.added;result.invalid+=imported.invalid;result.error+=imported.error;}
    Snapshot snapshot=store.ReadSnapshot();
    if(!exportPath.empty()&&!store.ExportCsv(exportPath,snapshot,error)) {fprintf(stderr,"Export: %s\n",error.c_str());return 1;}
    int64_t tokens=0;std::set<std::string> software,models;
    for(const auto& row:snapshot.rows) {
        tokens+=row.total();
        if(row.total()>0) {
            software.insert(row.software);
            models.insert((row.provider.empty()?"unknown-provider:"+row.software:row.provider)+"/"+row.model);
        }
    }
    printf("{\"events\":%lld,\"added\":%lld,\"tokens\":%lld,\"software\":%zu,\"modelIdentities\":%zu,\"dailyModelRows\":%zu,\"invalid\":%lld}\n",
           static_cast<long long>(snapshot.events),static_cast<long long>(result.added),static_cast<long long>(tokens),software.size(),models.size(),snapshot.rows.size(),static_cast<long long>(result.invalid));
    if(!result.error.empty())fprintf(stderr,"%s\n",result.error.c_str());
    if(!snapshot.error.empty())fprintf(stderr,"%s\n",snapshot.error.c_str());
    if(network)printf("%s\n",WideToUtf8(networkStatus).c_str());
    if(exchangeCheck) {
        printf("{\"usdToCny\":%.8f,\"manual\":%s,\"publicationDate\":\"%s\",\"source\":\"%s\"}\n",EffectiveUsdToCny(state.prefs),
            state.prefs.usdToCny>0?"true":"false",state.prefs.usdToCny>0?"":state.prefs.exchangeRateDate.c_str(),
            state.prefs.usdToCny>0?"manual":state.prefs.exchangeRateSource.c_str());
        if(state.prefs.usdToCny<=0&&!state.prefs.exchangeRateStatus.empty())printf("%s\n",state.prefs.exchangeRateStatus.c_str());
    }
    fflush(stdout);return snapshot.error.empty()?0:1;
}
} // namespace

int WINAPI wWinMain(HINSTANCE appInstance,HINSTANCE,PWSTR,int show) {
    instance=appInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool diagnose=false,scan=true,network=false,hidden=false,exchangeCheck=false;
    int exitAfterSeconds=0;
    std::wstring exportPath,folder;std::string source;
    dataDirectory=GetDataDirectory();
    for(int i=1;i<argc;++i) {
        std::wstring arg=argv[i];
        if(arg==L"--data-dir"&&i+1<argc)dataDirectory=argv[++i];
        else if(arg==L"--diagnose")diagnose=true;
        else if(arg==L"--no-scan")scan=false;
        else if(arg==L"--network-check")network=true;
        else if(arg==L"--exchange-check")exchangeCheck=true;
        else if(arg==L"--tray")hidden=true;
        else if(arg==L"--exit-after"&&i+1<argc)exitAfterSeconds=std::clamp(_wtoi(argv[++i]),1,3600);
        else if(arg==L"--export"&&i+1<argc)exportPath=argv[++i];
        else if(arg==L"--import"&&i+2<argc){source=WideToUtf8(argv[++i]);folder=argv[++i];}
    }
    LocalFree(argv);
    try {dataDirectory=fs::absolute(dataDirectory).lexically_normal().wstring();fs::create_directories(dataDirectory);}catch(...){MessageBoxW(nullptr,L"无法创建数据目录。",L"TokenScope",MB_OK|MB_ICONERROR);return 1;}
    settingsPath=(fs::path(dataDirectory)/L"settings.ini").wstring();
    if(diagnose){int result=Diagnose(exportPath,scan,source,folder,network,exchangeCheck);CoUninitialize();return result;}
    std::wstring mutexName=L"Local\\TokenScope-"+std::to_wstring(std::hash<std::wstring>{}(dataDirectory));
    HANDLE mutex=CreateMutexW(nullptr,TRUE,mutexName.c_str());
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND other=FindWindowW(L"TokenScopeMain",nullptr);if(other){ShowWindow(other,SW_RESTORE);SetForegroundWindow(other);}CloseHandle(mutex);CoUninitialize();return 0;}
    std::string error;
    if(!store.Open((fs::path(dataDirectory)/L"usage.sqlite3").wstring(),error)){MessageBoxW(nullptr,Utf8ToWide(error).c_str(),L"数据库初始化失败",MB_OK|MB_ICONERROR);CloseHandle(mutex);CoUninitialize();return 1;}
    LoadPreferences();state.snapshot=store.ReadSnapshot();DecorateSources();state.chartEnd=LocalToday();lastDay=Utf8ToWide(LocalToday());
    state.onlineStatus=L"内置参考价目离线可用；可按需更新三源价目，仅补未定价记录，不上传本地日志。";
    WNDCLASSEXW klass{};klass.cbSize=sizeof(klass);klass.lpfnWndProc=WindowProc;klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,0,0,LR_DEFAULTSIZE));
    klass.hIconSm=klass.hIcon;klass.lpszClassName=L"TokenScopeMain";RegisterClassExW(&klass);
    klass.lpszClassName=L"TokenScopeRate";klass.lpfnWndProc=RateDialogProc;klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassExW(&klass);
    klass.lpszClassName=L"TokenScopePrice";klass.lpfnWndProc=PriceDialogProc;RegisterClassExW(&klass);
    klass.lpszClassName=L"TokenScopeImport";klass.lpfnWndProc=ImportDialogProc;RegisterClassExW(&klass);
    klass.lpszClassName=L"TokenScopeSourcePath";klass.lpfnWndProc=SourcePathDialogProc;RegisterClassExW(&klass);
    klass.lpszClassName=L"TokenScopeConnection";klass.lpfnWndProc=ConnectionDialogProc;RegisterClassExW(&klass);
    mainWindow=CreateWindowExW(0,L"TokenScopeMain",L"TokenScope · 本地智能体用量",WS_OVERLAPPEDWINDOW|WS_VSCROLL,
                             CW_USEDEFAULT,CW_USEDEFAULT,1180,850,nullptr,nullptr,instance,nullptr);
    if(!mainWindow){CloseHandle(mutex);CoUninitialize();return 1;}
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");AddTray();
    tooltipWindow=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,mainWindow,nullptr,instance,nullptr);
    if(tooltipWindow){TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.uFlags=TTF_SUBCLASS;tool.hwnd=mainWindow;tool.uId=1;GetClientRect(mainWindow,&tool.rect);tool.lpszText=const_cast<wchar_t*>(L"");SendMessageW(tooltipWindow,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));SendMessageW(tooltipWindow,TTM_SETMAXTIPWIDTH,0,440);}
    SetTimer(mainWindow,TIMER_REFRESH,15000,nullptr);
    if(exitAfterSeconds)SetTimer(mainWindow,TIMER_EXIT,exitAfterSeconds*1000,nullptr);
    if(!hidden){ShowWindow(mainWindow,show);UpdateWindow(mainWindow);}
    StartCollection();
    StartExchangeRate();
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    ReleaseMutex(mutex);CloseHandle(mutex);CoUninitialize();return static_cast<int>(message.wParam);
}
