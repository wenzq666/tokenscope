#include "app.h"
#include "i18n.h"
#include "adapters.h"
#include "adapters/extended.h"
#include "pricing.h"
#include "../vendor/sqlite3.h"
#include "../vendor/yyjson.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <limits>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace tokenscope {
namespace fs = std::filesystem;
namespace {
constexpr int64_t kMaxToken = INT64_MAX / 8;
constexpr int64_t kMaxPrice = 10000000000LL; // USD 10,000 / million tokens.
struct Tokens {
    int64_t input = 0, output = 0, cached = 0, written = 0, reasoning = 0, unclassified = 0, written1h = 0;
    bool operator==(const Tokens& b) const {
        return input == b.input && output == b.output && cached == b.cached && written == b.written &&
               reasoning == b.reasoning;
    }
};
std::string Str(yyjson_val* obj, const char* key) {
    auto v = yyjson_obj_get(obj, key);
    auto s = yyjson_get_str(v);
    return s ? std::string(s, yyjson_get_len(v)) : std::string();
}
yyjson_val* Obj(yyjson_val* obj, const char* key) {
    auto v = yyjson_obj_get(obj, key);
    return yyjson_is_obj(v) ? v : nullptr;
}
bool Number(yyjson_val* o, const char* key, int64_t& value, bool required = false) {
    auto v = yyjson_obj_get(o, key);
    if (!v || yyjson_is_null(v)) return !required;
    if (!yyjson_is_int(v)) return false;
    if (yyjson_is_uint(v) && yyjson_get_uint(v) > static_cast<uint64_t>(kMaxToken)) return false;
    value = yyjson_get_sint(v);
    return value >= 0 && value <= kMaxToken;
}
bool CodexTokens(yyjson_val* v, Tokens& t) {
    return v && Number(v, "input_tokens", t.input, true) && Number(v, "output_tokens", t.output, true) &&
           Number(v, "cached_input_tokens", t.cached) && Number(v, "cache_write_input_tokens", t.written) &&
           Number(v, "reasoning_output_tokens", t.reasoning) && t.cached <= t.input && t.written <= t.input &&
           t.reasoning <= t.output;
}
std::string TokenKey(const Tokens& t) {
    return std::to_string(t.input) + ":" + std::to_string(t.output) + ":" + std::to_string(t.cached) + ":" +
           std::to_string(t.written) + ":" + std::to_string(t.reasoning);
}
Tokens Delta(const Tokens& a, const Tokens& b) {
    return {a.input - b.input, a.output - b.output, a.cached - b.cached, a.written - b.written,
            a.reasoning - b.reasoning};
}
std::optional<int64_t> Estimate(const Tokens& t, const PriceRule& p) {
    if (t.unclassified || t.cached > t.input || t.written > t.input - t.cached) return std::nullopt;
    const std::array<int64_t, 4> tokens{{t.input - t.cached - t.written, t.output, t.cached, t.written}};
    const std::array<int64_t, 4> rates{
        {p.inputMicrosPerMillion, p.outputMicrosPerMillion, p.cachedMicrosPerMillion, p.writeMicrosPerMillion}};
    int64_t whole = 0, fraction = 0;
    for (size_t i = 0; i < tokens.size(); i++) {
        // Split before multiplication: exact fixed-point arithmetic without relying
        // on floating point, compiler-specific 128-bit types, or overflow wraparound.
        int64_t quotient = tokens[i] / 1000000, remainder = tokens[i] % 1000000;
        if (rates[i] && quotient > (INT64_MAX - whole) / rates[i]) return std::nullopt;
        whole += quotient * rates[i];
        fraction += remainder * rates[i];
    }
    int64_t extra = (fraction + 500000) / 1000000; // Round the complete event once, half up.
    if (whole > INT64_MAX - extra) return std::nullopt;
    return whole + extra;
}
bool Regressed(const Tokens& a, const Tokens& b) {
    return a.input < b.input || a.output < b.output || a.cached < b.cached || a.written < b.written ||
           a.reasoning < b.reasoning;
}
std::string DayString(const SYSTEMTIME& s) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", s.wYear, s.wMonth, s.wDay);
    return buf;
}
bool IsoUtc(const std::string& text, SYSTEMTIME& utc) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != ' ') ||
        text[13] != ':' || text[16] != ':')
        return false;
    auto num = [&](size_t n, size_t count) -> int {
        int v = 0;
        for (size_t i = n; i < n + count; i++) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9') return -1;
            v = v * 10 + text[i] - '0';
        }
        return v;
    };
    int year = num(0, 4), month = num(5, 2), day = num(8, 2), hour = num(11, 2), minute = num(14, 2),
        second = num(17, 2);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59)
        return false;
    SYSTEMTIME s{};
    s.wYear = WORD(year);
    s.wMonth = WORD(month);
    s.wDay = WORD(day);
    s.wHour = WORD(hour);
    s.wMinute = WORD(minute);
    s.wSecond = WORD(second);
    size_t p = 19;
    if (p < text.size() && text[p] == '.') {
        ++p;
        size_t start = p;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') ++p;
        if (start == p) return false;
    }
    int offset = 0;
    if (p < text.size() && (text[p] == 'Z' || text[p] == 'z')) {
        if (++p != text.size()) return false;
    } else if (p + 6 == text.size() && (text[p] == '+' || text[p] == '-') && text[p + 3] == ':') {
        int h = num(p + 1, 2), m = num(p + 4, 2);
        if (h < 0 || h > 14 || m < 0 || m > 59) return false;
        offset = (h * 60 + m) * (text[p] == '+' ? 1 : -1);
        p += 6;
    } else
        return false; // Never silently assign a timezone to an ambiguous timestamp.
    FILETIME ft{};
    if (!SystemTimeToFileTime(&s, &ft)) return false;
    SYSTEMTIME check{};
    FileTimeToSystemTime(&ft, &check);
    if (check.wDay != s.wDay || check.wMonth != s.wMonth) return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    int64_t shifted = static_cast<int64_t>(ticks.QuadPart) - static_cast<int64_t>(offset) * 600000000LL;
    if (shifted < 0) return false;
    ticks.QuadPart = static_cast<uint64_t>(shifted);
    ft = {ticks.LowPart, ticks.HighPart};
    return FileTimeToSystemTime(&ft, &utc) != FALSE;
}
std::string LocalDay(const std::string& timestamp) {
    SYSTEMTIME utc{}, local{};
    if (!IsoUtc(timestamp, utc)) return {};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    return DayString(local);
}
std::string NowUtc() {
    SYSTEMTIME s{};
    GetSystemTime(&s);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute,
                  s.wSecond);
    return b;
}
std::string Csv(const std::string& v) {
    std::string out = "\"";
    // Spreadsheet applications may execute formulas even in quoted CSV cells.
    // Prefix text identities with an apostrophe when a formula marker follows
    // optional whitespace; token counts and numeric prices are separate columns.
    const size_t first = v.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && (v[first] == '=' || v[first] == '+' || v[first] == '-' || v[first] == '@'))
        out += '\'';
    for (char c : v) {
        if (c == '\"') out += '\"';
        out += c;
    }
    return out + '\"';
}
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(p); }
    void S(int i, const std::string& s) {
        sqlite3_bind_text(p, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    }
    void N(int i, int64_t n) { sqlite3_bind_int64(p, i, n); }
    bool Row() {
        int r = sqlite3_step(p);
        if (r == SQLITE_ROW) return true;
        if (r != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
        return false;
    }
    int64_t Num(int i) { return sqlite3_column_int64(p, i); }
    std::string Text(int i) {
        auto s = sqlite3_column_text(p, i);
        return s ? reinterpret_cast<const char*>(s) : "";
    }
};
void Exec(sqlite3* db, const char* sql) {
    char* e = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::string msg = e ? e : sqlite3_errmsg(db);
        sqlite3_free(e);
        throw std::runtime_error(msg);
    }
}
// Releases up to 0.4.0 always wrote user_version=6 while still adding tables and columns, so a v6
// database can lack any of them. Databases at v6 or below (and brand new ones) run this idempotent
// probing baseline once; every later change is a numbered migration.
void MigrateToBaseline(sqlite3* db) {
    Exec(
        db,
        "CREATE TABLE IF NOT EXISTS events(event_key TEXT PRIMARY KEY,software TEXT NOT NULL,utc TEXT NOT NULL,day TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,input INTEGER NOT NULL,output INTEGER NOT NULL,cached INTEGER NOT NULL,written INTEGER NOT NULL,reasoning INTEGER NOT NULL,origin TEXT NOT NULL,cost_usd_micros INTEGER,price_version INTEGER);"
        "CREATE INDEX IF NOT EXISTS events_day ON events(day,software);"
        "CREATE TABLE IF NOT EXISTS checkpoints(path TEXT NOT NULL,source TEXT NOT NULL,offset INTEGER NOT NULL,size INTEGER NOT NULL,identity TEXT NOT NULL,prefix TEXT NOT NULL,prefix_length INTEGER NOT NULL,session TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,has_previous INTEGER NOT NULL,prev_input INTEGER NOT NULL,prev_output INTEGER NOT NULL,prev_cached INTEGER NOT NULL,prev_written INTEGER NOT NULL,prev_reasoning INTEGER NOT NULL,last_write INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(path,source));"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO meta VALUES('invalid',0);"
        "CREATE TABLE IF NOT EXISTS prices(id INTEGER PRIMARY KEY AUTOINCREMENT,software TEXT NOT NULL,model TEXT NOT NULL,provider TEXT NOT NULL,input_rate INTEGER NOT NULL,output_rate INTEGER NOT NULL,cached_rate INTEGER NOT NULL,write_rate INTEGER NOT NULL,created_utc TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS prices_identity ON prices(software,model,provider,id);");
    bool hasPriceVersion = false;
    {
        Statement columns(db, "PRAGMA table_info(events)");
        while (columns.Row())
            if (columns.Text(1) == "price_version") hasPriceVersion = true;
    }
    if (!hasPriceVersion) Exec(db, "ALTER TABLE events ADD COLUMN price_version INTEGER");
    bool hasLastWrite = false;
    {
        Statement columns(db, "PRAGMA table_info(checkpoints)");
        while (columns.Row())
            if (columns.Text(1) == "last_write") hasLastWrite = true;
    }
    if (!hasLastWrite) Exec(db, "ALTER TABLE checkpoints ADD COLUMN last_write INTEGER NOT NULL DEFAULT 0");
    bool hasUnclassified = false;
    {
        Statement columns(db, "PRAGMA table_info(events)");
        while (columns.Row())
            if (columns.Text(1) == "unclassified") hasUnclassified = true;
    }
    if (!hasUnclassified) Exec(db, "ALTER TABLE events ADD COLUMN unclassified INTEGER NOT NULL DEFAULT 0");
    for (const auto* definition : {"cache_write_1h INTEGER NOT NULL DEFAULT 0", "auto_price_version INTEGER",
                                   "price_source TEXT NOT NULL DEFAULT ''"}) {
        const std::string column = std::string(definition).substr(0, std::string(definition).find(' '));
        bool found = false;
        {
            Statement columns(db, "PRAGMA table_info(events)");
            while (columns.Row())
                if (columns.Text(1) == column) found = true;
        }
        if (!found) Exec(db, ("ALTER TABLE events ADD COLUMN " + std::string(definition)).c_str());
    }
    Exec(
        db,
        "CREATE TABLE IF NOT EXISTS adapter_checkpoints(source TEXT NOT NULL,path TEXT NOT NULL,fingerprint TEXT NOT NULL,files INTEGER NOT NULL,PRIMARY KEY(source,path));"
        "CREATE TABLE IF NOT EXISTS event_origins(event_key TEXT NOT NULL,origin TEXT NOT NULL,PRIMARY KEY(event_key,origin));"
        "CREATE TABLE IF NOT EXISTS event_scopes(event_key TEXT NOT NULL,scope TEXT NOT NULL,rollup INTEGER NOT NULL,PRIMARY KEY(event_key,scope));"
        "CREATE INDEX IF NOT EXISTS event_scope_rollup ON event_scopes(scope,rollup,event_key);"
        "CREATE TABLE IF NOT EXISTS auto_prices(id INTEGER PRIMARY KEY AUTOINCREMENT,fingerprint TEXT UNIQUE NOT NULL,quote TEXT NOT NULL);");
    // Backfill the original locations without resetting cumulative native
    // checkpoints. Additional copies are recorded when actually parsed.
    {
        Statement marker(db, "SELECT value FROM meta WHERE key='event_origins_v1'");
        if (!marker.Row()) {
            Exec(
                db,
                "BEGIN IMMEDIATE;INSERT OR IGNORE INTO event_origins(event_key,origin) SELECT event_key,origin FROM events;INSERT INTO meta(key,value) VALUES('event_origins_v1',1);COMMIT;");
        }
    }
    // Re-read Claude metadata once to backfill explicit one-hour cache writes.
    {
        Statement marker(db, "SELECT value FROM meta WHERE key='adapter_v3'");
        if (!marker.Row()) {
            Exec(db,
                 "DELETE FROM checkpoints WHERE source='claude';INSERT INTO meta(key,value) VALUES('adapter_v3',1);");
        }
    }
}

struct Migration {
    int version;
    const char* sql;
};
// Append only. Each entry upgrades user_version from version-1 to version in one transaction.
constexpr Migration kMigrations[] = {
    // Speeds up the unpriced-event scan in ReloadPricing, which runs before every collection.
    {7, "CREATE INDEX IF NOT EXISTS events_unpriced ON events(event_key) "
        "WHERE cost_usd_micros IS NULL AND price_version IS NULL AND auto_price_version IS NULL"},
};
constexpr int kBaselineSchemaVersion = 6;
constexpr int kSchemaVersion = kMigrations[std::size(kMigrations) - 1].version;

int UserVersion(sqlite3* db) {
    Statement version(db, "PRAGMA user_version");
    return version.Row() ? static_cast<int>(version.Num(0)) : 0;
}

void MigrateSchema(sqlite3* db) {
    int version = UserVersion(db);
    if (version > kSchemaVersion)
        throw std::runtime_error(Tr8("数据库由更新版本的 TokenScope 创建（架构 v") + std::to_string(version) +
                                 Tr8("，本版本支持到 v") + std::to_string(kSchemaVersion) +
                                 Tr8("）；请升级后再打开。"));
    if (version <= kBaselineSchemaVersion) {
        MigrateToBaseline(db);
        Exec(db, ("PRAGMA user_version=" + std::to_string(kBaselineSchemaVersion)).c_str());
        version = kBaselineSchemaVersion;
    }
    for (const auto& migration : kMigrations) {
        if (migration.version <= version) continue;
        if (migration.version != version + 1) throw std::runtime_error(Tr8("数据库迁移序号不连续。"));
        Exec(db, "BEGIN IMMEDIATE");
        try {
            Exec(db, migration.sql);
            Exec(db, ("PRAGMA user_version=" + std::to_string(migration.version)).c_str());
            Exec(db, "COMMIT");
        } catch (...) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
        version = migration.version;
    }
}
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
struct Checkpoint {
    int64_t offset = 0, size = 0, prefixLength = 0, lastWrite = 0;
    std::string identity, prefix, session, model, provider;
    Tokens previous{};
    bool hasPrevious = false;
};
std::string Prefix(HANDLE file, int64_t length) {
    LARGE_INTEGER zero{};
    SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
    std::array<unsigned char, 128> bytes{};
    DWORD count = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(length), &count, nullptr)) return {};
    uint64_t hash = 1469598103934665603ULL;
    for (DWORD i = 0; i < count; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return std::to_string(hash);
}
std::string Fingerprint(const fs::path& path, const std::atomic_bool& cancelled) {
    uint64_t hash = 1469598103934665603ULL;
    size_t files = 0;
    auto add = [&](const fs::path& item) {
        if (++files > 50000) throw std::runtime_error("Usage scan exceeds the 50,000-file limit.");
        std::error_code ec;
        const auto size = fs::file_size(item, ec);
        if (ec) return;
        const auto time = fs::last_write_time(item, ec);
        if (ec) return;
        const std::string text = WideToUtf8(item.wstring()) + ":" + std::to_string(size) + ":" +
                                 std::to_string(static_cast<int64_t>(time.time_since_epoch().count()));
        for (unsigned char c : text) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
    };
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        add(path);
        add(fs::path(path.wstring() + L"-wal"));
    } else if (fs::is_directory(path, ec)) {
        fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) throw std::runtime_error("Cannot enumerate usage folder.");
        while (it != end && !cancelled) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                it.disable_recursion_pending();
            else {
                if (it.depth() >= 12) it.disable_recursion_pending();
                if (it->is_regular_file(ec)) add(it->path());
                ec.clear();
            }
            it.increment(ec);
            if (ec) throw std::runtime_error("Cannot enumerate usage folder.");
        }
    }
    return std::to_string(hash);
}
bool IsUsageExport(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    Handle file;
    file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.h == INVALID_HANDLE_VALUE) return false;
    std::array<char, 65536> prefix{};
    DWORD count = 0;
    if (!ReadFile(file.h, prefix.data(), static_cast<DWORD>(prefix.size()), &count, nullptr)) return false;
    const std::string text(prefix.data(), count);
    auto document = yyjson_read(text.data(), text.size(), YYJSON_READ_STOP_WHEN_DONE);
    if (!document) return false;
    auto probe = [&](auto&& self, yyjson_val* value, int depth) -> bool {
        if (depth > 5 || !yyjson_is_obj(value)) return false;
        if (Str(value, "schema") == "tokenscope.usage.v1") return true;
        const auto type = Str(value, "type"), object = Str(value, "object");
        // App logs can embed an API-shaped message while using different
        // identity and cache conventions. Keep the native outer record in
        // control so an upgrade does not re-import it under export identities.
        if (type == "assistant" || type == "user" || type == "system" || type == "event_msg" ||
            type == "session_meta" || type == "turn_context" || type == "function_call" ||
            yyjson_obj_get(value, "providerData") || yyjson_obj_get(value, "modelID"))
            return false;
        if (Obj(value, "usage") &&
            ((object == "response" || object == "chat.completion" || object == "chat.completion.chunk") ||
             (type == "message" && Str(value, "role") == "assistant")))
            return true;
        for (const auto* wrapper : {"response", "data", "records", "responses", "message"}) {
            auto child = yyjson_obj_get(value, wrapper);
            if (yyjson_is_arr(child)) child = yyjson_arr_get_first(child);
            if (self(self, child, depth + 1)) return true;
        }
        return false;
    };
    const bool found = probe(probe, yyjson_doc_get_root(document), 0);
    yyjson_doc_free(document);
    return found;
}
} // namespace

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                nullptr, nullptr);
    if (!n) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
double EffectiveUsdToCny(const Preferences& prefs) {
    const auto valid = [](double rate) {
        return std::isfinite(rate) && rate > 0 && rate < 10000;
    };
    return valid(prefs.usdToCny) ? prefs.usdToCny : valid(prefs.automaticUsdToCny) ? prefs.automaticUsdToCny : 0;
}
std::string LocalToday() {
    SYSTEMTIME s{};
    GetLocalTime(&s);
    return DayString(s);
}
std::string ShiftDay(const std::string& day, int delta) {
    SYSTEMTIME utc{};
    if (!IsoUtc(day + "T12:00:00Z", utc)) return {};
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    ULARGE_INTEGER t{};
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    int64_t n = static_cast<int64_t>(t.QuadPart) + static_cast<int64_t>(delta) * 864000000000LL;
    if (n < 0) return {};
    t.QuadPart = static_cast<uint64_t>(n);
    ft = {t.LowPart, t.HighPart};
    if (!FileTimeToSystemTime(&ft, &utc)) return {};
    return DayString(utc);
}

struct Store::Impl {
    sqlite3* db = nullptr;
    std::vector<SourceStatus> sources;
    PricingCatalog pricing;
    fs::path dataPath;
    std::string pricingStamp;
    std::string lastRepricedStamp;
    std::map<std::string, std::optional<std::pair<int64_t, PricingQuote>>> quoteCache;
    ~Impl() {
        if (db) sqlite3_close(db);
    }
    void LoadPricing() {
        std::string stamp;
        std::atomic_bool stop = false;
        for (const auto& feed : PricingFeeds()) stamp += Fingerprint(dataPath / feed.cacheFile, stop) + ";";
        if (!pricingStamp.empty() && pricingStamp == stamp) return;
        quoteCache.clear(); // LoadJson validates each feed before replacing its previous valid entries.
        for (const auto& feed : PricingFeeds()) {
            const auto file = dataPath / feed.cacheFile;
            std::error_code ec;
            if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > 32 * 1024 * 1024 || ec) continue;
            std::ifstream input(file, std::ios::binary);
            if (!input) continue;
            const std::string body{std::istreambuf_iterator<char>(input), {}};
            std::ifstream metadata(fs::path(file.wstring() + L".date"), std::ios::binary);
            std::string date;
            std::getline(metadata, date);
            if (date.empty()) date = "本地缓存";
            std::string error;
            pricing.LoadJson(feed.feed, body, date, error);
        }
        pricingStamp = std::move(stamp);
    }
    void AutomaticPrice(const std::string& key, const std::string& software, const std::string& model,
                        const std::string& provider, const Tokens& tokens, int64_t version) {
        std::optional<PricingQuote> quote;
        int64_t id = version;
        if (version) {
            Statement old(db, "SELECT quote FROM auto_prices WHERE id=?");
            old.N(1, version);
            std::string error;
            if (old.Row()) quote = ParsePricingQuote(old.Text(0), error);
        } else {
            const std::string identity = software + '\0' + model + '\0' + provider;
            auto found = quoteCache.find(identity);
            if (found == quoteCache.end()) {
                auto matched = pricing.Lookup(model, provider, software);
                if (!matched)
                    found = quoteCache.emplace(identity, std::nullopt).first;
                else {
                    const auto fingerprint = PricingFingerprint(*matched);
                    Statement add(db, "INSERT OR IGNORE INTO auto_prices(fingerprint,quote) VALUES(?,?)");
                    add.S(1, fingerprint);
                    add.S(2, SerializePricingQuote(*matched));
                    add.Row();
                    Statement saved(db, "SELECT id FROM auto_prices WHERE fingerprint=?");
                    saved.S(1, fingerprint);
                    if (!saved.Row()) return;
                    found = quoteCache.emplace(identity, std::make_pair(saved.Num(0), *matched)).first;
                }
            }
            if (!found->second) return;
            id = found->second->first;
            quote = found->second->second;
        }
        if (!quote) return;
        const auto cost = ComputePrice(*quote, {tokens.input, tokens.output, tokens.cached, tokens.written,
                                                tokens.reasoning, tokens.unclassified, tokens.written1h});
        if (!cost && !version) return; // A matching name alone does not make a missing rate zero.
        Statement update(db,
                         "UPDATE events SET cost_usd_micros=?,auto_price_version=?,price_source=? WHERE event_key=?");
        if (cost)
            update.N(1, *cost);
        else
            sqlite3_bind_null(update.p, 1);
        update.N(2, id);
        update.S(3, quote->source + (quote->inferredProvider ? " · 首方参考" : ""));
        update.S(4, key);
        update.Row();
    }
    void PriceEvent(const std::string& key) {
        Statement event(
            db,
            "SELECT software,model,provider,input,output,cached,written,reasoning,price_version,unclassified,auto_price_version,cache_write_1h FROM events WHERE event_key=?");
        event.S(1, key);
        if (!event.Row() || event.Text(1).empty()) return;
        Tokens t{event.Num(3), event.Num(4), event.Num(5), event.Num(6), event.Num(7), event.Num(9), event.Num(11)};
        int64_t version = event.Num(8);
        if (!version && event.Num(10)) {
            AutomaticPrice(key, event.Text(0), event.Text(1), event.Text(2), t, event.Num(10));
            return;
        }
        Statement price(
            db,
            version
                ? "SELECT id,input_rate,output_rate,cached_rate,write_rate FROM prices WHERE id=?"
                : "SELECT id,input_rate,output_rate,cached_rate,write_rate FROM prices WHERE software=? AND model=? AND provider=? ORDER BY id DESC LIMIT 1");
        if (version)
            price.N(1, version);
        else {
            price.S(1, event.Text(0));
            price.S(2, event.Text(1));
            price.S(3, event.Text(2));
        }
        if (!price.Row()) {
            AutomaticPrice(key, event.Text(0), event.Text(1), event.Text(2), t, 0);
            return;
        }
        PriceRule rule;
        rule.inputMicrosPerMillion = price.Num(1);
        rule.outputMicrosPerMillion = price.Num(2);
        rule.cachedMicrosPerMillion = price.Num(3);
        rule.writeMicrosPerMillion = price.Num(4);
        const auto cost = Estimate(t, rule);
        Statement update(
            db, "UPDATE events SET cost_usd_micros=?,price_version=?,price_source='手动费率' WHERE event_key=?");
        if (cost)
            update.N(1, *cost);
        else
            sqlite3_bind_null(update.p, 1);
        // Bind a price version only once a trustworthy estimate is possible. If a
        // previously priced streaming record later changes, preserve its old rule.
        if (version || cost)
            update.N(2, price.Num(0));
        else
            sqlite3_bind_null(update.p, 2);
        update.S(3, key);
        update.Row();
    }
    void SaveEvent(const std::string& key, const std::string& software, const std::string& timestamp,
                   const std::string& model, const std::string& provider, const Tokens& t, const std::string& origin,
                   ScanResult& result, bool merge) {
        const std::string day = LocalDay(timestamp);
        if (day.empty()) {
            ++result.invalid;
            return;
        }
        Statement q(
            db,
            merge
                ? "INSERT INTO events(event_key,software,utc,day,model,provider,input,output,cached,written,reasoning,origin,unclassified,cache_write_1h) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(event_key) DO UPDATE SET unclassified=MAX(0,MAX(input+output+unclassified,excluded.input+excluded.output+excluded.unclassified)-MAX(input,excluded.input)-MAX(output,excluded.output)),input=MAX(input,excluded.input),output=MAX(output,excluded.output),cached=MAX(cached,excluded.cached),written=MAX(written,excluded.written),cache_write_1h=MAX(cache_write_1h,excluded.cache_write_1h),reasoning=MAX(reasoning,excluded.reasoning),model=CASE WHEN model='' THEN excluded.model ELSE model END,provider=CASE WHEN provider='' THEN excluded.provider ELSE provider END WHERE excluded.input>input OR excluded.output>output OR excluded.cached>cached OR excluded.written>written OR excluded.cache_write_1h>cache_write_1h OR excluded.reasoning>reasoning OR excluded.input+excluded.output+excluded.unclassified>input+output+unclassified OR (model='' AND excluded.model<>'') OR (provider='' AND excluded.provider<>'')"
                : "INSERT OR IGNORE INTO events(event_key,software,utc,day,model,provider,input,output,cached,written,reasoning,origin,unclassified,cache_write_1h) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        // changes() counts updates too. Check existence before upsert to report new events honestly.
        bool exists = false;
        if (merge) {
            Statement before(db, "SELECT 1 FROM events WHERE event_key=?");
            before.S(1, key);
            exists = before.Row();
        }
        q.S(1, key);
        q.S(2, software);
        q.S(3, timestamp);
        q.S(4, day);
        q.S(5, model);
        q.S(6, provider);
        q.N(7, t.input);
        q.N(8, t.output);
        q.N(9, t.cached);
        q.N(10, t.written);
        q.N(11, t.reasoning);
        q.S(12, origin);
        q.N(13, t.unclassified);
        q.N(14, t.written1h);
        q.Row();
        if (sqlite3_changes(db)) {
            if (!exists) ++result.added;
            PriceEvent(key);
        }
        // A duplicate can be observed through a newly selected log copy.
        // Keep its original audit origin and token identity, but retain this
        // additional read location so an unchanged duplicate-only source is
        // still reported as usable after its checkpoint or a restart.
        Statement observed(db, "INSERT OR IGNORE INTO event_origins(event_key,origin) VALUES(?,?)");
        observed.S(1, key);
        observed.S(2, origin);
        observed.Row();
    }
    void Line(const std::string& line, const std::string& source, const std::string& origin, Checkpoint& cp,
              ScanResult& result) {
        // Large tool outputs and conversation bodies contain no usage. This is only a
        // conservative prefilter: possible matches are still parsed and type-checked.
        // Keep malformed lines visible in diagnostics even when they have no match.
        const bool plausibleJson =
            line.size() > 4096 && (line.front() == '{' || line.front() == ' ' || line.front() == '\t');
        if (plausibleJson && source == "codex" && line.find("\"token_count\"") == std::string::npos &&
            line.find("\"turn_context\"") == std::string::npos && line.find("\"session_meta\"") == std::string::npos)
            return;
        if (plausibleJson && source == "claude" && line.find("\"assistant\"") == std::string::npos) return;
        yyjson_read_err err{};
        yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(line.data()), line.size(), 0, nullptr, &err);
        if (!doc) {
            ++result.invalid;
            return;
        }
        struct Free {
            yyjson_doc* doc;
            ~Free() { yyjson_doc_free(doc); }
        } free{doc};
        auto root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            ++result.invalid;
            return;
        }
        const auto type = Str(root, "type");
        const auto timestamp = Str(root, "timestamp");
        if (source == "codex") {
            auto payload = Obj(root, "payload");
            if (type == "session_meta") {
                auto id = Str(payload, "id");
                if (!id.empty()) cp.session = id;
                auto provider = Str(payload, "model_provider");
                if (!provider.empty()) cp.provider = provider;
                return;
            }
            if (type == "turn_context") {
                cp.model = Str(payload, "model");
                auto provider = Str(payload, "model_provider");
                if (!provider.empty()) cp.provider = provider;
                return;
            }
            if (type != "event_msg" || Str(payload, "type") != "token_count") return;
            auto info = Obj(payload, "info");
            if (!info) return; // Rate-limit-only records are not usage.
            Tokens total{}, last{}, event{};
            auto totalObj = Obj(info, "total_token_usage");
            auto lastObj = Obj(info, "last_token_usage");
            bool hasTotal = totalObj && CodexTokens(totalObj, total);
            bool hasLast = lastObj && CodexTokens(lastObj, last);
            if ((totalObj && !hasTotal) || (lastObj && !hasLast) || (!hasTotal && !hasLast)) {
                ++result.invalid;
                return;
            }
            if (LocalDay(timestamp).empty()) {
                ++result.invalid;
                return;
            }
            if (hasTotal) {
                if (cp.hasPrevious && total == cp.previous) return; // Repeated cumulative notifications.
                if (!cp.hasPrevious)
                    event = total;
                else if (Regressed(total, cp.previous))
                    event = hasLast ? last : total;
                else
                    event = Delta(total, cp.previous);
                cp.previous = total;
                cp.hasPrevious = true;
            } else
                event = last;
            if (event.cached > event.input || event.written > event.input || event.reasoning > event.output) {
                ++result.invalid;
                return;
            }
            std::string model = Str(info, "model");
            if (model.empty()) model = Str(payload, "model");
            if (model.empty()) model = cp.model;
            const std::string key = "codex|" + cp.session + "|" + timestamp + "|" + TokenKey(hasTotal ? total : last);
            SaveEvent(key, "Codex", timestamp, model, cp.provider, event, origin, result, false);
        } else if (source == "claude") {
            if (type != "assistant") return;
            auto message = Obj(root, "message");
            auto usage = Obj(message, "usage");
            if (!usage) return;
            auto model = Str(message, "model");
            if (model == "<synthetic>") return;
            Tokens t{};
            int64_t base = 0;
            if (!Number(usage, "input_tokens", base, true) || !Number(usage, "output_tokens", t.output, true) ||
                !Number(usage, "cache_read_input_tokens", t.cached) ||
                !Number(usage, "cache_creation_input_tokens", t.written) ||
                !Number(usage, "reasoning_output_tokens", t.reasoning) || t.reasoning > t.output) {
                ++result.invalid;
                return;
            }
            if (!Number(Obj(usage, "cache_creation"), "ephemeral_1h_input_tokens", t.written1h) ||
                t.written1h > t.written) {
                ++result.invalid;
                return;
            }
            t.input = base + t.cached + t.written; // Claude input_tokens excludes cache reads and cache writes.
            auto id = Str(message, "id");
            if (id.empty()) id = Str(root, "requestId");
            if (id.empty()) id = Str(root, "uuid");
            if (id.empty()) {
                ++result.invalid;
                return;
            }
            auto provider = Str(message, "provider");
            if (provider.empty()) provider = Str(root, "provider");
            SaveEvent("claude|" + id, "Claude Code", timestamp, model, provider, t, origin, result, true);
        }
    }
    Checkpoint Load(const std::string& path, const std::string& source) {
        Checkpoint cp;
        Statement s(
            db,
            "SELECT offset,size,identity,prefix,prefix_length,session,model,provider,has_previous,prev_input,prev_output,prev_cached,prev_written,prev_reasoning,last_write FROM checkpoints WHERE path=? AND source=?");
        s.S(1, path);
        s.S(2, source);
        if (s.Row()) {
            cp.offset = s.Num(0);
            cp.size = s.Num(1);
            cp.identity = s.Text(2);
            cp.prefix = s.Text(3);
            cp.prefixLength = s.Num(4);
            cp.session = s.Text(5);
            cp.model = s.Text(6);
            cp.provider = s.Text(7);
            cp.hasPrevious = s.Num(8) != 0;
            cp.previous = {s.Num(9), s.Num(10), s.Num(11), s.Num(12), s.Num(13)};
            cp.lastWrite = s.Num(14);
        }
        return cp;
    }
    void Save(const std::string& path, const std::string& source, const Checkpoint& cp) {
        Statement s(
            db,
            "INSERT OR REPLACE INTO checkpoints(path,source,offset,size,identity,prefix,prefix_length,session,model,provider,has_previous,prev_input,prev_output,prev_cached,prev_written,prev_reasoning,last_write) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        s.S(1, path);
        s.S(2, source);
        s.N(3, cp.offset);
        s.N(4, cp.size);
        s.S(5, cp.identity);
        s.S(6, cp.prefix);
        s.N(7, cp.prefixLength);
        s.S(8, cp.session);
        s.S(9, cp.model);
        s.S(10, cp.provider);
        s.N(11, cp.hasPrevious);
        s.N(12, cp.previous.input);
        s.N(13, cp.previous.output);
        s.N(14, cp.previous.cached);
        s.N(15, cp.previous.written);
        s.N(16, cp.previous.reasoning);
        s.N(17, cp.lastWrite);
        s.Row();
    }
    void File(const fs::path& path, const std::string& source, const std::atomic_bool& cancelled, ScanResult& result) {
        Handle file;
        file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) {
            if (result.error.empty()) result.error = Tr8("部分日志正在独占写入或无读取权限，将在下次重试。");
            return;
        }
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!GetFileInformationByHandle(file.h, &fi)) return;
        int64_t size = (static_cast<int64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
        int64_t lastWrite =
            (static_cast<int64_t>(fi.ftLastWriteTime.dwHighDateTime) << 32) | fi.ftLastWriteTime.dwLowDateTime;
        std::string identity = std::to_string(fi.dwVolumeSerialNumber) + ":" + std::to_string(fi.nFileIndexHigh) + ":" +
                               std::to_string(fi.nFileIndexLow) + ":" +
                               std::to_string(fi.ftCreationTime.dwHighDateTime) + ":" +
                               std::to_string(fi.ftCreationTime.dwLowDateTime);
        std::string origin = WideToUtf8(path.lexically_normal().wstring());
        Checkpoint cp = Load(origin, source);
        const bool changed = cp.identity != identity || size < cp.offset || size < cp.size ||
                             (size == cp.size && cp.lastWrite != lastWrite) ||
                             (cp.prefixLength && Prefix(file.h, cp.prefixLength) != cp.prefix);
        if (!changed && cp.offset == size && cp.size == size) return; // No DB writes for unchanged completed files.
        if (changed) {
            cp = Checkpoint{};
        }
        cp.identity = identity;
        cp.size = size;
        cp.lastWrite = lastWrite;
        if (!cp.prefixLength) {
            cp.prefixLength = std::min<int64_t>(size, 128);
            cp.prefix = Prefix(file.h, cp.prefixLength);
        }
        LARGE_INTEGER start{};
        start.QuadPart = cp.offset;
        if (!SetFilePointerEx(file.h, start, nullptr, FILE_BEGIN)) return;
        std::array<char, 65536> buffer{};
        std::string line;
        int64_t cursor = cp.offset;
        bool oversized = false;
        const auto initialAdded = result.added, initialInvalid = result.invalid;
        Exec(db, "BEGIN IMMEDIATE");
        try {
            DWORD count = 0;
            while (!cancelled.load() && cursor < size) {
                const DWORD wanted = static_cast<DWORD>(std::min<int64_t>(buffer.size(), size - cursor));
                if (!ReadFile(file.h, buffer.data(), wanted, &count, nullptr)) {
                    if (result.error.empty()) result.error = Tr8("部分日志读取中断，已保留完成行，将在下次重试。");
                    break;
                }
                if (!count) break;
                for (DWORD i = 0; i < count; i++) {
                    ++cursor;
                    char c = buffer[i];
                    if (c == '\n') {
                        if (oversized)
                            ++result.invalid;
                        else {
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (cp.offset == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                                static_cast<unsigned char>(line[1]) == 0xBB &&
                                static_cast<unsigned char>(line[2]) == 0xBF)
                                line.erase(0, 3);
                            if (!line.empty()) Line(line, source, origin, cp, result);
                        }
                        line.clear();
                        oversized = false;
                        cp.offset = cursor;
                    } else if (!oversized) {
                        if (line.size() >= 16 * 1024 * 1024) {
                            line.clear();
                            oversized = true;
                        } else
                            line += c;
                    }
                }
            }
            // Deliberately do not consume a final line without LF: it may still be being written.
            Save(origin, source, cp);
            Statement invalid(db, "UPDATE meta SET value=value+? WHERE key='invalid'");
            invalid.N(1, result.invalid - initialInvalid);
            invalid.Row();
            Exec(db, "COMMIT");
        } catch (...) {
            Exec(db, "ROLLBACK");
            quoteCache.clear();
            result.added = initialAdded;
            result.invalid = initialInvalid;
            throw;
        }
    }
    ScanResult Folder(const std::wstring& folder, const std::string& source, const std::atomic_bool& cancelled) {
        ScanResult out;
        if (source != "codex" && source != "claude") {
            out.error = Tr8("此来源尚无经过验证的采集适配器。");
            return out;
        }
        const auto catalog = CatalogSources();
        const auto* spec = FindSource(catalog, source);
        std::error_code ec;
        if (fs::is_regular_file(folder, ec)) {
            if (cancelled) return out;
            ++out.files;
            File(folder, source, cancelled, out);
            return out;
        }
        if (!fs::is_directory(folder, ec)) {
            out.error = Tr8("日志目录不存在或无法读取。");
            return out;
        }
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
        int visited = 0;
        while (it != end && !cancelled.load()) {
            const auto attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }
            if (it.depth() >= 12) it.disable_recursion_pending();
            auto extension = it->path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (it->is_regular_file(ec)) {
                if (++visited > 50000) {
                    out.error = Tr8("日志目录超过 50,000 个文件，请选择具体来源目录。");
                    break;
                }
                try {
                    if ((extension == L".json" || extension == L".jsonl") && IsUsageExport(it->path())) {
                        auto imported = External(*spec, it->path().wstring(), cancelled, true);
                        out.added += imported.added;
                        out.files += imported.files;
                        out.invalid += imported.invalid;
                        if (!imported.error.empty()) out.error = imported.error;
                    } else if (extension == L".jsonl") {
                        ++out.files;
                        File(it->path(), source, cancelled, out);
                    }
                } catch (const std::exception& e) {
                    out.error = e.what();
                    break;
                }
            }
            it.increment(ec);
            if (ec) {
                if (out.error.empty()) out.error = Tr8("部分子目录无法读取。");
                ec.clear();
            }
        }
        return out;
    }
    int64_t CountSource(const SourceSpec& spec) {
        Statement count(db, "SELECT COUNT(*) FROM events WHERE software=?");
        count.S(1, spec.name);
        return count.Row() ? count.Num(0) : 0;
    }
    bool HasUsageAtPath(const SourceSpec& spec, const std::wstring& supplied) {
        auto path = fs::path(supplied).lexically_normal();
        path.make_preferred();
        const auto exact = WideToUtf8(path.wstring());
        auto prefix = exact;
        if (!prefix.empty() && prefix.back() != '\\') prefix += '\\';
        Statement found(
            db,
            "SELECT 1 FROM event_origins observed JOIN events event ON event.event_key=observed.event_key WHERE event.software=? AND (replace(observed.origin,'/','\\')=? COLLATE NOCASE OR substr(replace(observed.origin,'/','\\'),1,length(?))=? COLLATE NOCASE) LIMIT 1");
        found.S(1, spec.name);
        found.S(2, exact);
        found.S(3, prefix);
        found.S(4, prefix);
        return found.Row();
    }
    ScanResult External(const SourceSpec& spec, const std::wstring& path, const std::atomic_bool& cancelled,
                        bool canonical = false) {
        ScanResult result;
        if (cancelled) return result;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            result.error = Tr8("用量文件或目录不存在。");
            return result;
        }
        const auto fingerprint = "v3:" + Fingerprint(path, cancelled);
        if (cancelled) return result;
        const auto origin = WideToUtf8(fs::path(path).lexically_normal().wstring());
        {
            Statement old(db, "SELECT fingerprint,files FROM adapter_checkpoints WHERE source=? AND path=?");
            old.S(1, spec.id);
            old.S(2, origin);
            if (old.Row() && old.Text(0) == fingerprint) {
                result.files = old.Num(1);
                return result;
            }
        }
        int64_t emitted = 0;
        const UsageEmitter emit = [&](const ParsedUsage& usage) {
            ++emitted;
            const auto valid = [](int64_t n) {
                return n >= 0 && n <= kMaxToken;
            };
            if (usage.key.empty() || usage.key.size() > 4096 || usage.model.size() > 1024 ||
                usage.provider.size() > 1024 || !valid(usage.input) || !valid(usage.output) ||
                !valid(usage.unclassified) || !valid(usage.cachedInput) || !valid(usage.cacheWrite) ||
                !valid(usage.reasoning) || usage.cachedInput > usage.input || !valid(usage.cacheWrite1h) ||
                usage.cacheWrite1h > usage.cacheWrite || usage.cacheWrite > usage.input - usage.cachedInput ||
                usage.reasoning > usage.output) {
                ++result.invalid;
                return;
            }
            std::string key = usage.key.starts_with(spec.id + "|") ? usage.key : spec.id + "|" + usage.key;
            SaveEvent(key, spec.name, usage.timestamp, usage.model, usage.provider,
                      {usage.input, usage.output, usage.cachedInput, usage.cacheWrite, usage.reasoning,
                       usage.unclassified, usage.cacheWrite1h},
                      origin, result, usage.merge);
            if (!usage.rollupScopes.empty() && !LocalDay(usage.timestamp).empty()) {
                for (const auto& scope : usage.rollupScopes) {
                    if (scope.empty() || scope.size() > 4096) continue;
                    Statement group(db, "INSERT OR IGNORE INTO event_scopes(event_key,scope,rollup) VALUES(?,?,?)");
                    group.S(1, key);
                    group.S(2, scope);
                    group.N(3, usage.rollup ? 1 : 0);
                    group.Row();
                }
            }
        };
        Exec(db, "BEGIN IMMEDIATE");
        try {
            AdapterScan scan;
            if (canonical || spec.format == SourceFormat::UsageExport)
                scan = CollectUsageExport(spec.id, path, cancelled, emit);
            else if (spec.format == SourceFormat::Domestic)
                scan = CollectDomestic(spec.id, path, cancelled, emit);
            else if (spec.format == SourceFormat::Cli)
                scan = CollectCli(spec.id, path, cancelled, emit);
            else if (spec.format == SourceFormat::Extended)
                scan = CollectExtended(spec.id, path, cancelled, emit);
            if (!emitted && !cancelled && !canonical && spec.format != SourceFormat::UsageExport) {
                auto fallback = CollectUsageExport(spec.id, path, cancelled, emit);
                if (emitted) scan = std::move(fallback);
            } else if (emitted && !cancelled && !canonical && fs::is_directory(path, ec)) {
                // A custom folder may contain native sessions and normalized
                // exports. Import the latter explicitly instead of dropping them
                // merely because some native records were already accepted.
                fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                while (it != end && !cancelled) {
                    const auto attributes = GetFileAttributesW(it->path().c_str());
                    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        it.disable_recursion_pending();
                        it.increment(ec);
                        continue;
                    }
                    if (it.depth() >= 12) it.disable_recursion_pending();
                    if (it->is_regular_file(ec) && IsUsageExport(it->path())) {
                        auto extra = CollectUsageExport(spec.id, it->path().wstring(), cancelled, emit);
                        scan.invalid += extra.invalid;
                        if (!extra.error.empty()) scan.error = extra.error;
                    }
                    it.increment(ec);
                    if (ec) ec.clear();
                }
            }
            result.files = scan.files;
            result.invalid += scan.invalid;
            result.error = scan.error;
            if (cancelled) {
                Exec(db, "ROLLBACK");
                quoteCache.clear();
                result.added = 0;
                result.invalid = 0;
                return result;
            }
            if (scan.error.empty()) {
                Statement checkpoint(
                    db, "INSERT OR REPLACE INTO adapter_checkpoints(source,path,fingerprint,files) VALUES(?,?,?,?)");
                checkpoint.S(1, spec.id);
                checkpoint.S(2, origin);
                checkpoint.S(3, fingerprint);
                checkpoint.N(4, result.files);
                checkpoint.Row();
            }
            Statement bad(db, "UPDATE meta SET value=value+? WHERE key='invalid'");
            bad.N(1, result.invalid);
            bad.Row();
            Exec(db, "COMMIT");
        } catch (...) {
            Exec(db, "ROLLBACK");
            quoteCache.clear();
            throw;
        }
        return result;
    }
    ScanResult Import(const SourceSpec& spec, const std::wstring& path, const std::atomic_bool& cancelled) {
        if (spec.format == SourceFormat::Codex || spec.format == SourceFormat::Claude) {
            std::error_code ec;
            auto extension = fs::path(path).extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (IsUsageExport(path) || (extension == L".json" && fs::is_regular_file(path, ec)))
                return External(spec, path, cancelled, true);
            return Folder(path, spec.id, cancelled);
        }
        return External(spec, path, cancelled, IsUsageExport(path));
    }
};

Store::Store() : impl_(std::make_unique<Impl>()) {}
Store::~Store() = default;
bool Store::Open(const std::wstring& path, std::string& error) {
    try {
        if (impl_->db) {
            error = Tr8("数据库已经打开。");
            return false;
        }
        fs::path p(path);
        if (p.has_parent_path()) fs::create_directories(p.parent_path());
        if (sqlite3_open_v2(WideToUtf8(path).c_str(), &impl_->db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(impl_->db));
        sqlite3_busy_timeout(impl_->db, 3000);
        Exec(impl_->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA trusted_schema=OFF;");
        MigrateSchema(impl_->db);
        impl_->dataPath = p.parent_path();
        impl_->LoadPricing();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (impl_->db) {
            sqlite3_close(impl_->db);
            impl_->db = nullptr;
        }
        return false;
    }
}
ScanResult Store::ImportFolder(const std::wstring& folder, const std::string& source,
                               const std::atomic_bool& cancelled) {
    if (!impl_->db) return {0, 0, 0, Tr8("数据库未打开。")};
    const auto catalog = CatalogSources();
    const auto* spec = FindSource(catalog, source);
    if (!spec) return {0, 0, 0, Tr8("此来源尚无经过验证的采集适配器。")};
    try {
        auto result = impl_->Import(*spec, folder, cancelled);
        if (!cancelled) {
            SourceStatus status;
            status.id = spec->id;
            status.name = spec->name;
            status.path = folder;
            status.status = result.error.empty() ? "ready" : "error";
            status.records = impl_->CountSource(*spec);
            status.files = result.files;
            status.detail = result.error.empty() ? Tr8("已配置用量来源；保持未知模型和未定价记录") : result.error;
            std::error_code pathError;
            const bool exists = !folder.empty() && fs::exists(folder, pathError);
            status.paths.push_back({folder, exists});
            status.customPath = true;
            if (!exists) {
                status.status = "not-found";
                status.detail = Tr8("未发现自定义用量路径；已保留历史记录。");
            } else if (result.error.empty() && result.invalid) {
                status.status = "error";
                status.detail =
                    Tr8("发现 ") + std::to_string(result.invalid) + Tr8(" 条无效用量记录；有效记录与历史记录已保留。");
            } else if (result.error.empty() && !impl_->HasUsageAtPath(*spec, folder))
                status.status = "empty";
            auto previous = std::find_if(impl_->sources.begin(), impl_->sources.end(),
                                         [&](const SourceStatus& old) { return old.id == source; });
            if (previous == impl_->sources.end())
                impl_->sources.push_back(std::move(status));
            else
                *previous = std::move(status);
        }
        return result;
    } catch (const std::exception& e) {
        return {0, 0, 0, e.what()};
    }
}
ScanResult Store::ScanDefaultSources(const std::atomic_bool& cancelled) {
    return ScanSources({}, cancelled);
}
ScanResult Store::ScanSources(const std::vector<std::pair<std::string, std::wstring>>& overrides,
                              const std::atomic_bool& cancelled) {
    ScanResult result;
    if (!impl_->db) {
        result.error = Tr8("数据库未打开。");
        return result;
    }
    try {
        const auto catalog = CatalogSources();
        std::map<std::string, std::wstring> selected;
        for (const auto& [id, path] : overrides) {
            if (!FindSource(catalog, id)) {
                if (result.error.empty()) result.error = Tr8("未知用量来源：") + id;
                continue;
            }
            selected[id] = path;
        }
        // Build every candidate's current presence before doing any log I/O.
        // Cancellation must not erase the other sources' paths or old counts.
        std::vector<SourceStatus> statuses;
        statuses.reserve(catalog.size());
        for (const auto& spec : catalog) {
            SourceStatus status;
            status.id = spec.id;
            status.name = spec.name;
            status.detail = WideToUtf8(spec.note);
            const auto custom = selected.find(spec.id);
            status.customPath = custom != selected.end();
            const auto paths = status.customPath ? std::vector<std::wstring>{custom->second} : spec.paths;
            if (!paths.empty()) status.path = paths.front();
            bool available = false;
            for (const auto& path : paths) {
                std::error_code ec;
                const bool exists = !path.empty() && fs::exists(path, ec);
                status.paths.push_back({path, exists});
                available = available || exists;
            }
            status.records = impl_->CountSource(spec);
            if (paths.empty()) {
                status.status = "needs-export";
                status.detail = Tr8("未配置导出路径；") + status.detail;
            } else if (!available) {
                status.status = "not-found";
                status.detail = (status.customPath ? Tr8("未发现自定义用量路径；不会搜索默认路径；")
                                                   : Tr8("未发现本地用量路径；")) +
                                status.detail;
            } else
                status.status = "pending";
            statuses.push_back(std::move(status));
        }
        impl_->sources = std::move(statuses);
        for (size_t index = 0; index < catalog.size(); ++index) {
            const auto& spec = catalog[index];
            auto& status = impl_->sources[index];
            if (cancelled.load()) break;
            if (status.status != "pending") {
                if (status.customPath && status.status == "not-found" && result.error.empty())
                    result.error = spec.name + Tr8(": 未发现自定义用量路径。");
                continue;
            }
            // These native locations contain no established actual meter. An
            // explicit export still goes through Import just like all sources.
            if (!status.customPath && (spec.id == "qoder" || spec.id == "trae-work")) {
                status.status = "needs-export";
                continue;
            }
            int64_t invalid = 0;
            bool failed = false;
            for (const auto& candidate : status.paths) {
                if (cancelled.load()) break;
                if (!candidate.exists) continue;
                try {
                    auto r = impl_->Import(spec, candidate.path, cancelled);
                    result.added += r.added;
                    result.files += r.files;
                    result.invalid += r.invalid;
                    status.files += r.files;
                    invalid += r.invalid;
                    if (!r.error.empty()) {
                        failed = true;
                        status.detail = r.error;
                        if (result.error.empty()) result.error = spec.name + ": " + r.error;
                    }
                } catch (const std::exception& e) {
                    failed = true;
                    status.detail = e.what();
                    if (result.error.empty()) result.error = spec.name + ": " + e.what();
                }
            }
            status.records = impl_->CountSource(spec);
            if (cancelled.load()) {
                status.status = "pending";
                status.detail = Tr8("采集已取消；路径信息和历史记录已保留。");
            } else if (failed)
                status.status = "error";
            else if (invalid) {
                status.status = "error";
                status.detail =
                    Tr8("发现 ") + std::to_string(invalid) + Tr8(" 条无效用量记录；有效记录与历史记录已保留。");
            } else {
                bool currentUsage = false;
                for (const auto& candidate : status.paths)
                    if (candidate.exists && impl_->HasUsageAtPath(spec, candidate.path)) {
                        currentUsage = true;
                        break;
                    }
                status.status = currentUsage ? "ready" : "empty";
                if (!currentUsage)
                    status.detail = Tr8("当前路径暂无可识别的实际用量；已保留历史记录；") + status.detail;
            }
        }
        for (size_t index = 0; index < catalog.size(); ++index)
            impl_->sources[index].records = impl_->CountSource(catalog[index]);
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}
Snapshot Store::ReadSnapshot() {
    Snapshot snapshot;
    snapshot.sources = impl_->sources;
    snapshot.updatedAt = NowUtc();
    if (!impl_->db) {
        snapshot.error = Tr8("数据库未打开。");
        return snapshot;
    }
    try {
        snapshot.pricingStatus =
            Tr8("已加载 ") + std::to_string(impl_->pricing.Size()) + Tr8(" 条参考价目 · 手动费率优先 · 未匹配保持未知");
        Statement rows(
            impl_->db,
            "SELECT day,software,model,provider,SUM(input),SUM(output),SUM(cached),SUM(written),SUM(reasoning),COUNT(*),SUM(cost_usd_micros),SUM(CASE WHEN cost_usd_micros IS NULL THEN 1 ELSE 0 END),SUM(unclassified),SUM(cache_write_1h),GROUP_CONCAT(DISTINCT price_source),SUM(CASE WHEN auto_price_version IS NOT NULL THEN 1 ELSE 0 END) FROM events e WHERE NOT EXISTS(SELECT 1 FROM event_scopes child JOIN event_scopes parent ON parent.scope=child.scope AND parent.rollup=1 JOIN events p ON p.event_key=parent.event_key AND p.software=e.software WHERE child.event_key=e.event_key AND child.rollup=0) GROUP BY day,software,model,provider ORDER BY day,software,model,provider");
        while (rows.Row()) {
            UsageRow r;
            r.day = rows.Text(0);
            r.software = rows.Text(1);
            r.model = rows.Text(2);
            r.provider = rows.Text(3);
            r.input = rows.Num(4);
            r.output = rows.Num(5);
            r.cachedInput = rows.Num(6);
            r.cacheWrite = rows.Num(7);
            r.reasoning = rows.Num(8);
            r.requests = rows.Num(9);
            r.unpricedRequests = rows.Num(11);
            r.unclassified = rows.Num(12);
            r.cacheWrite1h = rows.Num(13);
            r.priceSources = rows.Text(14);
            r.automaticRequests = rows.Num(15);
            if (sqlite3_column_type(rows.p, 10) != SQLITE_NULL) r.costUsdMicros = rows.Num(10);
            snapshot.events += r.requests;
            snapshot.rows.push_back(std::move(r));
        }
        Statement bad(impl_->db, "SELECT value FROM meta WHERE key='invalid'");
        if (bad.Row()) snapshot.invalidRecords = bad.Num(0);
    } catch (const std::exception& e) {
        snapshot.error = e.what();
    }
    return snapshot;
}
bool Store::SetPriceRule(const PriceRule& rule, std::string& error) {
    if (!impl_->db) {
        error = Tr8("数据库未打开。");
        return false;
    }
    if (rule.software.empty() || rule.model.empty() || rule.software.size() > 256 || rule.model.size() > 512 ||
        rule.provider.size() > 256) {
        error = Tr8("必须为真实软件与已识别模型设置价格，未知模型不可定价。");
        return false;
    }
    const std::array<int64_t, 4> rates{{rule.inputMicrosPerMillion, rule.outputMicrosPerMillion,
                                        rule.cachedMicrosPerMillion, rule.writeMicrosPerMillion}};
    for (auto r : rates)
        if (r < 0 || r > kMaxPrice) {
            error = Tr8("价格须在 USD 0–10,000 / 百万 Token 范围内。");
            return false;
        }
    try {
        Exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement add(
                impl_->db,
                "INSERT INTO prices(software,model,provider,input_rate,output_rate,cached_rate,write_rate,created_utc) VALUES(?,?,?,?,?,?,?,?)");
            add.S(1, rule.software);
            add.S(2, rule.model);
            add.S(3, rule.provider);
            add.N(4, rule.inputMicrosPerMillion);
            add.N(5, rule.outputMicrosPerMillion);
            add.N(6, rule.cachedMicrosPerMillion);
            add.N(7, rule.writeMicrosPerMillion);
            add.S(8, NowUtc());
            add.Row();
            std::vector<std::string> pending;
            Statement find(
                impl_->db,
                "SELECT event_key FROM events WHERE software=? AND model=? AND provider=? AND price_version IS NULL AND cost_usd_micros IS NULL");
            find.S(1, rule.software);
            find.S(2, rule.model);
            find.S(3, rule.provider);
            while (find.Row()) pending.push_back(find.Text(0));
            for (const auto& key : pending) impl_->PriceEvent(key);
            Exec(impl_->db, "COMMIT");
        } catch (...) {
            Exec(impl_->db, "ROLLBACK");
            throw;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
bool Store::ReloadPricing(std::string& error) {
    if (!impl_->db) {
        error = Tr8("数据库未打开。");
        return false;
    }
    try {
        impl_->LoadPricing();
        if (impl_->lastRepricedStamp == impl_->pricingStamp) return true;
        std::vector<std::string> pending;
        {
            Statement find(
                impl_->db,
                "SELECT event_key FROM events WHERE model<>'' AND price_version IS NULL AND auto_price_version IS NULL AND cost_usd_micros IS NULL");
            while (find.Row()) pending.push_back(find.Text(0));
        }
        Exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            for (const auto& key : pending) impl_->PriceEvent(key);
            Exec(impl_->db, "COMMIT");
            impl_->lastRepricedStamp = impl_->pricingStamp;
        } catch (...) {
            Exec(impl_->db, "ROLLBACK");
            impl_->quoteCache.clear();
            throw;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
bool Store::ExportCsv(const std::wstring& path, const Snapshot& snapshot, std::string& error) {
    Handle file;
    file.h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.h == INVALID_HANDLE_VALUE) {
        error = Tr8("无法创建导出文件。");
        return false;
    }
    // UTF-8 BOM so Excel detects the encoding, then the header in the interface language.
    std::string out =
        std::string("\xEF\xBB\xBF") +
        Tr8("日期,软件,模型,服务商,输入Token,输出Token,缓存读取Token,缓存写入Token,推理Token,未分类Token,总Token,用量记录数,预估费用USD,未定价记录数,1小时缓存写入Token,价格来源,自动估价记录数") +
        "\r\n";
    for (const auto& r : snapshot.rows) {
        out += Csv(r.day) + "," + Csv(r.software) + "," + Csv(r.model) + "," + Csv(r.provider) + "," +
               std::to_string(r.input) + "," + std::to_string(r.output) + "," + std::to_string(r.cachedInput) + "," +
               std::to_string(r.cacheWrite) + "," + std::to_string(r.reasoning) + "," + std::to_string(r.unclassified) +
               "," + std::to_string(r.total()) + "," + std::to_string(r.requests) + ",";
        if (r.costUsdMicros) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(*r.costUsdMicros) / 1000000.0);
            out += buf;
        }
        out += "," + std::to_string(r.unpricedRequests) + "," + std::to_string(r.cacheWrite1h) + "," +
               Csv(r.priceSources) + "," + std::to_string(r.automaticRequests) + "\r\n";
    }
    size_t at = 0;
    while (at < out.size()) {
        DWORD written = 0;
        DWORD count = static_cast<DWORD>(std::min<size_t>(out.size() - at, 1024 * 1024));
        if (!WriteFile(file.h, out.data() + at, count, &written, nullptr) || !written) {
            error = Tr8("写入导出文件失败。");
            return false;
        }
        at += written;
    }
    return true;
}
} // namespace tokenscope
