#include <memory>
#include "Components/Services/Shell/Platform/IShellOps.hpp"

#ifdef _WIN32
#include "Components/Services/Shell/Platform/PlatformWin_Shell.hpp"
#elif __linux__
#include "Components/Services/Shell/Platform/PlatformLinux_Shell.hpp"
#endif

namespace cleat {

    std::unique_ptr<IShellOps> CreateShellOps() {
#ifdef _WIN32
        return std::make_unique<ShellOpsWin>();
#elif __linux__
        return std::make_unique<ShellOpsLinux>();
#else
#error "Unsupported platform"
#endif
    }

} // namespace cleat
