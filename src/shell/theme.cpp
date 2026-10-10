#include "shell/shell.h"
#include <dwmapi.h>

namespace tokenscope::shell {

bool SystemUsesDarkTheme() {
    DWORD light = 1, size = sizeof(light);
    // Missing value (older Windows, policies) means the light theme.
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

void ApplyWindowChrome(HWND window) {
    if (!window) return;
    SetWindowTextW(window, Tr(L"TokenScope · 本地智能体用量"));
    // DWMWA_USE_IMMERSIVE_DARK_MODE; 20 on Windows 10 20H1+ and 11. Older builds ignore it.
    const BOOL dark = state.prefs.darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
}

void RefreshTheme() {
    const bool dark = state.prefs.theme == 2 || (state.prefs.theme == 0 && SystemUsesDarkTheme());
    state.prefs.darkMode = dark;
    ApplyWindowChrome(mainWindow);
    if (mainWindow) InvalidateRect(mainWindow, nullptr, FALSE);
}

} // namespace tokenscope::shell
