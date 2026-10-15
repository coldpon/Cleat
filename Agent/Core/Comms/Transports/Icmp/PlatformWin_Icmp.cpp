// Core/Comms/Transports/Icmp/PlatformWin_Icmp.cpp
// ICMP 回声实现(Windows): IcmpSendEcho(iphlpapi, 免管理员, 无需 raw socket)。
#include "Core/Comms/Transports/Icmp/PlatformWin_Icmp.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <iphlpapi.h>     // 先引入(定义 IPAddr 等基础类型)

#include <vector>

namespace cleat {

	namespace {

		// 应答缓冲 = ICMP_ECHO_REPLY + 8(保证 Data 对齐) + payload
		size_t ReplyBufferSize(size_t payload_len) {
			return sizeof(ICMP_ECHO_REPLY) + 8 + payload_len;
		}

	} // namespace

	PlatformWin_Icmp::PlatformWin_Icmp() {
		handle_ = IcmpCreateFile();
		if (handle_ == INVALID_HANDLE_VALUE) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PlatformWin_Icmp: IcmpCreateFile 失败 err=" << GetLastError());
		}
	}

	PlatformWin_Icmp::~PlatformWin_Icmp() {
		Reset();
	}

	bool PlatformWin_Icmp::Echo(const std::string& target, const std::vector<uint8_t>& payload,
		std::vector<uint8_t>& reply_payload, int timeout_ms) {

			if (handle_ == INVALID_HANDLE_VALUE) {
				return false;
			}
			const unsigned long addr = ::inet_addr(target.c_str());
			if (addr == INADDR_NONE) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "PlatformWin_Icmp: 目标非 IPv4: " << target);
				return false;
			}

			std::vector<uint8_t> buf(ReplyBufferSize(payload.size()));
			auto* reply = reinterpret_cast<ICMP_ECHO_REPLY*>(buf.data());
			const DWORD count = IcmpSendEcho(handle_, addr,
				payload.empty() ? nullptr : const_cast<uint8_t*>(payload.data()),
				static_cast<WORD>(payload.size()), nullptr, buf.data(),
				static_cast<DWORD>(buf.size()), static_cast<DWORD>(timeout_ms));
			if (count == 0) {
				LOG_DEBUG("PlatformWin_Icmp: IcmpSendEcho 失败 err=" << GetLastError());
				return false;
			}
			if (reply->Status != IP_SUCCESS) {
				LOG_DEBUG("PlatformWin_Icmp: 应答状态非成功 status=" << reply->Status);
				return false;
			}
			// 应答 payload(下行分片)
			reply_payload.assign(reinterpret_cast<const uint8_t*>(reply->Data),
				reinterpret_cast<const uint8_t*>(reply->Data) + reply->DataSize);
			return true;
	}

	void PlatformWin_Icmp::Reset() {

			if (handle_ != INVALID_HANDLE_VALUE) {
				IcmpCloseHandle(handle_);
				handle_ = INVALID_HANDLE_VALUE;
			}
	}

	std::unique_ptr<IIcmpOps> CreateIcmpOps() {
		return std::make_unique<PlatformWin_Icmp>();
	}

} // namespace cleat
