#pragma once
#include <string>

namespace tokenscope {

// The calendar is anchored to the local current day, independently of the
// selected usage day or the trend chart's navigation position.
struct HeatmapCalendar {
    std::string first;
    std::string last;
    std::string gridFirst;
    int days = 0;
    int columns = 0;
};

HeatmapCalendar MakeHeatmapCalendar(const std::string& today, int columns);

} // namespace tokenscope
