#include "shell/shell.h"

namespace tokenscope::shell {
namespace {

// Highest alert level already shown for a period, persisted as "<period>|<level>" so a restart
// does not repeat the balloon. Level 1 = 80%, 2 = 100%.
int AlertedLevel(const wchar_t* key, const std::string& period) {
    const auto stored = WideToUtf8(IniString(key));
    const auto bar = stored.find('|');
    if (bar == std::string::npos || stored.substr(0, bar) != period) return 0;
    return std::clamp(std::atoi(stored.c_str() + bar + 1), 0, 2);
}
void RememberLevel(const wchar_t* key, const std::string& period, int level) {
    IniWrite(key, Utf8ToWide(period + "|" + std::to_string(level)));
}
int LevelFor(double spent, double limit) {
    if (!(limit > 0)) return 0;
    if (spent >= limit) return 2;
    if (spent >= limit * 0.8) return 1;
    return 0;
}
std::wstring Money(double value) {
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"%ls%.2f", state.prefs.budgetCny ? L"¥" : L"$", value);
    return text;
}

} // namespace

void CheckBudgetAlerts() {
    if (state.collecting || (state.prefs.dailyBudget <= 0 && state.prefs.monthlyBudget <= 0)) return;
    const auto budget = ComputeBudget(state.snapshot, state.prefs);
    if (!budget.available) return;
    const auto today = LocalToday();
    const auto month = today.substr(0, 7);
    struct Check {
        const wchar_t* key;
        std::string period;
        double spent, limit;
        const wchar_t *nearTitle, *overTitle;
    };
    const Check checks[] = {
        {L"BudgetAlertDay", today, budget.today, state.prefs.dailyBudget, Tr(L"今日预估费用接近预算"),
         Tr(L"今日预估费用已超出预算")},
        {L"BudgetAlertMonth", month, budget.month, state.prefs.monthlyBudget, Tr(L"本月预估费用接近预算"),
         Tr(L"本月预估费用已超出预算")},
    };
    for (const auto& check : checks) {
        const int level = LevelFor(check.spent, check.limit);
        if (level <= AlertedLevel(check.key, check.period)) continue;
        RememberLevel(check.key, check.period, level);
        ShowTrayBalloon(level == 2 ? check.overTitle : check.nearTitle,
                        TrF(L"已用 {0}，预算 {1}（按标准 API 价格估算）。", Money(check.spent), Money(check.limit)));
        return; // One balloon at a time; the next check shows any remaining alert.
    }
}

} // namespace tokenscope::shell
