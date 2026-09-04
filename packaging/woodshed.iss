; Inno Setup script for Woodshed.
;
; Build it with:
;     "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" packaging\woodshed.iss
;
; It expects the app to have been built first, in Release, into the "build" tree at the repository
; root - see BuildDir below. The result lands in packaging\output.
;
; What this deliberately does NOT do: install a Visual C++ redistributable. The application links
; the C++ runtime statically (see CMAKE_MSVC_RUNTIME_LIBRARY in the root CMakeLists), so the only
; DLLs it imports ship with Windows itself. One file, no prerequisites.

#define AppName        "Woodshed"
#define AppVersion     "0.1.0"
#define AppPublisher   "Anders Nordlander"
#define AppExeName     "Woodshed.exe"
#define AppUrl         "https://github.com/carlandersnordlander/Woodshed"
#define BuildDir       "..\build\standalone\Release"

[Setup]
; A GUID of its own, generated once and never changed: it is how Windows recognises an upgrade of
; this application rather than a second copy of it, and how the uninstaller finds what it installed.
AppId={{6E1F2B84-9C3A-4D77-B0E5-3A1D8F4C27B9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}

; Per-user, under Local AppData. No administrator prompt, which matters: a person who has just
; compiled a program themselves should not have to hand it the whole machine to try it. It also
; keeps the app out of Program Files, where it could not write beside itself anyway.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}

; 64-bit only. The audio engine, the ASIO wrapper and the DSP are all built x64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir=output
OutputBaseFilename=Woodshed-{#AppVersion}-setup
SetupIconFile=..\standalone\woodshed.ico
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; The licence the person is agreeing to, shown before anything is written.
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "swedish";  MessagesFile: "compiler:Languages\Swedish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; The captures that come with the source, so a fresh install has something to load into a block and
; is not silent until the person has found a capture of their own.
Source: "..\example_models\*.nam"; DestDir: "{app}\example captures"; Flags: ignoreversion

; What it is, what it is built on, and under what licence - readable without a network connection.
Source: "..\README.md";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";         DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md";  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Only what the installer put there. Settings, saved rigs, projects, separated stems and any Python
; environment live in %APPDATA%\Woodshed and are deliberately left alone: uninstalling an
; application is not the same as saying the work done with it can go.
Type: filesandordirs; Name: "{app}\example captures"
