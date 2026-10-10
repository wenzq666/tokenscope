#include "shell/shell.h"

namespace tokenscope::shell {

HINSTANCE instance = nullptr;
HWND mainWindow = nullptr, tooltipWindow = nullptr;
UINT taskbarCreated = 0;
ViewState state;
Store store;
std::wstring dataDirectory, settingsPath, importCodex, importClaude;
std::vector<std::pair<std::string, std::wstring>> configuredImports;
std::thread collector, networkWorker, exchangeWorker, connectionWorker;
std::atomic_bool cancelled{false}, networkBusy{false}, exchangeBusy{false}, connectionBusy{false};
std::map<std::string, ConnectionState> connectionStates;
std::string exchangeFetchedDay;
ULONGLONG exchangeLastAttempt = 0;
NOTIFYICONDATAW trayIcon{};
int keyboardHit = -1;
bool shuttingDown = false, pauseCollection = false, priceDialogActive = false, importRefreshPending = false;
std::wstring lastDay;
ULONGLONG lastCollectionStart = 0;
bool watchRefreshPending = false;

} // namespace tokenscope::shell
