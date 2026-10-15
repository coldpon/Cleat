// 监听器管理用例。搬运自 api/listeners.go 与 cli/cli.go listener 子命令。
package service

import (
	"net/http"
	"strings"

	"cleat/teamserver/internal/beacon"
	"cleat/teamserver/internal/session"
)

// Listeners 运行中的监听器列表(TCP/HTTPS/DNS/WSS/QUIC)。搬运自 api.listListeners。
func (s *Service) Listeners() []session.ListenerInfo { return s.mgr.ListenerInfos() }

// ListListenerAddrs 监听器地址列表(CLI 视图, 仅地址串)。搬运自 cli 的 mgr.ListListeners 直调。
// 与 Listeners 的差异是两个历史视图(REST 结构化 / CLI 纯地址), 原样保留。
func (s *Service) ListListenerAddrs() []string { return s.mgr.ListListeners() }

// AddListener 动态启动监听器。搬运自 api.addListener 的协议分派主体。
// proto: "tcp"(默认加密盒+伪装配置) / "https"(beacon, 自签证书) /
//        "dns"(beacon, 迷你 DNS/UDP; 需 domain) /
//        "wss"(HTTPS 监听 + WebSocket 路由; wsPath 空 = /ws) /
//        "quic"(beacon, quic-go/UDP + 自签 TLS, ALPN=cleat-quic)。
// 错误分类: domain 缺失/未知协议→invalid(400), 其余→500; 消息原文保留。
func (s *Service) AddListener(op, proto, addr, domain, wsPath string) error {
	switch proto {
	case "tcp":
		if err := s.mgr.AddDefaultListener(addr); err != nil {
			if op != "" {
				s.st.Audit(op, 0, "add_listener", "listener", "error")
			}
			return err
		}
	case "https":
		if s.bh == nil {
			return fail("beacon handler 未启用(服务端不支持 https 监听器)")
		}
		tlsCfg, err := beacon.SelfSignedTLS()
		if err != nil {
			return fail("生成自签证书失败: " + err.Error())
		}
		mux := http.NewServeMux()
		mux.HandleFunc("/poll", s.bh.Poll())
		mux.HandleFunc("/upload", s.bh.Upload())
		if err := s.mgr.AddHTTPSListener(addr, mux, tlsCfg, ""); err != nil {
			if op != "" {
				s.st.Audit(op, 0, "add_listener", "https", "error")
			}
			return err
		}
	case "dns":
		if s.bh == nil {
			return fail("beacon handler 未启用(服务端不支持 dns 监听器)")
		}
		if domain == "" {
			return invalid("dns 模式需要 domain(与 Agent --domain 一致)")
		}
		if err := s.mgr.AddDNSListener(addr, beacon.NewDNSListener(s.bh, domain)); err != nil {
			if op != "" {
				s.st.Audit(op, 0, "add_listener", "dns", "error")
			}
			return err
		}
	case "wss":
		// wss = 骑 HTTPS 监听(同端口同自签证书) 的 WebSocket 通道; 与启动配置
		// (-ws-path/server.toml ws_path) 同一形态, 此处供面板动态创建。
		if s.bh == nil {
			return fail("beacon handler 未启用(服务端不支持 wss 监听器)")
		}
		path := strings.TrimSpace(wsPath)
		if path == "" {
			path = "/ws"
		}
		if !strings.HasPrefix(path, "/") {
			path = "/" + path
		}
		if path == "/poll" || path == "/upload" {
			return invalid("ws 路径与 beacon 固定路由冲突: " + path)
		}
		tlsCfg, err := beacon.SelfSignedTLS()
		if err != nil {
			return fail("生成自签证书失败: " + err.Error())
		}
		mux := http.NewServeMux()
		mux.HandleFunc("/poll", s.bh.Poll())
		mux.HandleFunc("/upload", s.bh.Upload())
		mux.HandleFunc(path, s.bh.WebSocket())
		if err := s.mgr.AddHTTPSListener(addr, mux, tlsCfg, "wss"); err != nil {
			if op != "" {
				s.st.Audit(op, 0, "add_listener", "wss", "error")
			}
			return err
		}
	case "quic":
		if s.bh == nil {
			return fail("beacon handler 未启用(服务端不支持 quic 监听器)")
		}
		ql, err := beacon.NewQUICListener(s.bh, addr)
		if err != nil {
			return fail("创建 QUIC 监听失败: " + err.Error())
		}
		if err := s.mgr.AddQUICListener(addr, ql); err != nil {
			if op != "" {
				s.st.Audit(op, 0, "add_listener", "quic", "error")
			}
			return err
		}
	default:
		return invalid("不支持的协议: " + proto)
	}
	if op != "" {
		s.st.Audit(op, 0, "add_listener", proto, "ok")
	}
	return nil
}

// AddListenerRaw 裸监听(原 CLI `listener add` 语义): AddListener(addr, nil, nil, true)。
// 加密盒(cbox/xbox=nil)但 mimic=true。本批只搬位置, 差异原样保留 —— 不得把两者
// "顺手统一", 加密盒归属属协议/会话层决策。
func (s *Service) AddListenerRaw(op, addr string) error {
	return s.mgr.AddListener(addr, nil, nil, true)
}

// RemoveListener 停止监听器。搬运自 api.deleteListener / cli listener rm。
// 返回错误时入口层映射 404(与原行为一致)。
func (s *Service) RemoveListener(op, addr string) error {
	if err := s.mgr.RemoveListener(addr); err != nil {
		if op != "" {
			s.st.Audit(op, 0, "remove_listener", "listener", "error")
		}
		return err
	}
	if op != "" {
		s.st.Audit(op, 0, "remove_listener", "listener", "ok")
	}
	return nil
}
