#define AppName "Studio Duo"
#define AppVersion GetEnv("STUDIO_DUO_VERSION")
#define AppExecutable GetEnv("STUDIO_DUO_EXECUTABLE")
#define SourceDirectory GetEnv("STUDIO_DUO_SOURCE_DIRECTORY")
#define OutputDirectory GetEnv("STUDIO_DUO_INSTALLER_OUTPUT_DIRECTORY")
#define VCRedistPath GetEnv("STUDIO_DUO_VC_REDIST")
#define VCRedistVersion GetEnv("STUDIO_DUO_VC_REDIST_VERSION")

[Setup]
AppId={{6B4141DC-39BF-429F-9D41-C21BDD63AE0E}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Studio Duo
AppPublisherURL=https://github.com/mbianchidev/studio-duo
AppSupportURL=https://github.com/mbianchidev/studio-duo/issues
AppUpdatesURL=https://github.com/mbianchidev/studio-duo/releases
DefaultDirName={autopf}\Studio Duo
DefaultGroupName=Studio Duo
DisableProgramGroupPage=yes
LicenseFile={#SourceDirectory}\LICENSE
OutputDir={#OutputDirectory}
OutputBaseFilename=Studio-Duo-{#AppVersion}-Windows-x64-Setup
SetupIconFile={#SourceDirectory}\assets\branding\StudioDuo.ico
CreateUninstallRegKey=yes
Uninstallable=yes
UninstallDisplayIcon={app}\Studio Duo.exe
UninstallDisplayName=Studio Duo {#AppVersion}
VersionInfoCompany=Studio Duo
VersionInfoDescription=Studio Duo Installer
VersionInfoProductName=Studio Duo
VersionInfoProductVersion={#AppVersion}
VersionInfoVersion={#AppVersion}.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesAssociations=no
CloseApplications=yes
RestartApplications=no
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#VCRedistPath}"; DestName: "vc_redist.x64.exe"; Flags: dontcopy noencryption
Source: "{#AppExecutable}"; DestDir: "{app}"; DestName: "Studio Duo.exe"; Flags: ignoreversion
Source: "{#SourceDirectory}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDirectory}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Studio Duo"; Filename: "{app}\Studio Duo.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Studio Duo"; Filename: "{app}\Studio Duo.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Studio Duo.exe"; Description: "Launch Studio Duo"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent; Check: not IsAutomaticUpdate
Filename: "{app}\Studio Duo.exe"; WorkingDir: "{app}"; Flags: nowait skipifdoesntexist; Check: IsAutomaticUpdate

[Code]
const
  VCRuntimeRegistryKey =
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';

var
  VCRuntimeRestartRequired: Boolean;

function IsAutomaticUpdate: Boolean;
begin
  Result :=
    ExpandConstant('{param:STUDIODUOUPDATE|0}') = '1';
end;

function VCRuntimeNeedsInstall: Boolean;
var
  Installed: Cardinal;
  InstalledVersion: String;
  InstalledPackedVersion: Int64;
  RequiredPackedVersion: Int64;
begin
  Result := True;
  if not RegQueryDWordValue(
      HKLM64,
      VCRuntimeRegistryKey,
      'Installed',
      Installed)
    or (Installed <> 1) then
    Exit;

  if not RegQueryStringValue(
      HKLM64,
      VCRuntimeRegistryKey,
      'Version',
      InstalledVersion) then
    Exit;

  if (Length(InstalledVersion) > 0)
    and ((InstalledVersion[1] = 'v') or (InstalledVersion[1] = 'V')) then
    Delete(InstalledVersion, 1, 1);

  if not StrToVersion(InstalledVersion, InstalledPackedVersion) then
    Exit;
  if not StrToVersion('{#VCRedistVersion}', RequiredPackedVersion) then
    RaiseException('The bundled Visual C++ Runtime version is invalid.');

  Result :=
    ComparePackedVersion(InstalledPackedVersion, RequiredPackedVersion) < 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep <> ssInstall) or not VCRuntimeNeedsInstall then
    Exit;

  WizardForm.StatusLabel.Caption :=
    'Installing the Microsoft Visual C++ Runtime...';
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not Exec(
      ExpandConstant('{tmp}\vc_redist.x64.exe'),
      '/install /quiet /norestart',
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode) then
    RaiseException(
      'Could not start the Microsoft Visual C++ Runtime installer: '
      + SysErrorMessage(ResultCode));

  if ResultCode = 3010 then
    VCRuntimeRestartRequired := True
  else if (ResultCode <> 0) and (ResultCode <> 1638) then
    RaiseException(
      Format('The Microsoft Visual C++ Runtime installer failed with exit code %d.', [ResultCode]));
end;

function NeedRestart: Boolean;
begin
  Result := VCRuntimeRestartRequired;
end;

// User data under %APPDATA%\Studio Duo is intentionally not removed on uninstall.
