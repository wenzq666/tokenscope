#include "../adapters.h"
#include "sqlite3.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace tokenscope {
namespace fs = std::filesystem;
namespace {
constexpr int64_t kLimit = INT64_MAX / 8;
constexpr size_t kLineLimit = 16 * 1024 * 1024;
constexpr uintmax_t kDocumentLimit = 64 * 1024 * 1024;
yyjson_val* Obj(yyjson_val* o, const char* key) {
    auto v = yyjson_obj_get(o, key); return yyjson_is_obj(v) ? v : nullptr;
}
bool Add(int64_t& n, int64_t other) {
    if (other < 0 || n < 0 || other > kLimit - n) return false;
    n += other; return true;
}
std::string Digest(const std::string& s) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : s) { hash ^= c; hash *= 1099511628211ULL; }
    return std::to_string(hash);
}
void Finish(ParsedUsage& u, const UsageEmitter& emit, int64_t& invalid) {
    if (u.key.empty() || u.timestamp.empty() || u.key.size() > 4096 ||
        u.model.size() > 1024 || u.provider.size() > 1024 ||
        u.input < 0 || u.output < 0 || u.input > kLimit || u.output > kLimit ||
        u.unclassified < 0 || u.unclassified > kLimit ||
        u.cachedInput < 0 || u.cacheWrite < 0 || u.reasoning < 0 ||
        u.cachedInput > u.input || u.cacheWrite > u.input - u.cachedInput ||
        u.reasoning > u.output) { ++invalid; return; }
    emit(u);
}
std::string Epoch(yyjson_val* root, const char* key, bool seconds) {
    auto v = yyjson_obj_get(root, key);
    if (!yyjson_is_num(v)) return {};
    const double number = yyjson_get_num(v) * (seconds ? 1000.0 : 1.0);
    if (!std::isfinite(number) || number < 0 || number > 253402300799000.0) return {};
    return IsoFromUnixMilliseconds(static_cast<int64_t>(number));
}
void GeminiMessage(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") != "gemini") return;
    auto tokens = Obj(root, "tokens"); if (!tokens) return;
    ParsedUsage u; u.key = JsonString(root, "id");
    if (!u.key.empty()) u.key = "gemini-cli:message:" + u.key;
    u.timestamp = JsonString(root, "timestamp"); u.model = JsonString(root, "model");
    int64_t thoughts = 0;
    if (!JsonCounter(tokens, "input", u.input, true) ||
        !JsonCounter(tokens, "output", u.output, true) ||
        !JsonCounter(tokens, "cached", u.cachedInput) ||
        !JsonCounter(tokens, "thoughts", thoughts) || !Add(u.output, thoughts)) {
        ++invalid; return;
    }
    // Google's candidatesTokenCount excludes thoughtsTokenCount; totalTokenCount
    // is prompt + candidates + thoughts. Cached/tool prompts are already input.
    u.reasoning = thoughts;
    Finish(u, emit, invalid);
}
void OpenCodeMessage(yyjson_val* root, const std::string& rowId,
                     const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "role") != "assistant") return;
    auto tokens = Obj(root, "tokens"); if (!tokens) return;
    ParsedUsage u; u.key = JsonString(root, "id"); if (u.key.empty()) u.key = rowId;
    if (!u.key.empty()) u.key = "opencode:message:" + u.key;
    u.model = JsonString(root, "modelID"); u.provider = JsonString(root, "providerID");
    auto time = Obj(root, "time"); u.timestamp = Epoch(time, "created", false);
    auto cache = Obj(tokens, "cache");
    if (!JsonCounter(tokens, "input", u.input, true) ||
        !JsonCounter(tokens, "output", u.output, true) ||
        !JsonCounter(tokens, "reasoning", u.reasoning) ||
        !JsonCounter(cache, "read", u.cachedInput) ||
        !JsonCounter(cache, "write", u.cacheWrite) ||
        !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite)) {
        ++invalid; return;
    }
    // OpenCode stores uncached input separately from cache.read/cache.write.
    // Its output is the provider's output total; reasoning is a subset.
    Finish(u, emit, invalid);
}
struct KimiWireState {
    std::string requestModel, requestAlias, createdAt;
    bool pendingRequest = false;
    std::map<std::string, size_t> occurrences;
};
void KimiUsageV2(yyjson_val* root, KimiWireState* state,
                 const UsageEmitter& emit, int64_t& invalid) {
    const auto type = JsonString(root, "type");
    if (type == "metadata") {
        int64_t created = 0;
        if (state && state->createdAt.empty() && JsonCounter(root, "created_at", created, true))
            state->createdAt = std::to_string(created);
        return;
    }
    if (type == "llm.request") {
        if (state) {
            state->requestModel = JsonString(root, "model");
            state->requestAlias = JsonString(root, "modelAlias");
            state->pendingRequest = true;
        }
        return;
    }
    if (type != "usage.record") return;
    auto usage = Obj(root, "usage"); if (!usage) { ++invalid; return; }
    ParsedUsage u; int64_t time = 0;
    const auto alias = JsonString(root, "model");
    // The new CLI persists its configured alias in usage.record.model. Only
    // the preceding durable request's model is the actual requested model.
    if (state && state->pendingRequest && !state->requestAlias.empty() && state->requestAlias == alias)
        u.model = state->requestModel;
    if (state) state->pendingRequest = false;
    if (!JsonCounter(root, "time", time, true) ||
        !JsonCounter(usage, "inputOther", u.input, true) ||
        !JsonCounter(usage, "output", u.output, true) ||
        !JsonCounter(usage, "inputCacheRead", u.cachedInput, true) ||
        !JsonCounter(usage, "inputCacheCreation", u.cacheWrite, true) ||
        !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite) || alias.size() > 1024) {
        ++invalid; return;
    }
    // The source substitutes emptyUsage() when its provider returned no usage.
    // A zero-only record cannot prove an actual metered request in that case.
    if (u.input == 0 && u.output == 0) return;
    u.timestamp = IsoFromUnixMilliseconds(time);
    // usage.record has no request ID. A path-independent signature preserves
    // copied/forked history; occurrence order retains identical real calls in
    // one wire. See the documented collision boundary across independent logs.
    const auto signature = (state ? state->createdAt : std::string()) + ":" + std::to_string(time) + ":" + Digest(alias + ":" +
        std::to_string(u.input) + ":" + std::to_string(u.output) + ":" +
        std::to_string(u.cachedInput) + ":" + std::to_string(u.cacheWrite));
    const size_t occurrence = state ? ++state->occurrences[signature] : 1;
    u.key = "kimi-code:v2:usage:" + signature + ":" + std::to_string(occurrence);
    // llm.request.provider is the protocol (e.g. openai), not a served vendor.
    // Count only usage.record: step.end and status totals cover the same tokens.
    Finish(u, emit, invalid);
}
bool Eligible(const std::string& source, const fs::path& file) {
    auto extension = file.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (source == "cursor") return extension == L".csv";
    if (source == "opencode") return extension == L".json" || extension == L".db";
    if (source == "kimi-code") return file.filename() == L"wire.jsonl";
    if (source == "copilot") return extension == L".jsonl" || extension == L".json";
    return extension == L".jsonl" || extension == L".json";
}
void JsonDocument(const std::string& source, const fs::path& file,
                  const UsageEmitter& emit, AdapterScan& result) {
    std::error_code ec; const auto size = fs::file_size(file, ec);
    if (ec) { result.error = "Cannot read usage file size."; return; }
    if (size > kDocumentLimit) { ++result.invalid; result.error = "Usage document exceeds 64 MiB limit."; return; }
    std::ifstream in(file, std::ios::binary);
    if (!in) { result.error = "Cannot open usage file read-only."; return; }
    std::string contents((std::istreambuf_iterator<char>(in)), {});
    auto doc = yyjson_read(contents.data(), contents.size(), YYJSON_READ_NOFLAG);
    if (!doc) { ++result.invalid; return; }
    ParseCliRecord(source, yyjson_doc_get_root(doc), WideToUtf8(file.wstring()), emit, result.invalid);
    yyjson_doc_free(doc);
}
void JsonLines(const std::string& source, const fs::path& file,
               const std::atomic_bool& cancelled, const UsageEmitter& emit, AdapterScan& result) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { result.error = "Cannot open usage log read-only."; return; }
    std::string line; line.reserve(8192); bool oversized = false;
    KimiWireState kimi;
    std::map<std::string, size_t> qwenOccurrences;
    auto qwenEmit = [&](const ParsedUsage& original) {
        ParsedUsage usage = original;
        if (usage.key.rfind("qwen-code:position:", 0) == 0)
            usage.key += ":" + std::to_string(++qwenOccurrences[usage.key]);
        emit(usage);
    };
    std::array<char, 65536> buffer{};
    // Consume only complete lines: a live writer's partial final record is retried.
    while (!cancelled && in) {
        in.read(buffer.data(), buffer.size()); const auto count = in.gcount();
        for (std::streamsize i = 0; i < count && !cancelled; ++i) {
            const char c = buffer[static_cast<size_t>(i)];
            if (c != '\n') {
                if (!oversized && line.size() < kLineLimit) line += c;
                else { oversized = true; line.clear(); }
                continue;
            }
            if (oversized) { ++result.invalid; oversized = false; line.clear(); continue; }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                auto doc = yyjson_read(line.data(), line.size(), YYJSON_READ_NOFLAG);
                if (!doc) ++result.invalid;
                else {
                    auto root = yyjson_doc_get_root(doc);
                    if (source == "kimi-code" && (JsonString(root, "type") == "usage.record" || JsonString(root, "type") == "llm.request" || JsonString(root, "type") == "metadata"))
                        KimiUsageV2(root, &kimi, emit, result.invalid);
                    else ParseCliRecord(source, root, WideToUtf8(file.wstring()), source == "qwen-code" ? UsageEmitter(qwenEmit) : emit, result.invalid);
                    yyjson_doc_free(doc);
                }
            }
            line.clear();
        }
    }
}
void OpenCodeDatabase(const fs::path& file, const std::atomic_bool& cancelled,
                      const UsageEmitter& emit, AdapterScan& result) {
    sqlite3* raw = nullptr;
    const auto path = WideToUtf8(file.wstring());
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw); result.error = "Cannot open OpenCode database read-only."; return;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_busy_timeout(db.get(), 500);
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT id,data FROM message", -1, &query, nullptr) != SQLITE_OK) {
        result.error = "OpenCode database schema does not expose supported message usage."; return;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(query, sqlite3_finalize);
    int step = SQLITE_DONE;
    while (!cancelled && (step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        auto bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
        const auto length = sqlite3_column_bytes(statement.get(), 1);
        if (!bytes || length <= 0) continue;
        if (static_cast<size_t>(length) > kLineLimit) { ++result.invalid; continue; }
        auto doc = yyjson_read(bytes, static_cast<size_t>(length), YYJSON_READ_NOFLAG);
        if (!doc) { ++result.invalid; continue; }
        auto id = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
        OpenCodeMessage(yyjson_doc_get_root(doc), id ? id : "", emit, result.invalid);
        yyjson_doc_free(doc);
    }
    if (step != SQLITE_DONE && step != SQLITE_ROW) result.error = "OpenCode database is busy or unreadable.";
}
bool CsvRows(const std::string& bytes, std::vector<std::vector<std::string>>& rows) {
    std::vector<std::string> row; std::string cell; bool quoted = false, closed = false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (quoted) {
            if (c == '"' && i + 1 < bytes.size() && bytes[i + 1] == '"') { cell += '"'; ++i; }
            else if (c == '"') { quoted = false; closed = true; }
            else cell += c;
        } else if (c == '"' && cell.empty() && !closed) quoted = true;
        else if (c == ',' || c == '\n') {
            if (c == '\n' && !cell.empty() && cell.back() == '\r') cell.pop_back();
            row.push_back(std::move(cell)); cell.clear(); closed = false;
            if (c == '\n') { rows.push_back(std::move(row)); row.clear(); }
        } else if (closed && c != '\r') return false;
        else cell += c;
    }
    if (quoted) return false;
    if (!cell.empty() || !row.empty()) { row.push_back(std::move(cell)); rows.push_back(std::move(row)); }
    return true;
}
bool CsvNumber(const std::string& text, int64_t& value) {
    value = 0; bool digit = false;
    for (unsigned char c : text) {
        if (c == ',') continue;
        if (c < '0' || c > '9' || value > (kLimit - (c - '0')) / 10) return false;
        digit = true; value = value * 10 + c - '0';
    }
    return digit;
}
void CursorCsv(const fs::path& file, const std::atomic_bool& cancelled,
                const UsageEmitter& emit, AdapterScan& result) {
    std::error_code ec; const auto size = fs::file_size(file, ec);
    if (ec || size > kDocumentLimit) { ++result.invalid; result.error = "Cursor CSV is unreadable or exceeds 64 MiB."; return; }
    std::ifstream in(file, std::ios::binary);
    if (!in) { result.error = "Cannot open Cursor CSV read-only."; return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
    std::vector<std::vector<std::string>> rows;
    if (!CsvRows(bytes, rows) || rows.empty()) { ++result.invalid; result.error = "Cursor CSV has invalid quoting."; return; }
    std::map<std::string, size_t> columns;
    for (size_t i = 0; i < rows.front().size(); ++i) columns[rows.front()[i]] = i;
    const std::array<const char*, 6> required{{"Date", "Model", "Input (w/ Cache Write)", "Input (w/o Cache Write)", "Cache Read", "Output Tokens"}};
    for (const auto name : required) if (!columns.contains(name)) {
        result.error = "Cursor CSV requires Date, Model, Input (w/ Cache Write), Input (w/o Cache Write), Cache Read, Output Tokens columns.";
        ++result.invalid; return;
    }
    std::map<std::string, size_t> occurrences;
    for (size_t n = 1; n < rows.size() && !cancelled; ++n) {
        const auto& row = rows[n]; if (row.size() == 1 && row[0].empty()) continue;
        if (row.size() != rows.front().size()) { ++result.invalid; continue; }
        auto cell = [&](const char* key) -> std::string { auto i = columns.find(key); return i == columns.end() ? "" : row[i->second]; };
        ParsedUsage u; u.timestamp = cell("Date"); u.model = cell("Model"); u.provider = cell("Provider");
        int64_t inclusive = 0, uncached = 0, total = 0;
        if (!CsvNumber(cell("Input (w/ Cache Write)"), inclusive) ||
            !CsvNumber(cell("Input (w/o Cache Write)"), uncached) || inclusive < uncached ||
            !CsvNumber(cell("Cache Read"), u.cachedInput) ||
            !CsvNumber(cell("Output Tokens"), u.output)) { ++result.invalid; continue; }
        u.input = inclusive; u.cacheWrite = inclusive - uncached;
        if (!Add(u.input, u.cachedInput)) { ++result.invalid; continue; }
        if (columns.contains("Total Tokens") && (!CsvNumber(cell("Total Tokens"), total) || total != u.input + u.output)) { ++result.invalid; continue; }
        // Export dates must carry a timezone. Never interpret an ambiguous CSV
        // date using the machine's current timezone or the import date.
        if (u.timestamp.size() < 20 || (u.timestamp.back() != 'Z' && u.timestamp.find('+', 10) == std::string::npos && u.timestamp.find('-', 10) == std::string::npos)) { ++result.invalid; continue; }
        u.key = cell("Request ID"); if (u.key.empty()) u.key = cell("Event ID");
        if (u.key.empty()) {
            const auto signature = u.timestamp + "|" + u.model + "|" + u.provider + "|" +
                std::to_string(u.input) + "|" + std::to_string(u.output) + "|" +
                std::to_string(u.cachedInput) + "|" + std::to_string(u.cacheWrite);
            const auto fingerprint = Digest(signature);
            u.key = "cursor:csv:" + fingerprint + ":" + std::to_string(++occurrences[fingerprint]);
        } else u.key = "cursor:request:" + u.key;
        Finish(u, emit, result.invalid);
    }
}
} // namespace

void ParseCliRecord(const std::string& source, yyjson_val* root, const std::string&,
                    const UsageEmitter& emit, int64_t& invalid) {
    if (!yyjson_is_obj(root)) { ++invalid; return; }
    if (source == "gemini-cli") {
        auto messages = yyjson_obj_get(root, "messages");
        if (yyjson_is_arr(messages)) {
            size_t index, maximum; yyjson_val* item;
            yyjson_arr_foreach(messages, index, maximum, item) GeminiMessage(item, emit, invalid);
        } else {
            GeminiMessage(root, emit, invalid);
            // Legacy JSONL full-history checkpoints retain stable message IDs.
            auto set = Obj(root, "$set"); auto list = yyjson_obj_get(set, "messages");
            if (yyjson_is_arr(list)) {
                size_t index, maximum; yyjson_val* item;
                yyjson_arr_foreach(list, index, maximum, item) GeminiMessage(item, emit, invalid);
            }
        }
    } else if (source == "opencode") {
        auto info = Obj(root, "info"); OpenCodeMessage(info ? info : root, "", emit, invalid);
    } else if (source == "qwen-code") {
        if (JsonString(root, "type") != "assistant") return;
        auto usage = Obj(root, "usageMetadata"); if (!usage) return;
        ParsedUsage u; u.key = JsonString(root, "uuid");
        if (!u.key.empty()) u.key = "qwen-code:message:" + u.key;
        u.timestamp = JsonString(root, "timestamp"); u.model = JsonString(root, "model");
        if (u.key.empty()) {
            // Current projects/chats records do not persist a per-message UUID.
            // Session and recorded timestamp are required; occurrence position
            // distinguishes simultaneous calls in a complete copied transcript.
            const auto session = JsonString(root, "sessionId");
            if (!session.empty() && !u.timestamp.empty())
                u.key = "qwen-code:position:" + Digest(session + ":" + u.timestamp + ":" + u.model);
        }
        int64_t total = 0;
        if (!yyjson_obj_get(usage, "promptTokenCount") && !yyjson_obj_get(usage, "candidatesTokenCount")) {
            // Some Qwen providers return only a trustworthy request total. Keep
            // its unknown split instead of inventing all-input or all-output.
            if (!JsonCounter(usage, "totalTokenCount", total, true)) { ++invalid; return; }
            u.unclassified = total; Finish(u, emit, invalid); return;
        }
        if (!JsonCounter(usage, "promptTokenCount", u.input, true) ||
            !JsonCounter(usage, "candidatesTokenCount", u.output, true) ||
            !JsonCounter(usage, "cachedContentTokenCount", u.cachedInput) ||
            !JsonCounter(usage, "thoughtsTokenCount", u.reasoning) ||
            !JsonCounter(usage, "totalTokenCount", total)) { ++invalid; return; }
        // Qwen's OpenAI converter maps completion_tokens to candidatesTokenCount,
        // so thoughts are normally already included (and can be estimated by the
        // source). Only an explicit provider total can prove an additive bucket.
        if (total > 0 && total == u.input + u.output + u.reasoning) {
            if (!Add(u.output, u.reasoning)) { ++invalid; return; }
        } else if (total > 0 && total != u.input + u.output) { ++invalid; return; }
        Finish(u, emit, invalid);
    } else if (source == "kimi-code") {
        if (JsonString(root, "type") == "usage.record" || JsonString(root, "type") == "llm.request") {
            KimiUsageV2(root, nullptr, emit, invalid); return;
        }
        auto message = Obj(root, "message"); if (!message || JsonString(message, "type") != "StatusUpdate") return;
        auto payload = Obj(message, "payload"); auto usage = Obj(payload, "token_usage"); if (!usage) return;
        ParsedUsage u; u.timestamp = Epoch(root, "timestamp", true);
        u.key = JsonString(payload, "message_id");
        if (!u.key.empty()) u.key = "kimi-code:step:" + u.key;
        else if (!u.timestamp.empty()) {
            char precise[64]; std::snprintf(precise, sizeof(precise), "%.17g", yyjson_get_num(yyjson_obj_get(root, "timestamp")));
            u.key = "kimi-code:step-at:" + std::string(precise);
        }
        // Wire StatusUpdate does not record the served model. Never consult
        // current config.toml and retroactively label historical requests.
        if (!JsonCounter(usage, "input_other", u.input, true) ||
            !JsonCounter(usage, "output", u.output, true) ||
            !JsonCounter(usage, "input_cache_read", u.cachedInput) ||
            !JsonCounter(usage, "input_cache_creation", u.cacheWrite) ||
            !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite)) { ++invalid; return; }
        Finish(u, emit, invalid);
    } else if (source == "copilot") {
        // assistant.usage is transient in normal CLI logs, but SDK consumers
        // can explicitly export it. Never turn context window size, premium
        // credits, compaction rollups or shutdown cumulative totals into calls.
        if (JsonString(root, "type") != "assistant.usage") return;
        auto usage = Obj(root, "data"); if (!usage) return;
        ParsedUsage u; u.key = JsonString(usage, "apiCallId");
        if (!u.key.empty()) u.rollupScopes.push_back("copilot:response:" + u.key);
        if (u.key.empty()) u.key = JsonString(root, "id");
        if (!u.key.empty()) u.key = "copilot:call:" + u.key;
        u.timestamp = JsonString(root, "timestamp"); u.model = JsonString(usage, "model");
        u.provider = JsonString(usage, "modelProvider");
        if (!JsonCounter(usage, "inputTokens", u.input, true) ||
            !JsonCounter(usage, "outputTokens", u.output, true) ||
            !JsonCounter(usage, "cacheReadTokens", u.cachedInput) ||
            !JsonCounter(usage, "cacheWriteTokens", u.cacheWrite) ||
            !JsonCounter(usage, "reasoningTokens", u.reasoning)) { ++invalid; return; }
        Finish(u, emit, invalid);
    }
}

AdapterScan CollectCli(const std::string& source, const std::wstring& path,
                       const std::atomic_bool& cancelled, const UsageEmitter& emit) {
    AdapterScan result;
    if (source != "opencode" && source != "gemini-cli" && source != "qwen-code" &&
        source != "kimi-code" && source != "copilot" && source != "cursor") {
        result.error = "Unsupported CLI usage format."; return result;
    }
    if (cancelled) return result;
    const fs::path base(path); std::error_code ec;
    if (!fs::exists(base, ec)) return result;
    auto consume = [&](const fs::path& file) {
        if (cancelled || !Eligible(source, file)) return;
        ++result.files;
        auto extension = file.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (source == "cursor") CursorCsv(file, cancelled, emit, result);
        else if (source == "opencode" && extension == L".db") OpenCodeDatabase(file, cancelled, emit, result);
        else if (extension == L".jsonl") JsonLines(source, file, cancelled, emit, result);
        else JsonDocument(source, file, emit, result);
    };
    if (fs::is_regular_file(base, ec)) consume(base);
    else {
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && !cancelled; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (it.depth() > 8 || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
                it.disable_recursion_pending(); continue;
            }
            if (it->is_regular_file(ec)) consume(it->path());
            if (result.files >= 50000) { result.error = "Usage scan reached its 50,000 file safety limit."; break; }
        }
    }
    return result;
}
} // namespace tokenscope
