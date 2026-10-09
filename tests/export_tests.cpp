#include "../src/adapters.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace tokenscope;
namespace {
void Require(bool pass, const char* reason) { if (!pass) throw std::runtime_error(reason); }
void Write(const fs::path& file, const std::string& text, bool append = false) {
    std::ofstream stream(file, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    Require(bool(stream), "Cannot create fixture"); stream << text; Require(bool(stream), "Cannot write fixture");
}
struct Result { AdapterScan scan; std::vector<ParsedUsage> rows; };
Result Read(const std::string& source, const fs::path& file) {
    Result result; std::atomic_bool cancelled = false;
    result.scan = CollectUsageExport(source, file.wstring(), cancelled, [&](const ParsedUsage& row) { result.rows.push_back(row); });
    return result;
}
std::string Normalized(const std::string& source, const std::string& counters,
                       const std::string& extra = "", const std::string& id = "stable-request") {
    return "{\"schema\":\"tokenscope.usage.v1\",\"source\":\"" + source + "\",\"id\":\"" + id +
        "\",\"timestamp\":\"2026-10-08T01:02:03+08:00\",\"usage\":" + counters + extra + "}";
}
void Valid(const Result& result, size_t rows = 1) {
    Require(result.scan.error.empty(), result.scan.error.c_str());
    Require(result.scan.invalid == 0, "Valid record marked invalid"); Require(result.rows.size() == rows, "Wrong accepted record count");
}
void Refused(const Result& result) {
    Require(result.rows.empty(), "Invalid or unsupported record was emitted");
    Require(!result.scan.error.empty(), "Unsupported import needs a useful error");
}
const char* chat = R"({"object":"chat.completion","id":"chat-1","created":1700000000,"model":"recorded-model","usage":{"prompt_tokens":100,"completion_tokens":20,"total_tokens":120,"prompt_tokens_details":{"cached_tokens":30},"completion_tokens_details":{"reasoning_tokens":4}}})";
const char* response = R"({"object":"response","id":"response-1","created_at":1700000000,"model":"response-model","usage":{"input_tokens":90,"output_tokens":10,"total_tokens":100,"input_tokens_details":{"cached_tokens":20,"cache_write_tokens":5},"output_tokens_details":{"reasoning_tokens":3}}})";

void TestAllSources(const fs::path& root) {
    const auto sources = CatalogSources(); Require(sources.size() >= 26 && FindSource(sources,"codex") && FindSource(sources,"dify"), "Original source catalog coverage regressed");
    for (const auto& source : sources) {
        const auto path = root / (source.id + ".json");
        Write(path, Normalized(source.id, R"({"input_tokens":100,"output_tokens":20,"cached_input_tokens":30,"cache_write_input_tokens":5,"reasoning_output_tokens":4,"total_tokens":123})", R"(,"model":"actual-model","provider":"actual-provider")"));
        const auto result = Read(source.id, path); Valid(result);
        const auto& row = result.rows.front();
        Require(row.key == "export:stable-request" && row.timestamp == "2026-10-08T01:02:03+08:00", "Source ID or actual timestamp changed");
        Require(row.model == "actual-model" && row.provider == "actual-provider", "Actual model/provider lost");
        Require(row.input == 100 && row.output == 20 && row.cachedInput == 30 && row.cacheWrite == 5 && row.reasoning == 4 && row.unclassified == 3, "Normalized counter semantics wrong");
        const auto replay = Read(source.id, path); Valid(replay);
        Require(replay.rows.front().key == row.key, "Replay identity changed");
    }
    const auto file = root / "total-only.json";
    Write(file, Normalized("manus", R"({"total_tokens":400})"));
    auto total = Read("manus", file); Valid(total);
    Require(total.rows[0].input == 0 && total.rows[0].output == 0 && total.rows[0].unclassified == 400, "Total-only tokens guessed into input/output");
    Require(total.rows[0].model.empty() && total.rows[0].provider.empty(), "Unknown model or provider inferred");
    Write(file, Normalized("manus", R"({"input_tokens":0,"output_tokens":0})"));
    Valid(Read("manus", file));
    Write(file, Normalized("manus", R"({"total_tokens":0})")); Valid(Read("manus", file));
    Refused(Read("not-in-catalog", file)); Refused(Read("chatgpt", file));
    Write(file, R"({"schema":"tokenscope.usage.v1","source":"dify","records":[{"id":"first","timestamp":"2026-10-08T01:00:00Z","usage":{"total_tokens":7}},{"source":"dify","id":"second","timestamp":"2026-10-08T02:00:00Z","usage":{"input_tokens":4,"output_tokens":2}}]})");
    auto document = Read("dify", file); Valid(document, 2);
    Require(document.rows[0].key == "export:first" && document.rows[1].key == "export:second", "Canonical records document identity lost");
}
void TestApiResponses(const fs::path& root) {
    const auto file = root / "api.json";
    Write(file, chat); auto result = Read("openai-agents", file); Valid(result);
    Require(result.rows[0].timestamp == "2023-11-14T22:13:20.000Z", "Chat created seconds converted incorrectly");
    Require(result.rows[0].input == 100 && result.rows[0].output == 20 && result.rows[0].cachedInput == 30 && result.rows[0].reasoning == 4, "Chat cache/reasoning counted twice");
    Require(result.rows[0].provider.empty(), "Provider guessed from model/response shape");
    Write(file, std::string(R"({"timestamp":"2026-10-08T01:02:03Z","response":)") + response + "}");
    result = Read("openai-agents", file); Valid(result);
    Require(result.rows[0].timestamp == "2023-11-14T22:13:20.000Z", "Wrapper time replaced the API's own recorded creation time");
    Write(file, std::string("{\"response\":") + response + "}"); result = Read("openai-agents", file); Valid(result);
    Require(result.rows[0].input == 90 && result.rows[0].cacheWrite == 5 && result.rows[0].unclassified == 0, "Responses API input details wrong");
    const auto originalKey = result.rows[0].key;
    Write(root / "renamed-copy.json", std::string("{\"response\":") + response + "}");
    auto copy = Read("openai-agents", root / "renamed-copy.json"); Valid(copy);
    Require(copy.rows[0].key == originalKey, "Copied response identity depends on path");
    Write(file, std::string(R"({"object":"response","id":"parent","created_at":1700000000,"usage":{"input_tokens":5,"output_tokens":3,"total_tokens":8},"response":)") + response + "}");
    result = Read("openai-agents", file); Valid(result);
    Require(result.rows[0].key == "export:parent", "Parent usage and nested response counted twice");
    Write(file, R"({"timestamp":"2026-10-08T01:02:03Z","provider":"explicit-provider","message":{"type":"message","role":"assistant","id":"anthropic-1","model":"claude-recorded","usage":{"input_tokens":5,"output_tokens":4,"cache_read_input_tokens":30,"cache_creation_input_tokens":20}}})");
    result = Read("chatgpt", file); Valid(result);
    Require(result.rows[0].input == 55 && result.rows[0].output == 4 && result.rows[0].cachedInput == 30 && result.rows[0].cacheWrite == 20, "Anthropic disjoint prompt/cache buckets not added exactly once");
    Require(result.rows[0].provider == "explicit-provider", "Wrapper provider lost");
    Write(file, R"({"type":"message","role":"assistant","id":"no-time","usage":{"input_tokens":4,"output_tokens":2}})");
    Refused(Read("chatgpt", file));
    Write(file, R"({"object":"response","id":"total-only","created_at":1700000000,"usage":{"total_tokens":17}})");
    result = Read("openai-agents", file); Valid(result); Require(result.rows[0].unclassified == 17, "API total-only usage lost");
}
void TestPlatforms(const fs::path& root) {
    const auto file = root / "platforms.json";
    Write(file, R"({"event":"message_end","message_id":"dify-1","created_at":1700000000,"metadata":{"usage":{"prompt_tokens":7,"completion_tokens":3,"total_tokens":10}}})");
    auto result = Read("dify", file); Valid(result);
    Require(result.rows[0].input == 7 && result.rows[0].output == 3, "Dify message usage wrong");
    Refused(Read("coze", file));
    Write(file, R"({"timestamp":"2026-10-08T01:02:03Z","data":{"event":"message_end","message_id":"dify-wrapped","metadata":{"usage":{"prompt_tokens":7,"completion_tokens":3}}}})");
    Valid(Read("dify", file));
    Write(file, R"({"timestamp":"2026-10-08T01:02:03Z","data":{"event":"message_end","id":"dify-internal-event","metadata":{"usage":{"prompt_tokens":7,"completion_tokens":3}}}})");
    result = Read("dify", file); Valid(result);
    Require(result.rows[0].key == "export:dify-internal-event", "Dify internal event id fallback lost");
    Write(file, R"({"event":"workflow_finished","data":{"id":"workflow-1","created_at":1700000000,"total_tokens":77,"outputs":{"response":{"object":"response","id":"must-not-read-child","created_at":1700000000,"usage":{"input_tokens":50,"output_tokens":20}}}}})");
    result = Read("dify", file); Valid(result);
    Require(result.rows[0].unclassified == 77 && result.rows[0].model.empty(), "Workflow total should keep unknown split/model");
    const std::string parent = R"({"event":"workflow_finished","task_id":"shared-task","data":{"id":"shared-workflow","created_at":1700000000,"total_tokens":77}})";
    const std::string child = R"({"event":"message_end","task_id":"shared-task","workflow_run_id":"shared-workflow","message_id":"nested-message","created_at":1700000000,"metadata":{"usage":{"prompt_tokens":50,"completion_tokens":20}}})";
    Write(file, "[" + child + "," + parent + "]"); result = Read("dify", file); Valid(result);
    Require(result.rows[0].key == "export:shared-workflow", "Dify child-first order double counted total");
    Write(file, "[" + parent + "," + child + "]"); Valid(Read("dify", file));
    Write(file, R"({"data":[{"execute_id":"coze-1","create_time":1700000000,"update_time":1700000100,"token":"0","usage":{"input_count":50,"output_count":100,"token_count":150},"output":"private text"}]})");
    result = Read("coze", file); Valid(result);
    Require(result.rows[0].input == 50 && result.rows[0].output == 100 && result.rows[0].unclassified == 0, "New Coze usage not preferred over legacy token field");
    Require(result.rows[0].timestamp == "2023-11-14T22:13:20.000Z", "Coze create_time seconds mistaken for milliseconds/update_time");
    Write(file, R"({"execute_id":"coze-legacy","create_time":1700000000,"token":"150"})");
    result = Read("coze", file); Valid(result); Require(result.rows[0].unclassified == 150, "Legacy Coze string token count lost");
}
void TestRefusals(const fs::path& root) {
    const auto file = root / "bad.json";
    for (const std::string& counters : {R"({})", R"({"input_tokens":1})", R"({"input_tokens":-1,"output_tokens":2})", R"({"total_tokens":1.5})", R"({"total_tokens":"10"})", R"({"input_tokens":3,"output_tokens":2,"total_tokens":4})", R"({"input_tokens":1,"output_tokens":2,"cached_input_tokens":2})", R"({"input_tokens":5,"output_tokens":2,"cached_input_tokens":3,"cache_write_input_tokens":3})", R"({"input_tokens":5,"output_tokens":2,"reasoning_output_tokens":3})", R"({"total_tokens":9223372036854775807})"}) {
        Write(file, Normalized("manus", counters)); const auto result = Read("manus", file);
        Refused(result); Require(result.scan.invalid == 1, "Malformed record should be one invalid record");
    }
    Write(file, R"({"schema":"tokenscope.usage.v1","source":"manus","id":"bad-date","timestamp":"2026-02-30T01:00:00Z","usage":{"total_tokens":3}})"); Refused(Read("manus", file));
    Write(file, R"({"schema":"tokenscope.usage.v1","source":"manus","id":"no-zone","timestamp":"2026-10-08T01:00:00","usage":{"total_tokens":3}})"); Refused(Read("manus", file));
    Write(file, R"({"schema":"tokenscope.usage.v1","source":"manus","timestamp":"2026-10-08T01:00:00Z","usage":{"total_tokens":3}})"); Refused(Read("manus", file));
    Write(file, R"({"schema":"tokenscope.usage.v2","source":"manus","id":"wrong-schema","timestamp":"2026-10-08T01:00:00Z","usage":{"total_tokens":3}})"); Refused(Read("manus", file));
    Write(file, R"({"id":"chat-export","timestamp":"2026-10-08T01:00:00Z","messages":[{"role":"assistant","content":"Lots of text"}],"credits":500,"tokens":1000})"); Refused(Read("chatgpt", file));
    Write(file, std::string("{\"credentials\":{\"response\":") + response + "},\"output\":{" + "\"response\":" + response + "}}"); Refused(Read("n8n", file));
    Write(file, R"({"data":{"resultData":{"runData":{"OpenAI":[{"id":"node","usage":{"total_tokens":100}}]}}}})"); Refused(Read("n8n", file));
    Write(root / "unsupported.txt", chat); Refused(Read("chatgpt", root / "unsupported.txt"));
    Refused(Read("chatgpt", root / "does-not-exist.json"));
}
void TestFilesAndCancellation(const fs::path& root) {
    const auto directory = root / "stream-folder"; fs::create_directories(directory);
    const auto file = directory / L"中文.JSONL";
    const auto good = Normalized("kimi", R"({"total_tokens":12})");
    Write(file, good.substr(0, good.size() / 2));
    auto pending = Read("kimi", file);
    Require(pending.scan.error.empty() && pending.scan.invalid == 0 && pending.rows.empty(), "A lone incomplete tail should wait quietly");
    Write(file, std::string("\xEF\xBB\xBF") + good + "\r\nmalformed-complete-line\n{\"schema\":");
    auto result = Read("kimi", directory);
    Require(result.scan.files == 1 && result.scan.invalid == 1 && result.rows.size() == 1, "BOM, malformed full line or half tail handled incorrectly");
    const auto second = Normalized("kimi", R"({"total_tokens":8})", "", "second");
    Write(file, std::string("\xEF\xBB\xBF") + good + "\n" + second.substr(0, second.size() / 2));
    result = Read("kimi", file); Valid(result);
    Write(file, second.substr(second.size() / 2) + "\n", true);
    result = Read("kimi", file); Valid(result, 2);
    Require(result.rows[1].key == "export:second" && result.rows[1].unclassified == 8, "Completed tail not read as stable request");
    Write(directory / "ignored.txt", response);
    result = Read("kimi", directory); Valid(result, 2); Require(result.scan.files == 1, "Unrelated extension was read");
    std::atomic_bool cancelled = true; size_t emitted = 0;
    auto stopped = CollectUsageExport("kimi", directory.wstring(), cancelled, [&](const ParsedUsage&) { ++emitted; });
    Require(stopped.files == 0 && stopped.error.empty() && emitted == 0, "Pre-cancelled import performed work");
    cancelled = false;
    stopped = CollectUsageExport("kimi", directory.wstring(), cancelled, [&](const ParsedUsage&) { ++emitted; cancelled = true; });
    Require(emitted == 1 && stopped.error.empty(), "Cancellation during scan did not stop emission");
    const auto large = root / "large.json";
    { std::ofstream stream(large, std::ios::binary); stream.seekp(64 * 1024 * 1024); stream.put('x'); }
    result = Read("kimi", large); Refused(result); Require(result.scan.invalid == 1, "Oversized JSON was not rejected");
}
} // namespace

int main() {
    const auto root = fs::path("build") / ("export-fixtures-" + std::to_string(GetCurrentProcessId()));
    try {
        fs::create_directories(root);
        TestAllSources(root); TestApiResponses(root); TestPlatforms(root); TestRefusals(root); TestFilesAndCancellation(root);
        std::cout << "Export tests passed: " << CatalogSources().size() << " sources, actual IDs/timestamps, cache semantics, total-only usage, parent/child deduplication, refusals, BOM, half-lines, size limits and cancellation.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Export test failed: " << error.what() << '\n'; return 1; }
}
