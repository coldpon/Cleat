#include "IFileOps.hpp"

#ifdef _WIN32
#include "PlatformWin_FileOps.cpp"   // 包含 Windows 实现类
#elif __linux__
#include "PlatformLinux_FileOps.cpp" // 包含 Linux 实现类
#else
#error "Unsupported platform"
#endif

namespace cleat::Tasks {

    std::unique_ptr<Tasks::IFileOps>CreateFileOps() {
#ifdef _WIN32
        return std::make_unique<Tasks::FileOpsWin>();
#elif __linux__
        // TODO: Linux 文件传输实现(PlatformLinux_FileOps.cpp 整类注释中) ——
        // 暂不可用, 返回 nullptr 由上层判空报 not supported(与其余平台工厂同模式)
        return nullptr;
#else
#error "Unsupported platform"
#endif
    }

} // namespace cleat::Tasks