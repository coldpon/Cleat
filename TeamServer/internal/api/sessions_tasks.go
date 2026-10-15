package api

import (
	"encoding/json"
	"io"
	"net/http"
	"strconv"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
)

// POST /api/sessions/{id}/task 异步提交一条任务(立即返回 task_id, 不阻塞等待)。
// 适合 beacon(HTTPS/DNS/ICMP)通道: 命令入 outbox, 由 Agent 下一次上传响应带回执行。
func (s *Server) submitTask(w http.ResponseWriter, r *http.Request) {
	id := pathInt(r, "id")
	if id <= 0 {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "invalid session id")
		return
	}
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限提交任务")
		return
	}
	var req struct {
		Cmd    string                 `json:"cmd"`
		Params map[string]interface{} `json:"params"`
		Mode   string                 `json:"mode"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON: "+err.Error())
		return
	}
	if req.Cmd == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 cmd")
		return
	}
	tid, err := s.svc.SubmitTask(identity.Username, id, req.Cmd, req.Params, req.Mode)
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"task_id": tid, "status": "ok"})
}

// GET /api/tasks/{id} 查询单任务详情。
func (s *Server) taskDetail(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	tid, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "invalid task id")
		return
	}
	t, ok := s.svc.Task(tid)
	if !ok {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "task not found")
		return
	}
	writeJSON(w, http.StatusOK, t)
}

// POST /api/sessions/{id}/upload-chunk?task_id=&chunk_index=
// body: 原始分块字节。服务端包装为混合帧(JSON头 + '\0' + 二进制)转发给 Agent。
// Web 端无法直接建 TCP, 此接口是上传链路的 HTTP 代理(对齐老 Server)。
func (s *Server) uploadChunk(w http.ResponseWriter, r *http.Request) {
	id := pathInt(r, "id")
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限上传分块")
		return
	}
	taskID := r.URL.Query().Get("task_id")
	chunkIdx := r.URL.Query().Get("chunk_index")
	if taskID == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 task_id")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 50*1024*1024))
	if err != nil || len(body) == 0 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "空分块或读取失败")
		return
	}
	resp, err := s.svc.UploadChunk(id, taskID, chunkIndexInt(chunkIdx), body)
	if err != nil {
		writeInternal(w, apierr.CodeInternalFailed, "内部错误", err)
		return
	}
	writeJSON(w, http.StatusOK, resp)
}

// GET /api/topology 网络拓扑(server/agent 节点 + 连接边)。
//
// 授权: 节点含各会话 hostname(主机清单类信息)。前端当前无消费方(仅 types.ts 占位类型,
// 无 getTopology()/无页面), 无任何 viewer 可见页面依赖 → 与 credentials/targets/listeners
// 读端点同款收敛(CanWrite)。
// 注: 若将来"多跳链路视图"落地于 viewer 可见页面, 需重新评估放开范围并在此补注依据。
func (s *Server) topology(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限查看网络拓扑")
		return
	}
	nodes, links := s.svc.Topology()
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"nodes": nodes,
		"links": links,
	})
}
