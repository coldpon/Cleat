// TcpWireCodec.cpp
#include "TcpWireCodec.hpp"

#include "Core/Comms/Transports/Tcp/Internal/TrafficShaper.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <cstdint>
#include <utility>

namespace cleat {

	bool TcpWireCodec::EncodeWire(const std::vector<uint8_t>& frame,
		std::vector<uint8_t>& out, const std::string& ua) {
		// 传输加密: 对载荷整体加密, 帧结构保持 [4B 长度][nonce||密文]
		std::vector<uint8_t> wire = frame;
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> enc;
			if (!CryptoManager::Instance().Encrypt(frame, enc)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "TcpWireCodec: EncodeWire 加密失败 (len=" << frame.size() << ")");
				return false;
			}
			wire = std::move(enc);
		}

		// [4B 大端长度][载荷]
		const uint32_t len = static_cast<uint32_t>(wire.size());
		uint8_t len_buf[4]{};
		len_buf[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
		len_buf[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
		len_buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
		len_buf[3] = static_cast<uint8_t>(len & 0xFF);

		std::vector<uint8_t> framed;
		framed.reserve(4 + wire.size());
		framed.insert(framed.end(), len_buf, len_buf + 4);
		framed.insert(framed.end(), wire.begin(), wire.end());

		// 流量整形(运行时开关, 原 CLEAT_HTTP_MIMIC 宏  运行时化):
		// 帧外包 HTTP 伪装头([HTTP头]\r\n\r\n + [4B长度][载荷]); ua = 连接级粘性
		// UA(同连接每帧一致, 见 TcpTransport/TrafficShaper 注)
		if (AgentConfig::Instance().GetTcpMimic()) {
			out = TrafficShaper::Wrap(MimicDirection::Request, framed, ua);
		}
		else {
			out = std::move(framed);
		}
		return true;
	}

	bool TcpWireCodec::DecodeWire(std::vector<uint8_t>& frame_body) {
	// 帧填充: 剥离帧尾随机填充(密文层), 还原真实载荷后再解密。
	// 线上帧长 = 真实载荷 + 填充 + 2B 长度标记, 两端格式见 TrafficShaper。
	// (开关与发送侧同源: GetTcpMimic —— 对端 listener 的伪装配置需一致)
	if (AgentConfig::Instance().GetTcpMimic()) {
		if (!TrafficShaper::StripPadding(frame_body)) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch, "TcpWireCodec: 帧填充剥离失败 (len=" << frame_body.size() << ")");
			return false;
		}
	}
		// 传输加密: 帧数据为 nonce||密文, 解密后才是原始载荷(JSON/混合帧)
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> dec;
			if (!CryptoManager::Instance().Decrypt(frame_body, dec)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "TcpWireCodec: 收到无法解密的帧 (len=" << frame_body.size()
					<< "), 可能 PSK 不匹配或数据被篡改, 丢弃");
				return false;
			}
			frame_body = std::move(dec);
		}
		return true;
	}

} // namespace cleat
