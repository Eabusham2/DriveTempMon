# DriveTempMon

[![Windows build, verify, and release](https://github.com/Eabusham2/DriveTempMon/actions/workflows/release.yml/badge.svg)](https://github.com/Eabusham2/DriveTempMon/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/Eabusham2/DriveTempMon)](https://github.com/Eabusham2/DriveTempMon/releases/latest)

A small native Windows tray application that watches HDD and SSD temperatures and
alerts when a drive gets too hot. Written in C++17 with the Windows API, with no
browser engine, .NET runtime, third-party monitoring process, or background service.

**[Download the latest release](https://github.com/Eabusham2/DriveTempMon/releases/latest)**

## Choose an edition

| Download | Behavior |
| --- | --- |
| `DriveTempMon-<version>-windows-x64-setup.exe` | Installs in Program Files, registers startup, adds a Windows Start menu search entry, and starts DriveTempMon in the tray. No desktop shortcut. |
| `DriveTempMon-<version>-windows-x64-portable.exe` | One standalone executable. Put it where you want before running it. Its first manual launch opens the window; later launches and startup runs go to the tray. The window has Add to startup / Remove from startup controls. |

Windows 10 version 1809 or newer and Windows 11, x64. SMART pass-through requires
administrator access, so the executable requests elevation. Install and configure
startup from the Windows administrator account that will use the app. Supplying a
different administrator account's credentials from a standard account configures
startup for that administrator, not for the original standard account.

The release is unsigned. Windows may show an unknown-publisher or SmartScreen
prompt. Each release includes `SHA256SUMS.txt` so downloads can be checked against
the build's published hashes.

The installed app has one Start menu entry so Windows Search can find it. It does
not place an icon on your desktop. Launching the installed app from search or
running the executable directly starts it in the tray. Click its tray icon to
open the window.

## Default behavior

| Setting | Default |
| --- | --- |
| Normal temperature polling | Every 30 seconds |
| Hot-drive refresh | Every 3 seconds, for hot drives only |
| Green | Below 35 °C |
| Yellow | 35 °C through 44 °C |
| Orange | 45 °C through 50 °C |
| Red and alert onset | Above 50 °C |
| Alert recovery | Below 50 °C |
| Fullscreen speech repeat | Five seconds after the previous spoken alert finishes |

The tray icon follows the **physically hottest drive with a valid reading**, using
that drive's configured color thresholds. Unknown temperature is shown explicitly;
when no drive has a valid reading, the tray icon is gray.

An alert that starts above 50 °C remains active at exactly 50 °C, then clears below
50 °C. This preserves the distinction between "exceeds 50" and "falls below 50."
All thresholds and the three timing settings can be changed. Thresholds must stay
in ascending order. Each drive can override the global defaults and independently
enable or disable alerts.

### Desktop alerts

When a drive exceeds its red threshold, a red, topmost popup appears in the
bottom-right work area, showing the drive's name and current temperature. It does
not take keyboard focus. Multiple hot drives have their own popups. An alert
beeps when displayed, unless the beep setting is disabled.

The temperature refreshes at the hot-drive interval. The popup clears when the
drive cools below its configured red threshold. Pressing its **×** dismisses that
drive's current heat episode; the alert rearms after cooling, so a continuously hot
drive does not immediately reopen a dismissed popup.

### Fullscreen alerts

During a fullscreen application, popups are suppressed and Windows speech says,
for example, "Your SSD Samsung990 is 53 degrees Celsius." The spoken device alias
is at most ten characters. Speech finishes before the five-second repeat gap
starts; utterances do not stack up. Several hot drives take turns.

When fullscreen ends, speech stops and any active, undismissed alerts return as
popups. The app uses Windows' Direct3D fullscreen notification state, with a
conservative foreground-window check for fullscreen and borderless applications.
Windows does not expose a perfect, universal detector for every graphics API,
overlay, game, and exclusive-display mode. See [Windows API references](#windows-api-references).

Speech uses the installed Windows SAPI voice. The app does not contact a cloud
speech service. Speech and beeps can each be turned off in Settings.

## Controls

| Action | Result |
| --- | --- |
| Single left click on the tray icon | Open the small main window |
| Right click on the tray icon | Open at the top, each drive's color/name/temperature, Close at the bottom |
| Click a drive in the tray menu or main window | Edit that drive's thresholds and alert setting |
| Middle click on the tray icon | Quit the app |
| Main-window Settings | Edit defaults, intervals, beeps, and fullscreen speech |
| Main-window To tray | Hide the window and keep monitoring |
| Main-window × | Hide to the tray |
| Main-window minimize | Minimize the window |
| Main-window Quit or tray Close | Exit and stop monitoring |

There is no maximize button. Only one normal instance runs in a Windows user
session. Launching a second copy leaves the existing monitor running. The tray's
Open command or an explicit `--show` command can show that existing instance.

If Windows Explorer restarts, DriveTempMon restores its tray icon. Drive arrival,
removal, and system resume request fresh device discovery. Normal polling and
SMART calls run on a sleeping worker, outside the window's message loop.

## Startup and portable use

Startup uses one Windows Task Scheduler logon task named `DriveTempMon-<user SID>`.
It runs the app for that user with the privileges needed for SMART access and
passes `--startup`, so no window opens at sign-in. There is no permanent service,
helper process, or repeated UAC prompt from the startup task.

The installer registers this automatically. The portable edition exposes a
startup toggle and records the executable's **actual full path**, including paths
with spaces. If you move a portable executable, remove its old startup entry and
add it again from the new location. Enabling startup for a different copy replaces
the user's existing DriveTempMon task; it does not create multiple startup copies.

Uninstalling the installed edition removes its program files, Start menu entry,
and its own startup task. It does not remove a startup task that has since been
reassigned to a different copy. User preferences are retained.

Settings live in `%LOCALAPPDATA%\DriveTempMon\settings.ini`. Drive settings use
stable device identity where the controller exposes it, so identical model names
can have different thresholds. The small `portable-seen.ini` tracks the first
launch of each portable location. Settings are written when preferences change,
not on every temperature poll. For isolated tests, `DRIVETEMPMON_CONFIG_DIR` can
override the settings directory.

## Supported drives

The sensor backend reads native NVMe SMART health data, Windows storage
temperature descriptors, ATA SMART temperature attributes, compatible USB SATA
SAT bridges, and standard SCSI/SAS temperature logs. The implementation extracts
temperature only; it does not run self-tests or modify drive SMART settings.

SMART protocols sometimes return a complete small log even when temperature is
the only wanted field. The app does not collect the other attributes or maintain
temperature-history files. Read support is cached to avoid transport probing on
every poll.

RAID/VMD controllers, proprietary USB NVMe enclosures, virtual disks, or drivers
that hide temperature data may show **unavailable**. A sleeping device is skipped
when Windows reports it asleep. Some drivers do not report power state reliably,
so no monitoring app can promise that every controller will avoid waking a drive.

Read [sensor support, power behavior, and limitations](docs/sensors.md) for the
actual supported commands and primary API references.

## Footprint

Each release includes **`FOOTPRINT.md` and `footprint.json` with measured values**,
including:

- Portable executable and installer sizes, plus installed program-file size.
- Average and maximum private memory and working set.
- Process CPU time and average CPU percentage, with the denominator stated.
- Process handles, threads, sampling duration, device counts, and sensor-poll count.

CI measures the real hidden-tray process after warmup for 40 seconds, covering a
normal 30-second polling interval. It uses real device discovery on a Windows
virtual machine. A hosted VM normally has no exposed physical SMART temperatures,
so these numbers describe the app's idle footprint there. Real storage-driver
latency, active voice memory, and game frame-time impact need measurement on the
target PC. Very small recorded CPU use may round to zero; that is not a claim of
literally zero overhead or identical performance to a closed process.

There is no busy polling when temperatures are normal, no continuous animation,
no network telemetry, and no application log written at every sample. The Windows
speech engine is created for active fullscreen alerts, rather than kept speaking
or polling in the background.

## Build and verify

Install Visual Studio 2022's C++ desktop workload, a Windows 10/11 SDK, and CMake.
Use a Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The release executable is `build\Release\DriveTempMon.exe`. Release builds use a
static MSVC runtime, size optimization, link-time optimization, and unused-code
elimination, so no Visual C++ redistributable installer is required.

Additional Windows checks run from an elevated PowerShell:

```powershell
./scripts/Test-Windows.ps1 -Executable ./build/Release/DriveTempMon.exe
./scripts/Measure-Footprint.ps1 -Executable ./build/Release/DriveTempMon.exe
```

The UI smoke mode is explicit and isolated: it exercises actual native windows,
tray icons, popup dismissal/rearming, and fullscreen presentation transitions
with a test fixture. It does not inject fake temperatures into normal monitoring.
The integration script also checks first/subsequent portable launches, single
instance behavior, and real startup task registration/removal. Parser fixtures
verify binary SMART responses; they do not replace physical-drive validation.

Policy and parser tests also build on Linux/macOS with CMake and a C++17 compiler:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For installer builds, use Inno Setup 6. The GitHub workflow supplies the exact
version and binary paths to `installer/DriveTempMon.iss`, then silently installs,
checks the executable/startup/menu/launch behavior, and uninstalls it before release.

### Release workflow

`main` is the maintained branch. Every successful push or manual workflow run on
`main` publishes `v1.0.<workflow run number>`. The Windows job must pass compilation,
policy/parser/settings tests, native UI tests, startup tests, footprint sampling,
and installer/uninstaller tests first. A separate job then checks all SHA-256 sums,
uploads the assets to a draft release, and publishes it. A failed build or test
does not publish a new release. A published version is not overwritten on a rerun.

The [requirements checklist](docs/requirements.md) maps the requested behavior to
its implementation and verification. CI results are available in
[GitHub Actions](https://github.com/Eabusham2/DriveTempMon/actions).

## Windows API references

- [Microsoft: SHQueryUserNotificationState and fullscreen state](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shqueryusernotificationstate)
- [Microsoft: User notification states](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state)
- [Microsoft: Windows Speech API ISpVoice](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms719574(v=vs.85))
- [Microsoft: Task Scheduler security contexts](https://learn.microsoft.com/en-us/windows/win32/taskschd/security-contexts-for-running-tasks)
- [Microsoft: MSVC runtime library options](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library)
- [GitHub: Windows 2022 runner tools](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)
- [Inno Setup: run/uninstall commands](https://jrsoftware.org/is6help/topic_runsection.htm)

## License

[MIT](LICENSE). Copyright © 2026 Eyad (Eabusham2).
