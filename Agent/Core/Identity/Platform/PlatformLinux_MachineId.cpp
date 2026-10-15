/**
 * @file PlatformLinux_MachineId.cpp
 * @brief Linux 身份源采集: /etc/machine-id > /var/lib/dbus/machine-id > DMI product_uuid
 */
#include "Core/Identity/Platform/IMachineIdOps.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace cleat {
namespace identity {

    namespace {

        std::string ReadFileTrimmed(const char* path) {
            std::ifstream in(path);
            if (!in) return {};
            std::string line;
            std::getline(in, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                line.back() == '\n')) {
                line.pop_back();
            }
            return line;
        }

        std::string ReadHostname() {
            char buf[256] = { 0 };
            if (gethostname(buf, sizeof(buf)) != 0) return {};
            return std::string(buf);
        }

    } // namespace

    std::vector<std::string> CollectSourcesLinux() {
        std::vector<std::string> out;
        auto add = [&out](const std::string& s) {
            if (!s.empty()) out.push_back(s);
            };
        add(ReadFileTrimmed("/etc/machine-id"));
        add(ReadFileTrimmed("/var/lib/dbus/machine-id"));
        add(ReadFileTrimmed("/sys/class/dmi/id/product_uuid"));   // 通常需 root, 失败即跳过
        return out;
    }

    std::string HostnameFallbackLinux() { return ReadHostname(); }

} // namespace identity
} // namespace cleat
