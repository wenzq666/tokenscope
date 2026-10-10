#pragma once
#include "../adapters.h"

namespace tokenscope {
// Native formats audited against missuo/tokens (MIT). No remote synchronization.
AdapterScan CollectExtended(const std::string& source, const std::wstring& path, const std::atomic_bool& cancelled,
                            const UsageEmitter& emit);
void ParseExtendedRecord(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid);
// Strict message-family parsers; false means this source belongs to another family.
bool ParseExtendedFamily(const std::string& source, yyjson_val* root, const std::string& origin,
                         const UsageEmitter& emit, int64_t& invalid);
} // namespace tokenscope
