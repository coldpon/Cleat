<div align="center">

<img src="docs/images/banner.png" width="640" alt="Cleat"/>

[![status](https://img.shields.io/badge/status-work%20in%20progress-orange)](https://github.com/coldpon/Cleat/commits/main)
[![License: MIT](https://img.shields.io/github/license/coldpon/Cleat?color=blue)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![Go](https://img.shields.io/badge/Go-1.21%2B-00ADD8?logo=go&logoColor=white)
![React](https://img.shields.io/badge/React-TS-61DAFB?logo=react&logoColor=black)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-6E7681)
[![stars](https://img.shields.io/github/stars/coldpon/Cleat?color=yellow)](https://github.com/coldpon/Cleat/stargazers)
[![issues](https://img.shields.io/github/issues/coldpon/Cleat)](https://github.com/coldpon/Cleat/issues)
[![last commit](https://img.shields.io/github/last-commit/coldpon/Cleat)](https://github.com/coldpon/Cleat/commits/main)

**简体中文** | [English](README_EN.md)

</div>
## 核心特性

### 数据平面 · 混合帧协议

`JSON 头 + 0x00 + 二进制载荷`。帧边界由 JSON 规范背书（首个裸零必是分隔符），中继节点只读头部即可路由转发，二进制段零膨胀。

### 控制平面 · 运行时通道链

HTTPS / TCP / DNS（含 DoH，RFC 8484）/ ICMP / WebSocket / QUIC（可选）六类通道，
优先级降级 + 回探 + 防震荡；`SetConfig` 远程换链、原子生效、无重启。

### 时间平面 · 条件睡眠掩码

低频通道空闲期整镜像加密驻留；多线程"停靠协议 + fail-closed 普查"，附六条编译形态红线（每条都是一次真机崩溃的教训）。

**此外：**

- **会话层与传输层解耦** —— 六类通道对上层是同一条帧字节流；分片/重组、加密、节流全部在传输层内消化
- **模块化能力** —— 能力以模块交付、运行时装载/卸载；核心命令面与模块命令面分离
- **Pivot 转发** —— 多级中转只解帧头即可路由，无需触碰载荷
- **工程纪律** —— 配置单一事实源、注册点唯一、畸形输入 fail-closed、绝不自研密码学

架构全貌、组件边界与设计原则见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 界面预览

![Cleat 操作台](docs/images/console.png)

> 操作台：会话管理、文件浏览、模块化能力入口（演示环境，主机信息已作演示化处理；界面为完整能力形态，本仓库为核心版）

![运行时通道配置](docs/images/runtime-config.png)

> 运行时通道配置：可视化编辑、校验后一键远程下发 —— 换链不重启

## 快速开始（Windows）

```cmd
:: 1) 依赖（一次性；官方 libsodium 预编译包，无需编译器）
scripts\build_libsodium.cmd

:: 2) 配置 + 编译（x64 Release）
cmake -S Agent -B Agent/build -G "Visual Studio 17 2022" -A x64
cmake --build Agent/build --config Release --parallel
```

其余组件：`cd TeamServer && go build ./cmd/teamserver`；前端构建见 `cleat-web/`。
QUIC 可选通道、vcpkg 依赖方式、疑难解答等完整说明见 [docs/BUILD.md](docs/BUILD.md)。

## 仓库结构

```
Agent/         C++ 落地端（Core：通信 / 掩码 / 任务；Components：Pivot / Shell / 文件传输…）
TeamServer/    Go 服务端
cleat-web/     操作台前端
scripts/       依赖构建脚本（libsodium / msquic）
docs/          文档（架构 / 构建）
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构总览与设计原则 |
| [BUILD.md](docs/BUILD.md) | 构建指南与疑难解答 |

## 项目状态与维护节奏

**这是一个开发中的工程作品，不是开箱即用的成品工具。**

- 起步于个人的业余练手项目，并非为交付生产使用而设计；要用它干活，需要补齐的还很多
- Agent 相对完整；TeamServer、前端与配套能力仍在持续完善中 —— 请自行评估所有缺口
- Issue 与 PR 会认真看，但**响应可能不快**（业余时间维护，数天到数周属正常，请多担待）
- 不承诺路线图与发布节奏；合并以"不破坏既有约束"为底线
- 用于生产或关键场景前，请自行完成充分评估

## 致谢

第三方组件、设计灵感与实现口径参考的声明，见 [CREDITS.md](CREDITS.md)。

## 合规声明

This tool is designed for **AUTHORIZED security testing and red team
operations ONLY**. Unauthorized use is strictly prohibited and may violate
local and international laws. Use at your own risk.

本工具仅面向**已获授权**的安全测试与红队演练。未经授权的使用被严格禁止，
可能违反当地及国际法律，后果自负。本仓库不提供检测规避（免杀）相关的
技术支持、承诺或讨论。

## License

[MIT](LICENSE)
