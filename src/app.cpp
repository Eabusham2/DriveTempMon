#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dbt.h>
#include <sapi.h>
#include <mmreg.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "model.hpp"
#include "policy.hpp"
#include "config.hpp"
#include "startup.hpp"
#include "sensors.hpp"

namespace {
constexpr wchar_t kMainClass[] = L"DriveTempMon.Main";
constexpr wchar_t kPopupClass[] = L"DriveTempMon.TemperatureAlert";
constexpr wchar_t kSettingsClass[] = L"DriveTempMon.Settings";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSamplesMessage = WM_APP + 2;
constexpr UINT kSpeechMessage = WM_APP + 3;
constexpr UINT kSmokeMessage = WM_APP + 4;
constexpr UINT_PTR kNormalTimer = 1;
constexpr UINT_PTR kHotTimer = 2;
constexpr UINT_PTR kAlertTimer = 3;
constexpr UINT_PTR kSpeechTimer = 4;
constexpr UINT_PTR kRescanTimer = 5;
constexpr UINT_PTR kBenchmarkTimer = 6;
constexpr int kSettingsButton = 101;
constexpr int kDriveList = 102;
constexpr int kToTrayButton = 103;
constexpr int kQuitButton = 104;
constexpr int kStartupButton = 105;
constexpr int kMenuOpen = 1001;
constexpr int kMenuQuit = 1002;
constexpr int kMenuDriveBase = 2000;
constexpr int kSaveButton = 201;
constexpr int kCancelButton = 202;
constexpr int kDefaultsButton = 203;
constexpr COLORREF kInk = RGB(25, 35, 47);
constexpr COLORREF kMuted = RGB(94, 107, 122);
constexpr COLORREF kPanel = RGB(247, 249, 251);
constexpr COLORREF kRed = RGB(185, 28, 28);

std::uint64_t FileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool WriteUtf8File(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
                         && written == text.size();
    CloseHandle(file);
    return success;
}

void ClearSpeechEvent(SPEVENT& event) {
    if (event.lParam) {
        if (event.elParamType == SPET_LPARAM_IS_TOKEN || event.elParamType == SPET_LPARAM_IS_OBJECT)
            reinterpret_cast<IUnknown*>(event.lParam)->Release();
        else if (event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING)
            CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
    }
    event = {};
}

std::wstring TempText(const dtm::Drive& drive) {
    if (drive.celsius == dtm::kUnknownTemperature)
        return drive.sleeping ? L"Asleep" : L"Unavailable";
    return std::to_wstring(drive.celsius) + L" \u00b0C";
}

COLORREF LevelColor(dtm::Level level) {
    switch (level) {
    case dtm::Level::Green: return RGB(22, 163, 74);
    case dtm::Level::Yellow: return RGB(234, 179, 8);
    case dtm::Level::Orange: return RGB(234, 88, 12);
    case dtm::Level::Red: return RGB(220, 38, 38);
    default: return RGB(120, 130, 143);
    }
}

HBITMAP MakeColorBitmap(COLORREF color, int size, bool glyph) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap || !raw) return nullptr;
    auto* pixels = static_cast<std::uint32_t*>(raw);
    const double center = (size - 1) / 2.0;
    const double radius = size * 0.46;
    const std::uint32_t fill = 0xff000000u | (static_cast<std::uint32_t>(GetRValue(color)) << 16)
        | (static_cast<std::uint32_t>(GetGValue(color)) << 8) | GetBValue(color);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x - center;
            const double dy = y - center;
            std::uint32_t pixel = dx * dx + dy * dy <= radius * radius ? fill : 0;
            if (glyph && x >= size * 0.25 && x <= size * 0.72 &&
                y >= size * 0.28 && y <= size * 0.72) {
                const bool edge = x <= size * 0.29 || x >= size * 0.68 ||
                    y <= size * 0.32 || y >= size * 0.68 ||
                    (y >= size * 0.51 && y <= size * 0.55);
                if (edge) pixel = 0xffffffffu;
            }
            pixels[y * size + x] = pixel;
        }
    }
    return bitmap;
}

HICON MakeTrayIcon(COLORREF color) {
    HBITMAP bitmap = MakeColorBitmap(color, 32, true);
    if (!bitmap) return nullptr;
    std::array<unsigned char, 128> bits{};
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, bits.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = bitmap;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(bitmap);
    if (mask) DeleteObject(mask);
    return icon;
}

bool IsFullscreenApp(HWND ownWindow) {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN)
        return true;
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == ownWindow || foreground == GetDesktopWindow() ||
        foreground == GetShellWindow() || IsIconic(foreground)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t className[80]{};
    GetClassNameW(foreground, className, 80);
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0) return false;
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(foreground, &bounds) ||
        !GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
    const bool coversMonitor = bounds.left <= monitor.rcMonitor.left + 2 &&
        bounds.top <= monitor.rcMonitor.top + 2 && bounds.right >= monitor.rcMonitor.right - 2 &&
        bounds.bottom >= monitor.rcMonitor.bottom - 2;
    // Conservative fallback also protects modern borderless games. Windows exposes no
    // universal exclusive-mode notification; query only while an alert is active.
    return coversMonitor && !(style & WS_CAPTION);
}

class SensorWorker {
    struct State {
        HWND owner = nullptr;
        mutable std::mutex mutex;
        std::condition_variable changed;
        bool stopping = false;
        bool pending = false;
        bool all = false;
        bool rediscover = false;
        std::vector<std::wstring> ids;
        std::vector<dtm::Drive> latest;
        std::uint64_t polls = 0;
    };
public:
    ~SensorWorker() { stop(); }
    void start(HWND owner) {
        state_->owner = owner;
        thread_ = std::thread([state = state_] { run(state); });
        request(true);
    }
    void stop() {
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            state_->stopping = true;
        }
        state_->changed.notify_one();
        if (!thread_.joinable()) return;
        const HANDLE threadHandle = static_cast<HANDLE>(thread_.native_handle());
        const ULONGLONG deadline = GetTickCount64() + 5000;
        // Cancellation can race with a driver starting the next request, so retry it
        // while waiting. Never indefinitely block Quit on a broken storage driver.
        while (WaitForSingleObject(threadHandle, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
            CancelSynchronousIo(threadHandle);
            WaitForSingleObject(threadHandle, 100);
        }
        if (WaitForSingleObject(threadHandle, 0) == WAIT_OBJECT_0) thread_.join();
        else {
            // This thread owns only its shared State and sensor objects, never App
            // or SensorWorker. It cannot post after stopping. Process exit reclaims
            // an uncooperative driver request without accessing destroyed UI state.
            thread_.detach();
        }
    }
    void request(bool rediscover, const std::vector<std::wstring>& onlyIds = {}) {
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->stopping) return;
            state_->pending = true;
            state_->rediscover = state_->rediscover || rediscover;
            if (onlyIds.empty()) state_->all = true;
            for (const auto& id : onlyIds)
                if (std::find(state_->ids.begin(), state_->ids.end(), id) == state_->ids.end()) state_->ids.push_back(id);
        }
        state_->changed.notify_one();
    }
    std::vector<dtm::Drive> snapshot() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->latest;
    }
    std::uint64_t polls() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->polls;
    }
private:
    static void run(const std::shared_ptr<State>& state) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        dtm::Sensors sensors;
        for (;;) {
            bool rediscover = false;
            std::vector<std::wstring> ids;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->changed.wait(lock, [&state] { return state->pending || state->stopping; });
                if (state->stopping) break;
                rediscover = state->rediscover;
                if (!state->all) ids = std::move(state->ids);
                state->ids.clear();
                state->rediscover = state->all = state->pending = false;
            }
            std::vector<dtm::Drive> result;
            try { result = sensors.poll(rediscover, ids); }
            catch (...) {
                // Sensor failures are explicit unknown readings and can recover on
                // the next scheduled read; they never terminate the tray process.
                std::lock_guard<std::mutex> guard(state->mutex);
                result = state->latest;
                for (auto& drive : result) {
                    drive.celsius = dtm::kUnknownTemperature;
                    drive.status = L"Temperature read failed";
                }
            }
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                state->latest = std::move(result);
                ++state->polls;
                if (state->stopping) break;
                PostMessageW(state->owner, kSamplesMessage, 0, 0);
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::thread thread_;
};

struct App;
struct Popup {
    App* app = nullptr;
    std::wstring id;
    HWND window = nullptr;
    UINT dpi = 96;
};
struct SettingsWindow {
    App* app = nullptr;
    std::wstring id;
    bool global = false;
    HWND window = nullptr;
    std::array<HWND, 6> edits{};
    HWND alerts = nullptr;
    HWND beeps = nullptr;
    HWND speech = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
};

LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PopupProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);

struct App {
    HINSTANCE instance = nullptr;
    HWND main = nullptr;
    HWND list = nullptr;
    HWND settingsWindow = nullptr;
    HWND startupButton = nullptr;
    UINT dpi = 96;
    UINT taskbarCreated = 0;
    UINT quitMessage = 0;
    UINT showMessage = 0;
    std::array<HICON, 6> icons{};
    std::array<HBITMAP, 6> menuBitmaps{};
    HFONT normalFont = nullptr;
    HFONT boldFont = nullptr;
    HFONT titleFont = nullptr;
    HBRUSH whiteBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HBRUSH redBrush = nullptr;
    dtm::Settings settings;
    std::vector<dtm::Drive> drives;
    std::map<std::wstring, dtm::AlertState> alerts;
    std::map<std::wstring, std::unique_ptr<Popup>> popups;
    SensorWorker worker;
    ISpVoice* voice = nullptr;
    bool speechBusy = false;
    bool speechEventsWired = false;
    ULONG speechStream = 0;
    unsigned utteranceCount = 0;
    std::uint64_t lastSpeechEndedAt = 0;
    std::uint64_t lastSpeechStartedAt = 0;
    std::uint64_t lastSpeechGapMs = 0;
    IStream* speechTestOutput = nullptr;
    bool smokeSpeechEnabled = false;
    std::vector<std::wstring> spokenIds;
    bool fullscreen = false;
    int fullscreenOverride = -1;
    bool trayAdded = false;
    bool hotTimerRunning = false;
    bool alertTimerRunning = false;
    bool portable = false;
    bool smoke = false;
    bool benchmark = false;
    bool finishing = false;
    bool speechUnavailable = false;
    unsigned normalTicks = 0;
    unsigned benchmarkSeconds = 0;
    std::wstring outputPath;
    int exitCode = 0;
    std::uint64_t startTicks = 0;
    std::uint64_t startCpu = 0;
    std::wstring lastError;

    int px(int logical) const { return MulDiv(logical, static_cast<int>(dpi), 96); }
    HFONT font(int size, int weight) const {
        return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 72), 0, 0, 0, weight,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    void createFonts() {
        if (normalFont) DeleteObject(normalFont);
        if (boldFont) DeleteObject(boldFont);
        if (titleFont) DeleteObject(titleFont);
        normalFont = font(10, FW_NORMAL);
        boldFont = font(10, FW_SEMIBOLD);
        titleFont = font(18, FW_SEMIBOLD);
    }
    HWND button(const wchar_t* title, int id, int x, int y, int width, int height) {
        HWND control = CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            px(x), px(y), px(width), px(height), main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
        return control;
    }
    const dtm::Drive* find(const std::wstring& id) const {
        const auto iterator = std::find_if(drives.begin(), drives.end(), [&id](const dtm::Drive& d) { return d.id == id; });
        return iterator == drives.end() ? nullptr : &*iterator;
    }
    std::vector<std::wstring> hotIds() const {
        std::vector<std::wstring> ids;
        for (const auto& d : drives) {
            const auto found = alerts.find(d.id);
            const auto limits = dtm::forDrive(settings, d.id);
            // Keep rapid sampling after X as long as the temperature remains high.
            if (dtm::aboveAlert(d.celsius, limits) || (found != alerts.end() && found->second.active))
                ids.push_back(d.id);
        }
        return ids;
    }
    std::vector<const dtm::Drive*> visibleAlerts() const {
        std::vector<const dtm::Drive*> result;
        for (const auto& d : drives) {
            const auto iterator = alerts.find(d.id);
            if (iterator != alerts.end() && iterator->second.visible() && d.celsius != dtm::kUnknownTemperature)
                result.push_back(&d);
        }
        return result;
    }
    dtm::Level trayLevel() const {
        const dtm::Drive* hottest = nullptr;
        for (const auto& d : drives)
            if (d.celsius != dtm::kUnknownTemperature && (!hottest || d.celsius > hottest->celsius)) hottest = &d;
        return hottest ? dtm::temperatureLevel(hottest->celsius, dtm::forDrive(settings, hottest->id)) : dtm::Level::Unknown;
    }
    void updateTray(bool add = false) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = main;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = kTrayMessage;
        icon.hIcon = icons[static_cast<unsigned>(trayLevel())];
        std::wstring tip = L"DriveTempMon";
        const dtm::Drive* hottest = nullptr;
        for (const auto& d : drives)
            if (d.celsius != dtm::kUnknownTemperature && (!hottest || d.celsius > hottest->celsius)) hottest = &d;
        if (hottest) tip += L" \u00b7 " + hottest->name + L": " + TempText(*hottest);
        else tip += drives.empty() ? L" \u00b7 Looking for drives" : L" \u00b7 Temperature unavailable";
        wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);
        if (add || !trayAdded) {
            trayAdded = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
            if (trayAdded) {
                icon.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &icon);
            }
        } else if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) {
            trayAdded = false;
            updateTray(true);
        }
    }
    void show() {
        if (startupButton)
            SetWindowTextW(startupButton, dtm::startup::isEnabled() ? L"Remove from startup" : L"Add to startup");
        ShowWindow(main, IsIconic(main) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(main);
    }
    void hide() { ShowWindow(main, SW_HIDE); }
    void resetTimers() {
        if (smoke) return;
        SetTimer(main, kNormalTimer, std::max(1u, settings.pollSeconds) * 1000u, nullptr);
        const bool hot = !hotIds().empty();
        if (hot) SetTimer(main, kHotTimer, std::max(1u, settings.hotSeconds) * 1000u, nullptr);
        else KillTimer(main, kHotTimer);
        hotTimerRunning = hot;
    }
    void updateList() {
        const int selection = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& d : drives) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
        if (selection >= 0 && selection < static_cast<int>(drives.size()))
            SendMessageW(list, LB_SETCURSEL, selection, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(list, drives.empty() ? SW_HIDE : SW_SHOW);
        InvalidateRect(list, nullptr, TRUE);
        InvalidateRect(main, nullptr, FALSE);
    }
    void applyReadings() {
        for (const auto& d : drives) alerts[d.id].observe(d.celsius, dtm::forDrive(settings, d.id));
        for (auto iterator = alerts.begin(); iterator != alerts.end();) {
            if (!find(iterator->first)) iterator = alerts.erase(iterator);
            else ++iterator;
        }
        updateTray();
        updateList();
        const bool hot = !hotIds().empty();
        if (!smoke && hot != hotTimerRunning) {
            if (hot) SetTimer(main, kHotTimer, settings.hotSeconds * 1000u, nullptr);
            else KillTimer(main, kHotTimer);
            hotTimerRunning = hot;
        }
        updateAlerts(true);
    }
    void stopSpeech() {
        KillTimer(main, kSpeechTimer);
        if (voice) {
            voice->SetNotifySink(nullptr);
            voice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
            voice->Release();
            voice = nullptr;
        }
        speechBusy = false;
        speechEventsWired = false;
        speechStream = 0;
        spokenIds.clear();
    }
    void speak() {
        KillTimer(main, kSpeechTimer);
        if ((smoke && !smokeSpeechEnabled) || !fullscreen || !settings.speech || speechBusy) return;
        const auto candidates = visibleAlerts();
        if (candidates.empty()) return;
        if (!voice) {
            if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ISpVoice, reinterpret_cast<void**>(&voice)))) {
                speechUnavailable = true;
                lastError = L"Windows text-to-speech is unavailable. Install a Windows speech voice.";
                InvalidateRect(main, nullptr, FALSE);
                return;
            }
            if (smokeSpeechEnabled) {
                ISpStream* output = nullptr;
                HRESULT result = CreateStreamOnHGlobal(nullptr, TRUE, &speechTestOutput);
                if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER,
                    IID_ISpStream, reinterpret_cast<void**>(&output));
                WAVEFORMATEX format{};
                format.wFormatTag = WAVE_FORMAT_PCM;
                format.nChannels = 1;
                format.nSamplesPerSec = 22050;
                format.nAvgBytesPerSec = 44100;
                format.nBlockAlign = 2;
                format.wBitsPerSample = 16;
                if (SUCCEEDED(result)) result = output->SetBaseStream(speechTestOutput, SPDFID_WaveFormatEx, &format);
                if (SUCCEEDED(result)) result = voice->SetOutput(output, TRUE);
                if (output) output->Release();
                if (FAILED(result)) {
                    stopSpeech();
                    speechUnavailable = true;
                    return;
                }
            }
            const HRESULT notifyResult = voice->SetNotifyWindowMessage(main, kSpeechMessage, 0, 0);
            const ULONGLONG interest = SPFEI(SPEI_END_INPUT_STREAM);
            const HRESULT interestResult = voice->SetInterest(interest, interest);
            speechEventsWired = SUCCEEDED(notifyResult) && SUCCEEDED(interestResult);
        }
        std::wstring sentence;
        spokenIds.clear();
        for (const auto* d : candidates) {
            const wchar_t* kind = d->kind == dtm::DriveKind::SSD ? L"SSD" : d->kind == dtm::DriveKind::HDD ? L"HDD" : L"drive";
            std::wstring name = d->shortName.empty() ? dtm::shortDriveName(d->name, d->number) : d->shortName;
            if (name.size() > 10) name.resize(10);
            sentence += L"Your " + std::wstring(kind) + L" " + name + L" is " +
                        std::to_wstring(d->celsius) + L" degrees Celsius. ";
            spokenIds.push_back(d->id);
        }
        const HRESULT result = voice->Speak(sentence.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, &speechStream);
        speechBusy = SUCCEEDED(result);
        if (speechBusy) {
            ++utteranceCount;
            lastSpeechStartedAt = GetTickCount64();
            lastSpeechGapMs = lastSpeechEndedAt ? lastSpeechStartedAt - lastSpeechEndedAt : 0;
        }
        if (FAILED(result)) {
            lastError = L"Windows could not play the temperature speech alert.";
            speechUnavailable = true;
            InvalidateRect(main, nullptr, FALSE);
        }
    }
    void speechFinished() {
        if (!speechBusy) return;
        speechBusy = false;
        spokenIds.clear();
        lastSpeechEndedAt = GetTickCount64();
        if (fullscreen && settings.speech && !visibleAlerts().empty())
            SetTimer(main, kSpeechTimer, settings.speechGapSeconds * 1000u, nullptr);
    }
    void speechEvents() {
        if (!voice) return;
        SPEVENT event{};
        ULONG fetched = 0;
        while (SUCCEEDED(voice->GetEvents(1, &event, &fetched)) && fetched) {
            if (event.eEventId == SPEI_END_INPUT_STREAM && event.ulStreamNum == speechStream) {
                speechFinished();
            }
            ClearSpeechEvent(event);
            fetched = 0;
        }
    }
    void updateAlerts(bool readingsChanged = false) {
        const auto candidates = visibleAlerts();
        const bool active = !candidates.empty();
        if (active && !alertTimerRunning && !smoke) {
            SetTimer(main, kAlertTimer, 1000, nullptr);
            alertTimerRunning = true;
        } else if (!active && alertTimerRunning) {
            KillTimer(main, kAlertTimer);
            alertTimerRunning = false;
        }
        const bool nowFullscreen = active && (fullscreenOverride >= 0 ? fullscreenOverride != 0 : IsFullscreenApp(main));
        const bool changedMode = nowFullscreen != fullscreen;
        fullscreen = nowFullscreen;
        if (!fullscreen || !settings.speech || !active) stopSpeech();
        else if (voice && speechBusy) {
            const bool stale = std::any_of(spokenIds.begin(), spokenIds.end(), [&](const std::wstring& id) {
                return std::none_of(candidates.begin(), candidates.end(), [&](const dtm::Drive* d) { return d->id == id; });
            });
            if (stale) stopSpeech();
            else {
                SPVOICESTATUS status{};
                // The normal path is the exact SAPI completion event. This fallback
                // also recovers if a voice cannot deliver notification messages.
                if (SUCCEEDED(voice->GetStatus(&status, nullptr)) && status.dwRunningState == SPRS_DONE)
                    speechFinished();
            }
        }
        for (auto iterator = popups.begin(); iterator != popups.end();) {
            const bool wanted = !fullscreen && std::any_of(candidates.begin(), candidates.end(), [&](const dtm::Drive* d) { return d->id == iterator->first; });
            if (!wanted) {
                if (iterator->second->window) DestroyWindow(iterator->second->window);
                iterator = popups.erase(iterator);
            } else ++iterator;
        }
        bool created = false;
        if (!fullscreen) {
            for (const auto* d : candidates) {
                auto iterator = popups.find(d->id);
                if (iterator == popups.end()) {
                    auto popup = std::make_unique<Popup>();
                    popup->app = this;
                    popup->id = d->id;
                    popup->dpi = dpi;
                    popup->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        kPopupClass, (L"DriveTempMon: " + d->name + L" " + TempText(*d)).c_str(), WS_POPUP,
                        0, 0, px(360), px(112), nullptr, nullptr, instance, popup.get());
                    if (popup->window) { popups.emplace(d->id, std::move(popup)); created = true; }
                } else if (readingsChanged) {
                    SetWindowTextW(iterator->second->window, (L"DriveTempMon: " + d->name + L" " + TempText(*d)).c_str());
                    InvalidateRect(iterator->second->window, nullptr, FALSE);
                }
            }
            if (created || changedMode) arrangePopups();
        }
        if (created && settings.beeps && !smoke) MessageBeep(MB_ICONHAND);
        if (fullscreen && settings.speech && !speechBusy && !voice && !speechUnavailable) speak();
        else if (fullscreen && changedMode && settings.speech && !speechBusy) speak();
    }
    void arrangePopups() {
        if (popups.empty()) return;
        MONITORINFO monitor{sizeof(monitor)};
        HWND reference = GetForegroundWindow();
        if (!reference) reference = main;
        GetMonitorInfoW(MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = px(360), height = px(112), gap = px(10);
        int x = monitor.rcWork.right - width - gap;
        int y = monitor.rcWork.bottom - height - gap;
        for (auto& entry : popups) {
            if (y < monitor.rcWork.top + gap) { y = monitor.rcWork.bottom - height - gap; x -= width + gap; }
            x = std::max(x, monitor.rcWork.left + gap);
            SetWindowPos(entry.second->window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            y -= height + gap;
        }
    }
    void dismiss(const std::wstring& id) {
        auto iterator = alerts.find(id);
        if (iterator != alerts.end()) iterator->second.dismiss();
        updateAlerts();
        InvalidateRect(list, nullptr, FALSE);
    }
    void showSettings(const std::wstring& id = {});
    void contextMenu() {
        HMENU menu = CreatePopupMenu();
        std::vector<std::wstring> menuIds;
        AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open");
        SetMenuDefaultItem(menu, kMenuOpen, FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (drives.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No drives detected yet");
        for (std::size_t i = 0; i < drives.size(); ++i) {
            const auto& d = drives[i];
            menuIds.push_back(d.id);
            const auto level = dtm::temperatureLevel(d.celsius, dtm::forDrive(settings, d.id));
            std::wstring name = d.name + L"\t" + TempText(d);
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
            item.wID = kMenuDriveBase + static_cast<UINT>(i);
            item.dwTypeData = name.data();
            item.hbmpItem = menuBitmaps[static_cast<unsigned>(level)];
            InsertMenuItemW(menu, static_cast<UINT>(-1), TRUE, &item);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuQuit, L"Close");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(main);
        const int choice = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                            point.x, point.y, main, nullptr);
        DestroyMenu(menu);
        PostMessageW(main, WM_NULL, 0, 0);
        if (choice == kMenuOpen) show();
        else if (choice == kMenuQuit) DestroyWindow(main);
        else if (choice >= kMenuDriveBase && choice < kMenuDriveBase + static_cast<int>(menuIds.size()))
            showSettings(menuIds[static_cast<std::size_t>(choice - kMenuDriveBase)]);
    }
    void drawMain(HDC dc) {
        RECT client{};
        GetClientRect(main, &client);
        FillRect(dc, &client, whiteBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kInk);
        HFONT previous = static_cast<HFONT>(SelectObject(dc, titleFont));
        RECT title{px(20), px(18), px(320), px(54)};
        DrawTextW(dc, L"DriveTempMon", -1, &title, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, normalFont);
        SetTextColor(dc, kMuted);
        RECT subtitle{px(20), px(56), px(420), px(84)};
        DrawTextW(dc, L"Drive temperatures, at a glance.", -1, &subtitle, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        if (drives.empty()) {
            RECT empty{px(36), px(130), px(416), px(240)};
            DrawTextW(dc, worker.polls() ? L"No supported physical drives were found.\nExternal adapters and virtual disks may not expose temperature."
                : L"Reading drive temperatures\u2026", -1, &empty, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
        std::wstring status = std::to_wstring(drives.size()) + (drives.size() == 1 ? L" drive  \u00b7  " : L" drives  \u00b7  ") +
            std::to_wstring(settings.pollSeconds) + L" s normal / " + std::to_wstring(settings.hotSeconds) + L" s hot";
        RECT statusRect{px(20), px(324), px(432), px(348)};
        DrawTextW(dc, status.c_str(), -1, &statusRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT hint{px(20), px(348), px(432), px(382)};
        std::wstring message = lastError.empty() ? L"Click a drive to adjust its temperature limits." : lastError;
        DrawTextW(dc, message.c_str(), -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    void drawDrive(const DRAWITEMSTRUCT& item) {
        if (item.itemID >= drives.size()) return;
        const auto& d = drives[item.itemID];
        const auto limits = dtm::forDrive(settings, d.id);
        HBRUSH background = CreateSolidBrush(item.itemState & ODS_SELECTED ? RGB(232, 241, 251) : RGB(247, 249, 251));
        FillRect(item.hDC, &item.rcItem, background);
        DeleteObject(background);
        const auto color = LevelColor(dtm::temperatureLevel(d.celsius, limits));
        HBRUSH dot = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(item.hDC, dot);
        HGDIOBJ oldPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
        Ellipse(item.hDC, item.rcItem.left + px(12), item.rcItem.top + px(20),
                item.rcItem.left + px(23), item.rcItem.top + px(31));
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldPen);
        DeleteObject(dot);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, kInk);
        HGDIOBJ oldFont = SelectObject(item.hDC, boldFont);
        RECT name{item.rcItem.left + px(34), item.rcItem.top + px(8), item.rcItem.right - px(104), item.rcItem.top + px(29)};
        DrawTextW(item.hDC, d.name.c_str(), -1, &name, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT temp{item.rcItem.right - px(102), item.rcItem.top + px(8), item.rcItem.right - px(12), item.rcItem.top + px(29)};
        SetTextColor(item.hDC, d.celsius == dtm::kUnknownTemperature ? kMuted : color);
        const auto text = TempText(d);
        DrawTextW(item.hDC, text.c_str(), -1, &temp, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, normalFont);
        SetTextColor(item.hDC, kMuted);
        std::wstring info;
        if (d.celsius == dtm::kUnknownTemperature) info = d.status.empty() ? L"Temperature is not exposed by this drive" : d.status;
        else {
            info = d.kind == dtm::DriveKind::SSD ? L"SSD" : d.kind == dtm::DriveKind::HDD ? L"HDD" : L"Drive";
            info += L"  \u00b7  Alert above " + std::to_wstring(limits.red) + L" \u00b0C";
            const auto state = alerts.find(d.id);
            if (!limits.alert) info += L"  \u00b7  Alerts off";
            else if (state != alerts.end() && state->second.dismissed) info += L"  \u00b7  Dismissed until cool";
        }
        RECT details{item.rcItem.left + px(34), item.rcItem.top + px(31), item.rcItem.right - px(12), item.rcItem.bottom - px(4)};
        DrawTextW(item.hDC, info.c_str(), -1, &details, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, oldFont);
        if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
    }
    void cleanup() {
        if (finishing) return;
        finishing = true;
        worker.stop();
        stopSpeech();
        if (speechTestOutput) { speechTestOutput->Release(); speechTestOutput = nullptr; }
        for (auto& popup : popups) if (IsWindow(popup.second->window)) DestroyWindow(popup.second->window);
        popups.clear();
        if (settingsWindow && IsWindow(settingsWindow)) DestroyWindow(settingsWindow);
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = main;
        icon.uID = 1;
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &icon);
        trayAdded = false;
    }
    ~App() {
        cleanup();
        for (const auto icon : icons) if (icon) DestroyIcon(icon);
        for (const auto bitmap : menuBitmaps) if (bitmap) DeleteObject(bitmap);
        if (normalFont) DeleteObject(normalFont);
        if (boldFont) DeleteObject(boldFont);
        if (titleFont) DeleteObject(titleFont);
        if (whiteBrush) DeleteObject(whiteBrush);
        if (panelBrush) DeleteObject(panelBrush);
        if (redBrush) DeleteObject(redBrush);
    }
    void reportBenchmark() {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        FILETIME created{}, ended{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user);
        const auto cpu = FileTimeValue(kernel) + FileTimeValue(user) - startCpu;
        const auto elapsed = GetTickCount64() - startTicks;
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        const auto available = std::count_if(drives.begin(), drives.end(), [](const dtm::Drive& d) { return d.celsius != dtm::kUnknownTemperature; });
        std::ostringstream json;
        json << "{\n  \"mode\": \"real-sensors-hidden-tray\",\n"
             << "  \"elapsed_ms\": " << elapsed << ",\n"
             << "  \"cpu_ms\": " << cpu / 10000.0 << ",\n"
             << "  \"cpu_percent_of_one_core\": " << (elapsed ? cpu / 100.0 / elapsed : 0) << ",\n"
             << "  \"working_set_bytes\": " << memory.WorkingSetSize << ",\n"
             << "  \"peak_working_set_bytes\": " << memory.PeakWorkingSetSize << ",\n"
             << "  \"private_bytes\": " << memory.PrivateUsage << ",\n"
             << "  \"handle_count\": " << handles << ",\n"
             << "  \"sensor_poll_count\": " << worker.polls() << ",\n"
             << "  \"drive_count\": " << drives.size() << ",\n"
             << "  \"temperature_drive_count\": " << available << ",\n"
             << "  \"normal_interval_seconds\": " << settings.pollSeconds << ",\n"
             << "  \"hot_interval_seconds\": " << settings.hotSeconds << ",\n"
             << "  \"tray_added\": " << (trayAdded ? "true" : "false") << "\n}\n";
        if (!WriteUtf8File(outputPath, json.str())) exitCode = 2;
        DestroyWindow(main);
    }
    void runSmoke() {
        settings = {};
        settings.beeps = false;
        settings.speech = false;
        fullscreenOverride = 0;
        dtm::Drive fixture;
        fixture.id = L"smoke-only-fixture";
        fixture.name = L"Smoke Test SSD";
        fixture.shortName = L"TestSSD";
        fixture.kind = dtm::DriveKind::SSD;
        fixture.celsius = 34;
        drives = {fixture};
        applyReadings();
        const bool windowCreated = IsWindow(main) != FALSE;
        const bool initialGreen = trayLevel() == dtm::Level::Green && popups.empty();
        drives[0].celsius = 51;
        applyReadings();
        const bool popupCreated = popups.size() == 1 &&
            (GetWindowLongPtrW(popups.begin()->second->window, GWL_EXSTYLE) & WS_EX_TOPMOST);
        fullscreenOverride = 1;
        updateAlerts();
        const bool fullscreenSuppressed = popups.empty();
        fullscreenOverride = 0;
        updateAlerts();
        const bool popupRestored = popups.size() == 1;
        dismiss(fixture.id);
        applyReadings();
        const bool dismissed = popups.empty();
        drives[0].celsius = 49;
        applyReadings();
        drives[0].celsius = 51;
        applyReadings();
        const bool rearmed = popups.size() == 1;
        show();
        ShowWindow(main, SW_MINIMIZE);
        const bool visibleWhileMinimized = !popups.empty() && IsWindowVisible(popups.begin()->second->window);
        ShowWindow(main, SW_RESTORE);
        SendMessageW(main, WM_CLOSE, 0, 0);
        const bool closeToTray = IsWindow(main) && !IsWindowVisible(main);
        const bool noMaximize = !(GetWindowLongPtrW(main, GWL_STYLE) & WS_MAXIMIZEBOX);
        settings.speech = true;
        settings.speechGapSeconds = 1;
        fullscreenOverride = 1;
        smokeSpeechEnabled = true;
        updateAlerts();
        const auto speechDeadline = GetTickCount64() + 15000;
        while (utteranceCount < 2 && !speechUnavailable && GetTickCount64() < speechDeadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                if (pending.message == WM_QUIT) break;
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            if (speechBusy && voice && !speechEventsWired) {
                SPVOICESTATUS status{};
                if (SUCCEEDED(voice->GetStatus(&status, nullptr)) && status.dwRunningState == SPRS_DONE)
                    speechFinished();
            }
        }
        STATSTG audio{};
        const bool synthesized = speechTestOutput && SUCCEEDED(speechTestOutput->Stat(&audio, STATFLAG_NONAME)) && audio.cbSize.QuadPart > 0;
        const bool repeatedAfterSilence = utteranceCount >= 2 && lastSpeechGapMs >= 1000;
        const auto measuredGap = lastSpeechGapMs;
        stopSpeech();
        smokeSpeechEnabled = false;
        const bool passed = windowCreated && trayAdded && initialGreen && popupCreated &&
            fullscreenSuppressed && popupRestored && dismissed && rearmed && closeToTray && noMaximize &&
            synthesized && repeatedAfterSilence && visibleWhileMinimized;
        std::ostringstream json;
        auto flag = [](bool value) { return value ? "true" : "false"; };
        json << "{\n  \"passed\": " << flag(passed) << ",\n"
             << "  \"fixture_mode\": true,\n  \"sensor_io\": false,\n"
             << "  \"window_created\": " << flag(windowCreated) << ",\n"
             << "  \"tray_added\": " << flag(trayAdded) << ",\n"
             << "  \"initial_green\": " << flag(initialGreen) << ",\n"
             << "  \"red_popup_created\": " << flag(popupCreated) << ",\n"
             << "  \"fullscreen_popup_suppressed\": " << flag(fullscreenSuppressed) << ",\n"
             << "  \"popup_restored\": " << flag(popupRestored) << ",\n"
             << "  \"dismissed_for_episode\": " << flag(dismissed) << ",\n"
             << "  \"cooling_rearms\": " << flag(rearmed) << ",\n"
             << "  \"close_to_tray\": " << flag(closeToTray) << ",\n"
             << "  \"no_maximize_button\": " << flag(noMaximize) << ",\n"
             << "  \"popup_visible_while_main_minimized\": " << flag(visibleWhileMinimized) << ",\n"
             << "  \"sapi_synthesized_to_memory\": " << flag(synthesized) << ",\n"
             << "  \"speech_repeated_after_silence\": " << flag(repeatedAfterSilence) << ",\n"
             << "  \"speech_test_configured_gap_ms\": 1000,\n"
             << "  \"speech_test_measured_gap_ms\": " << measuredGap << "\n}\n";
        const bool reported = WriteUtf8File(outputPath, json.str());
        exitCode = passed && reported ? 0 : 2;
        DestroyWindow(main);
    }
};

void App::showSettings(const std::wstring& id) {
    if (settingsWindow && IsWindow(settingsWindow)) {
        ShowWindow(settingsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(settingsWindow);
        return;
    }
    auto* context = new SettingsWindow;
    context->app = this;
    context->id = id;
    context->global = id.empty();
    context->dpi = dpi;
    const int height = context->global ? 500 : 332;
    RECT outer{0, 0, px(400), px(height)};
    AdjustWindowRectExForDpi(&outer, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT parent{};
    GetWindowRect(main, &parent);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass,
        context->global ? L"DriveTempMon settings" : L"Drive temperature limits", WS_CAPTION | WS_SYSMENU,
        parent.left + px(24), parent.top + px(24), outer.right - outer.left, outer.bottom - outer.top,
        main, nullptr, instance, context);
    if (!window) { delete context; return; }
    settingsWindow = window;
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
}

LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        context = static_cast<SettingsWindow*>(create->lpCreateParams);
        context->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (!context) return DefWindowProcW(window, message, wParam, lParam);
    App& app = *context->app;
    auto scale = [context](int value) { return MulDiv(value, static_cast<int>(context->dpi), 96); };
    auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND child = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text,
            WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(w), scale(h), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        return child;
    };
    auto load = [&](const dtm::Thresholds& limits) {
        const std::array<int, 3> values{limits.yellow, limits.orange, limits.red};
        for (unsigned i = 0; i != 3; ++i)
            SetWindowTextW(context->edits[i], std::to_wstring(values[i]).c_str());
        SendMessageW(context->alerts, BM_SETCHECK, limits.alert ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    switch (message) {
    case WM_CREATE: {
        context->font = CreateFontW(-MulDiv(10, static_cast<int>(context->dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        std::wstring title = context->global ? L"Defaults for drives without custom limits" :
            app.find(context->id) ? app.find(context->id)->name : L"Drive";
        control(L"STATIC", title.c_str(), SS_LEFT | SS_NOPREFIX, 20, 18, 360, 40, 0);
        const std::array<const wchar_t*, 3> labels{L"Yellow at (\u00b0C)", L"Orange at (\u00b0C)", L"Red / alert above (\u00b0C)"};
        for (unsigned i = 0; i != 3; ++i) {
            control(L"STATIC", labels[i], SS_LEFT, 20, 72 + static_cast<int>(i) * 36, 235, 24, 0);
            context->edits[i] = control(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 278, 68 + static_cast<int>(i) * 36, 92, 28, 310 + static_cast<int>(i));
            SendMessageW(context->edits[i], EM_SETLIMITTEXT, 3, 0);
        }
        context->alerts = control(L"BUTTON", L"Enable over-temperature alerts", WS_TABSTOP | BS_AUTOCHECKBOX,
                                  20, 184, 350, 28, 320);
        control(L"STATIC", L"Green is below yellow. Red starts above the limit; an active alert clears below it.",
                SS_LEFT, 20, 220, 350, 42, 0);
        load(context->global ? app.settings.defaults : dtm::forDrive(app.settings, context->id));
        int buttonY = 284;
        if (context->global) {
            const std::array<const wchar_t*, 3> labels2{L"Normal checks (seconds)", L"Hot checks (seconds)", L"Silence after speech (seconds)"};
            const std::array<unsigned, 3> values{app.settings.pollSeconds, app.settings.hotSeconds, app.settings.speechGapSeconds};
            for (unsigned i = 0; i != 3; ++i) {
                control(L"STATIC", labels2[i], SS_LEFT, 20, 274 + static_cast<int>(i) * 34, 252, 24, 0);
                context->edits[i + 3] = control(L"EDIT", std::to_wstring(values[i]).c_str(),
                    WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 278, 270 + static_cast<int>(i) * 34, 92, 28, 313 + static_cast<int>(i));
                SendMessageW(context->edits[i + 3], EM_SETLIMITTEXT, 4, 0);
            }
            context->beeps = control(L"BUTTON", L"Beep when a desktop alert appears", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 378, 350, 25, 321);
            context->speech = control(L"BUTTON", L"Speak alerts during fullscreen apps", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 408, 350, 25, 322);
            SendMessageW(context->beeps, BM_SETCHECK, app.settings.beeps ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(context->speech, BM_SETCHECK, app.settings.speech ? BST_CHECKED : BST_UNCHECKED, 0);
            buttonY = 452;
        }
        control(L"BUTTON", context->global ? L"Reset" : L"Use defaults", WS_TABSTOP | BS_PUSHBUTTON, 20, buttonY, 106, 30, kDefaultsButton);
        control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 192, buttonY, 82, 30, kCancelButton);
        control(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 286, buttonY, 84, 30, kSaveButton);
        SetFocus(context->edits[0]);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), kInk);
        return reinterpret_cast<LRESULT>(app.whiteBrush);
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, app.whiteBrush);
        return 1;
    }
    case WM_COMMAND: {
        const int command = LOWORD(wParam);
        if (command == kCancelButton || command == IDCANCEL) { DestroyWindow(window); return 0; }
        if (command == kDefaultsButton) {
            if (context->global) {
                const dtm::Settings defaults;
                load(defaults.defaults);
                const std::array<unsigned, 3> values{defaults.pollSeconds, defaults.hotSeconds, defaults.speechGapSeconds};
                for (unsigned i = 0; i != 3; ++i) SetWindowTextW(context->edits[i + 3], std::to_wstring(values[i]).c_str());
                SendMessageW(context->beeps, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(context->speech, BM_SETCHECK, BST_CHECKED, 0);
            } else {
                dtm::Settings next = app.settings;
                next.drives.erase(context->id);
                std::wstring error;
                if (!dtm::saveSettings(next, &error)) { MessageBoxW(window, error.c_str(), L"Could not save settings", MB_OK | MB_ICONERROR); return 0; }
                app.settings = std::move(next);
                app.applyReadings();
                DestroyWindow(window);
            }
            return 0;
        }
        if (command == kSaveButton || command == IDOK) {
            auto read = [&](unsigned index, unsigned maximum, unsigned& value) {
                wchar_t text[24]{};
                GetWindowTextW(context->edits[index], text, 24);
                wchar_t* end = nullptr;
                const unsigned long converted = wcstoul(text, &end, 10);
                if (!*text || !end || *end || converted > maximum) return false;
                value = static_cast<unsigned>(converted);
                return true;
            };
            std::array<unsigned, 6> values{};
            bool valid = true;
            for (unsigned i = 0; i != 3; ++i) valid = read(i, 125, values[i]) && valid;
            dtm::Thresholds limits;
            limits.yellow = static_cast<int>(values[0]);
            limits.orange = static_cast<int>(values[1]);
            limits.red = static_cast<int>(values[2]);
            limits.alert = SendMessageW(context->alerts, BM_GETCHECK, 0, 0) == BST_CHECKED;
            valid = valid && dtm::validThresholds(limits);
            dtm::Settings next = app.settings;
            if (context->global) {
                valid = read(3, 3600, values[3]) && read(4, 60, values[4]) && read(5, 300, values[5]) && valid;
                valid = valid && values[3] >= 1 && values[4] >= 1 && values[4] <= values[3] && values[5] >= 1;
                next.pollSeconds = values[3];
                next.hotSeconds = values[4];
                next.speechGapSeconds = values[5];
                next.defaults = limits;
                next.beeps = SendMessageW(context->beeps, BM_GETCHECK, 0, 0) == BST_CHECKED;
                next.speech = SendMessageW(context->speech, BM_GETCHECK, 0, 0) == BST_CHECKED;
            } else next.drives[context->id] = limits;
            if (!valid) {
                MessageBoxW(window, L"Use temperature limits from 0 to 125: yellow below orange, orange at or below red. Normal checks: 1\u20133600 s. Hot checks: 1\u201360 s, no slower than normal. Speech silence: 1\u2013300 s.",
                            L"Check the values", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            std::wstring error;
            if (!dtm::saveSettings(next, &error)) { MessageBoxW(window, error.c_str(), L"Could not save settings", MB_OK | MB_ICONERROR); return 0; }
            app.settings = std::move(next);
            app.speechUnavailable = false;
            app.lastError.clear();
            app.applyReadings();
            app.resetTimers();
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DPICHANGED: {
        struct Change { HWND owner; HFONT font; UINT oldDpi; UINT newDpi; } change{};
        change.owner = window;
        change.oldDpi = context->dpi;
        change.newDpi = HIWORD(wParam);
        HFONT previousFont = context->font;
        context->dpi = change.newDpi;
        context->font = CreateFontW(-MulDiv(10, static_cast<int>(context->dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        change.font = context->font;
        EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
            const auto& value = *reinterpret_cast<const Change*>(parameter);
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, value.owner, reinterpret_cast<POINT*>(&rect), 2);
            MoveWindow(child, MulDiv(rect.left, value.newDpi, value.oldDpi), MulDiv(rect.top, value.newDpi, value.oldDpi),
                MulDiv(rect.right - rect.left, value.newDpi, value.oldDpi), MulDiv(rect.bottom - rect.top, value.newDpi, value.oldDpi), TRUE);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(value.font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&change));
        if (previousFont) DeleteObject(previousFont);
        const auto& rect = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCDESTROY:
        if (app.settingsWindow == window) app.settingsWindow = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (context->font) DeleteObject(context->font);
        delete context;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* popup = reinterpret_cast<Popup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        popup = static_cast<Popup*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(popup));
    }
    if (!popup) return DefWindowProcW(window, message, wParam, lParam);
    App& app = *popup->app;
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, app.redBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ oldFont = SelectObject(dc, app.boldFont);
        RECT label{app.px(18), app.px(12), bounds.right - app.px(44), app.px(34)};
        DrawTextW(dc, L"DRIVE TEMPERATURE ALERT", -1, &label, DT_SINGLELINE | DT_NOPREFIX);
        const dtm::Drive* drive = app.find(popup->id);
        if (drive) {
            SelectObject(dc, app.normalFont);
            RECT name{app.px(18), app.px(43), bounds.right - app.px(120), app.px(88)};
            DrawTextW(dc, drive->name.c_str(), -1, &name, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, app.titleFont);
            RECT temp{bounds.right - app.px(116), app.px(44), bounds.right - app.px(16), app.px(85)};
            const auto text = TempText(*drive);
            DrawTextW(dc, text.c_str(), -1, &temp, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, app.normalFont);
        RECT bottom{app.px(18), app.px(88), bounds.right - app.px(18), app.px(108)};
        const std::wstring caption = L"Refreshing every " + std::to_wstring(app.settings.hotSeconds) + L" seconds";
        DrawTextW(dc, caption.c_str(), -1, &bottom, DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, app.titleFont);
        RECT close{bounds.right - app.px(38), app.px(2), bounds.right - app.px(6), app.px(36)};
        DrawTextW(dc, L"\u00d7", -1, &close, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (GET_X_LPARAM(lParam) >= bounds.right - app.px(44) && GET_Y_LPARAM(lParam) <= app.px(40)) {
            const auto id = popup->id;
            app.dismiss(id);
        }
        return 0;
    }
    case WM_CLOSE: {
        const auto id = popup->id;
        app.dismiss(id);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        app.arrangePopups();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->main = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    if (message == app->taskbarCreated && app->taskbarCreated) { app->trayAdded = false; app->updateTray(true); return 0; }
    if (message == app->quitMessage && app->quitMessage) { DestroyWindow(window); return 0; }
    if (message == app->showMessage && app->showMessage) { app->show(); return 0; }
    switch (message) {
    case WM_CREATE: {
        app->dpi = GetDpiForWindow(window);
        app->createFonts();
        app->button(L"Settings", kSettingsButton, 344, 24, 88, 32);
        app->list = CreateWindowExW(0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            app->px(20), app->px(96), app->px(412), app->px(216), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDriveList)), app->instance, nullptr);
        SendMessageW(app->list, LB_SETITEMHEIGHT, 0, app->px(58));
        SendMessageW(app->list, WM_SETFONT, reinterpret_cast<WPARAM>(app->normalFont), TRUE);
        ShowWindow(app->list, SW_HIDE);
        app->button(L"To tray", kToTrayButton, 244, 392, 88, 32);
        app->button(L"Quit", kQuitButton, 344, 392, 88, 32);
        if (app->portable) {
            app->startupButton = app->button(L"Add to startup",
                kStartupButton, 20, 392, 178, 32);
        }
        app->updateTray(true);
        if (!app->smoke) {
            app->worker.start(window);
            app->resetTimers();
        } else PostMessageW(window, kSmokeMessage, 0, 0);
        if (app->benchmark) SetTimer(window, kBenchmarkTimer, app->benchmarkSeconds * 1000u, nullptr);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        app->drawMain(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = static_cast<UINT>(app->px(58));
        return TRUE;
    case WM_DRAWITEM:
        if (wParam == kDriveList) { app->drawDrive(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE; }
        break;
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(app->panelBrush);
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSettingsButton: app->showSettings(); return 0;
        case kToTrayButton: app->hide(); return 0;
        case kQuitButton: DestroyWindow(window); return 0;
        case kStartupButton: {
            std::wstring error;
            if (!dtm::startup::setEnabled(!dtm::startup::isEnabled(), error))
                MessageBoxW(window, error.c_str(), L"Startup setting", MB_OK | MB_ICONERROR);
            SetWindowTextW(app->startupButton, dtm::startup::isEnabled() ? L"Remove from startup" : L"Add to startup");
            return 0;
        }
        case kDriveList:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                const auto index = static_cast<std::size_t>(SendMessageW(app->list, LB_GETCURSEL, 0, 0));
                if (index < app->drives.size()) app->showSettings(app->drives[index].id);
            }
            return 0;
        }
        break;
    case kTrayMessage: {
        const UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) app->show();
        else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) app->contextMenu();
        else if (event == WM_MBUTTONUP) DestroyWindow(window);
        return 0;
    }
    case kSamplesMessage:
        if (!app->finishing) { app->drives = app->worker.snapshot(); app->applyReadings(); }
        return 0;
    case kSpeechMessage:
        app->speechEvents();
        return 0;
    case kSmokeMessage:
        app->runSmoke();
        return 0;
    case WM_TIMER:
        switch (wParam) {
        case kNormalTimer:
            app->worker.request(++app->normalTicks % 10 == 0);
            break;
        case kHotTimer: {
            const auto ids = app->hotIds();
            if (!ids.empty()) app->worker.request(false, ids);
            break;
        }
        case kAlertTimer: app->updateAlerts(); break;
        case kSpeechTimer: app->speak(); break;
        case kRescanTimer: KillTimer(window, kRescanTimer); app->worker.request(true); break;
        case kBenchmarkTimer: KillTimer(window, kBenchmarkTimer); app->reportBenchmark(); break;
        }
        return 0;
    case WM_DEVICECHANGE:
        if (!app->smoke && (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE))
            SetTimer(window, kRescanTimer, 750, nullptr);
        return TRUE;
    case WM_POWERBROADCAST:
        if (!app->smoke && (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)) app->worker.request(true);
        return TRUE;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        app->arrangePopups();
        return 0;
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wParam);
        app->createFonts();
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        const std::array<std::array<int, 5>, 5> positions{{
            {kSettingsButton, 344, 24, 88, 32}, {kToTrayButton, 244, 392, 88, 32},
            {kQuitButton, 344, 392, 88, 32}, {kStartupButton, 20, 392, 178, 32}, {kDriveList, 20, 96, 412, 216}}};
        for (const auto& position : positions) {
            HWND child = GetDlgItem(window, position[0]);
            if (child) {
                MoveWindow(child, app->px(position[1]), app->px(position[2]), app->px(position[3]), app->px(position[4]), TRUE);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->normalFont), TRUE);
            }
        }
        SendMessageW(app->list, LB_SETITEMHEIGHT, 0, app->px(58));
        InvalidateRect(window, nullptr, TRUE);
        app->arrangePopups();
        return 0;
    }
    case WM_CLOSE:
        app->hide();
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wParam) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        app->cleanup();
        PostQuitMessage(app->exitCode);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com)) {
        // Task Scheduler connections require impersonation; no credentials are stored.
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    }
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool show = false, startup = false, quit = false, enableStartup = false, disableStartup = false;
    App app;
    app.instance = instance;
    for (int i = 1; arguments && i < count; ++i) {
        const std::wstring arg = arguments[i];
        if (arg == L"--show") show = true;
        else if (arg == L"--startup") startup = true;
        else if (arg == L"--quit") quit = true;
        else if (arg == L"--enable-startup") enableStartup = true;
        else if (arg == L"--disable-startup") disableStartup = true;
        else if (arg == L"--smoke-test" && i + 1 < count) { app.smoke = true; app.outputPath = arguments[++i]; }
        else if (arg == L"--benchmark" && i + 2 < count) {
            const int seconds = _wtoi(arguments[++i]);
            app.benchmarkSeconds = static_cast<unsigned>(std::clamp(seconds, 1, 3600));
            app.benchmark = true;
            app.outputPath = arguments[++i];
        } else {
            if (arguments) LocalFree(arguments);
            if (SUCCEEDED(com)) CoUninitialize();
            return 64;
        }
    }
    if (arguments) LocalFree(arguments);
    app.quitMessage = RegisterWindowMessageW(L"DriveTempMon.Quit.1");
    app.showMessage = RegisterWindowMessageW(L"DriveTempMon.Show.1");
    if (enableStartup || disableStartup) {
        std::wstring error;
        const bool success = dtm::startup::setEnabled(enableStartup && !disableStartup, error);
        if (SUCCEEDED(com)) CoUninitialize();
        return success ? 0 : 1;
    }
    if (quit) {
        HWND other = FindWindowW(kMainClass, L"DriveTempMon");
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(other, app.quitMessage, 0, 0);
            const DWORD result = process ? WaitForSingleObject(process, 15000) : WAIT_OBJECT_0;
            if (process) CloseHandle(process);
            if (SUCCEEDED(com)) CoUninitialize();
            return result == WAIT_TIMEOUT ? 1 : 0;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    std::wstring mutexName = L"Local\\DriveTempMon.SingleInstance.1";
    if (app.smoke) mutexName += L".Smoke." + std::to_wstring(GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) { if (SUCCEEDED(com)) CoUninitialize(); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (show) {
            HWND other = FindWindowW(kMainClass, L"DriveTempMon");
            if (other) PostMessageW(other, app.showMessage, 0, 0);
        }
        CloseHandle(mutex);
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    app.portable = dtm::portableMode();
    app.settings = app.smoke ? dtm::Settings{} : dtm::loadSettings();
    if (app.portable && !startup && !app.benchmark && !app.smoke) {
        show = show || dtm::portableFirstLaunch();
        dtm::markPortableLaunched();
    }
    if (startup || app.benchmark) show = false;
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    app.whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    app.panelBrush = CreateSolidBrush(kPanel);
    app.redBrush = CreateSolidBrush(kRed);
    for (unsigned i = 0; i < app.icons.size(); ++i) {
        const COLORREF color = LevelColor(static_cast<dtm::Level>(i));
        app.icons[i] = MakeTrayIcon(color);
        app.menuBitmaps[i] = MakeColorBitmap(color, 16, false);
    }
    WNDCLASSEXW mainClass{sizeof(mainClass)};
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = app.icons[static_cast<unsigned>(dtm::Level::Green)];
    mainClass.hIconSm = mainClass.hIcon;
    mainClass.lpszClassName = kMainClass;
    WNDCLASSEXW popupClass = mainClass;
    popupClass.lpfnWndProc = PopupProc;
    popupClass.lpszClassName = kPopupClass;
    WNDCLASSEXW settingsClass = mainClass;
    settingsClass.lpfnWndProc = SettingsProc;
    settingsClass.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&mainClass) || !RegisterClassExW(&popupClass) || !RegisterClassExW(&settingsClass)) {
        CloseHandle(mutex);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    app.dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT outer{0, 0, app.px(452), app.px(444)};
    AdjustWindowRectExForDpi(&outer, style, FALSE, 0, app.dpi);
    FILETIME created{}, ended{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user);
    app.startCpu = FileTimeValue(kernel) + FileTimeValue(user);
    app.startTicks = GetTickCount64();
    HWND window = CreateWindowExW(0, kMainClass, app.smoke ? L"DriveTempMon Smoke Test" : L"DriveTempMon", style, CW_USEDEFAULT, CW_USEDEFAULT,
        outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr, instance, &app);
    if (!window) {
        CloseHandle(mutex);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    // An elevated SMART reader still accepts these benign user-session lifecycle
    // messages from an unelevated second invocation or Windows Explorer.
    CHANGEFILTERSTRUCT filter{sizeof(filter)};
    ChangeWindowMessageFilterEx(window, app.quitMessage, MSGFLT_ALLOW, &filter);
    ChangeWindowMessageFilterEx(window, app.showMessage, MSGFLT_ALLOW, &filter);
    ChangeWindowMessageFilterEx(window, app.taskbarCreated, MSGFLT_ALLOW, &filter);
    if (show) app.show();
    MSG message{};
    int loopResult = 0;
    while ((loopResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (app.settingsWindow && IsDialogMessageW(app.settingsWindow, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    const int result = loopResult < 0 ? 1 : static_cast<int>(message.wParam);
    app.cleanup();
    CloseHandle(mutex);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
