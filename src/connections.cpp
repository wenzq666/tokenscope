// Protocol/schema reference: missuo/tokens (MIT), independently implemented in native C++.
// See docs/connections.md for the experimental dashboard API boundary and attribution.
#include "connections.h"
#include "yyjson.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <string_view>
#include <thread>

namespace tokenscope {
namespace {
constexpr int64_t kDay = 86400000;
constexpr int64_t kCounterLimit = INT64_MAX / 16;
constexpr size_t kPageBytes = 8 * 1024 * 1024;
constexpr const char* kCursorUrl = "https://cursor.com/api/dashboard/get-filtered-usage-events";
constexpr const char* kTraeUrl = "https://api-sg-central.trae.ai/trae/api/v1/pay/query_user_usage_group_by_session";
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
template<class String> struct ScopedWipe {
    String& text;
    ~ScopedWipe() {
        if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(typename String::value_type));
    }
};

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE handle) : value(handle) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::string_view Text(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view();
}
bool Unique(yyjson_val* object) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) > 128) return false;
    std::set<std::string_view> keys;
    size_t index, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, index, count, key, value) {
        if (!keys.insert(Text(key)).second) return false;
    }
    return true;
}
bool Counter(yyjson_val* value, int64_t& number, bool required = true) {
    number = 0;
    if (!value || yyjson_is_null(value)) return !required;
    if (yyjson_is_int(value)) {
        if (yyjson_is_uint(value) && yyjson_get_uint(value) > static_cast<uint64_t>(kCounterLimit)) return false;
        number = yyjson_get_sint(value);
    } else if (yyjson_is_real(value)) {
        const double n = yyjson_get_real(value);
        if (!std::isfinite(n) || n < 0 || n > 9007199254740991.0 || std::floor(n) != n) return false;
        number = static_cast<int64_t>(n);
    } else if (yyjson_is_str(value)) {
        const auto text = Text(value);
        if (text.empty()) return false;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec != std::errc() || end != text.data() + text.size()) return false;
    } else return false;
    return number >= 0 && number <= kCounterLimit;
}
bool SafeText(std::string_view value, size_t limit) {
    if (value.size() > limit) return false;
    for (const unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
bool OptionalText(yyjson_val* object, const char* key, std::string& out, size_t limit = 1024) {
    auto* value = yyjson_obj_get(object, key);
    if (!value || yyjson_is_null(value)) { out.clear(); return true; }
    if (!yyjson_is_str(value) || !SafeText(Text(value), limit)) return false;
    out = Text(value);
    return true;
}
std::string Escape(std::string_view value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}
std::string Secret(const std::string& source, const std::string& token) {
    std::string result = token;
    while (!result.empty() && (result.front() == ' ' || result.front() == '\t')) result.erase(result.begin());
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) result.pop_back();
    const std::string prefix = source == "cursor" ? "WorkosCursorSessionToken=" : "Cloud-IDE-JWT ";
    if (result.starts_with(prefix)) result.erase(0, prefix.size());
    if (result.size() < 16 || result.size() > 16384) return {};
    for (const unsigned char c : result) {
        if (c < 0x21 || c > 0x7e || c == ';' || c == ',' || c == '"' || c == '\\') return {};
    }
    return result;
}
bool Window(int64_t start, int64_t end) {
    return start >= 946684800000LL && end > start && end <= 253402300799999LL && end - start <= 30 * kDay;
}
uint64_t Hash(std::string_view text) {
    uint64_t value = 1469598103934665603ULL;
    for (unsigned char c : text) { value ^= c; value *= 1099511628211ULL; }
    return value;
}
struct Page {
    size_t received = 0, usage = 0;
    std::optional<size_t> total;
    std::optional<bool> hasNext;
    uint64_t signature = 0;
    std::vector<std::string> rows;
};

bool ParsePage(const std::string& source, const std::string& json, size_t pageNumber,
               int64_t start, int64_t end, Page& page) {
    if (json.empty() || json.size() > kPageBytes) return false;
    yyjson_read_err error{};
    Document doc(yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &error), yyjson_doc_free);
    if (!doc) return false;
    auto* root = yyjson_doc_get_root(doc.get());
    if (!Unique(root)) return false;
    const bool cursor = source == "cursor";
    auto* rows = yyjson_obj_get(root, cursor ? "usageEventsDisplay" : "user_usage_group_by_sessions");
    if (!yyjson_is_arr(rows) || yyjson_arr_size(rows) > 1000) return false;
    auto* total = yyjson_obj_get(root, cursor ? "totalUsageEventsCount" : "total");
    if (total && !yyjson_is_null(total)) {
        int64_t value = 0;
        if (!Counter(total, value) || value > 250000) return false;
        page.total = static_cast<size_t>(value);
    }
    if (cursor) {
        auto* pagination = yyjson_obj_get(root, "pagination");
        if (pagination && !yyjson_is_null(pagination)) {
            if (!Unique(pagination)) return false;
            auto* current = yyjson_obj_get(pagination, "currentPage");
            if (current) {
                int64_t number = 0;
                if (!Counter(current, number) || number != static_cast<int64_t>(pageNumber)) return false;
            }
            auto* next = yyjson_obj_get(pagination, "hasNextPage");
            if (next) {
                if (!yyjson_is_bool(next)) return false;
                page.hasNext = yyjson_get_bool(next);
            }
        }
    }
    std::string signature;
    size_t index, count;
    yyjson_val* row;
    yyjson_arr_foreach(rows, index, count, row) {
        ++page.received;
        if (!Unique(row)) return false;
        int64_t timestamp = 0;
        if (!Counter(yyjson_obj_get(row, cursor ? "timestamp" : "usage_time"), timestamp) ||
            timestamp < (cursor ? 946684800000LL : 946684800LL) ||
            timestamp > (cursor ? 253402300799999LL : 253402300799LL)) return false;
        const int64_t milliseconds = cursor ? timestamp : timestamp * 1000;
        std::string model, identity, session;
        if (!OptionalText(row, cursor ? "model" : "model_name", model)) return false;
        if (cursor) {
            for (const char* key : {"id", "requestId", "eventId", "responseId"}) {
                std::string candidate;
                if (!OptionalText(row, key, candidate, 4096)) return false;
                if (identity.empty()) identity = std::move(candidate);
            }
            if (!OptionalText(row, "conversationId", session, 4096)) return false;
        } else {
            if (!OptionalText(row, "session_id", identity, 4096) || identity.empty()) return false;
        }
        auto* usage = yyjson_obj_get(row, cursor ? "tokenUsage" : "extra_info");
        if (!usage || yyjson_is_null(usage)) {
            // Cursor explicitly identifies legacy request-based rows. No conversion from credits.
            auto* isToken = yyjson_obj_get(row, "isTokenBasedCall");
            if (!cursor || !yyjson_is_bool(isToken) || yyjson_get_bool(isToken)) return false;
            signature += identity + "|" + session + "|" + std::to_string(timestamp) + "|legacy;";
            continue;
        }
        if (cursor && identity.empty() && session.empty()) return false;
        if (!Unique(usage)) return false;
        int64_t input = 0, output = 0, read = 0, write = 0;
        if (!Counter(yyjson_obj_get(usage, cursor ? "inputTokens" : "input_token"), input) ||
            !Counter(yyjson_obj_get(usage, cursor ? "outputTokens" : "output_token"), output) ||
            !Counter(yyjson_obj_get(usage, cursor ? "cacheReadTokens" : "cache_read_token"), read, false) ||
            !Counter(yyjson_obj_get(usage, cursor ? "cacheWriteTokens" : "cache_write_token"), write, false) ||
            read > kCounterLimit - input || write > kCounterLimit - input - read) return false;
        std::string serialized = "{";
        if (cursor) {
            if (!identity.empty()) serialized += "\"requestId\":" + Escape(identity) + ",";
            if (!session.empty()) serialized += "\"conversationId\":" + Escape(session) + ",";
            serialized += "\"timestamp\":" + std::to_string(timestamp) + ",\"model\":" + Escape(model) +
                ",\"tokenUsage\":{\"inputTokens\":" + std::to_string(input) +
                ",\"outputTokens\":" + std::to_string(output) +
                ",\"cacheReadTokens\":" + std::to_string(read) +
                ",\"cacheWriteTokens\":" + std::to_string(write) + "}}";
        } else {
            serialized += "\"session_id\":" + Escape(identity) + ",\"usage_time\":" + std::to_string(timestamp) +
                ",\"model_name\":" + Escape(model) + ",\"extra_info\":{\"input_token\":" + std::to_string(input) +
                ",\"output_token\":" + std::to_string(output) + ",\"cache_read_token\":" + std::to_string(read) +
                ",\"cache_write_token\":" + std::to_string(write) + "}}";
        }
        signature += serialized;
        if (milliseconds >= start && milliseconds <= end) {
            if (input || output || read || write) ++page.usage;
            page.rows.push_back(std::move(serialized));
        }
    }
    page.signature = Hash(signature);
    return true;
}

bool Publish(const std::filesystem::path& directory, const std::string& json,
             const std::atomic_bool& cancelled, std::wstring& published) {
    const auto target = directory / L"usage.json";
    const auto pending = directory / (L"usage.pending." + std::to_wstring(GetCurrentProcessId()) +
                                      L"." + std::to_wstring(GetTickCount64()));
    bool written = false;
    {
        FileHandle file(CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) return false;
        size_t offset = 0;
        while (offset < json.size() && !cancelled.load()) {
            DWORD bytes = 0;
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(1024 * 1024, json.size() - offset));
            if (!WriteFile(file.value, json.data() + offset, requested, &bytes, nullptr) || bytes == 0) break;
            offset += bytes;
        }
        written = offset == json.size() && FlushFileBuffers(file.value);
    }
    if (!written || cancelled.load() ||
        !MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        return false;
    }
    published = target.wstring();
    return true;
}
} // namespace

const std::vector<ConnectionProfile>& ConnectionProfiles() {
    static const std::vector<ConnectionProfile> profiles = {
        {"cursor", "Cursor", "https://cursor.com/dashboard/usage",
         "先在自己的 Cursor 网页登录，按 F12 打开开发者工具，在 Network 的本人用量请求头 Cookie 中复制 WorkosCursorSessionToken 的值，粘贴到下方。不要复制其他 Cookie、账号密码或 API Key。"},
        {"trae-code", "TRAE 国际版", "https://www.trae.ai/account-setting",
         "先在自己的 TRAE 国际版网页登录，按 F12 打开开发者工具，在 Network 的本人用量请求头中复制 Authorization 的 Cloud-IDE-JWT 值，粘贴到下方。国内版、TraeWork 和模型 API Key 不适用。"}
    };
    return profiles;
}
const ConnectionProfile* FindConnectionProfile(const std::string& sourceId) {
    for (const auto& profile : ConnectionProfiles()) if (profile.sourceId == sourceId) return &profile;
    return nullptr;
}

ConnectionHttpRequest::~ConnectionHttpRequest() {
    for (auto& header : headers)
        if (!header.second.empty()) SecureZeroMemory(header.second.data(), header.second.size());
}

std::optional<ConnectionHttpRequest> BuildConnectionRequest(const std::string& sourceId,
    const std::string& token, int64_t startMilliseconds, int64_t endMilliseconds,
    size_t page, std::string& error) {
    error.clear();
    if (!FindConnectionProfile(sourceId)) { error = "这个来源暂不支持账号连接，请使用本地日志或真实用量导出。"; return {}; }
    auto secret = Secret(sourceId, token);
    ScopedWipe wipe{secret};
    if (secret.empty()) { error = "请输入有效会话令牌，仅粘贴说明中指定的值，不要附加其他请求头。"; return {}; }
    if (!Window(startMilliseconds, endMilliseconds) || page < 1 || page > 500) {
        error = "同步日期范围无效，每次最多同步 30 天。"; return {};
    }
    ConnectionHttpRequest request;
    request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
    if (sourceId == "cursor") {
        request.url = kCursorUrl;
        request.headers.emplace_back("Cookie", "WorkosCursorSessionToken=" + secret);
        request.headers.emplace_back("Origin", "https://cursor.com");
        request.headers.emplace_back("Referer", "https://cursor.com/dashboard/usage");
        // The reference verifies these dashboard parameters only. Date filtering is local.
        request.body = "{\"teamId\":0,\"page\":" + std::to_string(page) + ",\"pageSize\":500}";
    } else {
        request.url = kTraeUrl;
        request.headers.emplace_back("Authorization", "Cloud-IDE-JWT " + secret);
        request.body = "{\"start_time\":" + std::to_string(startMilliseconds / 1000) +
            ",\"end_time\":" + std::to_string(endMilliseconds / 1000) +
            ",\"page_size\":20,\"page_num\":" + std::to_string(page) + ",\"usage_type\":[5,6]}";
    }
    return request;
}

ConnectionHttpResponse SendConnectionRequest(const ConnectionHttpRequest& request,
                                               const std::atomic_bool& cancelled) {
    ConnectionHttpResponse result;
    auto fail = [&]() { result.networkError = GetLastError(); if (!result.networkError) result.networkError = ERROR_INVALID_DATA; return result; };
    const bool cursor = request.url == kCursorUrl;
    if ((!cursor && request.url != kTraeUrl) || request.method != "POST" ||
        request.body.size() > 4096 || request.maxResponseBytes > kPageBytes ||
        request.timeoutMilliseconds == 0 || request.timeoutMilliseconds > 15000 || cancelled.load()) {
        result.networkError = cancelled.load() ? ERROR_CANCELLED : ERROR_INVALID_PARAMETER;
        return result;
    }
    const wchar_t* host = cursor ? L"cursor.com" : L"api-sg-central.trae.ai";
    const wchar_t* path = cursor ? L"/api/dashboard/get-filtered-usage-events" : L"/trae/api/v1/pay/query_user_usage_group_by_session";
    InternetHandle session(WinHttpOpen(L"TokenScope/0.4.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return fail();
    const int timeout = static_cast<int>(request.timeoutMilliseconds);
    if (!WinHttpSetTimeouts(session.value, std::min(5000, timeout), std::min(5000, timeout), timeout, timeout)) return fail();
    InternetHandle connection(WinHttpConnect(session.value, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return fail();
    InternetHandle handle(WinHttpOpenRequest(connection.value, L"POST", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!handle.value) return fail();
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(handle.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return fail();
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(handle.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) return fail();
    std::wstring headers;
    ScopedWipe wipeHeaders{headers};
    for (const auto& [name, value] : request.headers) {
        if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-") != std::string::npos ||
            !SafeText(value, 20000) || value.find_first_of("\r\n") != std::string::npos) {
            result.networkError = ERROR_INVALID_PARAMETER; return result;
        }
        headers.append(name.begin(), name.end()); headers += L": ";
        headers.append(value.begin(), value.end()); headers += L"\r\n";
    }
    if (cancelled.load()) { result.networkError = ERROR_CANCELLED; return result; }
    if (!WinHttpSendRequest(handle.value, headers.c_str(), static_cast<DWORD>(headers.size()),
            const_cast<char*>(request.body.data()), static_cast<DWORD>(request.body.size()),
            static_cast<DWORD>(request.body.size()), 0) || !WinHttpReceiveResponse(handle.value, nullptr)) return fail();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return fail();
    result.status = status;
    if (status != 200) return result; // Do not even retain authentication/error/redirect response bodies.
    std::array<char, 65536> buffer{};
    for (;;) {
        if (cancelled.load()) { result.body.clear(); result.networkError = ERROR_CANCELLED; return result; }
        DWORD bytes = 0;
        if (!WinHttpReadData(handle.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes)) return fail();
        if (!bytes) break;
        if (bytes > request.maxResponseBytes - std::min(request.maxResponseBytes, result.body.size())) {
            result.body.clear(); result.networkError = ERROR_FILE_TOO_LARGE; return result;
        }
        result.body.append(buffer.data(), bytes);
    }
    return result;
}

ConnectionSyncResult SyncConnection(const std::string& sourceId, const std::string& token,
    const std::wstring& destinationDirectory, const std::atomic_bool& cancelled) {
    return SyncConnection(sourceId, token, destinationDirectory, cancelled, {}, SendConnectionRequest);
}

ConnectionSyncResult SyncConnection(const std::string& sourceId, const std::string& token,
    const std::wstring& destinationDirectory, const std::atomic_bool& cancelled,
    const ConnectionSyncOptions& options, const ConnectionTransport& transport) {
    ConnectionSyncResult result;
    auto fail = [&](const char* error) { result.error = error; result.cancelled = cancelled.load(); return result; };
    try {
        if (cancelled.load()) return fail("同步已取消，已有用量缓存已保留。");
        if (!FindConnectionProfile(sourceId)) return fail("这个来源暂不支持账号连接，请使用本地日志或真实用量导出。");
        auto secret = Secret(sourceId, token);
        ScopedWipe wipeSecret{secret};
        if (secret.empty()) return fail("请输入有效会话令牌；尚未发送联网请求。");
        if (destinationDirectory.empty() || !transport || !options.maxPages || options.maxPages > 500 ||
            options.maxResponseBytes < 256 || options.maxResponseBytes > 64 * 1024 * 1024 ||
            !options.timeoutMilliseconds || options.timeoutMilliseconds > 120000)
            return fail("同步限制或用量缓存目录无效。");
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const int64_t end = options.endMilliseconds ? options.endMilliseconds :
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        const int64_t start = options.startMilliseconds ? options.startMilliseconds : end - 30 * kDay;
        if (!Window(start, end)) return fail("同步日期范围无效，每次最多同步 30 天。");
        const std::filesystem::path directory(destinationDirectory);
        std::error_code diskError;
        std::filesystem::create_directories(directory, diskError);
        if (diskError) return fail("无法创建本机用量缓存目录。");
        FileHandle lock(CreateFileW((directory / L"usage.sync.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) return fail("正在执行另一次同步，或无法访问用量缓存目录。");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMilliseconds);
        size_t bytes = 0;
        std::optional<size_t> total;
        std::set<uint64_t> pages;
        std::vector<std::string> rows;
        bool completed = false;
        for (size_t pageNumber = 1; pageNumber <= options.maxPages; ++pageNumber) {
            if (cancelled.load()) return fail("同步已取消，已有用量缓存已保留。");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return fail("同步超时，已有用量缓存已保留。");
            std::string planError;
            auto request = BuildConnectionRequest(sourceId, token, start, end, pageNumber, planError);
            if (!request) return fail("无法创建厂商用量查询请求。");
            request->timeoutMilliseconds = static_cast<unsigned>(std::min<int64_t>(15000, remaining));
            if (bytes >= options.maxResponseBytes) return fail("下载用量超过大小限制，未保存不完整数据。");
            request->maxResponseBytes = std::min(kPageBytes, options.maxResponseBytes - bytes);
            auto response = transport(*request, cancelled);
            ++result.pages;
            if (cancelled.load()) return fail("同步已取消，已有用量缓存已保留。");
            if (response.networkError) return fail("用量请求失败或超时，已有缓存已保留。");
            if (response.status == 401 || response.status == 403) {
                result.authenticationFailed = true;
                return fail("会话过期、无访问权限或需要登录验证，请重新登录后输入新的会话令牌。");
            }
            if (response.status != 200) {
                result.error = "用量服务返回 HTTP " + std::to_string(response.status) + "，已有缓存已保留。";
                return result;
            }
            if (response.body.size() > request->maxResponseBytes)
                return fail("下载用量超过大小限制，未保存不完整数据。");
            bytes += response.body.size();
            Page page;
            if (!ParsePage(sourceId, response.body, pageNumber, start, end, page))
                return fail("用量接口格式变化或数据无效，未保存不完整数据。");
            if (page.total) {
                if (total && total != page.total) return fail("分页过程中用量发生变化，请重试获取完整数据。");
                total = page.total;
            }
            if (page.received && !pages.insert(page.signature).second)
                return fail("用量服务重复返回同一页，未保存不完整数据。");
            result.records += page.received;
            result.usageRecords += page.usage;
            for (auto& row : page.rows) {
                if (row.find(secret) != std::string::npos)
                    return fail("用量响应包含敏感内容，未保存缓存。");
                rows.push_back(std::move(row));
            }
            if (total) {
                if (result.records > *total || (!page.received && result.records < *total) ||
                    (page.hasNext && !*page.hasNext && result.records < *total) ||
                    (page.hasNext && *page.hasNext && result.records == *total))
                    return fail("用量分页不完整或相互矛盾，未保存不完整数据。");
                completed = result.records == *total;
            } else if (page.hasNext) completed = !*page.hasNext;
            else completed = page.received == 0 || (sourceId == "cursor" && page.received < 500);
            if (completed) break;
            if (sourceId == "trae-code") {
                for (int i = 0; i < 6 && !cancelled.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!completed) return fail("用量分页超过页数限制，未保存不完整数据。");
        if (std::chrono::steady_clock::now() >= deadline) return fail("同步超时，已有用量缓存已保留。");
        std::string json = sourceId == "cursor" ? "{\"usageEventsDisplay\":[" : "[";
        for (size_t i = 0; i < rows.size(); ++i) { if (i) json += ','; json += rows[i]; }
        json += sourceId == "cursor" ? "]}" : "]";
        if (json.size() > options.maxResponseBytes) return fail("用量缓存超过大小限制，已有缓存已保留。");
        if (!Publish(directory, json, cancelled, result.cachePath))
            return fail("无法保存用量缓存或同步已取消，已有缓存已保留。");
        result.success = true;
        return result;
    } catch (...) {
        // Exception text may include paths, headers or response bytes. Never propagate it.
        return fail("同步失败，已有用量缓存已保留。");
    }
}

} // namespace tokenscope
