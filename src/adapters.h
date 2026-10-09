#pragma once
#include "app.h"
#include "yyjson.h"
#include <functional>

namespace tokenscope {

enum class SourceFormat { Codex, Claude, Domestic, Cli, UsageExport, Extended };

struct SourceSpec {
    std::string id, name;
    SourceFormat format = SourceFormat::UsageExport;
    std::vector<std::wstring> paths; // Explicit app-owned usage locations, never whole-disk scans.
    std::wstring note;
};

struct ParsedUsage {
    std::string key; // Stable source request/message identity, independent of imported path.
    std::string timestamp; // ISO8601 with explicit UTC or offset.
    std::string model, provider;
    int64_t input = 0, output = 0, cachedInput = 0, cacheWrite = 0, reasoning = 0, unclassified = 0;
    bool merge = true; // Latest/final snapshot of the same request, never sum stream fragments.
    std::vector<std::string> rollupScopes; // Explicit workflow/task identities, never inferred from time.
    bool rollup = false; // A parent total supersedes children sharing one of these identities.
    int64_t cacheWrite1h = 0; // Subset of cacheWrite, from explicit one-hour cache counters.
};
using UsageEmitter = std::function<void(const ParsedUsage&)>;

struct AdapterScan {
    int64_t files = 0, invalid = 0;
    std::string error;
};

std::vector<SourceSpec> CatalogSources();
const SourceSpec* FindSource(const std::vector<SourceSpec>& sources, const std::string& id);
std::string IsoFromUnixMilliseconds(int64_t milliseconds);
std::string JsonString(yyjson_val* object, const char* key);
bool JsonCounter(yyjson_val* object, const char* key, int64_t& value, bool required = false);

// Native collectors read only the supplied app-owned path. Root Store persists/deduplicates emits.
AdapterScan CollectDomestic(const std::string& source, const std::wstring& path,
                           const std::atomic_bool& cancelled, const UsageEmitter& emit);
AdapterScan CollectCli(const std::string& source, const std::wstring& path,
                      const std::atomic_bool& cancelled, const UsageEmitter& emit);

// Pure record parsers used by collectors and meaningful fixtures. 'invalid' counts malformed usage.
void ParseDomesticRecord(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid);
void ParseCliRecord(const std::string& source, yyjson_val* root, const std::string& origin,
                    const UsageEmitter& emit, int64_t& invalid);

// Portable normalized export accepted for all catalog entries. Never derives usage from text/credits.
AdapterScan CollectUsageExport(const std::string& source, const std::wstring& path,
                              const std::atomic_bool& cancelled, const UsageEmitter& emit);

} // namespace tokenscope
