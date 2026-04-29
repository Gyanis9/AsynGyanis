# Scheduler 模块接口文档

## 概述

`Scheduler` 是协程调度器，每个 `EventLoop` 持有一个实例。提供本地无锁队列（单消费者）用于本线程协程调度，以及全局互斥队列用于跨线程调度。支持工作窃取（Work Stealing），空闲线程可从其他 Scheduler 窃取任务。

## 核心类

### Scheduler

**头文件**：`Scheduler.h` | **实现**：`Scheduler.cpp`

**内部状态**：
| 成员 | 类型 | 说明 |
|------|------|------|
| `m_localQueue` | `std::vector<coroutine_handle<>>` | 本地就绪队列（无锁，仅本线程访问） |
| `m_globalQueue` | `std::deque<coroutine_handle<>>` | 全局队列（mutex 保护，跨线程） |
| `m_globalMutex` | `std::mutex` | 全局队列互斥锁 |

### 调度方法

#### schedule

```cpp
void schedule(std::coroutine_handle<> handle);
```

**功能**：将协程加入本地就绪队列。仅由所属 EventLoop 线程调用。无需加锁。

**参数**：`handle` 为 `nullptr` 时静默忽略。

#### scheduleRemote

```cpp
void scheduleRemote(std::coroutine_handle<> handle);
```

**功能**：跨线程调度——将协程推入全局队列。线程安全（内部加锁）。用于其他线程向本 Scheduler 提交任务。

**参数**：`handle` 为 `nullptr` 时静默忽略。

### 执行方法

#### runOne

```cpp
bool runOne();
```

**功能**：执行一个就绪协程。

**优先级**：
1. 优先处理全局队列中的跨线程任务（尝试加锁）
2. 其次处理本地队列中的任务

**返回值**：成功执行一个协程返回 `true`，无任务返回 `false`。

#### runAll

```cpp
void runAll();
```

**功能**：执行所有就绪协程（清空本地队列）。

**内部逻辑**：
```cpp
while (hasWork()) { runOne(); }
```

**注意**：`hasWork()` 仅检查本地队列（不检查全局队列以避免锁竞争）。跨线程任务在每轮 `runOne` 开头被检查。

### 工作窃取

#### stealFrom

```cpp
std::coroutine_handle<> stealFrom(Scheduler& other);
```

**功能**：从另一个 Scheduler 窃取任务。优先窃取全局队列，其次是本地队列的前半部分。

**返回值**：窃取到的协程句柄，无任务时返回 `nullptr`。

### 查询方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `hasWork()` | `bool` | 本地队列是否有待处理协程 |
| `localQueueSize()` | `size_t` | 本地队列当前大小（用于监控） |

## 使用示例

```cpp
#include "Core/Scheduler.h"

Core::Scheduler scheduler;

// 本地调度
scheduler.schedule(myCoroutineHandle);
scheduler.runAll();  // 全部执行

// 跨线程调度
std::thread other([&]() {
    scheduler.scheduleRemote(remoteHandle);
});
scheduler.runOne();  // 处理跨线程任务
```

## 性能特征

- `schedule`：O(1) 追加到 vector 末尾，无锁
- `runOne`：本地队列 O(1) pop，全局队列需加锁约 50-100ns
- 工作窃取：需要加锁，O(n) 查找

## 依赖

- C++20 标准库（`<coroutine>`、`<deque>`、`<mutex>`、`<vector>`）
