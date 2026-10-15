// 任务提交与查询用例。搬运自 api/sessions_tasks.go(submitTask/taskDetail)与 cli/cli_tasks.go。
//
// 旧病灶: 服务层直接构造 store.Task 生命周期 + goroutine 同步等待回写, 复刻
// session/tasks.go 派发流程, 不走 compensate/防双执行 —— 同时造成: 状态撒谎
// (帧未发即 dispatched)/崩溃恢复重派缺字节/占会话 cmdMu 最长 60s。
// 现在: ModuleLoad 的参数校验在此层完成, 字节获取经 payloadResolver 注入
// (SetModuleRepo), 派发/等待/回写/超时/补偿全部走 Manager.SubmitTask 唯一路径。
// SubmitTaskPlain 已合并(差异曾是历史路径分支, 唯一路径后无存在理由), CLI/API 同入口。
package service

import "fmt"

// Task/Tasks 的返回类型经 service 包级别名(store.Task), 本文件无需直连 store 导入。

// SubmitTask 异步提交一条任务(立即返回 task_id, 不阻塞等待)。
// 适合 beacon(HTTPS/DNS/ICMP)通道: 命令入 outbox, 由 Agent 下一次上传响应带回执行。
// 大载荷命令(ModuleLoad)的校验在函数内完成, 字节随帧经 resolver 附带。
// 错误分类: 请求性失败→invalid(400), 其余→500, 消息原文保留。
func (s *Service) SubmitTask(op string, sid int, cmd string, params map[string]interface{}, mode string) (int64, error) {
	// O-2(TS收尾E2E): 对不存在的会话提交任务原先照常 pending(永远无人派发)。
	// 会话不在持久层(从未注册/已被清除)时直接 400; 离线会话有持久记录仍允许提交,
	// 由上线补派(reuseSession/BeaconGet/poll)承接。
	if s.st.Get(sid) == nil {
		return 0, invalid(fmt.Sprintf("会话 %d 不存在", sid))
	}
	// ModuleLoad 前置校验(原病灶分支仅存的部分): 缺 module_id/模块不在仓库 → 400,
	// 读哈希失败 → 500 拒绝下发。字节本身不在此读 —— 派发时由 resolver 取(崩溃
	// 恢复重派也能重新取到, 修复"重派缺字节"复合 bug)。
	if cmd == "ModuleLoad" {
		if s.modRepo == nil {
			return 0, fail("模块仓库未初始化")
		}
		mid, _ := params["module_id"].(string)
		if mid == "" {
			return 0, invalid("缺少 module_id")
		}
		info, err := s.modRepo.Info(mid)
		if err != nil || info.SHA256 == "" {
			return 0, fail("读取模块哈希失败, 拒绝下发")
		}
		if _, ok := params["sha256"]; !ok {
			if params == nil {
				params = map[string]interface{}{}
			}
			params["sha256"] = info.SHA256
		}
	}
	// 注意: 不再调 EnsureModuleSha256 —— 它仅对 ModuleLoad 生效(command.go 判 cmd),
	// 与上方校验块重复, 且其裸 error 经 invalid 包装会把"读哈希失败"错标成 400
	// (原语义 500)。它继续服务 api 的 hybridCommand 通道。
	tid, err := s.mgr.APISubmitTask(sid, cmd, params, mode)
	if err != nil {
		return 0, err
	}
	if op != "" {
		s.st.Audit(op, sid, cmd, "task", "queued")
	}
	return tid, nil
}

// Task 按 ID 取任务详情(原 api.taskDetail / cli.taskInfo 数据源)。
func (s *Service) Task(tid int64) (*Task, bool) { return s.mgr.APITask(tid) }

// Tasks 某会话(或全部, sid<=0)任务列表(原 cli.taskList 数据源)。
func (s *Service) Tasks(sid int) []Task { return s.mgr.APITasks(sid) }
