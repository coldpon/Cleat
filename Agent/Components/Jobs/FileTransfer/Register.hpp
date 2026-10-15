#pragma once
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Components/Jobs/FileTransfer/Handler/FileDownloadReqHandler.hpp"
#include "Components/Jobs/FileTransfer/Handler/FileUploadReqHandler.hpp"
#include "Components/Jobs/FileTransfer/Handler/FileUploadDataHandler.hpp"
#include "Common/Protocol/CmdDef.hpp"

namespace cleat {

    /**
     * @brief 注册文件传输任务（FileTransfer）
     *
     * 下载/上传分块任务，均为重量级 I/O 优先级。
     */
    inline void RegisterFileTransfer(CommandDispatcher& dispatcher) {
        dispatcher.RegisterHandler(Cmd::FileManager::DownloadReq(),
            []() { return std::make_unique<FileDownloadReqHandler>(); }, Priority::HeavyIO);
        dispatcher.RegisterHandler(Cmd::FileManager::UploadReq(),
            []() { return std::make_unique<FileUploadReqHandler>(); }, Priority::HeavyIO);
        dispatcher.RegisterHandler(Cmd::FileManager::UploadData(),
            []() { return std::make_unique<FileUploadDataHandler>(); }, Priority::HeavyIO);
    }

} // namespace cleat
