#include "Core/Inject/InjectionRegistry.hpp"

namespace cleat {
namespace injection {

    InjectionRegistry& InjectionRegistry::Instance() {
        static InjectionRegistry registry;
        return registry;
    }

    void InjectionRegistry::Register(std::unique_ptr<IInjectionMethod> method) {
        if (!method || method->Name() == nullptr || method->Name()[0] == '\0') {
            return;   // 无名方法不可寻址, 注册即垃圾
        }
        const std::string key = method->Name();
        std::lock_guard<std::mutex> lock(mutex_);
        methods_[key] = std::move(method);
    }

    IInjectionMethod* InjectionRegistry::Find(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = methods_.find(name);
        return it == methods_.end() ? nullptr : it->second.get();
    }

    std::vector<std::string> InjectionRegistry::KnownMethods() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        names.reserve(methods_.size());
        for (const auto& kv : methods_) {
            names.push_back(kv.first);
        }
        return names;
    }

} // namespace injection
} // namespace cleat
