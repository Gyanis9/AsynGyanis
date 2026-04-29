# EventLoop 模块接口文档

## 概述

`EventLoop` 是每线程事件循环的核心组件。每个工作线程运行一个 `EventLoop` 实例，驱动 epoll 等待和协程调度。事件循环的主流程为：运行所有就绪协程 → epoll_wait 等待 I/O 事件 → 恢复就绪的协程 → 再次运行所有就绪协程 → 循环。

## 核心类

### EventLoop

**头文件**：`EventLoop.h` | **实现**：`EventLoop.cpp`

**内部状态**：
| 成员 | 类型 | 说明 |
|------|------|------|
| `m_epoll` | `Epoll` | epoll 实例 |
| `m_scheduler` | `Scheduler` | 协程调度器 |
| `m_wakeupFd` | `int` | eventfd，用于跨线程唤醒 epoll_wait |
| `m_running` | `atomic<bool>` | 运行状态标志 |
| `m_stopRequested` | `atomic<bool>` | 停止请求标志 |

### 构造函数

```cpp
EventLoop();
```

**功能**：创建 epoll 实例和 eventfd 唤醒描述符。

### 生命周期方法

#### run

```cpp
void run();
```

**功能**：启动事件循环（阻塞调用线程）。

**主循环**：
```
while (!m_stopRequested) {
    m_scheduler.runAll();           // 运行所有就绪协程
    for (auto& ev : m_epoll.wait(1)) {
        if (ev 是唤醒事件): drain eventfd
        if (ev 是 EpollAwaiter 事件): 调度对应协程
    }
    m_scheduler.runAll();           // 恢复被唤醒的协程
}
```

**说明**：`epoll.wait(1)` 使用 1ms 超时，既保持响应延迟又避免忙等待。

#### stop

```cpp
void stop();
```

**功能**：设置 `m_stopRequested = true` 并通过 eventfd 唤醒 `epoll_wait`。线程安全，可跨线程调用。

#### wake

```cpp
void wake() const;
```

**功能**：向 `m_wakeupFd` 写入数据，唤醒被阻塞的 `epoll_wait`。

### 访问器

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `epoll()` | `Epoll&` | 获取 epoll 实例 |
| `scheduler()` | `Scheduler&` | 获取协程调度器 |
| `isRunning()` | `bool` | 查询运行状态 |

## 使用示例

```cpp
#include "Core/EventLoop.h"
#include "Core/Task.h"

Core::EventLoop loop;

// 调度一个协程
auto task = []() -> Core::Task<void> {
    co_return;
}();
loop.scheduler().schedule(task.handle());

// 在工作线程中启动事件循环
std::thread worker([&]() { loop.run(); });

// 从主线程停止
loop.stop();
worker.join();
```

## 性能特征

- `epoll.wait(1)` 使用 1ms 超时，空载 CPU 唤醒约 1000 次/秒
- eventfd 唤醒延迟 < 10μs
- 所有协程调度在同一线程内完成，无锁竞争

## 依赖

- `Epoll` — epoll 实例
- `Scheduler` — 协程调度器
- `Base/Exception.h` — 异常类
