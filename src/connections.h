#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tokenscope {

struct ConnectionProfile {
    std::string sourceId, name, websiteUrl, credentialHint;
};
// Only these sources have an implemented, read-only authenticated usage sync.
const std::vector<ConnectionProfile>& ConnectionProfiles();
const ConnectionProfile* FindConnectionProfile(const std::string& sourceId);

struct ConnectionHttpRequest {
    ConnectionHttpRequest() = default;
    ConnectionHttpRequest(const ConnectionHttpRequest&) = default;
    ConnectionHttpRequest(ConnectionHttpRequest&&) noexcept = default;
    ConnectionHttpRequest& operator=(const ConnectionHttpRequest&) = default;
    ConnectionHttpRequest& operator=(ConnectionHttpRequest&&) noexcept = default;
    ~ConnectionHttpRequest(); // Best-effort secure erasure of owned header copies.
    std::string url, method = "POST", body;
    // Sensitive: never log this structure. Credentials occur only in headers.
    std::vector<std::pair<std::string, std::string>> headers;
    size_t maxResponseBytes = 8 * 1024 * 1024;
    unsigned timeoutMilliseconds = 15000;
};
struct ConnectionHttpResponse {
    unsigned status = 0;
    std::string body; // Untrusted. Never expose the body in diagnostics.
    unsigned networkError = 0; // Windows error code, never server-provided text.
};
using ConnectionTransport = std::function<ConnectionHttpResponse(
    const ConnectionHttpRequest&, const std::atomic_bool& cancelled)>;

struct ConnectionSyncOptions {
    // Zero end means now; zero start means end minus 30 days. Max window is 30 days.
    int64_t startMilliseconds = 0, endMilliseconds = 0;
    size_t maxPages = 100, maxResponseBytes = 32 * 1024 * 1024;
    unsigned timeoutMilliseconds = 120000;
};
struct ConnectionSyncResult {
    bool success = false, cancelled = false, authenticationFailed = false;
    size_t pages = 0, records = 0, usageRecords = 0;
    std::wstring cachePath;
    std::string error; // Fixed diagnostics plus numeric HTTP status; never raw response/secrets.
};

// Pure request planning, suitable for a fixture transport. Dates are epoch milliseconds.
std::optional<ConnectionHttpRequest> BuildConnectionRequest(
    const std::string& sourceId, const std::string& token,
    int64_t startMilliseconds, int64_t endMilliseconds, size_t page, std::string& error);

ConnectionHttpResponse SendConnectionRequest(const ConnectionHttpRequest& request,
                                               const std::atomic_bool& cancelled);

// Atomically publishes destinationDirectory/usage.json only after complete pagination.
// Cache schema remains compatible with the existing Cursor / TRAE native cache collectors.
ConnectionSyncResult SyncConnection(const std::string& sourceId, const std::string& token,
    const std::wstring& destinationDirectory, const std::atomic_bool& cancelled);
ConnectionSyncResult SyncConnection(const std::string& sourceId, const std::string& token,
    const std::wstring& destinationDirectory, const std::atomic_bool& cancelled,
    const ConnectionSyncOptions& options, const ConnectionTransport& transport);

} // namespace tokenscope
