package api

import (
	"net/http"
	"strconv"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/service"
)

// GET /api/audit 审计日志查询(分页 + 过滤)。
//
// 过滤参数: operator(操作员模糊) / session_id / cmd(命令模糊) / mode / status /
// start / end(毫秒时间戳) / limit / offset。
// 权限: viewer 及以上可读(auth 中间件已认证)。审计为 append-only,
// 删除/导出能力(后续)仅 admin。
func (s *Server) auditList(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	f := service.AuditFilter{
		Operator: q.Get("operator"),
		Cmd:      q.Get("cmd"),
		Mode:     q.Get("mode"),
		Status:   q.Get("status"),
		Limit:    atoiDefault(q.Get("limit"), 100),
		Offset:   atoiDefault(q.Get("offset"), 0),
	}
	if sid := q.Get("session_id"); sid != "" {
		if v, err := strconv.Atoi(sid); err == nil && v > 0 {
			f.SessionID = &v
		}
	}
	f.Start = parseInt64Default(q.Get("start"), 0)
	f.End = parseInt64Default(q.Get("end"), 0)
	items, total, err := s.svc.AuditList(f)
	if err != nil {
		writeInternal(w, apierr.CodeInternalFailed, "查询审计失败", err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"items": items, "total": total})
}

// GET /api/events/history 历史事件(结果型事件落盘后从这里读)。
//
// 过滤参数: type(事件类型) / session_id / start / end / limit / offset。
// 权限: viewer 及以上可读。
func (s *Server) eventsHistory(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	f := service.EventFilter{
		Type:   q.Get("type"),
		Limit:  atoiDefault(q.Get("limit"), 200),
		Offset: atoiDefault(q.Get("offset"), 0),
	}
	if sid := q.Get("session_id"); sid != "" {
		if v, err := strconv.Atoi(sid); err == nil && v > 0 {
			f.SessionID = &v
		}
	}
	f.Start = parseInt64Default(q.Get("start"), 0)
	f.End = parseInt64Default(q.Get("end"), 0)
	items, total, err := s.svc.EventsHistory(f)
	if err != nil {
		writeInternal(w, apierr.CodeInternalFailed, "查询事件失败", err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"items": items, "total": total})
}

// atoiDefault 解析非负整数, 空/非法回退默认值。
func atoiDefault(v string, def int) int {
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < 0 {
		return def
	}
	return n
}

// parseInt64Default 解析非负 64 位整数, 空/非法回退默认值。
func parseInt64Default(v string, def int64) int64 {
	if v == "" {
		return def
	}
	n, err := strconv.ParseInt(v, 10, 64)
	if err != nil || n < 0 {
		return def
	}
	return n
}
