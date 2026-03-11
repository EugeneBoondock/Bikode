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
#ifndef OutputBaseName
  #define OutputBaseName "Bikode-Setup-{#AppVersion}-{#BuildArch}"
#endif

#define AppName "Bikode"
#define AppExeName "Bikode.exe"

[Setup]
AppId={{D9DD16F7-EF8B-49B0-B3AB-67853D9A49DE}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Boondock Labs
AppPublisherURL=https://boondocklabs.co.za/
AppSupportURL=https://www.bikode.co.za/
AppUpdatesURL=https://www.bikode.co.za/
AppComments=Bikode IDE built by Boondock Labs. Visit https://www.bikode.co.za/
DefaultDirName={autopf}\\Bikode
DefaultGroupName=Bikode
DisableProgramGroupPage=yes
LicenseFile={#ProjectRoot}\\installer\\BikodeLicense.txt
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
SetupIconFile={#ProjectRoot}\\res\\biko_white.ico
WizardImageFile={#ProjectRoot}\\res\\InstallerWizardLarge.bmp
WizardSmallImageFile={#ProjectRoot}\\res\\InstallerWizardSmall.bmp
WizardBackColor=$181818
WizardBackImageFile={#ProjectRoot}\\res\\InstallerWizardBackground-100.png,{#ProjectRoot}\\res\\InstallerWizardBackground-150.png
WizardBackImageOpacity=168
Compression=lzma
SolidCompression=yes
WizardStyle=modern dark hidebevels includetitlebar
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\\{#AppExeName}
VersionInfoCompany=Boondock Labs
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

[Code]
procedure InitializeWizard;
begin
  WizardForm.WelcomeLabel1.Caption :=
    'Install Bikode by Boondock Labs with the same dark shell, grey grain, and real logo as the editor.';
  WizardForm.WelcomeLabel2.Caption :=
    'Official websites: boondocklabs.co.za for the builders and bikode.co.za for the IDE.';
  WizardForm.FinishedHeadingLabel.Caption := 'Bikode is ready to launch';
  WizardForm.FinishedLabel.Caption :=
    'Open Bikode to jump into the editor, terminal, and AI workspace at https://www.bikode.co.za/.';
end;

