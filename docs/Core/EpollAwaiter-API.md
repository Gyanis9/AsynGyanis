# EpollAwaiter 模块接口文档

## 概述

`EpollAwaiter` 是 C++20 协程的可等待对象（Awaitable），用于将文件描述符的就绪事件与协程的挂起/恢复机制连接。当协程执行 `co_await EpollAwaiter(epoll, fd, EPOLLIN)` 时，当前协程被挂起，fd 被注册到 epoll（边缘触发模式），直到 fd 变为可读时协程被调度恢复。

## 核心类

### EpollAwaiter

**头文件**：`EpollAwaiter.h`（Header-only）

**内部状态**：
| 成员 | 类型 | 说明 |
|------|------|------|
| `m_epoll` | `Epoll*` | 指向 EventLoop 的 Epoll 实例 |
| `m_fd` | `int` | 等待就绪的文件描述符 |
| `m_eventMask` | `uint32_t` | 监听的事件类型（EPOLLIN / EPOLLOUT） |

### 构造函数

```cpp
EpollAwaiter(Epoll& epoll, int fd, uint32_t eventMask);
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `epoll` | `Epoll&` | EventLoop 的 Epoll 实例 |
| `fd` | `int` | 要等待的文件描述符 |
| `eventMask` | `uint32_t` | 事件类型，典型值 EPOLLIN 或 EPOLLOUT |

### Awaitable 接口

#### await_ready

```cpp
bool await_ready() const noexcept;
```

**功能**：判断协程是否需要挂起。**始终返回 `false`**，即每次 `co_await` 都会挂起协程。这是为了保证每次 I/O 操作后 fd 从 epoll 中正确移除。

#### await_suspend

```cpp
void await_suspend(std::coroutine_handle<> handle) noexcept;
```

**功能**：挂起协程前，将 fd 注册到 epoll 并关联协程句柄地址。

**流程**：
1. 调用 `m_epoll->addFd(m_fd, m_eventMask | EPOLLET, handle.address())`
2. 将 `handle.address()` 作为 `epoll_event.data.ptr` 存储
3. 当 fd 就绪时，EventLoop 通过 `std::coroutine_handle<>::from_address(ev.data.ptr)` 恢复该协程

#### await_resume

```cpp
void await_resume() const;
```

**功能**：协程恢复时，从 epoll 中移除 fd。调用 `m_epoll->delFd(m_fd)`。

## 工作流程

```
co_await EpollAwaiter(epoll, fd, EPOLLIN)
  │
  ├── await_ready() → false (始终挂起)
  │
  ├── await_suspend(handle)
  │     └── epoll_ctl(ADD, fd, EPOLLIN | EPOLLET, handle.address())
  │          协程挂起 ...
  │
  ├── [fd 可读] → epoll_wait 返回
  │     └── EventLoop 读取 ev.data.ptr → schedule(handle)
  │          协程恢复
  │
  └── await_resume()
        └── epoll_ctl(DEL, fd)
```

## 事件掩码

| 掩码 | 说明 | 典型场景 |
|------|------|---------|
| `EPOLLIN` | fd 可读 | asyncAccept / asyncRecv 重试 |
| `EPOLLOUT` | fd 可写 | asyncConnect / asyncSend 重试 |
| `EPOLLET` | 边缘触发 | 内部自动添加，避免重复通知 |

## 使用示例

```cpp
#include "Core/EpollAwaiter.h"

// 在协程中使用
Task<ssize_t> asyncRecvExample(Epoll& epoll, int fd, void* buf, size_t len) {
    while (true) {
        ssize_t n = recv(fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) co_return n;
        if (errno == EAGAIN) {
            co_await EpollAwaiter(epoll, fd, EPOLLIN);  // 挂起等待可读
            continue;  // 重试
        }
        throw SystemException("recv failed");
    }
}
```

## 性能特征

- `await_ready`：内联空函数，0 开销
- `await_suspend`：一次 `epoll_ctl(ADD)` 系统调用，约 200-500ns
- `await_resume`：一次 `epoll_ctl(DEL)` 系统调用，约 200-500ns
- 使用 EPOLLET 边缘触发，避免 level-triggered 的重复通知开销

## 依赖

- `Epoll.h`
- C++20 标准库（`<coroutine>`）
