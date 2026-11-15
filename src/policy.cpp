#include "policy.hpp"
#include <algorithm>
#include <cwctype>

namespace dtm {
bool validThresholds(const Thresholds& t) {
    return t.yellow >= -20 && t.yellow < t.orange && t.orange <= t.red && t.red <= 125;
}
Level temperatureLevel(int c, const Thresholds& t) {
    if (c == kUnknownTemperature || c < -60 || c > 150) return Level::Unknown;
    if (c > t.red) return Level::Red;
    if (c >= t.orange) return Level::Orange;
    if (c >= t.yellow) return Level::Yellow;
    return Level::Green;
}
bool aboveAlert(int c, const Thresholds& t) {
    return t.alert && c != kUnknownTemperature && c >= -60 && c <= 150 && c > t.red;
}
bool belowAlert(int c, const Thresholds& t) {
    return c != kUnknownTemperature && c >= -60 && c <= 150 && c < t.red;
}
bool AlertState::observe(int c, const Thresholds& t) {
    hasReading = c != kUnknownTemperature && c >= -60 && c <= 150;
    if (!t.alert || belowAlert(c, t)) {
        active = false;
        dismissed = false;
        return false;
    }
    if (!hasReading) {
        // Preserve the episode for rapid retries without displaying a stale value
        // or re-arming an acknowledged episode before a known cool reading.
        return false;
    }
    if (!active && aboveAlert(c, t)) {
        active = true;
        return !dismissed;
    }
    return false;
}

std::wstring shortDriveName(std::wstring_view name, unsigned number) {
    // Prefer an identifying model token over generic transport/type words.
    std::vector<std::wstring> tokens;
    std::wstring token;
    for (wchar_t c : name) {
        if (std::iswalnum(c)) token += c;
        else if (!token.empty()) { tokens.push_back(token); token.clear(); }
    }
    if (!token.empty()) tokens.push_back(token);
    auto generic = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towupper(c); });
        return s == L"SSD" || s == L"HDD" || s == L"NVME" || s == L"SATA" || s == L"USB"
            || s == L"ATA" || s == L"DISK" || s == L"DEVICE" || s == L"SCSI"
            || s == L"PCIE" || s == L"TB" || s == L"GB";
    };
    auto capacity = [](const std::wstring& s) {
        if (s.size() < 3) return false;
        auto suffix = s.substr(s.size() - 2);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](wchar_t c) { return std::towupper(c); });
        return (suffix == L"TB" || suffix == L"GB") &&
            std::all_of(s.begin(), s.end()-2, [](wchar_t c) { return std::iswdigit(c) != 0; });
    };
    for (const auto& s : tokens) {
        bool letter = false, digit = false;
        for (auto c : s) { letter |= std::iswalpha(c) != 0; digit |= std::iswdigit(c) != 0; }
        if (letter && digit && !generic(s) && !capacity(s)) return s.substr(0, 10);
    }
    // Numeric model identifiers, e.g. Samsung 990 PRO, are useful when no mixed token exists.
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& s = tokens[i];
        if (s.size() >= 3 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return std::iswdigit(c) != 0; })) {
            std::wstring result = s;
            if (i+1 < tokens.size() && !generic(tokens[i+1]) && !capacity(tokens[i+1]))
                result += tokens[i+1];
            return result.substr(0,10);
        }
    }
    for (const auto& s : tokens) {
        if (!generic(s) && !capacity(s) && std::any_of(s.begin(), s.end(), [](wchar_t c) { return std::iswalpha(c) != 0; }))
            return s.substr(0, 10);
    }
    return (L"Drive" + std::to_wstring(number)).substr(0, 10);
}
}
