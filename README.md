# AsynGyanis

> 基于 C++20 协程、epoll 边缘触发驱动的工业级高性能异步 HTTP 服务器框架

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Linux](https://img.shields.io/badge/platform-Linux-orange)](https://kernel.org)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## 特性

- **epoll 边缘触发 I/O** — 网络 I/O 走非阻塞 syscall（`accept4`/`recv`/`send`）+ epoll EPOLLET 边缘触发，统一事件循环
- **真多线程** — `IoContext` 每线程一个 `EventLoop` + 独立 `HttpServer`，`SO_REUSEPORT` 内核级负载均衡
- **C++20 协程** — `Task<T>` 惰性启动，`co_await` 挂起/恢复，`EpollAwaiter` 等待 fd 就绪后自动恢复
- **工作窃取调度** — `Scheduler` 本地无锁队列 + 全局队列，空闲线程自动窃取任务
- **并发连接处理** — accept 循环不阻塞，每个连接独立协程并发执行
- **TLS/HTTPS 支持** — 基于 OpenSSL 的 `TlsSocket`，`SSL_read`/`SSL_write` + epoll 非阻塞集成
- **HTTP/1.1 Keep-Alive** — 持久连接 + llhttp 增量解析
- **优雅启停** — 基于 `std::stop_token` 的协作式取消，`SIGINT`/`SIGTERM` 安全退出
- **热加载配置** — 基于 inotify 的 YAML/JSON 配置文件自动重载
- **结构化日志** — 6 级日志，4 种 Sink（控制台/文件/滚动/异步），C++20 `std::format`
- **URL 路由** — 精确匹配、参数化路径（`:id`）、通配符（`*`）、中间件洋葱模型
- **IPv4/IPv6 统一地址** — `Core::InetAddress` 封装 `sockaddr_storage`，支持 DNS 解析

## 架构

```
                        ┌──────────────────────────────────────────────┐
                        │                  Net (libNet.a)              │
                        │               HTTP / TCP 应用层               │
                        │                                              │
   HTTP Request ──────▶ │  HttpServer ─▶ Router ─▶ Handler (co_await)  │
                        │     │              │            │            │
                        │     ▼              │            ▼            │
   HTTP Response ◀───── │  HttpSession       │      HttpResponse       │
                        │     │              │                         │
                        │     ▼              │                         │
                        │  HttpParser        │                         │
                        │  (llhttp)          │                         │
                        └──────┬─────────────┴─────────────────────────┘
                               │  ┌──────────────────────┐
                               │  │ TcpServer  (×N 线程)  │
                               │  │ TcpAcceptor          │
                               │  │ TcpStream            │
                               │  │ StreamBuffer         │
                               │  │ MiddlewarePipeline   │
                               │  └──────────────────────┘
                               │
                        ┌──────▼──────────────────────────────────────┐
                        │                Core (libCore.a)             │
                        │               异步运行时 + 协程调度            │
                        │                                             │
                        │  ┌──────────────┐   ┌──────────────────┐    │
                        │  │  IoContext   │   │  CoroutinePool   │    │
                        │  │  (阻塞 run)   │   │  (帧内存池)       │    │
                        │  └──────┬───────┘   └──────────────────┘    │
                        │         │                                   │
                        │  ┌──────▼──────────────────────────────┐    │
                        │  │          ThreadPool                 │    │
                        │  │  std::jthread × N                   │    │
                        │  │  ┌──────────┐  ┌──────────┐         │    │
                        │  │  │ EventLoop│  │ EventLoop│  ...    │    │
                        │  │  │ ┌──────┐ │  │ ┌──────┐ │         │    │
                        │  │  │ │Epoll │ │  │ │Epoll │ │         │    │
                        │  │  │ └──────┘ │  │ └──────┘ │         │    │
                        │  │  └────┬─────┘  └────┬─────┘         │    │
                        │  └───────┼──────────────┼──────────────┘    │
                        │          │              │                   │
                        │  ┌───────▼──────────────▼──────────────┐    │
                        │  │            Scheduler                │    │
                        │  │   本地队列 (lock-free) │ 全局队列      │    │
                        │  │   工作窃取 ◀──▶ 跨线程调度             │    │
                        │  └──────────────────┬──────────────────┘    │
                        │                     │                       │
                        │          ┌──────────▼──────────┐            │
                        │          │ co_await 原语       │            │
                        │          │ EpollAwaiter        │           │
                        │          │ Task<T> (协程类型)   │            │
                        │          └─────────────────────┘            │
                        │                                             │
                        │  ┌───────────┐ ┌───────────┐ ┌───────────┐  │
                        │  │AsyncSocket│ │InetAddress│ │  Timer    │  │
                        │  └───────────┘ └───────────┘ └───────────┘  │
                        │  ┌──────────┐ ┌──────────────────────────┐  │
                        │  │BufferPool│ │Connection │ ConnectionMgr│  │
                        │  └──────────┘ └──────────────────────────┘  │
                        └──────┬──────────────────────────────────────┘
                               │
                        ┌──────▼──────────────────────────────────────┐
                        │                Base (libBase.a)             │
                        │                基础设施                      │
                        │                                             │
                        │  ┌────────────────┐  ┌───────────────────┐  │
                        │  │  Logger        │  │  Config           │  │
                        │  │  LoggerRegistry│  │  ConfigManager    │  │
                        │  │  LogSink×4     │  │  ConfigFileWatcher│  │
                        │  │  LogCommon     │  │  ConfigValue      │  │
                        │  └────────────────┘  └───────────────────┘  │
                        │  ┌──────────────────────────────────────┐   │
                        │  │              Exception               │   │
                        │  │  Exception / SystemException /       │   │
                        │  │  NetworkException / ConfigException  │   │
                        │  └──────────────────────────────────────┘   │
                        └─────────────────────────────────────────────┘
```

### 多线程模型

```
IoContext (主线程，阻塞 run)
  │
  └── ThreadPool
        │
        ├── std::jthread #0 ── EventLoop[0] ── Epoll[0] + Scheduler[0]
        │     └── HttpServer[0] ── TcpAcceptor[0] (SO_REUSEPORT)
        │
        ├── std::jthread #1 ── EventLoop[1] ── Epoll[1] + Scheduler[1]
        │     └── HttpServer[1] ── TcpAcceptor[1] (SO_REUSEPORT)
        │
        └── ... (N 个线程)
        
  内核 SO_REUSEPORT 将 TCP 连接均匀分发到各 TcpAcceptor
```

### 请求处理流程

```
Client ──TCP──▶ SO_REUSEPORT ──内核分发──▶ TcpAcceptor[N]
                                                │ (accept4 + EpollAwaiter)
                                                ▼
                                           HttpSession
                                                │ (recv + EpollAwaiter)
                                                ▼
                                           HttpParser ──llhttp──▶ HttpRequest
                                                                     │
                                                Router ──────────────▶ Handler
                                                │    │                    │
                                                │    ▼                    ▼
                                                │  MiddlewarePipeline  业务逻辑
                                                │    │                    │
                                                │    └────────────────────┘
                                                ▼
                                           HttpResponse ──toString()──▶ (send) ──▶ Client
```

### I/O 模型（纯 epoll 边缘触发）

```
co_await asyncRecv(buf, len)
  → recv(fd, buf, len, MSG_NOSIGNAL)
     → 成功: co_return n
     → EAGAIN: co_await EpollAwaiter(epoll, fd, EPOLLIN | EPOLLET)
        → epoll_ctl(ADD, fd, EPOLLIN | EPOLLET, handle)
        → 协程挂起                                // Scheduler 运行其他协程

  → fd 可读 → epoll_wait 返回
     → scheduler.schedule(handle)
     → 协程恢复 → await_resume() → epoll_ctl(DEL)
     → 重试 recv()
```

### 依赖关系

```
Net  ──▶ Core ──▶ Base
 │        │        │
 │        ├────────┼── OpenSSL (TLS)
 │        │        │
 └── llhttp       └── yaml-cpp
```

- **Base** 依赖 `yaml-cpp`，提供日志、配置、异常等基础设施
- **Core** 依赖 `Base` + `OpenSSL`，构建协程运行时 + epoll 事件循环 + TLS 安全层
- **Net** 依赖 `Core` + `llhttp`，提供 HTTP/HTTPS 服务端能力

## 快速开始

### 前置依赖

- **CMake** ≥ 3.20
- **Conan** ≥ 2.0
- **GCC** ≥ 13 或 **Clang** ≥ 17（需支持 C++20 协程）
- **Linux** ≥ 2.6.32（epoll 支持）

### 构建

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install conan
cmake --preset debug
cmake --build build/debug -j$(nproc)
cd build/debug && ctest --output-on-failure
```

### Release 构建

```bash
cmake --preset release
cmake --build build/release -j$(nproc)
```

### 运行

```bash
# HTTP 模式（默认）
./build/release/samples/echo_server --port 8080

# HTTPS 模式
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj "/CN=localhost"
./build/release/samples/echo_server --https --cert cert.pem --key key.pem

# 指定线程数
./build/release/samples/echo_server --port 8080 --threads 4
```

### 压测

```bash
# HTTP
ab -n 1000000 -c 200 http://localhost:8080/bench

# HTTPS
ab -n 500000 -c 200 https://localhost:8080/bench
```

## 代码示例

### 多线程 HTTP 服务器

```cpp
#include "Core/IoContext.h"
#include "Core/InetAddress.h"
#include "Core/ThreadPool.h"
#include "Net/HttpServer.h"
#include "Net/Router.h"

int main() {
    Core::IoContext ctx(4);  // 4 个工作线程
    auto addr = Core::InetAddress::any(8080);
    auto &pool = ctx.threadPool();

    // 每线程一个 HttpServer（SO_REUSEPORT 负载均衡）
    std::vector<std::unique_ptr<Net::HttpServer>> servers;
    for (unsigned i = 0; i < pool.threadCount(); ++i) {
        auto &loop = pool.eventLoop(i);
        auto server = std::make_unique<Net::HttpServer>(loop, addr);

        server->router().get("/", [](auto& req, auto& res) -> Core::Task<void> {
            res.setStatus(200);
            res.setHeader("Content-Type", "text/plain");
            res.setBody("Hello, AsynGyanis!");
            co_return;
        });

        auto task = server->start();
        loop.scheduler().schedule(task.handle());
        servers.push_back(std::move(server));
    }

    ctx.run();  // 阻塞直到 SIGINT → stop()
}
```

### 单线程最小服务器（调试用）

```cpp
#include "Core/EventLoop.h"
#include "Core/InetAddress.h"
#include "Net/HttpServer.h"

int main() {
    Core::EventLoop loop;
    Net::HttpServer server(loop, Core::InetAddress::any(8080));

    server.router().get("/", [](auto& req, auto& res) -> Core::Task<void> {
        res.setBody("OK");
        co_return;
    });

    auto task = server.start();
    loop.scheduler().schedule(task.handle());
    loop.run();
}
```

### 多线程 HTTPS 服务器

```cpp
#include "Core/IoContext.h"
#include "Core/InetAddress.h"
#include "Core/ThreadPool.h"
#include "Net/HttpsServer.h"
#include "Net/Router.h"

int main() {
    Core::IoContext ctx(4);
    auto addr = Core::InetAddress::any(443);
    auto &pool = ctx.threadPool();

    std::vector<std::unique_ptr<Net::TcpServer>> servers;
    for (unsigned i = 0; i < pool.threadCount(); ++i) {
        auto &loop = pool.eventLoop(i);
        auto server = std::make_unique<Net::HttpsServer>(
            loop, addr, "cert.pem", "key.pem");

        server->router().get("/", [](auto& req, auto& res) -> Core::Task<void> {
            res.setStatus(200);
            res.setHeader("Content-Type", "text/plain");
            res.setBody("Hello HTTPS!");
            co_return;
        });

        auto task = server->start();
        loop.scheduler().schedule(task.handle());
        servers.push_back(std::move(server));
    }

    ctx.run();
}
```

### 异步 TCP 客户端

```cpp
Core::Task<void> ping(Core::EventLoop& loop) {
    auto sock = Core::AsyncSocket::create(loop);
    co_await sock.asyncConnect(*Core::InetAddress::resolve("localhost", 8080));

    const char* msg = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    co_await sock.asyncSend(msg, strlen(msg));

    char buf[4096];
    ssize_t n = co_await sock.asyncRecv(buf, sizeof(buf));
    printf("%.*s\n", (int)n, buf);
}
```

## 模块概览

### Base — 基础设施（`libBase.a`，依赖 `yaml-cpp`）

| 分类 | 类 | 职责 |
|------|----|------|
| **日志** | `Logger` / `LoggerRegistry` | 命名日志器 + 全局注册表（Meyers 单例） |
| | `LogSink` → `ConsoleSink`, `FileSink`, `RollingFileSink`, `AsyncSink` | 四种输出目标 |
| | `LogFormatter` → `DefaultFormatter`, `ColorFormatter` | 格式化器 |
| | `LoggerConfigLoader` | 从 ConfigManager 读 YAML 初始化日志系统 |
| | `LogCommon` | 日志等级、时间戳、LogEvent、ANSI 颜色 |
| **配置** | `Config` (→ `ConfigManager`) | YAML/JSON 加载，热更新 |
| | `ConfigValue` | `std::variant` 类型安全容器（7 种类型） |
| | `ConfigFileWatcher` | 跨平台文件监听（inotify / Win32 / 轮询） |
| | `ConfigType` | 类型枚举、`splitKey`、`isYamlFile` |
| **异常** | `Exception` ← `SystemException` ← `NetworkException` | 携带 `source_location` + `error_code` |
| | `ConfigException` 及其 5 种派生类 | 配置错误层次 |

### Core — 异步运行时（`libCore.a`，依赖 `Base`）

| 分类 | 类 | 职责 |
|------|----|------|
| **入口** | `IoContext` | 运行时主入口，持有 `ThreadPool`，`run()` 阻塞直到 `stop()`，析构自动停止 |
| **线程** | `EventLoop` | 每线程事件循环 — epoll 统一等待 wakeup eventfd / EpollAwaiter，调度协程 |
| | `ThreadPool` | `std::jthread` 线程池，每线程绑定一个 `EventLoop` |
| **系统** | `Epoll` | epoll RAII（`addFd` / `modFd` / `delFd` / `wait`） |
| **地址** | `InetAddress` | IPv4/IPv6 统一封装（`sockaddr_storage`），DNS 解析、`any()`/`localhost()` 工厂 |
| **协程** | `Task<T>` | 协程返回类型（惰性启动，`FinalAwaiter` 推入调度器） |
| | `Scheduler` | 本地无锁队列 + 全局队列，工作窃取 |
| | `CoroutinePool` | thread-local 协程帧 slab 分配器 |
| **Awaiter** | `EpollAwaiter` | `co_await` epoll 边缘触发事件（fd 就绪→恢复协程） |
| | `Timer` | 可 await 定时器（`timerfd_create` + epoll） |
| **TLS** | `TlsContext` | SSL_CTX RAII 管理（证书加载、SSL 对象创建） |
| | `TlsSocket` | TLS 套接字（SSL_read/SSL_write + epoll 非阻塞集成） |
| **I/O** | `AsyncSocket` | 异步 TCP socket（非阻塞 syscall + epoll EPOLLET） |
| **资源** | `BufferPool` | 固定大小缓冲池 |
| | `Cancelable` | `std::stop_source` / `std::stop_token` 协作取消 |
| | `Connection` / `ConnectionManager` | 连接基类 / 全局连接追踪、优雅关闭 |

### Net — 网络应用层（`libNet.a`，依赖 `Core` + `llhttp`）

| 分类 | 类 | 职责 |
|------|----|------|
| **TCP** | `TcpAcceptor` | 监听 socket（`SO_REUSEPORT`） |
| | `TcpStream` | 缓冲流（`readExact` / `readUntil` / `writeAll`） |
| | `TcpServer` | TCP 服务基类，并发连接处理 |
| **HTTP** | `HttpRequest` / `HttpResponse` | 请求/响应对象 |
| | `HttpParser` | llhttp 增量解析器 |
| **路由** | `Router` | 精确/参数化（`:id`）/通配符（`*`）匹配 |
| | `MiddlewarePipeline` | 洋葱模型中间件 |
| **服务** | `HttpSession` | 每连接 HTTP 事务循环（Keep-Alive） |
| | `HttpServer` | 完整 HTTP 服务器 |
| | `HttpsSession` | 每连接 HTTPS 事务（TLS 握手 + HTTP over TLS） |
| | `HttpsServer` | 完整 HTTPS 服务器（持有 TlsContext） |
| **辅助** | `StreamBuffer` | 环形缓冲区（零拷贝解析） |

## 目录结构

```
AsynGyanis/
├── src/
│   ├── Base/        # 基础设施（18 文件）
│   ├── Core/        # 异步运行时（30 文件）
│   └── Net/         # 网络应用层（24 文件）
├── tests/Base/      # Catch2 单元测试（8 套）
├── docs/Base/       # 中文 API 文档（11 份）
├── samples/         # 示例 + 压测脚本
├── CMakeLists.txt
├── CMakePresets.json
├── conanfile.py
└── conandata.yml
```

## 外部依赖

| 库 | 版本 | 用途 |
|----|------|------|
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.9.0 | YAML 配置解析 |
| [llhttp](https://github.com/nodejs/llhttp) | 9.3.0 | HTTP/1.1 解析 |
| [OpenSSL](https://www.openssl.org/) | 3.6.2 | TLS/HTTPS 安全传输 |
| [Catch2](https://github.com/catchorg/Catch2) | 3.8.0 | 单元测试 |

## 编码规范

| 规则 | 示例 |
|------|------|
| 类名：大驼峰 | `class AsyncSocket` |
| 函数名：小驼峰 | `void asyncRecv()` |
| 成员变量：`m_` 前缀 | `int m_fd` |
| 命名空间 | `Base`, `Core`, `Net` |

## 版权

Copyright (c) 2026 — MIT License
