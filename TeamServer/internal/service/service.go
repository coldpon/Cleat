// Package service 服务层: 一个导出方法 = 一个操作员用例。
// 入口层(api/cli)只做参数转换/权限判断/呈现, 业务与审计一律经此层。
// 依赖方向(G1): service → 领域层(session/pivot/modules/beacon/artifact/auth) → 基础层(store/logx)。
//
// 第 2 批结构手术: 方法体自 api/cli 原样搬入, 只换位置不改逻辑。
//
// 审计约定: 方法签名带 op(操作员名)时, op=="" 表示本地 CLI 入口 —— CLI 操作历史上
// 不落审计, 以此约定保持与搬运前零行为差异; HTTP 入口恒传真实用户名。
package service

import (
	"errors"
	"sync"

	"cleat/teamserver/internal/artifact"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/beacon"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/modules"
	"cleat/teamserver/internal/pivot"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// 跨包别名与分类哨兵: 入口层引用这些标识时不再直连领域/基础层。
var (
	ErrNoSession = session.ErrNoSession

	ErrModuleExists   = modules.ErrModuleExists
	ErrModuleNotFound = modules.ErrModuleNotFound

	// 错误分类哨兵(Error.Class 的取值), 入口层映射: invalid→400 / notFound→404 / auth→401。
	ErrInvalid  = errors.New("invalid request")
	ErrNotFound = errors.New("not found")
	ErrAuth     = errors.New("auth failed")
)

const RoleViewer = store.RoleViewer

type (
	Task           = store.Task
	AuditFilter    = store.AuditFilter
	EventFilter    = store.EventFilter
	ArtifactSpec   = artifact.Spec
	ArtifactResult = artifact.Result
)

// Error 分类错误: Msg 保持业务原文(入口层原样回给调用方), Class 供入口层映射状态码。
// 引入原因: 原 handler 在同一函数内对不同失败点返回不同 HTTP 状态码; 业务下沉后由
// Class 保留该信息, 消息零改动。未分类错误一律按内部错误(500)处理(见 api.statusFromServiceErr)。
type Error struct {
	Class error
	Msg   string
}

func (e *Error) Error() string { return e.Msg }
func (e *Error) Unwrap() error { return e.Class }

func invalid(msg string) error  { return &Error{Class: ErrInvalid, Msg: msg} }  // 400
func notFound(msg string) error { return &Error{Class: ErrNotFound, Msg: msg} } // 404
func authFail(msg string) error { return &Error{Class: ErrAuth, Msg: msg} }     // 401
func fail(msg string) error     { return &Error{Msg: msg} }                     // 未分类(500)

// Service 服务层聚合根。st 恒非 nil(New 契约, main 必传)—— 原 handler 里散落的
type Service struct {
	mgr     *session.Manager
	st      *store.Store
	piv     *pivot.Pivot    // 可为 nil: 未启用 pivot
	modRepo *modules.Repo   // 可为 nil: 未启用模块仓库
	bh      *beacon.Handler // 可为 nil: 未启用 beacon(https/dns 动态监听器)
	authH   *auth.Authenticator

	// 装配: main.go 经 SetArtifact 注入; 未装配时由 artifactEnv 惰性兜底(默认 ./artifacts)。
	artifactMu           sync.Mutex             // 保护下列字段的惰性初始化
	artifactOutRoot      string                 // 产物根(空=./artifacts)
	artifactStubDir      string                 // 外部 stub 目录覆盖(空=仅 go:embed 内置)
	artifactAllowCompile bool                   // 现场编译路径(默认 false; 见 artifact.Generate)
	stubReg              *artifact.StubRegistry // 预编译 stub 注册表(惰性)
	gen                  *artifact.Generator    // 产物环境(惰性)
}

// New 创建服务层。token 为 fallback bearer(转发给认证器, 语义同原 api.New 的 auth.New(st, token))。
func New(mgr *session.Manager, st *store.Store, piv *pivot.Pivot, token string) *Service {
	return &Service{mgr: mgr, st: st, piv: piv, authH: auth.New(st, token)}
}

// SetModuleRepo 设置模块仓库(保持 main.go 原时序: repo 构造晚于 service 创建)。
// 由 dispatchTask 派发时经本闭包获取 —— session 包不知道 modules 包, 分层不破。
// 曾同时造成状态撒谎/崩溃恢复重派缺字节/cmdMu 占 60s, 已随第 3 批收编进唯一派发路径。
func (s *Service) SetModuleRepo(r *modules.Repo) {
	s.modRepo = r
	if r != nil && s.mgr != nil {
		s.mgr.SetPayloadResolver(func(sid int, cmd string, params map[string]interface{}) ([]byte, error) {
			if cmd != "ModuleLoad" {
				return nil, nil // 无载荷命令
			}
			mid, _ := params["module_id"].(string)
			if mid == "" {
				return nil, errors.New("缺少 module_id")
			}
			return r.Get(mid)
		})
	}
}

// SetBeaconHandler 设置 beacon 处理器(保持 main.go 原时序)。
func (s *Service) SetBeaconHandler(bh *beacon.Handler) { s.bh = bh }

// Auth 暴露认证器(HTTP 中间件/登录用)。
func (s *Service) Auth() *auth.Authenticator { return s.authH }

// ManagerForTest **仅测试使用**: 暴露底层 Manager 供跨包测试构造补偿场景
// (如 ModuleLoad 离线提交→上线补偿)。生产入口层不得经此绕过 service 用例方法。
func (s *Service) ManagerForTest() *session.Manager { return s.mgr }

// devModeWarnOnce 开发态无认证模式只打一次红字警告到 stderr(搬运自 api 中间件;
// api 不再持 logx, 经此入口保持原行为)。
var devModeWarnOnce sync.Once

// DevModeWarn 开发态警告(幂等, 仅一次)。
func (s *Service) DevModeWarn() {
	devModeWarnOnce.Do(func() {
		logx.Printf("⚠️  开发模式: 未配置 token 且无操作员, 任何请求视为 viewer。请勿公网部署!\n")
	})
}
