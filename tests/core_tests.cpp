#include "../src/app.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace tokenscope;
namespace {
void Require(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}
void Write(const fs::path& p, const std::string& text, bool append = false) {
    std::ofstream out(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    Require(bool(out), "cannot create fixture");
    out << text;
    Require(bool(out), "cannot write fixture");
}
std::string Usage(const char* time, int input, int output, int cached = 0, int reasoning = 0, bool last = true) {
    std::string values = "{\"input_tokens\":" + std::to_string(input) + ",\"output_tokens\":" + std::to_string(output) +
                         ",\"cached_input_tokens\":" + std::to_string(cached) +
                         ",\"reasoning_output_tokens\":" + std::to_string(reasoning) + "}";
    return "{\"timestamp\":\"" + std::string(time) +
           "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":" + values +
           (last ? ",\"last_token_usage\":" + values : "") + "}}}\n";
}
const UsageRow* Find(const Snapshot& s, const std::string& app, const std::string& model) {
    for (const auto& r : s.rows)
        if (r.software == app && r.model == model) return &r;
    return nullptr;
}
int64_t Sum(const Snapshot& s) {
    int64_t n = 0;
    for (const auto& r : s.rows) n += r.total();
    return n;
}
} // namespace
int main() {
    wchar_t temp[MAX_PATH];
    Require(GetTempPathW(MAX_PATH, temp) > 0, "temp directory unavailable");
    fs::path root = fs::path(temp) / (L"TokenScope-core-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root / L"中文日志" / L"codex");
        fs::create_directories(root / L"中文日志" / L"claude");
        std::atomic_bool cancelled = false;
        std::string error;
        const auto codex = root / L"中文日志" / L"codex", claude = root / L"中文日志" / L"claude";
        std::string log =
            "{\"type\":\"session_meta\",\"payload\":{\"id\":\"session-A\",\"model_provider\":\"recorded-provider\",\"tool_example\":{\"schema\":\"tokenscope.usage.v1\"}}}\n"
            "{\"type\":\"turn_context\",\"payload\":{\"model\":\"model-A\"}}\n";
        log += Usage("2026-10-07T08:00:00Z", 100, 10, 20, 3);
        log += Usage("2026-10-07T08:00:01Z", 100, 10, 20, 3); // Identical cumulative notification.
        log += "malformed JSON\n{\"type\":\"turn_context\",\"payload\":{\"model\":\"model-B\"}}\n";
        log += Usage("2026-10-07T08:01:00Z", 250, 25, 60, 7, false); // Delta, total-only record.
        log += Usage("2026-10-07T08:02:00Z", 20, 2);                 // Reset cumulative counters.
        log += "{\"type\":\"response_item\",\"payload\":{\"content\":\"PRIVATE-CONTENT-MUST-NOT-BE-PERSISTED\"}}\n";
        log += "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":null}}\n";
        Write(codex / L"rollout-中文.jsonl", log);
        Write(codex / L"session-settings.json",
              "{\"current_model\":\"unrelated-settings\",\"tool_example\":{\"schema\":\"tokenscope.usage.v1\"}}");
        {
            Store store;
            Require(store.Open((root / L"用量.sqlite3").wstring(), error), error.c_str());
            auto r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.error.empty(), r.error.c_str());
            Require(r.added == 3, "Codex cumulative or reset event count wrong");
            Require(r.invalid == 1, "bad complete line must be reported once");
            auto s = store.ReadSnapshot();
            Require(s.events == 3, "events duplicated");
            auto a = Find(s, "Codex", "model-A"), b = Find(s, "Codex", "model-B");
            Require(a && a->input == 100 && a->output == 10 && a->cachedInput == 20 && a->reasoning == 3,
                    "model A counters wrong");
            Require(b && b->input == 170 && b->output == 17 && b->cachedInput == 40 && b->reasoning == 4,
                    "model switch/reset delta wrong");
            Require(Sum(s) == 297, "cache or reasoning double counted");
            Require(a->provider == "recorded-provider", "recorded provider lost");
            for (const auto& row : s.rows)
                Require(!row.costUsdMicros && row.unpricedRequests == row.requests,
                        "unknown price must remain unknown");
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && r.invalid == 0 && Sum(store.ReadSnapshot()) == 297,
                    "incremental replay counted twice");
            Write(codex / L"archived-copy.jsonl", log);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && Sum(store.ReadSnapshot()) == 297, "archive/import duplicate counted twice");

            std::string next = Usage("2026-10-07T08:03:00Z", 50, 7);
            Write(codex / L"rollout-中文.jsonl", next.substr(0, next.size() / 2), true);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && r.invalid == 0, "incomplete tail consumed");
            Write(codex / L"rollout-中文.jsonl", next.substr(next.size() / 2), true);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1 && Sum(store.ReadSnapshot()) == 332, "completed tail not consumed exactly once");
            Write(codex / L"rollout-中文.jsonl",
                  "{\"type\":\"turn_context\",\"payload\":{}}\n" + Usage("2026-10-07T08:04:00Z", 60, 8), true);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            s = store.ReadSnapshot();
            auto unknown = Find(s, "Codex", "");
            Require(r.added == 1 && unknown && unknown->total() == 11, "unknown model inferred or discarded");
            Write(codex / L"zero.jsonl", "{\"type\":\"session_meta\",\"payload\":{\"id\":\"zero-session\"}}\n" +
                                             Usage("2026-10-07T08:05:00Z", 0, 0));
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1, "valid zero usage was discarded");
            const std::string anonymous = Usage("2026-10-07T08:06:00Z", 7, 3);
            Write(codex / L"no-session-id.jsonl", anonymous);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1, "anonymous session usage lost");
            Write(codex / L"renamed-anonymous-copy.jsonl", anonymous);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0, "anonymous renamed copy double counted by path");
            Write(
                codex / L"last-only.jsonl",
                "{\"timestamp\":\"2026-10-07T08:07:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":3,\"output_tokens\":1}}}}\n");
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1, "last-only record lost");
            const std::string rewritePrefix =
                "{\"type\":\"session_meta\",\"payload\":{\"id\":\"same-length-rewrite-session\",\"model_provider\":\"rewrite-provider\"}}\n"
                "{\"type\":\"turn_context\",\"payload\":{\"model\":\"rewrite-model\"}}\n";
            Write(codex / L"same-length-rewrite.jsonl", rewritePrefix + Usage("2026-10-07T08:09:00Z", 4, 2));
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1, "rewrite baseline missing");
            const auto oldWrite = fs::last_write_time(codex / L"same-length-rewrite.jsonl");
            Write(codex / L"same-length-rewrite.jsonl", rewritePrefix + Usage("2026-10-07T08:09:01Z", 5, 2));
            fs::last_write_time(codex / L"same-length-rewrite.jsonl", oldWrite + std::chrono::seconds(2));
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1, "same-length replacement skipped by checkpoint");

            std::string cl =
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:10:00Z\",\"uuid\":\"part1\",\"message\":{\"type\":\"message\",\"role\":\"assistant\",\"id\":\"msg-A\",\"model\":\"qwen-recorded\",\"usage\":{\"input_tokens\":5,\"output_tokens\":4,\"cache_read_input_tokens\":30,\"cache_creation_input_tokens\":20}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:10:01Z\",\"uuid\":\"part2\",\"message\":{\"id\":\"msg-A\",\"model\":\"qwen-recorded\",\"usage\":{\"input_tokens\":5,\"output_tokens\":4,\"cache_read_input_tokens\":30,\"cache_creation_input_tokens\":20}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:10:02Z\",\"uuid\":\"part3\",\"message\":{\"id\":\"msg-A\",\"model\":\"qwen-recorded\",\"usage\":{\"input_tokens\":5,\"output_tokens\":9}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:11:00+00:00\",\"message\":{\"id\":\"msg-zero\",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:11:01Z\",\"message\":{\"id\":\"msg-formula\",\"model\":\"=1+1\",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:12:00Z\",\"message\":{\"id\":\"synthetic\",\"model\":\"<synthetic>\",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T08:13:00Z\",\"message\":{\"id\":\"negative\",\"usage\":{\"input_tokens\":-1,\"output_tokens\":2}}}\n";
            Write(claude / L"会话.jsonl", cl);
            r = store.ImportFolder(claude.wstring(), "claude", cancelled);
            Require(r.added == 3 && r.invalid == 1, "Claude replay/synthetic/invalid handling wrong");
            s = store.ReadSnapshot();
            auto c = Find(s, "Claude Code", "qwen-recorded");
            Require(c && c->input == 55 && c->output == 9 && c->cachedInput == 30 && c->cacheWrite == 20 &&
                        c->requests == 1,
                    "Claude cache or stable-ID merging wrong");
            Require(c->provider.empty(), "provider guessed from product despite custom model");
            auto z = Find(s, "Claude Code", "");
            Require(z && z->total() == 0 && z->requests == 1, "Claude zero unknown model lost");
            r = store.ImportFolder(claude.wstring(), "claude", cancelled);
            Require(r.added == 0 && r.invalid == 0, "Claude incremental replay counted twice");
            Write(claude / L"resume-copy.jsonl", cl);
            r = store.ImportFolder(claude.wstring(), "claude", cancelled);
            Require(r.added == 0 && Find(store.ReadSnapshot(), "Claude Code", "qwen-recorded")->requests == 1,
                    "Claude copied session double counted");
            Require(store.ExportCsv((root / L"导出.csv").wstring(), s, error), error.c_str());
            std::ifstream csv(root / L"导出.csv", std::ios::binary);
            std::string csvText((std::istreambuf_iterator<char>(csv)), {});
            Require(csvText.find("qwen-recorded") != std::string::npos && csvText.starts_with("\xEF\xBB\xBF"),
                    "UTF-8 CSV export incorrect");
            Require(csvText.find("\"'=1+1\"") != std::string::npos, "CSV text formula not escaped");
            cancelled = true;
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.files == 0 && r.added == 0, "cancel ignored");
            cancelled = false;
            r = store.ImportFolder(codex.wstring(), "unsupported", cancelled);
            Require(!r.error.empty(), "unsupported source silently accepted");
            Require(ShiftDay("2024-02-28", 1) == "2024-02-29" && ShiftDay("2026-12-31", 1) == "2027-01-01",
                    "date arithmetic wrong");
            Require(ShiftDay("2026-02-30", 1).empty(), "invalid calendar date accepted");
            Require(WideToUtf8(Utf8ToWide("中文模型")) == "中文模型", "Unicode round trip failed");
            Write(codex / L"oversized-tail.jsonl", std::string(16 * 1024 * 1024 + 32, 'x'));
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && r.invalid == 0, "unterminated oversized line consumed");
            Write(codex / L"oversized-tail.jsonl", "\n" + Usage("2026-10-07T08:07:30Z", 2, 1), true);
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1 && r.invalid == 1, "oversized complete line not bounded/recovered");
            Write(codex / L"partial-reopen.jsonl", Usage("2026-10-07T08:08:00Z", 6, 2).substr(0, 35));
            r = store.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && r.invalid == 0, "partial tail not preserved before close");
        }
        {
            Store reopened;
            Require(reopened.Open((root / L"用量.sqlite3").wstring(), error), error.c_str());
            auto before = reopened.ReadSnapshot();
            auto r = reopened.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 0 && reopened.ReadSnapshot().events == before.events,
                    "checkpoint not durable across reopen");
            Write(codex / L"partial-reopen.jsonl", Usage("2026-10-07T08:08:00Z", 6, 2).substr(35), true);
            r = reopened.ImportFolder(codex.wstring(), "codex", cancelled);
            Require(r.added == 1 && reopened.ReadSnapshot().events == before.events + 1,
                    "partial tail lost across reopen");
        }
        {
            std::ifstream db(root / L"用量.sqlite3", std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(db)), {});
            Require(bytes.find("PRIVATE-CONTENT-MUST-NOT-BE-PERSISTED") == std::string::npos,
                    "conversation body persisted in database");
        }
        {
            // Numbered migrations: the baseline is followed by v7, and a newer schema is refused.
            const auto schemaPath = root / L"schema.sqlite3";
            {
                Store fresh;
                Require(fresh.Open(schemaPath.wstring(), error), error.c_str());
            }
            auto query = [&](const char* sql) {
                sqlite3* db = nullptr;
                Require(sqlite3_open(WideToUtf8(schemaPath.wstring()).c_str(), &db) == SQLITE_OK,
                        "cannot inspect schema fixture");
                sqlite3_stmt* statement = nullptr;
                Require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
                const int64_t value = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
                sqlite3_finalize(statement);
                sqlite3_close(db);
                return value;
            };
            Require(query("PRAGMA user_version") == 7, "fresh database did not reach schema v7");
            Require(query("SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='events_unpriced'") == 1,
                    "v7 migration index missing");
            Require(query("PRAGMA user_version=99") == -1, "cannot simulate a newer schema");
            Store newer;
            Require(!newer.Open(schemaPath.wstring(), error) && error.find("v99") != std::string::npos,
                    "newer schema was opened by an older build");
        }
        fs::create_directories(root / L"pricing-claude");
        fs::create_directories(root / L"pricing-codex");
        const auto priceClaude = root / L"pricing-claude", priceCodex = root / L"pricing-codex";
        auto pricedMessage = [](const char* id, int output) {
            return "{\"type\":\"assistant\",\"timestamp\":\"2026-10-07T10:00:00Z\",\"message\":{\"id\":\"" +
                   std::string(id) +
                   "\",\"model\":\"priced-model\",\"usage\":{\"input_tokens\":100,\"output_tokens\":" +
                   std::to_string(output) + ",\"cache_read_input_tokens\":20,\"cache_creation_input_tokens\":30}}}\n";
        };
        Write(priceClaude / L"prices.jsonl", pricedMessage("priced-A", 10));
        {
            Store priced;
            Require(priced.Open((root / L"prices.sqlite3").wstring(), error), error.c_str());
            auto r = priced.ImportFolder(priceClaude.wstring(), "claude", cancelled);
            Require(r.added == 1, "pricing fixture missing");
            PriceRule rule;
            rule.software = "Claude Code";
            rule.model = "priced-model";
            rule.inputMicrosPerMillion = 2000000;
            rule.outputMicrosPerMillion = 4000000;
            rule.cachedMicrosPerMillion = 500000;
            rule.writeMicrosPerMillion = 3000000;
            Require(priced.SetPriceRule(rule, error), error.c_str());
            auto s = priced.ReadSnapshot();
            auto p = Find(s, "Claude Code", "priced-model");
            Require(p && p->costUsdMicros == 340 && p->unpricedRequests == 0,
                    "cached/write cost counted twice or fixed-point arithmetic wrong");
            rule.inputMicrosPerMillion = 10000000;
            rule.outputMicrosPerMillion = 20000000;
            rule.cachedMicrosPerMillion = 2000000;
            rule.writeMicrosPerMillion = 6000000;
            Require(priced.SetPriceRule(rule, error), error.c_str());
            Require(Find(priced.ReadSnapshot(), "Claude Code", "priced-model")->costUsdMicros == 340,
                    "new price overwrote priced history");
            Write(priceClaude / L"prices.jsonl", pricedMessage("priced-B", 10), true);
            r = priced.ImportFolder(priceClaude.wstring(), "claude", cancelled);
            Require(r.added == 1 && Find(priced.ReadSnapshot(), "Claude Code", "priced-model")->costUsdMicros == 1760,
                    "future record did not use latest price");
            Write(priceClaude / L"prices.jsonl", pricedMessage("priced-A", 20), true);
            r = priced.ImportFolder(priceClaude.wstring(), "claude", cancelled);
            Require(r.added == 0 && Find(priced.ReadSnapshot(), "Claude Code", "priced-model")->costUsdMicros == 1800,
                    "Claude usage growth did not retain old bound price");
            Write(priceClaude / L"duplicate-copy.jsonl", pricedMessage("priced-A", 10) + pricedMessage("priced-B", 10));
            r = priced.ImportFolder(priceClaude.wstring(), "claude", cancelled);
            Require(r.added == 0 && Find(priced.ReadSnapshot(), "Claude Code", "priced-model")->costUsdMicros == 1800,
                    "replay changed cost");
            PriceRule invalid = rule;
            invalid.model.clear();
            Require(!priced.SetPriceRule(invalid, error), "unknown model accepted for pricing");
            invalid = rule;
            invalid.inputMicrosPerMillion = -1;
            Require(!priced.SetPriceRule(invalid, error), "negative price accepted");
            invalid = rule;
            invalid.inputMicrosPerMillion = 10000000001LL;
            Require(!priced.SetPriceRule(invalid, error), "over-limit price accepted");

            const std::string codexPrice =
                "{\"type\":\"session_meta\",\"payload\":{\"id\":\"priced-codex-session\",\"model_provider\":\"test-provider\"}}\n"
                "{\"type\":\"turn_context\",\"payload\":{\"model\":\"codex-priced\"}}\n"
                "{\"timestamp\":\"2026-10-07T11:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":100,\"output_tokens\":10,\"cached_input_tokens\":20,\"cache_write_input_tokens\":30}}}}\n"
                "{\"timestamp\":\"2026-10-07T11:01:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":200,\"output_tokens\":20,\"cached_input_tokens\":100,\"cache_write_input_tokens\":130}}}}\n";
            Write(priceCodex / L"prices.jsonl", codexPrice);
            r = priced.ImportFolder(priceCodex.wstring(), "codex", cancelled);
            Require(r.added == 2, "mixed pricing fixture missing");
            PriceRule cp;
            cp.software = "Codex";
            cp.model = "codex-priced";
            cp.provider = "test-provider";
            cp.inputMicrosPerMillion = 2000000;
            cp.outputMicrosPerMillion = 4000000;
            cp.cachedMicrosPerMillion = 500000;
            cp.writeMicrosPerMillion = 3000000;
            Require(priced.SetPriceRule(cp, error), error.c_str());
            s = priced.ReadSnapshot();
            p = Find(s, "Codex", "codex-priced");
            Require(p && p->requests == 2 && p->unpricedRequests == 1 && p->costUsdMicros == 240,
                    "partial known cost lost or ambiguous cache categories priced");
            Write(
                priceCodex / L"huge.jsonl",
                "{\"type\":\"turn_context\",\"payload\":{\"model\":\"huge-model\"}}\n"
                "{\"timestamp\":\"2026-10-07T12:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":1000000000000000000,\"output_tokens\":0}}}}\n");
            r = priced.ImportFolder(priceCodex.wstring(), "codex", cancelled);
            Require(r.added == 1, "overflow fixture missing");
            PriceRule huge;
            huge.software = "Codex";
            huge.model = "huge-model";
            huge.inputMicrosPerMillion = 10000000000LL;
            Require(priced.SetPriceRule(huge, error), error.c_str());
            s = priced.ReadSnapshot();
            p = Find(s, "Codex", "huge-model");
            Require(p && !p->costUsdMicros && p->unpricedRequests == 1, "cost overflow wrapped or became zero");
            huge.inputMicrosPerMillion = 0;
            Require(priced.SetPriceRule(huge, error), error.c_str());
            s = priced.ReadSnapshot();
            p = Find(s, "Codex", "huge-model");
            Require(p && p->costUsdMicros == 0 && p->unpricedRequests == 0,
                    "explicit zero price confused with missing price");
        }
        {
            Store priced;
            Require(priced.Open((root / L"prices.sqlite3").wstring(), error), error.c_str());
            Write(priceClaude / L"prices.jsonl", pricedMessage("priced-C", 10), true);
            auto r = priced.ImportFolder(priceClaude.wstring(), "claude", cancelled);
            Require(r.added == 1 && Find(priced.ReadSnapshot(), "Claude Code", "priced-model")->costUsdMicros == 3220,
                    "latest price version not durable");
        }
        // Only remove this process-created directory; never operate on the temp root itself.
        Require(fs::equivalent(root.parent_path(), fs::path(temp)) &&
                    root.filename().wstring().starts_with(L"TokenScope-core-tests-"),
                "cleanup path escaped test directory");
        fs::remove_all(root);
        std::cout
            << "PASS: cumulative deltas, reset, model switch, replay, cache subsets, UTF-8 paths, zero/unknown, CSV, bad/partial lines, durable checkpoints, fixed-point costs, versioned pricing, partial estimates, streaming repricing, overflow\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\nFixtures retained at " << WideToUtf8(root.wstring()) << "\n";
        return 1;
    }
}
