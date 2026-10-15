#include "Components/Commands/SystemInfo/Platform/ISystemInfo.hpp"
#include <memory>

#ifdef _WIN32
#include "Components/Commands/SystemInfo/Platform/PlatformWin_SystemInfo.hpp"
#elif __linux__
#include "Components/Commands/SystemInfo/Platform/PlatformLinux_SystemInfo.hpp"
#endif

namespace cleat {

    std::unique_ptr<ISystemInfo> CreateSystemInfo() {
#ifdef _WIN32
        return std::make_unique<SystemInfoWin>();
#elif __linux__
        return std::make_unique<SystemInfoLinux>();
#else
#error "Unsupported platform"
#endif
    }

} // namespace cleat