package session

// TS-4/G7 的 -race 用例(安全专项修复清单附录批 3 用例 A/B 设计):
// Record.Closed 曾被 Session.mu(s.mu)/infoMu/无锁 三路并发读写 —— 本文件用
// 并发循环把它钉在 race detector 上。修复 = 全部收口 Record.IsClosed/SetClosed。

import (
	"sync"
	"testing"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// 用例 A: sendFrame(读 Closed) × MarkOffline(写 Closed) —— beacon 模式
// (Conn=nil 时 sendFrame 走 outbox 分支, 不需要真实连接)。
func TestClosedRaceSendFrameVsMarkOffline(t *testing.T) {
	s := New(nil, eventbus.New())
	s.Record.ID = 1
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		for i := 0; i < 2000; i++ {
			_ = s.sendFrame([]byte("x"))
		}
	}()
	go func() {
		defer wg.Done()
		s.MarkOffline()
	}()
	wg.Wait()
}

// 用例 B(原设计)已移出: reuseSession × sendFrame 的并发同时暴露 **Session.Conn
// 的锁分叉**(s.mu 写 × writeMu 读, 登记 F7/A2-9 残留) —— 与 Closed 收口无关,
// 待 F7 修复后回归。见 TestSubmitTaskUnknownSession 所在批次的 F7 登记。

// 用例 C: IsClosed 读 × SetClosed 写 —— 收口后 API 的最小化常驻 race 回归。
func TestClosedAccessRace(t *testing.T) {
	r := &store.Record{}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		for i := 0; i < 5000; i++ {
			_ = r.IsClosed()
		}
	}()
	go func() {
		defer wg.Done()
		for i := 0; i < 5000; i++ {
			r.SetClosed(i%2 == 0)
		}
	}()
	wg.Wait()
}
