param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$OutputDirectory = 'test-output'
)
. "$PSScriptRoot/Test-Support.ps1"
Ensure-TestDesktop
$Executable = (Resolve-Path $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
$previousConfig = $env:DRIVETEMPMON_CONFIG_DIR
$env:DRIVETEMPMON_CONFIG_DIR = Join-Path $OutputDirectory 'portable-config'
New-Item -ItemType Directory -Path $env:DRIVETEMPMON_CONFIG_DIR -Force | Out-Null
$portableDir = Join-Path $OutputDirectory 'Portable Location With Spaces'
New-Item -ItemType Directory -Path $portableDir -Force | Out-Null
$portable = Join-Path $portableDir 'DriveTempMon.exe'
Copy-Item $Executable $portable -Force
$process = $null
$checks = [ordered]@{}

try {
    $smokePath = Join-Path $OutputDirectory 'ui-smoke.json'
    # Capture the real native fixture UI; no image/export dependency ships in the app.
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace DriveTempMonTests {
    public static class Preview {
        [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
        [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
        [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetProp(IntPtr window, string name, IntPtr value);
    }
}
'@
    function Save-PreviewWindow([IntPtr]$Window, [string]$Name, [bool]$FromScreen = $false) {
        $bounds = [DriveTempMonTests.Preview+Rect]::new()
        Assert-True ([DriveTempMonTests.Preview]::GetWindowRect($Window, [ref]$bounds)) 'Cannot measure preview window.'
        $bitmap = [System.Drawing.Bitmap]::new($bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            if ($FromScreen) {
                $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
            } else {
                $dc = $graphics.GetHdc()
                try { Assert-True ([DriveTempMonTests.Preview]::PrintWindow($Window, $dc, 2)) 'Native preview capture failed.' }
                finally { $graphics.ReleaseHdc($dc) }
            }
            $bitmap.Save((Join-Path $OutputDirectory $Name), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
    $process = Start-Process -FilePath $portable -ArgumentList @('--smoke-test', ('"{0}"' -f $smokePath)) -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $preview = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
        $preview = [DriveTempMonTests.Preview]::FindWindow('DriveTempMon.Main', 'DriveTempMon Nickname Preview')
        if ($preview -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 20
    }
    Assert-True ($preview -ne [IntPtr]::Zero) 'Nickname preview fixture did not become ready.'
    try {
        Save-PreviewWindow $preview 'drive-nicknames.png'
        # Exercise the same tray callback used by a real right-click.
        Assert-True ([DriveTempMonTests.Preview]::PostMessage($preview, 0x8001, [IntPtr]::Zero, [IntPtr]0x7B)) 'Cannot open tray menu.'
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        $menu = [IntPtr]::Zero
        while ([DateTime]::UtcNow -lt $deadline) {
            $menu = [DriveTempMonTests.Preview]::FindWindow('#32768', $null)
            [uint32]$menuPid = 0
            if ($menu -ne [IntPtr]::Zero) {
                [DriveTempMonTests.Preview]::GetWindowThreadProcessId($menu, [ref]$menuPid) | Out-Null
                if ($menuPid -eq $process.Id) { break }
            }
            $menu = [IntPtr]::Zero
            Start-Sleep -Milliseconds 20
        }
        Assert-True ($menu -ne [IntPtr]::Zero) 'Native tray menu did not open.'
        Start-Sleep -Milliseconds 150
        Save-PreviewWindow $menu 'drive-nicknames-tray-menu.png' $true
    } finally {
        [DriveTempMonTests.Preview]::PostMessage($preview, 0x001F, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        [DriveTempMonTests.Preview]::SetProp($preview, 'DriveTempMon.PreviewCaptured', [IntPtr]1) | Out-Null
    }
    Assert-True ($process.WaitForExit(15000)) 'UI smoke test did not finish after capture.'
    $process.Refresh()
    Assert-True ($process.ExitCode -eq 0) "UI smoke test failed ($($process.ExitCode))."
    $process = $null
    Assert-True (Test-Path $smokePath) 'The UI smoke test did not write its report.'
    $smoke = Get-Content $smokePath -Raw | ConvertFrom-Json
    Assert-True ($smoke.passed -eq $true) 'The native UI smoke test failed.'
    $checks.nativeUiSmoke = $true

    # A clean portable location opens settings on its first explicit launch.
    $process = Start-Process -FilePath $portable -PassThru
    Start-Sleep -Seconds 3
    $process.Refresh()
    Assert-True (-not $process.HasExited) 'The portable app exited unexpectedly.'
    Assert-True ([DriveTempMonTests.Windows]::VisibleMainWindows($process.Id) -eq 1) 'First portable launch did not show the main window.'
    $checks.portableFirstLaunchShowsWindow = $true

    $second = Start-Process -FilePath $portable -PassThru
    Assert-True ($second.WaitForExit(10000)) 'A second application instance remained running.'
    Assert-True ($second.ExitCode -eq 0) 'Second instance did not exit cleanly.'
    $process.Refresh()
    Assert-True (-not $process.HasExited) 'The existing instance was unexpectedly terminated.'
    $checks.singleInstance = $true
    Stop-Monitor $portable $process
    $process = $null

    $process = Start-Process -FilePath $portable -PassThru
    Start-Sleep -Seconds 3
    $process.Refresh()
    Assert-True (-not $process.HasExited) 'The second normal launch exited unexpectedly.'
    Assert-True ([DriveTempMonTests.Windows]::VisibleMainWindows($process.Id) -eq 0) 'Subsequent manual launch did not remain in the tray.'
    $checks.laterManualLaunchIsTrayOnly = $true
    Stop-Monitor $portable $process
    $process = $null

    $process = Start-Process -FilePath $portable -ArgumentList '--startup' -PassThru
    Start-Sleep -Seconds 3
    $process.Refresh()
    Assert-True (-not $process.HasExited) 'The startup launch exited unexpectedly.'
    Assert-True ([DriveTempMonTests.Windows]::VisibleMainWindows($process.Id) -eq 0) 'Startup launch showed a window.'
    $checks.startupIsTrayOnly = $true
    Stop-Monitor $portable $process
    $process = $null

    Invoke-AppCommand $portable @('--enable-startup')
    $tasks = @(Get-MonitorTask $portable)
    Assert-True ($tasks.Count -eq 1) 'Portable startup did not create exactly one task for its actual location.'
    Assert-True ($tasks[0].Actions[0].Arguments -eq '--startup') 'Startup task does not use --startup.'
    Assert-True ($tasks[0].Principal.RunLevel -eq 'Highest') 'Startup task is not configured for SMART access.'
    Assert-True ($tasks[0].Triggers.Count -ge 1) 'Startup task has no logon trigger.'
    $checks.portableStartupUsesActualPath = $true
    Invoke-AppCommand $portable @('--disable-startup')
    Assert-True (@(Get-MonitorTask $portable).Count -eq 0) 'Portable startup task was not removed.'
    $checks.portableStartupRemoval = $true
    $checks.passed = $true
    $checks | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'windows-integration.json') -Encoding utf8
    $checks | ConvertTo-Json -Depth 5 | Write-Output
}
finally {
    if ($null -ne $process) { Stop-Monitor $portable $process }
    if (@(Get-MonitorTask $portable).Count -gt 0) {
        Invoke-AppCommand $portable @('--disable-startup')
    }
    $env:DRIVETEMPMON_CONFIG_DIR = $previousConfig
}
