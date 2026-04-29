[Setup]
AppName=FluxPlayer
AppVersion=0.1.0
AppPublisher=FluxPlayer
DefaultDirName={autopf}\FluxPlayer
DefaultGroupName=FluxPlayer
OutputDir=..\dist
OutputBaseFilename=FluxPlayer-0.1.0-Setup
SetupIconFile=..\source\pic.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "..\build\Release\FluxPlayer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\*.dll";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs
Source: "..\build\Release\shaders\*";      DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs
Source: "..\build\Release\fonts\*";        DestDir: "{app}\fonts";   Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\FluxPlayer";        Filename: "{app}\FluxPlayer.exe"
Name: "{group}\Uninstall";         Filename: "{uninstallexe}"
Name: "{commondesktop}\FluxPlayer"; Filename: "{app}\FluxPlayer.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "创建桌面快捷方式"; GroupDescription: "附加任务"

[Run]
Filename: "{app}\FluxPlayer.exe"; Description: "启动 FluxPlayer"; Flags: nowait postinstall skipifsilent
