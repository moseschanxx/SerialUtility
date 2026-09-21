; Inno Setup 6 script for the BuildAI Serial Utility Windows installer.
;
; Compile (CI or locally, after `scripts\build.ps1 -Config Release -Deploy`):
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=..\..\dist\Release\bin /DOutputDir=..\..\dist\installer BuildAI-SerialUtility.iss
; SourceDir must be the windeployqt output (exe + Qt DLLs + plugin folders).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Release\bin"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist\installer"
#endif

#define AppName "BuildAI Serial Utility"
#define AppExeName "BuildAI-SerialUtility.exe"
#define AppPublisher "BuildAI"
#define AppURL "https://build.ai"

[Setup]
AppId={{9B40AC82-DDF5-4ACC-AD60-A73B55A9FF25}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\BuildAI\SerialUtility
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=BuildAI-SerialUtility-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\resources\icons\buildai.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=..\..\packaging\windows\license.txt
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} installer
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; windeployqt output only; user data (QSettings, quick_commands.json, logs) is deliberately kept.
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\tls"
