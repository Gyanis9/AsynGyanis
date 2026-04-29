# Core 模块 API 文档

Core 是 AsynGyanis 框架的异步运行时核心, 提供协程调度、epoll 事件循环、异步 I/O、TLS 加密、连接管理等基础设施。

## 模块索引

| 模块 | 文件 | 说明 |
|------|------|------|
| [Epoll](Epoll-API.md) | `Epoll.h` | Linux epoll 实例的 RAII 封装 |
| [EpollAwaiter](EpollAwaiter-API.md) | `EpollAwaiter.h` | 协程可等待对象 — 将 fd 注册到 epoll 并挂起直到就绪 |
| [AsyncSocket](AsyncSocket-API.md) | `AsyncSocket.h` | 异步非阻塞 TCP socket |
| [InetAddress](InetAddress-API.md) | `InetAddress.h` | IPv4/IPv6 网络地址封装 |
| [EventLoop](EventLoop-API.md) | `EventLoop.h` | 每线程事件循环 |
| [Scheduler](Scheduler-API.md) | `Scheduler.h` | 协程调度器 — 本地队列 + 全局队列 + 工作窃取 |
| [Task](Task-API.md) | `Task.h` | 协程返回类型 `Task<T>` |
| [Timer](Timer-API.md) | `Timer.h` | 基于 `timerfd` 的可等待定时器 |
| [ThreadPool](ThreadPool-API.md) | `ThreadPool.h` | 固定大小线程池 |
| [IoContext](IoContext-API.md) | `IoContext.h` | 异步运行时主入口 |
| [TlsContext](TlsContext-API.md) | `TlsContext.h` | SSL_CTX RAII 管理 |
| [TlsSocket](TlsSocket-API.md) | `TlsSocket.h` | TLS 加密套接字 |
| [Connection](Connection-API.md) | `Connection.h` | TCP 连接基类 |
| [ConnectionManager](ConnectionManager-API.md) | `ConnectionManager.h` | 全局连接追踪器 |
| [BufferPool](BufferPool-API.md) | `BufferPool.h` | 固定大小缓冲池 |
| [CoroutinePool](CoroutinePool-API.md) | `CoroutinePool.h` | 协程帧内存池 |
| [Cancelable](Cancelable-API.md) | `Cancelable.h` | 协作取消混入类 |

## 架构概览

```
IoContext (运行时入口)
  └── ThreadPool (线程池)
       └── EventLoop × N (每线程事件循环)
            ├── Epoll (事件多路复用)
            ├── Scheduler (协程调度)
            └── AsyncSocket / TlsSocket (异步 I/O)
```

## 命名空间

所有 Core 模块均位于 `Core` 命名空间下。

## 线程模型

- 每个 `EventLoop` 绑定到一个 `std::jthread`
- `Scheduler` 的本地队列仅由所属 `EventLoop` 线程访问（无锁）
- 全局队列用于跨线程协程调度，由 `std::mutex` 保护
- `ConnectionManager` 使用 `std::shared_mutex` 实现读写锁
