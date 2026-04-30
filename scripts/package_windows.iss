; Windows 安装包配置脚本（Inno Setup 6）
;
; 用法：由 package_windows.ps1 自动调用，不建议直接运行
;   版本号通过命令行参数注入：ISCC.exe /DAppVersion=x.x.x
;   发版只需修改 CMakeLists.txt 中的 project VERSION，无需改此文件
;
; 输出：dist\FluxPlayer-{版本号}-Setup.exe

[Setup]
AppName=FluxPlayer
; {#AppVersion} 由 package_windows.ps1 通过 /DAppVersion 参数传入
AppVersion={#AppVersion}
AppPublisher=FluxPlayer
; {autopf} = C:\Program Files 或 C:\Program Files (x86)，自动适配系统位数
DefaultDirName={autopf}\FluxPlayer
DefaultGroupName=FluxPlayer
; 安装包输出目录（相对于 .iss 文件位置）
OutputDir=..\dist
OutputBaseFilename=FluxPlayer-{#AppVersion}-Setup
SetupIconFile=..\source\pic.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
; 主程序
Source: "..\build\FluxPlayer.exe"; DestDir: "{app}"; Flags: ignoreversion
; FFmpeg DLL 及 MinGW 运行时（*.dll）
Source: "..\build\*.dll";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs
; GLSL 着色器（运行时从可执行文件目录相对路径加载）
Source: "..\build\shaders\*";      DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs
; 字体文件（ImGui 渲染字幕和 UI 使用）
Source: "..\build\fonts\*";        DestDir: "{app}\fonts";   Flags: ignoreversion recursesubdirs

[UninstallDelete]
; 卸载时清理运行时生成的文件
Type: files; Name: "{app}\imgui.ini"

[Icons]
; 开始菜单快捷方式
Name: "{group}\FluxPlayer";         Filename: "{app}\FluxPlayer.exe"
Name: "{group}\Uninstall";          Filename: "{uninstallexe}"
; 桌面快捷方式（可选，由 Tasks 控制）
Name: "{commondesktop}\FluxPlayer"; Filename: "{app}\FluxPlayer.exe"; Tasks: desktopicon

[Tasks]
; 安装向导中显示的可选任务
Name: desktopicon; Description: "创建桌面快捷方式"; GroupDescription: "附加任务"

[Run]
; 安装完成后可选择立即启动
Filename: "{app}\FluxPlayer.exe"; Description: "启动 FluxPlayer"; Flags: nowait postinstall skipifsilent

[Code]
// 卸载时清理平台缓存目录（%LOCALAPPDATA%\FluxPlayer）
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
    if CurUninstallStep = usPostUninstall then
    begin
        if MsgBox('是否同时删除配置文件、日志和录制文件？' + #13#10 +
                  '(' + ExpandConstant('{localappdata}\FluxPlayer') + ')',
                  mbConfirmation, MB_YESNO) = IDYES then
        begin
            DelTree(ExpandConstant('{localappdata}\FluxPlayer'), True, True, True);
        end;
    end;
end;
