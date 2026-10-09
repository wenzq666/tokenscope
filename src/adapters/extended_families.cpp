#include "extended.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>

namespace tokenscope {
namespace {
constexpr int64_t kLimit = INT64_MAX / 8;
using Doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
yyjson_val* Object(yyjson_val* root, const char* key) {
    auto value = yyjson_obj_get(root, key); return yyjson_is_obj(value) ? value : nullptr;
}
bool Present(yyjson_val* root, const char* key) { return yyjson_obj_get(root, key) != nullptr; }
bool Add(int64_t& value, int64_t extra) {
    if (value < 0 || extra < 0 || extra > kLimit - value) return false;
    value += extra; return true;
}
bool Digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
bool Iso(const std::string& text) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') return false;
    auto number = [&](size_t at, size_t length) {
        int result = 0;
        for (size_t i = at; i < at + length; ++i) {
            if (text[i] < '0' || text[i] > '9') return -1;
            result = result * 10 + text[i] - '0';
        }
        return result;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    if (year < 1601 || month < 1 || month > 12 || day < 1 ||
        number(11, 2) < 0 || number(11, 2) > 23 || number(14, 2) < 0 || number(14, 2) > 59 ||
        number(17, 2) < 0 || number(17, 2) > 59) return false;
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    const int maximum = days[month] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day > maximum) return false;
    size_t at = 19;
    if (text[at] == '.') {
        const size_t start = ++at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
        if (at == start) return false;
    }
    if (at + 1 == text.size() && text[at] == 'Z') return true;
    return at + 6 == text.size() && (text[at] == '+' || text[at] == '-') && text[at + 3] == ':' &&
        number(at + 1, 2) >= 0 && number(at + 1, 2) <= 23 && number(at + 4, 2) >= 0 && number(at + 4, 2) <= 59;
}
std::string Time(yyjson_val* value) {
    if (yyjson_is_str(value)) {
        std::string text(yyjson_get_str(value), yyjson_get_len(value));
        if (Iso(text)) return text;
        if (!Digits(text)) return {};
        int64_t epoch = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), epoch);
        if (parsed.ec != std::errc() || epoch < 0) return {};
        // Native task timestamps are milliseconds. ISO strings are preserved verbatim.
        return epoch <= 253402300799000LL ? IsoFromUnixMilliseconds(epoch) : std::string{};
    }
    if (!yyjson_is_int(value)) return {};
    const auto epoch = yyjson_get_sint(value);
    return epoch >= 0 && epoch <= 253402300799000LL ? IsoFromUnixMilliseconds(epoch) : std::string{};
}
std::string Time(yyjson_val* root, const char* key) { return Time(yyjson_obj_get(root, key)); }
std::string Identity(yyjson_val* root, const char* key) {
    auto value = yyjson_obj_get(root, key);
    if (yyjson_is_str(value)) return JsonString(root, key);
    if (yyjson_is_int(value) && yyjson_get_sint(value) >= 0) return std::to_string(yyjson_get_sint(value));
    return {};
}
std::string ParentName(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    path.resize(slash);
    const size_t parent = path.find_last_of('/');
    return path.substr(parent == std::string::npos ? 0 : parent + 1);
}
void Finish(ParsedUsage& usage, const UsageEmitter& emit, int64_t& invalid) {
    if (usage.key.empty() || usage.key.size() > 4096 || !Iso(usage.timestamp) ||
        usage.model.size() > 1024 || usage.provider.size() > 1024 ||
        usage.input < 0 || usage.output < 0 || usage.unclassified < 0 ||
        usage.input > kLimit || usage.output > kLimit || usage.unclassified > kLimit ||
        usage.cachedInput < 0 || usage.cacheWrite < 0 || usage.reasoning < 0 ||
        usage.cachedInput > usage.input || usage.cacheWrite > usage.input - usage.cachedInput ||
        usage.reasoning > usage.output) { ++invalid; return; }
    // Transcript placeholders and credits-only records are not evidence of token use.
    if (usage.input == 0 && usage.output == 0 && usage.unclassified == 0) return;
    emit(usage);
}
bool Fields(yyjson_val* root, const char* input, const char* output, const char* read, const char* write,
            ParsedUsage& usage) {
    return (Present(root, input) || Present(root, output) || Present(root, read) || Present(root, write)) &&
        JsonCounter(root, input, usage.input) && JsonCounter(root, output, usage.output) &&
        JsonCounter(root, read, usage.cachedInput) && JsonCounter(root, write, usage.cacheWrite);
}
void TaskEntry(const std::string& source, yyjson_val* entry, const std::string& task,
               std::map<std::string, size_t>& occurrences, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(entry, "type") != "say" || JsonString(entry, "say") != "api_req_started") return;
    const auto text = JsonString(entry, "text");
    if (text.empty() || text.size() > 16 * 1024 * 1024) return;
    Doc document(yyjson_read(text.data(), text.size(), 0), yyjson_doc_free);
    auto payload = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_obj(payload)) { ++invalid; return; }
    if (!Present(payload, "tokensIn") && !Present(payload, "tokensOut") &&
        !Present(payload, "cacheReads") && !Present(payload, "cacheWrites")) return;
    ParsedUsage usage; usage.timestamp = Time(entry, "ts");
    auto model = Object(entry, "modelInfo");
    usage.model = JsonString(model, "modelId");
    usage.provider = JsonString(payload, "apiProtocol");
    if (usage.provider.empty()) usage.provider = JsonString(model, "providerId");
    if (!Fields(payload, "tokensIn", "tokensOut", "cacheReads", "cacheWrites", usage)) { ++invalid; return; }
    // Cline's SDK translator explicitly subtracts caches before persisting
    // tokensIn. Roo/Kilo Task.ts instead persists costResult.totalInputTokens,
    // already inclusive even for Anthropic. apiProtocol is not this distinction.
    if (source == "cline") {
        if (!Add(usage.input, usage.cachedInput) || !Add(usage.input, usage.cacheWrite)) { ++invalid; return; }
    }
    auto id = Identity(payload, "requestId");
    if (id.empty()) id = Identity(entry, "id");
    if (!id.empty()) usage.key = source + ":request:" + id;
    else if (!task.empty() && !usage.timestamp.empty()) {
        const auto base = source + ":task:" + task + ":" + usage.timestamp;
        usage.key = base + ":" + std::to_string(occurrences[base]++);
    }
    Finish(usage, emit, invalid);
}
void ClineCli(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    auto entries = yyjson_obj_get(root, "messages");
    if (!yyjson_is_arr(entries)) return;
    size_t index, maximum; yyjson_val* entry;
    yyjson_arr_foreach(entries, index, maximum, entry) {
        if (JsonString(entry, "role") != "assistant") continue;
        auto metrics = Object(entry, "metrics"); if (!metrics) continue;
        ParsedUsage usage; auto model = Object(entry, "modelInfo");
        usage.model = JsonString(model, "id"); usage.provider = JsonString(model, "provider");
        usage.timestamp = Time(entry, "ts");
        const auto id = Identity(entry, "id");
        if (!id.empty()) usage.key = "cline:message:" + id;
        // CLI metrics.inputTokens is the inclusive prompt total.
        if (!Fields(metrics, "inputTokens", "outputTokens", "cacheReadTokens", "cacheWriteTokens", usage)) { ++invalid; continue; }
        Finish(usage, emit, invalid);
    }
}
void Amp(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    auto ledger = Object(root, "usageLedger");
    auto events = yyjson_obj_get(ledger, "events"); if (!yyjson_is_arr(events)) return;
    const auto thread = Identity(root, "id");
    std::map<std::string, size_t> occurrences;
    size_t index, maximum; yyjson_val* event;
    yyjson_arr_foreach(events, index, maximum, event) {
        auto tokens = Object(event, "tokens"); if (!tokens) continue;
        ParsedUsage usage; usage.timestamp = Time(event, "timestamp");
        usage.model = JsonString(event, "model"); usage.provider = JsonString(event, "provider");
        if (!Fields(tokens, "input", "output", "cacheReadInputTokens", "cacheCreationInputTokens", usage) ||
            !Add(usage.input, usage.cachedInput) || !Add(usage.input, usage.cacheWrite)) { ++invalid; continue; }
        auto id = Identity(event, "id");
        if (!id.empty()) usage.key = "amp:ledger:" + id;
        else if (!thread.empty() && !usage.timestamp.empty()) {
            const auto base = "amp:thread:" + thread + ":" + usage.timestamp + ":" +
                JsonString(event, "operationType") + ":" + Identity(event, "fromMessageId") + ":" + Identity(event, "toMessageId");
            usage.key = base + ":" + std::to_string(occurrences[base]++);
        }
        Finish(usage, emit, invalid);
    }
    // Message-only history lacks a reliable per-call time in older Amp files.
    // Do not synthesize one from thread creation, message ordinal, or file mtime.
}
void PiMessage(const std::string& source, yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    if (JsonString(root, "type") != "message") return;
    auto message = Object(root, "message");
    if (JsonString(message, "role") != "assistant") return;
    if (source == "openclaw" && (JsonString(message, "api") == "openclaw-transcript" ||
        (JsonString(message, "provider") == "openclaw" &&
         (JsonString(message, "model") == "delivery-mirror" || JsonString(message, "model") == "gateway-injected")))) return;
    // Codex app-server mirrors contain only the final response of a turn, and
    // may duplicate the same call in a shared Codex home. Count its complete
    // rollout through the Codex source rather than this incomplete mirror.
    if (source == "openclaw" && JsonString(message, "idempotencyKey").rfind("codex-app-server:", 0) == 0) return;
    auto tokens = Object(message, "usage"); if (!tokens) return;
    ParsedUsage usage; usage.timestamp = Time(root, "timestamp");
    if (source == "openclaw") { const auto own = Time(message, "timestamp"); if (!own.empty()) usage.timestamp = own; }
    usage.model = JsonString(message, "responseModel");
    if (usage.model.empty()) usage.model = JsonString(message, "model");
    usage.provider = JsonString(message, "provider");
    const bool split = Present(tokens, "input") || Present(tokens, "output") || Present(tokens, "cacheRead") || Present(tokens, "cacheWrite");
    if (split) {
        if (!Fields(tokens, "input", "output", "cacheRead", "cacheWrite", usage) ||
            !JsonCounter(tokens, source == "pi" ? "reasoning" : "reasoningTokens", usage.reasoning) ||
            !Add(usage.input, usage.cachedInput) || !Add(usage.input, usage.cacheWrite)) { ++invalid; return; }
        int64_t total = 0;
        if (!JsonCounter(tokens, "totalTokens", total) || (Present(tokens, "totalTokens") && total != usage.input + usage.output)) { ++invalid; return; }
    } else if (!JsonCounter(tokens, "totalTokens", usage.unclassified, true)) { ++invalid; return; }
    auto id = Identity(message, "responseId");
    if (!id.empty()) usage.key = source + ":response:" + id;
    else if (!(id = Identity(root, "id")).empty() && !usage.timestamp.empty())
        usage.key = source + ":event:" + id + ":" + usage.timestamp + ":" + usage.provider + ":" + usage.model;
    Finish(usage, emit, invalid);
}
void CodebuffMessage(yyjson_val* message, const UsageEmitter& emit, int64_t& invalid) {
    auto role = JsonString(message, "variant"); if (role.empty()) role = JsonString(message, "role");
    if (role != "ai" && role != "agent" && role != "assistant") return;
    auto metadata = Object(message, "metadata");
    auto tokens = Object(metadata, "usage");
    if (!tokens) tokens = Object(Object(metadata, "codebuff"), "usage");
    if (!tokens) return;
    ParsedUsage usage; usage.timestamp = Time(message, "timestamp");
    if (usage.timestamp.empty()) usage.timestamp = Time(message, "createdAt");
    if (usage.timestamp.empty()) usage.timestamp = Time(metadata, "timestamp");
    usage.model = JsonString(tokens, "model"); if (usage.model.empty()) usage.model = JsonString(metadata, "model");
    usage.provider = JsonString(tokens, "provider"); if (usage.provider.empty()) usage.provider = JsonString(metadata, "provider");
    const auto id = Identity(message, "id"); if (!id.empty()) usage.key = "codebuff:message:" + id;
    bool valid = false;
    if (Present(tokens, "input_tokens") || Present(tokens, "output_tokens")) {
        valid = Fields(tokens, "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens", usage) &&
            Add(usage.input, usage.cachedInput) && Add(usage.input, usage.cacheWrite);
    } else if (Present(tokens, "inputTokens") || Present(tokens, "outputTokens")) {
        valid = Fields(tokens, "inputTokens", "outputTokens", "cacheReadInputTokens", "cacheCreationInputTokens", usage) &&
            Add(usage.input, usage.cachedInput) && Add(usage.input, usage.cacheWrite);
    } else if (Present(tokens, "prompt_tokens") || Present(tokens, "completion_tokens")) {
        valid = Fields(tokens, "prompt_tokens", "completion_tokens", "cache_read_input_tokens", "cache_creation_input_tokens", usage);
        auto details = Object(tokens, "prompt_tokens_details");
        if (valid && !Present(tokens, "cache_read_input_tokens")) valid = JsonCounter(details, "cached_tokens", usage.cachedInput);
    } else if (Present(tokens, "promptTokens") || Present(tokens, "completionTokens")) {
        valid = Fields(tokens, "promptTokens", "completionTokens", "cacheReadInputTokens", "cacheCreationInputTokens", usage);
        auto details = Object(tokens, "promptTokensDetails");
        if (valid && !Present(tokens, "cacheReadInputTokens")) valid = JsonCounter(details, "cachedTokens", usage.cachedInput);
    } else if (Present(tokens, "totalTokens")) valid = JsonCounter(tokens, "totalTokens", usage.unclassified, true);
    else return; // Credits and RunState snapshots cannot establish per-call tokens.
    if (!valid) { ++invalid; return; }
    const char* totalKey = Present(tokens, "totalTokens") ? "totalTokens" : "total_tokens";
    if (Present(tokens, totalKey)) {
        int64_t total = 0;
        if (!JsonCounter(tokens, totalKey, total, true) || total != usage.input + usage.output + usage.unclassified) { ++invalid; return; }
    }
    Finish(usage, emit, invalid);
}
} // namespace

bool ParseExtendedFamily(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid) {
    if (source == "cline" || source == "roo-code" || source == "roocode" || source == "kilo-code" || source == "kilocode") {
        if (source == "cline" && yyjson_is_obj(root) && yyjson_is_arr(yyjson_obj_get(root, "messages"))) {
            ClineCli(root, emit, invalid); return true;
        }
        const auto task = ParentName(origin); std::map<std::string, size_t> occurrences;
        if (yyjson_is_arr(root)) {
            size_t index, maximum; yyjson_val* entry;
            yyjson_arr_foreach(root, index, maximum, entry) TaskEntry(source, entry, task, occurrences, emit, invalid);
        } else TaskEntry(source, root, task, occurrences, emit, invalid);
        return true;
    }
    if (source == "amp") { Amp(root, emit, invalid); return true; }
    if (source == "pi" || source == "openclaw") {
        if (yyjson_is_arr(root)) {
            size_t index, maximum; yyjson_val* entry;
            yyjson_arr_foreach(root, index, maximum, entry) PiMessage(source, entry, emit, invalid);
        } else PiMessage(source, root, emit, invalid);
        return true;
    }
    if (source == "codebuff") {
        if (yyjson_is_arr(root)) {
            size_t index, maximum; yyjson_val* entry;
            yyjson_arr_foreach(root, index, maximum, entry) CodebuffMessage(entry, emit, invalid);
        } else CodebuffMessage(root, emit, invalid);
        return true;
    }
    // These native files are session aggregates with no per-call timestamps.
    // Their normalized export path remains available without inventing daily use.
    if (source == "droid" || source == "mux") return true;
    return false;
}
} // namespace tokenscope
