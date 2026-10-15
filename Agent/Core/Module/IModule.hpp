/**
 * @file IModule.hpp
 * @brief Agent模块接口定义
 * 
 * 每个功能/服务都是一个独立的模块，拥有自己的平台接口和提供者，
 * 并向分发器注册其处理器。
 * 
 * @note 未来动态加载设计要点：
 * - IModule是一个纯虚接口（vtable在DLL间是ABI稳定的）。
 * - 模块应该只依赖IModule/CommandDispatcher/它们自己的头文件；
 *   具体的核心类不能泄漏到模块边界外。
 */

#pragma once
#include <memory>
#include <utility>
#include <vector>

namespace cleat {

    class CommandDispatcher;

    /**
     * @brief 自包含的功能模块接口
     * 
     * 每个模块负责注册自己的处理器到分发器，
     * 并管理自己的资源和生命周期。
     */
    class IModule {
    public:
        virtual ~IModule() = default;

        /**
         * @brief 注册模块的所有处理器到分发器
         * @param dispatcher 命令分发器引用
         */
        virtual void Register(CommandDispatcher& dispatcher) = 0;

        /**
         * @brief 可选的模块清理函数
         * 
         * 在ModuleRegistry::ShutdownAll()中以反向注册顺序调用，
         * 在模块销毁之前执行（停止服务/任务/转发等）。
         */
        virtual void Shutdown() {}
    };

    /**
     * @brief 包装普通注册函数的模块适配器
     * 
     * 用于包装简单的注册函数（如Ping、服务、任务、攻击模块等）。
     */
    class FunctionModule : public IModule {
    public:
        using RegisterFn = void (*)(CommandDispatcher&);
        using ShutdownFn = void (*)();

        explicit FunctionModule(RegisterFn fn) : fn_(fn) {}

        void Register(CommandDispatcher& dispatcher) override {
            fn_(dispatcher);
        }

        void Shutdown() override {
            if (shutdown_) shutdown_();
        }

        /**
         * @brief 链式调用设置清理函数
         * @param fn 清理函数指针
         * @return 当前模块对象的引用（支持链式调用）
         * 
         * @example modules_.Add<FunctionModule>(RegisterX)
         *                       .WithShutdown([]{ ... });
         */
        FunctionModule& WithShutdown(ShutdownFn fn) {
            shutdown_ = fn;
            return *this;
        }

    private:
        RegisterFn fn_;
        ShutdownFn shutdown_ = nullptr;
    };

    /**
     * @brief 模块注册表，拥有模块列表并管理其生命周期
     * 
     * 注册所有模块并以反向顺序销毁（先声明的模块后销毁）。
     */
    class ModuleRegistry {
    public:
        /**
         * @brief 创建并获取模块所有权，返回稳定引用
         * @tparam T 模块类型
         * @tparam Args 构造函数参数类型
         * @param args 构造函数参数
         * @return 模块对象的引用
         * 
         * @note vector存储unique_ptr，所以模块对象的地址永远不会改变，
         * 即使vector增长也是如此。
         */
        template <typename T, typename... Args>
        T& Add(Args&&... args);

        void RegisterAll(CommandDispatcher& dispatcher);

        /**
         * @brief 销毁所有模块（反向注册顺序）
         */
        void ShutdownAll();

    private:
        std::vector<std::unique_ptr<IModule>> modules_;
    };

    template <typename T, typename... Args>
    T& ModuleRegistry::Add(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *ptr;
        modules_.push_back(std::move(ptr));
        return ref;
    }

    inline void ModuleRegistry::RegisterAll(CommandDispatcher& dispatcher) {
        for (auto& module : modules_) {
            module->Register(dispatcher);
        }
    }

    inline void ModuleRegistry::ShutdownAll() {
        // 以反向注册顺序运行每个模块的清理回调，
        // 然后销毁模块（通过clear()也是反向顺序）。
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
            (*it)->Shutdown();
        }
        modules_.clear(); // 反向销毁顺序
    }

} // namespace cleat
