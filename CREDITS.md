# Credits

本项目的实现受益于以下公开工作与开源组件，在此致谢。
（思路与技术路线的参考均已在对应源码文件头注释中就地声明。）

## 技术路线参考

- **[AdaptixC2](https://github.com/Adaptix-Framework/AdaptixC2)** (GPL-3) —
  BOF/COFF 加载器的重定位口径（覆盖的重定位类型集合与分节映射方式）参考其
  `beacon/bof_loader` 实现；本项目加载器为独立实现，未复制其代码。
- **[Havoc](https://github.com/HavocFramework/Havoc)** — 睡眠掩码的窗口执行链
  （timer queue + 上下文链 + 整镜像加密）与其 `Demon` 的 TimerObf 思路同源；
  多线程停靠协议为本项目为自身架构独立设计。

## 上游开源组件

- **[libsodium](https://libsodium.gitbook.io)** (ISC) — 加密后端
- **[mbed TLS](https://www.trustedfirmware.com/projects/mbed-tls/)** (Apache-2.0) — TLS 支持（`Agent/Common/ThirdParty/mbedtls`）
- **[nlohmann/json](https://github.com/nlohmann/json)** (MIT) — JSON 头序列化（`json.hpp`，含 Hedley）
- **[MsQuic](https://github.com/microsoft/msquic)** (MIT) — QUIC 通道
- **cleat-web 前端模板** (MIT, © Sat Naing) — 见 `cleat-web/LICENSE`

## 灵感与生态

- Cobalt Strike 的 Beacon Object File 公开约定（`MODULE$FUNC`、Beacon API 语义）
- Ekko / Foliage 等 sleep-obfuscation 公开研究
