// Features/FileManager/Platform/IFileOps.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace cleat {
	namespace Tasks{
		struct FileEntry {
			std::string name;
			uint64_t size_bytes = 0;
			bool is_directory = false;
			bool is_hidden = false;
			bool is_readonly = false;
			std::chrono::system_clock::time_point modified_time;
		};

		class IFileOps {
		public:
			virtual ~IFileOps() = default;

			// 列出目录，失败时 error_msg 会被填充
			virtual std::vector<FileEntry> ListDirectory(const std::string& path, std::string& error_msg) = 0;
			virtual bool CreateDirectory(const std::string& path) = 0;
			virtual bool FileExists(const std::string& path) = 0;
			virtual bool DeleteFile(const std::string& path) = 0;
			virtual std::vector<uint8_t> ReadFile(const std::string& path) = 0;
			virtual bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) = 0;
			virtual std::vector<std::string> GetDrives(std::string& error_msg) = 0; // 也加上错误参数

			// 流式接口（文件传输用）：分段读 + 追加写，避免大文件整体进出内存
			virtual bool ReadFileChunk(const std::string& path, uint64_t offset, size_t size, std::vector<uint8_t>& out) = 0;
			virtual bool AppendFile(const std::string& path, const std::vector<uint8_t>& data) = 0;
			virtual uint64_t GetFileSize(const std::string& path) = 0;

			// 原子替换: 将 tmp_path 移动到 dest_path(覆盖已存在), 用于上传完成后落盘。
			// 上传先写临时文件, 全部完成后再替换目标, 失败/中断时原文件不丢失。
			virtual bool ReplaceFile(const std::string& tmp_path, const std::string& dest_path) = 0;

		};

		std::unique_ptr<IFileOps> CreateFileOps();
	}
	

} // namespace cleat