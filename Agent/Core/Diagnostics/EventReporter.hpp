/**
 * @file EventReporter.hpp
 * @brief 日志事件上报器(身份/诊断重构 ①: 从 AgentContext 抽出, )
 * @details
 * 职责: Agent 自日志 → Server 的遥测上报管线 —— 节流/去重/有界队列/会话绑定。
 * **不含**: 网络细节(出站经 SetSender 注入的回调, 由装配根绑定 OutboundSink)、
 * 生命周期(纯逻辑类, 无线程)。
 *
 *   - 日志线程: Logger EventSink → HandleEventReport(节流 + 有界入队)
 *   - 主循环:   Drain(锁内限量取帧 → 锁外发送), 发送失败丢弃不重试(EventReport 低价值)
 *
 * 行为保持清单(迁移验收项): 10s 节流 + 512 上限去重表、队列 256 有界、
 * sid==0 整批丢弃(防匿名注册风暴)、发送时重绑当前 sid、递归入 sink 防护。
 *
 * 限速(实现细节, 见 .cpp 匿名命名空间):
 *   单条 10s 去重 → 全局令牌桶(ERROR 2/s·突发5, 其余 5/s·突发10)
 *   → 队列 256 预检 → Drain 每轮 ≤ 32 条。
 *   主循环需周期调用 Drain; 长期不调会攒满队列并开始丢弃新事件。
 */
#pragma once

#include "Common/Logger/Logger.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cleat {

    class EventReporter {
    public:
        /// 出站回调: 帧字节 → 发送(绑定当前会话)。返回 false = 发送失败(丢弃剩余)。
        using Sender = std::function<bool(const std::vector<uint8_t>&)>;

        /// 注入出站回调(装配期; 未注入前 HandleEventReport 直接丢弃)
        void SetSender(Sender sender);

        /// 挂载 Logger EventSink(幂等; 重复调用覆盖)
        void Install();

        /// 只读: 是否已 Install(装配根就绪断言用; 幂等 Install 的完成凭证)
        bool IsInstalled() const { return installed_; }

        /// 只读: 出站回调是否已绑定(装配根就绪断言用)
        bool IsSenderSet() const { return senderSet_; }

        /// 析构: 对称注销 Logger EventSink。
        /// Install 注册的 lambda 捕获裸 this, 而 Logger 是函数级静态单例(晚于本类
        /// 析构); 本类又是 AgentContext 最后声明的成员(最先析构)——不注销则此后
        /// 任何 WARN+ 日志(成员析构期 ~SessionLoop/~CommsManager、静态析构期
        /// migrate/任务 detached 线程)都会经悬垂 this 触碰已释放的 mutex_/queue_
        /// = UAF。注销动作是"注册者负责反注册"规约的落点。
        ~EventReporter();

        /// 日志线程入口: 节流 + 去重 + 有界入队(递归入 sink 防护)
        void HandleEventReport(const EventRecord& rec);

        /// 主循环入口: 锁内限量取帧(每轮上限 32) → 逐帧重绑当前 sid 后发送
        void Drain();

    private:
        std::mutex mutex_;                                   ///< 队列锁(仅保护 queue_)
        std::deque<std::vector<uint8_t>> queue_;             ///< 待发上报帧(有界 256)
        Sender sender_;                                      ///< 出站回调
        bool senderSet_ = false;
        bool installed_ = false;                             ///< Install 是否已注册(析构仅据此注销, 防误清他人 sink)
    };

} // namespace cleat