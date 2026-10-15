// SOCKS 出口(pivot)/多跳入口/反向端口转发用例。搬运自 api/pivots.go、api/rportfwd.go、
// cli/cli.go(pivotStart/entry/rportfwdCmd)。
package service

import (
	"cleat/teamserver/internal/pivot"
)

// Pivots SOCKS 出口列表(nil 安全, 未启用时返回空表)。搬运自 api.listPivots / cli pivots。
func (s *Service) Pivots() []string {
	if s.piv == nil {
		return []string{}
	}
	return s.piv.Listeners()
}

// PivotInfos SOCKS 出口结构化列表(nil 安全)。REST /api/pivots 用 —— 前端按
// session_id/entry_host 等字段消费(CLI 仍走 Pivots() 的可读字符串)。
func (s *Service) PivotInfos() []pivot.ListenerInfo {
	if s.piv == nil {
		return []pivot.ListenerInfo{}
	}
	return s.piv.ListenerInfos()
}

// Rportfwds 反向端口转发列表(nil 安全)。搬运自 api.listRportfwds / cli rportfwd list。
func (s *Service) Rportfwds() []pivot.RportfwdInfo {
	if s.piv == nil {
		return []pivot.RportfwdInfo{}
	}
	return s.piv.RportfwdInfos()
}

// PivotAdd 为指定会话启动 SOCKS 出口。搬运自 api.addPivot / cli.pivotStart 主体。
func (s *Service) PivotAdd(op string, sid, port int, eHost string, ePort int, eUser, ePass string) error {
	if s.piv == nil {
		return fail("pivot 未启用")
	}
	if err := s.piv.StartSOCKS(sid, port, eHost, ePort, eUser, ePass); err != nil {
		if op != "" {
			s.st.Audit(op, sid, "add_pivot", "pivot", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, sid, "add_pivot", "pivot", "ok")
	}
	return nil
}

// PivotRemove 停止 SOCKS 出口。搬运自 api.deletePivot。错误时入口层映射 404。
func (s *Service) PivotRemove(op, addr string) error {
	if s.piv == nil {
		return fail("pivot 未启用")
	}
	if err := s.piv.Stop(addr); err != nil {
		if op != "" {
			s.st.Audit(op, 0, "remove_pivot", "pivot", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, 0, "remove_pivot", "pivot", "ok")
	}
	return nil
}

// EntryAdd 为指定会话启动多跳入口(TS 生成 RFC1929 凭据下发)。搬运自 api.addEntry / cli.entry。
func (s *Service) EntryAdd(op string, sid, port int) (user, pass string, err error) {
	if s.piv == nil {
		return "", "", fail("pivot 未启用")
	}
	cred, err := s.piv.StartEntry(sid, port)
	if err != nil {
		if op != "" {
			s.st.Audit(op, sid, "add_entry", "pivot", "error")
		}
		return "", "", err
	}
	if op != "" {
		s.st.Audit(op, sid, "add_entry", "pivot", "ok")
	}
	return cred.User, cred.Pass, nil
}

// EntryCredFor 按入口端口取已登记凭据(多跳链式用)。搬运自 cli.pivotStart 的凭据查询。
func (s *Service) EntryCredFor(port int) (user, pass string, ok bool) {
	if s.piv == nil {
		return "", "", false
	}
	cred, ok := s.piv.EntryCredFor(port)
	if !ok {
		return "", "", false
	}
	return cred.User, cred.Pass, true
}

// RportfwdAdd 启动反向端口转发。搬运自 api.addRportfwd / cli.rportfwdCmd 主体。
func (s *Service) RportfwdAdd(op string, sid, lport int, tHost string, tPort int) error {
	if s.piv == nil {
		return fail("pivot 未启用")
	}
	if err := s.piv.StartRportfwd(sid, lport, tHost, tPort); err != nil {
		if op != "" {
			s.st.Audit(op, sid, "add_rportfwd", "pivot", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, sid, "add_rportfwd", "pivot", "ok")
	}
	return nil
}

// PipeEntryAdd 开会话借道入口(TS 生成凭据下发, P1')。
func (s *Service) PipeEntryAdd(op string, sid int, listenAddr string) (pivot.PipeEntry, error) {
	if s.piv == nil {
		return pivot.PipeEntry{}, fail("pivot 未启用")
	}
	entry, err := s.piv.StartPipeEntry(sid, listenAddr)
	if err != nil {
		if op != "" {
			s.st.Audit(op, sid, "add_pipe_entry", "pivot", "error")
		}
		return pivot.PipeEntry{}, err
	}
	if op != "" {
		s.st.Audit(op, sid, "add_pipe_entry", "pivot", "ok")
	}
	return entry, nil
}

// PipeEntryRemove 关闭会话借道入口。
func (s *Service) PipeEntryRemove(op, pipeName string) error {
	if s.piv == nil {
		return fail("pivot 未启用")
	}
	if err := s.piv.StopPipeEntry(pipeName); err != nil {
		if op != "" {
			s.st.Audit(op, 0, "remove_pipe_entry", "pivot", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, 0, "remove_pipe_entry", "pivot", "ok")
	}
	return nil
}

// PipeEntries 会话借道入口凭据列表(nil 安全)。
func (s *Service) PipeEntries() []pivot.PipeEntry {
	if s.piv == nil {
		return []pivot.PipeEntry{}
	}
	return s.piv.PipeEntryInfos()
}

// VirtualLinks 会话借道链路边列表(nil 安全)。
func (s *Service) VirtualLinks() []pivot.VirtualLinkInfo {
	if s.piv == nil {
		return []pivot.VirtualLinkInfo{}
	}
	return s.piv.VirtualLinkInfos()
}

// RportfwdRemove 停止反向端口转发。搬运自 api.deleteRportfwd / cli.rportfwdCmd stop。错误映射 404。
func (s *Service) RportfwdRemove(op string, sid, lport int) error {
	if s.piv == nil {
		return fail("pivot 未启用")
	}
	if err := s.piv.StopRportfwd(sid, lport); err != nil {
		if op != "" {
			s.st.Audit(op, sid, "remove_rportfwd", "pivot", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, sid, "remove_rportfwd", "pivot", "ok")
	}
	return nil
}
