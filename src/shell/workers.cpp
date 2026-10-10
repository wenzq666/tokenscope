#include "shell/shell.h"
#include "http.h"

namespace tokenscope::shell {
namespace {

bool DownloadPublicJson(const std::string& resource, size_t maximumBytes, std::string& body, std::wstring& error) {
    HttpsRequest request;
    if (!SplitHttpsUrl(resource, request.host, request.path)) {
        error = Tr(L"地址无效");
        return false;
    }
    request.maxResponseBytes = maximumBytes;
    // Public constant URLs only. No local identities, prompts or credentials are sent.
    auto response = SendHttps(request, cancelled);
    if (cancelled || response.networkError == ERROR_CANCELLED) {
        error = Tr(L"已取消");
        return false;
    }
    if (response.networkError == ERROR_FILE_TOO_LARGE) {
        error = Tr(L"超过响应大小限制");
        return false;
    }
    if (response.networkError) {
        error = Tr(L"网络或证书错误（") + std::to_wstring(response.networkError) + Tr(L"）");
        return false;
    }
    if (response.status != 200) {
        error = L"HTTP " + std::to_wstring(response.status);
        return false;
    }
    body = std::move(response.body);
    return true;
}

std::string UtcNowIso() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char date[32];
    std::snprintf(date, sizeof(date), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
    return date;
}

} // namespace

bool WriteFileAtomically(const fs::path& path, const std::string& bytes) {
    const fs::path pending = fs::path(path.wstring() + L".pending");
    {
        std::ofstream output(pending, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            DeleteFileW(pending.c_str());
            return false;
        }
    }
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        return false;
    }
    return true;
}
bool WriteTextFile(const fs::path& path, const std::string& text) {
    return WriteFileAtomically(path, text);
}

void StartCollection() {
    if (state.collecting || shuttingDown || priceDialogActive) return;
    if (collector.joinable()) collector.join();
    state.collecting = true;
    lastCollectionStart = GetTickCount64();
    state.notice = Tr(L"正在读取用量记录…");
    const auto extraImports = configuredImports;
    collector = std::thread([extraImports] {
        auto packet = std::make_unique<ScanPacket>();
        try {
            std::string pricingError;
            store.ReloadPricing(pricingError);
            packet->result = store.ScanSources(extraImports, cancelled);
            if (!pricingError.empty()) packet->result.error += pricingError;
            packet->snapshot = store.ReadSnapshot();
        } catch (const std::exception& error) {
            packet->snapshot.error = error.what();
        }
        if (!PostMessageW(mainWindow, WM_IMPORTED, 0, reinterpret_cast<LPARAM>(packet.get()))) return;
        packet.release();
    });
    Redraw();
}

std::wstring CheckModelCatalogue() {
    const auto date = UtcNowIso();
    int updated = 0;
    std::wstring errors;
    PricingCatalog validator(false);
    for (const auto& feed : PricingFeeds()) {
        if (cancelled) break;
        std::string body, validationError;
        std::wstring error;
        if (!DownloadPublicJson(feed.url, 32 * 1024 * 1024, body, error) ||
            !validator.LoadJson(feed.feed, body, date, validationError)) {
            errors += Utf8ToWide(feed.name) + Tr(L"：") + (error.empty() ? Tr(L"价目格式校验失败") : error) + Tr(L"；");
            continue;
        }
        const fs::path cache = fs::path(dataDirectory) / feed.cacheFile;
        if (!WriteFileAtomically(cache, body)) {
            errors += Utf8ToWide(feed.name) + Tr(L"：写入缓存失败；");
            continue;
        }
        // The price cache is already valid without its date; report but still count it as updated.
        if (!WriteTextFile(fs::path(cache.wstring() + L".date"), date))
            errors += Utf8ToWide(feed.name) + Tr(L"：获取时间写入失败；");
        ++updated;
    }
    return Tr(L"已更新 ") + std::to_wstring(updated) + L" / " + std::to_wstring(PricingFeeds().size()) +
           Tr(L" 个参考价目源，匹配后估算未定价用量；已定价历史保持原价。") +
           (errors.empty() ? L"" : Tr(L" 未完成：") + errors);
}

void ApplyExchangeQuote(const ExchangeRateQuote& quote) {
    state.prefs.automaticUsdToCny = quote.usdToCny;
    state.prefs.exchangeRateDate = quote.date;
    state.prefs.exchangeRateSource = quote.source;
}
void LoadExchangeCache() {
    const fs::path path = fs::path(dataDirectory) / L"exchange-rate.json";
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes > 64 * 1024) return;
    std::ifstream input(path, std::ios::binary);
    std::string body(static_cast<size_t>(bytes), '\0');
    if (!input.read(body.data(), static_cast<std::streamsize>(body.size()))) return;
    std::string error;
    auto quote = ParseExchangeRateJson(body, error);
    // Publication dates are compared with the same local calendar day used for usage buckets.
    const auto today = LocalToday();
    if (!quote || quote->date > ShiftDay(today, 1)) return;
    ApplyExchangeQuote(*quote);
    state.prefs.exchangeRateStatus = Tr8("使用已缓存的公开日参考价");
    std::ifstream metadata(fs::path(path.wstring() + L".date"), std::ios::binary);
    char fetched[12]{};
    metadata.read(fetched, 10);
    if (metadata.gcount() == 10) exchangeFetchedDay.assign(fetched, 10);
    // A cache with an old publication date must be refreshed even if its metadata is fresh.
    if (quote->date < ShiftDay(today, -7)) exchangeFetchedDay.clear();
}
ExchangePacket FetchExchangeRate(const std::wstring& directory) {
    ExchangePacket packet;
    std::string body, parseError;
    std::wstring networkError;
    if (!DownloadPublicJson(ExchangeRateUrl(), 64 * 1024, body, networkError)) {
        packet.error = WideToUtf8(networkError);
        return packet;
    }
    packet.quote = ParseExchangeRateJson(body, parseError);
    const auto today = LocalToday();
    // Allow one day of skew: west of Frankfurt the ECB publication date can be "tomorrow" locally.
    if (!packet.quote || packet.quote->date > ShiftDay(today, 1) || packet.quote->date < ShiftDay(today, -31)) {
        packet.quote.reset();
        packet.error = Tr8("公开汇率格式或发布日期无效");
        return packet;
    }
    const fs::path cache = fs::path(directory) / L"exchange-rate.json";
    packet.fetchedDay = today;
    if (!WriteFileAtomically(cache, body)) {
        packet.error = Tr8("已取得公开日参考价，缓存写入失败");
        return packet;
    }
    if (!WriteTextFile(fs::path(cache.wstring() + L".date"), today))
        packet.error = Tr8("已取得公开日参考价，获取日期写入失败");
    return packet;
}
void ApplyExchangeResult(const ExchangePacket& packet) {
    if (packet.quote) {
        ApplyExchangeQuote(*packet.quote);
        exchangeFetchedDay = packet.fetchedDay;
        state.prefs.exchangeRateStatus = packet.error.empty() ? Tr8("最新公布的日参考价；非实时成交价") : packet.error;
    } else {
        state.prefs.exchangeRateStatus =
            Tr8("公开汇率更新失败：") + packet.error +
            (state.prefs.automaticUsdToCny > 0 ? Tr8("；沿用缓存，见公布日期") : Tr8("；尚无可用汇率"));
    }
}
void StartExchangeRate(bool force) {
    if (shuttingDown || state.prefs.usdToCny > 0) return;
    const auto now = GetTickCount64();
    if (!force && ((state.prefs.automaticUsdToCny > 0 && exchangeFetchedDay == LocalToday()) ||
                   (exchangeLastAttempt && now - exchangeLastAttempt < 30 * 60 * 1000)))
        return;
    if (exchangeBusy.exchange(true)) return;
    if (exchangeWorker.joinable()) exchangeWorker.join();
    exchangeLastAttempt = now;
    state.prefs.exchangeRateStatus =
        state.prefs.automaticUsdToCny > 0 ? Tr8("正在更新公开日参考价，暂用缓存") : Tr8("正在获取公开日参考价…");
    const auto directory = dataDirectory;
    exchangeWorker = std::thread([directory] {
        auto packet = std::make_unique<ExchangePacket>();
        try {
            *packet = FetchExchangeRate(directory);
        } catch (const std::exception& error) {
            packet->error = error.what();
        }
        if (PostMessageW(mainWindow, WM_EXCHANGE, 0, reinterpret_cast<LPARAM>(packet.get()))) packet.release();
    });
    Redraw();
}
void StartOnlineCheck() {
    if (networkBusy.exchange(true)) return;
    if (networkWorker.joinable()) networkWorker.join();
    state.onlineStatus = Tr(L"正在更新 LiteLLM、OpenRouter 与 models.dev 公开价目…");
    networkWorker = std::thread([] {
        std::unique_ptr<std::wstring> message;
        try {
            message = std::make_unique<std::wstring>(CheckModelCatalogue());
        } catch (const std::exception& e) {
            message = std::make_unique<std::wstring>(Tr(L"价目更新失败，保留原有数据：") + Utf8ToWide(e.what()));
        }
        if (PostMessageW(mainWindow, WM_ONLINE, 0, reinterpret_cast<LPARAM>(message.get()))) message.release();
        networkBusy = false;
    });
    Redraw();
}

void StopWorkers() {
    // Order matters: SendHttps re-checks the flag after registering its handle, so any request
    // that registers after this store is refused and any earlier one is closed below.
    cancelled = true;
    // Blocking WinHTTP calls ignore the flag; closing their handles makes them return immediately.
    CancelHttpRequests();
    StopSourceWatcher();
    for (auto* worker : {&collector, &networkWorker, &exchangeWorker, &connectionWorker})
        if (worker->joinable()) worker->join();
}

} // namespace tokenscope::shell
