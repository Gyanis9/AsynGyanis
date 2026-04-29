# Task 模块接口文档

## 概述

`Task<T>` 是框架的协程返回类型。使用惰性启动（`initial_suspend` 返回 `suspend_always`），协程创建后不会立即执行，需通过 `Scheduler::schedule()` 或 `co_await` 显式恢复。完成时通过 `FinalAwaiter` 自动将调用方协程推回调度器。

## 核心类

### Task\<T\>

**头文件**：`Task.h`（Header-only 模板）

### Task\<void\>

`Task<T>` 的 `void` 特化版本。使用 `return_void()` 代替 `return_value()`，无返回值存储。

### Promise 类型

| 成员 | 说明 |
|------|------|
| `get_return_object()` | 从 `coroutine_handle` 构造 `Task` |
| `initial_suspend()` | 返回 `suspend_always`（惰性启动） |
| `final_suspend()` | 返回 `FinalAwaiter`（恢复 continuation） |
| `unhandled_exception()` | 捕获异常到 `m_exception` |
| `return_value(U&&)` / `return_void()` | 存储返回值 |
| `result()` | 返回存储值或重新抛出异常 |

### FinalAwaiter

协程结束时的 awaitable。`await_suspend` 返回 `promise.m_continuation`（如果有），使调用方协程恢复执行。

### Task 作为 Awaitable

`Task` 本身也是 Awaitable，支持嵌套协程：

```cpp
co_await someSubTask();  // someSubTask 返回 Task<T>
```

**Awaitable 方法**：

| 方法 | 说明 |
|------|------|
| `await_ready()` | 检查协程是否已完成（`handle.done()`） |
| `await_suspend(continuation)` | 将自身设为子协程的 continuation，返回子协程句柄 |
| `await_resume()` | 调用 `promise.result()` 获取结果 |

### 查询方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `isReady()` | `bool` | 协程是否已完成 |
| `handle()` | `Handle` | 底层 `coroutine_handle`，用于调度 |

### 生命周期

- 移动语义（不可复制）
- 析构时调用 `m_handle.destroy()` 释放协程帧
- **重要**：必须在协程完成或不再需要时销毁 Task，否则协程帧泄漏

## 使用示例

```cpp
#include "Core/Task.h"

Core::Task<int> compute() {
    co_return 42;
}

Core::Task<void> process(Core::Scheduler& scheduler) {
    auto task = compute();
    int result = co_await task;  // 挂起当前协程，等待 compute 完成
    // result == 42
}
```

## 依赖

- C++20 标准库（`<coroutine>`、`<exception>`、`<optional>`）
