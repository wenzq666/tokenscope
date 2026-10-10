#include "../adapters.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

namespace tokenscope {
namespace fs = std::filesystem;
namespace {
constexpr size_t kLineLimit = 16 * 1024 * 1024;
constexpr uint64_t kDocumentLimit = 64 * 1024 * 1024;
constexpr uint64_t kJsonlLimit = 512 * 1024 * 1024;
constexpr int64_t kCounterLimit = INT64_MAX / 16;
constexpr size_t kFileLimit = 10000;

yyjson_val* Object(yyjson_val* root, const char* key) {
    auto value = yyjson_obj_get(root, key);
    return yyjson_is_obj(value) ? value : nullptr;
}
bool Has(yyjson_val* root, const char* key) {
    return yyjson_obj_get(root, key) != nullptr;
}
bool Add(int64_t& value, int64_t other) {
    if (other < 0 || value < 0 || other > kCounterLimit - value) return false;
    value += other;
    return true;
}
bool Digits(const std::string& value, size_t first, size_t count, unsigned& number) {
    if (first + count > value.size()) return false;
    number = 0;
    for (size_t index = first; index < first + count; ++index) {
        if (value[index] < '0' || value[index] > '9') return false;
        number = number * 10 + static_cast<unsigned>(value[index] - '0');
    }
    return true;
}
bool ValidTimestamp(const std::string& value) {
    if (value.size() < 20 || value.size() > 40 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':')
        return false;
    unsigned year, month, day, hour, minute, second;
    if (!Digits(value, 0, 4, year) || !Digits(value, 5, 2, month) || !Digits(value, 8, 2, day) ||
        !Digits(value, 11, 2, hour) || !Digits(value, 14, 2, minute) || !Digits(value, 17, 2, second) || year < 1601 ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return false;
    SYSTEMTIME date{};
    date.wYear = static_cast<WORD>(year);
    date.wMonth = static_cast<WORD>(month);
    date.wDay = static_cast<WORD>(day);
    date.wHour = static_cast<WORD>(hour);
    date.wMinute = static_cast<WORD>(minute);
    date.wSecond = static_cast<WORD>(second);
    FILETIME file{};
    SYSTEMTIME checked{};
    if (!SystemTimeToFileTime(&date, &file) || !FileTimeToSystemTime(&file, &checked) || checked.wYear != year ||
        checked.wMonth != month || checked.wDay != day)
        return false;
    size_t position = 19;
    if (position < value.size() && value[position] == '.') {
        const size_t first = ++position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') ++position;
        if (position == first || position - first > 9) return false;
    }
    if (position + 1 == value.size() && value[position] == 'Z') return true;
    unsigned offsetHours, offsetMinutes;
    return position + 6 == value.size() && (value[position] == '+' || value[position] == '-') &&
           value[position + 3] == ':' && Digits(value, position + 1, 2, offsetHours) &&
           Digits(value, position + 4, 2, offsetMinutes) && offsetHours <= 14 && offsetMinutes <= 59 &&
           (offsetHours < 14 || offsetMinutes == 0);
}
std::string EpochSeconds(yyjson_val* root, const char* key) {
    auto value = yyjson_obj_get(root, key);
    if (!yyjson_is_num(value)) return {};
    const double seconds = yyjson_get_num(value);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 253402300799.0) return {};
    return IsoFromUnixMilliseconds(static_cast<int64_t>(seconds * 1000.0));
}
struct Context {
    std::string timestamp, model, provider, source;
    bool normalized = false;
};
struct Parser {
    const std::string& source;
    const std::atomic_bool& cancelled;
    const UsageEmitter& emit;
    AdapterScan& result;
    int64_t recognized = 0, accepted = 0, completeRecords = 0;
    bool waitingForTail = false;
    struct DifyCandidate {
        ParsedUsage usage;
        bool workflow;
        std::string task, run;
    };
    std::vector<DifyCandidate> dify;
    Parser(const std::string& selectedSource, const std::atomic_bool& stop, const UsageEmitter& callback,
           AdapterScan& scan)
        : source(selectedSource), cancelled(stop), emit(callback), result(scan) {}

    void Finish(ParsedUsage& usage, bool valid, bool deferred = false) {
        ++recognized;
        if (!valid || usage.key.empty() || usage.key.size() > 4096 || !ValidTimestamp(usage.timestamp) ||
            usage.model.size() > 1024 || usage.provider.size() > 1024 || usage.input < 0 || usage.output < 0 ||
            usage.unclassified < 0 || usage.input > kCounterLimit || usage.output > kCounterLimit ||
            usage.unclassified > kCounterLimit || usage.cachedInput < 0 || usage.cacheWrite < 0 ||
            usage.reasoning < 0 || usage.cacheWrite1h < 0 || usage.cacheWrite1h > usage.cacheWrite ||
            usage.cachedInput > usage.input || usage.cacheWrite > usage.input - usage.cachedInput ||
            usage.reasoning > usage.output) {
            ++result.invalid;
            return;
        }
        if (!cancelled) {
            ++accepted;
            if (!deferred) emit(usage);
        }
    }
    ParsedUsage Base(yyjson_val* root, const Context& context, const char* id = "id") {
        ParsedUsage usage;
        const auto identity = JsonString(root, id);
        if (!identity.empty()) usage.key = "export:" + identity;
        usage.timestamp = Has(root, "timestamp") ? JsonString(root, "timestamp") : context.timestamp;
        usage.model = Has(root, "model") ? JsonString(root, "model") : context.model;
        usage.provider = Has(root, "provider") ? JsonString(root, "provider") : context.provider;
        return usage;
    }
    bool Total(yyjson_val* counters, const char* key, ParsedUsage& usage, bool hasSplit) {
        if (!Has(counters, key)) return hasSplit;
        int64_t total = 0;
        if (!JsonCounter(counters, key, total, true)) return false;
        const int64_t classified = usage.input + usage.output;
        if (!hasSplit)
            usage.unclassified = total;
        else if (total < classified)
            return false;
        else
            usage.unclassified = total - classified;
        return true;
    }
    void Normalized(yyjson_val* root, const Context& context) {
        auto usage = Base(root, context);
        auto counters = Object(root, "usage");
        const auto origin = Has(root, "source") ? JsonString(root, "source") : context.source;
        const bool input = Has(counters, "input_tokens"), output = Has(counters, "output_tokens");
        bool valid = origin == source && counters && input == output;
        if (input && output)
            valid = valid && JsonCounter(counters, "input_tokens", usage.input, true) &&
                    JsonCounter(counters, "output_tokens", usage.output, true);
        valid = valid && JsonCounter(counters, "cached_input_tokens", usage.cachedInput) &&
                JsonCounter(counters, "cache_write_input_tokens", usage.cacheWrite) &&
                JsonCounter(counters, "reasoning_output_tokens", usage.reasoning) &&
                JsonCounter(counters, "cache_write_1h_input_tokens", usage.cacheWrite1h) &&
                Total(counters, "total_tokens", usage, input && output);
        Finish(usage, valid);
    }
    void ApiUsage(yyjson_val* root, const Context& context, bool anthropic, bool chat) {
        auto usage = Base(root, context);
        auto counters = Object(root, "usage");
        const char* created = chat ? "created" : "created_at";
        if (Has(root, created)) usage.timestamp = EpochSeconds(root, created);
        const char* input = chat ? "prompt_tokens" : "input_tokens";
        const char* output = chat ? "completion_tokens" : "output_tokens";
        const bool hasInput = Has(counters, input), hasOutput = Has(counters, output);
        bool valid = counters && hasInput == hasOutput;
        if (hasInput && hasOutput)
            valid = valid && JsonCounter(counters, input, usage.input, true) &&
                    JsonCounter(counters, output, usage.output, true);
        if (anthropic) {
            valid = valid && hasInput && hasOutput &&
                    JsonCounter(counters, "cache_read_input_tokens", usage.cachedInput) &&
                    JsonCounter(counters, "cache_creation_input_tokens", usage.cacheWrite) &&
                    JsonCounter(Object(counters, "cache_creation"), "ephemeral_1h_input_tokens", usage.cacheWrite1h) &&
                    Add(usage.input, usage.cachedInput) && Add(usage.input, usage.cacheWrite);
        } else {
            auto inputDetails = Object(counters, chat ? "prompt_tokens_details" : "input_tokens_details");
            auto outputDetails = Object(counters, chat ? "completion_tokens_details" : "output_tokens_details");
            valid = valid && JsonCounter(inputDetails, "cached_tokens", usage.cachedInput) &&
                    JsonCounter(inputDetails, "cache_write_tokens", usage.cacheWrite) &&
                    JsonCounter(outputDetails, "reasoning_tokens", usage.reasoning) &&
                    Total(counters, "total_tokens", usage, hasInput && hasOutput);
        }
        Finish(usage, valid);
    }
    void Dify(yyjson_val* root, const Context& context, bool workflow) {
        auto data = workflow ? Object(root, "data") : root;
        auto usage = Base(data, context, workflow ? "id" : "message_id");
        if (!workflow && usage.key.empty()) {
            const auto identity = JsonString(root, "id");
            if (!identity.empty()) usage.key = "export:" + identity;
        }
        if (Has(data, "created_at")) usage.timestamp = EpochSeconds(data, "created_at");
        usage.rollup = workflow;
        const auto task = JsonString(root, "task_id");
        const auto run = workflow ? JsonString(data, "id") : JsonString(root, "workflow_run_id");
        if (!task.empty()) usage.rollupScopes.push_back("dify:task:" + task);
        if (!run.empty()) usage.rollupScopes.push_back("dify:run:" + run);
        bool valid = source == "dify" && data;
        if (workflow)
            valid = valid && Total(data, "total_tokens", usage, false);
        else {
            auto counters = Object(Object(root, "metadata"), "usage");
            const bool split = Has(counters, "prompt_tokens") && Has(counters, "completion_tokens");
            valid = valid && counters && (Has(counters, "prompt_tokens") == Has(counters, "completion_tokens"));
            if (split)
                valid = valid && JsonCounter(counters, "prompt_tokens", usage.input, true) &&
                        JsonCounter(counters, "completion_tokens", usage.output, true);
            valid = valid && Total(counters, "total_tokens", usage, split);
        }
        const auto before = accepted;
        Finish(usage, valid, true);
        if (accepted != before) {
            if (dify.size() >= 100000) {
                result.error = "Dify export exceeds the 100,000-record limit.";
                return;
            }
            dify.push_back({usage, workflow, JsonString(root, "task_id"),
                            workflow ? JsonString(data, "id") : JsonString(root, "workflow_run_id")});
        }
    }
    void FlushDify() {
        std::set<std::string> tasks, runs;
        for (const auto& candidate : dify)
            if (candidate.workflow) {
                if (!candidate.task.empty()) tasks.insert(candidate.task);
                if (!candidate.run.empty()) runs.insert(candidate.run);
            }
        for (const auto& candidate : dify) {
            if (cancelled) break;
            // A whole workflow total and its own message total are alternative
            // levels of the same usage. Prefer the parent only when the export
            // supplies an explicit shared identity, regardless of record order.
            if (!candidate.workflow && ((!candidate.task.empty() && tasks.count(candidate.task)) ||
                                        (!candidate.run.empty() && runs.count(candidate.run))))
                continue;
            emit(candidate.usage);
        }
    }
    void Coze(yyjson_val* root, const Context& context) {
        auto usage = Base(root, context, "execute_id");
        if (Has(root, "create_time")) usage.timestamp = EpochSeconds(root, "create_time");
        bool valid = source == "coze";
        auto counters = Object(root, "usage");
        if (counters && Has(counters, "token_count")) {
            const bool split = Has(counters, "input_count") && Has(counters, "output_count");
            valid = valid && (Has(counters, "input_count") == Has(counters, "output_count"));
            if (split)
                valid = valid && JsonCounter(counters, "input_count", usage.input, true) &&
                        JsonCounter(counters, "output_count", usage.output, true);
            valid = valid && Total(counters, "token_count", usage, split);
        } else {
            auto token = yyjson_obj_get(root, "token");
            if (yyjson_is_str(token)) {
                const auto text = JsonString(root, "token");
                valid = valid && !text.empty() && text.size() <= 18;
                for (char digit : text) {
                    if (digit < '0' || digit > '9' || usage.unclassified > (kCounterLimit - (digit - '0')) / 10) {
                        valid = false;
                        break;
                    }
                    usage.unclassified = usage.unclassified * 10 + digit - '0';
                }
            } else
                valid = valid && JsonCounter(root, "token", usage.unclassified, true);
        }
        Finish(usage, valid);
    }
    void Walk(yyjson_val* root, Context context = {}, unsigned depth = 0) {
        if (cancelled || depth > 12) return;
        if (yyjson_is_arr(root)) {
            size_t index, count;
            yyjson_val* value;
            yyjson_arr_foreach(root, index, count, value) {
                if (cancelled) break;
                Walk(value, context, depth + 1);
            }
            return;
        }
        if (!yyjson_is_obj(root)) return;
        if (Has(root, "source")) {
            context.source = JsonString(root, "source");
            if (context.source != source) {
                ++recognized;
                ++result.invalid;
                return;
            }
        }
        if (Has(root, "timestamp")) context.timestamp = JsonString(root, "timestamp");
        if (Has(root, "model")) context.model = JsonString(root, "model");
        if (Has(root, "provider")) context.provider = JsonString(root, "provider");
        if (Has(root, "schema")) {
            if (JsonString(root, "schema") != "tokenscope.usage.v1") {
                ++recognized;
                ++result.invalid;
                return;
            }
            context.normalized = true;
        }
        if (context.normalized) {
            if (Has(root, "records")) {
                auto records = yyjson_obj_get(root, "records");
                if (!yyjson_is_arr(records)) {
                    ++recognized;
                    ++result.invalid;
                    return;
                }
                Walk(records, context, depth + 1);
            } else
                Normalized(root, context);
            return;
        }
        const auto event = JsonString(root, "event");
        if (event == "message_end" || event == "workflow_finished") {
            Dify(root, context, event == "workflow_finished");
            return;
        }
        if (Has(root, "execute_id") && (Has(root, "token") || Object(root, "usage"))) {
            Coze(root, context);
            return;
        }
        const auto type = JsonString(root, "type"), object = JsonString(root, "object");
        if (Object(root, "usage") && (object == "chat.completion" || object == "chat.completion.chunk")) {
            ApiUsage(root, context, false, true);
            return;
        }
        if (Object(root, "usage") && object == "response") {
            ApiUsage(root, context, false, false);
            return;
        }
        if (Object(root, "usage") && type == "message" && JsonString(root, "role") == "assistant") {
            ApiUsage(root, context, true, false);
            return;
        }
        // Only known transport wrappers are traversed. Prompt text, output,
        // choices, credentials and arbitrary node/execution trees stay opaque.
        for (const char* key : {"response", "data", "records", "responses", "message"}) {
            auto child = yyjson_obj_get(root, key);
            if (yyjson_is_obj(child) || yyjson_is_arr(child)) Walk(child, context, depth + 1);
        }
    }
    void ReadJson(std::string& text) {
        if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        if (text.find_first_not_of(" \r\n\t") == std::string::npos) return;
        ++completeRecords;
        auto document = yyjson_read(text.data(), text.size(), YYJSON_READ_NOFLAG);
        if (!document) {
            ++result.invalid;
            return;
        }
        Walk(yyjson_doc_get_root(document));
        yyjson_doc_free(document);
    }
};
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit File(const fs::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~File() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
void ReadFileUsage(const fs::path& path, bool jsonl, Parser& parser) {
    File file(path);
    if (file.handle == INVALID_HANDLE_VALUE) {
        parser.result.error = "Cannot open usage export read-only.";
        return;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0) {
        parser.result.error = "Cannot read usage export size.";
        return;
    }
    if (static_cast<uint64_t>(size.QuadPart) > (jsonl ? kJsonlLimit : kDocumentLimit)) {
        ++parser.result.invalid;
        parser.result.error = "Usage export exceeds the JSON (64 MiB) or JSONL (512 MiB) limit.";
        return;
    }
    ++parser.result.files;
    std::array<char, 65536> buffer{};
    std::string pending;
    bool oversized = false;
    uint64_t consumed = 0;
    while (!parser.cancelled) {
        DWORD received = 0;
        if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr)) {
            parser.result.error = "Cannot read usage export.";
            return;
        }
        if (!received) break;
        consumed += received;
        if (consumed > (jsonl ? kJsonlLimit : kDocumentLimit)) {
            ++parser.result.invalid;
            parser.result.error = "Usage export grew beyond its size limit while reading.";
            return;
        }
        if (!jsonl) {
            pending.append(buffer.data(), received);
            continue;
        }
        for (DWORD index = 0; index < received; ++index) {
            if (parser.cancelled) return;
            const char character = buffer[index];
            if (character == '\n') {
                if (oversized)
                    ++parser.result.invalid;
                else
                    parser.ReadJson(pending);
                pending.clear();
                oversized = false;
            } else if (!oversized) {
                if (pending.size() < kLineLimit)
                    pending.push_back(character);
                else {
                    pending.clear();
                    oversized = true;
                }
            }
        }
    }
    if (!jsonl && !parser.cancelled) parser.ReadJson(pending);
    if (jsonl && (!pending.empty() || oversized)) parser.waitingForTail = true;
    // An unterminated JSONL tail belongs to a writer in progress. Wait for its
    // newline on the next scan rather than persisting a fragment as zero usage.
}
bool Extension(const fs::path& path, bool& jsonl) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    jsonl = extension == L".jsonl";
    return jsonl || extension == L".json";
}
} // namespace

AdapterScan CollectUsageExport(const std::string& source, const std::wstring& path, const std::atomic_bool& cancelled,
                               const UsageEmitter& emit) {
    AdapterScan result;
    const auto catalog = CatalogSources();
    if (!FindSource(catalog, source)) {
        result.error = "Unknown usage source; choose a catalog entry.";
        return result;
    }
    if (cancelled) return result;
    const fs::path root(path);
    std::error_code error;
    std::vector<fs::path> files;
    if (fs::is_regular_file(root, error))
        files.push_back(root);
    else if (fs::is_directory(root, error)) {
        fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end && !cancelled; iterator.increment(error)) {
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (iterator.depth() >= 12) iterator.disable_recursion_pending();
            if (!iterator->is_regular_file(error)) continue;
            bool jsonl = false;
            if (Extension(iterator->path(), jsonl)) files.push_back(iterator->path());
            if (files.size() >= kFileLimit) {
                result.error = "Usage export folder exceeds the 10,000-file limit.";
                break;
            }
        }
    } else {
        result.error = "Usage export file or folder does not exist.";
        return result;
    }
    if (error) result.error = "Cannot enumerate usage export folder.";
    std::sort(files.begin(), files.end());
    Parser parser{source, cancelled, emit, result};
    for (const auto& file : files) {
        if (cancelled) break;
        bool jsonl = false;
        if (!Extension(file, jsonl)) {
            result.error = "Only JSON and JSONL usage exports are supported here.";
            continue;
        }
        ReadFileUsage(file, jsonl, parser);
    }
    parser.FlushDify();
    if (!cancelled && !parser.accepted && result.error.empty() && !(parser.waitingForTail && !parser.completeRecords)) {
        result.error =
            parser.recognized || result.invalid
                ? "No valid usage records; check source, request ID, timestamp and Token fields."
                : "No supported Token records. Import tokenscope.usage.v1 or an actual supported API usage response; chat text and credits are not usage.";
    }
    return result;
}
} // namespace tokenscope
