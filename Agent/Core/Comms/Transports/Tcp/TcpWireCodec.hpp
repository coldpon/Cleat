// Core/Comms/Transports/Tcp/TcpWireCodec.hpp
// TCP 传输的线上帧编解码(Phase 0 S2):
// 职责: "明文帧 <-> 线上字节" 的纯变换, 不持有 socket/线程/流状态。
//   EncodeWire: 明文帧 -> [4B 大端长度][加密载荷](可选 HTTP 伪装壳 + 帧填充)
//   DecodeWire: 帧体(已剥 4B 长度头) -> 明文帧(剥帧填充 + 解密)
// 流状态(伪装头剥离/帧切分)由调用方持有(TcpTransport/CommsManager), 本类无状态。
// 加密沿用 CryptoManager(XChaCha20-Poly1305, 与传输解耦); 伪装壳沿用 TrafficShaper。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cleat {

	class TcpWireCodec {
	public:
		// 明文帧 -> 可直接写入 socket 的线上字节
		// (内部: 可选加密 -> 4B 大端长度头 -> 可选伪装壳 Wrap)
		// ua: 伪装头 User-Agent(连接级粘性, TcpTransport 连接时选定; 空 = 池随机)。
		// 失败(加密失败等)返回 false, out 内容无效
		static bool EncodeWire(const std::vector<uint8_t>& frame,
			std::vector<uint8_t>& out, const std::string& ua = "");

		// 帧体(不含 4B 长度头, 可能含帧尾填充) -> 明文帧(剥填充 + 解密, 原地)
		// 失败返回 false(填充非法/解密失败/PSK 不匹配); 加密未启用时原样通过
		static bool DecodeWire(std::vector<uint8_t>& frame_body);
	};

} // namespace cleat
