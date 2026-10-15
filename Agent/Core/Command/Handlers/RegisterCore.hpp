/**
 * @file RegisterCore.hpp
 * @brief 核心层命令统一注册点(无条件注册, 不得随功能裁剪)
 *   - ExitProgram / SetConfig / GetConfig / SetWorkerTimeout: 生存命令, 优先级 0
 *   - AgentDescribe: 机器可读自述, 优先级 0
 *   - MigrateSpawn: 进程注入/迁移原语(宿主层,  迁入), 优先级 0
 *   - BofExec: BOF 加载执行原语(宿主层, ), 优先级 0
 *   - InjectExec: 注入原语(远程命令驱动的注入器, 宿主层, ), 优先级 0
 *   - Ping: 连通性探测(原 Features/Ping, 同属生存层), 优先级 1
 *   - JobList / JobStatus / JobKill: 任务系统命令面, 优先级 2 / 2 / 1
 *   - ModuleLoad / ModuleUnload / ModuleStatus: 模块命令面( 从
 *     ModuleManager::RegisterModuleCommands 归位), 优先级 1 / 1 / 2
 * 由 AgentContext::RegisterAllHandlers 在条件编译块之外无条件调用。
 * SystemInfoModule 属"身份依赖"(FirstConnect 的 machine_id 等注册帧
 * 参数来自其 Provider), 同样无条件注册, 不得随功能裁剪。
 *
 * **注册点唯一原则**: 核心层命令只允许在本文件注册 —— handler 实现住
 * Handlers/ 目录(一命令一对 hpp/cpp), 不得在子系统实现文件里私设注册
 * 函数(此前 ModuleManager 私设过, 属分层破窗, 已归位)。
 * **目录分组**: handler 按子系统分子目录 —— LifecycleHandlers/
 * (生存类: Ping/Exit/AgentDescribe/SetWorkerTimeout)、ConfigHandlers/(配置读写)、
 * JobHandlers/(作业命令面 + ServiceResponses 工具)、ModuleHandlers/(模块三命令
 * + BofExec)、MigrationHandlers/(MigrateSpawn); 基础设施(IHandler/CommandRequest/
 * ResourceKey)与本注册点留根。InjectHandlers/(InjectExec) 同此纪律。
 * 例外: 随对象生命周期动态注册/注销的 handler(如模块的 ModuleExecHandler)
 * 归运行时所有者(ModuleManager), 不进本文件、不进任何分组子目录。
 */

#pragma once

#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Core/Command/Handlers/LifecycleHandlers/AgentDescribeHandler.hpp"
#include "Core/Command/Handlers/ModuleHandlers/BofHandler.hpp"
#include "Core/Command/Handlers/LifecycleHandlers/ExitProgramHandler.hpp"
#include "Core/Command/Handlers/InjectHandlers/InjectHandler.hpp"
#include "Core/Command/Handlers/JobHandlers/JobKillHandler.hpp"
#include "Core/Command/Handlers/JobHandlers/JobListHandler.hpp"
#include "Core/Command/Handlers/JobHandlers/JobStatusHandler.hpp"
#include "Core/Command/Handlers/ModuleHandlers/ModuleLoadHandler.hpp"
#include "Core/Command/Handlers/ModuleHandlers/ModuleUnloadHandler.hpp"
#include "Core/Command/Handlers/ModuleHandlers/ModuleStatusHandler.hpp"
#include "Core/Command/Handlers/ConfigHandlers/SetConfigHandler.hpp"
#include "Core/Command/Handlers/ConfigHandlers/GetConfigHandler.hpp"
#include "Core/Command/Handlers/LifecycleHandlers/SetWorkerTimeoutHandler.hpp"
#include "Core/Command/Handlers/LifecycleHandlers/PingHandler.hpp"
#ifdef CLEAT_WITH_MASK
#include "Core/Command/Handlers/LifecycleHandlers/SleepMaskProbeHandler.hpp"
#endif
#include "Core/Command/Handlers/MigrationHandlers/MigrateSpawnHandler.hpp"
#include "Common/Protocol/CmdDef.hpp"

namespace cleat {

    inline void RegisterCore(CommandDispatcher& dispatcher) {
        // 生存命令: 0 = 最高调度优先级(与旧 getPriority 表中自毁类同级)
        dispatcher.RegisterHandler(Cmd::ExitProgram(), []() {
            return std::make_unique<ExitProgramHandler>();
            }, Priority::Survival);
        dispatcher.RegisterHandler(Cmd::Runtime::SetConfig(), []() {
            return std::make_unique<SetConfigHandler>();
            }, Priority::Survival);
        dispatcher.RegisterHandler(Cmd::Runtime::GetConfig(), []() {
            return std::make_unique<GetConfigHandler>();
            }, Priority::Survival);
        // AgentDescribe: 机器可读自述(版本/构建标识 + 活注册命令清单 + 已加载模块)。
        // 只读诊断, 与 GetConfig 同档; factory 捕获 dispatcher 引用 —— 清单在
        // Dispatch 调用现场快照, 模块后续动态注册的命令自动进清单。
        dispatcher.RegisterHandler(Cmd::Runtime::AgentDescribe(), [&dispatcher]() {
            return std::make_unique<AgentDescribeHandler>(dispatcher);
            }, Priority::Survival);
        dispatcher.RegisterHandler(Cmd::Runtime::SetWorkerTimeout(), []() {
            return std::make_unique<SetWorkerTimeoutHandler>();
            }, Priority::Survival);
        dispatcher.RegisterHandler(Cmd::Heartbeat::Ping(), []() {
            return std::make_unique<PingHandler>();
            }, Priority::Preparation);
#ifdef CLEAT_WITH_MASK
        // SleepMaskProbe: 睡眠掩码自检(P1;  MaskWindow 设计; 
        // 由 SleepMaskTest 更名——原名像遗留测试程序, 实为随产物交付的诊断命令)——
        // 干跑(普查/原语/RC4 往返)或真窗口(停靠 + 普查拒绝不变量)。随掩码功能整体
        // 裁剪, 与 CmdDef 的 SleepMaskProbe 定义解耦(未编入时命令字面量照常下发,
        // 走未注册报错)。
        dispatcher.RegisterHandler(Cmd::Runtime::SleepMaskProbe(), []() {
            return std::make_unique<SleepMaskProbeHandler>();
            }, Priority::Preparation);
#endif
        // MigrateSpawn: 进程注入/迁移原语 —— 宿主层命令, 与 SetConfig 同级,
        // 无条件注册( 从 Components/Commands 迁入; 曾挂在 MINIMAL
        // 裁剪区内, 与本文件"生存命令不得被裁剪"原则冲突)。
        dispatcher.RegisterHandler(Cmd::Migrate::Spawn(), []() {
            return std::make_unique<MigrateSpawnHandler>();
            }, Priority::Survival);
        // BofExec: BOF 加载执行原语 —— 宿主层命令, 与 MigrateSpawn 同档
        // 无条件注册。执行模型见 BofHandler.hpp 文件头: 同步跑
        // go(), 不声明 ResourceKey, 慢 BOF 用 params.timeout_ms 放宽本任务超时。
        dispatcher.RegisterHandler(Cmd::Bof::Exec(), []() {
            return std::make_unique<BofHandler>();
            }, Priority::Survival);
        // InjectExec: 注入原语(远程命令驱动的注入器) —— 宿主层命令, 与 MigrateSpawn/
        // BofExec 同档无条件注册。方法注册表在 Core/Inject(加方法 =
        // 建目录 + RegisterBuiltin.cpp 加一行); handler 只做声明校验: method/
        // payload_kind/target_kind 与方法声明支持面不符 → 明确拒绝(不嗅探字节)。
        dispatcher.RegisterHandler(Cmd::Inject::Exec(), []() {
            return std::make_unique<InjectHandler>();
            }, Priority::Survival);
        // 任务系统命令面(JobCenter;  Phase 1) —— 诊断/控制面, 不得随功能裁剪。
        // job_list/job_status 只读(Discovery 档); job_kill 改变运行态(Preparation 档)。
        dispatcher.RegisterHandler(Cmd::Job::List(), []() {
            return std::make_unique<JobListHandler>();
            }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Job::Status(), []() {
            return std::make_unique<JobStatusHandler>();
            }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Job::Kill(), []() {
            return std::make_unique<JobKillHandler>();
            }, Priority::Preparation);
        // 模块命令面。
        // 定档(用户裁定): Load/Unload → Preparation(1); Status → Discovery(2)
        // (Status 是只读查询, 与其它发现类同档; Load/Unload 改变模块生命周期, 属准备动作)
        dispatcher.RegisterHandler(Cmd::Module::Load(), []() {
            return std::make_unique<ModuleLoadHandler>();
            }, Priority::Preparation);
        dispatcher.RegisterHandler(Cmd::Module::Unload(), []() {
            return std::make_unique<ModuleUnloadHandler>();
            }, Priority::Preparation);
        dispatcher.RegisterHandler(Cmd::Module::Status(), []() {
            return std::make_unique<ModuleStatusHandler>();
            }, Priority::Discovery);
    }

} // namespace cleat
