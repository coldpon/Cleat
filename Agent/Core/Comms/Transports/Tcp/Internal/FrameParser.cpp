#include "FrameParser.hpp"
#include "Core/Comms/Transports/Tcp/Internal/RingBuffer.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace cleat {

	FrameParser::FrameParser(RingBuffer& ring) : ring_(ring) {}

	bool FrameParser::TryParse(std::vector<uint8_t>& out_payload) {
		// 1. 至少 4 字节长度头
		if (ring_.ReadableSize() < 4) {
			return false;
		}

		// 2. 偷看长度头(不消费); peek_length 已做网络序→主机序转换
		uint32_t len = 0;
		if (!peek_length(len)) {
			return false;
		}

		// 3. 长度合法性(0 或超上限 = 非法帧)
		if (len == 0 || len > MAX_PACKET_SIZE) {
			// 丢弃长度头 + 已到达的包体(可能只丢部分: 长度字段损坏时无法确定帧边界,
			// 上层连续解析失败会熔断重连, 见 TcpTransport::kDecodeFailThreshold)
			uint8_t dummy[4];
			ring_.Read(dummy, 4);
			const size_t to_drop = std::min(static_cast<size_t>(len),
				ring_.ReadableSize());
			std::array<uint8_t, CHUNK_SIZE> chunk{};
			size_t remaining = to_drop;
			while (remaining > 0) {
				const size_t read_now = std::min(remaining, CHUNK_SIZE);
				const size_t actual = ring_.Read(chunk.data(), read_now);
				if (actual == 0) break;
				remaining -= actual;
			}
			return false;
		}

		// 4. 完整帧是否已到达
		if (ring_.ReadableSize() < 4 + static_cast<unsigned long long>(len)) {
			return false;   // 数据不足, 等待更多
		}

		// 5. 消耗长度头, 读取包体(可读性已确认, 读取必然完整)
		uint8_t header[4];
		ring_.Read(header, 4);
		out_payload.resize(len);
		if (len > 0) {
			ring_.Read(out_payload.data(), len);
		}
		return true;
	}

	// ---------- 私有辅助 ----------

	bool FrameParser::peek_length(uint32_t& out_len) const {
		if (ring_.ReadableSize() < 4) {
			return false;
		}
		uint8_t buf[4];
		if (ring_.Peek(buf, 4) != 4) {
			return false;
		}
		// 网络序(大端) → 主机序, 与 TcpWireCodec::EncodeWire 的写端一致
		out_len = (static_cast<uint32_t>(buf[0]) << 24) |
			(static_cast<uint32_t>(buf[1]) << 16) |
			(static_cast<uint32_t>(buf[2]) << 8) |
			(static_cast<uint32_t>(buf[3]));
		return true;
	}

} // namespace cleat
