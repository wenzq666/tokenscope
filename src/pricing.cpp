#include "pricing.h"
#include "i18n.h"
#include "yyjson.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <string_view>

namespace tokenscope {
namespace {
constexpr int64_t kMaxRate = 10000000000LL;
constexpr size_t kMaxFeedBytes = 32 * 1024 * 1024;
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
std::string Text(yyjson_val* o, const char* key) {
    auto* v = yyjson_obj_get(o, key);
    const char* s = yyjson_get_str(v);
    return s ? std::string(s, yyjson_get_len(v)) : std::string();
}
std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}
bool Starts(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
std::string Provider(std::string s) {
    s = Lower(std::move(s));
    for (const auto& p : std::array<std::pair<std::string_view, std::string_view>, 24>{
             {{"gemini", "google"},
              {"google_ai_studio", "google"},
              {"google-generative-ai", "google"},
              {"deepseek-ai", "deepseek"},
              {"deepseek_ai", "deepseek"},
              {"moonshot", "moonshotai"},
              {"moonshot-ai", "moonshotai"},
              {"mistralai", "mistral"},
              {"x-ai", "xai"},
              {"z-ai", "zai"},
              {"z_ai", "zai"},
              {"dashscope", "alibaba"},
              {"qwen_ai_platform", "alibaba"},
              {"bedrock", "amazon-bedrock"},
              {"bedrock_converse", "amazon-bedrock"},
              {"vertex_ai", "google-vertex"},
              {"vertex_ai-anthropic", "google-vertex-anthropic"},
              {"fireworks_ai", "fireworks-ai"},
              {"together_ai", "togetherai"},
              {"novita", "novita-ai"},
              {"github_copilot", "github-copilot"},
              {"azure_ai", "azure-cognitive-services"},
              {"vercel_ai_gateway", "vercel"},
              {"minimax_m2", "minimax"}}})
        if (s == p.first) return std::string(p.second);
    return s;
}
bool BlockedProvider(const std::string& p) {
    return Starts(p, "unpriced") || p == "github-copilot" || p == "kenari" || p == "kimi-for-coding" ||
           p.find("coding-plan") != std::string::npos || p.find("token-plan") != std::string::npos ||
           p == "cline-pass" || p == "lmstudio" || p == "ollama";
}
std::string InferProvider(const std::string& m) {
    if (Starts(m, "claude-")) return "anthropic";
    if (Starts(m, "gpt-") || Starts(m, "chatgpt-") || m == "o1" || Starts(m, "o1-") || m == "o3" || Starts(m, "o3-") ||
        m == "o4" || Starts(m, "o4-"))
        return "openai";
    if (Starts(m, "gemini-")) return "google";
    if (Starts(m, "deepseek-")) return "deepseek";
    if (Starts(m, "kimi-") || Starts(m, "moonshot-")) return "moonshotai";
    if (Starts(m, "minimax-")) return "minimax";
    if (Starts(m, "qwen")) return "alibaba";
    if (Starts(m, "glm-")) return "zai";
    if (Starts(m, "grok-")) return "xai";
    if (Starts(m, "mistral-") || Starts(m, "codestral-") || Starts(m, "devstral-")) return "mistral";
    if (Starts(m, "composer-") || Starts(m, "composer ")) return "cursor";
    return {};
}
std::string ModelForProvider(std::string m, const std::string& provider) {
    m = Lower(std::move(m));
    const auto slash = m.find('/');
    if (slash != std::string::npos && Provider(m.substr(0, slash)) == provider) m.erase(0, slash + 1);
    return m;
}
std::string Normalized(std::string m) {
    // Safe spelling normalization, never strip a version, unknown gateway, fast or thinking suffix.
    const auto open = m.rfind('(');
    if (open != std::string::npos && m.back() == ')') {
        const auto suffix = m.substr(open + 1, m.size() - open - 2);
        if (suffix == "low" || suffix == "medium" || suffix == "high" || suffix == "xhigh" || suffix == "minimal" ||
            suffix == "none")
            m = Lower(m.substr(0, open));
    }
    if (Starts(m, "claude-"))
        for (size_t i = 1; i + 1 < m.size(); ++i)
            if (m[i] == '.' && std::isdigit(static_cast<unsigned char>(m[i - 1])) &&
                std::isdigit(static_cast<unsigned char>(m[i + 1])))
                m[i] = '-';
    if (Starts(m, "composer ") || Starts(m, "composer-")) {
        if (m.size() > 7 && m.substr(m.size() - 7) == " (fast)") m = m.substr(0, m.size() - 7) + "-fast";
        std::replace(m.begin(), m.end(), ' ', '-');
    }
    return m;
}
std::optional<int64_t> Rate(yyjson_val* o, const char* key, long double multiplier) {
    auto* v = yyjson_obj_get(o, key);
    if (!v || yyjson_is_null(v)) return std::nullopt;
    long double n;
    if (yyjson_is_num(v))
        n = yyjson_get_num(v);
    else if (yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        char* end = nullptr;
        n = std::strtold(s, &end);
        if (!end || end == s || *end != '\0') return std::nullopt;
    } else
        return std::nullopt;
    n *= multiplier;
    if (!std::isfinite(n) || n < 0 || n > kMaxRate) return std::nullopt;
    return static_cast<int64_t>(std::llround(n));
}
PricingRates CostRates(yyjson_val* v) {
    return {Rate(v, "input", 1000000),       Rate(v, "output", 1000000),         Rate(v, "cache_read", 1000000),
            Rate(v, "cache_write", 1000000), Rate(v, "cache_write_1h", 1000000), Rate(v, "reasoning", 1000000)};
}
bool HasRate(const PricingRates& r) {
    return r.input || r.output || r.cacheRead || r.cacheWrite || r.cacheWrite1h || r.reasoning;
}
void MergeRates(PricingRates& base, const PricingRates& newer) {
    if (newer.input) base.input = newer.input;
    if (newer.output) base.output = newer.output;
    if (newer.cacheRead) base.cacheRead = newer.cacheRead;
    if (newer.cacheWrite) base.cacheWrite = newer.cacheWrite;
    if (newer.cacheWrite1h) base.cacheWrite1h = newer.cacheWrite1h;
    if (newer.reasoning) base.reasoning = newer.reasoning;
}
bool ValidQuote(const PricingQuote& q) {
    return !q.provider.empty() && !q.model.empty() && q.model.size() <= 512 && q.provider.size() <= 128 &&
           !BlockedProvider(q.provider) && HasRate(q.rates);
}
std::string SourceName(PricingFeed feed) {
    return feed == PricingFeed::LiteLLM ? "LiteLLM" : feed == PricingFeed::OpenRouter ? "OpenRouter" : "models.dev";
}
PricingQuote NewQuote(PricingFeed feed, const std::string& at) {
    PricingQuote q;
    q.source = SourceName(feed);
    q.fetchedAt = at;
    for (const auto& s : PricingFeeds())
        if (s.feed == feed) q.sourceUrl = s.url;
    return q;
}
int RateCount(const PricingRates& r) {
    return int(bool(r.input)) + int(bool(r.output)) + int(bool(r.cacheRead)) + int(bool(r.cacheWrite)) +
           int(bool(r.cacheWrite1h));
}
void SortTiers(PricingQuote& q) {
    std::stable_sort(q.tiers.begin(), q.tiers.end(),
                     [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
}
// Plain token-count above_* prices are prompt-context tariffs for these identified first-party endpoints.
bool KnownContextProvider(const std::string& p) {
    return p == "openai" || p == "anthropic" || p == "google" || p == "xai" || p == "alibaba" || p == "minimax";
}
void AddLiteLLM(std::vector<PricingQuote>& out, yyjson_val* root, const std::string& at) {
    size_t idx, max;
    yyjson_val *key, *v;
    yyjson_obj_foreach(root, idx, max, key, v) {
        if (!yyjson_is_obj(v)) continue;
        const auto mode = Text(v, "mode");
        if (!mode.empty() && mode != "chat" && mode != "completion") continue;
        auto q = NewQuote(PricingFeed::LiteLLM, at);
        q.matchedKey = yyjson_get_str(key);
        q.provider = Provider(Text(v, "litellm_provider"));
        if (q.provider.empty()) {
            const auto slash = q.matchedKey.find('/');
            q.provider = slash == std::string::npos ? InferProvider(Lower(q.matchedKey))
                                                    : Provider(q.matchedKey.substr(0, slash));
        }
        q.model = ModelForProvider(q.matchedKey, q.provider);
        q.rates = {Rate(v, "input_cost_per_token", 1e12L),
                   Rate(v, "output_cost_per_token", 1e12L),
                   Rate(v, "cache_read_input_token_cost", 1e12L),
                   Rate(v, "cache_creation_input_token_cost", 1e12L),
                   Rate(v, "cache_creation_input_token_cost_above_1hr", 1e12L),
                   Rate(v, "reasoning_cost_per_token", 1e12L)};
        if (KnownContextProvider(q.provider))
            for (int threshold : {128000, 200000, 256000, 272000}) {
                char suffix[48];
                std::snprintf(suffix, sizeof(suffix), "_above_%dk_tokens", threshold / 1000);
                auto field = [&](const char* name) {
                    return Rate(v, (std::string(name) + suffix).c_str(), 1e12L);
                };
                PricingRates r{field("input_cost_per_token"),
                               field("output_cost_per_token"),
                               field("cache_read_input_token_cost"),
                               field("cache_creation_input_token_cost"),
                               std::nullopt,
                               std::nullopt};
                if (HasRate(r)) q.tiers.push_back({threshold, q.provider == "xai" && threshold == 200000, r});
            }
        if (ValidQuote(q)) out.push_back(std::move(q));
    }
}
void AddModelsDev(std::vector<PricingQuote>& out, yyjson_val* root, const std::string& at) {
    size_t pi, pm;
    yyjson_val *pk, *pv;
    yyjson_obj_foreach(root, pi, pm, pk, pv) {
        const std::string provider = Provider(yyjson_get_str(pk));
        auto* models = yyjson_obj_get(pv, "models");
        size_t i, n;
        yyjson_val *mk, *mv;
        yyjson_obj_foreach(models, i, n, mk, mv) {
            auto* cost = yyjson_obj_get(mv, "cost");
            if (!yyjson_is_obj(cost)) continue;
            auto q = NewQuote(PricingFeed::ModelsDev, at);
            q.provider = provider;
            const auto id = Text(mv, "id");
            q.model = ModelForProvider(id.empty() ? yyjson_get_str(mk) : id, provider);
            q.matchedKey = std::string(yyjson_get_str(pk)) + "/" + (id.empty() ? yyjson_get_str(mk) : id);
            q.rates = CostRates(cost);
            auto* tiers = yyjson_obj_get(cost, "tiers");
            size_t ti, tn;
            yyjson_val* tv;
            yyjson_arr_foreach(tiers, ti, tn, tv) {
                auto* type = yyjson_obj_get(tv, "tier");
                auto* size = yyjson_obj_get(type, "size");
                if (Text(type, "type") != "context" || !yyjson_is_int(size)) continue;
                const int64_t limit = yyjson_get_sint(size);
                if (limit <= 0 || limit > 1000000000) continue;
                auto r = CostRates(tv);
                if (HasRate(r)) q.tiers.push_back({limit, provider == "xai" && limit == 200000, r});
            }
            // New explicit context tiers are authoritative; legacy 200K alias may point at 272K/512K.
            if (q.tiers.empty()) {
                auto r = CostRates(yyjson_obj_get(cost, "context_over_200k"));
                if (HasRate(r)) q.tiers.push_back({200000, provider == "xai", r});
            }
            SortTiers(q);
            if (ValidQuote(q)) out.push_back(std::move(q));
        }
    }
}
void AddOpenRouter(std::vector<PricingQuote>& out, yyjson_val* root, const std::string& at) {
    auto* data = yyjson_obj_get(root, "data");
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(data, i, n, v) {
        auto* p = yyjson_obj_get(v, "pricing");
        if (!yyjson_is_obj(p)) continue;
        auto q = NewQuote(PricingFeed::OpenRouter, at);
        q.provider = "openrouter";
        q.model = Lower(Text(v, "id"));
        q.matchedKey = "openrouter/" + q.model;
        q.rates = {Rate(p, "prompt", 1e12L),
                   Rate(p, "completion", 1e12L),
                   Rate(p, "input_cache_read", 1e12L),
                   Rate(p, "input_cache_write", 1e12L),
                   std::nullopt,
                   Rate(p, "internal_reasoning", 1e12L)};
        if (ValidQuote(q)) out.push_back(std::move(q));
    }
}
#include "pricing-data.inc"
template <size_t N> std::string Bundled(const char* const (&parts)[N]) {
    std::string s;
    for (const auto* part : parts) s += part;
    return s;
}
std::string EscapeJson(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", c);
            result += hex;
        } else
            result += static_cast<char>(c);
    }
    return result + '"';
}
std::string RatesJson(const PricingRates& r) {
    std::string s = "{";
    size_t i = 0;
    for (const auto& p :
         std::array<std::pair<const char*, std::optional<int64_t>>, 6>{{{"input", r.input},
                                                                        {"output", r.output},
                                                                        {"cacheRead", r.cacheRead},
                                                                        {"cacheWrite", r.cacheWrite},
                                                                        {"cacheWrite1h", r.cacheWrite1h},
                                                                        {"reasoning", r.reasoning}}}) {
        if (i++) s += ',';
        s += EscapeJson(p.first) + ":" + (p.second ? std::to_string(*p.second) : "null");
    }
    return s + '}';
}
bool ReadRates(yyjson_val* v, PricingRates& r) {
    if (!yyjson_is_obj(v)) return false;
    std::array<std::pair<const char*, std::optional<int64_t>*>, 6> fields{{{"input", &r.input},
                                                                           {"output", &r.output},
                                                                           {"cacheRead", &r.cacheRead},
                                                                           {"cacheWrite", &r.cacheWrite},
                                                                           {"cacheWrite1h", &r.cacheWrite1h},
                                                                           {"reasoning", &r.reasoning}}};
    for (const auto& p : fields) {
        auto* value = yyjson_obj_get(v, p.first);
        if (!value || yyjson_is_null(value)) continue;
        if (!yyjson_is_int(value) ||
            (yyjson_is_uint(value) && yyjson_get_uint(value) > static_cast<uint64_t>(kMaxRate)))
            return false;
        const auto n = yyjson_get_sint(value);
        if (n < 0 || n > kMaxRate) return false;
        *p.second = n;
    }
    return true;
}
} // namespace

const std::vector<PricingFeedSpec>& PricingFeeds() {
    static const std::vector<PricingFeedSpec> sources = {
        {PricingFeed::LiteLLM, "LiteLLM",
         "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json",
         L"pricing-litellm.json"},
        {PricingFeed::ModelsDev, "models.dev", "https://models.dev/api.json", L"model-catalog.json"},
        {PricingFeed::OpenRouter, "OpenRouter", "https://openrouter.ai/api/v1/models", L"pricing-openrouter.json"}};
    return sources;
}
PricingCatalog::PricingCatalog(bool bundled) {
    if (bundled) {
        std::string error;
        LoadJson(PricingFeed::LiteLLM, Bundled(kBundledLiteLLM), kBundledPricingDate, error);
        LoadJson(PricingFeed::ModelsDev, Bundled(kBundledModelsDev), kBundledPricingDate, error);
        // Manually verified against Cursor's official table on 2026-10-09. A '-' cache-write quote stays unknown.
        for (const auto& v :
             std::array<std::array<int64_t, 3>, 2>{{{{500000, 2500000, 200000}}, {{3000000, 15000000, 500000}}}}) {
            PricingQuote q;
            q.provider = "cursor";
            q.model = v[0] == 500000 ? "composer-2.5" : "composer-2.5-fast";
            q.source = "Cursor";
            q.sourceUrl = "https://cursor.com/docs/models-and-pricing";
            q.fetchedAt = "2026-10-09";
            q.matchedKey = "cursor/" + q.model;
            q.rates.input = v[0];
            q.rates.output = v[1];
            q.rates.cacheRead = v[2];
            entries_.push_back(std::move(q));
        }
    }
}
bool PricingCatalog::LoadJson(PricingFeed feed, const std::string& json, const std::string& at, std::string& error) {
    error.clear();
    if (json.empty() || json.size() > kMaxFeedBytes) {
        error = Tr8("价目数据为空或超过 32 MiB。");
        return false;
    }
    yyjson_read_err e{};
    Document doc(yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &e), yyjson_doc_free);
    if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc.get()))) {
        error = Tr8("公开价目数据不是有效 JSON 对象。");
        return false;
    }
    std::vector<PricingQuote> parsed;
    auto* root = yyjson_doc_get_root(doc.get());
    switch (feed) {
    case PricingFeed::LiteLLM:
        AddLiteLLM(parsed, root, at);
        break;
    case PricingFeed::ModelsDev:
        AddModelsDev(parsed, root, at);
        break;
    case PricingFeed::OpenRouter:
        AddOpenRouter(parsed, root, at);
        break;
    }
    if (parsed.empty() || parsed.size() > 50000) {
        error = Tr8("价目数据没有可用费率，或条目过多。");
        return false;
    }
    const auto source = SourceName(feed);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& q) { return q.source == source; }),
                   entries_.end());
    entries_.insert(entries_.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    return true;
}
size_t PricingCatalog::Size() const {
    return entries_.size();
}
std::optional<PricingQuote> PricingCatalog::Lookup(const std::string& raw, const std::string& rawProvider,
                                                   const std::string& software) const {
    auto model = Lower(raw);
    auto provider = Provider(rawProvider);
    if (model.empty() || model == "unknown" || model == "auto" || model == "default" || model == "router")
        return std::nullopt;
    if (provider == "unknown" || provider == "default") provider.clear();
    if (BlockedProvider(provider)) return std::nullopt;
    bool inferred = false;
    if (provider.empty()) {
        const auto slash = model.find('/');
        if (slash != std::string::npos) {
            provider = Provider(model.substr(0, slash));
            model.erase(0, slash + 1);
        } else {
            provider = InferProvider(Normalized(model));
            inferred = true;
        }
    }
    if (provider.empty() || BlockedProvider(provider)) return std::nullopt;
    model = ModelForProvider(std::move(model), provider);
    const auto normalized = Normalized(model);
    const PricingQuote* best = nullptr;
    int score = -1;
    for (const auto& q : entries_) {
        if (q.provider != provider) continue;
        const bool exact = q.model == model;
        const bool norm = Normalized(q.model) == normalized;
        if (!exact && !norm) continue;
        int rank = (exact ? 10000 : 5000) + RateCount(q.rates) * 10 + static_cast<int>(q.tiers.size()) * 100;
        if (q.source == "LiteLLM")
            rank += 3;
        else if (q.source == "models.dev")
            rank += 2;
        // Cursor-only Composer prices never leak to another software or endpoint.
        if (provider == "cursor" && !software.empty() && Lower(software) != "cursor") continue;
        if (rank > score || (rank == score && best && q.matchedKey < best->matchedKey)) {
            best = &q;
            score = rank;
        }
    }
    if (!best) return std::nullopt;
    auto result = *best;
    result.inferredProvider = inferred;
    result.normalizedModel = best->model != model;
    return result;
}
std::optional<int64_t> ComputePrice(const PricingQuote& quote, const PricingUsage& u) {
    const std::array<int64_t, 7> values{u.input,     u.output,       u.cachedInput, u.cacheWrite,
                                        u.reasoning, u.unclassified, u.cacheWrite1h};
    for (auto n : values)
        if (n < 0 || n > INT64_MAX / 8) return std::nullopt;
    if (u.unclassified || u.cachedInput > u.input || u.cacheWrite > u.input - u.cachedInput || u.reasoning > u.output ||
        u.cacheWrite1h > u.cacheWrite)
        return std::nullopt;
    auto r = quote.rates;
    for (const auto& tier : quote.tiers)
        if (tier.inclusive ? u.input >= tier.threshold : u.input > tier.threshold) {
            MergeRates(r, tier.rates);
            if (quote.provider == "anthropic" && tier.rates.input && !tier.rates.cacheWrite1h &&
                *tier.rates.input <= kMaxRate / 2)
                r.cacheWrite1h = *tier.rates.input * 2;
        }
    if (u.cacheWrite1h && !r.cacheWrite1h && quote.provider == "anthropic" && r.input && *r.input <= kMaxRate / 2)
        r.cacheWrite1h = *r.input * 2;
    const int64_t reasoningSeparate = r.reasoning ? u.reasoning : 0;
    const std::array<int64_t, 6> buckets{u.input - u.cachedInput - u.cacheWrite,
                                         u.output - reasoningSeparate,
                                         u.cachedInput,
                                         u.cacheWrite - u.cacheWrite1h,
                                         u.cacheWrite1h,
                                         reasoningSeparate};
    const std::array<std::optional<int64_t>, 6> rates{r.input,      r.output,       r.cacheRead,
                                                      r.cacheWrite, r.cacheWrite1h, r.reasoning};
    int64_t whole = 0, fraction = 0;
    for (size_t i = 0; i < buckets.size(); ++i) {
        if (!buckets[i]) continue;
        if (!rates[i] || *rates[i] < 0 || *rates[i] > kMaxRate) return std::nullopt;
        const auto rate = *rates[i], q = buckets[i] / 1000000, rem = buckets[i] % 1000000;
        if (rate && q > (INT64_MAX - whole) / rate) return std::nullopt;
        whole += q * rate;
        const int64_t part = rem * rate;
        const int64_t extra = part / 1000000;
        if (whole > INT64_MAX - extra) return std::nullopt;
        whole += extra;
        fraction += part % 1000000;
    }
    const int64_t rounded = (fraction + 500000) / 1000000;
    if (whole > INT64_MAX - rounded) return std::nullopt;
    return whole + rounded;
}
std::string PricingFingerprint(const PricingQuote& q) {
    std::string bytes =
        q.source + '\n' + q.sourceUrl + '\n' + q.fetchedAt + '\n' + q.provider + '\n' + q.model + '\n' + q.matchedKey;
    auto append = [&](const PricingRates& r) {
        for (const auto& n : {r.input, r.output, r.cacheRead, r.cacheWrite, r.cacheWrite1h, r.reasoning})
            bytes += '|' + (n ? std::to_string(*n) : "?");
    };
    append(q.rates);
    for (const auto& t : q.tiers) {
        bytes += '|' + std::to_string(t.threshold) + (t.inclusive ? ">=" : ">");
        append(t.rates);
    }
    bytes += q.inferredProvider ? "|inferred" : "|explicit";
    bytes += q.normalizedModel ? "|normalized" : "|exact";
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char result[32];
    std::snprintf(result, sizeof(result), "auto-v1-%016llx", static_cast<unsigned long long>(hash));
    return result;
}
std::string SerializePricingQuote(const PricingQuote& q) {
    std::string s = "{\"schema\":\"tokenscope.pricing.v1\"";
    for (const auto& p : std::array<std::pair<const char*, std::string>, 7>{{{"provider", q.provider},
                                                                             {"model", q.model},
                                                                             {"source", q.source},
                                                                             {"sourceUrl", q.sourceUrl},
                                                                             {"fetchedAt", q.fetchedAt},
                                                                             {"matchedKey", q.matchedKey},
                                                                             {"fingerprint", PricingFingerprint(q)}}})
        s += ',' + EscapeJson(p.first) + ':' + EscapeJson(p.second);
    s += ",\"inferredProvider\":" + std::string(q.inferredProvider ? "true" : "false") +
         ",\"normalizedModel\":" + (q.normalizedModel ? "true" : "false") + ",\"rates\":" + RatesJson(q.rates) +
         ",\"tiers\":[";
    for (size_t i = 0; i < q.tiers.size(); ++i) {
        if (i) s += ',';
        const auto& t = q.tiers[i];
        s += "{\"threshold\":" + std::to_string(t.threshold) + ",\"inclusive\":" + (t.inclusive ? "true" : "false") +
             ",\"rates\":" + RatesJson(t.rates) + '}';
    }
    return s + "]}";
}
std::optional<PricingQuote> ParsePricingQuote(const std::string& json, std::string& error) {
    error.clear();
    auto bad = [&]() -> std::optional<PricingQuote> {
        error = Tr8("历史价目版本格式无效。");
        return std::nullopt;
    };
    if (json.empty() || json.size() > 65536) return bad();
    Document doc(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
    if (!doc) return bad();
    auto* v = yyjson_doc_get_root(doc.get());
    if (Text(v, "schema") != "tokenscope.pricing.v1") return bad();
    PricingQuote q;
    q.provider = Text(v, "provider");
    q.model = Text(v, "model");
    q.source = Text(v, "source");
    q.sourceUrl = Text(v, "sourceUrl");
    q.fetchedAt = Text(v, "fetchedAt");
    q.matchedKey = Text(v, "matchedKey");
    if (q.provider.empty() || q.provider.size() > 128 || BlockedProvider(q.provider) || q.model.empty() ||
        q.model.size() > 512)
        return bad();
    if (q.source != "LiteLLM" && q.source != "models.dev" && q.source != "OpenRouter" && q.source != "Cursor")
        return bad();
    bool urlKnown = false;
    for (const auto& feed : PricingFeeds())
        if (q.source == feed.name && q.sourceUrl == feed.url) urlKnown = true;
    if (q.source == "Cursor" && q.sourceUrl == "https://cursor.com/docs/models-and-pricing" && q.provider == "cursor")
        urlKnown = true;
    if (!urlKnown || q.fetchedAt.empty() || q.fetchedAt.size() > 64 || q.matchedKey.empty() ||
        q.matchedKey.size() > 1024)
        return bad();
    auto* inferred = yyjson_obj_get(v, "inferredProvider");
    auto* normalized = yyjson_obj_get(v, "normalizedModel");
    if (!yyjson_is_bool(inferred) || !yyjson_is_bool(normalized) || !ReadRates(yyjson_obj_get(v, "rates"), q.rates) ||
        !HasRate(q.rates))
        return bad();
    q.inferredProvider = yyjson_get_bool(inferred);
    q.normalizedModel = yyjson_get_bool(normalized);
    auto* tiers = yyjson_obj_get(v, "tiers");
    if (!yyjson_is_arr(tiers) || yyjson_arr_size(tiers) > 32) return bad();
    size_t i, n;
    yyjson_val* tv;
    int64_t previous = 0;
    yyjson_arr_foreach(tiers, i, n, tv) {
        auto* threshold = yyjson_obj_get(tv, "threshold");
        auto* inclusive = yyjson_obj_get(tv, "inclusive");
        if (!yyjson_is_int(threshold) || !yyjson_is_bool(inclusive)) return bad();
        const auto limit = yyjson_get_sint(threshold);
        PricingTier t;
        if (limit <= previous || limit > 1000000000 || !ReadRates(yyjson_obj_get(tv, "rates"), t.rates) ||
            !HasRate(t.rates))
            return bad();
        t.threshold = limit;
        t.inclusive = yyjson_get_bool(inclusive);
        q.tiers.push_back(t);
        previous = limit;
    }
    if (Text(v, "fingerprint") != PricingFingerprint(q)) return bad();
    return q;
}
} // namespace tokenscope
