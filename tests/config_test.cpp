#include "config.hpp"
#include <windows.h>
#include <cstdlib>
#include <iostream>
using namespace dtm;
static unsigned tests = 0;
static void require(bool ok, const char* name) {
    ++tests;
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
int main() {
    wchar_t base[MAX_PATH]{}, unique[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH,base) > 0, "temporary path");
    require(GetTempFileNameW(base,L"dtm",0,unique) != 0, "unique temporary path");
    DeleteFileW(unique);
    require(CreateDirectoryW(unique,nullptr) != FALSE, "create isolated config folder");
    require(SetEnvironmentVariableW(L"DRIVETEMPMON_CONFIG_DIR",unique) != FALSE, "isolate settings");
    Settings s = loadSettings();
    require(s.pollSeconds == 30 && s.hotSeconds == 3 && s.speechGapSeconds == 5, "missing file gets defaults");
    s.pollSeconds = 45; s.hotSeconds = 2; s.speechGapSeconds = 7;
    s.beeps = false; s.speech = true; s.defaults = {30,40,55,true};
    const std::wstring first = L"Samsung-\u0394-serial-1";
    const std::wstring second = L"Samsung-\u0394-serial-2";
    s.drives[first] = {31,41,51,false};
    s.drives[second] = {36,46,56,true};
    s.nicknames[first] = L"\u0623\u0644\u0639\u0627\u0628 & [SSD]";
    s.nicknames[L"new-drive"] = L"Backups";
    s.nicknames[L"empty-nickname"] = L"";
    std::wstring error;
    require(saveSettings(s,&error), "atomic save succeeds");
    Settings read = loadSettings();
    require(read.pollSeconds == 45 && read.hotSeconds == 2 && read.speechGapSeconds == 7, "intervals persist");
    require(!read.beeps && read.speech, "audio settings persist");
    require(read.defaults.red == 55 && read.defaults.yellow == 30, "defaults persist");
    require(read.drives.size() == 2, "two same-model identities survive");
    require(forDrive(read,first).red == 51 && !forDrive(read,first).alert, "unicode identity and alert persist");
    require(forDrive(read,second).red == 56 && forDrive(read,second).alert, "distinct drive override persists");
    require(forDrive(read,L"new-drive").red == 55, "new drive inherits defaults");
    require(read.nicknames.size() == 2 && read.nicknames.at(first) == s.nicknames.at(first), "unicode nickname persists; empty nickname omitted");
    require(read.nicknames.at(L"new-drive") == L"Backups" && read.drives.count(L"new-drive") == 0, "nickname does not create a threshold override");
    Settings invalid = read;
    invalid.hotSeconds = invalid.pollSeconds + 1;
    require(!saveSettings(invalid,&error) && !error.empty(), "reject invalid intervals");
    require(loadSettings().hotSeconds == 2, "failed save leaves previous file intact");
    invalid = read; invalid.drives[first].orange = 10;
    require(!saveSettings(invalid,&error), "reject bad per-drive threshold ordering");
    require(portableFirstLaunch(), "first portable launch detected");
    markPortableLaunched();
    require(!portableFirstLaunch(), "portable launch marker persists");
    read.defaults.red = 57;
    read.nicknames.erase(first);
    require(saveSettings(read,&error) && !portableFirstLaunch(), "saving settings preserves launch marker");
    require(loadSettings().defaults.red == 57, "replacement readable after earlier reads");
    const auto renamed = loadSettings();
    require(renamed.nicknames.count(first) == 0 && renamed.drives.at(first).red == 51, "clearing nickname preserves drive override");
    require(renamed.nicknames.at(L"new-drive") == L"Backups" && forDrive(renamed,L"new-drive").red == 57, "nicknamed drive still follows updated defaults");
    DeleteFileW(settingsPath().c_str());
    const std::wstring seen = std::wstring(unique) + L"\\portable-seen.ini";
    DeleteFileW(seen.c_str());
    require(RemoveDirectoryW(unique) != FALSE, "test leaves no files behind");
    SetEnvironmentVariableW(L"DRIVETEMPMON_CONFIG_DIR",nullptr);
    std::cout << tests << " configuration assertions passed\n";
}
