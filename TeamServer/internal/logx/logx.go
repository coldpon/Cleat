// Package logx 进程日志: 双 sink 架构(第 5 批交互重写, )。
//
//   - 诊断日志(Printf/Println): 全量进 data/logs/ 日文件(自动轮转), **永不打进
//     操作员 REPL** —— 曾直接写 stdout, 与命令输入搅在一起(本次重写的核心痛点)。
//   - 操作员事件(Notifyf): 会话上线/下线/关键错误等需要操作员立即知晓的事件,
//     经通知函数打到 REPL(Readline 模式经 PrintTransientf 在提示符上方安全打印,
//     不破坏行缓冲; 无 REPL 时打 stdout)。
//
// 时间格式与 Agent 端对齐([15:04:05]), 承重结构不变。
// 对外 API 兼容: Printf/Println/Guard 语义不变(第 4 批 slog 壳内换芯保留)。
package logx

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"
	"time"
)

// logDir 诊断日志目录(相对 server 工作目录; main 保证存在)。
const logDir = "data/logs"

// notifyFn 操作员事件通知函数。nil = 打 stdout(server 无 REPL / 测试)。
// REPL 启动时注入 Shell.PrintTransientf 包装 —— 提示符上方安全打印。
var (
	notifyMu  sync.RWMutex
	notifyFn  func(msg string)
	notifyTag = "[事件]"
)

// SetNotifier 注入 REPL 通知函数(Readline 的 PrintTransientf 包装)。nil 恢复 stdout。
func SetNotifier(fn func(msg string)) {
	notifyMu.Lock()
	notifyFn = fn
	notifyMu.Unlock()
}

// SetNotifyTag 通知行前缀(默认 "[事件]")。
func SetNotifyTag(tag string) {
	notifyMu.Lock()
	notifyTag = tag
	notifyMu.Unlock()
}

// Notifyf 操作员事件: 会话上线/下线/关键错误等需要立即知晓的事件。
// 有 REPL → 提示符上方横幅; 无 REPL → stdout。**不要**用于高频诊断(那是 Printf/文件的事)。
func Notifyf(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	notifyMu.RLock()
	fn, tag := notifyFn, notifyTag
	notifyMu.RUnlock()
	if fn != nil {
		fn(tag + " " + msg)
		return
	}
	fmt.Printf("[%s] %s %s\n", time.Now().Format("15:04:05"), tag, msg)
}

// ---- 诊断日志(文件 sink) ----

// fileSink 诊断日志文件: data/logs/teamserver-YYYYMMDD.log, 按日轮转(切换日自动开新文件)。
// 并发安全: mu 保护文件句柄与换日。
type fileSink struct {
	mu   sync.Mutex
	day  string
	file *os.File
}

var sink = &fileSink{}

// setFile 打开(或随日切换)诊断日志文件。
func (s *fileSink) setFile(now time.Time) *os.File {
	day := now.Format("")
	if s.file != nil && s.day == day {
		return s.file
	}
	if s.file != nil {
		_ = s.file.Close()
	}
	_ = os.MkdirAll(logDir, 0o755)
	f, err := os.OpenFile(filepath.Join(logDir, "teamserver-"+day+".log"),
		os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return nil // 日志文件不可用: 诊断日志丢弃(操作员事件仍走通知), 不阻断主流程
	}
	s.file, s.day = f, day
	return f
}

func (s *fileSink) write(line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	f := s.setFile(time.Now())
	if f != nil {
		_, _ = f.WriteString(line)
	}
}

// ---- slog 内核(第 4 批保留: API 不变, sink 分流) ----

// stampHandler slog.Handler: 输出 `[HH:MM:05] 消息` 到指定 writer。
// 诊断日志写文件 sink; 操作员事件(Notifyf)不经 Handler(直接走通知函数)。
type stampHandler struct {
	mu  sync.Mutex
	w   io.Writer
	now func() time.Time // 测试注入
}

func (h *stampHandler) Enabled(_ context.Context, l slog.Level) bool { return l >= slog.LevelInfo }
func (h *stampHandler) WithAttrs(_ []slog.Attr) slog.Handler         { return h }
func (h *stampHandler) WithGroup(_ string) slog.Handler              { return h }

func (h *stampHandler) Handle(_ context.Context, r slog.Record) error {
	now := time.Now()
	if h.now != nil {
		now = h.now()
	}
	// 换行语义: 消息按规范自带 \n(fmt 语义); 缺失时补一个 —— 避免行间空行。
	msg := r.Message
	if !strings.HasSuffix(msg, "\n") {
		msg += "\n"
	}
	line := fmt.Sprintf("[%s] %s", now.Format("15:04:05"), msg)
	h.mu.Lock()
	_, _ = io.WriteString(h.w, line)
	h.mu.Unlock()
	if f := sink.setFile(now); f != nil {
		sink.mu.Lock()
		_, _ = f.WriteString(line)
		sink.mu.Unlock()
	}
	return nil
}

// logxLogger 包内共享 slog 实例: 诊断行打 stdout(保留历史行为: server 无 REPL 场景
// 控制台诊断可见)+ 文件。REPL 启动时经 SetStdoutQuiet 静默 stdout(诊断只进文件)。
var logxLogger = slog.New(&stampHandler{w: os.Stdout})

// stdoutQuiet REPL 模式标志: 诊断日志不再打 stdout(全部进文件), 避免搅乱命令输入。
var (
	stdoutQuietMu sync.Mutex
	stdoutQuiet   bool
)

// SetStdoutQuiet REPL 模式开关: true = 诊断日志只进文件(stdout 静默)。
func SetStdoutQuiet(quiet bool) {
	stdoutQuietMu.Lock()
	stdoutQuiet = quiet
	stdoutQuietMu.Unlock()
}

// Guard 在 defer 中调用: 捕获 goroutine 内 panic, 记日志+调用栈后不再上抛。
// 本服务是"每连接/每任务一个 goroutine"的模型, 任何一处未捕获 panic 都会
// 带崩整个进程。每个 goroutine 入口必须
// defer Guard —— 长生命周期 goroutine 同时还需有明确退出信号(见 G3 规范)。
// 用法:
//
//	go func() {
//	    defer logx.Guard("tcp-accept")
//	    ...
//	}()
func Guard(scope string) {
	if r := recover(); r != nil {
		// 防重蹈覆辙: 格式串曾 3 动词 2 参数且
		// scope 从未进格式串 —— panic 日志一直丢作用域名, 第三个 %s 输出 MISSING。
		Printf("[panic] %s: %v\n%s\n", scope, r, debug.Stack())
		// panic 属关键错误: 即使 REPL 静默也打通知(操作员必须知道进程差点崩)。
		Notifyf("goroutine panic 已捕获(%s), 详见日志文件", scope)
	}
}

// Printf 输出带时间戳前缀的格式化诊断日志。
// 换行语义与 fmt.Printf 一致, 由调用方 format 自带 \n(便于多段拼接)。
// **REPL 模式下不打 stdout**(SetStdoutQuiet), 全量进 data/logs/ —— 诊断不搅命令输入。
func Printf(format string, args ...interface{}) {
	stdoutQuietMu.Lock()
	quiet := stdoutQuiet
	stdoutQuietMu.Unlock()
	if quiet {
		// REPL 静默: 直接写文件 sink(绕过 stdout handler); 换行语义同 handler(去重)
		msg := fmt.Sprintf("[%s] "+format, append([]interface{}{time.Now().Format("15:04:05")}, args...)...)
		if !strings.HasSuffix(msg, "\n") {
			msg += "\n"
		}
		sink.write(msg)
		return
	}
	logxLogger.Info(fmt.Sprintf(format, args...))
}

// Println 输出带时间戳前缀的诊断日志行(操作数间加空格并换行, 与 fmt.Println 一致)。
func Println(args ...interface{}) {
	stdoutQuietMu.Lock()
	quiet := stdoutQuiet
	stdoutQuietMu.Unlock()
	if quiet {
		// Sprintln 自带尾部 \n → 不再补, 避免空行
		sink.write(fmt.Sprintf("[%s] %s", time.Now().Format("15:04:05"), fmt.Sprintln(args...)))
		return
	}
	logxLogger.Info(fmt.Sprintln(args...))
}

// LogPath 返回当前诊断日志文件路径(REPL `logs` 命令展示用)。
func LogPath() string {
	sink.mu.Lock()
	defer sink.mu.Unlock()
	f := sink.setFile(time.Now())
	if f == nil {
		return ""
	}
	return f.Name()
}
