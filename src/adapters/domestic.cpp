#include "../adapters.h"
#include "i18n.h"
#include "zstd.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <set>

namespace tokenscope {
namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxLine = 16 * 1024 * 1024;
constexpr int64_t kMaxCounter = INT64_MAX / 8;
yyjson_val* Object(yyjson_val* value, const char* key) {
    auto result = yyjson_obj_get(value, key);
    return yyjson_is_obj(result) ? result : nullptr;
}
bool Has(yyjson_val* value, const char* key) {
    return yyjson_obj_get(value, key) != nullptr;
}
bool Valid(const ParsedUsage& u) {
    return !u.key.empty() && !u.timestamp.empty() && u.input <= kMaxCounter && u.output <= kMaxCounter &&
           u.cachedInput <= u.input && u.cacheWrite <= u.input - u.cachedInput && u.reasoning <= u.output;
}
std::string Timestamp(yyjson_val* value, const char* key) {
    auto timestamp = JsonString(value, key);
    if (!timestamp.empty()) return timestamp;
    int64_t ms = 0;
    return JsonCounter(value, key, ms, true) ? IsoFromUnixMilliseconds(ms) : "";
}
bool AliasCounter(yyjson_val* value, const char* first, const char* second, int64_t& target) {
    if (Has(value, first)) return JsonCounter(value, first, target, true);
    return !second || JsonCounter(value, second, target);
}
void WorkBuddy(yyjson_val* root, const UsageEmitter& emit, int64_t& invalid) {
    const auto type = JsonString(root, "type");
    if (type != "message" && type != "function_call" && type != "assistant") return;
    if (type == "message" && JsonString(root, "role") != "assistant") return;
    auto provider = Object(root, "providerData"), message = Object(root, "message");
    auto raw = Object(provider, "rawUsage"), usage = Object(message, "usage");
    if (!raw && !usage) return;
    ParsedUsage u;
    u.model = JsonString(provider, "model");
    u.provider = JsonString(provider, "provider"); // Never guess provider from a model prefix.
    const auto request = JsonString(provider, "conversationRequestId");
    auto id = JsonString(provider, "messageId");
    if (id.empty()) id = JsonString(root, "id");
    u.key = request.empty() ? id : request + ":" + id;
    u.timestamp = Timestamp(root, "timestamp");
    bool valid = true;
    if (raw) {
        valid =
            JsonCounter(raw, "prompt_tokens", u.input, true) && JsonCounter(raw, "completion_tokens", u.output, true);
        auto promptDetails = Object(raw, "prompt_tokens_details"),
             completionDetails = Object(raw, "completion_tokens_details");
        if (Has(raw, "prompt_cache_hit_tokens"))
            valid = valid && JsonCounter(raw, "prompt_cache_hit_tokens", u.cachedInput, true);
        else if (Has(raw, "cache_read_input_tokens"))
            valid = valid && JsonCounter(raw, "cache_read_input_tokens", u.cachedInput, true);
        else
            valid = valid && JsonCounter(promptDetails, "cached_tokens", u.cachedInput);
        valid = valid && AliasCounter(raw, "prompt_cache_write_tokens", "cache_creation_input_tokens", u.cacheWrite);
        if (Has(completionDetails, "reasoning_tokens"))
            valid = valid && JsonCounter(completionDetails, "reasoning_tokens", u.reasoning, true);
        else
            valid = valid && JsonCounter(raw, "completion_thinking_tokens", u.reasoning);
    } else {
        // WorkBuddy's persisted message.usage already includes cache reads in input_tokens.
        valid = JsonCounter(usage, "input_tokens", u.input, true) &&
                JsonCounter(usage, "output_tokens", u.output, true) &&
                JsonCounter(usage, "cache_read_input_tokens", u.cachedInput) &&
                JsonCounter(usage, "cache_creation_input_tokens", u.cacheWrite) &&
                JsonCounter(usage, "reasoning_tokens", u.reasoning);
    }
    if (id.empty() || !valid || !Valid(u)) {
        ++invalid;
        return;
    }
    // rawUsage.credit is deliberately ignored: credits have no authoritative token/USD conversion.
    emit(u);
}
yyjson_val* LastStreamUsage(yyjson_val* data) {
    // Official v4 compact stream preserves usage as {type:"chunk", time, chunk:{type:"usage",usage}}.
    auto stream = yyjson_obj_get(data, "stream");
    if (!yyjson_is_arr(stream)) return nullptr;
    for (size_t i = yyjson_arr_size(stream); i > 0; --i) {
        auto entry = yyjson_arr_get(stream, i - 1);
        auto chunk = JsonString(entry, "type") == "chunk" ? Object(entry, "chunk") : nullptr;
        if (JsonString(chunk, "type") == "usage")
            if (auto usage = Object(chunk, "usage")) return usage;
    }
    return nullptr;
}
void DeepSeek(yyjson_val* root, const std::string& session, const UsageEmitter& emit, int64_t& invalid) {
    const auto type = JsonString(root, "type");
    if (type != "assistant/message" && type != "assistant/attempt") return;
    auto data = Object(root, "data"), usage = Object(data, "usage");
    if (!usage) usage = LastStreamUsage(data);
    if (!usage) return; // Missing authoritative counters remain uncollected.
    ParsedUsage u;
    int64_t uncached = 0;
    bool valid =
        JsonCounter(usage, "inputTokens", uncached, true) && JsonCounter(usage, "outputTokens", u.output, true) &&
        JsonCounter(usage, "cacheReadTokens", u.cachedInput) && JsonCounter(usage, "cacheWriteTokens", u.cacheWrite) &&
        JsonCounter(usage, "reasoningTokens", u.reasoning);
    u.input = uncached + u.cachedInput + u.cacheWrite; // All three source buckets are disjoint.
    auto message = Object(data, "message");
    auto id = JsonString(message, "id");
    if (!id.empty())
        u.key = "message:" + id; // Message IDs survive forks and copied session archives.
    else {
        int64_t seq = 0;
        if (session.empty() || !JsonCounter(root, "seq", seq, true)) valid = false;
        u.key = "session:" + session + ":attempt:" + std::to_string(seq);
    }
    u.timestamp = Timestamp(root, "time");
    u.model = JsonString(data, "model");
    u.provider = JsonString(data, "provider");
    if (!valid || !Valid(u)) {
        ++invalid;
        return;
    }
    emit(u);
}
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit FileHandle(const fs::path& path)
        : value(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~FileHandle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
class Lines {
public:
    using OnLine = std::function<void(const std::string&)>;
    Lines(OnLine line, int64_t& invalid) : line_(std::move(line)), invalid_(invalid) {}
    void Add(const char* text, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            const char c = text[i];
            if (c == '\n') {
                if (oversized_)
                    ++invalid_;
                else if (!buffer_.empty()) {
                    if (buffer_.back() == '\r') buffer_.pop_back();
                    line_(buffer_);
                }
                buffer_.clear();
                oversized_ = false;
            } else if (!oversized_) {
                if (buffer_.size() == kMaxLine) {
                    buffer_.clear();
                    oversized_ = true;
                } else
                    buffer_ += c;
            }
        }
    }

private:
    OnLine line_;
    int64_t& invalid_;
    std::string buffer_;
    bool oversized_ = false;
};
void ReadFileUsage(const std::string& source, const fs::path& file, const std::atomic_bool& cancelled,
                   const UsageEmitter& emit, AdapterScan& result) {
    FileHandle handle(file);
    if (handle.value == INVALID_HANDLE_VALUE) {
        result.error = Tr8("部分用量文件暂时无法读取");
        return;
    }
    ++result.files;
    bool deepseek = source == "deepseek-harness", headerSeen = !deepseek, skipSeed = false;
    std::string session, model, provider;
    auto line = [&](const std::string& text) {
        if (cancelled) return;
        yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
        if (!doc) {
            ++result.invalid;
            return;
        }
        auto root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            ++result.invalid;
            yyjson_doc_free(doc);
            return;
        }
        const auto type = JsonString(root, "type");
        auto data = Object(root, "data");
        if (deepseek) {
            if (type == "session") {
                int64_t version = 0;
                session = JsonString(root, "id");
                headerSeen = JsonCounter(root, "version", version, true) && version == 4 && !session.empty();
                skipSeed = yyjson_get_bool(yyjson_obj_get(root, "isSeeded"));
                if (!headerSeen) result.error = Tr8("DeepSeek Harness 会话格式暂不支持；仅支持已核实的 v4 日志");
            } else if (type == "request/header") {
                auto config = Object(Object(data, "header"), "config");
                model = JsonString(config, "model");
                provider = JsonString(config, "provider");
            } else if (type == "request/context") {
                model = JsonString(data, "model");
                provider = JsonString(data, "provider");
            } else if (type == "session/end-seed" && yyjson_get_bool(yyjson_obj_get(data, "inherited")))
                skipSeed = false;
        }
        if (headerSeen && !skipSeed)
            ParseDomesticRecord(
                source, root, session,
                [&](const ParsedUsage& parsed) {
                    auto u = parsed;
                    if (deepseek) {
                        if (u.model.empty()) u.model = model;
                        if (u.provider.empty()) u.provider = provider;
                    }
                    emit(u);
                },
                result.invalid);
        yyjson_doc_free(doc);
    };
    Lines lines(line, result.invalid);
    std::array<char, 65536> input{}, output{};
    if (file.extension() != L".zstd") {
        DWORD n = 0;
        while (!cancelled && ReadFile(handle.value, input.data(), static_cast<DWORD>(input.size()), &n, nullptr) && n)
            lines.Add(input.data(), n);
        return; // A non-newline-terminated live tail is retried on the next scan.
    }
    std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> decoder(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!decoder) {
        result.error = Tr8("无法分配 DeepSeek Harness 解压内存");
        return;
    }
    ZSTD_initDStream(decoder.get());
    ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_windowLogMax, 27); // Refuse >128 MiB attacker-controlled windows.
    DWORD n = 0;
    while (!cancelled && ReadFile(handle.value, input.data(), static_cast<DWORD>(input.size()), &n, nullptr) && n) {
        ZSTD_inBuffer in{input.data(), n, 0};
        while (!cancelled && in.pos < in.size) {
            ZSTD_outBuffer out{output.data(), output.size(), 0};
            const auto rc = ZSTD_decompressStream(decoder.get(), &out, &in);
            if (ZSTD_isError(rc)) {
                result.error = Tr8("DeepSeek Harness 压缩日志解码失败；源文件未修改");
                return;
            }
            lines.Add(output.data(), out.pos);
        }
    }
    // Truncated final frame is normal while DSH appends. Keep only complete JSONL records.
}
bool Candidate(const std::string& source, const fs::path& file) {
    if (source == "workbuddy" || source == "codebuddy") return file.extension() == L".jsonl";
    if (source == "deepseek-harness")
        return file.filename() == L"session.v4.jsonl.zstd" || file.filename() == L"session.v4.jsonl";
    return false;
}
} // namespace

void ParseDomesticRecord(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid) {
    if (!yyjson_is_obj(root)) return;
    if (source == "workbuddy" || source == "codebuddy")
        WorkBuddy(root, emit, invalid);
    else if (source == "deepseek-harness")
        DeepSeek(root, origin, emit, invalid);
}

AdapterScan CollectDomestic(const std::string& source, const std::wstring& path, const std::atomic_bool& cancelled,
                            const UsageEmitter& emit) {
    AdapterScan result;
    if (cancelled) return result;
    // Other products have no verified readable native counters yet. The common normalized
    // export collector handles them explicitly; never interpret encrypted DBs or credits.
    if (source != "workbuddy" && source != "codebuddy" && source != "deepseek-harness") return result;
    std::error_code ec;
    const fs::path root(path);
    if (fs::is_regular_file(root, ec)) {
        if (Candidate(source, root)) ReadFileUsage(source, root, cancelled, emit, result);
        return result;
    }
    if (!fs::is_directory(root, ec)) return result;
    std::set<fs::path> seen;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !cancelled; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) || it.depth() > 16) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !Candidate(source, it->path())) continue;
        // Two live/plain/compressed copies of a v4 session are deduplicated by stable IDs.
        if (seen.insert(it->path()).second) ReadFileUsage(source, it->path(), cancelled, emit, result);
    }
    return result;
}
} // namespace tokenscope
