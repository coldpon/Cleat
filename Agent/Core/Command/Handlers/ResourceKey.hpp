/**
 * @file ResourceKey.hpp
 * @brief 跨命令互斥的资源身份
 * @details
 * **存在理由**: 超时语义 (b) = 遗弃执行线程 + `epoch` 隔离可见产物 + `ResourceKey` 互斥副作用。
 * 其中 `epoch` 只管"迟到响应该不该发", **不管副作用** —— 被遗弃的线程仍在改共享资源。故必须有
 *
 * **粒度原则(用户  裁定, 清单/2.3b/2.3c)**:
 *   * **Service 类一律全局单例**(键盘钩子/剪贴板监听/采集/文件监控): 它们抢的是独占资源,
 *     第二份不是浪费而是冲突 → `id` 为空(全局单例), 二次开启由上层**拒绝**而非排队;
 *   * **不凭空细分设备维度**: 今天一个 Service 实例只对一台设备, 造 `camera:0` 这样的 id 不表达
 *     任何真实约束; 将来真支持"一服务多设备"时再加 id(纯扩展, 不改互斥语义);
 *   * **只读/无副作用命令不参与互斥**(默认空 key): `GetSystemInfo`/`EnumProcess`/`EdrRecon::Scan`
 *     等并发是安全的, 强行互斥只会平白降低并发度;
 *   * **Terminal 单开**: 协议里没有终端身份(`Terminal::Input/Output` 只带命令名与 Agent 会话 id),
 *     多开在协议上**不可寻址**; 支持它需先给协议加终端 id(触碰"线格式不动"的约束), 故暂缓。
 *
 * **归一化是正确性的前提**: 不归一化时 `a.txt` 与 `A.TXT` 会被当成两个 key, 互斥形同虚设。
 * 归一化由 `MakeFileKey`/`MakeRegistryKey` 提供, **不要**在各 handler 里手写。
 */
#pragma once

#include <cctype>
#include <string>

namespace cleat {

    /// 需要互斥的资源类别(未列出的资源 = 不参与互斥)
    enum class ResourceKind {
        None = 0,     ///< 不参与互斥(默认; 只读/无副作用命令)
        File,         ///< 同一路径的读/写/删/改名
        Registry,     ///< 同一注册表键
        Clipboard,    ///< 剪贴板(全局单例)
        InputHook,    ///< 键盘钩子(全进程一份)
        Capture,      ///< 屏幕/摄像头/音频采集(按服务, 全局单例)
        FileMonitor,  ///< 文件变更监控(全局单例)
        Pivot,        ///< 一个 pivot/rportfwd 实例(按端口/隧道)
        Process,      ///< 目标 PID(注入/终止等破坏性操作)
        Terminal,     ///< 终端会话
        Module,       ///< 一个模块的加载/卸载生命周期
    };

    inline const char* ToString(ResourceKind k) {
        switch (k) {
        case ResourceKind::None: return "None";
        case ResourceKind::File: return "File";
        case ResourceKind::Registry: return "Registry";
        case ResourceKind::Clipboard: return "Clipboard";
        case ResourceKind::InputHook: return "InputHook";
        case ResourceKind::Capture: return "Capture";
        case ResourceKind::FileMonitor: return "FileMonitor";
        case ResourceKind::Pivot: return "Pivot";
        case ResourceKind::Process: return "Process";
        case ResourceKind::Terminal: return "Terminal";
        case ResourceKind::Module: return "Module";
        }
        return "?";
    }

    /**
     * @brief 资源身份(key 相等 = 互斥的两条命令)
     * @details `kind == None` 或 `id` 为空且 kind 非全局单例类时, 视为**不参与互斥**。
     *          全局单例类(Clipboard/InputHook/Capture/FileMonitor/Terminal)以**空 id** 表达
     *          "该 kind 只有一个实例" —— 这是有意义的空, 不是"未声明"。
     */
    struct ResourceKey {
        ResourceKind kind = ResourceKind::None;
        std::string id;

        /// 是否参与互斥(默认构造 → 不参与, 与今天无互斥的行为一致)
        bool Participates() const {
            if (kind == ResourceKind::None) return false;
            switch (kind) {
            case ResourceKind::Clipboard:
            case ResourceKind::InputHook:
            case ResourceKind::Capture:
            case ResourceKind::FileMonitor:
            case ResourceKind::Terminal:
                return true;             // 全局单例: 空 id 即 "唯一实例"
            default:
                return !id.empty();      // 需要具体身份的类: 空 id = 未声明 → 不互斥
            }
        }

        bool operator==(const ResourceKey& o) const {
            return kind == o.kind && id == o.id;
        }
        bool operator!=(const ResourceKey& o) const { return !(*this == o); }

        /// 供日志/排障: "File:C:\a.txt" / "Clipboard:*"
        std::string Describe() const {
            return std::string(ToString(kind)) + ":" + (id.empty() ? "*" : id);
        }
    };

    /**
     * @brief 路径键归一化(Windows 语义)
     * @details 小写 + 反斜杠转正斜杠 + 去尾分隔符 + 折叠重复分隔符。
     *          **不做** `..` 解析 —— 那需要卷/当前目录上下文, 且 handler 拿到的多为绝对路径;
     *          若将来要支持相对路径, 应在调用方先 `绝对化` 再进本函数。
     */
    inline ResourceKey MakeFileKey(const std::string& path) {
        std::string s;
        s.reserve(path.size());
        bool last_sep = false;
        for (char ch : path) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const char n = (c == '\\') ? '/' : c;
            if (n == '/') {
                if (last_sep) continue;      // 折叠重复分隔符
                last_sep = true;
            }
            else {
                last_sep = false;
            }
            s.push_back(n);
        }
        while (!s.empty() && s.back() == '/' && s.size() > 1) s.pop_back();   // 保留根 "/"
        return ResourceKey{ ResourceKind::File, std::move(s) };
    }

    /// 注册表键归一化: 大写 + 反斜杠转正斜杠(键名大小写不敏感, 与 Win32 语义一致)
    inline ResourceKey MakeRegistryKey(const std::string& hive_path) {
        std::string s;
        s.reserve(hive_path.size());
        for (char ch : hive_path) {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            s.push_back((c == '/') ? '\\' : c);
        }
        while (!s.empty() && s.back() == '\\') s.pop_back();
        return ResourceKey{ ResourceKind::Registry, std::move(s) };
    }

    /// 全局单例类资源的键(空 id 是"唯一实例"的有意义表达)
    inline ResourceKey GlobalKey(ResourceKind kind) {
        return ResourceKey{ kind, std::string() };
    }

    /// PID 键(进程类: 不同 PID 可并发, 同 PID 互斥)
    inline ResourceKey MakeProcessKey(unsigned long pid) {
        return ResourceKey{ ResourceKind::Process, std::to_string(pid) };
    }

    /// Pivot 实例键(每端口/隧道一个实例; 端口冲突是真实风险, 故 fail-closed)
    inline ResourceKey MakePivotKey(unsigned long session_id, unsigned int port) {
        return ResourceKey{ ResourceKind::Pivot,
            std::to_string(session_id) + ":" + std::to_string(port) };
    }

} // namespace cleat
