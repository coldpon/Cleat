# ===========================================================================
# 第三方依赖(公共树)接线。
#
# 说明: 开发树 vendor 了预编译 libsodium 与整棵 mbedTLS; 公共树各归其位:
#   - libsodium(Windows): 无上游 CMake —— 由 vcpkg(unofficial-sodium) 或
#     scripts/build_libsodium.cmd 解压的**官方 MSVC 预编译包**提供,
#     消费逻辑在 Agent/CMakeLists.txt 的 libsodium 段(FATAL 指引齐全)。
#   - libsodium/mbedTLS(Linux): 发行版包(pkg-config / find_package)。
#   - mbedTLS(Linux 消费): 上游自带 CMake, 此处 FetchContent 自动拉取。
#   - msquic: 不自动拉取(构建依赖 perl/quictls/xdp, 见脚本)。需要 QUIC 时:
#       1) scripts\build_msquic_quictls.cmd
#       2) cmake -DCLEAT_WITH_QUIC=ON ...
#     默认 OFF —— 无 QUIC 的四通道形态可直接编译, 体积少约 2.5 MiB。
# ===========================================================================
include(FetchContent)

# agent 全静态纪律: 依赖一律静态库(before MakeAvailable 生效)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "agent deps are static" FORCE)

# ---- mbedTLS(仅 Linux 消费端: HTTPS / WSS; 上游自带 CMake) ----
if(NOT WIN32)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(mbedtls)
    # 兼容目标: 主 CMakeLists 按 `mbedtls_static` 链接(上游无此目标名)
    add_library(mbedtls_static INTERFACE)
    target_link_libraries(mbedtls_static INTERFACE mbedtls mbedx509 mbedcrypto)
endif()
