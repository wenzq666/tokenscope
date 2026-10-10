#include "../src/pricing.h"
#include <iostream>
#include <stdexcept>
#include <climits>
#include <fstream>
#include <iterator>

using namespace tokenscope;
namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Cost(const PricingQuote& q, const PricingUsage& u, int64_t expected, const char* label) {
    const auto c = ComputePrice(q, u);
    Check(c && *c == expected, label);
}
} // namespace
int main(int argc, char** argv) {
    try {
        std::string error;
        PricingCatalog c(false);
        Check(c.LoadJson(PricingFeed::LiteLLM, R"({
          "gpt-test":{"litellm_provider":"openai","mode":"chat","input_cost_per_token":0.000002,"output_cost_per_token":0.00001,"cache_read_input_token_cost":0.0000002},
          "azure/gpt-test":{"litellm_provider":"azure","mode":"chat","input_cost_per_token":0.000004,"output_cost_per_token":0.00002},
          "claude-test-4-6":{"litellm_provider":"anthropic","input_cost_per_token":0.000003,"output_cost_per_token":0.000015,"cache_read_input_token_cost":0.0000003,"cache_creation_input_token_cost":0.00000375,"cache_creation_input_token_cost_above_1hr":0.000006},
          "gpt-long":{"litellm_provider":"openai","input_cost_per_token":0.0000025,"output_cost_per_token":0.000015,"cache_read_input_token_cost":0.00000025,"input_cost_per_token_above_272k_tokens":0.000005,"output_cost_per_token_above_272k_tokens":0.0000225,"cache_read_input_token_cost_above_272k_tokens":0.0000005},
          "xai/grok-test":{"litellm_provider":"xai","input_cost_per_token":0.000002,"output_cost_per_token":0.000006,"cache_read_input_token_cost":0.0000005,"input_cost_per_token_above_200k_tokens":0.000004,"output_cost_per_token_above_200k_tokens":0.000012,"cache_read_input_token_cost_above_200k_tokens":0.000001},
          "github_copilot/gpt-test":{"litellm_provider":"github_copilot","input_cost_per_token":0,"output_cost_per_token":0},
          "bad-nan":{"litellm_provider":"openai","input_cost_per_token":"NaN"},
          "embed":{"litellm_provider":"openai","mode":"embedding","input_cost_per_token":0.000001}
        })",
                         "2026-10-09", error),
              error.c_str());
        Check(c.Size() == 5, "invalid/non-text/subscription rows accepted");
        auto q = c.Lookup("gpt-test", "openai");
        Check(bool(q), "direct price missing");
        Cost(*q, {1000000, 1000000, 500000, 0, 100000}, 11100000, "cache or reasoning double counted");
        Check(!c.Lookup("gpt-test", "unknown-gateway"), "explicit gateway inherited first party tariff");
        auto az = c.Lookup("gpt-test", "azure");
        Check(az && az->rates.input == 4000000, "provider prices crossed");
        auto inferred = c.Lookup("gpt-test", "");
        Check(inferred && inferred->inferredProvider, "first party reference not marked inferred");
        Check(c.Lookup("openai/gpt-test", "openai").has_value(), "known first party prefix failed");
        Check(!c.Lookup("arbitrary/gpt-test", "openai"), "unknown routing prefix was guessed");
        Check(!c.Lookup("gpt-test-xyz", "openai") && !c.Lookup("gpt-test-fast", "openai"),
              "unknown suffix was guessed");
        Check(!c.Lookup("gpt-test", "unpriced:subscription") && !c.Lookup("gpt-test", "github-copilot"),
              "subscription zero priced");
        auto claude = c.Lookup("Claude-Test-4.6(high)", "Anthropic");
        Check(claude && claude->normalizedModel, "safe Claude spelling failed");
        Cost(*claude, {1000000, 100000, 200000, 300000, 50000, 0, 100000}, 4410000, "5m/1h cache split wrong");
        Check(!ComputePrice(*claude, {100, 10, 80, 30}), "invalid cache exceeds input priced");
        Check(!ComputePrice(*claude, {100, 10, 0, 0, 11}), "invalid reasoning exceeds output priced");
        Check(!ComputePrice(*claude, {100, 10, 0, 0, 0, 10}), "total only usage priced");
        Check(!ComputePrice(*claude, {100, 10, 0, 20, 0, 0, 21}), "1h exceeds writes priced");
        Cost(*q, {1000000, 0, 0, 0}, 2000000, "empty categories require no absent rates");
        Check(!ComputePrice(*q, {1000000, 0, 0, 1}), "missing cache write rate invented");
        auto longQ = c.Lookup("gpt-long", "openai");
        Check(bool(longQ), "long prompt price missing");
        Cost(*longQ, {272000, 1000}, 695000, "272K boundary selected high");
        Cost(*longQ, {272001, 1000}, 1382505, "OpenAI >272K must charge full request");
        Cost(*longQ, {272001, 1000, 200000}, 482505, "cached prompt must participate in context selection");
        auto grok = c.Lookup("grok-test", "xai");
        Check(bool(grok), "xAI price missing");
        Cost(*grok, {199999, 1000}, 405998, "xAI below threshold wrong");
        Cost(*grok, {200000, 1000}, 812000, "xAI inclusive threshold wrong");
        Check(c.LoadJson(PricingFeed::ModelsDev, R"({
          "google":{"models":{"gemini-test":{"id":"gemini-test","cost":{"input":1,"output":2,"cache_read":0.1,"tiers":[{"input":3,"output":4,"cache_read":0.3,"tier":{"type":"context","size":200000}}]}}}},
          "minimax":{"models":{"MiniMax-M3":{"cost":{"input":0.3,"output":1.2,"cache_read":0.06,"tiers":[{"input":0.6,"output":2.4,"cache_read":0.12,"tier":{"type":"context","size":512000}}],"context_over_200k":{"input":0.6,"output":2.4}}}}},
          "alibaba":{"models":{"qwen-tier":{"cost":{"input":1,"output":2,"tiers":[{"input":2,"output":4,"tier":{"type":"context","size":32000}},{"input":3,"output":6,"tier":{"type":"context","size":256000}}]}}}},
          "zai":{"models":{"glm-test":{"cost":{"input":1,"output":2,"reasoning":4}}}},
          "kenari":{"models":{"claude-test-4-6":{"cost":{"input":0,"output":0}}}}
        })",
                         "2026-10-10", error),
              error.c_str());
        auto google = c.Lookup("gemini-test", "gemini");
        Check(bool(google), "Google provider alias failed");
        Cost(*google, {200000, 1000, 100000}, 112000, "Google base context price wrong");
        Cost(*google, {200001, 1000, 100000}, 334003, "Google must select full input/output/cache tariff");
        auto mini = c.Lookup("MiniMax-M3", "minimax");
        Check(mini && mini->tiers.size() == 1 && mini->tiers[0].threshold == 512000,
              "legacy 200K alias shadowed 512K explicit tier");
        Cost(*mini, {300000, 1000}, 91200, "MiniMax legacy 200K alias overcharged");
        auto qwen = c.Lookup("qwen-tier", "dashscope");
        Check(bool(qwen), "Alibaba provider alias failed");
        Cost(*qwen, {256001, 1000}, 774003, "arbitrary context tiers failed");
        auto glm = c.Lookup("glm-test", "z-ai");
        Check(bool(glm), "Z.AI alias failed");
        Cost(*glm, {1000000, 1000000, 0, 0, 250000}, 3500000, "explicit differential reasoning rate wrong");
        Check(c.LoadJson(PricingFeed::OpenRouter, R"({"data":[
           {"id":"openai/gpt-test","pricing":{"prompt":"0.000003","completion":"0.000012","input_cache_read":"0.0000003"}},
           {"id":"vendor/model-free:free","pricing":{"prompt":"0","completion":"0"}},
           {"id":"invalid","pricing":{"prompt":"-1","completion":"NaN"}}
        ]})",
                         "2026-10-11", error),
              error.c_str());
        auto router = c.Lookup("openai/gpt-test", "openrouter");
        Check(router && router->source == "OpenRouter" && router->rates.input == 3000000,
              "OpenRouter routing/provider isolation failed");
        Check(c.Lookup("gpt-test", "openai")->rates.input == 2000000, "OpenRouter displaced direct price");
        auto free = c.Lookup("vendor/model-free:free", "openrouter");
        Check(bool(free), "explicit free row absent");
        Cost(*free, {1000000, 1000000}, 0, "explicit free row not priced zero");
        Check(!ComputePrice(*free, {1000000, 1000000, 1000}), "free row missing cache field silently guessed");
        auto oldSize = c.Size();
        Check(!c.LoadJson(PricingFeed::LiteLLM, "{}", "date", error) && c.Size() == oldSize,
              "empty feed destroyed old cache");
        Check(!c.LoadJson(PricingFeed::ModelsDev, "bad json", "date", error) && c.Size() == oldSize,
              "invalid feed destroyed old cache");
        Check(!c.Lookup("auto", "") && !c.Lookup("default", "") && !c.Lookup("router", "openrouter"),
              "router label priced as model");
        auto fingerprint = PricingFingerprint(*q);
        Check(fingerprint == PricingFingerprint(*q), "unstable price fingerprint");
        auto change = *q;
        change.rates.input = 5000000;
        Check(fingerprint != PricingFingerprint(change), "tariff change not fingerprinted");
        change = *q;
        change.fetchedAt = "new-date";
        Check(fingerprint != PricingFingerprint(change), "provenance not fingerprinted");
        const auto serialized = SerializePricingQuote(*longQ);
        auto parsed = ParsePricingQuote(serialized, error);
        Check(parsed && PricingFingerprint(*parsed) == PricingFingerprint(*longQ),
              "historical quote did not round trip");
        Cost(*parsed, {272001, 1000, 200000}, 482505, "persisted historical tier changed");
        Check(!ParsePricingQuote("{}", error) && !ParsePricingQuote("{\"schema\":\"other\"}", error),
              "foreign quote schema accepted");
        auto tampered = serialized;
        const auto pos = tampered.find("2500000");
        Check(pos != std::string::npos, "tariff fixture absent");
        tampered.replace(pos, 7, "3500000");
        Check(!ParsePricingQuote(tampered, error), "modified tariff passed fingerprint validation");
        PricingQuote huge = *q;
        huge.rates.input = 10000000000LL;
        Check(!ComputePrice(huge, {INT64_MAX / 8, 0}), "cost overflow wrapped");
        PricingQuote round = *q;
        round.rates.input = 250000;
        Cost(round, {6, 0}, 2, "whole request half-up rounding failed");
        PricingCatalog bundled;
        Check(bundled.Size() > 1000, "bundled offline reference missing");
        Check(bundled.Lookup("gpt-5.4", "openai").has_value(), "offline GPT tariff missing");
        Check(bundled.Lookup("claude-sonnet-4-6", "anthropic").has_value(), "offline Claude tariff missing");
        Check(bundled.Lookup("gemini-2.5-pro", "google").has_value(), "offline Gemini tariff missing");
        Check(bundled.Lookup("MiniMax-M2.7", "minimax").has_value(), "offline MiniMax tariff missing");
        auto composer = bundled.Lookup("Composer 2.5 (Fast)", "", "Cursor");
        Check(composer && composer->rates.input == 3000000 && composer->rates.output == 15000000,
              "stale Composer Fast override reused");
        Check(!bundled.Lookup("composer-2.5", "cursor", "Other app"), "Cursor-specific rate crossed software");
        Check(ParsePricingQuote(SerializePricingQuote(*composer), error).has_value(),
              "Cursor quote persistence failed");
        if (argc == 4) {
            PricingCatalog online(false);
            for (size_t i = 0; i < 3; ++i) {
                std::ifstream in(argv[i + 1], std::ios::binary);
                Check(bool(in), "live feed fixture not found");
                const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                Check(online.LoadJson(static_cast<PricingFeed>(i), data, "2026-10-09", error), error.c_str());
            }
            Check(online.Size() > 5000, "full feeds were not all loaded");
            for (const auto& item :
                 std::vector<std::pair<std::string, std::string>>{{"gpt-5.4", "openai"},
                                                                  {"claude-sonnet-4-6", "anthropic"},
                                                                  {"google/gemini-2.5-pro", "openrouter"},
                                                                  {"MiniMax-M3", "minimax"}}) {
                auto rate = online.Lookup(item.first, item.second);
                Check(rate.has_value(), "actual public feed model not resolved");
                Check(ParsePricingQuote(SerializePricingQuote(*rate), error).has_value(),
                      "actual public tariff persistence failed");
            }
            std::cout << "pricing_tests: actual public feeds " << online.Size() << " tariff rows\n";
        }
        std::cout << "pricing_tests: PASS; bundled " << bundled.Size() << " provider/model tariff rows\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "pricing_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
