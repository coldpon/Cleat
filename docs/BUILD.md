# 构建指南

> 本文是"拿到源码 → 编出可执行产物"的完整教程。当前基准平台：**Windows + VS2022**。

## 1. 要求

| 组件 | 要求 |
| --- | --- |
| Agent | Windows 10+，Visual Studio 2022（含 C++ 桌面开发），CMake ≥ 3.20，Git |
| TeamServer | Go 1.21+ |
| cleat-web | Node.js 18+ / pnpm |

## 2. 快速开始（Windows）

```cmd
:: 1) 依赖（一次性）：下载官方 libsodium 预编译包并按构建期望布局安装
scripts\build_libsodium.cmd

:: 2) 配置 + 编译（x64 Release）
cmake -S Agent -B Agent/build -G "Visual Studio 17 2022" -A x64
cmake --build Agent/build --config Release --parallel

:: 产物
::   Agent/build/Release/CleatAgent.exe
```

首次 configure 不需要网络（依赖已由步骤 1 就位）。

## 3. 依赖说明

设计原则：**仓库不携带任何二进制依赖**。所有第三方产物由脚本站（或包管理器）提供，
仓库保持纯源码。

### libsodium（必需）

上游**不提供 CMake**，因此本仓库提供两种正规途径（任选其一）：

1. **官方预编译包（推荐，零编译器需求）**：`scripts\build_libsodium.cmd`
   - 下载 libsodium 官方 MSVC 包，**瘦身安装**（仅 `include/` + x64/Win32 的
     Debug/Release 静态库，约 13 MB；完整包 77 MB 的大部分变体构建并不需要）
   - 重复执行可安全"治愈"任何多余内容
2. **vcpkg**：`vcpkg install libsodium`，CMake 会自动探测 `unofficial-sodium`，
   无需跑脚本

Linux 侧使用发行版包（`apt install libsodium-dev`），CMake 走 pkg-config。

### mbedTLS（仅 Linux 消费）

上游自带 CMake，构建时由 FetchContent **自动拉取**（v3.6.3），无需手工处理。

### msquic（可选，QUIC 通道）

**默认关闭**——不带 QUIC 的四通道形态可直接编译，且产物小 ~2.5 MB。
需要 QUIC 时：

```cmd
:: 1) 构建 msquic 静态库（自动拉取 msquic + quictls + xdp 头文件与便携 Perl；
::    首次约 15-30 分钟，结果安装到 Agent/Common/ThirdParty/msquic/）
scripts\build_msquic_quictls.cmd

:: 2) 打开开关重新配置
cmake -S Agent -B Agent/build -G "Visual Studio 17 2022" -A x64 -DCLEAT_WITH_QUIC=ON
```

> 为什么默认关：msquic+quictls 静态链入约 +2.5 MB，并非所有部署都需要；
> 且其源码构建链（Perl/quictls/xdp）较重，交给需要的人按需开启。
> `-DCLEAT_WITH_QUIC=ON` 但缺库时，configure 会给出明确指引后中止。

## 4. 构建配置选项（速查）

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CLEAT_WITH_QUIC` | OFF | 链接 QUIC 通道（见上） |
| `CLEAT_WITH_MASK` | OFF | 睡眠掩码运行时（要求 /MD 运行时，见下） |
| `CLEAT_RUNTIME_MD` | ON | 动态 CRT（/MD）。**掩码构建的硬前提**，构建系统会拒绝 /MT + 掩码组合 |
| `CLEAT_BUILD_TESTS` | OFF | 测试目标（本公共树不含测试套件，见 §6） |

## 5. 其它组件

```bash
# TeamServer
cd TeamServer && go build ./cmd/teamserver

# cleat-web
cd cleat-web && pnpm install && pnpm build
```

## 6. 测试

公共树**不包含**测试套件（`CLEAT_BUILD_TESTS=ON` 时会得到明确的提示而非报错）。
核心层的回归由内部测试树保障；框架层提供的可观测能力包括：

- 睡眠掩码自检探针（普查 + 加密原语 + 加解密往返）
- 通道健康/自检日志（启动即打印通道描述符与不可用原因）

## 7. 疑难解答

| 现象 | 处理 |
| --- | --- |
| configure 报 "libsodium 未就绪" | 跑 `scripts\build_libsodium.cmd`，或用 vcpkg 提供 `unofficial-sodium` |
| configure 报 "CLEAT_WITH_QUIC=ON 但未找到 msquic" | 跑 `scripts\build_msquic_quictls.cmd`，或改回默认 OFF |
| 链接警告 `LNK4098`（LIBCMT 冲突） | **无害**。官方 libsodium 静态库用 /MT 构建，与 /MD 主产物共存时链接器提示；本项目一贯配置，实测稳定 |
| 构建后提示 "跳过 POST_BUILD TLS 目录检查" | 正常：该检查脚本不随公共仓发布，属构建期防线之一，运行期有等价 taint 兜底 |
| Debug 配置 | 同一脚本一次安装即含 Debug/Release 静态库，直接 `--config Debug` 可用 |

## 8. 验证状态

- **Windows VS2022 x64 Release**：configure → build 全量实测通过（零错误），
  产物 `CleatAgent.exe`
- Linux：Agent 构建路径已接线（pkg-config + FetchContent），以实际发行版环境为准
