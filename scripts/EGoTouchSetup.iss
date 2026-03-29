; EGoTouch 自带打包安装脚本 (Inno Setup)
; 请使用 Inno Setup 6.x 编译此脚本

[Setup]
; 基础应用信息
AppName=EGoTouch Controller Software
AppVersion=1.0.0
AppPublisher=EGoTouch Team
AppPublisherURL=https://github.com/awarson2233/EGoTouchRev-rebuild

; 安装文件生成的输出路径和包名
OutputDir=..\build
OutputBaseFilename=EGoTouch_Setup_v1.0.0
Compression=lzma2/ultra64
SolidCompression=yes

; 关键项：明确要求以管理员权限运行（安装系统服务必备）
PrivilegesRequired=admin

; 明确指定这是一个原生 ARM64 安装程序
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64

; 默认安装路径，{autopf} 会根据系统自动映射到 Program Files (x86) 或 Program Files
DefaultDirName={autopf}\EGoTouchRev
DefaultGroupName=EGoTouch

; 简化安装向导外观
DisableWelcomePage=no
DisableDirPage=no

; 允许用户在桌面创建快捷方式
[Tasks]
Name: "desktopicon"; Description: "在桌面创建 EGoTouch 诊断控制台快捷方式"; GroupDescription: "附加任务:"

[Files]
; 复制主服务程序和相关文件
Source: "..\build\EGoTouchService.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\EGoTouchApp.exe"; DestDir: "{app}"; Flags: ignoreversion
; 复制任何可能的依赖项（如 config.ini），如果不存在可以先忽略。注：当前 build 目录下存在 config.ini
Source: "..\build\config.ini"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
; 在开始菜单创建 App（诊断控制台）的入口
Name: "{group}\EGoTouch Diagnostics Workbench"; Filename: "{app}\EGoTouchApp.exe"
Name: "{group}\卸载 EGoTouch"; Filename: "{uninstallexe}"
; 勾选创建桌面快捷方式时，生成快捷方式
Name: "{commondesktop}\EGoTouch 诊控面板"; Filename: "{app}\EGoTouchApp.exe"; Tasks: desktopicon

[Run]
; 安装阶段完毕后，自动执行以下命令 (runhidden=后台隐式执行) \
; --install 是在代码 ServiceEntry.cpp 里写的自带服务注册命令
Filename: "{app}\EGoTouchService.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated
; 我们再利用系统 sc 启动服务
Filename: "{sys}\sc.exe"; Parameters: "start EGoTouchService"; Flags: runhidden waituntilterminated

[UninstallRun]
; 卸载阶段开始时，首切需要把服务关掉并自动移除，防卸载文件锁定
; 先尝试停止系统服务
Filename: "{sys}\sc.exe"; Parameters: "stop EGoTouchService"; Flags: runhidden waituntilterminated
; 删除服务
Filename: "{app}\EGoTouchService.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated

[UninstallDelete]
; 移除日志文件和其他由程序生成的运行文件，确保卸载干净
Type: filesandordirs; Name: "C:\ProgramData\EGoTouchRev"
