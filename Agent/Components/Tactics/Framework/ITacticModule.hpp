#pragma once
#include "Common/ThirdParty/json.hpp"
#include <string>

namespace cleat {

    /**
     * @brief 战术能力模块接口（原 IAntiModule， 改名消歧）
     *
     * 【我不是谁】与 Core/Module/IModule.hpp 是两套东西，别混淆：
     *   - IModule       : 可动态下发的 DLL 模块（ModExec 入口，ModuleManager 反射加载）
     *   - ITacticModule : 编译进核心的战术能力对象，TacticManager 静态注册 + 配置驱动
     * 两者都叫「模块」：一个是功能包，一个是能力对象。
     *
     * 生命周期：RegisterModule 一次（装配期）→ Apply(config) 多次（服务端下发配置）
     *          → 动作型命令经 TacticManager::GetModule<T>() 取用。
     */
    class ITacticModule {
    public:
        virtual ~ITacticModule() = default;

        // 应用配置(由 TacticManager 调用); 返回是否全部成功,
        // 失败时 TacticManager 会汇总并回传给 Server
        virtual bool Apply(const nlohmann::json& config) = 0;

        // 返回模块名称，用于日志和配置索引
        virtual std::string GetName() const = 0;
    };

} // namespace cleat