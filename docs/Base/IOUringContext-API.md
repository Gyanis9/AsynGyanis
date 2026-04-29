# IOUringContext 模块接口文档

## 概述

`IOUringContext` 是对 Linux `io_uring` 子系统的 RAII 封装类。负责管理 io_uring 实例的完整生命周期（初始化与销毁），提供安全、零所有权的 SQE（提交队列条目）和 CQE（完成队列事件）指针访问接口。

## 核心概念

io_uring 是 Linux 内核提供的高性能异步 I/O 接口，使用共享内存环形缓冲区在用户态和内核态之间传递 I/O 请求和完成事件，避免传统 `epoll`/`select` 的系统调用开销。

## API 参考

### 构造函数

```cpp
explicit IOUringContext(unsigned entries, unsigned flags = 0);
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `entries` | `unsigned` | SQ 条目数。必须是 2 的幂（如 2, 4, 8, ..., 4096） |
| `flags` | `unsigned` | io_uring 初始化标志位。常用值：`0` (默认)、`IORING_SETUP_SQPOLL` (内核轮询)、`IORING_SETUP_SINGLE_ISSUER` (单提交者优化) |

**异常**：`io_uring_queue_init` 失败时抛出 `std::runtime_error`。

**示例**：
```cpp
Base::IOUringContext ctx(256);                         // 默认配置
Base::IOUringContext ctx2(512, IORING_SETUP_SQPOLL);   // 内核轮询模式
```

### 析构函数

```cpp
~IOUringContext();
```

自动调用 `io_uring_queue_exit()` 释放内核资源。

### 移动语义

```cpp
IOUringContext(IOUringContext&& other) noexcept;
IOUringContext& operator=(IOUringContext&& other) noexcept;
```

**功能说明**：
- 移动构造：接管源对象的 ring 所有权，源对象置为非初始化状态
- 移动赋值：先释放当前 ring 资源（如已初始化），再接管源 ring
- 支持自移动赋值（安全处理）
- 拷贝操作已删除（`= delete`）

### SQE 获取

```cpp
[[nodiscard]] io_uring_sqe* getSqe() noexcept;
```

**功能**：从提交队列（SQ）获取下一个可用的 SQE 指针。

**返回值**：
| 返回值 | 说明 |
|--------|------|
| 非空指针 | 可用 SQE，调用者可填充后提交 |
| `nullptr` | SQ 已满，无可用条目 |

**使用模式**：
```cpp
io_uring_sqe* sqe = ctx.getSqe();
if (!sqe) {
    // 队列已满，需要先提交或等待
    return;
}
io_uring_prep_nop(sqe);                                    // 填充操作
io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(id));    // 设置用户数据
```

### 提交操作

#### submit

```cpp
int submit();
```

**功能**：将已填充的 SQE 一次性提交至内核。

**返回值**：已提交的 SQE 数量（>= 0）；失败返回 `-errno`。

#### submitAndWait

```cpp
int submitAndWait(unsigned waitNr = 1);
```

**功能**：提交 SQE 并阻塞等待至少 `waitNr` 个完成事件。

**参数**：`waitNr` — 最少等待完成数，默认 1。

**返回值**：同 `submit()`。

### CQE 获取

#### waitCqe

```cpp
[[nodiscard]] io_uring_cqe* waitCqe();
```

**功能**：阻塞等待一个完成事件。

**返回值**：CQE 指针；失败返回 `nullptr`。

#### peekCqe

```cpp
[[nodiscard]] io_uring_cqe* peekCqe();
```

**功能**：非阻塞方式查看 CQE 队列头。无可用事件时立即返回。

**返回值**：CQE 指针；无可用事件返回 `nullptr`。

#### cqeSeen

```cpp
void cqeSeen(io_uring_cqe* cqe);
```

**功能**：将已处理的 CQE 标记为已消费，推进 CQ 环形缓冲区。

**参数**：`cqe` — 从 `peekCqe`/`waitCqe` 获取的完成事件指针。

### 访问器

#### fd

```cpp
[[nodiscard]] int fd() const noexcept;
```

**功能**：获取 io_uring ring 文件描述符，用于 `epoll`/`poll` 集成。

#### entries

```cpp
[[nodiscard]] unsigned entries() const noexcept;
```

**功能**：获取构造时指定的 SQ 条目容量。

## 完整使用流程

```cpp
#include "Base/IOUringContext.h"
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>

// 1. 创建 io_uring 上下文
Base::IOUringContext ctx(256);

// 2. 获取 SQE 并填充操作
io_uring_sqe* sqe = ctx.getSqe();
io_uring_prep_nop(sqe);
io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(42));

// 3. 提交到内核
ctx.submit();

// 4. 等待完成
io_uring_cqe* cqe = ctx.waitCqe();
if (cqe) {
    auto data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
    int result = cqe->res;
    // 处理结果...

    // 5. 标记为已消费
    ctx.cqeSeen(cqe);
}
```

## 状态管理

| 状态 | 含义 | 有效操作 |
|------|------|---------|
| 已初始化 (`m_initialized=true`) | ring 可用 | `getSqe`, `submit`, `peekCqe`, `waitCqe`, `cqeSeen`, `fd`, `entries` |
| 已移动 | ring 所有权已转移 | 仅析构安全 |
| 已销毁 | ring 已释放 | 无 |

## 性能特征

| 操作 | 时间复杂度 | 系统调用 |
|------|-----------|---------|
| `getSqe()` | O(1) | 无（共享内存访问） |
| `submit()` | O(1) | `io_uring_enter` |
| `submitAndWait()` | O(1) + 等待 | `io_uring_enter`（阻塞） |
| `peekCqe()` | O(1) | 无（共享内存访问） |
| `waitCqe()` | O(1) + 等待 | `io_uring_enter`（阻塞） |
| `cqeSeen()` | O(1) | 无（共享内存更新） |

## 注意事项

1. **必须是 2 的幂**：`entries` 参数必须为 2 的幂（2, 4, 8, ..., 4096），否则初始化失败
2. **SQE 所有权**：`getSqe()` 返回的指针由 io_uring 内部管理，不可 `free`/`delete`
3. **CQE 消费**：每次 `peekCqe()` 或 `waitCqe()` 后必须调用 `cqeSeen()`
4. **移动后状态**：移动后的源对象处于非初始化状态，不可再使用
5. **线程安全**：本类不提供内置的线程安全机制，需外部同步

## 依赖

- `liburing`：Linux io_uring 用户态库
- C++20 标准库
