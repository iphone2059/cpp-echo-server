# cpp-echo-server

Windows 原生 TCP/UDP Echo 服务器。工程只实现一种数据通路：注册 I/O（RIO）负责数据收发，RIO 完成队列通过 IOCP 通知工作线程；TCP 接受连接使用 AcceptEx，不存在普通 send/recv 或回退后端。

## 架构

`src` 平铺 7 个 `.cpp` 源文件，**没有头文件**：6 个业务模块各自是一个 C++20 命名模块，把声明与定义合并在同一个 `.cpp` 中；`main.cpp` 不是模块，直接实现 `wmain` 并 import 所需模块；逻辑与参数解析细节与客户端保持同一套模块划分。

| 源文件 | 模块名 | 职责 |
| --- | --- | --- |
| `main.cpp` | 普通 TU（非模块） | 命令行入口、控制台停止处理器、退出码 |
| `contract.cpp` | `ces.contract` | 参数解析、错误优先级和跨字段验证 |
| `types.cpp` | `ces.types` | Options、Event、Action、Completion、Lifecycle、算术边界与错误分类；保留 MSVC 编译约束与 CES_* 常量 |
| `native.cpp` | `ces.native` | Win32/Winsock/RIO 所有者、监听 socket、AcceptEx、注册内存与完成所有权核算 |
| `engine.cpp` | `ces.engine` | 纯引擎：Connection、Acceptor、UDP 状态机与定时器（零 Windows/RIO 引用） |
| `internal.cpp` | `ces.internal` | Worker Runtime、事件循环、完成调度、Worker 创建/停止/Join 与最终统计 |
| `server.cpp` | `ces.server` | 协议装配、停止协调与最终统计输出 |

模块依赖保持分层方向：`ces.types` 为叶子，`ces.contract` 依赖它，`ces.native` 依赖 `ces.types`/`ces.contract`，纯引擎 `ces.engine` 只依赖 `ces.types`，`ces.internal` 依赖 `ces.types`/`ces.engine`（native 与 contract 只在 `CES_PURE_ENGINE_ONLY` 守卫之外引入），`ces.server` 依赖 runtime 与 native。Windows/Winsock SDK 头放在每个模块的 `module;` 全局模块片段中。

## 构建

依赖 Windows 与 **Visual Studio 2026**（MSVC v145 x64 工具集、C++20 命名模块、clang-format）。整个实现就是一个工程 `cpp-echo-server.vcxproj`，同一个工程文件提供六套配置：

| 配置 | 源码 | 产物 |
| --- | --- | --- |
| `Debug` / `Release` | `src/*.cpp`（6 个模块 + `main.cpp`） | `cpp-echo-server.exe` |
| `DebugTests` / `ReleaseTests` | `src/*.cpp` + `tests/*.cpp`（入口 `tests/ces_test_entry.cpp`） | `cpp-echo-server-tests.exe` |
| `DebugPure` / `ReleasePure` | `src/{types,engine,internal}.cpp` + 纯引擎套件与入口，定义 `CES_PURE_ENGINE_ONLY` | `cpp-echo-server-pure-tests.exe` |

测试可执行文件按套件运行：`cpp-echo-server-tests.exe contracts|engine|pure|fault <mode>`；纯引擎可执行文件只提供 `pure` 套件，并且**不链接 ws2_32/winmm**，用来证明纯引擎不依赖原生层。

`build.ps1` 用 `vswhere` 定位 MSBuild 与 VS 自带 clang-format，先做格式检查（`--dry-run --Werror`），再依次构建 `Debug`、`DebugTests`、`DebugPure`（Release 同理），然后运行各套件、命名模块源策略、fail-fast 边界、进程契约与 reset storm；每个门禁的日志保存在 `logs/<config>-<gate>.log`，构建前后比较源码内容快照，构建或测试自身改写源码即报错。

```powershell
.\build_debug.ps1
.\build_release.ps1
```

需要调整格式时显式运行 `.ormat.ps1`；该脚本才会改写源码。

## 运行

```powershell
.\x64\Release\cpp-echo-server.exe /p tcp /s 7000 /q
.\x64\Release\cpp-echo-server.exe /p udp /s 7000 /q
```

参数：`/p tcp|udp`；`/s` 监听端口；`/t` TCP 空闲超时；`/w` 总运行秒数；`/b` socket 缓冲区；`/k` UDP 接收槽位（TCP 拒绝）；`/threads` TCP 工作线程；`/rio-buffer` 每 worker 注册缓冲区；`/cq` 每 CQ 容量；`/memory` 注册内存上限；`/q` 静默；`/stats` 最终统计；`/h` 帮助。

最终统计满足 `echo-binary-contract-v1` 的字段顺序与记账恒等式，与客户端、Rust、Zig、Swift 实现一致。
