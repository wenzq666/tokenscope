#include "shell/shell.h"

namespace tokenscope::shell {

void UpdateTray() {
    std::wstring tip = TraySummary(state);
    wcsncpy_s(trayIcon.szTip, tip.c_str(), _TRUNCATE);
    trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &trayIcon);
}
void AddTray() {
    trayIcon = {};
    trayIcon.cbSize = sizeof(trayIcon);
    trayIcon.hWnd = mainWindow;
    trayIcon.uID = 1;
    trayIcon.uCallbackMessage = WM_TRAY;
    trayIcon.hIcon =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!trayIcon.hIcon) trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = TraySummary(state);
    wcsncpy_s(trayIcon.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &trayIcon);
    trayIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &trayIcon);
}
void ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW balloon = trayIcon;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(balloon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(balloon.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}
void Redraw() {
    keyboardHit = -1;
    InvalidateRect(mainWindow, nullptr, FALSE);
    UpdateTray();
}
void OpenMain(bool settings) {
    state.chart = Chart::Trend;
    state.selectedDay.clear();
    state.chartEnd = LocalToday();
    state.page = settings ? Page::Settings : Page::Overview;
    state.scroll = 0;
    ShowWindow(mainWindow, SW_RESTORE);
    SetForegroundWindow(mainWindow);
    Redraw();
}
void Execute(int command, const std::string& value) {
    switch (command) {
    case CmdOverview:
        state.page = Page::Overview;
        state.scroll = 0;
        break;
    case CmdSoftware:
        state.page = Page::Software;
        state.scroll = 0;
        break;
    case CmdModels:
        state.page = Page::Models;
        state.scroll = 0;
        break;
    case CmdSettings:
        state.page = Page::Settings;
        state.scroll = 0;
        break;
    case CmdToday:
    case CmdWeek:
    case CmdMonth:
    case CmdAll:
        state.prefs.range = static_cast<Range>(command - CmdToday);
        state.selectedDay.clear();
        state.scroll = 0;
        SavePreferences();
        break;
    case CmdTrend:
        state.chart = Chart::Trend;
        break;
    case CmdHeatmap:
        state.chart = Chart::Heatmap;
        break;
    case CmdTokens:
        state.costMetric = false;
        break;
    case CmdCost:
        state.costMetric = true;
        break;
    case CmdSelectDay:
        state.selectedDay = value;
        break;
    case CmdClearDay:
        state.selectedDay.clear();
        break;
    case CmdTrendWeek:
        state.trendMonth = false;
        break;
    case CmdTrendMonth:
        state.trendMonth = true;
        break;
    case CmdPrevious:
        state.chartEnd = ShiftDay(state.chartEnd.empty() ? LocalToday() : state.chartEnd, state.trendMonth ? -30 : -7);
        break;
    case CmdNext:
        state.chartEnd = std::min(
            LocalToday(), ShiftDay(state.chartEnd.empty() ? LocalToday() : state.chartEnd, state.trendMonth ? 30 : 7));
        break;
    case CmdRefresh:
        StartCollection();
        StartExchangeRate(true);
        break;
    case CmdCurrency:
        state.prefs.cny = !state.prefs.cny;
        SavePreferences();
        StartExchangeRate();
        break;
    case CmdExchangeRate:
        EditExchangeRate();
        break;
    case CmdTrendColor:
        ChooseChartColor(false);
        break;
    case CmdHeatColor:
        ChooseChartColor(true);
        break;
    case CmdResetColors:
        state.prefs.trendColor = RGB(53, 101, 223);
        state.prefs.heatColor = RGB(105, 157, 47);
        SavePreferences();
        break;
    case CmdAutoRefresh:
        state.prefs.autoRefresh = !state.prefs.autoRefresh;
        SavePreferences();
        break;
    case CmdOpenData:
        ShellExecuteW(mainWindow, L"open", dataDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case CmdExport:
        ExportData();
        break;
    case CmdImportSource:
        AddImportSource();
        break;
    case CmdSourcePath:
        ConfigureSourcePath(value);
        break;
    case CmdSourceReset:
        RefreshConfiguredSource(value, L"");
        break;
    case CmdSourceConnect:
        ConnectSource(value);
        break;
    case CmdSourceSync:
        SyncSource(value);
        break;
    case CmdSourceDisconnect:
        DisconnectSource(value);
        break;
    case CmdSourceHelp:
        SourceHelp(value);
        break;
    case CmdImportCodex:
    case CmdImportClaude: {
        std::wstring folder = ChooseFolder();
        if (!folder.empty()) RefreshConfiguredSource(command == CmdImportCodex ? "codex" : "claude", folder);
        break;
    }
    case CmdOnlineCheck:
        StartOnlineCheck();
        StartExchangeRate(true);
        break;
    case CmdPricing:
        EditModelPrice();
        break;
    case CmdTrayMonth:
        state.trayMonth = true;
        break;
    case CmdTrayToday:
        state.trayMonth = false;
        break;
    case CmdTheme:
        state.prefs.theme = (state.prefs.theme + 1) % 3;
        SavePreferences();
        RefreshTheme();
        break;
    case CmdLanguage:
        state.prefs.language = (state.prefs.language + 1) % 3;
        SetLanguage(ResolveLanguage(static_cast<LanguagePreference>(state.prefs.language)));
        SavePreferences();
        ApplyWindowChrome(mainWindow);
        state.notice = Tr(L"界面语言已切换；部分已显示的状态在下次刷新后更新。");
        break;
    case CmdBudget:
        EditBudget();
        break;
    case CmdOpenMain:
        OpenMain();
        return;
    case CmdExit:
        DestroyWindow(mainWindow);
        return;
    }
    Redraw();
}

void UpdateScroll(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, state.contentHeight - 1);
    info.nPage = rect.bottom;
    state.scroll = std::clamp(state.scroll, 0, std::max<int>(0, state.contentHeight - rect.bottom));
    info.nPos = state.scroll;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}
void ScrollBy(int pixels) {
    state.scroll += pixels;
    UpdateScroll(mainWindow);
    Redraw();
}
void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CmdOpenMain, Tr(L"打开 TokenScope"));
    AppendMenuW(menu, MF_STRING, CmdRefresh, Tr(L"刷新用量"));
    AppendMenuW(menu, MF_STRING, 900, pauseCollection ? Tr(L"恢复自动采集") : Tr(L"暂停自动采集"));
    AppendMenuW(menu, MF_STRING, CmdSettings, Tr(L"设置"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdExit, Tr(L"退出"));
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(mainWindow);
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, mainWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(mainWindow, WM_NULL, 0, 0);
    if (choice == 900) {
        pauseCollection = !pauseCollection;
        state.notice = pauseCollection ? Tr(L"自动采集已暂停。") : Tr(L"自动采集已恢复。");
        Redraw();
    } else if (choice == CmdSettings)
        OpenMain(true);
    else if (choice)
        Execute(choice);
}
void PaintBuffered(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT rect{};
    GetClientRect(window, &rect);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, rect.right), std::max(1L, rect.bottom));
    HGDIOBJ previous = SelectObject(memory, bitmap);
    state.dpi = GetDpiForWindow(window) / 96.0f;
    PaintView(window, memory, rect, state);
    if (keyboardHit >= 0 && keyboardHit < static_cast<int>(state.hits.size())) {
        RECT focus = state.hits[keyboardHit].rect;
        DrawFocusRect(memory, &focus);
    }
    BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
    UpdateScroll(window);
}
void HitClick(LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const auto hits = state.hits;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (PtInRect(&it->rect, point)) {
            Execute(it->command, it->value);
            return;
        }
}
void Hover(HWND window, LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    bool interactive = false;
    std::wstring text;
    for (auto it = state.hits.rbegin(); it != state.hits.rend(); ++it)
        if (PtInRect(&it->rect, point)) {
            interactive = true;
            if (it->command == CmdSelectDay) {
                int64_t total = 0;
                bool known = false;
                for (const auto& row : state.snapshot.rows)
                    if (row.day == it->value) {
                        total += row.total();
                        known = true;
                    }
                text = Utf8ToWide(it->value) + L" · " +
                       (known ? FormatCount(total) + L" Token" : Tr(L"未采集 · 用量未知"));
            }
            break;
        }
    SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));
    if (tooltipWindow) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window;
        tool.uId = 1;
        tool.lpszText = text.empty() ? const_cast<wchar_t*>(L"") : text.data();
        SendMessageW(tooltipWindow, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(tooltipWindow, TTM_ACTIVATE, !text.empty(), 0);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated && message == taskbarCreated) {
        AddTray();
        return 0;
    }
    switch (message) {
    case WM_PAINT:
        PaintBuffered(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        UpdateScroll(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetDpiForWindow(window);
        info->ptMinTrackSize = {MulDiv(800, dpi ? dpi : 96, 96), MulDiv(600, dpi ? dpi : 96, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONUP:
        HitClick(lParam);
        return 0;
    case WM_MOUSEMOVE:
        Hover(window, lParam);
        return 0;
    case WM_MOUSEWHEEL:
        ScrollBy(-GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 90);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            state.scroll -= 40;
            break;
        case SB_LINEDOWN:
            state.scroll += 40;
            break;
        case SB_PAGEUP:
            state.scroll -= static_cast<int>(info.nPage) * 3 / 4;
            break;
        case SB_PAGEDOWN:
            state.scroll += static_cast<int>(info.nPage) * 3 / 4;
            break;
        case SB_THUMBTRACK:
            state.scroll = info.nTrackPos;
            break;
        }
        UpdateScroll(window);
        Redraw();
    }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        if (wParam == VK_TAB && !state.hits.empty()) {
            keyboardHit = (keyboardHit + (GetKeyState(VK_SHIFT) < 0 ? -1 : 1) + static_cast<int>(state.hits.size())) %
                          static_cast<int>(state.hits.size());
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if ((wParam == VK_RETURN || wParam == VK_SPACE) && keyboardHit >= 0 &&
            keyboardHit < static_cast<int>(state.hits.size())) {
            auto hit = state.hits[keyboardHit];
            Execute(hit.command, hit.value);
            return 0;
        }
        if (wParam == VK_F5 || (wParam == 'R' && GetKeyState(VK_CONTROL) < 0)) Execute(CmdRefresh);
        if (wParam == 'T') Execute(CmdTrend);
        if (wParam == 'H') Execute(CmdHeatmap);
        if (wParam >= '1' && wParam <= '4') Execute(CmdToday + static_cast<int>(wParam - '1'));
        if (wParam == VK_DOWN) ScrollBy(50);
        if (wParam == VK_UP) ScrollBy(-50);
        if (wParam == VK_NEXT) ScrollBy(400);
        if (wParam == VK_PRIOR) ScrollBy(-400);
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        state.notice = Tr(L"已隐藏到托盘，继续采集。");
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_EXIT) {
            DestroyWindow(window);
            return 0;
        }
        if (wParam == TIMER_REFRESH) {
            std::wstring today = Utf8ToWide(LocalToday());
            if (today != lastDay) {
                lastDay = today;
                state.chartEnd = LocalToday();
                Redraw();
            }
            // With change notifications active, polling is only a slow safety net (new folders,
            // network drives and filesystems without notifications).
            if (state.prefs.autoRefresh && !pauseCollection &&
                (!SourceWatcherActive() || GetTickCount64() - lastCollectionStart >= kFallbackPollInterval))
                StartCollection();
            StartExchangeRate();
        }
        if (wParam == TIMER_WATCH) OnWatchTimer();
        return 0;
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) RefreshTheme();
        return 0;
    case WM_SOURCES_CHANGED:
        OnSourcesChanged();
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            AddTray();
            if (!pauseCollection) StartCollection();
        }
        return TRUE;
    case WM_TRAY: {
        UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT) OpenMain();
        if (event == WM_CONTEXTMENU) ShowTrayMenu();
        if (event == WM_LBUTTONDBLCLK) OpenMain();
        return 0;
    }
    case WM_IMPORTED: {
        std::unique_ptr<ScanPacket> packet(reinterpret_cast<ScanPacket*>(lParam));
        if (collector.joinable()) collector.join();
        state.collecting = false;
        state.snapshot = std::move(packet->snapshot);
        DecorateSources();
        if (!packet->notice.empty())
            state.notice = packet->notice;
        else if (!state.snapshot.error.empty())
            state.notice = Tr(L"采集异常：") + Utf8ToWide(state.snapshot.error);
        else if (!packet->result.error.empty())
            state.notice = Tr(L"部分来源未完成：") + Utf8ToWide(packet->result.error);
        else
            state.notice =
                Tr(L"已更新 · 新增 ") + FormatCount(packet->result.added) + Tr(L" 条用量记录") +
                (packet->result.invalid ? Tr(L" · 跳过 ") + FormatCount(packet->result.invalid) + Tr(L" 条无效记录")
                                        : L"");
        RestartSourceWatcher();
        CheckBudgetAlerts();
        if (importRefreshPending) {
            importRefreshPending = false;
            StartCollection();
        } else if (watchRefreshPending)
            OnSourcesChanged();
        Redraw();
        return 0;
    }
    case WM_ONLINE: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        state.onlineStatus = *result;
        if (state.collecting || priceDialogActive)
            importRefreshPending = true;
        else
            StartCollection();
        Redraw();
        return 0;
    }
    case WM_EXCHANGE: {
        std::unique_ptr<ExchangePacket> packet(reinterpret_cast<ExchangePacket*>(lParam));
        if (exchangeWorker.joinable()) exchangeWorker.join();
        exchangeBusy = false;
        ApplyExchangeResult(*packet);
        CheckBudgetAlerts();
        Redraw();
        return 0;
    }
    case WM_CONNECTION: {
        std::unique_ptr<ConnectionPacket> packet(reinterpret_cast<ConnectionPacket*>(lParam));
        if (connectionWorker.joinable()) connectionWorker.join();
        connectionBusy = false;
        auto& connection = connectionStates[packet->id];
        if (packet->result.success) {
            connection.state = "connected";
            connection.detail = packet->result.usageRecords
                                    ? TrF(L"最近 30 天 · 已同步 {0} 条真实用量到本地缓存", packet->result.usageRecords)
                                    : Tr(L"连接成功，最近 30 天暂无可采集 Token 用量");
            RefreshConfiguredSource(packet->id, packet->result.cachePath);
        } else {
            connection.state = packet->result.authenticationFailed ? "expired" : "error";
            connection.detail =
                packet->result.authenticationFailed
                    ? Tr(L"凭据无效或已过期，请重新授权")
                    : Utf8ToWide(packet->result.error.empty() ? Tr8("同步未完成，请稍后重试") : packet->result.error);
            state.notice = Utf8ToWide(packet->id) + Tr(L"：") + connection.detail + Tr(L"；保留之前的缓存和历史用量。");
        }
        DecorateSources();
        Redraw();
        return 0;
    }
    case WM_DESTROY:
        shuttingDown = true;
        KillTimer(window, TIMER_REFRESH);
        Shell_NotifyIconW(NIM_DELETE, &trayIcon);
        StopWorkers();
        MSG pending{};
        while (PeekMessageW(&pending, window, WM_IMPORTED, WM_IMPORTED, PM_REMOVE))
            delete reinterpret_cast<ScanPacket*>(pending.lParam);
        while (PeekMessageW(&pending, window, WM_ONLINE, WM_ONLINE, PM_REMOVE))
            delete reinterpret_cast<std::wstring*>(pending.lParam);
        while (PeekMessageW(&pending, window, WM_EXCHANGE, WM_EXCHANGE, PM_REMOVE))
            delete reinterpret_cast<ExchangePacket*>(pending.lParam);
        while (PeekMessageW(&pending, window, WM_CONNECTION, WM_CONNECTION, PM_REMOVE))
            delete reinterpret_cast<ConnectionPacket*>(pending.lParam);
        SavePreferences();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace tokenscope::shell
