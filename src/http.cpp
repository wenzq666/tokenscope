#include "http.h"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace tokenscope {
namespace {

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() {
        if (value) WinHttpCloseHandle(value);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

// WinHTTP documents that closing a request handle from another thread terminates a pending
// synchronous call. The registry hands ownership of the handle to whichever side closes first.
std::mutex activeMutex;
std::vector<HINTERNET> activeRequests;

class ActiveRequest {
    HINTERNET handle_;

public:
    explicit ActiveRequest(HINTERNET handle) : handle_(handle) {
        std::lock_guard lock(activeMutex);
        activeRequests.push_back(handle_);
    }
    ~ActiveRequest() {
        std::lock_guard lock(activeMutex);
        auto found = std::find(activeRequests.begin(), activeRequests.end(), handle_);
        if (found == activeRequests.end()) return; // Already closed by CancelHttpRequests.
        activeRequests.erase(found);
        WinHttpCloseHandle(handle_);
    }
    ActiveRequest(const ActiveRequest&) = delete;
    ActiveRequest& operator=(const ActiveRequest&) = delete;
    HINTERNET get() const { return handle_; }
};

HttpsResponse Failure(unsigned code = 0) {
    HttpsResponse response;
    response.networkError = code ? code : GetLastError();
    if (!response.networkError) response.networkError = ERROR_INVALID_DATA;
    return response;
}

} // namespace

bool SplitHttpsUrl(std::string_view url, std::wstring& host, std::wstring& path) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) return false;
    url.remove_prefix(scheme.size());
    const auto slash = url.find('/');
    const auto hostPart = url.substr(0, slash);
    if (hostPart.empty() || hostPart.find_first_of(":@?#") != std::string_view::npos) return false;
    for (unsigned char ch : url)
        if (ch <= 32 || ch >= 127) return false;
    host.assign(hostPart.begin(), hostPart.end());
    if (slash == std::string_view::npos)
        path = L"/";
    else
        path.assign(url.begin() + static_cast<std::ptrdiff_t>(slash), url.end());
    return true;
}

std::wstring HttpUserAgent(const wchar_t* comment) {
    std::wstring agent = L"TokenScope/" TOKENSCOPE_VERSION_WSTRING;
    if (comment && *comment) agent += std::wstring(L" (") + comment + L")";
    return agent;
}

HttpsResponse SendHttps(const HttpsRequest& request, const std::atomic_bool& cancelled) {
    if (cancelled.load()) return Failure(ERROR_CANCELLED);
    if (request.host.empty() || request.path.empty() || request.path.front() != L'/' || !request.method ||
        request.body.size() > MAXDWORD)
        return Failure(ERROR_INVALID_PARAMETER);
    InternetHandle session(WinHttpOpen(HttpUserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return Failure();
    if (!WinHttpSetTimeouts(session.value, request.resolveTimeout, request.connectTimeout, request.sendTimeout,
                            request.receiveTimeout))
        return Failure();
    InternetHandle connection(WinHttpConnect(session.value, request.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return Failure();
    HINTERNET raw = WinHttpOpenRequest(connection.value, request.method, request.path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!raw) return Failure();
    ActiveRequest handle(raw);
    if (request.hardened) {
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
            return Failure();
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        if (!WinHttpSetOption(handle.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)))
            return Failure();
    }
    if (cancelled.load()) return Failure(ERROR_CANCELLED);
    const auto bodySize = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(
            handle.get(), request.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : request.headers.c_str(),
            request.headers.empty() ? 0 : static_cast<DWORD>(request.headers.size()),
            bodySize ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA, bodySize, bodySize, 0) ||
        !WinHttpReceiveResponse(handle.get(), nullptr))
        return Failure(cancelled.load() ? ERROR_CANCELLED : 0);
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        return Failure();
    HttpsResponse response;
    response.status = status;
    if (status != 200) return response; // Never retain error, redirect or authentication bodies.
    std::array<char, 65536> buffer{};
    for (;;) {
        if (cancelled.load()) return Failure(ERROR_CANCELLED);
        DWORD bytes = 0;
        if (!WinHttpReadData(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes))
            return Failure(cancelled.load() ? ERROR_CANCELLED : 0);
        if (!bytes) break;
        if (bytes > request.maxResponseBytes - std::min(request.maxResponseBytes, response.body.size()))
            return Failure(ERROR_FILE_TOO_LARGE);
        response.body.append(buffer.data(), bytes);
    }
    return response;
}

void CancelHttpRequests() {
    std::lock_guard lock(activeMutex);
    for (HINTERNET handle : activeRequests) WinHttpCloseHandle(handle);
    activeRequests.clear();
}

} // namespace tokenscope
