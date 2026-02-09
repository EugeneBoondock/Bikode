#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildArch
  #define BuildArch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\\bin\\x64\\Release"
#endif
#ifndef ProjectRoot
  #define ProjectRoot ".."
#endif
#ifndef OutputDir
  #define OutputDir "..\\dist"
#endif

#define AppName "Bikode"
#define AppExeName "Bikode.exe"

[Setup]
AppId={{D9DD16F7-EF8B-49B0-B3AB-67853D9A49DE}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=BoondockLabs
AppPublisherURL=https://github.com/boondocklabs/bikode
AppComments=Bikode IDE by BoondockLabs (wrapper around Notepad2e)
DefaultDirName={autopf}\\Bikode
DefaultGroupName=Bikode
DisableProgramGroupPage=yes
LicenseFile={#ProjectRoot}\\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=Bikode-Setup-{#AppVersion}-{#BuildArch}
SetupIconFile={#ProjectRoot}\\res\\Biko.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\\{#AppExeName}
VersionInfoCompany=BoondockLabs
VersionInfoProductName=Bikode
VersionInfoDescription=Bikode Setup

#if BuildArch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\\Bikode.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist ignoreversion skipifsourcedoesntexist
Source: "{#ProjectRoot}\\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\\Bikode"; Filename: "{app}\\{#AppExeName}"
Name: "{autodesktop}\\Bikode"; Filename: "{app}\\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\\{#AppExeName}"; Description: "Launch Bikode"; Flags: nowait postinstall skipifsilent
