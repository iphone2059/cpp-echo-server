# cpp-echo-server

Windows 原生 TCP/UDP Echo 服务端。工程只实现一种数据通路：注册 I/O（RIO）负责数据收发，RIO 完成队列通过 IOCP 通知工作线程。不存在普通 `send`/`recv`、数据面事件轮询或其他回退后端。

## 架构

- TCP 接入：多个预投递 `AcceptEx`，完成由接入 IOCP 回收，再把已连接的 RIO socket 分配给固定工作线程。
- TCP 数据：每个连接拥有一个 RIO RQ；每个工作线程独占一个 RIO CQ、一个 IOCP 和一块预注册内存。
- UDP 数据：预投递固定深度的 `RIOReceiveEx`，完成后用 `RIOSendEx` 回显，再恢复接收。
- CQ 唤醒：IOCP 只表示“RIO CQ 可读”；线程批量 `RIODequeueCompletion` 排空 CQ 后调用 `RIONotify` 重装通知。
- 生命周期：所有 TCP 工作线程首次成功武装 RIO 通知后才开放接入；停止时先关闭 `AcceptEx` 接入并收回全部已发布 handoff，再向工作线程发布 admission-closed 屏障。连接数、handoff 与 RIO 完成全部归零后才释放注册资源。
- UDP 停止或运行期错误会先关闭 socket 请求取消，再继续排空 CQ；注册内存、请求上下文和 CQ 在 `outstanding == 0` 前不会释放。
- TCP 空闲截止时间存储在工作线程私有的预分配索引最小堆中；IOCP 等待时间直接取最近截止时间，不周期扫描全部连接。
- TCP 工作线程的数据面等待由 RIO CQ 的 IOCP 通知和最近截止时间驱动；TCP 主协调器以最长 10 ms 间隔采样停止/失败控制状态，接入线程和 UDP 循环使用最长 100 ms 的有界 IOCP 等待促进控制与截止时间收敛。因此不能把整个进程描述为“无轮询”，但这些控制等待不处理替代的数据面路径。
- `RIONotify` 只有 `ERROR_SUCCESS` 被接受；返回的其他状态直接作为原生错误报告。`RIO_CORRUPT_CQ`、必需的通知失败和不可缺少的 IOCP 控制投递失败属于内部不变量损坏，进程以退出码 4 确定性终止，不重试、不轮询 CQ，也不切换后端。
- 只有在目标硬件测量证明关闭尾延迟或控制面 CPU 成为实质瓶颈，并且另行审查专用控制唤醒的所有权与终态收敛后，才考虑替换上述 10/100 ms 有界等待；该优化不属于负载数据快速路径。

## 构建

依赖 Windows、Visual Studio MSVC x64 工具集、CMake 3.28+、Ninja 和 clang-format。脚本通过 `vswhere` 进入 MSVC 开发环境，格式化本项目源码，使用静态 CRT，执行干净构建和全部测试。

```powershell
.\build_debug.ps1
.\build_release.ps1
```

MSVC 使用 `/std:c++latest`，即已安装工具集所支持的最新 C++ 特性集；这不宣称工具集已经完整实现最终 C++26 标准。

## 运行

```powershell
.\build\release\cpp-echo-server.exe /p tcp /s 7000 /threads 8 /cq 65536 /memory 2147483648 /stats
.\build\release\cpp-echo-server.exe /p udp /s 7000 /k 4096 /cq 8192 /memory 1073741824 /stats
```

参数：`/p tcp|udp` 选择协议；`/s` 端口；`/t` TCP 空闲超时秒数；`/w` 总运行秒数；`/b` socket 缓冲区；`/k` UDP 并发深度；`/threads` TCP 工作线程；`/rio-buffer` 每槽字节数；`/cq` 每 CQ 容量；`/memory` 注册内存上限；`/q` 静默；`/stats` 输出统计。

`/stats` 保留 TCP 的逐工作线程记录，并在所有工作线程加入、接入屏障关闭且 RIO 终态完成排空后输出 `final protocol=tcp ... active=0` 汇总。UDP 在通知状态解除且 `outstanding=0` 后输出 `final protocol=udp` 汇总。`bytes` 只累计成功 RIO 发送完成的字节，`MiB_per_sec` 使用至少 1 ms 的保护后运行时间；UDP 的 receive/completion 数还包含停止期间排空的终态完成，因此不把接收块数标为逻辑 echo 数。

参数解析是严格的：未知开关、空值或非法值即返回退出码 1；即使同时传入 `/h`，也不会掩盖其他格式错误。

TCP 的 CQ 容量至少按每连接两个完成槽计算；UDP 至少按深度的两倍配置。回环结果主要反映本机协议栈、调度与内存路径，不代表真实网络或目标 NIC 的上限；极限值应结合目标 CPU、NUMA、NIC/RSS 队列和实际 p99/p999 测量调优。
