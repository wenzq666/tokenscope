#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tokenscope {

enum class PricingFeed { LiteLLM, OpenRouter, ModelsDev };
struct PricingUsage {
    // TokenScope semantics: input includes cache; output includes reasoning.
    int64_t input = 0, output = 0, cachedInput = 0, cacheWrite = 0;
    int64_t reasoning = 0, unclassified = 0;
    int64_t cacheWrite1h = 0; // Subset of cacheWrite; remainder uses the normal/5m rate.
};
struct PricingRates {
    // USD micro-dollars per million tokens. Missing is unknown; explicit zero is valid.
    std::optional<int64_t> input, output, cacheRead, cacheWrite, cacheWrite1h, reasoning;
};
struct PricingTier {
    int64_t threshold = 0;  // Select the full-request tier from inclusive input tokens.
    bool inclusive = false; // false: input > threshold; true: input >= threshold.
    PricingRates rates;
};
struct PricingQuote {
    std::string provider, model, source, sourceUrl, fetchedAt, matchedKey;
    PricingRates rates;
    std::vector<PricingTier> tiers;
    bool inferredProvider = false;
    bool normalizedModel = false;
};
struct PricingFeedSpec {
    PricingFeed feed;
    const char* name;
    const char* url;
    const wchar_t* cacheFile;
};
const std::vector<PricingFeedSpec>& PricingFeeds();

class PricingCatalog {
public:
    explicit PricingCatalog(bool bundled = true);
    // Replace this feed atomically after complete validation; a bad/empty document preserves it.
    bool LoadJson(PricingFeed feed, const std::string& json, const std::string& fetchedAt, std::string& error);
    std::optional<PricingQuote> Lookup(const std::string& model, const std::string& provider,
                                       const std::string& software = {}) const;
    size_t Size() const;

private:
    std::vector<PricingQuote> entries_;
};

std::optional<int64_t> ComputePrice(const PricingQuote& quote, const PricingUsage& usage);
// Stable fingerprint of tariff + provenance for historical price versions.
std::string PricingFingerprint(const PricingQuote& quote);
std::string SerializePricingQuote(const PricingQuote& quote);
std::optional<PricingQuote> ParsePricingQuote(const std::string& json, std::string& error);

} // namespace tokenscope
