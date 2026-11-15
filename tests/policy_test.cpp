#include "policy.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace dtm;
static unsigned tests = 0;
static void require(bool ok, const char* name) {
    ++tests;
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
int main() {
    const Thresholds t;
    require(validThresholds(t), "valid defaults");
    require(!validThresholds({45,35,50,true}), "reject inverted thresholds");
    require(!validThresholds({35,45,151,true}), "reject unsafe numeric bound");
    require(temperatureLevel(34,t)==Level::Green, "green below 35");
    require(temperatureLevel(35,t)==Level::Yellow, "yellow begins at 35");
    require(temperatureLevel(44,t)==Level::Yellow, "yellow ends at 44");
    require(temperatureLevel(45,t)==Level::Orange, "orange begins at 45");
    require(temperatureLevel(50,t)==Level::Orange, "not red at exactly 50");
    require(temperatureLevel(51,t)==Level::Red, "red above 50");
    require(temperatureLevel(kUnknownTemperature,t)==Level::Unknown, "unknown stays unknown");
    require(temperatureLevel(200,t)==Level::Unknown, "reject corrupt hot reading");
    AlertState a;
    require(!a.observe(50,t) && !a.visible(), "no alert exactly at threshold");
    require(a.observe(51,t) && a.visible(), "new hot episode");
    require(!a.observe(58,t) && a.visible(), "no duplicate onset for sustained heat");
    require(!a.observe(50,t) && a.visible(), "hold active at exact threshold");
    a.dismiss();
    require(!a.visible(), "dismiss hides alert");
    require(!a.observe(60,t) && !a.visible(), "dismiss survives hotter samples");
    require(!a.observe(kUnknownTemperature,t) && !a.visible(), "read failure does not undo dismissal");
    require(!a.observe(60,t) && !a.visible(), "read recovery keeps dismissal");
    require(!a.observe(49,t) && !a.dismissed && !a.active, "known cool sample rearms");
    require(a.observe(51,t) && a.visible(), "next episode alerts");
    require(!a.observe(kUnknownTemperature,t) && !a.visible() && a.active, "unknown hides stale alert but retains rapid retry");
    require(!a.observe(56,t) && a.visible(), "reading recovery resumes same undismissed episode");
    Thresholds off=t; off.alert=false;
    require(!a.observe(65,off) && !a.visible(), "disabled per-device alert");
    Thresholds custom{25,30,40,true};
    require(temperatureLevel(41,custom)==Level::Red && aboveAlert(41,custom), "custom per-drive threshold");
    require(shortDriveName(L"Samsung SSD 990 PRO 4TB",0)==L"990PRO", "model token deterministic");
    require(shortDriveName(L"WDC WD100EMAZ-00WJTA0",1)==L"WD100EMAZ", "model identification");
    require(shortDriveName(L"Samsung VeryLongModelNumber12345",0).size()<=10, "speech max 10 chars");
    require(shortDriveName(L"",2)==L"Drive2", "empty name safe fallback");
    require(shortDriveName(L"USB SCSI Disk Device",2)==L"Drive2", "generic name safe fallback");
    std::cout << tests << " policy assertions passed\n";
}
