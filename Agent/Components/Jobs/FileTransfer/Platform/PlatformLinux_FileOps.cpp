#include "IFileOps.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

namespace cleat::Tasks {

    //class FileOpsLinux : public IFileOps {
    //    // 使用 std::filesystem 实现所有方法（暂时留空，后续实现）
    //public:
    //    std::vector<FileEntry> ListDirectory(const std::string& path, std::string& error_msg) override {
    //        error_msg = "Not implemented";
    //        return {};
    //    }
    //    bool CreateDirectory(const std::string& path) override { return false; }
    //    bool FileExists(const std::string& path) override { return false; }
    //    bool DeleteFile(const std::string& path) override { return false; }
    //    std::vector<uint8_t> ReadFile(const std::string& path) override { return {}; }
    //    bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) override { return false; }
    //    std::vector<std::string> GetDrives(std::string& error_msg) override { return {}; }
    //};


} // namespace cleat::Tasks