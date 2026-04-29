# AsyncSocket 模块接口文档

## 概述

`AsyncSocket` 是异步非阻塞 TCP socket 的核心封装。它将传统的 socket 操作（create/bind/listen/accept/connect/recv/send）包装为 C++20 协程风格的 `Task<T>` 返回类型，内部通过 `EpollAwaiter` 处理 EAGAIN/EWOULDBLOCK 等待，实现真正的异步 I/O。

## 核心类

### AsyncSocket

**头文件**：`AsyncSocket.h` | **实现**：`AsyncSocket.cpp`

**内部状态**：
| 成员 | 类型 | 说明 |
|------|------|------|
| `m_loop` | `EventLoop&` | 关联的事件循环 |
| `m_fd` | `int` | socket 文件描述符，-1 表示已关闭 |

### 构造与析构

```cpp
AsyncSocket(EventLoop& loop, int fd);
~AsyncSocket();  // 自动调用 close()
```

**参数**：`fd` 为 -1 表示"空" socket（用于占位，如 `HttpsSession` 的基类初始化）。

**析构**：自动调用 `close()`，包括 `shutdown(SHUT_RDWR)` + `::close(fd)`。

### 移动语义

```cpp
AsyncSocket(AsyncSocket&& other) noexcept;
AsyncSocket& operator=(AsyncSocket&& other) noexcept;
```

**功能**：转移所有权，源对象的 `m_fd` 被置为 -1。

### 工厂方法

```cpp
static AsyncSocket create(EventLoop& loop, int domain = AF_INET, int type = SOCK_STREAM);
```

**功能**：创建新 socket。自动设置 `SOCK_NONBLOCK | SOCK_CLOEXEC` 标志。

**异常**：`::socket()` 失败时抛出 `SystemException`。

### 绑定与监听

```cpp
bool bind(const sockaddr* addr, socklen_t addrLen) const;
bool bind(const InetAddress& addr) const;
bool listen(int backlog = SOMAXCONN) const;
```

**功能**：标准 socket bind/listen 操作。`bind` 内部自动设置 `SO_REUSEADDR`。

### 异步 I/O 方法

#### asyncAccept

```cpp
Task<AsyncSocket> asyncAccept() const;
```

**功能**：协程式接受新连接。

**内部循环**：
1. `accept4(m_fd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)` 尝试接受
2. 成功 → `co_return AsyncSocket(m_loop, fd)`
3. `EAGAIN/EWOULDBLOCK` → `co_await EpollAwaiter(epoll, fd, EPOLLIN)` → 重试
4. `EINTR/ECONNABORTED` → 立即重试
5. `EMFILE/ENFILE/ENOBUFS/ENOMEM` → EpollAwaiter 等待后重试
6. 其他错误 → 抛出 `SystemException`

#### asyncConnect

```cpp
Task<> asyncConnect(const sockaddr* addr, socklen_t addrLen) const;
Task<> asyncConnect(const InetAddress& addr) const;
```

**功能**：协程式非阻塞连接。

**流程**：
1. `connect(m_fd, addr, addrLen)` → 成功则 `co_return`
2. `EINPROGRESS` → `co_await EpollAwaiter(epoll, fd, EPOLLOUT)` → `getsockopt(SO_ERROR)` 检查
3. 其他错误 → 抛出 `SystemException`

#### asyncRecv

```cpp
Task<ssize_t> asyncRecv(void* buf, size_t len);
```

**功能**：协程式接收数据。

**内部循环**：
1. `recv(m_fd, buf, len, MSG_NOSIGNAL)` 
2. `n >= 0` → `co_return n`（0 表示对端关闭）
3. `EAGAIN` → `co_await EpollAwaiter(epoll, fd, EPOLLIN)` → 重试

#### asyncSend

```cpp
Task<ssize_t> asyncSend(const void* buf, size_t len);
```

**功能**：协程式发送数据。

**内部循环**：
1. `send(m_fd, buf, len, MSG_NOSIGNAL)`
2. `n >= 0` → `co_return n`
3. `EAGAIN` → `co_await EpollAwaiter(epoll, fd, EPOLLOUT)` → 重试

### 控制方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `close()` | `void` | 关闭 socket（shutdown + close），幂等 |
| `fd()` | `int` | 获取原始文件描述符 |
| `setNonBlocking()` | `void` | 设置 O_NONBLOCK 标志 |
| `setSockOpt(...)` | `bool` | 设置 socket 选项 |
| `remoteAddress()` | `InetAddress` | 获取对端地址 |
| `localAddress()` | `InetAddress` | 获取本地地址 |

## 使用示例

```cpp
#include "Core/AsyncSocket.h"
#include "Core/EventLoop.h"

// 服务端
Core::Task<void> server(Core::EventLoop& loop) {
    auto listenSock = Core::AsyncSocket::create(loop);
    listenSock.bind(Core::InetAddress::any(8080));
    listenSock.listen();

    auto clientSock = co_await listenSock.asyncAccept();
    char buf[4096];
    ssize_t n = co_await clientSock.asyncRecv(buf, sizeof(buf));
    co_await clientSock.asyncSend("OK", 2);
    clientSock.close();
}

// 客户端
Core::Task<void> client(Core::EventLoop& loop) {
    auto sock = Core::AsyncSocket::create(loop);
    co_await sock.asyncConnect(*Core::InetAddress::resolve("localhost", 8080));
    co_await sock.asyncSend("GET / HTTP/1.1\r\n\r\n", 18);
    char buf[4096];
    ssize_t n = co_await sock.asyncRecv(buf, sizeof(buf));
}
```

## 性能特征

- 每次 I/O 成功时开销：一次系统调用（`recv`/`send`/`accept4`）
- EAGAIN 时额外开销：一次 `epoll_ctl(ADD)` + 一次 `epoll_ctl(DEL)`
- `close()` 包含 `shutdown(SHUT_RDWR)` 避免 RST

## 依赖

- `EventLoop` — 提供 `Epoll` 引用
- `EpollAwaiter` — EAGAIN 等待机制
- `InetAddress` — 地址封装
- `Task<T>` — 协程返回类型
- `Base/Exception.h` — 异常类
