#include "app.h"
#include "adapters.h"
#include "pricing.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <optional>

using namespace tokenscope;
namespace fs = std::filesystem;
namespace {
void Check(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& data, bool append = false) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    out << data;
    Check(out.good(), "fixture write failed");
    static int writeSequence = 0;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(++writeSequence));
}
struct Db {
    sqlite3* db = nullptr;
    explicit Db(const fs::path& path, bool readonly = true) {
        Check(sqlite3_open_v2(WideToUtf8(path.wstring()).c_str(), &db,
                              readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              nullptr) == SQLITE_OK,
              "verification database open failed");
    }
    ~Db() { sqlite3_close(db); }
    void Sql(const std::string& query) {
        char* error = nullptr;
        const int result = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &error);
        const std::string detail = error ? error : "fixture SQL failed";
        sqlite3_free(error);
        if (result != SQLITE_OK) throw std::runtime_error(detail);
    }
    std::optional<int64_t> Number(const char* query, const std::string& key = {}) {
        sqlite3_stmt* s = nullptr;
        Check(sqlite3_prepare_v2(db, query, -1, &s, nullptr) == SQLITE_OK, "verification prepare failed");
        if (!key.empty()) sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<int64_t> result;
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
            result = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return result;
    }
};
int64_t Count(const fs::path& db, const char* table) {
    Db inspect(db);
    const auto sql = "SELECT COUNT(*) FROM " + std::string(table);
    return inspect.Number(sql.c_str()).value_or(-1);
}
std::optional<int64_t> Cost(const fs::path& db, const std::string& id, const std::string& source = "workbuddy") {
    Db inspect(db);
    return inspect.Number("SELECT cost_usd_micros FROM events WHERE event_key=?", source + "|export:" + id);
}
int64_t Version(const fs::path& db, const std::string& id, const char* column = "auto_price_version") {
    Db inspect(db);
    const std::string sql = "SELECT " + std::string(column) + " FROM events WHERE event_key=?";
    return inspect.Number(sql.c_str(), "workbuddy|export:" + id).value_or(0);
}
std::string Software(const std::string& source) {
    const auto sources = CatalogSources();
    const auto* spec = FindSource(sources, source);
    Check(spec != nullptr, "test source absent from catalog");
    return spec->name;
}
void Import(Store& store, const fs::path& file, std::atomic_bool& cancelled, const std::string& source = "workbuddy") {
    const auto r = store.ImportFolder(file.wstring(), source, cancelled);
    if (!r.error.empty()) throw std::runtime_error(r.error);
    Check(r.invalid == 0, "valid pricing fixture marked invalid");
}
std::string Record(const std::string& id, const std::string& model = "pricing-test",
                   const std::string& provider = "openai", int input = 1000000, int output = 100000,
                   int cached = 200000, int written = 0) {
    return "{\"schema\":\"tokenscope.usage.v1\",\"source\":\"workbuddy\",\"id\":\"" + id +
           "\",\"timestamp\":\"2026-10-09T00:00:00Z\",\"model\":\"" + model + "\",\"provider\":\"" + provider +
           "\",\"usage\":{\"input_tokens\":" + std::to_string(input) + ",\"output_tokens\":" + std::to_string(output) +
           ",\"cached_input_tokens\":" + std::to_string(cached) +
           ",\"cache_write_input_tokens\":" + std::to_string(written) + ",\"reasoning_output_tokens\":1000}}\n";
}
void Feeds(const fs::path& dir, bool second = false) {
    for (const auto& spec : PricingFeeds())
        Write(dir / (std::wstring(spec.cacheFile) + L".date"), second ? "2026-10-10" : "2026-10-09");
    Write(dir / L"pricing-litellm.json", second ? R"({
      "pricing-test":{"litellm_provider":"openai","input_cost_per_token":0.000004,"output_cost_per_token":0.000008,"cache_read_input_token_cost":0.000001},
      "cancel-priced":{"litellm_provider":"openai","input_cost_per_token":0.000003,"output_cost_per_token":0.000006,"cache_read_input_token_cost":0.0000005},
      "claude-priced-4-6":{"litellm_provider":"anthropic","input_cost_per_token":0.000003,"output_cost_per_token":0.000015,"cache_read_input_token_cost":0.0000003,"cache_creation_input_token_cost":0.00000375,"cache_creation_input_token_cost_above_1hr":0.000006}
    })"
                                                : R"({
      "pricing-test":{"litellm_provider":"openai","input_cost_per_token":0.000002,"output_cost_per_token":0.000004,"cache_read_input_token_cost":0.0000005},
      "cancel-priced":{"litellm_provider":"openai","input_cost_per_token":0.000003,"output_cost_per_token":0.000006,"cache_read_input_token_cost":0.0000005},
      "claude-priced-4-6":{"litellm_provider":"anthropic","input_cost_per_token":0.000003,"output_cost_per_token":0.000015,"cache_read_input_token_cost":0.0000003,"cache_creation_input_token_cost":0.00000375,"cache_creation_input_token_cost_above_1hr":0.000006}
    })");
    Write(dir / L"model-catalog.json",
          second ? R"({"deepseek":{"models":{"deepseek-priced":{"cost":{"input":2,"output":4,"cache_read":0.4}}}}})"
                 : R"({"deepseek":{"models":{"deepseek-priced":{"cost":{"input":1,"output":2,"cache_read":0.2}}}}})");
    Write(
        dir / L"pricing-openrouter.json",
        second
            ? R"({"data":[{"id":"vendor/router-priced","pricing":{"prompt":"0.000006","completion":"0.000012","input_cache_read":"0.0000012"}}]})"
            : R"({"data":[{"id":"vendor/router-priced","pricing":{"prompt":"0.000003","completion":"0.000006","input_cache_read":"0.0000006"}}]})");
}
std::atomic_bool* cancelAfterInsert = nullptr;
void CancelFunction(sqlite3_context* context, int, sqlite3_value**) {
    if (cancelAfterInsert) *cancelAfterInsert = true;
    sqlite3_result_int(context, 1);
}
int RegisterTestFunction(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_create_function_v2(db, "tokenscope_test_cancel", 0, SQLITE_UTF8 | SQLITE_INNOCUOUS, nullptr,
                                      CancelFunction, nullptr, nullptr, nullptr);
}
void Rollback(const fs::path& root) {
    const auto dir = root / L"rollback", db = dir / L"usage.sqlite3", file = dir / L"cancel.jsonl";
    Feeds(dir);
    Check(sqlite3_auto_extension(reinterpret_cast<void (*)(void)>(RegisterTestFunction)) == SQLITE_OK,
          "test auto-extension registration failed");
    std::atomic_bool cancelled = false;
    std::string error;
    {
        Store store;
        Check(store.Open(db.wstring(), error), error.c_str());
        {
            Db writer(db, false);
            writer.Sql(
                "CREATE TRIGGER fixture_cancel AFTER INSERT ON events BEGIN SELECT tokenscope_test_cancel(); END;");
        }
        Write(file, Record("cancel-first", "cancel-priced"));
        cancelAfterInsert = &cancelled;
        const auto r = store.ImportFolder(file.wstring(), "workbuddy", cancelled);
        cancelAfterInsert = nullptr;
        Check(cancelled && r.error.empty() && r.added == 0, "deterministic mid-import cancellation failed");
        Check(Count(db, "events") == 0 && Count(db, "auto_prices") == 0 && Count(db, "adapter_checkpoints") == 0,
              "cancelled import did not roll back event, quote and checkpoint");
        cancelled = false;
        Write(file, Record("cancel-second", "cancel-priced"));
        Import(store, file, cancelled);
        Check(Cost(db, "cancel-second") == 3100000, "post-rollback request did not price");
        Db inspect(db);
        Check(inspect.Number("SELECT COUNT(*) FROM events e JOIN auto_prices p ON p.id=e.auto_price_version") == 1,
              "quote cache retained rolled-back database version");
    }
    sqlite3_cancel_auto_extension(reinterpret_cast<void (*)(void)>(RegisterTestFunction));
    Store reopened;
    Check(reopened.Open(db.wstring(), error), error.c_str());
    Check(Cost(db, "cancel-second") == 3100000, "post-rollback tariff not durable");
}
void Migration(const fs::path& root) {
    const auto dir = root / L"migration", db = dir / L"v02.sqlite3", file = dir / L"new.jsonl";
    Feeds(dir);
    {
        Db old(db, false);
        old.Sql(
            "CREATE TABLE events(event_key TEXT PRIMARY KEY,software TEXT NOT NULL,utc TEXT NOT NULL,day TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,input INTEGER NOT NULL,output INTEGER NOT NULL,cached INTEGER NOT NULL,written INTEGER NOT NULL,reasoning INTEGER NOT NULL,origin TEXT NOT NULL,cost_usd_micros INTEGER,price_version INTEGER,unclassified INTEGER NOT NULL DEFAULT 0);"
            "INSERT INTO events VALUES('old-priced','Legacy','2026-10-08T00:00:00Z','2026-10-08','pricing-test','openai',1000000,100000,200000,0,1000,'owned-fixture',4321,NULL,0);"
            "INSERT INTO events VALUES('old-pending','Legacy','2026-10-08T00:00:00Z','2026-10-08','pricing-test','openai',1000000,100000,200000,0,1000,'owned-fixture',NULL,NULL,0);PRAGMA user_version=4;");
    }
    Store store;
    std::string error;
    Check(store.Open(db.wstring(), error), error.c_str());
    Check(store.ReloadPricing(error), error.c_str());
    Db inspect(db);
    Check(inspect.Number("SELECT cost_usd_micros FROM events WHERE event_key='old-priced'") == 4321,
          "v0.2 migration overwrote previously priced history");
    Check(inspect.Number("SELECT cost_usd_micros FROM events WHERE event_key='old-pending'") == 2100000,
          "v0.2 pending history did not receive public pricing");
    Check(inspect.Number("SELECT cache_write_1h FROM events WHERE event_key='old-pending'") == 0,
          "v0.2 cache subtype default incorrect");
    Check(inspect.Number("SELECT COUNT(*) FROM events e JOIN auto_prices p ON p.id=e.auto_price_version") == 1,
          "migrated automatic version missing");
    std::atomic_bool cancelled = false;
    Write(file, Record("after-migration"));
    Import(store, file, cancelled);
    Check(Cost(db, "after-migration") == 2100000, "new request failed after migration");
}
void PricingLifecycle(const fs::path& root) {
    const auto dir = root / L"生命周期", db = dir / L"usage.sqlite3", file = dir / L"usage.jsonl",
               claude = dir / L"claude-native.jsonl";
    Feeds(dir);
    std::atomic_bool cancelled = false;
    std::string error;
    int64_t oldVersion = 0;
    {
        Store store;
        Check(store.Open(db.wstring(), error), error.c_str());
        Write(file, Record("old") + Record("stream") + Record("deep-old", "deepseek-priced", "deepseek") +
                        Record("router-old", "vendor/router-priced", "openrouter"));
        Import(store, file, cancelled);
        if (Cost(db, "old") != 2100000 || Cost(db, "deep-old") != 1040000 || Cost(db, "router-old") != 3120000)
            throw std::runtime_error(
                "initial three-feed prices incorrect: " + std::to_string(Cost(db, "old").value_or(-1)) + "," +
                std::to_string(Cost(db, "deep-old").value_or(-1)) + "," +
                std::to_string(Cost(db, "router-old").value_or(-1)));
        oldVersion = Version(db, "old");
        Check(oldVersion > 0 && Version(db, "stream") == oldVersion, "initial quote version not shared/pinned");
        Write(
            claude,
            R"({"type":"assistant","timestamp":"2026-10-09T00:00:00Z","message":{"id":"native-1h","model":"claude-priced-4-6","provider":"anthropic","usage":{"input_tokens":500000,"output_tokens":100000,"cache_read_input_tokens":200000,"cache_creation_input_tokens":300000,"cache_creation":{"ephemeral_5m_input_tokens":200000,"ephemeral_1h_input_tokens":100000}}}})"
            "\n");
        Import(store, claude, cancelled, "claude");
        {
            Db inspect(db);
            Check(inspect.Number("SELECT cost_usd_micros FROM events WHERE event_key='claude|native-1h'") == 4410000,
                  "native Claude 1h tariff incorrect");
            Check(inspect.Number("SELECT cache_write_1h FROM events WHERE event_key='claude|native-1h'") == 100000,
                  "native 1h counter not persisted");
        }
        auto snapshot = store.ReadSnapshot();
        bool sawNative = false;
        for (const auto& row : snapshot.rows)
            if (row.model == "claude-priced-4-6") {
                sawNative = row.cacheWrite1h == 100000 && row.automaticRequests == 1 &&
                            row.priceSources.find("LiteLLM") != std::string::npos;
            }
        Check(sawNative, "1h or price provenance missing from aggregate rows");
        Feeds(dir, true);
        Check(store.ReloadPricing(error), error.c_str());
        Write(file, Record("new") + Record("deep-new", "deepseek-priced", "deepseek") +
                        Record("router-new", "vendor/router-priced", "openrouter") +
                        Record("stream", "pricing-test", "openai", 2000000, 200000, 400000));
        Import(store, file, cancelled);
        Check(Cost(db, "old") == 2100000 && Version(db, "old") == oldVersion, "new tariff rewrote old event");
        Check(Cost(db, "new") == 4200000 && Cost(db, "deep-new") == 2080000 && Cost(db, "router-new") == 6240000,
              "reloaded three-source prices not used by new events");
        Check(Cost(db, "stream") == 4200000 && Version(db, "stream") == oldVersion,
              "stream growth used refreshed tariff instead of pinned quote");
        Write(
            file,
            Record("custom", "pricing-test", "custom-gateway") +
                "{\"schema\":\"tokenscope.usage.v1\",\"source\":\"workbuddy\",\"id\":\"total-only\",\"timestamp\":\"2026-10-09T00:00:00Z\",\"model\":\"pricing-test\",\"provider\":\"openai\",\"usage\":{\"total_tokens\":1000000}}\n");
        Import(store, file, cancelled);
        Check(!Cost(db, "custom") && !Cost(db, "total-only"), "custom gateway or aggregate-only usage invented price");
        PriceRule rule;
        rule.software = Software("workbuddy");
        rule.model = "pricing-test";
        rule.provider = "openai";
        rule.inputMicrosPerMillion = 10000000;
        rule.outputMicrosPerMillion = 20000000;
        rule.cachedMicrosPerMillion = 1000000;
        rule.writeMicrosPerMillion = 12000000;
        Check(store.SetPriceRule(rule, error), error.c_str());
        Write(file, Record("manual-new"));
        Import(store, file, cancelled);
        Check(Cost(db, "manual-new") == 10200000 && Version(db, "manual-new", "price_version") > 0,
              "manual price did not take priority for new event");
        Check(Cost(db, "old") == 2100000 && Cost(db, "stream") == 4200000 && !Cost(db, "total-only"),
              "manual rule rewrote automatic history or invented split");
        rule.provider = "custom-gateway";
        Check(store.SetPriceRule(rule, error), error.c_str());
        Check(Cost(db, "custom") == 10200000, "explicit manual gateway rule did not fill unpriced request");
        Write(dir / L"pricing-litellm.json", "invalid JSON");
        Check(store.ReloadPricing(error), error.c_str());
        Write(file, Record("valid-cache-survives", "cancel-priced"));
        Import(store, file, cancelled);
        Check(Cost(db, "valid-cache-survives") == 3100000,
              "invalid downloaded cache destroyed previous valid in-memory tariff");
    }
    {
        Store reopened;
        Check(reopened.Open(db.wstring(), error), error.c_str());
        Check(reopened.ReloadPricing(error), error.c_str());
        Check(Cost(db, "old") == 2100000 && Cost(db, "new") == 4200000 && Cost(db, "stream") == 4200000 &&
                  Cost(db, "manual-new") == 10200000 && Cost(db, "custom") == 10200000,
              "reopen changed persisted historical prices");
        Write(file, Record("stream", "pricing-test", "openai", 3000000, 300000, 600000));
        Import(reopened, file, cancelled);
        Check(Cost(db, "stream") == 6300000 && Version(db, "stream") == oldVersion,
              "pinned historical quote lost after reopen");
        Db inspect(db);
        Check(
            inspect.Number(
                "SELECT COUNT(*) FROM events WHERE auto_price_version IS NOT NULL AND auto_price_version NOT IN (SELECT id FROM auto_prices)") ==
                0,
            "dangling automatic tariff references");
    }
}
} // namespace
int main() {
    const auto root = fs::current_path() / L"build" /
                      (L"pricing-store-fixtures-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root);
        PricingLifecycle(root);
        Migration(root);
        Rollback(root);
        Check(root.parent_path() == fs::current_path() / L"build" &&
                  root.filename().wstring().starts_with(L"pricing-store-fixtures-"),
              "fixture cleanup escaped workspace");
        fs::remove_all(root);
        std::cout
            << "pricing_store_tests: PASS; three-feed pricing, real 1h counters, historical pinning, streaming growth, manual priority, unknowns, reopen, v0.2 migration and cancelled quote rollback\n";
        return 0;
    } catch (const std::exception& e) {
        cancelAfterInsert = nullptr;
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)(void)>(RegisterTestFunction));
        std::cerr << "pricing_store_tests: FAIL: " << e.what() << "\nFixtures retained: " << WideToUtf8(root.wstring())
                  << '\n';
        return 1;
    }
}
