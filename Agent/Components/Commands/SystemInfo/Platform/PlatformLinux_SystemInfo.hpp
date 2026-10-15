// Commands/SystemInfo/Platform/PlatformLinux_SystemInfo.hpp
#pragma once
#include "ISystemInfo.hpp"

namespace cleat {

    class SystemInfoLinux : public ISystemInfo {
    public:
        SystemInfo GetSystemInfo() override;
    };

} // namespace cleat
