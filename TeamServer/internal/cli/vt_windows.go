//go:build windows

// Windows 终端 VT 使能(第 5 批 CLI 重写配套, ):
// readline 库的 raw_windows.go 只切 raw 模式,**不开 ENABLE_VIRTUAL_TERMINAL_PROCESSING**
// —— 传统 conhost 下 ANSI 光标/颜色序列原样打印 = 整屏乱码(peng 实测 )。
// 本文件对 CONIN$/CONOUT$ 两个控制台设备开启 VT; SetConsoleMode 作用域是控制台设备
// 本身(非句柄私有), 句柄关闭后设置保持。
// 防重蹈覆辙: 若 VT 使能失败(极老系统), 必须走 plainLoop 朴素模式降级 —— 不要让
// readline 直接在无 VT 终端上跑, 乱码会把操作员挡在门外。
package cli

import (
	"os"

	"golang.org/x/sys/windows"
)

// enableVT 对 stdin/stdout 所属控制台开启虚拟终端处理。
// 返回是否全部成功(失败 = 调用方降级朴素 REPL)。
func enableVT() bool {
	ok := true
	for _, dev := range []string{"CONIN$", "CONOUT$"} {
		h, err := windows.CreateFile(
			windows.StringToUTF16Ptr(dev),
			windows.GENERIC_READ|windows.GENERIC_WRITE,
			windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE,
			nil, windows.OPEN_EXISTING, 0, 0,
		)
		if err != nil {
			ok = false
			continue
		}
		var st uint32
		if err := windows.GetConsoleMode(h, &st); err != nil {
			ok = false
			_ = windows.CloseHandle(h)
			continue
		}
		if err := windows.SetConsoleMode(h, st|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING); err != nil {
			ok = false
		}
		_ = windows.CloseHandle(h)
	}
	return ok
}

// stdinIsPipe 判定 stdin 是否为管道/重定向(REPL 无人值守场景, 如 batch 管道)。
func stdinIsPipe() bool {
	st, err := os.Stdin.Stat()
	if err != nil {
		return false
	}
	return st.Mode()&os.ModeCharDevice == 0
}
