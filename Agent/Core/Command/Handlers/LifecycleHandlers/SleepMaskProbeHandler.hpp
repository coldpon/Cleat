#pragma once
/**
 * @file SleepMaskProbeHandler.hpp
 * @brief 睡眠掩码自检命令
 * @details
 * 参数: `{ "execute": bool(默认 false), "window_ms": uint(默认 3000) }`
 *   - execute=false  → 干跑: 镜像枚举 + 线程普查快照 + 原语预检 + RC4 往返(任何时刻可跑);
 *   - execute=true   → 执行一次真实掩码窗口(停靠 → 整镜像 RC4 加密 → 过窗 → 解密还原)。
 *
 * 命名注: 原名 SleepMaskTestHandler —— 被误读为"忘删的测试程序"的风险
 * 高于其字面语义(实际是随产物交付、供操作员诊断掩码前提的正式命令), 故更名 Probe。
 * 对应命令字面量 SleepMaskProbe(CmdDef::Runtime)。
 *
 * 安全不变量: execute 路径受双重闸门 —— MaskGate::TestPrecheck(静默谓词, 自身豁免)
 * 与普查(名单外镜像内线程 → 拒绝)。注意本命令运行在 JobRuntime 线程(kind=Command),
 * 发起者即自身; session-main/cmd-worker/cmd-reaper 经 MaskPark 停靠, 其余线程一律
 * 被普查拒绝 —— 这正是该命令作为"隔离测试床"的验收语义( P1: 输出普查快照/
 * 哈希前后/窗口耗时/降级原因)。
 *
 * 构建门控: 未定义 CLEAT_WITH_MASK 时 Execute 返回明确错误(命令注册同样裁剪)。
 */
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class SleepMaskProbeHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
