#pragma once
#include "model.hpp"
#include <string_view>
namespace dtm {
bool validThresholds(const Thresholds&);
Level temperatureLevel(int celsius, const Thresholds&);
bool aboveAlert(int celsius, const Thresholds&);
bool belowAlert(int celsius, const Thresholds&);
std::wstring shortDriveName(std::wstring_view name, unsigned number);
struct AlertState {
    bool active = false;
    bool dismissed = false;
    bool hasReading = true;
    bool observe(int celsius, const Thresholds&);
    void dismiss() { dismissed = true; }
    bool visible() const { return active && !dismissed && hasReading; }
};
}
