#pragma once
// Dialog state shared between dialogs.cpp and the source-management actions.
#include "shell/shell.h"

namespace tokenscope::shell {

struct ImportDialogState {
    std::vector<SourceSpec> sources;
    HWND combo = nullptr, description = nullptr, currentPath = nullptr;
    HFONT font = nullptr;
    int selected = -1;
    bool folder = false, done = false;
};

struct SourcePathDialogState {
    SourceSpec source;
    std::wstring candidates, value;
    HWND edit = nullptr;
    HFONT font = nullptr;
    bool saved = false, reset = false, done = false;
};

struct ConnectionDialogState {
    ConnectionProfile profile;
    HWND edit = nullptr;
    HFONT font = nullptr;
    std::string token;
    bool done = false, saved = false;
};

} // namespace tokenscope::shell
