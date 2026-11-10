; Inno Setup 6 script. Build the DLL first (Release, x64).
[Setup]
AppName=DXV Codec for Windows Media Foundation
AppVersion=1.0.0
AppPublisher=Your Name
DefaultDirName={commonpf64}\DXV Codec
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=DXV-Codec-Setup
Compression=lzma2
SolidCompression=yes
MinVersion=10.0.19041

[Files]
Source: "..\build\Release\dxv_mft.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Run]
; Packaged apps (Media Player, Movies & TV) run in an AppContainer and need
; read access to the DLL: grant ALL APPLICATION PACKAGES (S-1-15-2-1).
Filename: "{sys}\icacls.exe"; Parameters: """{app}"" /grant *S-1-15-2-1:(OI)(CI)(RX) /T"; Flags: runhidden
