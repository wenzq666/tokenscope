#pragma once
#include <string>

namespace tokenscope {

// Current-user Windows DPAPI storage. Only cursor and trae-code are allowed.
// Credentials never appear in diagnostics; failures clear the Load output.
bool SaveConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,
                              const std::string& token,std::string& error);
bool LoadConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,
                              std::string& token,std::string& error);
bool HasConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId);
bool RemoveConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,
                                std::string& error);

} // namespace tokenscope
