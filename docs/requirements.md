# DriveTempMon requirements

This file preserves the requested behavior and the implementation decisions used
to make the edge cases unambiguous. The Windows executable is native C++17.

| Area | Required behavior |
| --- | --- |
| Temperature input | Read HDD/SSD temperature through Windows storage/SMART interfaces; no general health dashboard or background logging. |
| Cadence | Normal polling every 30 seconds. Hot drives refresh every 3 seconds. Both intervals are configurable. |
| Colors | Green below 35°C, yellow from 35°C, orange from 45°C through 50°C, red above 50°C, initially. Configure defaults and per-drive overrides. |
| Tray icon | Reflect the highest measured drive temperature using that drive's thresholds. An unavailable temperature must not be fabricated. |
| Desktop alert | Red, topmost, bottom-right popup with model name, live temperature, and X dismissal; beep when an alert starts. |
| Alert threshold | Start strictly above the configured red threshold. Continue at equality; clear below it. |
| Dismissal | X acknowledges the current hot episode. Re-arm after a known reading below the threshold. |
| Exclusive fullscreen | Suppress the desktop overlay; say the drive type, a name of at most ten characters, and its Celsius temperature. |
| Speech cadence | Wait five seconds after speech completes before repeating. Never queue an unbounded backlog of temperature messages. |
| Fullscreen exit | Stop obsolete speech and return to the desktop popup for unacknowledged hot drives. |
| Tray controls | Left click opens; middle click quits; right menu has Open first, colored device rows with temperatures, and Close last. |
| Main window | Compact, no maximize/fullscreen button; device selection opens per-drive settings. Minimize is normal minimization. Closing hides to tray. Dedicated To tray and Quit buttons. |
| Portable mode | One standalone EXE, first manual launch opens the main screen; later manual launches and all startup launches go to tray. Add/remove startup for its current path. |
| Installed mode | Installer registers startup, launches the app, creates a searchable Start menu entry, and creates no desktop shortcut. |
| Startup | Elevated, interactive scheduled task for the signed-in administrator account; no additional persistent process/service. |
| Single instance | Reopening does not create another monitor. Explicit Open uses the existing instance. |
| Repository | One branch, main, README and topic tags. Successful CI produces a version tag and a release with installer and portable EXE. |
| Footprint | Size-optimized native binary, static runtime, sleeping/event-driven idle behavior, low-priority drive work, no network use, no per-poll settings/log writes. Report measured numbers. |

## Practical boundaries

- Reading temperature requires a small standard SMART/health or temperature
  response from a device. Protocols do not universally offer a one-byte
  temperature-only transaction. The app interprets temperature fields only.
- Windows and the storage controller must expose a usable temperature path.
  Unsupported RAID/USB bridges and virtual disks appear as unavailable.
- No running monitor has literally zero CPU, memory, or I/O cost. CI measurements
  characterize a Windows virtual machine; physical-drive latency and gaming
  performance require a real-machine measurement.
- Windows' Direct3D fullscreen notification state is supplemented by a
  foreground fullscreen-window check because games can use different
  presentation paths.
- A searchable classic Windows app normally needs a Start menu registration.
  The installer uses that entry and does not add a desktop icon.
