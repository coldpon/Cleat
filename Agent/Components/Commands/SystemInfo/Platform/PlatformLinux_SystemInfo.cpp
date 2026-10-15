// Commands/SystemInfo/Platform/PlatformLinux_SystemInfo.cpp
#include "PlatformLinux_SystemInfo.hpp"

#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <limits.h>

namespace cleat {
    namespace {

        std::string Trim(std::string s) {
            const char* ws = " \t\r\n";
            auto b = s.find_first_not_of(ws);
            if (b == std::string::npos) return {};
            auto e = s.find_last_not_of(ws);
            return s.substr(b, e - b + 1);
        }

        // 读取 /etc/os-release 键值（引号包裹值去引号）
        std::string ReadOsRelease(const std::string& key) {
            std::ifstream in("/etc/os-release");
            if (!in) return {};
            std::string line;
            while (std::getline(in, line)) {
                auto pos = line.find('=');
                if (pos == std::string::npos) continue;
                std::string k = Trim(line.substr(0, pos));
                if (k != key) continue;
                std::string v = Trim(line.substr(pos + 1));
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                    v = v.substr(1, v.size() - 2);
                return v;
            }
            return {};
        }

        std::string GetHostName() {
            char buf[256] = { 0 };
            if (gethostname(buf, sizeof(buf) - 1) == 0)
                return std::string(buf);
            return "Unknown";
        }

        // 与 Windows 保持同构: <machine-id>@<hostname>
        std::string GetOSVersionString() {
            std::string pretty = ReadOsRelease("PRETTY_NAME");
            if (!pretty.empty()) return pretty;
            std::string name = ReadOsRelease("NAME");
            std::string ver = ReadOsRelease("VERSION_ID");
            if (!name.empty() && !ver.empty()) return name + " " + ver;
            if (!name.empty()) return name;
            return "Linux";
        }

        std::string GetArchitecture() {
            struct utsname uts;
            if (uname(&uts) == 0) return std::string(uts.machine);
            return "Unknown";
        }

        std::string GetCPUInfo(uint32_t& out_cores) {
            out_cores = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
            std::ifstream in("/proc/cpuinfo");
            if (!in) return "Unknown CPU";
            std::string line;
            while (std::getline(in, line)) {
                auto pos = line.find(':');
                if (pos == std::string::npos) continue;
                std::string key = Trim(line.substr(0, pos));
                if (key == "model name" || key == "Hardware")
                    return Trim(line.substr(pos + 1));
            }
            return "Unknown CPU";
        }

        uint64_t GetTotalRamBytes() {
            long pages = sysconf(_SC_PHYS_PAGES);
            long page_size = sysconf(_SC_PAGE_SIZE);
            if (pages > 0 && page_size > 0)
                return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
            return 0;
        }

        uint32_t GetCurrentPID() {
            return static_cast<uint32_t>(getpid());
        }

        std::string GetProcessName() {
            char buf[PATH_MAX] = { 0 };
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                std::string path(buf);
                auto pos = path.find_last_of('/');
                return (pos != std::string::npos) ? path.substr(pos + 1) : path;
            }
            return "unknown";
        }

        std::string GetPrivilegeLevel() {
            // root -> High, 其余 -> Medium（Linux 无 SYSTEM 概念, 服务化 root 即最高）
            return (geteuid() == 0) ? "High" : "Medium";
        }

        std::string GetLocalIP() {
            // UDP connect 法: 不发包即可取默认路由出接口地址
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) return {};
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(53);
            if (inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr) != 1) {
                close(sock);
                return {};
            }
            if (connect(sock, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) != 0) {
                close(sock);
                return {};
            }
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
                close(sock);
                return {};
            }
            close(sock);
            char ip[INET_ADDRSTRLEN] = { 0 };
            if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)) == nullptr)
                return {};
            return std::string(ip);
        }

    } // anonymous namespace

    SystemInfo SystemInfoLinux::GetSystemInfo() {
        SystemInfo info;
        info.hostname = GetHostName();
        info.os_name = "Linux";
        info.os_version = GetOSVersionString();
        info.architecture = GetArchitecture();
        info.total_ram_bytes = GetTotalRamBytes();
        uint32_t cores = 0;
        info.cpu_brand = GetCPUInfo(cores);
        info.cpu_cores = cores;
        info.local_ip = GetLocalIP();
        info.pid = GetCurrentPID();
        info.process_name = GetProcessName();
        info.privilege = GetPrivilegeLevel();
        return info;
    }

} // namespace cleat
