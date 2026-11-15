#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BinaryPath
  #define BinaryPath "..\build\Release\DriveTempMon.exe"
#endif
#ifndef ReleaseDir
  #define ReleaseDir "..\dist"
#endif

[Setup]
AppId={{C6443472-5CBD-46F8-ACEB-D417CD84B6C3}
AppName=DriveTempMon
AppVersion={#AppVersion}
AppPublisher=Eyad / Eabusham2
AppPublisherURL=https://github.com/Eabusham2/DriveTempMon
AppSupportURL=https://github.com/Eabusham2/DriveTempMon/issues
AppUpdatesURL=https://github.com/Eabusham2/DriveTempMon/releases
DefaultDirName={autopf}\DriveTempMon
DefaultGroupName=DriveTempMon
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\DriveTempMon.exe
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#ReleaseDir}
OutputBaseFilename=DriveTempMon-{#AppVersion}-windows-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
VersionInfoVersion={#AppVersion}
VersionInfoDescription=DriveTempMon installer

[Files]
Source: "{#BinaryPath}"; DestDir: "{app}"; DestName: "DriveTempMon.exe"; Flags: ignoreversion
Source: "installed.flag"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
; A Start menu entry makes Windows Search discover the app. No desktop icon is created.
Name: "{commonprograms}\DriveTempMon"; Filename: "{app}\DriveTempMon.exe"; Comment: "Drive temperatures and alert settings"

[Run]
Filename: "{app}\DriveTempMon.exe"; Description: "Start DriveTempMon"; Flags: nowait runascurrentuser

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  AppExe: String;
begin
  Result := '';
  AppExe := ExpandConstant('{app}\DriveTempMon.exe');
  if FileExists(AppExe) then
    Exec(AppExe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    if not Exec(ExpandConstant('{app}\DriveTempMon.exe'), '--enable-startup', '',
                SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      RaiseException('Could not configure DriveTempMon startup. Re-run Setup as the Windows account that will use DriveTempMon.');
    if ResultCode <> 0 then
      RaiseException('DriveTempMon startup registration failed (exit code ' + IntToStr(ResultCode) + ').');
  end;
end;

function InitializeUninstall(): Boolean;
var
  AppExe: String;
  ResultCode: Integer;
begin
  Result := True;
  AppExe := ExpandConstant('{app}\DriveTempMon.exe');
  if not FileExists(AppExe) then
    exit;
  Exec(AppExe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if not Exec(AppExe, '--disable-startup', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := False
  else if ResultCode <> 0 then
    Result := False;
  if not Result then begin
    Log('Startup cleanup failed; leaving application installed to avoid a dangling startup task.');
    if not UninstallSilent then
      MsgBox('DriveTempMon could not remove its startup task. Please run this uninstaller from the Windows account used to install it.', mbError, MB_OK);
  end;
end;
