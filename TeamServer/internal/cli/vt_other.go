//go:build !windows

// 非 Windows: POSIX 终端原生处理 ANSI, 无需 VT 使能。
package cli

func enableVT() bool { return true }

func stdinIsPipe() bool {
	st, err := os.Stdin.Stat()
	if err != nil {
		return false
	}
	return st.Mode()&os.ModeCharDevice == 0
}
