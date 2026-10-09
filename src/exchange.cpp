#include "exchange.h"
#include "yyjson.h"
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

namespace tokenscope {
namespace {
constexpr size_t kMaxResponseBytes = 64 * 1024;
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

std::string_view Text(yyjson_val* value) {
    if (!yyjson_is_str(value)) return {};
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

bool UniqueObject(yyjson_val* value) {
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) > 32) return false;
    std::vector<std::string_view> keys;
    size_t index, count;
    yyjson_val *key, *child;
    yyjson_obj_foreach(value, index, count, key, child) {
        const auto text = Text(key);
        for (const auto previous : keys) if (previous == text) return false;
        keys.push_back(text);
    }
    return true;
}

bool ValidDate(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i = 0; i < date.size(); ++i) {
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    }
    const int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
                     (date[2] - '0') * 10 + date[3] - '0';
    const int month = (date[5] - '0') * 10 + date[6] - '0';
    const int day = (date[8] - '0') * 10 + date[9] - '0';
    if (year < 1999 || month < 1 || month > 12 || day < 1) return false;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

std::optional<ExchangeRateQuote> Fail(std::string& error, const char* message) {
    error = message;
    return std::nullopt;
}
} // namespace

const char* ExchangeRateUrl() {
    return "https://api.frankfurter.dev/v2/providers/ecb/rate/usd/cny";
}

const char* ExchangeRateSource() {
    return "Frankfurter / ECB";
}

std::optional<ExchangeRateQuote> ParseExchangeRateJson(const std::string& json,
                                                      std::string& error) {
    error.clear();
    if (json.empty() || json.size() > kMaxResponseBytes)
        return Fail(error, "Exchange rate response is empty or exceeds 64 KiB.");
    yyjson_read_err diagnostic{};
    Document doc(yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0,
                                 nullptr, &diagnostic), yyjson_doc_free);
    if (!doc) return Fail(error, "Exchange rate response is not valid JSON.");
    auto* root = yyjson_doc_get_root(doc.get());
    if (!UniqueObject(root))
        return Fail(error, "Exchange rate response must be an object with unique keys.");
    if (Text(yyjson_obj_get(root, "base")) != "USD")
        return Fail(error, "Exchange rate base must be USD.");
    const auto date = Text(yyjson_obj_get(root, "date"));
    if (!ValidDate(date)) return Fail(error, "Exchange rate date is invalid.");

    auto* pairRate = yyjson_obj_get(root, "rate");
    auto* pairQuote = yyjson_obj_get(root, "quote");
    auto* rates = yyjson_obj_get(root, "rates");
    auto* amount = yyjson_obj_get(root, "amount");
    yyjson_val* value = nullptr;
    if (pairRate || pairQuote) {
        if (rates || amount || Text(pairQuote) != "CNY")
            return Fail(error, "Exchange rate quote must be CNY in a single-pair response.");
        value = pairRate;
    } else {
        if (!yyjson_is_num(amount) || yyjson_get_num(amount) != 1.0)
            return Fail(error, "Legacy exchange rate amount must be exactly one USD.");
        if (!UniqueObject(rates))
            return Fail(error, "Legacy exchange rate currencies are invalid or ambiguous.");
        value = yyjson_obj_get(rates, "CNY");
    }
    if (!yyjson_is_num(value))
        return Fail(error, "USD/CNY exchange rate must be a JSON number.");
    const double rate = yyjson_get_num(value);
    // Broad corruption bounds for this specific currency pair, not a guessed fallback rate.
    if (!std::isfinite(rate) || rate < 0.01 || rate > 1000.0)
        return Fail(error, "USD/CNY exchange rate is outside the accepted reference range.");
    return ExchangeRateQuote{rate, std::string(date), ExchangeRateSource()};
}

} // namespace tokenscope
