#pragma once
#include "model.hpp"
namespace dtm {
Settings loadSettings();
bool saveSettings(const Settings&, std::wstring* error = nullptr);
Thresholds forDrive(const Settings&, const std::wstring& id);
bool portableMode();
bool portableFirstLaunch();
void markPortableLaunched();
std::wstring settingsPath();
}
