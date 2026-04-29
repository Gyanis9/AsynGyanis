# Epoll 模块接口文档

## 概述

`Epoll` 模块提供了 Linux epoll 实例的 RAII 封装。管理 epoll 文件描述符的生命周期，提供添加、修改、删除被监听 fd 的统一接口，以及阻塞等待 IO 事件的能力。析构时自动关闭 epoll 描述符。

## 核心类

### Epoll

**功能**：封装 Linux epoll 系统调用，在 `epoll_create1` / `epoll_ctl` / `epoll_wait` 之上提供类型安全的 C++ 接口。

**内部状态**：
| 成员 | 类型 | 说明 |
|------|------|------|
| `m_fd` | `int` | epoll 文件描述符，初始值 -1 |
| `m_events` | `std::vector<epoll_event>` | epoll_wait 输出缓冲区，默认 1024 容量 |

### 构造函数

```cpp
Epoll();
```

**功能**：调用 `epoll_create1(EPOLL_CLOEXEC)` 创建 epoll 实例。

**异常**：创建失败时抛出 `std::runtime_error`。

### 析构函数

```cpp
~Epoll();
```

**功能**：`close(m_fd)` 关闭 epoll 描述符。

### 移动语义

```cpp
Epoll(Epoll&& other) noexcept;
Epoll& operator=(Epoll&& other) noexcept;
```

**功能**：转移所有权。移动后源对象的 `m_fd` 被置为 -1，重复析构是安全的。

### addFd

```cpp
bool addFd(int fd, uint32_t events, void* userData = nullptr) const;
```

**功能**：向 epoll 实例注册一个文件描述符。

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 要监听的文件描述符 |
| `events` | `uint32_t` | 事件掩码（EPOLLIN / EPOLLOUT 等） |
| `userData` | `void*` | 关联用户数据（通常为协程句柄地址 `handle.address()`） |

**返回值**：调用 `epoll_ctl(EPOLL_CTL_ADD)` 成功时返回 true。

**注意**：如果 fd 已经在 epoll 中注册，`EPOLL_CTL_ADD` 会失败（`EEXIST`），此时应使用 `modFd`。

### modFd

```cpp
bool modFd(int fd, uint32_t events, void* userData = nullptr) const;
```

**功能**：修改已注册 fd 的监听事件掩码或关联数据。

**返回值**：调用 `epoll_ctl(EPOLL_CTL_MOD)` 成功时返回 true。

### delFd

```cpp
bool delFd(int fd) const;
```

**功能**：从 epoll 实例中移除 fd。

**返回值**：调用 `epoll_ctl(EPOLL_CTL_DEL)` 成功时返回 true。

**注意**：内核会自动从 epoll 中移除已关闭的 fd，显式调用 `delFd` 是最佳实践。

### wait

```cpp
[[nodiscard]] std::span<epoll_event> wait(int timeoutMs = -1);
```

**功能**：阻塞等待 IO 事件。

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `timeoutMs` | `int` | 超时毫秒数，-1 表示无限等待，0 表示立即返回 |

**返回值**：就绪事件的 `std::span`，长度为 0 表示超时或无事件。EINTR 时返回空 span。

### fd

```cpp
[[nodiscard]] int fd() const noexcept;
```

**功能**：获取底层 epoll 描述符。

## 使用示例

```cpp
#include "Core/Epoll.h"

Core::Epoll epoll;

// 添加监听 fd
int fd = /* ... */;
int sentinel = 0;
epoll.addFd(fd, EPOLLIN, &sentinel);

// 阻塞等待
for (auto events = epoll.wait(5000); auto& ev : events) {
    if (ev.data.ptr == &sentinel) {
        // fd 可读
    }
}

// 修改监听
epoll.modFd(fd, EPOLLIN | EPOLLOUT, &sentinel);

// 移除
epoll.delFd(fd);
```

## 性能特征

- `addFd` / `modFd` / `delFd`：单次 `epoll_ctl` 系统调用，约 200-500ns
- `wait`：阻塞等待，延迟取决于事件到达时间
- 默认最大事件数 1024，单次 `wait` 可返回多个事件，批量处理效率高
- 当前仅支持 level-triggered 模式（EPOLLET 由 `EpollAwaiter` 动态添加）

## 依赖

- C++20 标准库（`<sys/epoll.h>`、`<span>`、`<vector>`）
- 无项目内部依赖
