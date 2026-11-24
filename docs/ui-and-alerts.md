# Native UI and alert behavior

The application uses a Win32 message loop, Windows GDI, a single sleeping sensor
worker, and an on-demand Windows SAPI voice. It does not run a frame loop, change
Windows timer resolution, or create a service. Normal sampling uses one 30-second
window timer. A separate 3-second timer requests only hot drives while an alert
episode remains active. The one-second fullscreen check exists only while an
undismissed, valid hot reading needs an alert.

The tray icon represents the hottest known physical temperature using that
drive's configured limits. Unknown readings are gray. The menu displays all
drives with a colored dot and temperature, and its entries open per-drive limits.
An unknown drive does not masquerade as cool.

Alert popups are independent, nonactivating tool windows with `WS_EX_TOPMOST`.
They remain visible if the main window is minimized and do not take keyboard
focus. Popups use the monitor work area so they clear the taskbar. Their X button
acknowledges the current episode; a known reading below the red limit rearms it.
The main window's X hides the main window, while minimizing uses the ordinary
Windows taskbar. Quit, tray Close, and tray middle-click end the application.

## Fullscreen and speech

Windows reports exclusive Direct3D fullscreen through
[`SHQueryUserNotificationState`](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shqueryusernotificationstate)
and
[`QUNS_RUNNING_D3D_FULL_SCREEN`](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state).
There is no event that announces every fullscreen transition, so the application
checks while alerting. A conservative foreground borderless-window check also
suppresses overlays for modern borderless games. This is not an injected game
hook or a guarantee that every graphics driver classifies exclusive mode alike.

During fullscreen, the application constructs one utterance containing the current
undismissed hot drives. Saved nicknames are spoken in full; automatic drive aliases
remain at most ten characters. It waits for the
SAPI [`SPEI_END_INPUT_STREAM`](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms720164(v=vs.85))
completion event, then starts the configured silence timer before repeating. The
event arrives through
[`SetNotifyWindowMessage`](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms717985(v=vs.85)).
`GetStatus` is a fallback for an engine that fails to deliver events. Cooled,
removed, or dismissed drives cancel obsolete queued speech. Leaving fullscreen
purges speech, releases the voice, and restores any current desktop alerts.

## Lifecycle and tests

`--startup` always starts hidden. A portable program's first ordinary launch opens
its window, and subsequent ordinary launches start hidden. `--show` explicitly
opens the existing or new instance. A session-local named mutex permits only one
ordinary process; starting another executable copy does not create another monitor.

`--smoke-test <report.json>` is an explicit fixture mode with no sensor I/O and no
settings writes. It creates the actual native window, tray icon, and alert popup,
tests acknowledgement, cooling, restoration, topmost style, minimized-window
behavior, and close-to-tray behavior. Fullscreen state is injected only in this
mode. The test also synthesizes real SAPI speech into a memory stream, checks that
audio bytes were produced, and observes an actual completion-to-repeat delay with
a one-second configured silence interval. The production default remains five
seconds. The test does not prove physical speaker output or exclusive game behavior.

`--benchmark <seconds> <report.json>` runs the normal sensor worker and hidden tray
with real readings. It reports working set, private bytes, process CPU time,
handle count, and actual sensor/drive counts. It does not manufacture hardware
measurements when a CI virtual disk exposes no sensor.

Shutdown cancels synchronous sensor I/O and retries cancellation for up to five
seconds. A driver that ignores cancellation cannot indefinitely hold the UI open:
the thread's separately owned state survives until process exit, and it cannot
post messages or access destroyed application objects after stop is requested.
Windows reclaims any remaining request when the process exits.
