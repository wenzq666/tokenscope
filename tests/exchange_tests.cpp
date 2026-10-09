#include "../src/exchange.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace tokenscope;
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Reject(const std::string& json, const char* message) {
    std::string error;
    Check(!ParseExchangeRateJson(json, error) && !error.empty(), message);
}
std::string Pair(const std::string& rate, const std::string& date = "2026-10-08") {
    return "{\"date\":\"" + date + "\",\"base\":\"USD\",\"quote\":\"CNY\",\"rate\":" + rate + "}";
}
} // namespace

int main() {
    try {
        std::string error = "old error";
        // Actual official API responses retrieved on 2026-10-09, ECB publication date 2026-10-08.
        const std::string realV2 = R"({"date":"2026-10-08","base":"USD","quote":"CNY","rate":6.7023})";
        const std::string realV1 = R"({"amount":1.0,"base":"USD","date":"2026-10-08","rates":{"CNY":6.7023}})";
        auto current = ParseExchangeRateJson(realV2, error);
        Check(current && std::abs(current->usdToCny - 6.7023) < 1e-12 &&
              current->date == "2026-10-08" && current->source == "Frankfurter / ECB" &&
              error.empty(), "actual v2 reference response failed");
        const auto legacy = ParseExchangeRateJson(realV1, error);
        Check(legacy && legacy->usdToCny == current->usdToCny &&
              legacy->date == current->date, "actual v1 cache response failed");
        Check(std::string(ExchangeRateUrl()) ==
              "https://api.frankfurter.dev/v2/providers/ecb/rate/usd/cny",
              "request URL did not select ECB USD/CNY");
        Check(std::string(ExchangeRateSource()) == current->source, "untrusted source attribution");
        Check(ParseExchangeRateJson(Pair("7"), error).has_value(), "integer rate rejected");
        Check(ParseExchangeRateJson(" \r\n" + realV2 + "\t", error).has_value(), "JSON whitespace rejected");
        Check(ParseExchangeRateJson(Pair("6.7", "2024-02-29"), error).has_value(), "leap day rejected");
        Check(ParseExchangeRateJson(Pair("6.7", "2000-02-29"), error).has_value(), "400-year leap day rejected");
        for (const char* value : {"0", "-7.2", "NaN", "Infinity", "1e999", "\"NaN\"",
                                 "\"6.7023\"", "null", "true", "[]", "{}", "0.009", "1000.01"})
            Reject(Pair(value), "invalid, nonfinite, or corrupt rate accepted");
        for (const char* date : {"", "2026-1-08", "2026-10-8", "2026-00-08", "2026-13-08",
                                "2026-10-00", "2026-04-31", "2026-02-29", "2100-02-29",
                                "0000-01-01", "1998-12-31", "2026-10-08Z", "202a-10-08"})
            Reject(Pair("6.7023", date), "invalid calendar date accepted");
        Reject("", "empty response accepted");
        Reject(std::string(64 * 1024 + 1, ' '), "oversized response accepted");
        Reject(realV2 + "{}", "trailing JSON accepted");
        Reject(realV2 + std::string(1, '\0'), "embedded NUL accepted");
        Reject("[]", "root array accepted");
        Reject("null", "root null accepted");
        Reject("{}", "missing response fields accepted");
        Reject(R"({"base":"EUR","date":"2026-10-08","quote":"CNY","rate":7.4972})", "EUR base converted as USD");
        Reject(R"({"base":"usd","date":"2026-10-08","quote":"CNY","rate":6.7})", "noncanonical base accepted");
        Reject(R"({"base":"USD","date":"2026-10-08","quote":"CNH","rate":6.7})", "offshore CNH converted as CNY");
        Reject(R"({"base":"USD","date":"2026-10-08","rate":6.7})", "missing quote accepted");
        Reject(R"({"base":"USD","date":"2026-10-08","quote":"CNY"})", "missing rate accepted");
        Reject(R"({"base":"USD\u0000EUR","date":"2026-10-08","quote":"CNY","rate":6.7})", "NUL currency suffix accepted");
        Reject(R"({"base":"USD","base":"EUR","date":"2026-10-08","quote":"CNY","rate":6.7})", "ambiguous base accepted");
        Reject(R"({"base":"USD","date":"2026-10-08","quote":"CNY","rate":6.7,"rate":7.7})", "ambiguous rate accepted");
        Reject(R"({"amount":100,"base":"USD","date":"2026-10-08","rates":{"CNY":670.23}})", "non-unit amount accepted");
        Reject(R"({"amount":0,"base":"USD","date":"2026-10-08","rates":{"CNY":6.7}})", "zero amount accepted");
        Reject(R"({"amount":"1","base":"USD","date":"2026-10-08","rates":{"CNY":6.7}})", "string amount accepted");
        Reject(R"({"base":"USD","date":"2026-10-08","rates":{"CNY":6.7}})", "missing amount accepted");
        Reject(R"({"amount":1,"base":"USD","date":"2026-10-08","rates":{"CNH":6.7}})", "legacy CNH accepted");
        Reject(R"({"amount":1,"base":"USD","date":"2026-10-08","rates":{"CNY":6.7,"CNY":7.7}})", "ambiguous legacy rate accepted");
        Reject(R"({"amount":1,"base":"USD","date":"2026-10-08","quote":"CNY","rate":6.7,"rates":{"CNY":7.7}})", "mixed response schemas accepted");
        std::cout << "exchange_tests PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "exchange_tests FAIL: " << e.what() << '\n';
        return 1;
    }
}
