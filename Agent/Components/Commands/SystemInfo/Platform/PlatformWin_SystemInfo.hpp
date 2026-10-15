// Features/SystemInfo/Platform/PlatformWin_SystemInfo.hpp
#pragma once
#include "ISystemInfo.hpp"

namespace cleat {

    class SystemInfoWin : public ISystemInfo {
    public:
        SystemInfo GetSystemInfo() override;
    };

} // namespace cleat