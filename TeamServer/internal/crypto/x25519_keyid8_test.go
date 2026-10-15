// x25519_keyid8_test.go — 8B key_id 握手/加解密往返验证(不依赖传输通道)
package crypto

import (
	"bytes"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"testing"

	"golang.org/x/crypto/chacha20poly1305"
)

// 模拟 Agent 侧: 生成 eph 密钥对 → 构造 42B 握手帧 → 8B key_id 加密帧
// Server 侧: Handshake 登记 → DecryptFrame 解密 → EncryptForKeyID 响应 → Agent 侧校验
func TestX25519KeyID8RoundTrip(t *testing.T) {
	// server 长期私钥
	serverPrivHex, _, err := GenerateX25519KeyPair()
	if err != nil {
		t.Fatal(err)
	}
	xb, err := NewX25519(serverPrivHex)
	if err != nil {
		t.Fatal(err)
	}

	// agent 侧模拟: 进程级临时密钥对 + 派生会话密钥 + 随机 8B key_id
	agentEph, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	pubHex := xb.PublicKeyHex()
	pubRaw, _ := hex.DecodeString(pubHex)
	serverPub, err := ecdh.X25519().NewPublicKey(pubRaw)
	if err != nil {
		t.Fatal(err)
	}
	shared, err := agentEph.ECDH(serverPub)
	if err != nil {
		t.Fatal(err)
	}
	var keyID uint64 = 0x1234567890abcdef // 8B 大端, 覆盖高位

	// 1. Agent → Server 握手帧: "KE"(2) + key_id(8B 大端) + eph_pub(32B) = 42B
	hs := make([]byte, 0, 42)
	hs = append(hs, 'K', 'E')
	var kidBuf [8]byte
	binary.BigEndian.PutUint64(kidBuf[:], keyID)
	hs = append(hs, kidBuf[:]...)
	hs = append(hs, agentEph.PublicKey().Bytes()...)
	if len(hs) != 42 {
		t.Fatalf("握手帧长度 = %d, want 42", len(hs))
	}
	if !xb.Handshake(hs) {
		t.Fatal("Handshake 未识别握手帧")
	}

	// 2. Agent 加密业务帧: [key_id 8B 大端][nonce||ct]
	aeadAgent, err := chacha20poly1305.NewX(shared)
	if err != nil {
		t.Fatal(err)
	}
	nonce := make([]byte, aeadAgent.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		t.Fatal(err)
	}
	ct := aeadAgent.Seal(nonce, nonce, []byte(`{"cmd":"Ping"}`), nil)
	frame := make([]byte, 0, 8+len(ct))
	frame = append(frame, kidBuf[:]...)
	frame = append(frame, ct...)

	// 3. Server 解密(按 8B key_id 路由)
	plain, err := xb.DecryptFrame(frame)
	if err != nil {
		t.Fatalf("DecryptFrame: %v", err)
	}
	if string(plain) != `{"cmd":"Ping"}` {
		t.Fatalf("解密内容不符: %s", plain)
	}

	// 4. Server 加密响应(同 key_id) → Agent 剥 8B 校验
	resp, err := xb.EncryptForKeyID(keyID, []byte(`{"status":"success"}`))
	if err != nil {
		t.Fatal(err)
	}
	gotID := binary.BigEndian.Uint64(resp[:8])
	if gotID != keyID {
		t.Fatalf("响应 key_id = %x, want %x", gotID, keyID)
	}
	body := resp[8:]
	decNonce, decCt := body[:aeadAgent.NonceSize()], body[aeadAgent.NonceSize():]
	dec, err := aeadAgent.Open(nil, decNonce, decCt, nil)
	if err != nil {
		t.Fatalf("Agent 解密响应失败: %v", err)
	}
	if !bytes.Equal(dec, []byte(`{"status":"success"}`)) {
		t.Fatalf("响应内容不符: %s", dec)
	}

	// 5. 未知 key_id → 必须报错(路由正确性)
	if _, err := xb.DecryptFrame(append([]byte{0, 0, 0, 0, 0, 0, 0, 0x2a}, ct...)); err == nil {
		t.Fatal("未知 key_id 应解密失败")
	}
}
