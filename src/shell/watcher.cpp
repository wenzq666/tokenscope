#include "shell/shell.h"

namespace tokenscope::shell {
namespace {

struct WatchTarget {
    std::wstring directory;
    bool recursive = true;
};

// One worker thread per group of handles; WaitForMultipleObjects accepts at most 64 including
// the stop event.
constexpr size_t kHandlesPerThread = MAXIMUM_WAIT_OBJECTS - 1;
constexpr size_t kMaximumTargets = kHandlesPerThread * 4;

HANDLE stopEvent = nullptr;
std::vector<std::thread> watchers;
std::vector<WatchTarget> watched;

bool IsWithin(const std::wstring& child, const std::wstring& parent) {
    if (child.size() < parent.size() || _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) != 0) return false;
    return child.size() == parent.size() || child[parent.size()] == L'\\' || parent.back() == L'\\';
}

std::vector<WatchTarget> CollectTargets() {
    std::vector<WatchTarget> targets;
    for (const auto& source : state.snapshot.sources)
        for (const auto& candidate : source.paths) {
            if (!candidate.exists || candidate.path.empty()) continue;
            std::error_code error;
            const fs::path path = fs::path(candidate.path).lexically_normal();
            if (fs::is_directory(path, error))
                targets.push_back({path.wstring(), true});
            else if (!error && path.has_parent_path())
                targets.push_back({path.parent_path().wstring(), false});
        }
    // Recursive watches first so nested targets can be dropped.
    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        if (left.recursive != right.recursive) return left.recursive;
        return left.directory.size() < right.directory.size();
    });
    std::vector<WatchTarget> unique;
    for (const auto& target : targets) {
        const bool covered = std::any_of(unique.begin(), unique.end(), [&](const WatchTarget& existing) {
            return existing.recursive
                       ? IsWithin(target.directory, existing.directory)
                       : !target.recursive && _wcsicmp(existing.directory.c_str(), target.directory.c_str()) == 0;
        });
        if (!covered) unique.push_back(target);
        if (unique.size() == kMaximumTargets) break; // The fallback poll still covers the rest.
    }
    return unique;
}

void WatchGroup(std::vector<WatchTarget> targets, HANDLE stop, HWND window) {
    std::vector<HANDLE> handles{stop};
    for (const auto& target : targets) {
        HANDLE change = FindFirstChangeNotificationW(target.directory.c_str(), target.recursive,
                                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change != INVALID_HANDLE_VALUE) handles.push_back(change);
    }
    for (;;) {
        const DWORD result =
            WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        const DWORD index = result - WAIT_OBJECT_0;
        if (index >= handles.size()) break;
        PostMessageW(window, WM_SOURCES_CHANGED, 0, 0);
        if (!FindNextChangeNotification(handles[index])) {
            FindCloseChangeNotification(handles[index]);
            handles.erase(handles.begin() + index);
            if (handles.size() == 1) break;
        }
    }
    for (size_t i = 1; i < handles.size(); ++i) FindCloseChangeNotification(handles[i]);
}

bool SameTargets(const std::vector<WatchTarget>& left, const std::vector<WatchTarget>& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& a, const auto& b) {
               return a.recursive == b.recursive && _wcsicmp(a.directory.c_str(), b.directory.c_str()) == 0;
           });
}

} // namespace

void RestartSourceWatcher() {
    if (shuttingDown || !mainWindow) return;
    auto targets = CollectTargets();
    if (stopEvent && SameTargets(targets, watched)) return;
    StopSourceWatcher();
    watched = targets;
    if (targets.empty()) return;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return;
    for (size_t offset = 0; offset < targets.size(); offset += kHandlesPerThread) {
        std::vector<WatchTarget> group(
            targets.begin() + static_cast<std::ptrdiff_t>(offset),
            targets.begin() + static_cast<std::ptrdiff_t>(std::min(targets.size(), offset + kHandlesPerThread)));
        watchers.emplace_back(WatchGroup, std::move(group), stopEvent, mainWindow);
    }
}

void StopSourceWatcher() {
    if (stopEvent) SetEvent(stopEvent);
    for (auto& watcher : watchers)
        if (watcher.joinable()) watcher.join();
    watchers.clear();
    watched.clear();
    if (stopEvent) CloseHandle(stopEvent);
    stopEvent = nullptr;
}

bool SourceWatcherActive() {
    return stopEvent != nullptr;
}

void OnSourcesChanged() {
    if (!state.prefs.autoRefresh || pauseCollection || shuttingDown) return;
    watchRefreshPending = true;
    if (state.collecting) return; // WM_IMPORTED calls back once the current scan finishes.
    // Settle bursts of writes, and never start scans closer together than kMinimumScanSpacing.
    const ULONGLONG since = GetTickCount64() - lastCollectionStart;
    const ULONGLONG delay = std::max<ULONGLONG>(1500, since < kMinimumScanSpacing ? kMinimumScanSpacing - since : 0);
    SetTimer(mainWindow, TIMER_WATCH, static_cast<UINT>(delay), nullptr);
}

void OnWatchTimer() {
    KillTimer(mainWindow, TIMER_WATCH);
    if (!watchRefreshPending) return;
    if (state.collecting || priceDialogActive) return; // Retried from WM_IMPORTED / the next change.
    watchRefreshPending = false;
    if (state.prefs.autoRefresh && !pauseCollection) StartCollection();
}

} // namespace tokenscope::shell
