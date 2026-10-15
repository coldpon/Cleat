/**
 * @file ModuleManager.cpp
 * @brief 远程模块运行时管理实现
 * 
 * 实现模块的动态加载、卸载、状态管理和命令分发功能。
 */

#include "ModuleManager.hpp"
#include <cctype>
#include <cstring>
#include <sodium.h>
#include "Common/Protocol/CmdDef.hpp"       // Priority 档位枚举(动态注册 module_id 命令用)
#include "Core/Command/Dispatch/CommandDispatcher.hpp"   // P2.6: 头文件改前向声明, 完整定义在此取
#include "Common/Platform/SecureWipe.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include"Common/Protocol/ModuleApi.hpp"
namespace cleat {

    namespace {

        /**
         * @brief 模块统一入口函数类型
         * 
         * C 风格函数，跨 CRT 安全。
         */
        using ModExecFn = int(CLEAT_MOD_CALL*)(const char* params_json, char* out, int out_size);

        /**
         * @brief 计算模块字节的 SHA-256 哈希值（小写 hex）
         * 
         * 用于 ModuleLoad 的完整性校验。（补充：因为加密算法的原因，实际上可以不进行校验，（XChaCha20-Poly1305 + X25519 ECDH）但暂时保留，无额外影响）
         * sodium_init 幂等线程安全（CryptoManager 已初始化过则立即返回）。
         * @param data 要计算哈希的数据
         * @return SHA-256 哈希值的十六进制字符串
         */
        std::string Sha256Hex(const std::vector<uint8_t>& data) {
            if (sodium_init() < 0) return {};
            unsigned char h[crypto_hash_sha256_BYTES];
            crypto_hash_sha256(h, data.data(), data.size());
            static const char* kHex = "0123456789abcdef";
            std::string out(crypto_hash_sha256_BYTES * 2, '0');
            for (size_t i = 0; i < crypto_hash_sha256_BYTES; ++i) {
                out[i * 2] = kHex[h[i] >> 4];
                out[i * 2 + 1] = kHex[h[i] & 0xF];
            }
            return out;
        }

        /**
         * @brief 模块命令执行处理器
         * 
         * 把 JSON 参数传给模块 ModExec，返回其 JSON 响应。
         */
        class ModuleExecHandler : public IHandler {
        public:
            /**
             * @brief 构造函数
             * @param mod 模块对象
             * @param module_id 模块ID
             */
            ModuleExecHandler(std::shared_ptr<LoadedModule> mod, std::string module_id)
                : mod_(std::move(mod)), module_id_(std::move(module_id)) {}

            /**
             * @brief 执行模块命令
             * @param params JSON 参数
             * @param binary_data 二进制数据（当前未使用）
             * @return 模块执行结果
             */
            nlohmann::json Execute(const CommandRequest& req) override {
                uintptr_t execRva = 0;
                for (const auto& [name, rva] : mod_->export_rvas) {
                    if (name == "ModExec") { execRva = rva; break; }
                }
                if (!execRva) {
                    return Packet::Error("ModExec entry point missing in module: " + module_id_);
                }
                auto modExec = reinterpret_cast<ModExecFn>(
                    static_cast<uint8_t*>(mod_->base) + execRva);

                const std::string paramStr = req.params.dump();
                std::vector<char> out(8 * 1024 * 1024);  // 模块输出上限 8MB(截图 base64 可达 MB 级; 越界由 out.back() 兜底)
                const int rc = modExec(paramStr.c_str(), out.data(),
                    static_cast<int>(out.size()));
                if (rc != 0) {
					return Packet::Error("Module execution error, code=" + std::to_string(rc),
						module_id_);
                }
                out.back() = '\0';   // 防模块越界写(缓冲已全 0 初始化)
                try {
                    return nlohmann::json::parse(out.data());
                }
                catch (...) {
                    // 非 JSON 输出按文本返回
                    return Packet::Ok({ {"result", std::string(out.data())} });
                }
            }

        private:
            std::shared_ptr<LoadedModule> mod_;    ///< 模块对象
            std::string module_id_;                ///< 模块ID
        };

        /**
         * @brief 系统命令处理器：ModuleLoad / ModuleUnload / ModuleStatus
         *
         * 三个静态 thunk 已迁至 Core/Command/Handlers/
         * Module{Load,Unload,Status}Handler.{hpp,cpp}, 注册点在 RegisterCore.hpp。
         * 本文件只保留 ModuleExecHandler —— 它随每次 Load() 动态创建、随
         * Unload() 注销, 生命周期与模块实例绑定, 属运行时产物而非静态命令面。
         */

    } // namespace

    // =========================================================================
    // ModuleManager 实现
    // =========================================================================

    ModuleManager& ModuleManager::Instance() {
        static ModuleManager instance;
        return instance;
    }

    void ModuleManager::AttachDispatcher(CommandDispatcher& dispatcher) {
        // 原 RegisterModuleCommands 的三条静态注册已迁至
        // RegisterCore.hpp(注册点唯一原则); 此处只注入 dispatcher 供动态
        // 注册(module_id 命令)使用。
        dispatcher_ = &dispatcher;
        LOG_INFO("ModuleManager: dispatcher 已注入(静态命令注册在 RegisterCore)");
    }

    void ModuleManager::SetDataSender(std::function<bool(const std::vector<uint8_t>&)> sender) {
        sender_ = std::move(sender);
    }

    bool ModuleManager::SendFrame(std::vector<uint8_t> frame) {
        if (!sender_) {
            LOG_WARN("ModuleManager: 模块上报被丢弃(sender_ 未注入)");
            return false;
        }
        if (!sender_(std::move(frame))) {
            LOG_WARN("ModuleManager: 模块上报发送失败(帧已保留待重发)");
            return false;
        }
        return true;
    }

    // =========================================================================
    // 模块协议 v2: CoreApi 回调表落地（纯 C 函数，模块线程可调）
    // =========================================================================

    namespace {

        /**
         * @brief API 日志函数实现
         * @param level 日志级别
         * @param msg 日志消息
         */
        void ApiLog(int level, const char* msg) {
            if (!msg) return;
            const std::string m(msg);
            switch (level) {
            case 0: LOG_DEBUG("[mod] " << m); break;
            case 1: LOG_INFO("[mod] " << m); break;
            case 2: LOG_WARN("[mod] " << m); break;
            default: CLEAT_LOGE(ErrDom::Mod, ErrCode::Unknown, "[mod] " << m); break;
            }
        }

        /**
         * @brief v3: 上报 JSON 帧(**携带长度**, 帧内允许出现任何字节含 NUL)
         * @details  的根因是"上报路径按 strlen 取长" —— 混合帧体一旦含 NUL 就被截断。
         *          v3 由调用方给出长度, 与帧内容无关。
         */
        int ApiSendV3(const uint8_t* data, uint32_t len) {
            if (!data || len == 0) return -1;
            std::vector<uint8_t> out(data, data + len);
            return ModuleManager::Instance().SendFrame(std::move(out)) ? 0 : -1;
        }

        /// v3: 上报混合帧(JSON 头 + 二进制体, 两段各自带长)
        int ApiSendBinaryV3(const uint8_t* head, uint32_t head_len,
            const uint8_t* body, uint32_t body_len) {
            if (!head || head_len == 0) return -1;
            std::vector<uint8_t> out(head, head + head_len);
            out.push_back(0);   // 混合帧分隔符: JSON 头 + '\0' + 二进制体
            if (body && body_len > 0) {
                out.insert(out.end(), body, body + body_len);
            }
            return ModuleManager::Instance().SendFrame(std::move(out)) ? 0 : -1;
        }

        /// v3: 日志(msg 不必 NUL 结尾)
        void ApiLogV3(int level, const uint8_t* msg, uint32_t len) {
            if (!msg || len == 0) return;
            const std::string m(reinterpret_cast<const char*>(msg), len);
            switch (level) {
            case 0: LOG_DEBUG("[mod] " << m); break;
            case 1: LOG_INFO("[mod] " << m); break;
            case 2: LOG_WARN("[mod] " << m); break;
            default: CLEAT_LOGE(ErrDom::Mod, ErrCode::Unknown, "[mod] " << m); break;
            }
        }

        /**
         * @brief v3 只读查询: 由装配方注入
         * @details 三个量分别归 `ClientContext`(会话 id)、`SessionRunner`(代际, )、
         *          `CommsManager`(链路状态) —— 都在装配根可达, 故不在此处硬连依赖,
         *          而是注入函数指针(模块侧因此不必再持有自己的 `ClientContext` 副本)。
         */
        ModuleManager::RuntimeQueries g_runtime;

        uint64_t ApiGetSessionId() { return g_runtime.session_id ? g_runtime.session_id() : 0; }
        uint64_t ApiGetEpoch() { return g_runtime.epoch ? g_runtime.epoch() : 0; }
        int ApiIsNetworkConnected() {
            return (g_runtime.connected && g_runtime.connected()) ? 1 : 0;
        }

        /**
         * @brief 模块 API 表（必须静态存储）
         * 
         * @note Load 的栈局部传给 ModInit 后随函数返回失效，
         * 模块保存的 g_api 会指向被覆盖的栈 → 调用 send 跳到垃圾地址(实测 rip=0xF 崩溃)
         */
        /// 把 ModInit 返回码翻译成一句人话(F15: 让 loader 的报错能定位到 ABI 不匹配)
        std::string AbiHint(int rc) {
            switch (rc) {
            case CLEAT_MOD_ERR_API_NULL: return " (api 指针为空)";
            case CLEAT_MOD_ERR_ABI_TOO_OLD: return " (核心 API 表过小: 模块所需字段不存在)";
            case CLEAT_MOD_ERR_ABI_VERSION: return " (ABI 版本不在模块支持区间)";
            default: return "";
            }
        }

        const CleatCoreApi kCoreApi = {
            /* 表头 */          sizeof(CleatCoreApi), CLEAT_ABI_VERSION, 0,
            /* 上报(带长度) */    ApiSendV3, ApiSendBinaryV3, ApiLogV3,
            /* v3 查询 */       ApiGetSessionId, ApiGetEpoch, ApiIsNetworkConnected,
        };

    } // namespace

    // =========================================================================
    // ModuleManager 核心方法实现
    // =========================================================================

    const CleatCoreApi* ModuleManager::CoreApi() const {
        return &kCoreApi;
    }

    void ModuleManager::SetRuntimeQueries(RuntimeQueries q) {
        g_runtime = std::move(q);
    }

    nlohmann::json ModuleManager::Load(const nlohmann::json& params,
        const std::vector<uint8_t>& blob) {
        const std::string module_id = params.value("module_id", "");
        if (module_id.empty() || blob.empty()) {
            return Packet::Error("Module ID or module bytes is empty");
        }
        if (blob.size() > static_cast<unsigned long long>(32 * 1024) * 1024) {
            return Packet::Error("Module too large (>32MiB)");
        }

        // 完整性强校验: params.sha256(64 hex) 存在即必须匹配 —— 防截断/丢帧产物
        // 进入反射加载器(AGENT_AUDIT B2-2/B2-6 出站丢帧的现实风险);
        // 缺省放行并告警, 兼容旧下发端(新 TeamServer 三条路径均自动携带)
        {
            auto it = params.find("sha256");
            if (it != params.end() && it->is_string()) {
                std::string expect = it->get<std::string>();
                for (auto& c : expect) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                const std::string actual = Sha256Hex(blob);
                if (expect.size() != crypto_hash_sha256_BYTES * 2 || expect != actual) {
                CLEAT_LOGE(ErrDom::Mod, ErrCode::IntegrityMismatch, "ModuleManager: 模块 SHA-256 校验失败 " << module_id
                    << " expect=" << expect << " actual=" << actual);
                SecureWipe(blob);
                return Packet::Error("Module SHA-256 verification failed (corrupted or tampered during transmission)", module_id);
                }
                LOG_INFO("ModuleManager: 模块 SHA-256 校验通过 " << module_id);
            }
            else {
                LOG_WARN("ModuleManager: ModuleLoad 未携带 sha256, 跳过完整性校验 " << module_id);
            }
        }

        ModuleInfo info;
        info.type = params.value("type", 1u);         // 1 = PE/DLL(反射加载)
        info.platform = params.value("platform", 1u); // 1 = x64 Windows
        info.module_id = module_id;

        // loader_ 可为空: CreateModuleLoader() 在当前平台无反射加载器时返回 nullptr
        // (Linux: 阶段 A 仅 Windows, 见 ModuleLoaderFactory.cpp)。不判空则空指针解引用崩溃。
        if (!loader_) {
            SecureWipe(blob);
            CLEAT_LOGE(ErrDom::Mod, ErrCode::NotSupported, "ModuleManager: 当前平台不支持模块加载 " << module_id);
            return Packet::Error("Module loading not supported on this platform", module_id);
        }

        std::string err;
        std::lock_guard<std::mutex> lock(mutex_);
        if (modules_.find(module_id) != modules_.end()) {
            SecureWipe(blob);
            return Packet::Error("Module already exists: " + module_id);
        }
        auto mod = loader_->Load(blob, info, err);
        // Phase D: blob 已复制进映像, 明文 DLL 本体不留驻留(freed heap 仍可被转储还原)
        SecureWipe(blob);
        if (!mod) {
            return Packet::Error("Module loading failed: " + err, module_id);
        }
        // 以 module_id 注册命令; factory 捕获 shared_ptr → 卸载与执行中命令并发安全
        std::shared_ptr<LoadedModule> sharedMod(
            mod.release(),
            // AD10(修复 ): 捕获 shared_ptr 而非裸指针 —— 删除器可能在
            // ModuleManager 之后才执行(最后一个在途 handler 释放时)。
            [loader = loader_](LoadedModule* m) {
                loader->Unload(m);   // DETACH + VirtualFree(B1-1: 引用计数归零才释放)
                delete m;
            });
        // 模块协议 v2: 可选导出 ModInit(CoreApi) —— 注入回调表(异步上报能力)
        for (const auto& [name, rva] : sharedMod->export_rvas) {
            if (name == "ModInit") {
                auto modInit = reinterpret_cast<CleatModInitFn>(
                    static_cast<uint8_t*>(sharedMod->base) + rva);
                const int rc = modInit(&kCoreApi);
                if (rc != 0) {
                    // F15: 失败**必须可区分** —— 带上模块返回码与 ABI 诊断, 而不是一句无码 message
                    CLEAT_LOGE(ErrDom::Mod, ErrCode::InternalFailed, "ModuleManager: 模块 ModInit 失败, 卸载: " << module_id
                        << " rc=" << rc << AbiHint(rc));
                    return Packet::Error(std::string("Module ModInit failed rc=")
                        + std::to_string(rc) + AbiHint(rc), module_id);
                }
                LOG_INFO("ModuleManager: 模块协议 v2 启用(ModInit) " << module_id);
                break;
            }
        }
        if (dispatcher_) {
            // 定档(用户裁定): 动态模块命令统一 Preparation(1);
            // "由模块自报档位"需给 ABI 再加字段, 归 (已无限期后推)。
            dispatcher_->RegisterHandler(module_id,
                [sharedMod, module_id]() {
                    return std::make_unique<ModuleExecHandler>(sharedMod, module_id);
                }, Priority::Preparation);
        }
        modules_.emplace(module_id,
            LoadedEntry{ sharedMod, module_id });
        LOG_INFO("ModuleManager: 模块已加载并注册命令 " << module_id);
        return Packet::Ok({ {"module_id", module_id},
                            {"command", module_id},
                            {"exports", sharedMod->exports.size()} });
    }

    nlohmann::json ModuleManager::Unload(const nlohmann::json& params) {
        const std::string module_id = params.value("module_id", "");
        std::shared_ptr<LoadedModule> mod;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = modules_.find(module_id);
            if (it == modules_.end()) {
                return Packet::Error("Module not found: " + module_id);
            }
            mod = it->second.mod;
            modules_.erase(it);
            // 先注销命令(阻止新命令进入), 再在锁外丢引用
            if (dispatcher_) {
                dispatcher_->UnregisterHandler(module_id);
            }
        }
        // B1-1 补完: 不直接 loader_->Unload —— 那会绕过 Load 里 shared_ptr 删除器的
        // 延迟卸载语义, 在途 handler 可能仍在执行模块代码。这里只丢引用:
        // 无在途命令时 mod 析构立即 DETACH+VirtualFree, 有则延迟到最后一个引用释放。
        LOG_INFO("ModuleManager: 模块已卸载 " << module_id);
        return Packet::Ok({ {"module_id", module_id}, {"unloaded", true} });
    }

    nlohmann::json ModuleManager::Status(const nlohmann::json&) const {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [id, entry] : modules_) {
            list.push_back({ {"module_id", id},
                             {"command", entry.command},
                             {"exports", entry.mod->exports.size()} });
        }
        return Packet::Ok({ {"modules", list} });
    }

    void ModuleManager::UnloadAll() {
        std::unordered_map<std::string, LoadedEntry> drained;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, entry] : modules_) {
                if (dispatcher_) dispatcher_->UnregisterHandler(id);
            }
            // 同 Unload: 只清引用, DETACH+VirtualFree 由 shared_ptr 删除器在
            // 最后一个在途 handler 释放后执行(B1-1 延迟卸载语义)
            drained.swap(modules_);
        }
        // 模块在其中 join 自有线程, 而该线程可能回调核心(查询/注销)再取 mutex_。
        // 持锁执行删除器 = 与模块线程互等死锁; 故换出容器后在锁外释放引用。
        drained.clear();
        LOG_INFO("ModuleManager: 全部模块已卸载");
    }

} // namespace cleat
