// Format adaptation from missuo/tokens, MIT; see docs/extended-adapters.md.
#include "extended.h"
#include "sqlite3.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace tokenscope {
namespace fs = std::filesystem;
namespace {
constexpr int64_t limit = INT64_MAX / 16;
constexpr size_t lineLimit = 16 * 1024 * 1024;
constexpr size_t documentLimit = 64 * 1024 * 1024;
yyjson_val* Obj(yyjson_val* root, const char* name) {
    auto v = yyjson_obj_get(root, name);
    return yyjson_is_obj(v) ? v : nullptr;
}
std::string Text(yyjson_val* value) {
    const char* s = yyjson_get_str(value);
    return s ? std::string(s, yyjson_get_len(value)) : std::string();
}
std::string Hash(const std::string& bytes) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return std::to_string(h);
}
bool Add(int64_t& n, int64_t m) {
    if (m < 0 || n < 0 || m > limit - n) return false;
    n += m;
    return true;
}
bool Number(yyjson_val* v, int64_t& out, bool required = false) {
    if (!v || yyjson_is_null(v)) return !required;
    if (yyjson_is_int(v)) {
        if (yyjson_is_uint(v) && yyjson_get_uint(v) > static_cast<uint64_t>(limit)) return false;
        out = yyjson_get_sint(v);
        return out >= 0 && out <= limit;
    }
    if (yyjson_is_real(v)) {
        double n = yyjson_get_real(v);
        if (!std::isfinite(n) || n < 0 || n > static_cast<double>(limit) || std::floor(n) != n) return false;
        out = static_cast<int64_t>(n);
        return true;
    }
    if (yyjson_is_str(v)) {
        auto s = Text(v);
        if (s.empty()) return false;
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc() && end == s.data() + s.size() && out >= 0 && out <= limit;
    }
    return false;
}
bool Count(yyjson_val* root, const char* name, int64_t& n, bool required = false) {
    return Number(yyjson_obj_get(root, name), n, required);
}
bool CounterAny(yyjson_val* root, std::initializer_list<const char*> names, int64_t& n, bool required = false) {
    for (auto name : names)
        if (yyjson_obj_get(root, name)) return Count(root, name, n, true);
    return !required;
}
std::string First(yyjson_val* root, std::initializer_list<const char*> names) {
    for (auto name : names) {
        auto s = JsonString(root, name);
        if (!s.empty()) return s;
    }
    return {};
}
bool HasZone(const std::string& s) {
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;
    auto n = [&](size_t at, size_t count) {
        int v = 0;
        for (size_t i = at; i < at + count; ++i) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1;
            v = v * 10 + s[i] - '0';
        }
        return v;
    };
    const int y = n(0, 4), m = n(5, 2), d = n(8, 2);
    if (y < 1601 || m < 1 || m > 12 || d < 1 || n(11, 2) < 0 || n(11, 2) > 23 || n(14, 2) < 0 || n(14, 2) > 59 ||
        n(17, 2) < 0 || n(17, 2) > 59)
        return false;
    const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return false;
    size_t at = 19;
    if (s[at] == '.') {
        size_t start = ++at;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
        if (at == start) return false;
    }
    if (at + 1 == s.size() && s[at] == 'Z') return true;
    return at + 6 == s.size() && (s[at] == '+' || s[at] == '-') && s[at + 3] == ':' && n(at + 1, 2) >= 0 &&
           n(at + 1, 2) <= 23 && n(at + 4, 2) >= 0 && n(at + 4, 2) <= 59;
}
// Unit is explicit at every call site. Never reinterpret mtime as a request date.
std::string Stamp(yyjson_val* v, int64_t multiplier = 1) {
    auto s = Text(v);
    if (HasZone(s)) return s;
    int64_t n = 0;
    if (!Number(v, n, true) || n <= 0 || n > 253402300799999LL / multiplier) return {};
    return IsoFromUnixMilliseconds(n * multiplier);
}
std::string Scope(const std::string& origin, bool parent = false) {
    fs::path p(Utf8ToWide(origin));
    return WideToUtf8((parent ? p.parent_path().filename() : p.stem()).wstring());
}
std::string Signature(const ParsedUsage& u) {
    return Hash(u.model + "|" + u.provider + "|" + std::to_string(u.input) + "|" + std::to_string(u.output) + "|" +
                std::to_string(u.cachedInput) + "|" + std::to_string(u.cacheWrite) + "|" + std::to_string(u.reasoning) +
                "|" + std::to_string(u.unclassified));
}
void Finish(ParsedUsage u, const UsageEmitter& emit, int64_t& invalid) {
    if (u.key.empty() || u.key.size() > 4096 || !HasZone(u.timestamp) || u.model.size() > 1024 ||
        u.provider.size() > 1024 || u.input < 0 || u.output < 0 || u.unclassified < 0 || u.cachedInput < 0 ||
        u.cacheWrite < 0 || u.reasoning < 0 || u.input > limit || u.output > limit || u.unclassified > limit ||
        u.cachedInput > u.input || u.cacheWrite > u.input - u.cachedInput || u.reasoning > u.output) {
        ++invalid;
        return;
    }
    if (u.input == 0 && u.output == 0 && u.unclassified == 0) return;
    emit(u);
}
void Trae(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (yyjson_is_arr(root)) {
        size_t i, n;
        yyjson_val* item;
        yyjson_arr_foreach(root, i, n, item) Trae(item, emit, invalid);
        return;
    }
    auto extra = Obj(root, "extra_info");
    if (!extra) return;
    ParsedUsage u;
    auto id = JsonString(root, "session_id");
    int64_t seconds = 0;
    if (id.empty() || !Count(root, "usage_time", seconds, true) || seconds > 253402300799LL ||
        !Count(extra, "input_token", u.input, true) || !Count(extra, "output_token", u.output, true) ||
        !Count(extra, "cache_read_token", u.cachedInput) || !Count(extra, "cache_write_token", u.cacheWrite) ||
        !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite)) {
        ++invalid;
        return;
    }
    u.model = JsonString(root, "model_name"); // Empty Auto routing remains unknown, never a fictitious model.
    u.key = "trae:usage:" + id + ":" + std::to_string(seconds) + ":" + Hash(u.model);
    u.timestamp = IsoFromUnixMilliseconds(seconds * 1000);
    Finish(u, emit, invalid);
}
void Cursor(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    auto rows = yyjson_obj_get(root, "usageEventsDisplay");
    if (!yyjson_is_arr(rows)) return;
    std::map<std::string, size_t> occurrences;
    size_t i, n;
    yyjson_val* row;
    yyjson_arr_foreach(rows, i, n, row) {
        auto usage = Obj(row, "tokenUsage");
        if (!usage) continue;
        ParsedUsage u;
        u.timestamp = Stamp(yyjson_obj_get(row, "timestamp"));
        u.model = JsonString(row, "model");
        if (!Count(usage, "inputTokens", u.input, true) || !Count(usage, "outputTokens", u.output, true) ||
            !Count(usage, "cacheReadTokens", u.cachedInput) || !Count(usage, "cacheWriteTokens", u.cacheWrite) ||
            !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite)) {
            ++invalid;
            continue;
        }
        auto id = First(row, {"id", "requestId", "eventId", "responseId"});
        if (!id.empty())
            u.key = "cursor:request:" + id;
        else {
            auto session = JsonString(row, "conversationId");
            if (session.empty()) {
                ++invalid;
                continue;
            }
            auto base = session + ":" + u.timestamp + ":" + Signature(u);
            u.key = "cursor:cache:" + base + ":" + std::to_string(++occurrences[base]);
        }
        if (u.model == "auto" || u.model == "agent_review") u.model.clear();
        Finish(u, emit, invalid);
    }
}
std::string OtelStamp(yyjson_val* root) {
    for (auto name : {"startTime", "endTime", "hrTime", "_hrTime", "time", "timestamp", "observedTimestamp"}) {
        auto v = yyjson_obj_get(root, name);
        if (yyjson_is_arr(v) && yyjson_arr_size(v) == 2) {
            int64_t seconds = 0, nanos = 0;
            if (Number(yyjson_arr_get(v, 0), seconds, true) && Number(yyjson_arr_get(v, 1), nanos, true) &&
                seconds > 0 && seconds < 253402300799LL && nanos < 1000000000LL)
                return IsoFromUnixMilliseconds(seconds * 1000 + nanos / 1000000);
        }
        auto stamp = Stamp(v);
        if (!stamp.empty()) return stamp;
    }
    auto nano = Text(yyjson_obj_get(root, "timeUnixNano"));
    if (!nano.empty()) {
        uint64_t n = 0;
        auto [end, ec] = std::from_chars(nano.data(), nano.data() + nano.size(), n);
        if (ec == std::errc() && end == nano.data() + nano.size() && n / 1000000ULL <= 253402300799999ULL)
            return IsoFromUnixMilliseconds(static_cast<int64_t>(n / 1000000ULL));
    }
    return {};
}
bool RealTraceId(const std::string& s) {
    return !s.empty() && s.find_first_not_of('0') != std::string::npos;
}
void Copilot(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") == "assistant.usage") {
        ParseCliRecord("copilot", root, "", emit, invalid);
        return;
    }
    auto attrs = Obj(root, "attributes");
    if (!attrs) return;
    const auto type = JsonString(root, "type"), name = JsonString(root, "name"),
               operation = JsonString(attrs, "gen_ai.operation.name");
    const bool span = type == "span" || (type.empty() && !name.empty() &&
                                         (yyjson_obj_get(root, "spanId") || yyjson_obj_get(root, "startTime")));
    const bool chat = span && (operation == "chat" || name.rfind("chat ", 0) == 0);
    const bool inference = !span && JsonString(attrs, "event.name") == "gen_ai.client.inference.operation.details";
    if (!chat && !inference) return; // Agent/session summaries overlap request spans.
    ParsedUsage u;
    u.timestamp = OtelStamp(root);
    u.model = First(attrs, {"gen_ai.response.model", "gen_ai.request.model", "gen_ai.model"});
    u.provider = First(attrs, {"gen_ai.provider.name", "gen_ai.system"});
    if (!Count(attrs, "gen_ai.usage.input_tokens", u.input, true) ||
        !Count(attrs, "gen_ai.usage.output_tokens", u.output, true) ||
        !CounterAny(attrs, {"gen_ai.usage.cache_read.input_tokens", "gen_ai.usage.cache_read_input_tokens"},
                    u.cachedInput) ||
        !CounterAny(attrs,
                    {"gen_ai.usage.cache_write.input_tokens", "gen_ai.usage.cache_creation.input_tokens",
                     "gen_ai.usage.cache_write_input_tokens", "gen_ai.usage.cache_creation_input_tokens"},
                    u.cacheWrite) ||
        !CounterAny(attrs, {"gen_ai.usage.reasoning.output_tokens", "gen_ai.usage.reasoning_tokens"}, u.reasoning)) {
        ++invalid;
        return;
    }
    // OTel input/output are provider totals; cache and reasoning are subsets.
    const auto response = JsonString(attrs, "gen_ai.response.id"), trace = First(root, {"traceId", "trace_id"}),
               spanId = First(root, {"spanId", "span_id"});
    // Chat spans keep their durable span identity even if response.id arrives
    // in a later snapshot. Request scopes persist preference over inference
    // logs/SDK exports across files and scans, using Store's existing rollups.
    if (chat && RealTraceId(trace) && RealTraceId(spanId))
        u.key = "copilot:span:" + trace + ":" + spanId;
    else if (!response.empty())
        u.key = "copilot:call:" + response;
    else if (RealTraceId(trace) && RealTraceId(spanId))
        u.key = "copilot:span:" + trace + ":" + spanId;
    else {
        ++invalid;
        return;
    }
    u.rollup = chat;
    if (!response.empty()) u.rollupScopes.push_back("copilot:response:" + response);
    if (RealTraceId(trace) && RealTraceId(spanId)) u.rollupScopes.push_back("copilot:span:" + trace + ":" + spanId);
    Finish(u, emit, invalid);
}
void Antigravity(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") != "usage") return;
    ParsedUsage u;
    u.model = JsonString(root, "modelId");
    u.provider = JsonString(root, "providerId");
    u.timestamp = Stamp(yyjson_obj_get(root, "timestamp"));
    u.key = JsonString(root, "responseId");
    if (!Count(root, "input", u.input, true) || !Count(root, "output", u.output, true) ||
        !Count(root, "cacheRead", u.cachedInput) || !Count(root, "cacheWrite", u.cacheWrite) ||
        !Count(root, "reasoning", u.reasoning) || !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite) ||
        !Add(u.output, u.reasoning)) {
        ++invalid;
        return;
    }
    if (u.key.empty()) {
        auto scope = JsonString(root, "sessionId");
        if (!scope.empty()) u.key = scope + ":" + u.timestamp + ":" + Signature(u);
    }
    if (!u.key.empty()) u.key = "antigravity:response:" + u.key;
    Finish(u, emit, invalid);
}
void CommandCode(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") != "message" || JsonString(Obj(root, "message"), "role") != "assistant") return;
    auto usage = Obj(root, "usage");
    if (!usage) return;
    ParsedUsage u;
    u.key = JsonString(root, "id");
    u.timestamp = JsonString(root, "timestamp");
    u.model = JsonString(root, "model");
    if (!u.key.empty()) u.key = "commandcode:message:" + u.key + ":" + u.timestamp;
    if (!Count(usage, "inputTokens", u.input, true) || !Count(usage, "outputTokens", u.output, true) ||
        !Count(usage, "cacheReadTokens", u.cachedInput) || !Count(usage, "cacheWriteTokens", u.cacheWrite)) {
        ++invalid;
        return;
    }
    auto slash = u.model.find('/');
    if (slash != std::string::npos) {
        u.provider = u.model.substr(0, slash);
        u.model = u.model.substr(slash + 1);
    }
    Finish(u, emit, invalid);
}
void Reasonix(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (yyjson_get_bool(yyjson_obj_get(root, "turn"))) return;
    int64_t total = 0, requests = 0;
    ParsedUsage u;
    u.timestamp = Stamp(yyjson_obj_get(root, "ts"));
    u.model = JsonString(root, "model");
    if (!Count(root, "total", total) || !Count(root, "requests", requests)) {
        ++invalid;
        return;
    }
    if (total == 0 && requests == 0) return;
    if (!Count(root, "prompt", u.input, true) || !Count(root, "completion", u.output, true) ||
        !Count(root, "reasoning", u.reasoning) || !Count(root, "cache_hit", u.cachedInput)) {
        ++invalid;
        return;
    }
    int64_t miss = 0;
    if (!Count(root, "cache_miss", miss)) {
        ++invalid;
        return;
    }
    if (miss) {
        u.input = miss;
        if (!Add(u.input, u.cachedInput)) {
            ++invalid;
            return;
        }
    }
    if (total > u.input + u.output) u.unclassified = total - u.input - u.output;
    auto slash = u.model.find('/');
    if (slash != std::string::npos) {
        u.provider = u.model.substr(0, slash);
        u.model = u.model.substr(slash + 1);
    }
    u.key = "reasonix:stat:" + u.timestamp + ":" + std::to_string(requests) + ":" + Signature(u);
    Finish(u, emit, invalid);
}
void Junie(yyjson_val* root, const std::string& origin, const UsageEmitter& emit, int64_t& invalid) {
    auto agent = Obj(Obj(root, "event"), "agentEvent");
    if (JsonString(agent, "kind") != "LlmResponseMetadataEvent") return;
    auto rows = yyjson_obj_get(agent, "modelUsage");
    if (!yyjson_is_arr(rows)) return;
    size_t i, n;
    yyjson_val* row;
    yyjson_arr_foreach(rows, i, n, row) {
        ParsedUsage u;
        u.timestamp = Stamp(yyjson_obj_get(root, "timestampMs"));
        u.model = JsonString(row, "model");
        u.provider = First(row, {"provider", "providerId"});
        if (!CounterAny(row, {"inputTokens", "input"}, u.input, true) ||
            !CounterAny(row, {"outputTokens", "output"}, u.output, true) ||
            !CounterAny(row, {"cacheInputTokens", "cacheReadInputTokens", "cacheRead"}, u.cachedInput) ||
            !CounterAny(row, {"cacheCreateTokens", "cacheCreationInputTokens", "cacheWrite"}, u.cacheWrite) ||
            !CounterAny(row, {"reasoningTokens", "reasoningOutputTokens", "thinkingTokens"}, u.reasoning) ||
            !Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite) || !Add(u.output, u.reasoning)) {
            ++invalid;
            continue;
        }
        auto scope = First(root, {"sessionId", "id"});
        if (scope.empty()) scope = Scope(origin, true);
        if (!scope.empty())
            u.key = "junie:event:" + scope + ":" + u.timestamp + ":" + std::to_string(i) + ":" + Signature(u);
        Finish(u, emit, invalid);
    }
}
void Grok(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "msg") != "shell.turn.inference_done") return;
    auto ctx = Obj(root, "ctx");
    if (!ctx) return;
    ParsedUsage u;
    u.timestamp = Stamp(yyjson_obj_get(root, "ts"));
    u.model = First(ctx, {"effective_model", "model"});
    u.provider = "xai";
    if (!Count(ctx, "prompt_tokens", u.input, true) || !Count(ctx, "completion_tokens", u.output, true) ||
        !Count(ctx, "cached_prompt_tokens", u.cachedInput) || !Count(ctx, "reasoning_tokens", u.reasoning)) {
        ++invalid;
        return;
    }
    const auto id = JsonString(ctx, "request_id"), session = JsonString(root, "sid");
    int64_t loop = 0;
    if (!id.empty())
        u.key = "grok:request:" + id;
    else if (!session.empty() && Count(ctx, "loop_index", loop, true))
        u.key = "grok:inference:" + session + ":" + u.timestamp + ":" + std::to_string(loop);
    Finish(u, emit, invalid);
}
void JcodeMessage(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "role") != "assistant") return;
    auto usage = Obj(root, "token_usage");
    if (!usage) return;
    ParsedUsage u;
    u.key = JsonString(root, "id");
    u.timestamp = JsonString(root, "timestamp");
    // Session model/provider can have changed since this historical message.
    u.model = JsonString(root, "model");
    u.provider = JsonString(root, "provider_key");
    if (!Count(usage, "input_tokens", u.input, true) || !Count(usage, "output_tokens", u.output, true) ||
        !Count(usage, "cache_read_input_tokens", u.cachedInput) ||
        !Count(usage, "cache_creation_input_tokens", u.cacheWrite) ||
        !Count(usage, "reasoning_output_tokens", u.reasoning)) {
        ++invalid;
        return;
    }
    if ((yyjson_obj_get(usage, "cache_creation_input_tokens") || u.cachedInput > u.input) &&
        (!Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite))) {
        ++invalid;
        return;
    }
    if (!u.key.empty()) u.key = "jcode:message:" + u.key;
    Finish(u, emit, invalid);
}
void OpenCodeReview(yyjson_val* root, const std::string& origin, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") != "llm_response") return;
    auto usage = Obj(root, "usage");
    if (!usage) return;
    ParsedUsage u;
    u.timestamp = Stamp(yyjson_obj_get(root, "timestamp"));
    u.model = JsonString(root, "model");
    u.provider = JsonString(root, "provider");
    if (!Count(usage, "prompt_tokens", u.input, true) || !Count(usage, "completion_tokens", u.output, true) ||
        !Count(usage, "cache_read_tokens", u.cachedInput) || !Count(usage, "cache_write_tokens", u.cacheWrite)) {
        ++invalid;
        return;
    }
    // Native versions do not declare whether prompt_tokens contains cache.
    // An explicit total can disambiguate disjoint buckets; without it, reject
    // cached rows rather than silently bill cached tokens twice.
    if (u.cachedInput || u.cacheWrite) {
        int64_t total = 0;
        if (!Count(usage, "total_tokens", total, true)) {
            ++invalid;
            return;
        }
        if (total == u.input + u.output + u.cachedInput + u.cacheWrite) {
            if (!Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite)) {
                ++invalid;
                return;
            }
        } else if (total != u.input + u.output) {
            ++invalid;
            return;
        }
    }
    auto id = First(root, {"id", "requestId", "responseId"});
    if (!id.empty())
        u.key = "opencodereview:request:" + id;
    else {
        auto scope = JsonString(root, "sessionId");
        if (scope.empty()) scope = Scope(origin);
        if (!scope.empty()) u.key = "opencodereview:call:" + scope + ":" + u.timestamp + ":" + Signature(u);
    }
    Finish(u, emit, invalid);
}
bool Eligible(const std::string& source, const fs::path& path) {
    auto name = path.filename().wstring(), extension = path.extension().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (source == "cline")
        return name == L"ui_messages.json" || (name.size() > 14 && name.ends_with(L".messages.json"));
    if (source == "roo-code" || source == "kilo-code") return name == L"ui_messages.json";
    if (source == "codebuff") return name == L"chat-messages.json";
    if (source == "openclaw") {
        for (const auto& part : path)
            if (part == L"codex-home") return false;
        return extension == L".jsonl" || name == L"openclaw-agent.sqlite";
    }
    if (source == "cursor") return name.rfind(L"usage", 0) == 0 && (extension == L".json" || extension == L".csv");
    if (source == "copilot") return extension == L".jsonl" || extension == L".json";
    if (source == "reasonix" || source == "commandcode" || source == "antigravity" || source == "pi" ||
        source == "opencodereview")
        return extension == L".jsonl";
    if (source == "junie") return name == L"events.jsonl";
    if (source == "grok-build") return name == L"unified.jsonl";
    if (source == "jcode") return extension == L".json" || extension == L".jsonl";
    if (source == "devin-cli" || source == "zcode" || source == "antigravity-cli")
        return extension == L".db" || extension == L".sqlite" || extension == L".sqlite3";
    if (source == "kilo-cli" || source == "mimo-code") return extension == L".db";
    if (source == "amp" || source == "trae-code") return extension == L".json";
    return false;
}
void ReadDocument(const std::string& source, const fs::path& path, const UsageEmitter& emit, AdapterScan& scan) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        scan.error = "Cannot open usage document read-only.";
        return;
    }
    std::string text;
    std::array<char, 65536> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        text.append(buffer.data(), static_cast<size_t>(in.gcount()));
        if (text.size() > documentLimit) {
            ++scan.invalid;
            scan.error = "Usage document exceeds 64 MiB.";
            return;
        }
    }
    auto doc = yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_BOM);
    if (!doc) {
        ++scan.invalid;
        return;
    }
    ParseExtendedRecord(source, yyjson_doc_get_root(doc), WideToUtf8(path.wstring()), emit, scan.invalid);
    yyjson_doc_free(doc);
}
void ReadLines(const std::string& source, const fs::path& path, const std::atomic_bool& cancelled,
               const UsageEmitter& emit, AdapterScan& scan) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        scan.error = "Cannot open usage log read-only.";
        return;
    }
    std::string line;
    std::array<char, 65536> buffer{};
    bool oversized = false;
    std::map<std::string, size_t> occurrences;
    auto emitOccurrence = [&](const ParsedUsage& original) {
        ParsedUsage u = original;
        if (source == "reasonix") u.key += ":" + std::to_string(++occurrences[u.key]);
        emit(u);
    };
    while (!cancelled && in) {
        in.read(buffer.data(), buffer.size());
        for (std::streamsize i = 0; i < in.gcount() && !cancelled; ++i) {
            char c = buffer[static_cast<size_t>(i)];
            if (c != '\n') {
                if (!oversized && line.size() < lineLimit)
                    line += c;
                else {
                    oversized = true;
                    line.clear();
                }
                continue;
            }
            if (oversized) {
                ++scan.invalid;
                oversized = false;
                line.clear();
                continue;
            }
            if (!line.empty()) {
                auto doc = yyjson_read(line.data(), line.size(), YYJSON_READ_ALLOW_BOM);
                if (doc) {
                    ParseExtendedRecord(source, yyjson_doc_get_root(doc), WideToUtf8(path.wstring()), emitOccurrence,
                                        scan.invalid);
                    yyjson_doc_free(doc);
                } else if (line.find_first_not_of(" \r\t") != std::string::npos)
                    ++scan.invalid;
            }
            line.clear();
        }
    }
}
// SQLite adapters below intentionally query request/message tables, never session totals.
void Sqlite(const std::string& source, const fs::path& path, const std::atomic_bool& cancelled,
            const UsageEmitter& emit, AdapterScan& scan);
struct ProtoField {
    uint64_t number = 0;
    std::string bytes;
    bool scalar = false;
};
using Proto = std::map<unsigned, ProtoField>;
bool Varint(const std::string& bytes, size_t& pos, uint64_t& n) {
    n = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        if (pos >= bytes.size()) return false;
        unsigned c = static_cast<unsigned char>(bytes[pos++]);
        if (shift == 63 && (c & 0xfe)) return false;
        n |= static_cast<uint64_t>(c & 127) << shift;
        if (!(c & 128)) return true;
    }
    return false;
}
bool Decode(const std::string& bytes, Proto& fields) {
    size_t pos = 0, count = 0;
    while (pos < bytes.size()) {
        uint64_t tag = 0, n = 0;
        if (++count > 10000 || !Varint(bytes, pos, tag) || tag < 8 || (tag >> 3) > 536870911ULL) return false;
        unsigned id = static_cast<unsigned>(tag >> 3), wire = tag & 7;
        if (wire == 0) {
            if (!Varint(bytes, pos, n)) return false;
            fields[id] = {n, {}, true};
        } else if (wire == 2) {
            if (!Varint(bytes, pos, n) || n > bytes.size() - pos) return false;
            fields[id] = {0, bytes.substr(pos, static_cast<size_t>(n)), false};
            pos += static_cast<size_t>(n);
        } else if (wire == 1 || wire == 5) {
            size_t len = wire == 1 ? 8 : 4;
            if (bytes.size() - pos < len) return false;
            pos += len;
        } else
            return false;
    }
    return true;
}
Proto Nested(const Proto& fields, unsigned id) {
    Proto next;
    auto it = fields.find(id);
    if (it != fields.end() && !it->second.scalar && !Decode(it->second.bytes, next)) next.clear();
    return next;
}
std::string ProtoText(const Proto& fields, unsigned id) {
    auto it = fields.find(id);
    return it != fields.end() && !it->second.scalar ? it->second.bytes : std::string();
}
bool ProtoCount(const Proto& fields, unsigned id, int64_t& n) {
    auto it = fields.find(id);
    if (it == fields.end()) return true;
    if (!it->second.scalar || it->second.number > static_cast<uint64_t>(limit)) return false;
    n = static_cast<int64_t>(it->second.number);
    return true;
}
std::string ProtoStamp(const Proto& fields) {
    int64_t seconds = 0, nanos = 0;
    if (!ProtoCount(fields, 1, seconds) || !ProtoCount(fields, 2, nanos) || seconds <= 0 || seconds > 253402300799LL ||
        nanos >= 1000000000LL)
        return {};
    return IsoFromUnixMilliseconds(seconds * 1000 + nanos / 1000000);
}
std::string Column(sqlite3_stmt* stmt, int col) {
    const char* bytes = reinterpret_cast<const char*>(sqlite3_column_blob(stmt, col));
    int n = sqlite3_column_bytes(stmt, col);
    return bytes && n > 0 && n <= static_cast<int>(documentLimit) ? std::string(bytes, static_cast<size_t>(n))
                                                                  : std::string();
}
bool SqlCounter(sqlite3_stmt* stmt, int col, int64_t& n, bool required = false) {
    int type = sqlite3_column_type(stmt, col);
    if (type == SQLITE_NULL) return !required;
    if (type != SQLITE_INTEGER) return false;
    n = sqlite3_column_int64(stmt, col);
    return n >= 0 && n <= limit;
}
void Sqlite(const std::string& source, const fs::path& path, const std::atomic_bool& cancelled,
            const UsageEmitter& emit, AdapterScan& scan) {
    if (source == "kilo-cli" || source == "mimo-code") {
        auto nested = CollectCli("opencode", path.wstring(), cancelled, emit);
        scan.invalid += nested.invalid;
        if (!nested.error.empty()) scan.error = nested.error;
        return;
    }
    sqlite3* raw = nullptr;
    auto utf8 = WideToUtf8(path.wstring());
    if (sqlite3_open_v2(utf8.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        scan.error = "Cannot open usage SQLite read-only.";
        return;
    }
    struct Close {
        sqlite3* db;
        ~Close() { sqlite3_close(db); }
    } close{raw};
    sqlite3_busy_timeout(raw, 500);
    sqlite3_progress_handler(
        raw, 1000, [](void* flag) { return static_cast<const std::atomic_bool*>(flag)->load() ? 1 : 0; },
        const_cast<std::atomic_bool*>(&cancelled));
    auto query = [&](const char* sql, const std::function<void(sqlite3_stmt*)>& row) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        int rc = SQLITE_DONE;
        size_t rows = 0;
        while (!cancelled && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (++rows > 1000000) {
                scan.error = "Usage SQLite exceeds 1000000 records.";
                break;
            }
            row(stmt);
        }
        if (rc != SQLITE_DONE && rc != SQLITE_ROW && rc != SQLITE_INTERRUPT)
            scan.error = "Cannot read usage SQLite records.";
        sqlite3_finalize(stmt);
        return true;
    };
    if (source == "openclaw") {
        query("SELECT event_json FROM transcript_events", [&](sqlite3_stmt* stmt) {
            auto bytes = Column(stmt, 0);
            auto doc = yyjson_read(bytes.data(), bytes.size(), YYJSON_READ_NOFLAG);
            if (!doc) {
                ++scan.invalid;
                return;
            }
            ParseExtendedFamily(source, yyjson_doc_get_root(doc), utf8, emit, scan.invalid);
            yyjson_doc_free(doc);
        });
        return;
    }
    if (source == "devin-cli") {
        query("SELECT row_id,session_id,chat_message,created_at FROM message_nodes", [&](sqlite3_stmt* stmt) {
            auto bytes = Column(stmt, 2);
            auto doc = yyjson_read(bytes.data(), bytes.size(), YYJSON_READ_NOFLAG);
            if (!doc) {
                ++scan.invalid;
                return;
            }
            auto root = yyjson_doc_get_root(doc), metadata = Obj(root, "metadata"), metrics = Obj(metadata, "metrics");
            if (JsonString(root, "role") == "assistant" && metrics) {
                ParsedUsage u;
                int64_t seconds = 0, rowId = 0;
                if (!SqlCounter(stmt, 0, rowId, true) || !SqlCounter(stmt, 3, seconds, true) || seconds <= 0 ||
                    seconds > 253402300799LL || !Count(metrics, "input_tokens", u.input, true) ||
                    !Count(metrics, "output_tokens", u.output, true) ||
                    !Count(metrics, "cache_read_tokens", u.cachedInput) ||
                    !Count(metrics, "cache_creation_tokens", u.cacheWrite) || !Add(u.input, u.cachedInput) ||
                    !Add(u.input, u.cacheWrite)) {
                    ++scan.invalid;
                } else {
                    u.model = JsonString(metadata, "generation_model");
                    if (u.model == "adaptive") u.model.clear();
                    u.timestamp = IsoFromUnixMilliseconds(seconds * 1000);
                    auto scope = Column(stmt, 1);
                    if (!scope.empty()) u.key = "devin:node:" + scope + ":" + std::to_string(rowId);
                    Finish(u, emit, scan.invalid);
                }
            }
            yyjson_doc_free(doc);
        });
        return;
    }
    if (source == "zcode") {
        const char* modern =
            "SELECT id,session_id,model_id,started_at,completed_at,input_tokens,output_tokens,reasoning_tokens,cache_read_input_tokens,cache_creation_input_tokens,computed_total_tokens FROM model_usage";
        const char* legacy =
            "SELECT id,session_id,model_id,started_at,completed_at,input_tokens,output_tokens,reasoning_tokens,cache_read_input_tokens,cache_creation_input_tokens,NULL FROM model_usage";
        auto parse = [&](sqlite3_stmt* stmt) {
            ParsedUsage u;
            int64_t start = 0, completed = 0, total = 0;
            if (!SqlCounter(stmt, 3, start) || !SqlCounter(stmt, 4, completed) || !SqlCounter(stmt, 5, u.input, true) ||
                !SqlCounter(stmt, 6, u.output, true) || !SqlCounter(stmt, 7, u.reasoning) ||
                !SqlCounter(stmt, 8, u.cachedInput) || !SqlCounter(stmt, 9, u.cacheWrite) ||
                !SqlCounter(stmt, 10, total)) {
                ++scan.invalid;
                return;
            }
            const auto id = Column(stmt, 0), session = Column(stmt, 1);
            if (!id.empty() && !session.empty()) u.key = "zcode:usage:" + session + ":" + id;
            u.model = Column(stmt, 2);
            u.timestamp = IsoFromUnixMilliseconds(start > 0 ? start : completed);
            // Modern computed_total proves whether a historical row is inclusive
            // or disjoint. Ambiguous old rows remain unclassified/unpriced.
            if (total > 0 && total == u.input + u.output + u.cachedInput + u.cacheWrite + u.reasoning) {
                if (!Add(u.input, u.cachedInput) || !Add(u.input, u.cacheWrite) || !Add(u.output, u.reasoning)) {
                    ++scan.invalid;
                    return;
                }
            } else if ((total > 0 && total != u.input + u.output) || (sqlite3_column_type(stmt, 10) == SQLITE_NULL &&
                                                                      (u.cachedInput || u.cacheWrite || u.reasoning))) {
                if (total <= 0) {
                    ++scan.invalid;
                    return;
                }
                u.input = u.output = u.cachedInput = u.cacheWrite = u.reasoning = 0;
                u.unclassified = total;
            }
            Finish(u, emit, scan.invalid);
        };
        if (!query(modern, parse)) query(legacy, parse);
        return;
    }
    if (source == "antigravity-cli") {
        std::map<std::string, std::string> responseTimes;
        std::map<int64_t, std::string> generationTimes;
        query("SELECT metadata FROM steps WHERE step_type=15 AND metadata IS NOT NULL", [&](sqlite3_stmt* stmt) {
            Proto fields;
            if (!Decode(Column(stmt, 0), fields)) {
                ++scan.invalid;
                return;
            }
            auto timestamp = ProtoStamp(Nested(fields, 1));
            if (timestamp.empty()) return;
            auto id = ProtoText(Nested(fields, 9), 11);
            if (!id.empty()) responseTimes[id] = timestamp;
            int64_t index = 0;
            if (ProtoCount(Nested(fields, 20), 3, index)) generationTimes[index] = timestamp;
        });
        query("SELECT idx,data FROM gen_metadata ORDER BY idx", [&](sqlite3_stmt* stmt) {
            Proto fields;
            if (!Decode(Column(stmt, 1), fields)) {
                ++scan.invalid;
                return;
            }
            auto chat = Nested(fields, 1), usage = Nested(chat, 4);
            if (usage.empty()) return;
            ParsedUsage u;
            int64_t system = 0, idx = 0;
            if (!ProtoCount(usage, 1, system) || !ProtoCount(usage, 2, u.input) ||
                !ProtoCount(usage, 5, u.cachedInput) || !ProtoCount(usage, 9, u.output) ||
                !ProtoCount(usage, 10, u.reasoning) || !Add(u.input, system) || !Add(u.input, u.cachedInput) ||
                !Add(u.output, u.reasoning)) {
                ++scan.invalid;
                return;
            }
            auto id = ProtoText(usage, 11);
            if (id.empty()) {
                ++scan.invalid;
                return;
            }
            u.key = "antigravity-cli:response:" + id;
            u.model = ProtoText(chat, 19);
            u.timestamp = ProtoStamp(Nested(Nested(chat, 9), 4));
            if (u.timestamp.empty()) {
                auto it = responseTimes.find(id);
                if (it != responseTimes.end()) u.timestamp = it->second;
            }
            if (u.timestamp.empty() && SqlCounter(stmt, 0, idx, true)) {
                auto it = generationTimes.find(idx);
                if (it != generationTimes.end()) u.timestamp = it->second;
            }
            // Opaque 8-byte fields, session created time and mtime are not dates.
            Finish(u, emit, scan.invalid);
        });
        return;
    }
}
} // namespace

void ParseExtendedRecord(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid) {
    if (ParseExtendedFamily(source, root, origin, emit, invalid)) return;
    if (source == "trae-code")
        Trae(root, emit, invalid);
    else if (source == "cursor")
        Cursor(root, emit, invalid);
    else if (source == "copilot")
        Copilot(root, emit, invalid);
    else if (source == "antigravity")
        Antigravity(root, emit, invalid);
    else if (source == "commandcode")
        CommandCode(root, emit, invalid);
    else if (source == "reasonix")
        Reasonix(root, emit, invalid);
    else if (source == "junie")
        Junie(root, origin, emit, invalid);
    else if (source == "grok-build")
        Grok(root, emit, invalid);
    else if (source == "opencodereview")
        OpenCodeReview(root, origin, emit, invalid);
    else if (source == "jcode") {
        auto rows = yyjson_obj_get(root, "messages");
        if (!rows) rows = yyjson_obj_get(root, "append_messages");
        if (yyjson_is_arr(rows)) {
            size_t i, n;
            yyjson_val* row;
            yyjson_arr_foreach(rows, i, n, row) JcodeMessage(row, emit, invalid);
        } else
            JcodeMessage(root, emit, invalid);
    }
}

AdapterScan CollectExtended(const std::string& source, const std::wstring& supplied, const std::atomic_bool& cancelled,
                            const UsageEmitter& emit) {
    AdapterScan scan;
    std::error_code ec;
    const fs::path root(supplied);
    auto read = [&](const fs::path& file, bool explicitFile = false) {
        auto extension = file.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (cancelled || (!Eligible(source, file) &&
                          !(explicitFile && source == "cursor" && (extension == L".csv" || extension == L".json"))))
            return;
        if (!explicitFile && source == "cursor" && extension == L".csv") {
            auto preferred = file;
            preferred.replace_extension(L".json");
            std::error_code pairedError;
            if (fs::is_regular_file(preferred, pairedError)) return;
        }
        if (++scan.files > 50000) {
            scan.error = "Usage scan exceeds 50000 files.";
            return;
        }
        if (extension == L".db" || extension == L".sqlite" || extension == L".sqlite3")
            Sqlite(source, file, cancelled, emit, scan);
        else if (extension == L".csv") {
            auto result = CollectCli(source, file.wstring(), cancelled, emit);
            scan.invalid += result.invalid;
            if (!result.error.empty()) scan.error = result.error;
        } else if (extension == L".jsonl" || extension == L".ndjson")
            ReadLines(source, file, cancelled, emit, scan);
        else
            ReadDocument(source, file, emit, scan);
    };
    if (fs::is_regular_file(root, ec)) {
        auto flags = GetFileAttributesW(root.c_str());
        if (flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT)) read(root, true);
        return scan;
    }
    if (!fs::is_directory(root, ec)) return scan;
    auto flags = GetFileAttributesW(root.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return scan;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    size_t visited = 0;
    for (; !cancelled && !ec && it != end; it.increment(ec)) {
        if (++visited > 50000) {
            scan.error = "Usage scan exceeds 50000 entries.";
            break;
        }
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            if (it.depth() >= 12 || (source == "cursor" && root.filename() == L"cursor-cache"))
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) read(it->path());
    }
    if (ec) scan.error = "Cannot enumerate usage directory.";
    return scan;
}
} // namespace tokenscope
