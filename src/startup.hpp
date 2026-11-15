#pragma once
#include <string>
namespace dtm::startup {
bool isEnabled();
bool setEnabled(bool enable, std::wstring& error);
bool isElevated();
std::wstring executablePath();
}
