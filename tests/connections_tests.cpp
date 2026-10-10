#include "../src/connections.h"
#include "../src/adapters/extended.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace tokenscope;
namespace fs = std::filesystem;
namespace {
constexpr int64_t kEnd = 1780000000000LL;
const std::string kCursorSecret = "user_fixture%3A%3AeyJ.fixture.not-a-real-session";
const std::string kTraeSecret = "eyJhbGciOiJIUzI1NiJ9.eyJleHAiOjE5MDAwMDAwMDB9.not-a-real-jwt";
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
    Check(out.good(), "fixture cache write failed");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string Header(const ConnectionHttpRequest& request, const std::string& name) {
    for (const auto& header : request.headers)
        if (header.first == name) return header.second;
    return {};
}
ConnectionSyncOptions Options() {
    ConnectionSyncOptions options;
    options.endMilliseconds = kEnd;
    return options;
}
std::string CursorRow(const std::string& id, int64_t time = kEnd - 1000) {
    return "{\"requestId\":\"" + id + "\",\"conversationId\":\"session-fixture\",\"timestamp\":\"" +
           std::to_string(time) +
           "\",\"model\":\"Claude Sonnet 4.6\",\"tokenUsage\":{\"inputTokens\":100,"
           "\"outputTokens\":50,\"cacheReadTokens\":20,\"cacheWriteTokens\":30},\"prompt\":\"private-chat\","
           "\"authorization\":\"" +
           kCursorSecret + "\"}";
}
std::string CursorPage(const std::string& rows, const std::string& total = {}) {
    return "{\"usageEventsDisplay\":[" + rows + "]" + (total.empty() ? "" : ",\"totalUsageEventsCount\":" + total) +
           "}";
}
std::string TraeRow(const std::string& id) {
    return "{\"session_id\":\"" + id + "\",\"usage_time\":" + std::to_string(kEnd / 1000 - 1) +
           ",\"model_name\":\"GPT-5.4\",\"extra_info\":{\"input_token\":10,\"output_token\":5,"
           "\"cache_read_token\":3,\"cache_write_token\":2},\"message\":\"private-chat\"}";
}
std::string TraePage(const std::string& rows, const std::string& total = {}) {
    return "{\"user_usage_group_by_sessions\":[" + rows + "]" + (total.empty() ? "" : ",\"total\":" + total) + "}";
}
struct OwnedFixture {
    fs::path root =
        fs::absolute(fs::path(L"build") / (L"connections-test-fixture-" + std::to_wstring(GetCurrentProcessId()) +
                                           L"-" + std::to_wstring(GetTickCount64())));
    ~OwnedFixture() {
        // Recursive cleanup is confined to this unique test-owned workspace directory.
        const auto parent = fs::absolute(fs::path(L"build")).lexically_normal();
        if (root.lexically_normal().parent_path() == parent &&
            root.filename().wstring().starts_with(L"connections-test-fixture-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
};
std::vector<ParsedUsage> NativeRows(const std::string& source, const fs::path& cache) {
    std::atomic_bool cancelled{false};
    std::vector<ParsedUsage> result;
    const auto scan =
        CollectExtended(source, cache.wstring(), cancelled, [&](const ParsedUsage& usage) { result.push_back(usage); });
    Check(scan.invalid == 0 && scan.error.empty(), "published cache is not compatible with the native collector");
    return result;
}
void FailurePreserves(const fs::path& directory, const std::string& source, const ConnectionTransport& transport,
                      ConnectionSyncOptions options = Options()) {
    Write(directory / L"usage.json", "previous valid cache");
    std::atomic_bool cancelled{false};
    auto result = SyncConnection(source, source == "cursor" ? kCursorSecret : kTraeSecret, directory.wstring(),
                                 cancelled, options, transport);
    Check(!result.success && result.cachePath.empty() && !result.error.empty(), "failure became a connected result");
    Check(result.error.find(kCursorSecret) == std::string::npos &&
              result.error.find(kTraeSecret) == std::string::npos &&
              result.error.find("server-private") == std::string::npos,
          "diagnostic leaked a credential or server response");
    Check(Read(directory / L"usage.json") == "previous valid cache", "failed sync replaced the previous valid cache");
    for (const auto& item : fs::directory_iterator(directory))
        Check(!item.path().filename().wstring().starts_with(L"usage.pending."),
              "failed atomic write retained a partial cache");
}
} // namespace

int main() {
    try {
        OwnedFixture fixture;
        std::atomic_bool cancelled{false};
        std::string error;
        Check(ConnectionProfiles().size() == 2 && FindConnectionProfile("cursor") &&
                  FindConnectionProfile("trae-code") && !FindConnectionProfile("trae-cn") &&
                  !FindConnectionProfile("copilot"),
              "unimplemented source advertised an account connection");
        auto request = BuildConnectionRequest("cursor", kCursorSecret, kEnd - 30LL * 86400000, kEnd, 2, error);
        Check(request && request->url == "https://cursor.com/api/dashboard/get-filtered-usage-events" &&
                  request->body == "{\"teamId\":0,\"page\":2,\"pageSize\":500}" &&
                  Header(*request, "Cookie") == "WorkosCursorSessionToken=" + kCursorSecret &&
                  Header(*request, "Origin") == "https://cursor.com" && Header(*request, "Authorization").empty() &&
                  request->body.find(kCursorSecret) == std::string::npos,
              "Cursor request identity or secret placement is wrong");
        auto prefixed = BuildConnectionRequest("cursor", "WorkosCursorSessionToken=" + kCursorSecret, kEnd - 86400000,
                                               kEnd, 1, error);
        Check(prefixed && Header(*prefixed, "Cookie") == Header(*request, "Cookie"),
              "Cursor cookie prefix was doubled");
        auto trae =
            BuildConnectionRequest("trae-code", "Cloud-IDE-JWT " + kTraeSecret, kEnd - 86400000, kEnd, 1, error);
        Check(trae && trae->url == "https://api-sg-central.trae.ai/trae/api/v1/pay/query_user_usage_group_by_session" &&
                  Header(*trae, "Authorization") == "Cloud-IDE-JWT " + kTraeSecret && Header(*trae, "Cookie").empty() &&
                  trae->body.find("\"start_time\":1779913600") != std::string::npos &&
                  trae->body.find("\"usage_type\":[5,6]") != std::string::npos &&
                  trae->body.find(kTraeSecret) == std::string::npos,
              "TRAE JWT scheme, seconds or usage types are wrong");
        for (const auto& token : {std::string(), std::string("short"), kCursorSecret + "\r\nX-Test: leak",
                                  kCursorSecret + "; other=cookie"})
            Check(!BuildConnectionRequest("cursor", token, kEnd - 86400000, kEnd, 1, error),
                  "unsafe credential accepted");
        Check(!BuildConnectionRequest("cursor", kCursorSecret, kEnd - 31LL * 86400000, kEnd, 1, error) &&
                  !BuildConnectionRequest("cursor", kCursorSecret, kEnd, kEnd, 1, error) &&
                  !BuildConnectionRequest("cursor", kCursorSecret, kEnd - 86400000, kEnd, 0, error),
              "invalid date or page accepted");
        auto unsafe = *request;
        unsafe.url = "https://untrusted.invalid/usage";
        Check(SendConnectionRequest(unsafe, cancelled).networkError != 0,
              "native transport accepted an arbitrary credential destination");
        size_t calls = 0;
        auto noNetwork = [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            ++calls;
            return ConnectionHttpResponse{200, CursorPage("", "0"), 0};
        };
        auto missing =
            SyncConnection("cursor", "", (fixture.root / L"no-token").wstring(), cancelled, Options(), noNetwork);
        Check(!missing.success && calls == 0 && !fs::exists(fixture.root / L"no-token"),
              "missing credential sent a request or wrote a cache");
        auto unsupported =
            SyncConnection("copilot", kCursorSecret, fixture.root.wstring(), cancelled, Options(), noNetwork);
        Check(!unsupported.success && calls == 0, "unsupported OAuth connection was faked");

        // Small server-clamped pages must still reach the authoritative total.
        calls = 0;
        const auto cursorDir = fixture.root / L"cursor";
        auto cursorResult =
            SyncConnection("cursor", kCursorSecret, cursorDir.wstring(), cancelled, Options(),
                           [&](const ConnectionHttpRequest& planned, const std::atomic_bool&) {
                               ++calls;
                               Check(planned.body.find("\"page\":" + std::to_string(calls)) != std::string::npos,
                                     "page number did not advance");
                               if (calls == 1)
                                   return ConnectionHttpResponse{200, CursorPage(CursorRow("request-1"), "3"), 0};
                               if (calls == 2)
                                   return ConnectionHttpResponse{200, CursorPage(CursorRow("request-2"), "3"), 0};
                               return ConnectionHttpResponse{
                                   200, CursorPage(CursorRow("outside-window", kEnd - 31LL * 86400000), "3"), 0};
                           });
        Check(cursorResult.success && calls == 3 && cursorResult.pages == 3 && cursorResult.records == 3 &&
                  cursorResult.usageRecords == 2,
              "Cursor pagination or local 30-day filter failed");
        const auto cache = Read(cursorResult.cachePath);
        Check(cache.find("private-chat") == std::string::npos && cache.find(kCursorSecret) == std::string::npos &&
                  cache.find("outside-window") == std::string::npos,
              "cache retained chat, credential or out-of-range metadata");
        auto rows = NativeRows("cursor", cursorResult.cachePath);
        Check(rows.size() == 2 && rows[0].input == 150 && rows[0].output == 50 && rows[0].cachedInput == 20 &&
                  rows[0].cacheWrite == 30 && rows[0].model == "Claude Sonnet 4.6" &&
                  rows[0].key == "cursor:request:request-1",
              "Cursor native cache lost raw model, identity or token buckets");
        const auto fallbackRow =
            R"({"conversationId":"same-conversation","timestamp":"1779999999000","model":"auto","tokenUsage":{"inputTokens":"10","outputTokens":2}})";
        auto fallback = SyncConnection(
            "cursor", kCursorSecret, (fixture.root / L"cursor-fallback").wstring(), cancelled, Options(),
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{200, CursorPage(std::string(fallbackRow) + "," + fallbackRow, "2"), 0};
            });
        Check(fallback.success, "Cursor fallback identity sync failed");
        rows = NativeRows("cursor", fallback.cachePath);
        Check(rows.size() == 2 && rows[0].key != rows[1].key && rows[0].model.empty(),
              "indistinguishable same-page requests collided or Auto became an invented model");

        calls = 0;
        auto traeResult =
            SyncConnection("trae-code", kTraeSecret, (fixture.root / L"trae").wstring(), cancelled, Options(),
                           [&](const ConnectionHttpRequest& planned, const std::atomic_bool&) {
                               ++calls;
                               Check(Header(planned, "Authorization") == "Cloud-IDE-JWT " + kTraeSecret,
                                     "TRAE token lost on later pages");
                               return ConnectionHttpResponse{
                                   200, TraePage(calls < 3 ? TraeRow("session-" + std::to_string(calls)) : ""), 0};
                           });
        Check(traeResult.success, "TRAE fixture sync failed");
        rows = NativeRows("trae-code", traeResult.cachePath);
        Check(calls == 3 && rows.size() == 2 && rows[0].input == 15 && rows[0].output == 5 &&
                  rows[0].cachedInput == 3 && rows[0].cacheWrite == 2 && rows[0].model == "GPT-5.4",
              "TRAE omitted total caused truncation or native buckets changed");
        Check(Read(traeResult.cachePath).find("private-chat") == std::string::npos, "TRAE cache retained chat text");
        auto empty = SyncConnection("cursor", kCursorSecret, (fixture.root / L"empty").wstring(), cancelled, Options(),
                                    noNetwork);
        Check(empty.success && empty.records == 0 && empty.usageRecords == 0 &&
                  Read(empty.cachePath) == "{\"usageEventsDisplay\":[]}",
              "valid empty account was confused with schema failure");
        auto legacy = SyncConnection(
            "cursor", kCursorSecret, (fixture.root / L"legacy").wstring(), cancelled, Options(),
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{
                    200,
                    CursorPage(
                        R"({"timestamp":"1779999999000","model":"old-model","isTokenBasedCall":false,"chargedCents":8})",
                        "1"),
                    0};
            });
        Check(legacy.success && legacy.records == 1 && legacy.usageRecords == 0,
              "explicit legacy request counts became tokens");

        const auto failures = fixture.root / L"failures";
        for (unsigned status : {401u, 403u, 302u, 429u, 500u}) {
            FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{status, "server-private " + kCursorSecret, 0};
            });
        }
        for (
            const auto& body :
            {std::string("<html>server-private</html>"), std::string("{}"),
             std::string("{\"usageEventsDisplay\":null}"),
             std::string("{\"usageEventsDisplay\":[],\"totalUsageEventsCount\":-1}"),
             CursorPage(R"({"timestamp":"1779999999000","model":"x","tokenUsage":{"inputTokens":1,"outputTokens":2}})",
                        "1"),
             CursorPage(R"({"requestId":"x","timestamp":1779999999,"tokenUsage":{"inputTokens":1,"outputTokens":2}})",
                        "1"),
             CursorPage(
                 R"({"requestId":"x","timestamp":"1779999999000","tokenUsage":{"inputTokens":-1,"outputTokens":2}})",
                 "1"),
             CursorPage(
                 R"({"requestId":"x","timestamp":"1779999999000","tokenUsage":{"inputTokens":1.5,"outputTokens":2}})",
                 "1"),
             CursorPage(
                 R"({"requestId":"x","timestamp":"1779999999000","tokenUsage":{"inputTokens":1,"inputTokens":2,"outputTokens":2}})",
                 "1"),
             CursorPage(R"({"requestId":"x","timestamp":"1779999999000","tokenUsage":{"totalTokens":8}})", "1"),
             CursorPage(R"({"requestId":"x","timestamp":"1779999999000","isTokenBasedCall":true})", "1"),
             CursorPage(CursorRow("x"), "0"), CursorPage("", "1"), CursorPage("", "0") + "{}"}) {
            FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{200, body, 0};
            });
        }
        FailurePreserves(failures, "trae-code", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{
                200, TraePage(R"({"session_id":"x","usage_time":1779999999,"dollar_float":1.0})", "1"), 0};
        });
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{
                200,
                "{\"usageEventsDisplay\":[" + CursorRow("flag") +
                    "],\"totalUsageEventsCount\":2,\"pagination\":{\"currentPage\":1,\"hasNextPage\":false}}",
                0};
        });
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{200, "{\"usageEventsDisplay\":[],\"pagination\":{\"currentPage\":2}}", 0};
        });
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{200, "server-private " + kCursorSecret, 12002};
        });
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{200,
                                          CursorPage("{\"requestId\":\"x\",\"timestamp\":1779999999000,\"model\":\"" +
                                                         kCursorSecret +
                                                         "\",\"tokenUsage\":{\"inputTokens\":1,\"outputTokens\":2}}",
                                                     "1"),
                                          0};
        });
        calls = 0;
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            ++calls;
            return ConnectionHttpResponse{200, calls == 1 ? CursorPage(CursorRow("x"), "2") : CursorPage("", "2"), 0};
        });
        Check(calls == 2, "incomplete advertised total was not detected on the next page");
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            return ConnectionHttpResponse{200, CursorPage(CursorRow("same-page"), "2"), 0};
        });
        calls = 0;
        FailurePreserves(failures, "cursor", [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
            ++calls;
            return ConnectionHttpResponse{
                200, CursorPage(CursorRow("request-" + std::to_string(calls)), calls == 1 ? "2" : "3"), 0};
        });
        auto pageLimit = Options();
        pageLimit.maxPages = 1;
        FailurePreserves(
            failures, "cursor",
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{200, CursorPage(CursorRow("cap"), "2"), 0};
            },
            pageLimit);
        auto sizeLimit = Options();
        sizeLimit.maxResponseBytes = 256;
        FailurePreserves(
            failures, "cursor",
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                return ConnectionHttpResponse{200, std::string(257, ' '), 0};
            },
            sizeLimit);
        auto cumulativeLimit = Options();
        cumulativeLimit.maxResponseBytes = 512;
        calls = 0;
        FailurePreserves(
            failures, "cursor",
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                ++calls;
                return ConnectionHttpResponse{200, CursorPage(CursorRow("size-" + std::to_string(calls)), "2"), 0};
            },
            cumulativeLimit);
        Check(calls == 2, "byte limit was not enforced cumulatively across pages");
        FailurePreserves(failures, "cursor",
                         [&](const ConnectionHttpRequest&, const std::atomic_bool&) -> ConnectionHttpResponse {
                             throw std::runtime_error(kCursorSecret);
                         });
        auto timeLimit = Options();
        timeLimit.timeoutMilliseconds = 1;
        FailurePreserves(
            failures, "cursor",
            [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                return ConnectionHttpResponse{200, CursorPage("", "0"), 0};
            },
            timeLimit);
        Write(failures / L"usage.json", "previous valid cache");
        auto cancelledResult =
            SyncConnection("cursor", kCursorSecret, failures.wstring(), cancelled, Options(),
                           [&](const ConnectionHttpRequest&, const std::atomic_bool&) {
                               cancelled.store(true);
                               return ConnectionHttpResponse{200, CursorPage(CursorRow("x"), "1"), 0};
                           });
        Check(!cancelledResult.success && cancelledResult.cancelled &&
                  Read(failures / L"usage.json") == "previous valid cache",
              "cancellation published a partial cache");
        calls = 0;
        auto alreadyCancelled =
            SyncConnection("cursor", kCursorSecret, failures.wstring(), cancelled, Options(), noNetwork);
        Check(alreadyCancelled.cancelled && calls == 0, "cancelled sync sent another request");
        cancelled.store(false);
        // A Windows open handle disallowing delete deterministically prevents atomic replacement.
        HANDLE locked = CreateFileW((failures / L"usage.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "cannot lock atomic-write fixture");
        auto diskFailure = SyncConnection("cursor", kCursorSecret, failures.wstring(), cancelled, Options(), noNetwork);
        CloseHandle(locked);
        Check(!diskFailure.success && Read(failures / L"usage.json") == "previous valid cache",
              "failed replacement destroyed previous cache");
        for (const auto& item : fs::directory_iterator(failures))
            Check(!item.path().filename().wstring().starts_with(L"usage.pending."),
                  "replacement failure retained temporary response data");
        HANDLE syncLock = CreateFileW((failures / L"usage.sync.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        Check(syncLock != INVALID_HANDLE_VALUE, "cannot acquire concurrent-sync fixture lock");
        calls = 0;
        auto busy = SyncConnection("cursor", kCursorSecret, failures.wstring(), cancelled, Options(), noNetwork);
        CloseHandle(syncLock);
        Check(!busy.success && calls == 0 && Read(failures / L"usage.json") == "previous valid cache",
              "overlapping sync bypassed its directory lock");
        std::cout << "connections_tests PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "connections_tests FAIL: " << e.what() << '\n';
        return 1;
    }
}
