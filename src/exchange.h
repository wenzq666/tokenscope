#pragma once
#include <optional>
#include <string>

namespace tokenscope {

struct ExchangeRateQuote {
    double usdToCny = 0;
    std::string date; // Published reference date, YYYY-MM-DD; not the download date.
    std::string source;
};

// Public daily ECB reference rate, delivered by Frankfurter. No API key required.
const char* ExchangeRateUrl();
const char* ExchangeRateSource();

// Pure validation. Accepts the v2 single-pair response and legacy v1 unit responses.
// Invalid data returns no quote and a diagnostic; callers retain their last valid cache.
std::optional<ExchangeRateQuote> ParseExchangeRateJson(const std::string& json, std::string& error);

} // namespace tokenscope
