#pragma once
// BofApi.hpp — BOF 执行期上下文与 Beacon* API 面。
//
// BOF 二进制经 COFF 符号表**按名**引用 Beacon* 函数(加载器依表解析),
// 这些实现里"输出去哪"来自当前执行上下文。每个 BOF 跑在自己的临时命令
// 线程上, 上下文经 thread_local 挂接:
//   * 输出缓冲 **per-execution**(非 Adaptix 的全局单例 bofOutputPacker):
//     本仓 CommandWorker 有并发槽位, 两个 BOF 同时跑时全局缓冲会串输出
//     ;
//   * 已知边界: BOF 自己 spawn 的线程里调 Beacon* 找不到上下文 → 输出
//     丢弃(v1 同步模型; 长任务 BOF 走 TaskManager 时再收口)。
//
// 结构布局口径: 与 CS beacon.h 的 datap/formatp **逐字段一致**, CS/
// TrustedSec 模板编译出的 BOF 直接可用; 参数打包口径(服务端)同 CS。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32

#include <windows.h>

namespace cleat {
    namespace bof {

        // 单次 BOF 输出上限(与 ModuleExecHandler 的 8MB 输出口径一致)
        inline constexpr size_t kMaxOutputBytes = 8 * 1024 * 1024;

        /// 与 CS beacon.h 的 datap 一致(BOF 侧头文件按此布局声明, 勿改字段序)
        struct DataParser {
            char* original;
            char* buffer;
            int length;
            int size;
        };

        /// 与 CS beacon.h 的 formatp 一致
        struct Format {
            char* original;
            char* buffer;
            int length;
            int size;
        };

        /// 每次执行的上下文(命令线程 thread_local 挂接, 见文件头)
        struct ExecContext {
            std::string output;      ///< BeaconOutput/BeaconPrintf 追加
            bool truncated = false;  ///< 输出超上限被截断(响应里告知服务端)
        };

        /// 取/挂当前线程上下文(loader 以 RAII 挂接, go() 返回即摘)
        ExecContext* CurrentContext();
        void SetCurrentContext(ExecContext* ctx);

        // ---- 参数解析(BOF 的 go() 入口自己调 BeaconDataParse 起头) ----
        void BeaconDataParse(DataParser* parser, char* data, int size);
        int BeaconDataInt(DataParser* parser);
        short BeaconDataShort(DataParser* parser);
        int BeaconDataLength(DataParser* parser);
        char* BeaconDataExtract(DataParser* parser, int* size);

        // ---- 输出 ----
        void BeaconOutput(int type, const char* data, int len);
        void BeaconPrintf(int type, const char* fmt, ...);

        // ---- 格式化(CS 口径: FormatInt 按**大端**打包) ----
        void BeaconFormatAlloc(Format* f, int maxsz);
        void BeaconFormatReset(Format* f);
        void BeaconFormatFree(Format* f);
        void BeaconFormatAppend(Format* f, const char* text, int len);
        void BeaconFormatPrintf(Format* f, const char* fmt, ...);
        char* BeaconFormatToString(Format* f);
        void BeaconFormatInt(Format* f, int value);

        // ---- 内部 API ----
        void BeaconUseToken(void* token);   ///< ImpersonateLoggedOnUser(令牌归 BOF 管, 此处不 Close)
        void BeaconRevertToken();           ///< RevertToSelf
        BOOL BeaconIsAdmin();
        void* BeaconAddValue(const char* key, void* value);   ///< 跨 BOF 调用持久(CS 语义)
        void* BeaconGetValue(const char* key);
        void* BeaconRemoveValue(const char* key);

        /// loader 用的 API 表(符号名 → 函数指针; 明文比较 —— BOF 符号表本身
        /// 明文, 哈希表不带来增益, 本仓口径见 ApiResolver.hpp 文件头)
        struct ApiEntry {
            const char* name;
            void* proc;
        };
        const ApiEntry* ApiTable(size_t& count);

    } // namespace bof
} // namespace cleat

#endif // _WIN32
