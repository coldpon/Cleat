#include "Core/Comms/Platform/INetworkOps.hpp"

#ifdef _WIN32
#include "Core/Comms/Platform/PlatformWin_Network.hpp"
#elif __linux__
#include "Core/Comms/Platform/PlatformLinux_Network.hpp"
#else
#error "Unsupported platform"
#endif

namespace cleat {

    // Platform implementation files are compiled separately by CMake
    // per platform; this factory only instantiates the right one.
    // Do not #include the .cpp here (would cause duplicate symbols).
    std::unique_ptr<INetworkOps> CreateNetworkOps() {
#ifdef _WIN32
        return std::make_unique<NetworkOpsWin>();
#elif __linux__
        return std::make_unique<NetworkOpsLinux>();
#else
#error "Unsupported platform"
#endif
    }

} // namespace cleat
