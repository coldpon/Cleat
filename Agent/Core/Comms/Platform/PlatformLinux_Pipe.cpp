#include "Core/Comms/Platform/IPipeOps.hpp"

namespace cleat {

    // 命名管道为 Windows 专属传输(L1 范围); Linux 侧如需等价物
    // (Unix domain socket)另起 PlatformLinux_ 实现并在 CMake 登记。
    std::unique_ptr<IPipeOps> CreatePipeOps() {
        return nullptr;
    }

} // namespace cleat
