# IoContext 模块接口文档

## 概述

`IoContext` 是异步运行时的主入口。组合 `ThreadPool`，提供 `run()`/`stop()` 生命周期管理。`run()` 阻塞调用线程（通常是 main 线程）直到 `stop()` 被调用。

## 核心类

### IoContext

**头文件**：`IoContext.h` | **实现**：`IoContext.cpp`

### 构造函数

```cpp
explicit IoContext(size_t threadCount = 0);
```

### 生命周期

| 方法 | 说明 |
|------|------|
| `run()` | 调用 `ThreadPool::start()`，然后阻塞在条件变量上等待 `stop()` |
| `stop()` | 通知条件变量，调用 `ThreadPool::stop()` |

### 访问器

| 方法 | 说明 |
|------|------|
| `threadPool()` | 获取 ThreadPool 引用 |
| `mainScheduler()` | 获取线程 0 的 Scheduler（用于主线程调度任务） |

### 典型用法

```cpp
Core::IoContext ctx(4);  // 4 个工作线程
// ... 创建 HttpServer、设置路由、调度接收任务 ...
ctx.run();  // 阻塞，直到 SIGINT 触发 ctx.stop()
```

## 依赖

- `ThreadPool`
