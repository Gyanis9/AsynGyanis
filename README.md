# AsynGyanis

AsynGyanis 是面向 Linux 与 Windows 的 C++23 异步服务器引擎：协程运行时、网络协议栈与数据访问组件共用一套事件循环与生命周期契约。

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://en.cppreference.com/w/cpp/23)
[![Linux](https://img.shields.io/badge/platform-Linux-orange)](https://kernel.org)
[![Windows](https://img.shields.io/badge/platform-Windows-blue)](https://microsoft.com/windows)
[![Linux 门禁](https://img.shields.io/github/actions/workflow/status/Gyanis9/AsynGyanis/linux-ci.yml?branch=main&label=Linux%20%E9%97%A8%E7%A6%81)](https://github.com/Gyanis9/AsynGyanis/actions/workflows/linux-ci.yml)
[![Windows 门禁](https://img.shields.io/github/actions/workflow/status/Gyanis9/AsynGyanis/windows-ci.yml?branch=main&label=Windows%20%E9%97%A8%E7%A6%81)](https://github.com/Gyanis9/AsynGyanis/actions/workflows/windows-ci.yml)

[快速开始](#快速开始) · [架构](#架构) · [文档](docs/文档导航.md) · [示例](#示例) · [贡献](CONTRIBUTING.md) · [安全](.github/SECURITY.md)

## 核心特性

**异步运行时** — 每线程一个事件循环（Linux epoll、Windows 完成端口、Linux 可选 io_uring），三后端共用水平触发契约；`Task<T>` 惰性协程、两级就绪队列、`std::stop_token` 协作式取消、阻塞工作经 `AsyncExecutor` 挪出循环线程再投回指定循环。

**网络协议栈** — HTTP/1.1、HTTP/2、HTTP/3 与自研 QUIC 传输层（丢包恢复、可插拔拥塞控制、ECN、1-RTT 密钥更新）、WebSocket（含 permessage-deflate 与主题扇出）、TLS/mTLS 与证书热轮换、带连接池与 h2 多路复用的出站 HTTP 客户端（代理、重定向、按幂等性决定是否重试）。

**数据访问** — 声明式 ORM、SQL 方言层、参数化执行、LIFO 连接池、RAII 事务与保存点、把行留在服务端的流式结果集；SQLite / MySQL / PostgreSQL / Redis 四驱动，可选依赖缺失时退化为报错桩。

**配置与可观测性** — YAML/JSON 多文件装载与热重载、严格类型化取值（宁可回落也不静默变形）；结构化日志、Prometheus 文本端点、进程级指标注册表（库内通道构造即登记）、OTLP/HTTP 指标与日志推送、链路追踪。

**安全与部署** — ACME（RFC 8555）证书自动签发与续期，支持 HTTP-01 与 DNS-01；安全响应头、Basic/Bearer 认证中间件、解析与并发限额、按来源限额与在途正文内存预算；多进程 worker 与崩溃补位、systemd 就绪上报与看门狗、SIGHUP 重载、POSIX 零停机换代。

**跨平台与交付** — 只支持 Linux 与 Windows；五个模块各自成库，可静态或共享，CMake 包配置支持按组件消费并自动带出传递依赖；Conan 2 是唯一维护的包路线。

## 快速开始

需要 CMake（使用仓库预设需 ≥ 3.25）、Ninja、Conan 2、Python，以及 MSVC ≥ 19.40 / GCC ≥ 13 / Clang ≥ 17。完整依赖与环境要求见 [入门与构建](docs/入门与构建.md)。

Linux：

```bash
python3 -m pip install "conan>=2"
conan profile detect
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Windows 在已加载 `vcvars64` 的终端执行同样的三条 CMake 命令（以 `python -m pip install "conan>=2"` 起头）。第三方依赖在配置阶段自动安装，不需要手工执行 `conan install`。

跑起来并验一次本地请求：

```bash
./build/debug/samples/ReferenceServer --port 8080 --threads 4 &
curl -i http://127.0.0.1:8080/
kill %1
```

`GET /` 回 200。其余参数打 `--help`；配置驱动、HTTPS/HTTP-3、多进程与证书自动化的起法见 [部署与运维](docs/部署与运维.md)。

## 最小示例

```cpp
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Process/GracefulShutdown.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include <cstddef>
#include <memory>
#include <vector>

using namespace AsynGyanis;

int main()
{
    Core::IoContext context(2);
    const auto      address = Core::InetAddress::resolve("127.0.0.1", 8080);

    std::vector<std::unique_ptr<Net::HttpServer>> servers;
    std::vector<Core::Task<>>                     accepts;
    for (std::size_t index = 0; index < context.threadPool().threadCount(); ++index)
    {
        auto server = std::make_unique<Net::HttpServer>(context.threadPool().eventLoop(index), *address);
        server->router().get("/", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void> {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody("Hello, AsynGyanis");
            co_return;
        });
        accepts.push_back(server->start());
        servers.push_back(std::move(server));
    }
    for (std::size_t index = 0; index < accepts.size(); ++index)
    {
        context.threadPool().eventLoop(index).scheduler().schedule(accepts[index].handle());
    }

    static_cast<void>(Core::GracefulShutdown::blockStopSignals());
    Core::GracefulShutdown shutdown;
    shutdown.onShutdown([&context] { context.stop(); });
    context.run();
    return 0;
}
```

`start()` 返回的协程必须被持有到服务器收口：投递句柄后立刻离开作用域会让协程帧被销毁。这条生命周期契约适用于库里所有常驻协程，见 [架构设计](docs/架构设计.md)。

作为依赖消费：

```cmake
find_package(AsynGyanis REQUIRED COMPONENTS Net)
target_link_libraries(app PRIVATE AsynGyanis::Net)
```

只请求 `Net` 会自动带出 `Core`、`Base`、`Platform` 及其外部依赖；只请求 `Base` 不会去找 OpenSSL。安装、ABI 边界与 Conan 包见 [入门与构建](docs/入门与构建.md)。

## 架构

![AsynGyanis 模块分层、挂载点与后端选择](assets/diagrams/png/layered-architecture-light.png)

| 模块 | 库 | 职责 |
| --- | --- | --- |
| `Platform` | `AsynGyanis::Platform` | 描述符、套接字、事件通知、定时器、文件监听、原子写、编码转换、进程与时间、CPU 亲和与并行度测算、服务状态通知 |
| `Base` | `AsynGyanis::Base` | 日志、配置、异常层次、JSON/YAML 原生接口的传递依赖 |
| `Core` | `AsynGyanis::Core` | 事件循环、协程运行时、异步 socket、TLS、多进程编排 |
| `Net` | `AsynGyanis::Net` | TCP/UDP、HTTP/1.1/2/3、QUIC、WebSocket、路由与中间件、ACME、遥测出口 |
| `Database` | `AsynGyanis::Database` | 连接抽象、连接池、SQL 方言、ORM、建表迁移 |

Net 与 Database 是并列的上层模块，共用 Core 的循环与执行器。命名空间到模块名为止，include 路径从 `src/` 起算（`#include "Core/EventLoop/EventLoop.h"`）。事件循环、调度归属、协程帧与停止顺序见 [架构设计](docs/架构设计.md)。

## 文档

| 你要做什么 | 看哪里 |
| --- | --- |
| 装工具链、构建、跑测试、以包形式消费 | [入门与构建](docs/入门与构建.md) |
| 理解循环、协程生命周期与模块边界 | [架构设计](docs/架构设计.md) |
| 写 HTTP/2/3、WebSocket、QUIC、出站客户端 | [网络编程](docs/网络编程.md) |
| 定义表结构、事务、连接池与流式结果 | [数据库访问](docs/数据库访问.md) |
| 起服务、多进程、停机换代、TLS 与证书自动化 | [部署与运维](docs/部署与运维.md) |
| 配置装载与限额、日志、指标、追踪、端点鉴权 | [配置与可观测性](docs/配置与可观测性.md) |
| 复跑门禁、示例矩阵、模糊测试、跨实现验收 | [测试与验证](docs/测试与验证.md) |
| 测吞吐与延迟、比对基线、评估构建选项 | [性能测试](docs/性能测试.md) |

更多入口：[文档导航](docs/文档导航.md)、[协议规范留存说明](docs/RFC/规范说明.md)、[贡献指南](CONTRIBUTING.md)、[安全政策](.github/SECURITY.md)、[更新日志](CHANGELOG.md)。

## 示例

`ReferenceServer` 是部署形态；能力另按模块拆成各自自检的程序，逐步打印 `✓`/`✗` 并留一行机器可读结论：

```bash
python scripts/run_samples.py --list                        # 现枚举清单与步数
python scripts/run_samples.py                               # 全部跑一遍
python scripts/run_samples.py --only NetHttpDemo --repeat 3 # 重复跑，抓靠时序侥幸的用例
```

源码在 `samples/` 下按模块分目录（`samples/Net/Http.cpp`、`samples/Database/OrmAndPool.cpp`、`samples/Core/Upgrade.cpp` 等），与 [网络编程](docs/网络编程.md)、[数据库访问](docs/数据库访问.md)、[部署与运维](docs/部署与运维.md) 的章节对应。

## 测试与验证

- `ctest --preset debug` 跑全量用例；Debug 档在 AddressSanitizer 下运行，零编译器告警是提交判据。
- 仓库级门禁（格式、复杂度冻结线、公开接口基线、图引追溯、台账日期、依赖同解、版本一致）都能在本地几秒到一两分钟内跑完，清单见 [测试与验证](docs/测试与验证.md)。
- 持续验证覆盖 Linux 与 Windows 两侧构建与用例、ThreadSanitizer、覆盖率（仅 Linux 能编到的部分）、按目标分档的持续模糊，以及拿不同源代码的实现当裁判的跨实现验收（h2spec、Autobahn、aioquic、Pebble）。
- 依赖真实服务端与机构的用例一律环境变量门控，没有凭据的机器上仍然全绿。

用例条数、覆盖率百分比与模糊执行总数以**当次运行**的作业摘要为准，本文件不抄历史数字。

## 参与项目

改动前请读 [贡献指南](CONTRIBUTING.md)：代码风格、注释与报错文案口径、提交信息规范、依赖变更要走的那四处。发现安全问题请按 [安全政策](.github/SECURITY.md) 私密报告，不要开公开 issue。

版本号遵循语义化版本，唯一真源是根 `CMakeLists.txt` 的 `project(AsynGyanis VERSION ...)`；发布节奏与历史见 [更新日志](CHANGELOG.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。
