#include "adapters/extended.h"
#include <iostream>
#include <stdexcept>

using namespace tokenscope;
namespace {
void Require(bool good, const char* text) {
    if (!good) throw std::runtime_error(text);
}
std::vector<ParsedUsage> Parse(const std::string& source, const std::string& json, int64_t& invalid,
                               const std::string& path = "C:/agent/tasks/persisted-task/ui_messages.json") {
    auto document = yyjson_read(json.data(), json.size(), 0);
    Require(document, "fixture JSON failed");
    std::vector<ParsedUsage> result;
    const bool recognized = ParseExtendedFamily(
        source, yyjson_doc_get_root(document), path, [&](const ParsedUsage& usage) { result.push_back(usage); },
        invalid);
    yyjson_doc_free(document);
    Require(recognized, "assigned family not recognized");
    return result;
}
const std::string openaiTask =
    R"({"type":"say","say":"api_req_started","ts":1791367200000,"modelInfo":{"modelId":"actual-openai-model","providerId":"azure"},"text":"{\"tokensIn\":100,\"tokensOut\":20,\"cacheReads\":40,\"cacheWrites\":10,\"apiProtocol\":\"azure/openai\"}"})";
const std::string anthropicTask =
    R"({"type":"say","say":"api_req_started","ts":"2026-10-07T10:00:00Z","id":"request-id","modelInfo":{"modelId":"actual-anthropic-model","providerId":"bedrock"},"text":"{\"tokensIn\":100,\"tokensOut\":20,\"cacheReads\":40,\"cacheWrites\":10,\"apiProtocol\":\"bedrock/anthropic\"}"})";
const std::string pi =
    R"({"type":"message","id":"entry-id","timestamp":"2026-10-07T10:00:00Z","message":{"role":"assistant","model":"requested-alias","responseModel":"served-model","responseId":"served-response","provider":"recorded-gateway","usage":{"input":10,"output":30,"cacheRead":80,"cacheWrite":15,"reasoning":20,"totalTokens":135},"content":"PRIVATE-BODY"}})";
} // namespace
int main() {
    try {
        int64_t invalid = 0;
        auto rows = Parse("roo-code", openaiTask, invalid);
        Require(rows.size() == 1 && invalid == 0 && rows[0].input == 100 && rows[0].output == 20 &&
                    rows[0].cachedInput == 40 && rows[0].cacheWrite == 10,
                "Roo OpenAI prompt cache counted twice");
        Require(rows[0].model == "actual-openai-model" && rows[0].provider == "azure/openai",
                "Roo per-request identity lost");
        const auto taskKey = rows[0].key;
        rows = Parse("roo-code", openaiTask, invalid, "E:/import/persisted-task/ui_messages.json");
        Require(rows.size() == 1 && rows[0].key == taskKey, "Roo copied task path changed identity");
        rows = Parse("roo-code", "[" + openaiTask + "," + openaiTask + "]", invalid);
        Require(rows.size() == 2 && rows[0].key != rows[1].key, "Roo distinct equal timestamp calls collapsed");
        rows = Parse("kilo-code", anthropicTask, invalid);
        Require(rows.size() == 1 && rows[0].input == 100 && rows[0].output == 20 &&
                    rows[0].provider == "bedrock/anthropic",
                "Kilo persisted Anthropic total cache counted twice");
        const auto requestKey = rows[0].key;
        rows = Parse("kilo-code", anthropicTask, invalid, "other-parent/renamed-task/ui_messages.json");
        Require(rows.size() == 1 && rows[0].key == requestKey, "request ID depends on imported folder");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"api_req_started","ts":"2026-10-07T10:00:00Z","text":"{\"tokensIn\":20,\"tokensOut\":5}"})",
            invalid);
        Require(rows.size() == 1 && rows[0].model.empty() && rows[0].provider.empty(),
                "Cline historical model guessed");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"api_req_started","ts":"2026-10-07T10:00:00Z","text":"{\"tokensIn\":20,\"tokensOut\":5,\"cacheReads\":10}"})",
            invalid);
        Require(rows.size() == 1 && rows[0].input == 30 && rows[0].output == 5 && rows[0].cachedInput == 10,
                "Cline persisted disjoint cache missing without protocol");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"api_req_started","ts":"2026-10-07T10:00:00Z","text":"{\"tokensIn\":-1,\"tokensOut\":5}"})",
            invalid);
        Require(rows.empty() && invalid == 1, "negative native counter accepted");
        rows = Parse(
            "cline",
            R"({"sessionId":"saved-session","messages":[{"id":"cli-request","role":"assistant","ts":1791367200000,"modelInfo":{"id":"cli-model","provider":"real-provider"},"metrics":{"inputTokens":100,"outputTokens":20,"cacheReadTokens":40,"cacheWriteTokens":10}},{"role":"user","metrics":{"inputTokens":999}}]})",
            invalid);
        Require(rows.size() == 1 && rows[0].input == 100 && rows[0].cachedInput == 40 && rows[0].model == "cli-model",
                "Cline CLI inclusive metrics wrong");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"assistant_message","ts":1791367200000,"text":"{\"tokensIn\":999,\"tokensOut\":999}"})",
            invalid);
        Require(rows.empty(), "Cline counted unrelated conversation text");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"api_req_started","ts":"2026-02-29T10:00:00Z","text":"{\"tokensIn\":1,\"tokensOut\":2}"})",
            invalid);
        Require(rows.empty() && invalid == 2, "invalid calendar date accepted");
        rows = Parse(
            "cline",
            R"({"type":"say","say":"api_req_started","ts":"2026-10-07T10:00:00","text":"{\"tokensIn\":1,\"tokensOut\":2}"})",
            invalid);
        Require(rows.empty() && invalid == 3, "missing timezone accepted");
        const std::string amp =
            R"({"id":"thread-id","created":1700000000000,"messages":[{"messageId":2,"role":"assistant","usage":{"model":"duplicate-message","inputTokens":10,"outputTokens":20}}],"usageLedger":{"events":[{"timestamp":"2026-10-07T10:00:00Z","model":"actual-ledger-model","operationType":"message","fromMessageId":1,"toMessageId":2,"tokens":{"input":10,"output":20,"cacheReadInputTokens":40,"cacheCreationInputTokens":5},"credits":100}]}})";
        rows = Parse("amp", amp, invalid);
        Require(rows.size() == 1 && rows[0].input == 55 && rows[0].output == 20 &&
                    rows[0].model == "actual-ledger-model" && rows[0].provider.empty(),
                "Amp ledger/message duplicated or provider guessed");
        const auto ampKey = rows[0].key;
        rows = Parse("amp", amp, invalid, "renamed/thread-copy.json");
        Require(rows.size() == 1 && rows[0].key == ampKey, "Amp imported copy identity changed");
        rows = Parse(
            "amp",
            R"({"id":"thread","created":1700000000000,"messages":[{"messageId":2,"role":"assistant","usage":{"model":"m","inputTokens":10,"outputTokens":20}}]})",
            invalid);
        Require(rows.empty(), "Amp message-only history invented a daily timestamp");
        rows = Parse(
            "amp",
            R"({"id":"thread","usageLedger":{"events":[{"timestamp":"2026-10-07T10:00:00Z","model":"m","credits":100}]}})",
            invalid);
        Require(rows.empty(), "Amp credits converted to tokens");
        rows = Parse("pi", pi, invalid);
        Require(rows.size() == 1 && rows[0].input == 105 && rows[0].output == 30 && rows[0].reasoning == 20 &&
                    rows[0].model == "served-model",
                "Pi cache/reasoning/model accounting wrong");
        const auto piKey = rows[0].key;
        rows = Parse("pi", pi, invalid, "forked/new-session.jsonl");
        Require(rows.size() == 1 && rows[0].key == piKey, "Pi fork copied request counted again");
        rows = Parse(
            "pi",
            R"({"type":"custom","id":"summary","timestamp":"2026-10-07T10:00:00Z","aggregateUsage":{"input":999,"output":999}})",
            invalid);
        Require(rows.empty(), "Pi parent aggregate counted");
        rows = Parse(
            "pi",
            R"({"type":"message","id":"bad-total","timestamp":"2026-10-07T10:00:00Z","message":{"role":"assistant","usage":{"input":10,"output":20,"totalTokens":999}}})",
            invalid);
        Require(rows.empty() && invalid == 4, "Pi inconsistent total accepted");
        rows = Parse(
            "pi",
            R"({"type":"message","id":"only-total","timestamp":"2026-10-07T10:00:00+08:00","message":{"role":"assistant","usage":{"totalTokens":77}}})",
            invalid);
        Require(rows.size() == 1 && rows[0].unclassified == 77 && rows[0].input == 0 && rows[0].output == 0,
                "Pi total-only split invented");
        const std::string openclaw =
            R"({"type":"message","id":"openclaw-event","message":{"role":"assistant","model":"actual-openclaw-model","provider":"real-provider","timestamp":1791367200000,"usage":{"input":10,"output":30,"cacheRead":80,"cacheWrite":15,"reasoningTokens":20,"totalTokens":135}}})";
        rows = Parse("openclaw", openclaw, invalid);
        Require(rows.size() == 1 && rows[0].input == 105 && rows[0].output == 30 && rows[0].reasoning == 20,
                "OpenClaw reasoning double counted");
        const auto clawKey = rows[0].key;
        rows = Parse("openclaw", openclaw, invalid, "migrated/sqlite-event");
        Require(rows.size() == 1 && rows[0].key == clawKey, "OpenClaw SQLite migration changed event identity");
        rows = Parse(
            "openclaw",
            R"({"type":"message","id":"mirror","message":{"role":"assistant","model":"delivery-mirror","provider":"openclaw","timestamp":1791367200000,"usage":{"input":100,"output":100}}})",
            invalid);
        Require(rows.empty(), "OpenClaw delivery artifact counted");
        rows = Parse(
            "openclaw",
            R"({"type":"message","id":"codex-mirror","message":{"role":"assistant","model":"served-codex-model","provider":"openai","timestamp":1791367200000,"idempotencyKey":"codex-app-server:thread:turn:assistant","usage":{"input":100,"output":100}}})",
            invalid);
        Require(rows.empty(), "OpenClaw incomplete Codex mirror duplicated Codex source");
        rows = Parse(
            "codebuff",
            R"([{ "variant":"ai","id":"codebuff-id","createdAt":"2026-10-07T10:00:00Z","metadata":{"model":"recorded-model","usage":{"input_tokens":10,"output_tokens":30,"cache_read_input_tokens":80,"cache_creation_input_tokens":15}}}])",
            invalid);
        Require(rows.size() == 1 && rows[0].input == 105 && rows[0].output == 30 && rows[0].model == "recorded-model",
                "Codebuff direct Anthropic usage lost");
        rows = Parse(
            "codebuff",
            R"({"role":"assistant","id":"openai-codebuff","timestamp":"2026-10-07T10:00:00Z","metadata":{"codebuff":{"usage":{"prompt_tokens":100,"completion_tokens":20,"prompt_tokens_details":{"cached_tokens":40},"model":"served-model"}}}})",
            invalid);
        Require(rows.size() == 1 && rows[0].input == 100 && rows[0].cachedInput == 40,
                "Codebuff OpenAI cached input added twice");
        rows = Parse(
            "codebuff",
            R"({"role":"assistant","id":"credits","timestamp":"2026-10-07T10:00:00Z","credits":100,"metadata":{"usage":{"credits":100}}})",
            invalid);
        Require(rows.empty(), "Codebuff credits converted to tokens");
        rows = Parse(
            "codebuff",
            R"({"role":"assistant","id":"snapshot","timestamp":"2026-10-07T10:00:00Z","metadata":{"runState":{"sessionState":{"mainAgentState":{"messageHistory":[{"role":"assistant","providerOptions":{"usage":{"input_tokens":999,"output_tokens":999}}}]}}}}})",
            invalid);
        Require(rows.empty(), "Codebuff replayed RunState history counted as fresh usage");
        rows = Parse(
            "droid",
            R"({"providerLockTimestamp":"2026-10-07T10:00:00Z","tokenUsage":{"inputTokens":999,"outputTokens":999}})",
            invalid);
        Require(rows.empty(), "Droid cumulative total invented daily spend");
        rows = Parse(
            "mux",
            R"({"lastRequest":{"timestamp":1791367200000},"byModel":{"p:m":{"input":{"tokens":999},"output":{"tokens":999}}}})",
            invalid);
        Require(rows.empty(), "Mux cumulative total attributed to last request day");
        auto document = yyjson_read("{}", 2, 0);
        Require(!ParseExtendedFamily(
                    "unrelated", yyjson_doc_get_root(document), "", [](const ParsedUsage&) {}, invalid),
                "unassigned family captured");
        yyjson_doc_free(document);
        Require(invalid == 4, "unexpected fixture validation failure");
        std::cout << "extended family tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extended family tests: " << error.what() << '\n';
        return 1;
    }
}
