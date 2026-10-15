package session

import (
	"cleat/teamserver/internal/store"
)

// APICommand 对指定会话下发 push 直发命令并等待响应。
func (m *Manager) APICommand(sid int, cmd string, params map[string]interface{}) (map[string]interface{}, error) {
	s := m.sessionByID(sid)
	if s == nil {
		return nil, ErrNoSession
	}
	return s.SendCommand(cmd, params)
}

// APIQueueCommand 对指定会话入队 pull 命令并等待响应。
func (m *Manager) APIQueueCommand(sid int, cmd string, params map[string]interface{}) (map[string]interface{}, error) {
	s := m.sessionByID(sid)
	if s == nil {
		return nil, ErrNoSession
	}
	return s.QueueCommand(cmd, params)
}

// SessionInfos 返回所有会话的公开信息。
func (m *Manager) SessionInfos() []Info {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]Info, 0, len(m.sessions))
	for _, s := range m.sessions {
		out = append(out, s.Info())
	}
	return out
}

// APIHybridUpload 对指定会话发送混合帧(JSON头+二进制)并等待响应(文件上传用)。
func (m *Manager) APIHybridUpload(sid int, req map[string]interface{}, bin []byte) (map[string]interface{}, error) {
	s := m.sessionByID(sid)
	if s == nil {
		return nil, ErrNoSession
	}
	return s.SendHybrid(req, bin)
}

// APIHybridPush 对指定会话非阻塞下推一帧混合命令(不等响应, PivotData 数据透传用)。
func (m *Manager) APIHybridPush(sid int, req map[string]interface{}, bin []byte) error {
	s := m.sessionByID(sid)
	if s == nil {
		return ErrNoSession
	}
	return s.SendHybridPush(req, bin)
}

// GetSessionInfo 返回单个会话公开信息。
func (m *Manager) GetSessionInfo(sid int) (Info, bool) {
	s := m.sessionByID(sid)
	if s == nil {
		return Info{}, false
	}
	return s.Info(), true
}

// APISubmitTask 提交一条异步任务(立即返回任务 ID, 不阻塞等待响应)。
func (m *Manager) APISubmitTask(sid int, cmd string, params map[string]interface{}, mode string) (int64, error) {
	return m.SubmitTask(sid, cmd, params, mode)
}

// APITask 按 ID 取任务详情。
func (m *Manager) APITask(id int64) (*store.Task, bool) {
	return m.st.GetTask(id)
}

// APITasks 返回某会话(或全部)任务列表。sid<=0 表示全部。
func (m *Manager) APITasks(sid int) []store.Task {
	if sid > 0 {
		return m.st.TasksBySession(sid)
	}
	return m.st.AllTasks()
}
