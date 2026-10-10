#include "shell/dialogs.h"

namespace tokenscope::shell {

const SourceSpec* FindSource(const std::string& id) {
    static const auto sources = CatalogSources();
    auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& source) { return source.id == id; });
    return found == sources.end() ? nullptr : &*found;
}
void RefreshConfiguredSource(const std::string& id, const std::wstring& path) {
    if (!FindSource(id)) return;
    auto connection = connectionStates.find(id);
    if (connection != connectionStates.end() && connection->second.state == "syncing") {
        state.notice = Tr(L"请等待本来源同步结束后再切换路径。");
        return;
    }
    const auto previous = configuredImports;
    auto existing = std::find_if(configuredImports.begin(), configuredImports.end(),
                                 [&](const auto& imported) { return imported.first == id; });
    if (path.empty()) {
        if (existing != configuredImports.end()) configuredImports.erase(existing);
    } else if (existing == configuredImports.end())
        configuredImports.emplace_back(id, path);
    else
        existing->second = path;
    if (!SavePreferences()) {
        configuredImports = previous;
        return;
    }
    if (state.collecting || priceDialogActive) {
        importRefreshPending = true;
        state.notice = Tr(L"路径设置已保存，当前操作结束后读取；保留已有历史用量。");
    } else
        StartCollection();
}
void ConfigureSourcePath(const std::string& id) {
    auto* source = FindSource(id);
    if (!source) return;
    auto connection = connectionStates.find(id);
    if (connection != connectionStates.end() && connection->second.state == "syncing") {
        state.notice = Tr(L"请等待本来源同步结束后再切换路径。");
        return;
    }
    SourcePathDialogState dialog;
    dialog.source = *source;
    for (const auto& entry : configuredImports)
        if (entry.first == id) dialog.value = entry.second;
    for (const auto& status : state.snapshot.sources)
        if (status.id == id) {
            for (const auto& path : status.paths)
                dialog.candidates += (path.exists ? Tr(L"已发现  ") : Tr(L"未发现  ")) + path.path + L"\r\n";
        }
    if (dialog.candidates.empty()) dialog.candidates = Tr(L"未配置本地路径，可选择真实用量文件或文件夹。");
    if (RunModal(L"TokenScopeSourcePath", Utf8ToWide(source->name) + Tr(L" · 采集路径"), 680, 468, &dialog,
                 dialog.done)) {
        if (dialog.saved || dialog.reset) RefreshConfiguredSource(id, dialog.reset ? L"" : dialog.value);
    }
    if (dialog.font) DeleteObject(dialog.font);
}
void DecorateSources() {
    for (auto& source : state.snapshot.sources) {
        const auto* profile = FindConnectionProfile(source.id);
        source.canAuthorize = profile != nullptr;
        if (!profile) continue;
        source.hasCredentials = HasConnectionCredential(dataDirectory, source.id);
        const auto found = connectionStates.find(source.id);
        source.connectionState =
            found != connectionStates.end() ? found->second.state : (source.hasCredentials ? "saved" : "unconfigured");
        source.connectionDetail =
            found != connectionStates.end() ? found->second.detail : Tr(L"实验性账号用量同步；不读取浏览器登录信息");
    }
}
void SyncSource(const std::string& id) {
    if (!FindConnectionProfile(id) || shuttingDown) return;
    if (connectionBusy) {
        state.notice = Tr(L"当前账号正在同步，请完成后再操作。");
        return;
    }
    std::string token, error;
    if (!LoadConnectionCredential(dataDirectory, id, token, error)) {
        state.notice = Tr(L"尚无可用授权，请重新连接。");
        return;
    }
    if (connectionWorker.joinable()) connectionWorker.join();
    connectionBusy = true;
    connectionStates[id] = {"syncing", Tr(L"正在读取最近 30 天实际用量"), GetTickCount64()};
    DecorateSources();
    const auto directory = (fs::path(dataDirectory) / L"connections" / Utf8ToWide(id)).wstring();
    connectionWorker = std::thread([id, directory, secret = std::move(token)]() mutable {
        auto packet = std::make_unique<ConnectionPacket>();
        packet->id = id;
        try {
            packet->result = SyncConnection(id, secret, directory, cancelled);
        } catch (...) {
            packet->result.error = Tr8("同步未完成");
        }
        if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size());
        secret.clear();
        if (PostMessageW(mainWindow, WM_CONNECTION, 0, reinterpret_cast<LPARAM>(packet.get()))) packet.release();
    });
    Redraw();
}
void ConnectSource(const std::string& id) {
    const auto* profile = FindConnectionProfile(id);
    if (!profile) return;
    if (connectionBusy) {
        state.notice = Tr(L"当前账号正在同步，请完成后再连接。");
        return;
    }
    ConnectionDialogState dialog;
    dialog.profile = *profile;
    RunModal(L"TokenScopeConnection", Utf8ToWide(Tr8(profile->name)) + Tr(L" · 授权连接"), 660, 430, &dialog,
             dialog.done);
    if (dialog.font) DeleteObject(dialog.font);
    if (dialog.saved && !shuttingDown) {
        std::string error;
        if (SaveConnectionCredential(dataDirectory, id, dialog.token, error)) {
            connectionStates[id] = {"saved", Tr(L"凭据已加密保存，尚待验证"), 0};
            if (!dialog.token.empty()) SecureZeroMemory(dialog.token.data(), dialog.token.size());
            dialog.token.clear();
            SyncSource(id);
        } else
            state.notice = Tr(L"凭据保存失败：") + Utf8ToWide(error);
    }
    if (!dialog.token.empty()) SecureZeroMemory(dialog.token.data(), dialog.token.size());
    DecorateSources();
}
void DisconnectSource(const std::string& id) {
    if (connectionBusy) {
        state.notice = Tr(L"请等待账号同步结束后解除连接。");
        return;
    }
    std::string error;
    if (!RemoveConnectionCredential(dataDirectory, id, error)) {
        state.notice = Tr(L"解除连接失败：") + Utf8ToWide(error);
        return;
    }
    connectionStates[id] = {"unconfigured", Tr(L"已删除本机凭据；保留同步缓存及历史用量"), 0};
    state.notice = Tr(L"已解除本机连接；如需撤销服务端登录会话，请在服务方账号设置中操作。");
    DecorateSources();
}
void SourceHelp(const std::string& id) {
    const auto* source = FindSource(id);
    if (!source) return;
    auto text = Tr(source->note) + L"\n\n";
    if (source->format == SourceFormat::UsageExport || source->paths.empty())
        text += Tr(
            L"当前接入方式：导入包含真实 Token、模型与调用时间的用量文件。点击“更改路径”选择文件或文件夹。普通聊天导出、积分和请求次数不能生成 Token 统计。\n\n此来源暂未提供账号直连按钮；网页登录本身不会使统计出现。");
    else
        text += Tr(
            L"当前接入方式：只读本地用量记录，通常无需额外账号授权。点击“更改路径”可查看完整路径或指定目录；“恢复默认”重新自动发现。\n\n若已发现路径仍无用量，需要该软件实际记录 Token；仅有程序安装目录或普通对话文本不够。");
    if (id == "trae-cn" || id == "trae-work")
        text += Tr(L"\n\nTRAE CN / TraeWork 与国际版 IDE 账号接口分开，当前不复用国际版凭据。");
    MessageBoxW(mainWindow, text.c_str(), (Utf8ToWide(source->name) + Tr(L" · 接入说明")).c_str(),
                MB_OK | MB_ICONINFORMATION);
}
void AddImportSource() {
    ImportDialogState dialog;
    dialog.sources = CatalogSources();
    if (dialog.sources.empty()) {
        state.notice = Tr(L"暂无可选择的来源。");
        return;
    }
    const UINT dpi = GetDpiForWindow(mainWindow);
    RECT owner{};
    GetWindowRect(mainWindow, &owner);
    RECT size{0, 0, MulDiv(600, dpi ? dpi : 96, 96), MulDiv(326, dpi ? dpi : 96, 96)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE;
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_DLGMODALFRAME, dpi ? dpi : 96);
    const int width = size.right - size.left, height = size.bottom - size.top;
    EnableWindow(mainWindow, FALSE);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TokenScopeImport", Tr(L"添加 / 导入用量来源"), style,
                                  owner.left + (owner.right - owner.left - width) / 2,
                                  owner.top + (owner.bottom - owner.top - height) / 2, width, height, mainWindow,
                                  nullptr, instance, &dialog);
    if (!window) {
        EnableWindow(mainWindow, TRUE);
        if (dialog.font) DeleteObject(dialog.font);
        state.notice = Tr(L"无法打开来源选择窗口。");
        return;
    }
    MSG message{};
    BOOL received = 1;
    while (!dialog.done && (received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!received) PostQuitMessage(static_cast<int>(message.wParam));
    if (IsWindow(window)) DestroyWindow(window);
    if (IsWindow(mainWindow)) {
        EnableWindow(mainWindow, TRUE);
        SetForegroundWindow(mainWindow);
    }
    if (dialog.font) DeleteObject(dialog.font);
    if (dialog.selected < 0 || shuttingDown) return;
    const auto& source = dialog.sources[static_cast<size_t>(dialog.selected)];
    const auto path = dialog.folder ? ChooseFolder() : ChooseUsageFile(source);
    if (path.empty()) return;
    RefreshConfiguredSource(source.id, path);
}
void ExportData() {
    // Use the cached immutable snapshot; never access the database from the UI thread.
    wchar_t path[32768] = L"TokenScope-usage.csv";
    OPENFILENAMEW save{};
    save.lStructSize = sizeof(save);
    save.hwndOwner = mainWindow;
    std::wstring filter = Tr(L"CSV 文件");
    filter.append(L"\0*.csv\0\0", 8);
    save.lpstrFilter = filter.c_str();
    save.lpstrFile = path;
    save.nMaxFile = 32768;
    save.lpstrDefExt = L"csv";
    save.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&save)) {
        Snapshot snapshot = state.snapshot;
        snapshot.rows = SelectRows(state);
        std::string error;
        if (!store.ExportCsv(path, snapshot, error))
            state.notice = Tr(L"导出失败：") + Utf8ToWide(error);
        else
            state.notice = Tr(L"已导出当前日期范围的软件 / 模型用量。");
    }
}

} // namespace tokenscope::shell
