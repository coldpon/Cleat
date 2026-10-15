// Package crypto 传输加密器(XChaCha20-Poly1305, 与 Agent libsodium
// crypto_aead_xchacha20poly1305_ietf 互通)。
// 帧格式保持 [4B 长度][数据], 数据 = nonce(24B) || 密文(明文+16B tag)。
// 照搬自旧 Server encrypt.go(去硬编码默认密钥, 密钥由外部注入)。
package crypto

import (
	"crypto/ecdh"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"sync"

	"golang.org/x/crypto/chacha20poly1305"
)

// Box 传输加密器。
type Box struct {
	key [32]byte
}

// New 从 64 位 hex 初始化加密器。必须与 Agent 编译期 CLEAT_PSK_HEX 一致。
func New(pskHex string) (*Box, error) {
	if len(pskHex) != 64 {
		return nil, fmt.Errorf("PSK 必须是 64 位 hex, 实际 %d 字符", len(pskHex))
	}
	raw, err := hex.DecodeString(pskHex)
	if err != nil {
		return nil, fmt.Errorf("PSK hex 解码失败: %v", err)
	}
	var c Box
	copy(c.key[:], raw)
	return &c, nil
}

// Encrypt 加密载荷 -> nonce(24B) || ciphertext。
func (c *Box) Encrypt(plaintext []byte) ([]byte, error) {
	aead, err := chacha20poly1305.NewX(c.key[:])
	if err != nil {
		return nil, err
	}
	nonce := make([]byte, aead.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return nil, err
	}
	return aead.Seal(nonce, nonce, plaintext, nil), nil
}

// Decrypt 解密载荷(nonce(24B) || ciphertext) -> 原始载荷。
// 篡改/错误 key 会返回错误。
func (c *Box) Decrypt(data []byte) ([]byte, error) {
	aead, err := chacha20poly1305.NewX(c.key[:])
	if err != nil {
		return nil, err
	}
	if len(data) < aead.NonceSize()+aead.Overhead() {
		return nil, errors.New("密文太短, 无法解密")
	}
	nonce, ciphertext := data[:aead.NonceSize()], data[aead.NonceSize():]
	return aead.Open(nil, nonce, ciphertext, nil)
}

// ===== X25519 握手模式(CS 式, 与 Agent --server-pubkey 配套) =====
//
// 线上契约(与 Agent CryptoManager 锁步):
//   - 握手帧(明文): ["KE" 2B][key_id 8B 大端][eph_pub 32B] = 42B
//     Agent 每次连接/重连后先发; Server 据 eph_pub 派生会话密钥并登记 key_id。
//   - 常规帧: [key_id 8B 大端][nonce 24B][ciphertext] —— Server 按 key_id 路由会话密钥。
//   - 会话密钥 = X25519(server_priv, agent_eph_pub), 与 Agent 端派生一致。
// : key_id 1B→8B(原 1B 在 30+ 客户端时碰撞率 ~11%, 会话密钥串线; 8B 碰撞概率 2^-64 可忽略)
// 安全性: Agent 内置 Server 公钥 → 只有真 Server 能完成协商; 每进程临时密钥对
// 提供前向保密; 无静态密钥落盘(私钥由 --x25519-key 注入)。

type X25519Box struct {
	priv *ecdh.PrivateKey
	mu   sync.RWMutex
	keys map[uint64][32]byte // key_id → 会话密钥
	// FIFO 淘汰队列(A3-16, ): keys 曾只增不删(注释自称"可能被清理"
	// 却无实现), 每次重连/握手泄漏一个会话密钥。超上限时按插入序淘汰最老者。
	// 被淘汰密钥的下帧解密报"未知 key_id" → Agent 重握手自愈(一次重连的代价,
	// 换内存严格有界)。同 key_id 重复握手不重复入队。
	keyOrder []uint64
}

// maxSessionKeys 会话密钥容量上限。8B key_id 下 4096 个 = 足够容纳全部活跃
// 会话; 只有大量死亡会话的陈旧密钥会累积到触发淘汰。
const maxSessionKeys = 4096

// GenerateX25519KeyPair 生成新的 X25519 长期密钥对, 返回 (私钥hex, 公钥hex)。
// 供 artifact generate 在 x25519 模式为 TeamServer 生成长期私钥、为 Agent 注入公钥。
func GenerateX25519KeyPair() (privHex, pubHex string, err error) {
	priv, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return "", "", err
	}
	keyBytes := priv.Bytes()
	return hex.EncodeToString(keyBytes), hex.EncodeToString(priv.PublicKey().Bytes()), nil
}

// NewX25519 从 64 位 hex 私钥初始化。
func NewX25519(privHex string) (*X25519Box, error) {
	if len(privHex) != 64 {
		return nil, fmt.Errorf("X25519 私钥必须是 64 位 hex, 实际 %d 字符", len(privHex))
	}
	raw, err := hex.DecodeString(privHex)
	if err != nil {
		return nil, fmt.Errorf("X25519 私钥解码失败: %v", err)
	}
	priv, err := ecdh.X25519().NewPrivateKey(raw)
	if err != nil {
		return nil, fmt.Errorf("X25519 私钥无效: %v", err)
	}
	return &X25519Box{priv: priv, keys: make(map[uint64][32]byte)}, nil
}

// PublicKeyHex 返回本机长期公钥 hex(供 Agent --server-pubkey 注入)。
func (x *X25519Box) PublicKeyHex() string {
	return hex.EncodeToString(x.priv.PublicKey().Bytes())
}

// Handshake 处理明文握手帧("KE"+key_id(8B)+eph_pub)。
// 返回 true 表示已识别并处理(消费, 非业务帧); 无效握手帧同样消费(防解析混淆)。
func (x *X25519Box) Handshake(frame []byte) bool {
	if len(frame) < 42 || frame[0] != 'K' || frame[1] != 'E' {
		return false
	}
	keyID := binary.BigEndian.Uint64(frame[2:10])
	pub, err := ecdh.X25519().NewPublicKey(frame[10:42])
	if err != nil {
		return true // 已识别为握手帧, 但公钥无效 → 消费并忽略
	}
	shared, err := x.priv.ECDH(pub)
	if err != nil {
		return true
	}
	var k [32]byte
	copy(k[:], shared)
	x.mu.Lock()
	if _, exists := x.keys[keyID]; !exists {
		// 容量守卫: 满则淘汰最老密钥(FIFO), keys 映射内存严格有界(A3-16)
		for len(x.keys) >= maxSessionKeys {
			oldest := x.keyOrder[0]
			x.keyOrder = x.keyOrder[1:]
			delete(x.keys, oldest)
		}
		x.keyOrder = append(x.keyOrder, keyID)
	}
	x.keys[keyID] = k
	x.mu.Unlock()
	return true
}

// DecryptFrame 解密常规帧([key_id 8B 大端][nonce||ct]) -> 明文帧。
func (x *X25519Box) DecryptFrame(data []byte) ([]byte, error) {
	if len(data) < 8 {
		return nil, errors.New("空帧")
	}
	keyID := binary.BigEndian.Uint64(data[:8])
	x.mu.RLock()
	k, ok := x.keys[keyID]
	x.mu.RUnlock()
	if !ok {
		return nil, fmt.Errorf("未知 key_id %d(可能未握手或被清理)", keyID)
	}
	aead, err := chacha20poly1305.NewX(k[:])
	if err != nil {
		return nil, err
	}
	body := data[8:]
	if len(body) < aead.NonceSize()+aead.Overhead() {
		return nil, errors.New("密文太短, 无法解密")
	}
	nonce, ciphertext := body[:aead.NonceSize()], body[aead.NonceSize():]
	return aead.Open(nil, nonce, ciphertext, nil)
}

// EncryptForKeyID 用指定会话密钥加密(响应帧, 与请求同 key_id)。
func (x *X25519Box) EncryptForKeyID(keyID uint64, plaintext []byte) ([]byte, error) {
	x.mu.RLock()
	k, ok := x.keys[keyID]
	x.mu.RUnlock()
	if !ok {
		return nil, fmt.Errorf("未知 key_id %d", keyID)
	}
	aead, err := chacha20poly1305.NewX(k[:])
	if err != nil {
		return nil, err
	}
	nonce := make([]byte, aead.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return nil, err
	}
	out := aead.Seal(nonce, nonce, plaintext, nil)
	// 响应帧头 = key_id 8B 大端
	keyBuf := make([]byte, 8)
	binary.BigEndian.PutUint64(keyBuf, keyID)
	return append(keyBuf, out...), nil
}
