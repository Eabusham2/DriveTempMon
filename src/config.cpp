#include "config.hpp"
#include "policy.hpp"
#include "startup.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <sstream>
#include <iomanip>

namespace dtm {
namespace {
std::wstring configDirectory() {
    wchar_t overridePath[32768]{};
    const auto n = GetEnvironmentVariableW(L"DRIVETEMPMON_CONFIG_DIR", overridePath, 32768);
    if (n && n < 32768) return overridePath;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
        std::wstring result = std::wstring(path) + L"\\DriveTempMon";
        CoTaskMemFree(path);
        return result;
    }
    // An absolute user temp path is preferable to silently writing beside an installed EXE.
    wchar_t temp[32768]{};
    const DWORD count = GetTempPathW(32768, temp);
    return (count && count < 32768 ? std::wstring(temp) : L".\\") + L"DriveTempMon";
}
std::wstring hashKey(const std::wstring& value) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (wchar_t c : value) {
        hash ^= static_cast<unsigned char>(c & 255);
        hash *= UINT64_C(1099511628211);
        hash ^= static_cast<unsigned char>((c >> 8) & 255);
        hash *= UINT64_C(1099511628211);
    }
    wchar_t result[17]{};
    swprintf_s(result, L"%016llx", static_cast<unsigned long long>(hash));
    return result;
}
std::wstring toHex(const std::wstring& value) {
    constexpr wchar_t alphabet[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(value.size()*4);
    for (wchar_t c : value)
        for (int shift = 12; shift >= 0; shift -= 4) result += alphabet[(c >> shift) & 15];
    return result;
}
std::wstring fromHex(const std::wstring& value) {
    if (value.size() % 4 != 0 || value.size() > 8192) return {};
    std::wstring result;
    for (std::size_t i = 0; i < value.size(); i += 4) {
        unsigned c = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto x = value[i+j];
            const int digit = x >= L'0' && x <= L'9' ? x-L'0' : x >= L'a' && x <= L'f' ? x-L'a'+10 : -1;
            if (digit < 0) return {};
            c = (c << 4) | static_cast<unsigned>(digit);
        }
        if (!c) return {};
        result += static_cast<wchar_t>(c);
    }
    return result;
}
int number(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& file) {
    wchar_t value[32]{};
    if (!GetPrivateProfileStringW(section, key, L"", value, 32, file.c_str())) return fallback;
    wchar_t* end = nullptr;
    const long parsed = wcstol(value, &end, 10);
    if (!end || *end != L'\0' || parsed < -100000 || parsed > 100000) return fallback;
    return static_cast<int>(parsed);
}
Thresholds readThresholds(const wchar_t* section, const std::wstring& file, Thresholds fallback) {
    Thresholds t{
        number(section,L"Yellow",fallback.yellow,file),
        number(section,L"Orange",fallback.orange,file),
        number(section,L"Red",fallback.red,file),
        number(section,L"Alerts",fallback.alert ? 1 : 0,file) != 0
    };
    return validThresholds(t) ? t : fallback;
}
std::wstring errorMessage(DWORD code) {
    wchar_t* message = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = n && message ? message : L"Windows error " + std::to_wstring(code);
    if (message) LocalFree(message);
    return result;
}
bool ensureDirectory(std::wstring* error) {
    const auto directory = configDirectory();
    const int status = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (status == ERROR_SUCCESS || status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) return true;
    if (error) *error = L"Could not create settings folder: " + errorMessage(static_cast<DWORD>(status));
    return false;
}
void appendThresholds(std::wostringstream& out, const Thresholds& t) {
    out << L"Yellow=" << t.yellow << L"\r\nOrange=" << t.orange << L"\r\nRed=" << t.red
        << L"\r\nAlerts=" << (t.alert ? 1 : 0) << L"\r\n";
}
}

std::wstring settingsPath() { return configDirectory() + L"\\settings.ini"; }
Settings loadSettings() {
    Settings s;
    const auto file = settingsPath();
    const auto p = number(L"General",L"PollSeconds",30,file);
    const auto h = number(L"General",L"HotSeconds",3,file);
    const auto gap = number(L"General",L"SpeechGapSeconds",5,file);
    s.pollSeconds = p >= 1 && p <= 3600 ? static_cast<unsigned>(p) : 30;
    s.hotSeconds = h >= 1 && h <= 60 ? static_cast<unsigned>(h) : 3;
    s.hotSeconds = std::min(s.hotSeconds, s.pollSeconds);
    s.speechGapSeconds = gap >= 1 && gap <= 300 ? static_cast<unsigned>(gap) : 5;
    s.beeps = number(L"General",L"Beeps",1,file) != 0;
    s.speech = number(L"General",L"Speech",1,file) != 0;
    s.defaults = readThresholds(L"Defaults",file,s.defaults);
    std::vector<wchar_t> sections(32768,0);
    const auto count = GetPrivateProfileSectionNamesW(sections.data(),static_cast<DWORD>(sections.size()),file.c_str());
    if (count >= sections.size()-2) return s;
    for (const wchar_t* pSection = sections.data(); *pSection; pSection += wcslen(pSection)+1) {
        if (wcsncmp(pSection,L"Drive_",6) != 0 || s.drives.size() >= 256) continue;
        wchar_t hex[8193]{};
        GetPrivateProfileStringW(pSection,L"IdHex",L"",hex,8193,file.c_str());
        const auto id = fromHex(hex);
        if (!id.empty()) s.drives[id] = readThresholds(pSection,file,s.defaults);
    }
    return s;
}
bool saveSettings(const Settings& s, std::wstring* error) {
    if (!validThresholds(s.defaults) || s.pollSeconds < 1 || s.pollSeconds > 3600 ||
        s.hotSeconds < 1 || s.hotSeconds > 60 || s.hotSeconds > s.pollSeconds ||
        s.speechGapSeconds < 1 || s.speechGapSeconds > 300 || s.drives.size() > 256) {
        if (error) *error = L"Invalid temperatures or intervals.";
        return false;
    }
    for (const auto& entry : s.drives) {
        if (!validThresholds(entry.second) || entry.first.empty() || entry.first.size() > 2048) {
            if (error) *error = L"Invalid drive settings.";
            return false;
        }
    }
    if (!ensureDirectory(error)) return false;
    std::wostringstream out;
    out << L"[General]\r\nPollSeconds=" << s.pollSeconds << L"\r\nHotSeconds=" << s.hotSeconds
        << L"\r\nSpeechGapSeconds=" << s.speechGapSeconds << L"\r\nBeeps=" << (s.beeps ? 1 : 0)
        << L"\r\nSpeech=" << (s.speech ? 1 : 0) << L"\r\n\r\n[Defaults]\r\n";
    appendThresholds(out,s.defaults);
    for (const auto& entry : s.drives) {
        out << L"\r\n[Drive_" << hashKey(entry.first) << L"]\r\nIdHex=" << toHex(entry.first) << L"\r\n";
        appendThresholds(out,entry.second);
    }
    const auto contents = std::wstring(1,static_cast<wchar_t>(0xfeff)) + out.str();
    const auto target = settingsPath();
    const auto temporary = target + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"Could not save settings: " + errorMessage(GetLastError());
        return false;
    }
    const DWORD bytes = static_cast<DWORD>(contents.size()*sizeof(wchar_t));
    DWORD written = 0;
    bool ok = WriteFile(file,contents.data(),bytes,&written,nullptr) && written == bytes;
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    if (ok && !FlushFileBuffers(file)) { ok = false; code = GetLastError(); }
    CloseHandle(file);
    if (ok && !MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false; code = GetLastError();
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        if (error) *error = L"Could not save settings: " + errorMessage(code);
        return false;
    }
    // Invalidate Windows' profile cache after atomic replacement.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,target.c_str());
    return true;
}
Thresholds forDrive(const Settings& s, const std::wstring& id) {
    const auto found = s.drives.find(id);
    return found == s.drives.end() ? s.defaults : found->second;
}
bool portableMode() {
    const auto exe = startup::executablePath();
    const auto slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    const auto marker = exe.substr(0,slash+1) + L"installed.flag";
    const auto attributes = GetFileAttributesW(marker.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool portableFirstLaunch() {
    if (!portableMode()) return false;
    auto exe = startup::executablePath();
    std::transform(exe.begin(),exe.end(),exe.begin(),[](wchar_t c) { return std::towlower(c); });
    const auto file = configDirectory() + L"\\portable-seen.ini";
    return number(L"Launches",hashKey(exe).c_str(),0,file) != 1;
}
void markPortableLaunched() {
    if (!portableMode() || !ensureDirectory(nullptr)) return;
    auto exe = startup::executablePath();
    std::transform(exe.begin(),exe.end(),exe.begin(),[](wchar_t c) { return std::towlower(c); });
    const auto file = configDirectory() + L"\\portable-seen.ini";
    WritePrivateProfileStringW(L"Launches",hashKey(exe).c_str(),L"1",file.c_str());
}
}
