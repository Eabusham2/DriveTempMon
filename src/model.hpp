#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace dtm {
enum class DriveKind { Unknown, HDD, SSD };
enum class Level { Unknown, Green, Yellow, Orange, Red };
struct Thresholds {
    int yellow = 35;
    int orange = 45;
    int red = 50;
    bool alert = true;
};
struct Settings {
    unsigned pollSeconds = 30;
    unsigned hotSeconds = 3;
    unsigned speechGapSeconds = 5;
    bool beeps = true;
    bool speech = true;
    Thresholds defaults;
    std::map<std::wstring, Thresholds> drives;
};
struct Drive {
    std::wstring id;
    std::wstring name;
    std::wstring shortName;
    std::wstring path;
    DriveKind kind = DriveKind::Unknown;
    unsigned number = 0;
    int celsius = -1000;
    std::wstring status;
    bool sleeping = false;
    std::uint64_t sampledAtMs = 0;
};
inline constexpr int kUnknownTemperature = -1000;
}
