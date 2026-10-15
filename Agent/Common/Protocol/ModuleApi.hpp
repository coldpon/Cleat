// ModuleApi.hpp — 模块协议 v2: CoreApi 回调表(纯 C, 跨 CRT 安全)
//
// 模块协议 v1: ModExec(params_json, out, out_size) — 一次性请求/响应,
//   适合截图/枚举类; 输出上限 8MB, 无法持续上报。
//
// 模块协议 v2: 可选导出 ModInit(const CleatCoreApi*),
//   核心在反射加载成功后调用, 模块保存 api 指针后可:
//     - api->send(json)        上报 JSON 帧(命令结果/事件, 核心加密出站)
//     - api->send_binary(...)  上报混合帧(JSON 头 + 二进制体, 文件分块/流数据)
//     - api->log(...)          本地日志(经核心 Logger)
//   适合长任务/持续上报型模块(文件传输/键盘记录/流服务)。
//
// 约定:
//   - 纯 C 结构: 无 C++ 类型跨边界(避免 CRT/ABI 不一致)
//   - 模块线程可自由调用 api(核心侧实现线程安全)
//   - ModInit 返回非 0 视为初始化失败(核心卸载模块)
//   - 旧模块不导出 ModInit → 兼容 v1, 仅用返回值
#pragma once
#include <cstdint>

// 调用约定: Windows 模块走 __stdcall; Linux .so 默认 cdecl
#ifdef _WIN32
#define CLEAT_MOD_CALL __stdcall
#else
#define CLEAT_MOD_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 协议 v3: 表头 + 携带长度的入口 + 只读查询 ----------------------------
// ABI 版本: 表在本进程内**不可变**, 故模块在 ModInit 校验一次即可(不逐调用传头) ——
// `struct_size` 是"表有多大"(ABI 兼容), 各调用的 `len` 是"这帧多长"(数据完整性),
// 两者是不同层级的量, 混在一个结构里会给维护制造歧义。
#define CLEAT_ABI_VERSION 3u

// ModInit 返回码(F15: 失败必须**可区分**, 不能再是一句无码的 message)
#define CLEAT_MOD_OK               0
#define CLEAT_MOD_ERR_API_NULL     1   /* api 指针为空 */
#define CLEAT_MOD_ERR_ABI_TOO_OLD  2   /* struct_size 小于模块所需: 尾部字段不存在 */
#define CLEAT_MOD_ERR_ABI_VERSION  3   /* 尺寸够但 abi_version 不在支持区间 */

typedef struct CleatCoreApi {
    // ---- 表头(必须最先; 模块在 ModInit 时校验一次) ----
    uint32_t struct_size;   // = sizeof(CleatCoreApi)
    uint16_t abi_version;   // = CLEAT_ABI_VERSION
    uint16_t reserved;      // 对齐 + 未来 flags

    // ---- 携带长度的上报入口 ----
    // 上报 JSON 帧; 长度由调用方给出, 帧内允许出现任何字节(含 NUL)
    int (*send)(const uint8_t* data, uint32_t len);
    // 上报混合帧: JSON 头 + 二进制体(两段各自带长)
    int (*send_binary)(const uint8_t* head, uint32_t head_len,
                          const uint8_t* body, uint32_t body_len);
    // 日志; msg 不必 NUL 结尾
    void (*log)(int level, const uint8_t* msg, uint32_t len);

    // ---- v3 新增只读查询(解决  模块侧 ClientContext 副本 /  链路状态恒 true) ----
    uint64_t(*get_session_id)(void);       // 0 = 未注册
    uint64_t(*get_epoch)(void);            //  连接代际(陈旧响应归属判据)
    int(*is_network_connected)(void);      // 1/0
} CleatCoreApi;

// 模块侧 ABI 校验(协议 v3): ModInit 开头调用一次, 失败即返回对应错误码。
// 用 inline + __cplusplus 守卫, 避免给纯 C 模块引入依赖。
#ifdef __cplusplus
inline int CleatApiCheck(const CleatCoreApi* api, uint32_t need_size,
    uint16_t min_version, uint16_t max_version) {
    if (!api) return CLEAT_MOD_ERR_API_NULL;
    if (api->struct_size < need_size) return CLEAT_MOD_ERR_ABI_TOO_OLD;
    if (api->abi_version < min_version || api->abi_version > max_version) {
        return CLEAT_MOD_ERR_ABI_VERSION;
    }
    return CLEAT_MOD_OK;
}
#endif

// 模块可选导出: 核心加载成功后调用(传 api 表); 返回 0 成功, 非 0 = 初始化失败
typedef int (CLEAT_MOD_CALL* CleatModInitFn)(const CleatCoreApi* api);

// 模块可选导出(协议 v2 扩展, ): 卸载前调用 —— 模块应停止自有的
// 工作线程/服务(Shell reader、钩子、采集线程等), 返回 0 成功。
// 核心在 DllMain(DETACH) 与释放映像之前调用; 模块必须等自有线程全部退出
// 后再返回, 否则 DETACH/memset 会命中仍在执行的模块代码(越界/崩溃)。
typedef int (CLEAT_MOD_CALL* CleatModDeinitFn)();

#ifdef __cplusplus
} // extern "C"
#endif
