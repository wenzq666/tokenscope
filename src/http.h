#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace tokenscope {

// Shared synchronous WinHTTP client for the few fixed HTTPS endpoints TokenScope talks to.
struct HttpsRequest {
    std::wstring host, path;
    const wchar_t* method = L"GET";
    // Sensitive when used for account sync: callers must never log this request.
    std::wstring headers;
    std::string_view body;
    size_t maxResponseBytes = 1024 * 1024;
    int resolveTimeout = 5000, connectTimeout = 5000, sendTimeout = 8000, receiveTimeout = 10000;
    // Disables redirects, cookies and automatic authentication (used for credentialed requests).
    bool hardened = false;
};
struct HttpsResponse {
    unsigned status = 0;
    std::string body;          // Untrusted. Only filled for HTTP 200.
    unsigned networkError = 0; // Windows error code; 0 when a status was received.
};

// Splits "https://host/path" into host and path. Rejects other schemes, ports and empty hosts.
bool SplitHttpsUrl(std::string_view url, std::wstring& host, std::wstring& path);

// Product user agent, e.g. "TokenScope/0.5.0 (public reference data)".
std::wstring HttpUserAgent(const wchar_t* comment = nullptr);

// Performs the request. Checks `cancelled` between reads; CancelHttpRequests() additionally
// aborts calls that are blocked inside WinHTTP so shutdown never waits for network timeouts.
HttpsResponse SendHttps(const HttpsRequest& request, const std::atomic_bool& cancelled);

// Closes every in-flight request handle. Safe to call from any thread, any number of times.
void CancelHttpRequests();

} // namespace tokenscope
