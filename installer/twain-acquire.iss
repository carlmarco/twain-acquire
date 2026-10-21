; This Source Code Form is subject to the terms of the Mozilla Public
; License, v. 2.0. If a copy of the MPL was not distributed with this
; file, You can obtain one at https://mozilla.org/MPL/2.0/.

; Inno Setup script. Built by the release workflow:
;   iscc /DAppVersion=1.2.3 /DStageDir=<stage> installer\twain-acquire.iss
; <stage>\app  - the application, its helpers and Qt (windeployqt output)
; <stage>\dsm  - TWAIN Data Source Manager 2.5.1: 32\TWAINDSM.dll, 64\TWAINDSM.dll
; <stage>\vc_redist.x64.exe - Microsoft Visual C++ runtime installer
; <stage>\LICENSE.txt, <stage>\THIRD_PARTY_NOTICES.txt

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\stage"
#endif

[Setup]
AppId={{6C1E2B0A-3F4D-4B8E-9A57-2D8F1C0E7B31}
AppName=twain-acquire
AppVersion={#AppVersion}
AppVerName=twain-acquire {#AppVersion}
AppPublisher=The twain-acquire contributors
DefaultDirName={autopf}\twain-acquire
DefaultGroupName=twain-acquire
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
LicenseFile={#StageDir}\LICENSE.txt
OutputDir=output
OutputBaseFilename=twain-acquire-{#AppVersion}-setup
UninstallDisplayIcon={app}\twain-acquire.exe
CloseApplications=yes

[Tasks]
Name: "twaindsm"; Description: "Install the TWAIN Data Source Manager 2.5.1 where none is installed"
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#StageDir}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
; A private copy of each DSM beside the helpers, used when the system copy is
; missing or the wrong bitness.
Source: "{#StageDir}\dsm\32\TWAINDSM.dll"; DestDir: "{app}\dsm32"; Flags: ignoreversion
Source: "{#StageDir}\dsm\64\TWAINDSM.dll"; DestDir: "{app}\dsm64"; Flags: ignoreversion
; The DSM is shared by every TWAIN program on the machine: never replace an
; existing one and never remove it on uninstall.
Source: "{#StageDir}\dsm\64\TWAINDSM.dll"; DestDir: "{sys}"; Flags: onlyifdoesntexist uninsneveruninstall 64bit; Tasks: twaindsm
Source: "{#StageDir}\dsm\32\TWAINDSM.dll"; DestDir: "{syswow64}"; Flags: onlyifdoesntexist uninsneveruninstall 32bit; Tasks: twaindsm

[Icons]
Name: "{autoprograms}\twain-acquire"; Filename: "{app}\twain-acquire.exe"
Name: "{autodesktop}\twain-acquire"; Filename: "{app}\twain-acquire.exe"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: waituntilterminated
Filename: "{app}\twain-acquire.exe"; Description: "Start twain-acquire"; Flags: nowait postinstall skipifsilent
