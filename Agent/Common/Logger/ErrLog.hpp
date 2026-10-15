#pragma once
// 结构化错误日志(二维: 域×语义) —— 错误码机制的唯一日志入口。
// 唯一事实源: doc/errors/catalog.toml; 码表: Common/Protocol/ErrorCode.hpp(生成)。
//
// 用法:
//   CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed, "connect failed " << host);
// 输出行:
//   ... [ERROR] code=PlatformApiFailed domain=PIV | connect failed 10.0.0.1:445
//
// 设计约束(与 LOG 宏逐条对齐, 不得弱化):
//   1. 编译期摘除: 交付构建(NDEBUG, MIN_LEVEL=2)下 ERROR 级别虽然保留,
//      但若 CLEAT_LOG_MIN_LEVEL 被覆盖为 4, 整条语句与 "code="/"domain="
//      字面量都不进产物 —— 静态特征摘除规则与 Logger.hpp:24 一致。
//   2. 语句形态: 保持 do/while(0) + if constexpr, 可用于不带大括号的
//      if/else 位置; 参数表达式仅在级别通过时求值, 禁止带副作用。
//   3. 码名/域名渲染只走 ErrCodeName/ErrDomName(生成头的 switch),
//      调用点永远不手写字符串码 —— 拼错在编译期即报错。
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/ErrorCode.hpp"

#include <sstream>
#include <string>

namespace cleat {

	// 前置结构化字段后走既有 Logger 通道(EventSink/落盘/控制台行为全部不变)。
	inline void LogErr(ErrDom dom, ErrCode code, const std::string& message) {
		std::ostringstream s;
		s << "code=" << ErrCodeName(code)
			<< " domain=" << ErrDomName(dom)
			<< " | " << message;
		Logger::Instance().Log(LogLevel::_ERROR, CLEAT_LOG_FILE, CLEAT_LOG_LINE, s.str());
	}

} // namespace cleat

#if CLEAT_LOG_MIN_LEVEL <= 3
#define CLEAT_LOGE(dom, code, msg) \
	do { \
		if constexpr (static_cast<int>(cleat::LogLevel::_ERROR) < CLEAT_LOG_MIN_LEVEL) { \
			/* 编译期摘除: 低于下限不产生任何代码 */ \
		} \
		else if (cleat::LogLevel::_ERROR >= cleat::Logger::Instance().GetLevel()) { \
			std::ostringstream _elog_stream; \
			_elog_stream << msg; \
			cleat::LogErr(dom, code, _elog_stream.str()); \
		} \
	} while (0)
#else
#define CLEAT_LOGE(dom, code, msg) ((void)0)
#endif
