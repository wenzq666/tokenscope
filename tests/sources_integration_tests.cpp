#include "app.h"
#include "adapters.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tokenscope;
namespace fs = std::filesystem;
namespace {
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void Write(const fs::path& path, const std::string& bytes, bool append = false) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    stream << bytes; Require(stream.good(), "fixture write failed");
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Sql(sqlite3* db, const std::string& query) {
    char* error = nullptr; const int result = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "fixture SQL failed"; sqlite3_free(error);
    if (result != SQLITE_OK) throw std::runtime_error(message);
}
int64_t DatabaseCount(const fs::path& path, const char* table) {
    sqlite3* db = nullptr;
    Require(sqlite3_open_v2(WideToUtf8(path.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "database verification open failed");
    sqlite3_stmt* statement = nullptr; const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    const int prepared = sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr);
    if (prepared != SQLITE_OK) { sqlite3_close(db); throw std::runtime_error("verification table missing"); }
    Require(sqlite3_step(statement) == SQLITE_ROW, "verification row missing");
    const auto result = sqlite3_column_int64(statement, 0); sqlite3_finalize(statement); sqlite3_close(db); return result;
}
std::string Name(const std::string& source) {
    static const auto catalog = CatalogSources(); const auto spec = FindSource(catalog, source);
    Require(spec != nullptr, "integration source missing from catalog"); return spec->name;
}
UsageRow Find(const Snapshot& snapshot, const std::string& source, const std::string& model) {
    const auto software = source == "Legacy App" ? source : Name(source);
    for (const auto& row : snapshot.rows) if (row.software == software && row.model == model) return row;
    throw std::runtime_error("expected software/model row missing: " + software + "/" + model);
}
int64_t Sum(const Snapshot& snapshot) { int64_t sum = 0; for (const auto& row : snapshot.rows) sum += row.total(); return sum; }
ScanResult Import(Store& store, const fs::path& path, const std::string& source, const std::atomic_bool& cancelled) {
    auto result = store.ImportFolder(path.wstring(), source, cancelled);
    if (!result.error.empty()) throw std::runtime_error("import " + source + ": " + result.error);
    return result;
}
std::string WorkBuddy(const std::string& id, int input = 100, int output = 20) {
    return "{\"type\":\"message\",\"role\":\"assistant\",\"id\":\"" + id +
        "\",\"timestamp\":\"2026-10-07T10:00:00Z\",\"content\":\"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED\",\"providerData\":{\"model\":\"real-model\",\"provider\":\"real-provider\",\"rawUsage\":{\"prompt_tokens\":" + std::to_string(input) +
        ",\"completion_tokens\":" + std::to_string(output) + ",\"prompt_cache_hit_tokens\":40,\"prompt_cache_write_tokens\":10,\"completion_tokens_details\":{\"reasoning_tokens\":5}}}}\n";
}
void LegacyDatabase(const fs::path& path) {
    sqlite3* db = nullptr; Require(sqlite3_open(WideToUtf8(path.wstring()).c_str(), &db) == SQLITE_OK, "legacy database fixture failed");
    Sql(db, "CREATE TABLE events(event_key TEXT PRIMARY KEY,software TEXT NOT NULL,utc TEXT NOT NULL,day TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,input INTEGER NOT NULL,output INTEGER NOT NULL,cached INTEGER NOT NULL,written INTEGER NOT NULL,reasoning INTEGER NOT NULL,origin TEXT NOT NULL,cost_usd_micros INTEGER);"
        "CREATE TABLE checkpoints(path TEXT NOT NULL,source TEXT NOT NULL,offset INTEGER NOT NULL,size INTEGER NOT NULL,identity TEXT NOT NULL,prefix TEXT NOT NULL,prefix_length INTEGER NOT NULL,session TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,has_previous INTEGER NOT NULL,prev_input INTEGER NOT NULL,prev_output INTEGER NOT NULL,prev_cached INTEGER NOT NULL,prev_written INTEGER NOT NULL,prev_reasoning INTEGER NOT NULL,PRIMARY KEY(path,source));"
        "CREATE TABLE meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL);INSERT INTO meta VALUES('invalid',4);"
        "CREATE TABLE prices(id INTEGER PRIMARY KEY AUTOINCREMENT,software TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,input_rate INTEGER NOT NULL,output_rate INTEGER NOT NULL,cached_rate INTEGER NOT NULL,write_rate INTEGER NOT NULL,created_utc TEXT NOT NULL);"
        "INSERT INTO events VALUES('legacy-identity','Legacy App','2026-10-07T09:00:00Z','2026-10-07','legacy-model','legacy-provider',101,19,20,10,3,'legacy-origin',345);"
        "INSERT INTO prices VALUES(1,'Legacy App','legacy-model','legacy-provider',1000000,2000000,500000,1500000,'2026-10-07T09:00:00Z');"
        "INSERT INTO checkpoints VALUES('legacy-file','codex',1,1,'identity','prefix',1,'session','legacy-model','legacy-provider',0,0,0,0,0,0);PRAGMA user_version=1;");
    sqlite3_close(db);
}
void OpenCodeDatabase(const fs::path& path) {
    fs::create_directories(path.parent_path()); sqlite3* db = nullptr;
    Require(sqlite3_open(WideToUtf8(path.wstring()).c_str(), &db) == SQLITE_OK, "OpenCode fixture open failed");
    Sql(db, "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,data TEXT);"
        "INSERT INTO message VALUES('shared-identity','fixture-session','{\"role\":\"assistant\",\"modelID\":\"opencode-actual\",\"providerID\":\"actual-gateway\",\"time\":{\"created\":1791367200000},\"tokens\":{\"input\":10,\"output\":30,\"reasoning\":20,\"cache\":{\"read\":80,\"write\":15}},\"content\":\"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED\"}');");
    sqlite3_close(db);
}
void CatalogExports(const fs::path& root, const std::atomic_bool& cancelled) {
    const auto sources = CatalogSources(); const auto database = root / L"catalog-usage.sqlite3";
    std::string error;
    {
        Store store; Require(store.Open(database.wstring(), error), error.c_str());
        for (const auto& source : sources) {
            const auto file = root / L"Catalog" / fs::path(source.id) / L"usage.jsonl";
            Write(file, "{\"schema\":\"tokenscope.usage.v1\",\"source\":\"" + source.id +
                "\",\"id\":\"same-catalog-request\",\"timestamp\":\"2026-10-07T10:00:00Z\",\"model\":\"recorded-catalog-model\",\"usage\":{\"input_tokens\":100,\"output_tokens\":20,\"total_tokens\":123}}\n");
            auto result = Import(store, file, source.id, cancelled); Require(result.added == 1, "catalog canonical file source not dispatched");
            const auto row = Find(store.ReadSnapshot(), source.id, "recorded-catalog-model");
            Require(row.input == 100 && row.output == 20 && row.unclassified == 3 && !row.costUsdMicros, "catalog canonical metadata or unknown split changed");
            result = Import(store, file, source.id, cancelled); Require(result.added == 0, "catalog canonical replay added tokens");
            const auto folderFile = file.parent_path() / L"folder-only.JSONL";
            Write(folderFile, "{\"schema\":\"tokenscope.usage.v1\",\"source\":\"" + source.id +
                "\",\"id\":\"folder-catalog-request\",\"timestamp\":\"2026-10-07T10:00:00Z\",\"model\":\"recorded-folder-model\",\"usage\":{\"input_tokens\":7,\"output_tokens\":3}}\n");
            result = Import(store, file.parent_path(), source.id, cancelled); Require(result.added == 1, "catalog canonical folder source not dispatched");
            Require(Find(store.ReadSnapshot(), source.id, "recorded-folder-model").total() == 10, "catalog folder usage missing");
            result = Import(store, file.parent_path(), source.id, cancelled); Require(result.added == 0, "catalog canonical folder replay added tokens");
            const auto apiFile = file.parent_path() / L"api-response.json";
            Write(apiFile, R"({"object":"response","id":"catalog-api-json","created_at":1791367200,"model":"recorded-api-model","usage":{"input_tokens":90,"output_tokens":10,"total_tokens":100}})");
            result = Import(store, apiFile, source.id, cancelled); Require(result.added == 1, "catalog actual API JSON response not dispatched");
            const auto apiLine = file.parent_path() / L"api-response.jsonl";
            Write(apiLine, R"({"object":"chat.completion","id":"catalog-api-jsonl","created":1791367200,"model":"recorded-api-model","usage":{"prompt_tokens":100,"completion_tokens":20,"total_tokens":120}})" "\n");
            result = Import(store, apiLine, source.id, cancelled); Require(result.added == 1, "catalog actual API JSONL response not dispatched");
            Require(Find(store.ReadSnapshot(), source.id, "recorded-api-model").total() == 220, "catalog actual API token totals wrong");
            result = Import(store, file.parent_path(), source.id, cancelled); Require(result.added == 0, "catalog mixed API/normalized folder replay duplicated tokens");
        }
        const auto snapshot = store.ReadSnapshot();
        Require(snapshot.events == static_cast<int64_t>(sources.size()) * 4 && Sum(snapshot) == static_cast<int64_t>(sources.size()) * 353,
            "catalog canonical source identities collided");
    }
    Store reopened; Require(reopened.Open(database.wstring(), error), error.c_str());
    const auto snapshot = reopened.ReadSnapshot();
    Require(snapshot.events == static_cast<int64_t>(sources.size()) * 4 && Sum(snapshot) == static_cast<int64_t>(sources.size()) * 353,
        "catalog canonical Store records not durable");
}
void MixedNativeAndNormalized(const fs::path& root, const std::atomic_bool& cancelled) {
    const auto folder = root / L"Mixed-WorkBuddy";
    Write(folder / L"native.jsonl", WorkBuddy("native-mixed"));
    Write(folder / L"normalized.jsonl", R"({"schema":"tokenscope.usage.v1","source":"workbuddy","id":"export-mixed","timestamp":"2026-10-07T10:00:00Z","model":"export-model","usage":{"input_tokens":7,"output_tokens":3}})" "\n");
    Store store; std::string error; Require(store.Open((root / L"mixed-usage.sqlite3").wstring(), error), error.c_str());
    auto result = Import(store, folder, "workbuddy", cancelled); Require(result.added == 2, "mixed native/normalized folder lost a usage channel");
    const auto snapshot = store.ReadSnapshot();
    Require(snapshot.events == 2 && Sum(snapshot) == 130 && Find(snapshot, "workbuddy", "real-model").total() == 120 &&
        Find(snapshot, "workbuddy", "export-model").total() == 10, "mixed usage counters/models attributed incorrectly");
    result = Import(store, folder, "workbuddy", cancelled); Require(result.added == 0 && Sum(store.ReadSnapshot()) == 130, "mixed source replay duplicated usage");
}
std::string DifyMessage(const std::string& id, const std::string& task, const std::string& run, int input, int output) {
    return "{\"event\":\"message_end\",\"message_id\":\"" + id + "\",\"task_id\":\"" + task +
        "\",\"workflow_run_id\":\"" + run + "\",\"created_at\":1791367200,\"metadata\":{\"usage\":{\"prompt_tokens\":" +
        std::to_string(input) + ",\"completion_tokens\":" + std::to_string(output) + "}}}\n";
}
void DifyRollups(const fs::path& root, const std::atomic_bool& cancelled) {
    for (const bool parentFirst : {false, true}) {
        const auto folder = root / (parentFirst ? L"Dify-parent-first" : L"Dify-child-first");
        const auto database = folder / L"usage.sqlite3";
        const auto parent = folder / L"parent.jsonl", child = folder / L"child.jsonl";
        Write(parent, R"({"event":"workflow_finished","task_id":"task-a","workflow_run_id":"run-a","data":{"id":"run-a","created_at":1791367200,"total_tokens":77}})" "\n");
        Write(child, DifyMessage("child-a", "task-a", "run-a", 50, 20));
        std::string error;
        {
            Store store; Require(store.Open(database.wstring(), error), error.c_str());
            auto result = Import(store, parentFirst ? parent : child, "dify", cancelled); Require(result.added == 1, "Dify initial request missing");
            Require(store.ReadSnapshot().events == 1 && Sum(store.ReadSnapshot()) == (parentFirst ? 77 : 70), "Dify initial aggregate wrong");
            result = Import(store, parentFirst ? child : parent, "dify", cancelled); Require(result.added == 1, "Dify separate scan request missing");
            Require(store.ReadSnapshot().events == 1 && Sum(store.ReadSnapshot()) == 77 && DatabaseCount(database, "events") == 2,
                "Dify parent/child cross-scan usage doubled or original metadata discarded");
            const auto independent = folder / L"independent.jsonl";
            Write(independent, DifyMessage("child-b", "task-b", "run-b", 8, 3));
            result = Import(store, independent, "dify", cancelled); Require(result.added == 1, "Dify unrelated task missing");
            Require(store.ReadSnapshot().events == 2 && Sum(store.ReadSnapshot()) == 88, "Dify unrelated task swallowed by another workflow");
            const auto unassociated = folder / L"unassociated.jsonl";
            Write(unassociated, DifyMessage("unassociated", "", "", 4, 1));
            result = Import(store, unassociated, "dify", cancelled); Require(result.added == 1, "Dify standalone message missing");
            Require(store.ReadSnapshot().events == 3 && Sum(store.ReadSnapshot()) == 93, "Dify unassociated usage guessed into parent workflow");
        }
        {
            Store reopened; Require(reopened.Open(database.wstring(), error), error.c_str());
            auto result = Import(reopened, parent, "dify", cancelled); Require(result.added == 0, "Dify parent replay added request");
            result = Import(reopened, child, "dify", cancelled); Require(result.added == 0, "Dify child replay added request");
            Require(reopened.ReadSnapshot().events == 3 && Sum(reopened.ReadSnapshot()) == 93 && DatabaseCount(database, "events") == 4,
                "Dify rollup relation not durable or replay changed visible usage");
        }
    }
}
} // namespace

int main() {
    wchar_t directory[MAX_PATH]; Require(GetTempPathW(MAX_PATH, directory) > 0, "temp unavailable");
    const auto root = fs::path(directory) / (L"TokenScope-sources-integration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root); const auto database = root / L"用户用量.sqlite3";
        LegacyDatabase(database); std::atomic_bool cancelled = false; std::string error;
        const auto wbFile = root / L"WorkBuddy" / L"session.jsonl";
        Write(wbFile, WorkBuddy("shared-identity"));
        const auto openFile = root / L"OpenCode" / L"opencode.db"; OpenCodeDatabase(openFile);
        const std::string geminiMessage = R"({"id":"shared-identity","type":"gemini","timestamp":"2026-10-07T10:00:00Z","model":"gemini-actual","tokens":{"input":100,"output":20,"cached":40,"thoughts":15,"tool":8,"total":135},"content":"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED"})";
        const auto geminiFile = root / L"Gemini" / L"session-old.json";
        Write(geminiFile, "{\"sessionId\":\"shared-session\",\"projectHash\":\"p\",\"messages\":[" + geminiMessage + "]}");
        const auto qwenFile = root / L"Qwen" / L"session.jsonl";
        Write(qwenFile, R"({"uuid":"total-only","type":"assistant","timestamp":"2026-10-07T10:00:00Z","model":"qwen-actual","usageMetadata":{"totalTokenCount":117},"message":{"content":"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED"}})" "\n");
        const auto exportFile = root / L"授权导出" / L"usage.jsonl";
        Write(exportFile, R"({"schema":"tokenscope.usage.v1","source":"manus","id":"shared-identity","timestamp":"2026-10-07T10:00:00Z","model":"manus-actual","provider":"recorded-provider","usage":{"input_tokens":10,"output_tokens":5},"content":"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED"})" "\n");
        int64_t finalEvents = 0, finalTokens = 0, initialCheckpoints = 0;
        {
            Store store; Require(store.Open(database.wstring(), error), error.c_str());
            auto snapshot = store.ReadSnapshot(); Require(snapshot.error.empty(), snapshot.error.c_str());
            const auto legacy = Find(snapshot, "Legacy App", "legacy-model");
            Require(legacy.total() == 120 && legacy.unclassified == 0 && legacy.costUsdMicros == 345 && snapshot.invalidRecords == 4, "legacy schema migration changed user usage/cost/meta");
            Require(DatabaseCount(database, "checkpoints") == 1 && DatabaseCount(database, "prices") == 1, "legacy checkpoints or price rules lost");
            initialCheckpoints = DatabaseCount(database, "adapter_checkpoints");
            cancelled = true; auto result = store.ImportFolder(wbFile.wstring(), "workbuddy", cancelled);
            Require(result.added == 0 && result.files == 0 && store.ReadSnapshot().events == 1, "cancelled scan imported source data");
            Require(DatabaseCount(database, "adapter_checkpoints") == initialCheckpoints, "cancelled scan created checkpoint"); cancelled = false;

            result = Import(store, wbFile, "workbuddy", cancelled); Require(result.added == 1, "WorkBuddy file import missing");
            result = Import(store, wbFile, "codebuddy", cancelled); Require(result.added == 1, "same message identity from another software collided");
            snapshot = store.ReadSnapshot();
            Require(Find(snapshot, "workbuddy", "real-model").total() == 120 && Find(snapshot, "codebuddy", "real-model").total() == 120, "software namespace isolation failed");
            result = Import(store, openFile, "opencode", cancelled); Require(result.added == 1, "OpenCode database import missing");
            const auto open = Find(store.ReadSnapshot(), "opencode", "opencode-actual");
            Require(open.total() == 135 && open.input == 105 && open.output == 30 && open.cachedInput == 80 && open.cacheWrite == 15, "OpenCode native token normalization changed in Store");
            result = Import(store, geminiFile, "gemini-cli", cancelled); Require(result.added == 1, "Gemini legacy file import missing");
            Require(Find(store.ReadSnapshot(), "gemini-cli", "gemini-actual").total() == 135, "Gemini thought subtotal wrong in Store");
            result = Import(store, qwenFile, "qwen-code", cancelled); Require(result.added == 1, "Qwen total-only import missing");
            const auto qwen = Find(store.ReadSnapshot(), "qwen-code", "qwen-actual");
            Require(qwen.total() == 117 && qwen.unclassified == 117 && qwen.input == 0 && qwen.output == 0 && !qwen.costUsdMicros, "total-only usage was discarded, invented split or wrongly priced");
            result = Import(store, exportFile, "manus", cancelled); Require(result.added == 1, "authorized normalized export import missing");
            Require(Find(store.ReadSnapshot(), "manus", "manus-actual").total() == 15, "normalized export attributed to wrong software");

            snapshot = store.ReadSnapshot(); Require(snapshot.events == 7 && Sum(snapshot) == 762, "multi-source aggregate counts mismatch");
            for (const auto& row : snapshot.rows) if (row.software != "Legacy App") Require(!row.costUsdMicros && row.unpricedRequests == row.requests, "unconfigured price fabricated");
            PriceRule wbPrice; wbPrice.software = Name("workbuddy"); wbPrice.model = "real-model"; wbPrice.provider = "real-provider";
            wbPrice.inputMicrosPerMillion = 2000000; wbPrice.outputMicrosPerMillion = 4000000;
            wbPrice.cachedMicrosPerMillion = 500000; wbPrice.writeMicrosPerMillion = 3000000;
            Require(store.SetPriceRule(wbPrice, error), error.c_str());
            snapshot = store.ReadSnapshot();
            Require(Find(snapshot, "workbuddy", "real-model").costUsdMicros == 230 && !Find(snapshot, "codebuddy", "real-model").costUsdMicros, "source-specific price rule leaked or cache counted twice");
            PriceRule totalPrice; totalPrice.software = Name("qwen-code"); totalPrice.model = "qwen-actual";
            totalPrice.inputMicrosPerMillion = 1000000; totalPrice.outputMicrosPerMillion = 2000000;
            Require(store.SetPriceRule(totalPrice, error), error.c_str());
            Require(!Find(store.ReadSnapshot(), "qwen-code", "qwen-actual").costUsdMicros, "unclassified total priced using invented split");
            Write(qwenFile, R"({"uuid":"total-only","type":"assistant","timestamp":"2026-10-07T10:00:00Z","model":"qwen-actual","usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":17,"totalTokenCount":117},"message":{"content":"PRIVATE-INTEGRATION-BODY-NOT-PERSISTED"}})" "\n");
            result = Import(store, qwenFile, "qwen-code", cancelled); Require(result.added == 0, "enriched usage created a second request");
            const auto enriched = Find(store.ReadSnapshot(), "qwen-code", "qwen-actual");
            Require(enriched.total() == 117 && enriched.input == 100 && enriched.output == 17 && enriched.unclassified == 0 && enriched.costUsdMicros == 134,
                "total-only enrichment doubled total or did not price the real split");

            result = Import(store, wbFile, "workbuddy", cancelled); Require(result.added == 0, "same source checkpoint replay counted twice");
            Write(root / L"WorkBuddy" / L"renamed-copy.jsonl", WorkBuddy("shared-identity"));
            result = Import(store, root / L"WorkBuddy", "workbuddy", cancelled); Require(result.added == 0, "renamed source copy counted twice");
            Write(root / L"Gemini" / L"session-new.jsonl", "{\"sessionId\":\"shared-session\",\"projectHash\":\"p\"}\n" + geminiMessage + "\n");
            result = Import(store, root / L"Gemini", "gemini-cli", cancelled); Require(result.added == 0, "Gemini JSON/JSONL migration counted twice");
            result = store.ImportFolder(wbFile.wstring(), "unregistered-source", cancelled); Require(!result.error.empty(), "unknown source accepted");
            finalEvents = store.ReadSnapshot().events; finalTokens = Sum(store.ReadSnapshot());
            Require(finalEvents == 7 && finalTokens == 762, "replay changed aggregate totals");

            const auto cancelledFile = root / L"Cancelled" / L"session.jsonl"; Write(cancelledFile, WorkBuddy("cancelled-new"));
            const auto before = DatabaseCount(database, "adapter_checkpoints"); cancelled = true;
            result = store.ImportFolder(cancelledFile.wstring(), "workbuddy", cancelled);
            Require(result.added == 0 && DatabaseCount(database, "adapter_checkpoints") == before, "new cancelled source cached"); cancelled = false;
            result = Import(store, cancelledFile, "workbuddy", cancelled); Require(result.added == 1, "cancelled source missed after resume");
            finalEvents = store.ReadSnapshot().events; finalTokens = Sum(store.ReadSnapshot());
            Require(finalEvents == 8 && finalTokens == 882, "resumed source wrong aggregate");
        }
        {
            Store reopened; Require(reopened.Open(database.wstring(), error), error.c_str());
            Require(reopened.ReadSnapshot().events == finalEvents && Sum(reopened.ReadSnapshot()) == finalTokens, "new source data not durable");
            auto result = Import(reopened, root / L"WorkBuddy", "workbuddy", cancelled); Require(result.added == 0, "durable WorkBuddy checkpoint replay added tokens");
            result = Import(reopened, root / L"Gemini", "gemini-cli", cancelled); Require(result.added == 0, "durable Gemini migration replay added tokens");
            result = Import(reopened, openFile, "opencode", cancelled); Require(result.added == 0, "durable OpenCode replay added tokens");
            result = Import(reopened, qwenFile, "qwen-code", cancelled); Require(result.added == 0, "durable Qwen replay added tokens");
            result = Import(reopened, exportFile, "manus", cancelled); Require(result.added == 0, "durable normalized export replay added tokens");
            const auto snapshot = reopened.ReadSnapshot(); Require(snapshot.events == finalEvents && Sum(snapshot) == finalTokens, "restart replay changed totals");
            Require(Find(snapshot, "Legacy App", "legacy-model").costUsdMicros == 345 && snapshot.invalidRecords == 4, "migration or replay changed legacy user data");
        }
        Require(Read(database).find("PRIVATE-INTEGRATION-BODY-NOT-PERSISTED") == std::string::npos &&
            Read(fs::path(database.wstring() + L"-wal")).find("PRIVATE-INTEGRATION-BODY-NOT-PERSISTED") == std::string::npos, "source body persisted in usage database");
        Require(DatabaseCount(openFile, "message") == 1, "OpenCode source database mutated");
        {
            sqlite3* writer = nullptr;
            Require(sqlite3_open(WideToUtf8(openFile.wstring()).c_str(), &writer) == SQLITE_OK, "live OpenCode fixture failed");
            Sql(writer, "PRAGMA journal_mode=WAL;PRAGMA wal_autocheckpoint=0;");
            Store walStore; Require(walStore.Open((root / L"wal-usage.sqlite3").wstring(), error), error.c_str());
            auto result = Import(walStore, openFile, "opencode", cancelled); Require(result.added == 1, "live WAL database initial scan missing");
            const auto mainSize = fs::file_size(openFile); const auto mainTime = fs::last_write_time(openFile);
            Sql(writer, "UPDATE message SET data=replace(data,'\"output\":30','\"output\":31') WHERE id='shared-identity';");
            Require(fs::file_size(openFile) == mainSize && fs::last_write_time(openFile) == mainTime, "WAL fixture unexpectedly rewrote main database");
            result = Import(walStore, openFile, "opencode", cancelled);
            Require(result.added == 0 && Find(walStore.ReadSnapshot(), "opencode", "opencode-actual").total() == 136,
                "SQLite WAL-only update missed by adapter checkpoint or readonly query");
            sqlite3_close(writer);
        }
        CatalogExports(root, cancelled);
        MixedNativeAndNormalized(root, cancelled);
        DifyRollups(root, cancelled);
        Require(fs::equivalent(root.parent_path(), fs::path(directory)) && root.filename().wstring().starts_with(L"TokenScope-sources-integration-"), "cleanup escaped owned fixture directory");
        fs::remove_all(root);
        std::cout << "PASS: multi-source Store integration, all catalog file/folder exports, mixed native/normalized, durable Dify parent/child rollups, legacy migration, duplicate/restart replay, metadata-only storage, exact pricing, total-only enrichment, WAL-only updates, cancelled checkpoints\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\nFixtures retained at " << WideToUtf8(root.wstring()) << "\n"; return 1;
    }
}
