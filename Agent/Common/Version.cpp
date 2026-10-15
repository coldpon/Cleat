/**
 * @file Version.cpp
 * @brief 构建标识的唯一求值点(见 Version.hpp 文件头)
 */

#include "Common/Version.hpp"

#include <string>

namespace cleat {

    namespace ver {

        const char* BuildId() {
            // 全仓唯一一处 __DATE__/__TIME__ 展开: FirstConnect(会话接管三元校验)
            // 与 AgentDescribe(自述)经此函数取到同一字符串。
            static const std::string s = __DATE__ " " __TIME__;
            return s.c_str();
        }

    } // namespace ver

} // namespace cleat
