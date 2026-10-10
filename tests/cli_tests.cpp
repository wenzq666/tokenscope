#include "adapters.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace tokenscope;
namespace fs = std::filesystem;
namespace {
void Require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
void Write(const fs::path& path, const std::string& bytes, bool append = false) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    out << bytes;
    Require(out.good(), "fixture write failed");
}
std::vector<ParsedUsage> Parse(const std::string& source, const std::string& json, int64_t& invalid) {
    auto doc = yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG);
    Require(doc, "fixture JSON invalid");
    std::vector<ParsedUsage> usage;
    ParseCliRecord(
        source, yyjson_doc_get_root(doc), "copied-origin-must-not-change-id",
        [&](const ParsedUsage& u) { usage.push_back(u); }, invalid);
    yyjson_doc_free(doc);
    return usage;
}
void Sql(sqlite3* db, const std::string& query) {
    char* error = nullptr;
    int r = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "fixture SQL failed";
    sqlite3_free(error);
    if (r != SQLITE_OK) throw std::runtime_error(message);
}
const std::string gemini =
    R"({"id":"gemini-msg-1","type":"gemini","timestamp":"2026-10-07T10:00:00Z","model":"recorded-model","tokens":{"input":100,"output":20,"cached":40,"thoughts":15,"tool":8,"total":135},"content":"PRIVATE-MESSAGE-MUST-NOT-BE-EMITTED"})";
const std::string opencode =
    R"({"role":"assistant","modelID":"actual-qwen-id","providerID":"custom-gateway","time":{"created":1791367200000},"tokens":{"input":10,"output":30,"reasoning":20,"cache":{"read":80,"write":15}}})";
} // namespace
int main() {
    wchar_t temp[MAX_PATH];
    Require(GetTempPathW(MAX_PATH, temp) > 0, "temp unavailable");
    const fs::path root = fs::path(temp) / (L"TokenScope-cli-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                            std::to_wstring(GetTickCount64()));
    try {
        int64_t invalid = 0;
        auto usage = Parse("gemini-cli", gemini, invalid);
        Require(usage.size() == 1 && invalid == 0, "Gemini usage lost");
        Require(usage[0].input == 100 && usage[0].cachedInput == 40 && usage[0].output == 35 &&
                    usage[0].reasoning == 15,
                "Gemini cached/tool/thoughts double counted");
        Require(usage[0].model == "recorded-model" && usage[0].provider.empty(), "Gemini historical identity inferred");
        const auto geminiKey = usage[0].key;
        usage = Parse("gemini-cli", "{\"sessionId\":\"s\",\"messages\":[" + gemini + "]}", invalid);
        Require(usage.size() == 1 && usage[0].key == geminiKey, "Gemini JSON migration must preserve request identity");
        usage = Parse("gemini-cli", "{\"$set\":{\"messages\":[" + gemini + "]}}", invalid);
        Require(usage.size() == 1 && usage[0].key == geminiKey, "Gemini legacy full checkpoint lost");
        usage = Parse(
            "gemini-cli",
            R"({"id":"zero","type":"gemini","timestamp":"2026-10-07T10:00:00Z","tokens":{"input":0,"output":0,"cached":0}})",
            invalid);
        Require(usage.size() == 1 && usage[0].model.empty() && usage[0].input == 0, "Gemini zero/unknown discarded");
        usage = Parse(
            "gemini-cli",
            R"({"id":"bad","type":"gemini","timestamp":"2026-10-07T10:00:00Z","tokens":{"input":1,"output":1,"cached":2}})",
            invalid);
        Require(usage.empty() && invalid == 1, "Gemini impossible cache accepted");
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"qwen-id","sessionId":"forked-session","timestamp":"2026-10-07T10:00:00+00:00","model":"qwen-actual","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":10,"thoughtsTokenCount":5,"cachedContentTokenCount":80,"toolUsePromptTokenCount":3}})",
            invalid);
        Require(usage.size() == 1 && usage[0].input == 100 && usage[0].output == 10 && usage[0].cachedInput == 80 &&
                    usage[0].reasoning == 5,
                "Qwen OpenAI-style thoughts counted twice");
        const auto qwenKey = usage[0].key;
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"qwen-id","sessionId":"original-session","timestamp":"2026-10-07T10:00:00+00:00","model":"qwen-actual","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":10,"thoughtsTokenCount":5,"cachedContentTokenCount":80}})",
            invalid);
        Require(usage.size() == 1 && usage[0].key == qwenKey, "Qwen forked copied request identity changed");
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"qwen-inclusive","timestamp":"2026-10-07T10:00:00Z","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":10,"thoughtsTokenCount":5,"totalTokenCount":110}})",
            invalid);
        Require(usage.size() == 1 && usage[0].output == 10, "Qwen explicit inclusive total doubled thoughts");
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"qwen-additive","timestamp":"2026-10-07T10:00:00Z","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":10,"thoughtsTokenCount":5,"totalTokenCount":115}})",
            invalid);
        Require(usage.size() == 1 && usage[0].output == 15, "Qwen explicit additive total lost thoughts");
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"qwen-total-only","timestamp":"2026-10-07T10:00:00Z","usageMetadata":{"totalTokenCount":117}})",
            invalid);
        Require(usage.size() == 1 && usage[0].input == 0 && usage[0].output == 0 && usage[0].unclassified == 117,
                "Qwen total-only count discarded or assigned fabricated split");
        usage =
            Parse("qwen-code",
                  R"({"type":"user","uuid":"user","usageMetadata":{"promptTokenCount":900,"candidatesTokenCount":90}})",
                  invalid);
        Require(usage.empty(), "Qwen user/tool data mistaken for generated usage");
        usage = Parse(
            "qwen-code",
            R"({"type":"assistant","uuid":"bad","timestamp":"2026-10-07T10:00:00Z","usageMetadata":{"promptTokenCount":-1,"candidatesTokenCount":1}})",
            invalid);
        Require(usage.empty() && invalid == 2, "negative source count accepted");
        usage = Parse(
            "kimi-code",
            R"({"timestamp":1791367200.125,"message":{"type":"StatusUpdate","payload":{"message_id":"step-id","token_usage":{"input_other":5,"output":10,"input_cache_read":80,"input_cache_creation":20},"context_tokens":100000}}})",
            invalid);
        Require(usage.size() == 1 && usage[0].input == 105 && usage[0].output == 10 && usage[0].cachedInput == 80 &&
                    usage[0].cacheWrite == 20,
                "Kimi additive cache or context handling wrong");
        Require(usage[0].model.empty() && usage[0].provider.empty() && !usage[0].timestamp.empty(),
                "Kimi current config inferred or epoch ignored");
        usage = Parse(
            "kimi-code",
            R"({"timestamp":1791367200,"message":{"type":"StatusUpdate","payload":{"context_tokens":100000,"context_usage":0.8}}})",
            invalid);
        Require(usage.empty(), "Kimi context occupancy mistaken for usage");
        const std::string kimiV2 =
            R"({"type":"usage.record","model":"friendly-alias","usage":{"inputOther":5,"output":10,"inputCacheRead":80,"inputCacheCreation":20,"raw":{"completion_tokens_details":{"reasoning_tokens":4}}},"usageScope":"turn","time":1791367200125})";
        usage = Parse("kimi-code", kimiV2, invalid);
        Require(usage.size() == 1 && usage[0].input == 105 && usage[0].output == 10 && usage[0].cachedInput == 80 &&
                    usage[0].cacheWrite == 20 && usage[0].model.empty() && usage[0].provider.empty(),
                "new Kimi usage lost, cache doubled, or alias/protocol guessed as actual model/provider");
        usage = Parse(
            "kimi-code",
            R"({"type":"usage.record","model":"friendly-alias","usage":{"inputOther":0,"output":0,"inputCacheRead":0,"inputCacheCreation":0},"time":1791367200125})",
            invalid);
        Require(usage.empty(), "new Kimi emptyUsage fallback mistaken for actual zero-cost usage");
        usage = Parse(
            "kimi-code",
            R"({"type":"context.append_loop_event","event":{"type":"step.end","uuid":"s1","usage":{"inputOther":500,"output":100,"inputCacheRead":0,"inputCacheCreation":0}},"time":1791367200124})",
            invalid);
        Require(usage.empty(), "new Kimi step.end duplicated usage.record");
        usage = Parse(
            "kimi-code",
            R"({"type":"agent.status.updated","usage":{"total":{"inputOther":500,"output":100,"inputCacheRead":0,"inputCacheCreation":0}},"time":1791367200126})",
            invalid);
        Require(usage.empty(), "new Kimi status aggregate duplicated calls");
        usage = Parse(
            "copilot",
            R"({"id":"event-id","type":"assistant.usage","timestamp":"2026-10-07T10:00:00Z","data":{"apiCallId":"call-1","model":"recorded-gpt","modelProvider":"other","inputTokens":100,"outputTokens":30,"cacheReadTokens":60,"cacheWriteTokens":10,"reasoningTokens":20,"cost":9,"copilotUsage":{"totalNanoAiu":8000000000}}})",
            invalid);
        Require(usage.size() == 1 && usage[0].input == 100 && usage[0].output == 30 && usage[0].reasoning == 20 &&
                    usage[0].provider == "other",
                "Copilot per-request counters wrong");
        usage = Parse(
            "copilot",
            R"({"id":"shutdown","type":"session.shutdown","timestamp":"2026-10-08T10:00:00Z","data":{"sessionStartTime":1791280800000,"currentModel":"gpt","modelMetrics":{"gpt":{"usage":{"inputTokens":9000,"outputTokens":500}}}}})",
            invalid);
        Require(usage.empty(), "Copilot multi-day cumulative shutdown assigned to a day");
        usage = Parse(
            "copilot",
            R"({"id":"context","type":"session.usage_info","timestamp":"2026-10-07T10:00:00Z","data":{"currentTokens":200000,"tokenLimit":250000}})",
            invalid);
        Require(usage.empty(), "Copilot context occupancy mistaken for consumption");

        std::atomic_bool cancelled = false;
        std::map<std::string, ParsedUsage> emitted;
        auto emit = [&](const ParsedUsage& u) {
            emitted[u.key] = u;
        };
        Write(root / L"中文目录" / L"session-old.json", "{\"sessionId\":\"s\",\"messages\":[" + gemini + "]}");
        Write(root / L"中文目录" / L"session-new.jsonl",
              "{\"sessionId\":\"s\",\"projectHash\":\"p\"}\n" + gemini + "\nnot-json\n" + gemini.substr(0, 80));
        auto result = CollectCli("gemini-cli", (root / L"中文目录").wstring(), cancelled, emit);
        Require(result.files == 2 && result.invalid == 1 && result.error.empty() && emitted.size() == 1,
                "Gemini migration replay/partial line handling wrong");
        Write(root / L"中文目录" / L"session-new.jsonl", gemini.substr(80) + "\n", true);
        result = CollectCli("gemini-cli", (root / L"中文目录" / L"session-new.jsonl").wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 1 && emitted.size() == 1,
                "single-file import or completed tail wrong");
        cancelled = true;
        result = CollectCli("gemini-cli", (root / L"中文目录").wstring(), cancelled, emit);
        Require(result.files == 0, "CLI cancellation ignored");
        cancelled = false;
        Write(root / L"uppercase" / L"session.JSONL", gemini + "\n" + gemini + "\n");
        result = CollectCli("gemini-cli", (root / L"uppercase" / L"session.JSONL").wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 0 && emitted.size() == 1,
                "uppercase JSONL treated as a full JSON document");
        Write(root / L"oversized" / L"session-limit.jsonl",
              std::string(16 * 1024 * 1024 + 5, 'x') + "\n" + gemini + "\n");
        result = CollectCli("gemini-cli", (root / L"oversized").wstring(), cancelled, emit);
        Require(result.invalid == 1 && emitted.size() == 1, "oversized JSONL not bounded/recovered");

        const auto database = root / L"OpenCode" / L"opencode.db";
        fs::create_directories(database.parent_path());
        sqlite3* db = nullptr;
        Require(sqlite3_open(WideToUtf8(database.wstring()).c_str(), &db) == SQLITE_OK, "fixture database open failed");
        Sql(db,
            "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,data TEXT);CREATE TABLE sentinel(value TEXT);INSERT INTO sentinel VALUES('read-only-original');");
        Sql(db, "INSERT INTO message VALUES('opencode-db-id','session','" + opencode + "');");
        Sql(db,
            "INSERT INTO message VALUES('user-id','session','{\"role\":\"user\",\"tokens\":{\"input\":500,\"output\":30}}');");
        sqlite3_close(db);
        emitted.clear();
        result = CollectCli("opencode", database.wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 0 && result.error.empty() && emitted.size() == 1,
                "OpenCode SQLite not parsed");
        const auto& open = emitted.begin()->second;
        Require(open.input == 105 && open.output == 30 && open.reasoning == 20 && open.cachedInput == 80 &&
                    open.cacheWrite == 15 && open.provider == "custom-gateway",
                "OpenCode cache input normalized incorrectly");
        const auto openKey = open.key;
        const std::string legacy = "{\"id\":\"opencode-db-id\"," + opencode.substr(1);
        Write(root / L"OpenCode" / L"storage" / L"message" / L"session" / L"opencode-db-id.json", legacy);
        result = CollectCli("opencode", (root / L"OpenCode" / L"storage" / L"message").wstring(), cancelled, emit);
        Require(emitted.size() == 1 && emitted.contains(openKey),
                "OpenCode legacy/database migration duplicated message");
        Require(sqlite3_open_v2(WideToUtf8(database.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                    SQLITE_OK,
                "readonly fixture reopen failed");
        sqlite3_stmt* sentinel = nullptr;
        sqlite3_prepare_v2(db, "SELECT value FROM sentinel", -1, &sentinel, nullptr);
        Require(sqlite3_step(sentinel) == SQLITE_ROW && std::string(reinterpret_cast<const char*>(
                                                            sqlite3_column_text(sentinel, 0))) == "read-only-original",
                "source database mutated");
        sqlite3_finalize(sentinel);
        sqlite3_close(db);

        emitted.clear();
        const std::string kimiRequest =
            R"({"type":"llm.request","kind":"loop","model":"served-v2-id","modelAlias":"friendly-alias","provider":"anthropic","time":1791367200120,"systemPrompt":"PRIVATE-MESSAGE-MUST-NOT-BE-EMITTED"})";
        const std::string kimiOrphan =
            R"({"type":"usage.record","model":"unrelated-alias","usage":{"inputOther":7,"output":2,"inputCacheRead":0,"inputCacheCreation":0},"time":1791367200126})";
        const auto kimiWire = root / L"KimiV2" / L"session-a" / L"agents" / L"main" / L"wire.jsonl";
        const auto kimiLines = "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1791367200000}\n" +
                               kimiRequest + "\n" + kimiV2 + "\n" + kimiRequest + "\n" + kimiV2 + "\n" + kimiOrphan +
                               "\n" +
                               R"({"type":"token_counting.measured","tokens":999999,"time":1791367200128})"
                               "\n";
        Write(kimiWire, kimiLines);
        result = CollectCli("kimi-code", kimiWire.wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 0 && emitted.size() == 3,
                "new Kimi identical real calls or context distinction wrong");
        int actualModels = 0, unknownModels = 0;
        for (const auto& [key, row] : emitted) {
            if (row.model == "served-v2-id") {
                ++actualModels;
                Require(row.input == 105 && row.output == 10, "new Kimi actual-model counters changed");
            }
            if (row.model.empty()) ++unknownModels;
        }
        Require(actualModels == 2 && unknownModels == 1,
                "new Kimi request alias association used stale/current configuration");
        Write(root / L"KimiV2" / L"session-copy" / L"agents" / L"main" / L"wire.jsonl", kimiLines);
        result = CollectCli("kimi-code", (root / L"KimiV2").wstring(), cancelled, emit);
        Require(result.files == 2 && result.invalid == 0 && emitted.size() == 3,
                "new Kimi copied/forked wire duplicated calls");

        emitted.clear();
        Write(
            root / L"Copilot" / L"sdk-export.JSONL",
            R"({"id":"sdk-event","type":"assistant.usage","timestamp":"2026-10-07T10:00:00Z","data":{"apiCallId":"sdk-call","model":"recorded-model","inputTokens":10,"outputTokens":5}})"
            "\n");
        result = CollectCli("copilot", (root / L"Copilot" / L"sdk-export.JSONL").wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 0 && emitted.size() == 1 && emitted.begin()->second.output == 5,
                "renamed Copilot SDK JSONL export not collected");

        emitted.clear();
        const std::string csvHeader =
            "Date,Model,Input (w/ Cache Write),Input (w/o Cache Write),Cache Read,Output Tokens,Total Tokens,Cost\r\n";
        const std::string csvRow = "2026-10-07T10:00:00Z,\"custom,model\",30,20,80,10,120,included\r\n";
        Write(root / L"Cursor" / L"usage.csv",
              "\xEF\xBB\xBF" + csvHeader + csvRow + csvRow + "2026-10-07 10:00:00,ambiguous,30,20,80,10,120,0\r\n");
        result = CollectCli("cursor", (root / L"Cursor" / L"usage.csv").wstring(), cancelled, emit);
        Require(result.files == 1 && result.invalid == 1 && emitted.size() == 2,
                "Cursor quoted CSV, duplicate genuine rows, or ambiguous timestamps wrong");
        const auto cursor = emitted.begin()->second;
        Require(cursor.input == 110 && cursor.output == 10 && cursor.cachedInput == 80 && cursor.cacheWrite == 10 &&
                    cursor.model == "custom,model",
                "Cursor CSV cache/write口径 wrong");
        Write(root / L"Cursor" / L"renamed.csv", csvHeader + csvRow + csvRow);
        result = CollectCli("cursor", (root / L"Cursor" / L"renamed.csv").wstring(), cancelled, emit);
        Require(emitted.size() == 2 && result.invalid == 0, "Cursor renamed export replay duplicated rows");
        Write(root / L"Cursor" / L"invalid.csv", "Date,Model,Credits\n2026-10-07T10:00:00Z,gpt,100\n");
        result = CollectCli("cursor", (root / L"Cursor" / L"invalid.csv").wstring(), cancelled, emit);
        Require(result.invalid == 1 && !result.error.empty() && emitted.size() == 2,
                "Cursor credits incorrectly converted to tokens");
        Write(root / L"Cursor" / L"quote.csv", csvHeader + "\"unfinished");
        result = CollectCli("cursor", (root / L"Cursor" / L"quote.csv").wstring(), cancelled, emit);
        Require(result.invalid == 1, "malformed CSV quoting accepted");
        emitted.clear();
        const std::string qwenProjects =
            R"({"type":"assistant","sessionId":"actual-project-session","timestamp":"2026-10-07T10:00:00Z","model":"actual-qwen-model","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":20,"cachedContentTokenCount":40}})";
        Write(root / L"QwenProjects" / L"project" / L"chats" / L"session.jsonl",
              qwenProjects + "\n" + qwenProjects + "\n");
        result = CollectCli("qwen-code", (root / L"QwenProjects").wstring(), cancelled, emit);
        Require(result.invalid == 0 && emitted.size() == 2,
                "Qwen current projects messages without UUID lost or simultaneous calls collapsed");
        Write(root / L"QwenProjects" / L"copied-project" / L"chats" / L"renamed-session.jsonl",
              qwenProjects + "\n" + qwenProjects + "\n");
        result = CollectCli("qwen-code", (root / L"QwenProjects").wstring(), cancelled, emit);
        Require(result.invalid == 0 && emitted.size() == 2, "Qwen copied current projects transcript duplicated calls");
        Require(invalid == 2, "valid source fixtures flagged unexpectedly");
        Require(fs::equivalent(root.parent_path(), fs::path(temp)) &&
                    root.filename().wstring().starts_with(L"TokenScope-cli-tests-"),
                "cleanup outside owned fixture directory");
        fs::remove_all(root);
        std::cout
            << "PASS: OpenCode readonly SQLite/legacy migration, Gemini JSON/JSONL, Qwen replay, Kimi v2 actual-model/legacy/cache/replay, Copilot per-request-only, Cursor strict CSV/cache/replay, bounded partial logs, cancellation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\nFixtures retained at " << WideToUtf8(root.wstring()) << "\n";
        return 1;
    }
}
