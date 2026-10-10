#include "shell/dialogs.h"

namespace tokenscope::shell {

// Creates child controls in logical 96-DPI coordinates, scaled to the dialog's DPI, with the
// shared dialog font. The caller keeps the returned font alive until the dialog is destroyed.
class DialogBuilder {
public:
    explicit DialogBuilder(HWND window) : window_(window) {
        const UINT dpi = GetDpiForWindow(window);
        dpi_ = dpi ? static_cast<int>(dpi) : 96;
        font_ = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    }
    int Px(int logical) const { return MulDiv(logical, dpi_, 96); }
    HWND Add(const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
        HWND control = CreateWindowExW(wcscmp(klass, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, klass, text,
                                       WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w), Px(h), window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        controls_.push_back(control);
        return control;
    }
    // Applies the font to every control and hands ownership of it to the caller.
    HFONT Finish() {
        const auto font =
            reinterpret_cast<WPARAM>(font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : controls_) SendMessageW(control, WM_SETFONT, font, TRUE);
        return font_;
    }

private:
    HWND window_;
    int dpi_ = 96;
    HFONT font_ = nullptr;
    std::vector<HWND> controls_;
};

struct RateDialogState {
    HWND edit = nullptr;
    HFONT font = nullptr;
    bool done = false, saved = false;
    double value = 0;
};
LRESULT CALLBACK RateDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<RateDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<RateDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"1 USD 等于多少 CNY？留空或填 0 使用公开汇率。\n手动值优先；仅用于显示换算。"), 0, 20, 16,
               420, 72);
        dialog->edit = ui.Add(L"EDIT", dialog->value > 0 ? std::to_wstring(dialog->value).c_str() : L"",
                              WS_TABSTOP | ES_AUTOHSCROLL, 20, 96, 420, 30, 10);
        ui.Add(L"BUTTON", Tr(L"保存"), WS_TABSTOP | BS_DEFPUSHBUTTON, 230, 146, 100, 34, IDOK);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 340, 146, 100, 34, IDCANCEL);
        dialog->font = ui.Finish();
        SetFocus(dialog->edit);
        SendMessageW(dialog->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buffer[128]{};
            GetWindowTextW(dialog->edit, buffer, 128);
            wchar_t* start = buffer;
            while (iswspace(*start)) ++start;
            wchar_t* end = nullptr;
            double value = *start ? wcstod(start, &end) : 0;
            if (!*start) end = start;
            while (end && iswspace(*end)) ++end;
            if (!std::isfinite(value) || value < 0 || value >= 10000 || !end || *end || (*start && end == start)) {
                MessageBoxW(window, Tr(L"留空或填 0 使用公开汇率；手动汇率须大于 0、小于 10000。"), Tr(L"汇率"),
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            dialog->value = value;
            dialog->saved = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void EditExchangeRate() {
    RateDialogState dialog;
    dialog.value = state.prefs.usdToCny;
    RunModal(L"TokenScopeRate", Tr(L"设置人民币汇率"), 460, 200, &dialog, dialog.done);
    if (dialog.font) DeleteObject(dialog.font);
    if (dialog.saved && !shuttingDown) {
        state.prefs.usdToCny = dialog.value;
        state.notice = dialog.value > 0 ? Tr(L"已使用手动汇率。") : Tr(L"已切换为公开汇率。");
        SavePreferences();
        StartExchangeRate(true);
    }
}
void ChooseChartColor(bool heat) {
    static COLORREF custom[16]{};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = mainWindow;
    choose.rgbResult = heat ? state.prefs.heatColor : state.prefs.trendColor;
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&choose)) {
        COLORREF color = choose.rgbResult;
        const double luminance = .2126 * GetRValue(color) + .7152 * GetGValue(color) + .0722 * GetBValue(color);
        if (luminance > 210) {
            const double scale = 210.0 / luminance;
            color = RGB(static_cast<int>(GetRValue(color) * scale), static_cast<int>(GetGValue(color) * scale),
                        static_cast<int>(GetBValue(color) * scale));
            state.notice = Tr(L"所选颜色较浅，已适当加深以便在图表中辨认。");
        }
        if (heat)
            state.prefs.heatColor = color;
        else
            state.prefs.trendColor = color;
        SavePreferences();
    }
}
struct PriceDialogState {
    HWND combo = nullptr;
    HWND fields[4]{};
    HFONT font = nullptr;
    std::vector<PriceRule> models;
    PriceRule selected;
    bool done = false, saved = false;
};
bool ParsePrice(std::wstring text, int64_t& result) {
    auto first = text.find_first_not_of(L" \t\r\n"), last = text.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    text = text.substr(first, last - first + 1);
    int64_t integer = 0, fraction = 0, scale = 1;
    bool decimal = false, digit = false;
    for (wchar_t ch : text) {
        if (ch == L'.' && !decimal) {
            decimal = true;
            continue;
        }
        if (ch < L'0' || ch > L'9') return false;
        digit = true;
        if (!decimal) {
            integer = integer * 10 + (ch - L'0');
            if (integer > 10000) return false;
        } else {
            if (scale >= 1000000) return false;
            fraction = fraction * 10 + (ch - L'0');
            scale *= 10;
        }
    }
    if (!digit) return false;
    result = integer * 1000000 + fraction * (1000000 / scale);
    return result <= 10000LL * 1000000;
}
LRESULT CALLBACK PriceDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<PriceDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<PriceDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"选择记录中的实际软件 / 模型 / 提供商："), 0, 20, 16, 620, 24);
        dialog->combo = ui.Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 46, 620, 260, 20);
        for (const auto& model : dialog->models) {
            auto label = Utf8ToWide(model.software + " / " + model.model + " / " +
                                    (model.provider.empty() ? Tr8("提供商未记录") : model.provider));
            SendMessageW(dialog->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(dialog->combo, CB_SETCURSEL, 0, 0);
        const wchar_t* labels[] = {Tr(L"普通输入"), Tr(L"输出"), Tr(L"缓存读取"), Tr(L"缓存写入")};
        for (int i = 0; i < 4; ++i) {
            const int y = 94 + i * 44;
            ui.Add(L"STATIC", labels[i], 0, 20, y + 4, 150, 26);
            dialog->fields[i] = ui.Add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 180, y, 220, 30, 30 + i);
            ui.Add(L"STATIC", Tr(L"USD / 百万 Token"), 0, 414, y + 4, 226, 26);
        }
        ui.Add(
            L"STATIC",
            Tr(L"四项均需填写，可填 0；最多六位小数。\n会补算该身份尚未定价的历史记录，并用于未来记录。\n已定价历史沿用旧版本；估算不等于实际扣费。"),
            0, 20, 274, 620, 116);
        ui.Add(L"BUTTON", Tr(L"保存并估算"), WS_TABSTOP | BS_DEFPUSHBUTTON, 364, 404, 170, 34, IDOK);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 544, 404, 96, 34, IDCANCEL);
        dialog->font = ui.Finish();
        SetFocus(dialog->fields[0]);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto index = static_cast<int>(SendMessageW(dialog->combo, CB_GETCURSEL, 0, 0));
            if (index < 0 || index >= static_cast<int>(dialog->models.size())) return 0;
            int64_t rates[4]{};
            for (int i = 0; i < 4; ++i) {
                wchar_t text[100]{};
                GetWindowTextW(dialog->fields[i], text, 100);
                if (!ParsePrice(text, rates[i])) {
                    MessageBoxW(window, Tr(L"每项请输入 0–10000 USD / 百万 Token，最多六位小数。"), Tr(L"模型价格"),
                                MB_OK | MB_ICONINFORMATION);
                    SetFocus(dialog->fields[i]);
                    return 0;
                }
            }
            dialog->selected = dialog->models[index];
            dialog->selected.inputMicrosPerMillion = rates[0];
            dialog->selected.outputMicrosPerMillion = rates[1];
            dialog->selected.cachedMicrosPerMillion = rates[2];
            dialog->selected.writeMicrosPerMillion = rates[3];
            dialog->saved = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void EditModelPrice() {
    if (state.collecting) {
        state.notice = Tr(L"首次采集完成后即可设置模型价格。");
        return;
    }
    PriceDialogState dialog;
    std::set<std::string> identities;
    for (const auto& row : state.snapshot.rows) {
        if (row.model.empty()) continue;
        if (identities.insert(row.software + "\n" + row.model + "\n" + row.provider).second)
            dialog.models.push_back({row.software, row.model, row.provider});
    }
    if (dialog.models.empty()) {
        state.notice = Tr(L"尚未采集到有实际模型 ID 的记录；未知模型保持未定价。");
        return;
    }
    priceDialogActive = true;
    RunModal(L"TokenScopePrice", Tr(L"手动模型价格 · USD / 百万 Token"), 660, 456, &dialog, dialog.done);
    priceDialogActive = false;
    if (dialog.font) DeleteObject(dialog.font);
    if (shuttingDown) return;
    if (!dialog.saved) {
        if (importRefreshPending) {
            importRefreshPending = false;
            StartCollection();
        }
        return;
    }
    if (collector.joinable()) collector.join();
    state.collecting = true;
    state.notice = Tr(L"正在按手动价格估算尚未定价记录…");
    collector = std::thread([rule = dialog.selected] {
        auto packet = std::make_unique<ScanPacket>();
        std::string error;
        try {
            if (store.SetPriceRule(rule, error))
                packet->notice = Tr(L"手动价格已保存；未定价记录已估算，已有历史价格未改写。");
            else
                packet->notice = Tr(L"价格保存失败：") + Utf8ToWide(error);
            packet->snapshot = store.ReadSnapshot();
        } catch (const std::exception& e) {
            packet->notice = Tr(L"计价失败：") + Utf8ToWide(e.what());
        }
        if (PostMessageW(mainWindow, WM_IMPORTED, 0, reinterpret_cast<LPARAM>(packet.get()))) packet.release();
    });
}
std::wstring ChooseFolder(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Tr(L"选择用量日志文件夹（只读导入）"));
    std::wstring folder;
    if (SUCCEEDED(dialog->Show(owner ? owner : mainWindow))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return folder;
}
void UpdateImportDescription(ImportDialogState& dialog) {
    const auto index = SendMessageW(dialog.combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<size_t>(index) >= dialog.sources.size()) return;
    const auto& selected = dialog.sources[static_cast<size_t>(index)];
    SetWindowTextW(dialog.description, Tr(selected.note).c_str());
    std::wstring text = Tr(L"尚未添加自定义用量路径");
    for (const auto& imported : configuredImports)
        if (imported.first == selected.id) {
            text = Tr(L"当前路径：") + imported.second;
            break;
        }
    SetWindowTextW(dialog.currentPath, text.c_str());
}
LRESULT CALLBACK ImportDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<ImportDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<ImportDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"选择记录所属的软件"), 0, 20, 18, 560, 25);
        dialog->combo = ui.Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 49, 560, 280, 70);
        for (const auto& source : dialog->sources) {
            const auto label = Utf8ToWide(source.name);
            SendMessageW(dialog->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(dialog->combo, CB_SETMINVISIBLE, 12, 0);
        SendMessageW(dialog->combo, CB_SETCURSEL, 0, 0);
        dialog->description = ui.Add(L"STATIC", L"", 0, 20, 92, 560, 58);
        dialog->currentPath = ui.Add(L"STATIC", L"", SS_ENDELLIPSIS, 20, 157, 560, 40);
        ui.Add(L"STATIC", Tr(L"只读实际 Token 记录，不修改原文件。\n普通聊天导出与积分记录不会生成 Token 用量。"), 0,
               20, 205, 560, 48);
        ui.Add(L"BUTTON", Tr(L"选择文件"), WS_TABSTOP | BS_DEFPUSHBUTTON, 215, 272, 110, 34, IDOK);
        ui.Add(L"BUTTON", Tr(L"选择文件夹"), WS_TABSTOP, 337, 272, 128, 34, 71);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 477, 272, 103, 34, IDCANCEL);
        dialog->font = ui.Finish();
        UpdateImportDescription(*dialog);
        SetFocus(dialog->combo);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 70 && HIWORD(wParam) == CBN_SELCHANGE) {
            UpdateImportDescription(*dialog);
            return 0;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == 71) {
            const auto selected = SendMessageW(dialog->combo, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR && static_cast<size_t>(selected) < dialog->sources.size()) {
                dialog->selected = static_cast<int>(selected);
                dialog->folder = LOWORD(wParam) == 71;
                DestroyWindow(window);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
std::wstring ChooseUsageFile(const SourceSpec& source, HWND owner) {
    wchar_t path[32768]{};
    std::wstring initial;
    for (const auto& configured : configuredImports)
        if (configured.first == source.id) {
            std::error_code error;
            fs::path previous(configured.second);
            initial = fs::is_directory(previous, error) ? previous.wstring() : previous.parent_path().wstring();
            break;
        }
    const auto title = Utf8ToWide(source.name) + Tr(L" · 选择真实用量文件（只读）");
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = owner ? owner : mainWindow;
    open.lpstrTitle = title.c_str();
    // Filter pairs are NUL separated, so build them from individually translated labels.
    std::wstring filter;
    for (const auto& [label, pattern] : std::initializer_list<std::pair<std::wstring, const wchar_t*>>{
             {Tr(L"用量文件"), L"*.json;*.jsonl;*.csv;*.db;*.sqlite;*.sqlite3;*.zstd"},
             {L"JSON / JSONL", L"*.json;*.jsonl"},
             {L"CSV", L"*.csv"},
             {L"SQLite", L"*.db;*.sqlite;*.sqlite3"},
             {Tr(L"压缩会话"), L"*.zstd"},
             {Tr(L"所有文件"), L"*.*"}}) {
        filter += label;
        filter += L'\0';
        filter += pattern;
        filter += L'\0';
    }
    filter += L'\0';
    open.lpstrFilter = filter.c_str();
    open.lpstrFile = path;
    open.nMaxFile = 32768;
    open.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_EXPLORER;
    if (GetOpenFileNameW(&open)) return path;
    if (const DWORD error = CommDlgExtendedError())
        state.notice = Tr(L"文件选择失败，错误码 ") + std::to_wstring(error);
    return {};
}
std::wstring EditText(HWND edit) {
    int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(length);
    return text;
}
LRESULT CALLBACK SourcePathDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<SourcePathDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<SourcePathDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"当前采集路径（可选中复制；发现状态以最近一次采集为准）"), 0, 20, 16, 640, 25);
        ui.Add(L"EDIT", dialog->candidates.c_str(),
               WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 20,
               47, 640, 105);
        ui.Add(L"STATIC", Tr(dialog->source.note).c_str(), 0, 20, 165, 640, 50);
        ui.Add(L"STATIC", Tr(L"自定义文件或文件夹路径"), 0, 20, 224, 640, 23);
        dialog->edit = ui.Add(L"EDIT", dialog->value.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 20, 253, 640, 31, 80);
        SendMessageW(dialog->edit, EM_SETLIMITTEXT, 32767, 0);
        ui.Add(L"BUTTON", Tr(L"选择文件"), WS_TABSTOP, 20, 300, 112, 32, 81);
        ui.Add(L"BUTTON", Tr(L"选择文件夹"), WS_TABSTOP, 145, 300, 125, 32, 82);
        ui.Add(
            L"STATIC",
            Tr(L"保存后仅读取此路径；尚不存在的路径会显示“未发现”。\n恢复默认会重新自动发现，已采集的历史数据会保留。"),
            0, 20, 347, 640, 48);
        ui.Add(L"BUTTON", Tr(L"恢复默认"), WS_TABSTOP, 20, 411, 118, 34, 83);
        ui.Add(L"BUTTON", Tr(L"保存路径"), WS_TABSTOP | BS_DEFPUSHBUTTON, 420, 411, 114, 34, IDOK);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 546, 411, 114, 34, IDCANCEL);
        dialog->font = ui.Finish();
        SetFocus(dialog->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 81 || LOWORD(wParam) == 82) {
            auto path = LOWORD(wParam) == 81 ? ChooseUsageFile(dialog->source, window) : ChooseFolder(window);
            if (!path.empty()) SetWindowTextW(dialog->edit, path.c_str());
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            auto text = EditText(dialog->edit);
            const auto first = text.find_first_not_of(L" \t\r\n"), last = text.find_last_not_of(L" \t\r\n");
            if (first == std::wstring::npos) {
                MessageBoxW(window, Tr(L"请输入路径；使用“恢复默认”重新自动发现。"), Tr(L"采集路径"),
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            text = text.substr(first, last - first + 1);
            if (text.size() > 1 && text.front() == L'"' && text.back() == L'"') text = text.substr(1, text.size() - 2);
            DWORD length = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
            if (!length || length > 32768) {
                MessageBoxW(window, Tr(L"路径过长或无效。"), Tr(L"采集路径"), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            std::wstring expanded(length, L'\0');
            ExpandEnvironmentStringsW(text.c_str(), expanded.data(), length);
            expanded.resize(length - 1);
            std::error_code error;
            auto absolute = fs::absolute(fs::path(expanded), error).lexically_normal();
            if (error || expanded.empty() || expanded.find_first_of(L"\r\n\t") != std::wstring::npos) {
                MessageBoxW(window, Tr(L"请输入有效文件或文件夹路径。"), Tr(L"采集路径"), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            dialog->value = absolute.wstring();
            dialog->saved = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == 83) {
            dialog->reset = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
bool RunModal(const wchar_t* klass, const std::wstring& title, int logicalWidth, int logicalHeight, void* dialog,
              bool& done) {
    const UINT dpi = GetDpiForWindow(mainWindow);
    RECT owner{};
    GetWindowRect(mainWindow, &owner);
    RECT size{0, 0, MulDiv(logicalWidth, dpi ? dpi : 96, 96), MulDiv(logicalHeight, dpi ? dpi : 96, 96)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE;
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_DLGMODALFRAME, dpi ? dpi : 96);
    EnableWindow(mainWindow, FALSE);
    auto window =
        CreateWindowExW(WS_EX_DLGMODALFRAME, klass, title.c_str(), style,
                        owner.left + (owner.right - owner.left - (size.right - size.left)) / 2,
                        owner.top + (owner.bottom - owner.top - (size.bottom - size.top)) / 2, size.right - size.left,
                        size.bottom - size.top, mainWindow, nullptr, instance, dialog);
    if (!window) {
        EnableWindow(mainWindow, TRUE);
        return false;
    }
    MSG message{};
    BOOL received = 1;
    while (!done && (received = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    if (!received) PostQuitMessage(static_cast<int>(message.wParam));
    if (IsWindow(window)) DestroyWindow(window);
    if (IsWindow(mainWindow)) {
        EnableWindow(mainWindow, TRUE);
        SetForegroundWindow(mainWindow);
    }
    return !shuttingDown;
}
LRESULT CALLBACK ConnectionDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<ConnectionDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<ConnectionDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"连接自己的账号，读取最近 30 天用量"), 0, 20, 16, 620, 27);
        ui.Add(L"STATIC", Utf8ToWide(Tr8(dialog->profile.credentialHint)).c_str(), 0, 20, 52, 620, 72);
        ui.Add(L"BUTTON", Tr(L"打开官方用量页面"), WS_TABSTOP, 20, 135, 250, 32, 90);
        ui.Add(L"STATIC", Tr(L"在系统默认浏览器打开；网页登录不会自动传回凭据。"), 0, 284, 131, 356, 46);
        ui.Add(L"STATIC", Tr(L"账号用量凭据（遮罩输入）"), 0, 20, 185, 620, 24);
        dialog->edit = ui.Add(L"EDIT", L"", WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 20, 218, 620, 32, 91);
        SendMessageW(dialog->edit, EM_SETLIMITTEXT, 16384, 0);
        ui.Add(
            L"STATIC",
            Tr(L"实验性 Dashboard 接口，服务方变更时可能需要重新连接。\n凭据由 Windows 在本机加密保存，仅发送到对应服务方。\n成功取得实际 Token 记录后才统计；不会把积分换算为 Token。\n连接成功后，本来源切换到该账号的本地同步缓存。"),
            0, 20, 266, 620, 88);
        ui.Add(L"BUTTON", Tr(L"授权并同步"), WS_TABSTOP | BS_DEFPUSHBUTTON, 354, 375, 166, 34, IDOK);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 534, 375, 106, 34, IDCANCEL);
        dialog->font = ui.Finish();
        SetFocus(dialog->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 90) {
            auto url = Utf8ToWide(dialog->profile.websiteUrl);
            ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            auto input = EditText(dialog->edit);
            const auto first = input.find_first_not_of(L" \t\r\n"), last = input.find_last_not_of(L" \t\r\n");
            std::string token = first == std::wstring::npos ? "" : WideToUtf8(input.substr(first, last - first + 1));
            SecureZeroMemory(input.data(), input.size() * sizeof(wchar_t));
            if (dialog->profile.sourceId == "cursor" && token.starts_with("WorkosCursorSessionToken="))
                token.erase(0, 25);
            if (dialog->profile.sourceId == "trae-code" && token.starts_with("Cloud-IDE-JWT ")) token.erase(0, 14);
            bool invalid = token.empty() || token.size() > 16384;
            for (unsigned char ch : token)
                if (ch <= 32 || ch >= 127 || ch == ';' || ch == '\r' || ch == '\n') invalid = true;
            if (!invalid) {
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                ULARGE_INTEGER ticks{};
                ticks.LowPart = now.dwLowDateTime;
                ticks.HighPart = now.dwHighDateTime;
                const auto milliseconds = static_cast<int64_t>((ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
                std::string error;
                invalid = !BuildConnectionRequest(dialog->profile.sourceId, token, milliseconds - 30LL * 86400000,
                                                  milliseconds, 1, error)
                               .has_value();
            }
            if (invalid) {
                if (!token.empty()) SecureZeroMemory(token.data(), token.size());
                MessageBoxW(window, Tr(L"请输入单独的有效凭据值，不包含 Cookie 列表、换行或其它请求头。"),
                            Tr(L"连接账号"), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            dialog->token = std::move(token);
            dialog->saved = true;
            SetWindowTextW(dialog->edit, L"");
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            SetWindowTextW(dialog->edit, L"");
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        SetWindowTextW(dialog->edit, L"");
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct BudgetDialogState {
    HWND daily = nullptr, monthly = nullptr, cny = nullptr;
    HFONT font = nullptr;
    double dailyValue = 0, monthlyValue = 0;
    bool cnyValue = false, done = false, saved = false;
};
bool ParseBudget(HWND edit, double& value) {
    auto text = EditText(edit);
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        value = 0;
        return true;
    }
    wchar_t* end = nullptr;
    value = wcstod(text.c_str() + first, &end);
    while (end && iswspace(*end)) ++end;
    return end && !*end && std::isfinite(value) && value >= 0 && value < 1e9;
}
std::wstring BudgetText(double value) {
    if (!(value > 0)) return {};
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"%.2f", value);
    return text;
}
LRESULT CALLBACK BudgetDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<BudgetDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<BudgetDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    switch (message) {
    case WM_CREATE: {
        DialogBuilder ui(window);
        ui.Add(L"STATIC", Tr(L"每日预算"), 0, 20, 22, 150, 26);
        dialog->daily =
            ui.Add(L"EDIT", BudgetText(dialog->dailyValue).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 180, 18, 200, 30, 60);
        ui.Add(L"STATIC", Tr(L"每月预算"), 0, 20, 66, 150, 26);
        dialog->monthly = ui.Add(L"EDIT", BudgetText(dialog->monthlyValue).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 180,
                                 62, 200, 30, 61);
        dialog->cny = ui.Add(L"BUTTON", Tr(L"按人民币 CNY 计算（否则为美元 USD）"), WS_TABSTOP | BS_AUTOCHECKBOX, 20,
                             106, 400, 28, 62);
        SendMessageW(dialog->cny, BM_SETCHECK, dialog->cnyValue ? BST_CHECKED : BST_UNCHECKED, 0);
        ui.Add(L"STATIC",
               Tr(L"留空或填 0 关闭该项提醒。达到 80% 和 100% 时各提醒一次。\n费用为标准 API 价格估算，不是订阅账单。"),
               0, 20, 144, 420, 74);
        ui.Add(L"BUTTON", Tr(L"保存"), WS_TABSTOP | BS_DEFPUSHBUTTON, 240, 232, 95, 32, IDOK);
        ui.Add(L"BUTTON", Tr(L"取消"), WS_TABSTOP, 345, 232, 95, 32, IDCANCEL);
        dialog->font = ui.Finish();
        SetFocus(dialog->daily);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            double daily = 0, monthly = 0;
            if (!ParseBudget(dialog->daily, daily) || !ParseBudget(dialog->monthly, monthly)) {
                MessageBoxW(window, Tr(L"请输入不小于 0 的金额，或留空关闭提醒。"), Tr(L"预算提醒"),
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            dialog->dailyValue = daily;
            dialog->monthlyValue = monthly;
            dialog->cnyValue = SendMessageW(dialog->cny, BM_GETCHECK, 0, 0) == BST_CHECKED;
            dialog->saved = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        dialog->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void EditBudget() {
    BudgetDialogState dialog;
    dialog.dailyValue = state.prefs.dailyBudget;
    dialog.monthlyValue = state.prefs.monthlyBudget;
    dialog.cnyValue =
        state.prefs.dailyBudget > 0 || state.prefs.monthlyBudget > 0 ? state.prefs.budgetCny : state.prefs.cny;
    RunModal(L"TokenScopeBudget", Tr(L"预算提醒"), 460, 282, &dialog, dialog.done);
    if (dialog.font) DeleteObject(dialog.font);
    if (!dialog.saved || shuttingDown) return;
    const bool changed = dialog.dailyValue != state.prefs.dailyBudget ||
                         dialog.monthlyValue != state.prefs.monthlyBudget || dialog.cnyValue != state.prefs.budgetCny;
    state.prefs.dailyBudget = dialog.dailyValue;
    state.prefs.monthlyBudget = dialog.monthlyValue;
    state.prefs.budgetCny = dialog.cnyValue;
    if (changed) {
        // New limits start a fresh alert cycle for the current day and month.
        IniWrite(L"BudgetAlertDay", L"");
        IniWrite(L"BudgetAlertMonth", L"");
    }
    state.notice =
        state.prefs.dailyBudget > 0 || state.prefs.monthlyBudget > 0 ? Tr(L"预算已保存。") : Tr(L"已关闭预算提醒。");
    SavePreferences();
    CheckBudgetAlerts();
}

void RegisterDialogClasses(WNDCLASSEXW klass) {
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const std::pair<const wchar_t*, WNDPROC> classes[] = {
        {L"TokenScopeRate", RateDialogProc},
        {L"TokenScopePrice", PriceDialogProc},
        {L"TokenScopeImport", ImportDialogProc},
        {L"TokenScopeSourcePath", SourcePathDialogProc},
        {L"TokenScopeConnection", ConnectionDialogProc},
        {L"TokenScopeBudget", BudgetDialogProc},
    };
    for (const auto& [name, procedure] : classes) {
        klass.lpszClassName = name;
        klass.lpfnWndProc = procedure;
        RegisterClassExW(&klass);
    }
}

} // namespace tokenscope::shell
