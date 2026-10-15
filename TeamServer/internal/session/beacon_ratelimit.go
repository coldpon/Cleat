// beacon 通道入站帧限流: per-session 滑动窗, 防失控 Agent 打爆服务端。
// 背景(peng  立案): Agent 侧日志循环上报类 bug 会以极高频率 POST 上行数据,
// 服务端此前对已注册会话的入站帧零限流 —— 单会话即可打满 CPU/带宽。
// 阈值依据: Agent 忙时节拍上限 = 1 req/s(PollClock fast_clock 固定 1s, 不抖不丢),
// 10 帧/s = 正常流量的 10 倍余量, 常态永远碰不到。
// 覆盖面: 三条 beacon 通道(HTTPS/DNS/ICMP)共用 Manager 帧入口, 在 HTTP handler 之上。
// TCP 长连接不在列: runLoop 单线程串行读单连接, 失控会话天然隔离, 不构成跨会话放大。
//
// 动作铁律: **静默丢帧, 绝不回错误** —— GET 返回空(=无帧语义), POST 返回"忽略本帧"
// (与 TCP 冲突帧同一形态, 调用方零改动)。Agent 完全无感, 继续按节拍睡下一觉。
// 历史教训: 对 beacon 回错误码会诱发重连/重注册风暴(匿名会话风暴一族, 见 BeaconFrame 注释),
// 风暴的破坏力远大于丢几帧数据。丢帧代价 = 该帧数据缺口, WARN 日志可见(每 key 10s 至多一条,
// 防告警风暴 —— 限流器自己不能成为新的洪水源)。
//
// 防泄漏(照抄 A1-1 教训): 窗口裁剪时空条目即删; 键 10min 无命中整体删除(离线会话不留尸)。
package session

import (
	"net"
	"strconv"
	"sync"
	"time"

	"cleat/teamserver/internal/logx"
)

// beaconIngestLimit 单键每秒放行帧数上限。
const beaconIngestLimit = 10

// ingestKeyTTL 键条目无命中后的存活期(离线会话清理)。
const ingestKeyTTL = 10 * time.Minute

// ingestWarnInterval 同键告警节流间隔。
const ingestWarnInterval = 10 * time.Second

var (
	ingestMu   sync.Mutex
	ingestHits = map[string][]time.Time{} // 键: "s:<sid>"(已注册会话) / "ip:<host>"(注册期/解析失败帧)
	ingestWarn = map[string]time.Time{}   // 上次告警时刻(告警节流)
)

// ingestAllowed 报告该键本帧是否放行, 并记录命中。滑动 1s 窗。
// now 参数化: 测试可注入时钟。
func ingestAllowed(key string, now time.Time) bool {
	ingestMu.Lock()
	defer ingestMu.Unlock()

	hits := ingestHits[key][:0]
	for _, t := range ingestHits[key] {
		if t.After(now.Add(-time.Second)) {
			hits = append(hits, t)
		}
	}
	if len(hits) == 0 {
		delete(ingestHits, key)
	} else if now.Sub(hits[len(hits)-1]) > ingestKeyTTL {
		// 键整体 TTL: 最新命中已超 TTL → 连键带告警条目一并清理(离线会话防泄漏)。
		delete(ingestHits, key)
		delete(ingestWarn, key)
		hits = hits[:0]
	} else {
		ingestHits[key] = hits
	}
	if len(hits) >= beaconIngestLimit {
		if last, ok := ingestWarn[key]; !ok || now.Sub(last) > ingestWarnInterval {
			logx.Printf("[!] beacon 入站限流: %s 达 %d 帧/s, 丢帧(防失控循环)\n", key, len(hits))
			ingestWarn[key] = now
		}
		return false
	}
	ingestHits[key] = append(hits, now)
	return true
}

// ingestKeyForFrame 按会话路由参数选限流键: 已注册会话用会话键, 其余(注册期/
// session_id 解析失败帧)退 IP 键 —— 解析失败帧以 IP 聚合(历史 #99→#113 同族场景)。
func ingestKeyForFrame(sid int, remoteIP string) string {
	if sid > 0 {
		return ingestKeySession(sid)
	}
	return ingestKeyIP(remoteIP)
}

func ingestKeySession(sid int) string { return "s:" + strconv.Itoa(sid) }

func ingestKeyIP(remoteIP string) string {
	host, _, err := net.SplitHostPort(remoteIP)
	if err != nil {
		host = remoteIP
	}
	return "ip:" + host
}
