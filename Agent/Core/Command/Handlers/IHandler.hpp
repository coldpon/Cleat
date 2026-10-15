#pragma once

#include "Common/ThirdParty/json.hpp"
#include "Core/Command/Handlers/ResourceKey.hpp"
#include "Core/Command/Handlers/CommandRequest.hpp"
#include <vector>

namespace cleat {

    /**
     * @brief 命令处理器接口（所有业务逻辑的基类）
     *
     * - 每个命令对应一个 IHandler 实现。
     * - Execute 接收 JSON 参数，返回 JSON 响应（或二进制数据）。
     * - 保证异常安全：CommandDispatcher 会捕获所有异常并转换为错误 JSON。
     */
    class IHandler {
    public:
        virtual ~IHandler() = default;

        /**
         * @brief 执行命令
         * @param req 请求对象(`req.params` / `payload` / `ctx`)
         * @return 响应 JSON（必须包含 "status" 和 "data" 或 "error" 字段）
         * @details
         * **无默认参数是有意的**: 旧形态 `Execute(req.params, req.payload = {})` 里的默认值让第二个
         * 参数看起来可选, 于是"没人用它"成了普遍印象 —— 而 F.9 已核实有 4 个在树功能依赖它。
         */
        virtual nlohmann::json Execute(const CommandRequest& req) = 0;

        /**
         * @brief 声明本 handler 会独占的资源
         * @return 默认空 key = **不参与互斥**, 与今天无互斥的行为等价
         * @details
         * 做成**可选虚函数**而非接口必改项: 把 `ResourceKey` 的改动面从"100+ handler 全改签名"
         * 降到"**只有真正持有资源的 ~10 个 handler 覆写**"。
         *
         * (`path` / `pid` / 端口) —— 无参形态只能表达"全局单例"那一类资源。故契约修订为
         * `ResourceOf(const CommandRequest&)`: 不参与互斥者忽略请求即可。
         *
         * 覆写者取键要用 `ResourceKey.hpp` 的工厂(`MakeFileKey`/`MakeProcessKey`/`GlobalKey`…),
         * **不要**手写归一化 —— 不归一化时 `a.txt` 与 `A.TXT` 会是两个 key, 互斥形同虚设。
         *
         * 释放"策略被强制放行并告警 —— 声明前先确认该命令确实需要一个独占窗口。
         */
        virtual ResourceKey ResourceOf(const CommandRequest& req) const {
            (void)req;
            return ResourceKey{};
        }
    };

} // namespace cleat