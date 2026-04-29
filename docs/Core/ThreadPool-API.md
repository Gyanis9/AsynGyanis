# ThreadPool 模块接口文档

## 概述

`ThreadPool` 是固定大小的线程池，每个工作线程绑定一个 `EventLoop`。使用 `std::jthread` 实现自动 join 和协作取消。

## 核心类

### ThreadPool

**头文件**：`ThreadPool.h` | **实现**：`ThreadPool.cpp`

### 构造函数

```cpp
explicit ThreadPool(size_t threadCount = 0);
```

`threadCount = 0` 使用 `std::thread::hardware_concurrency()`，最小保证 1。

### 生命周期

| 方法 | 说明 |
|------|------|
| `start()` | 启动所有工作线程（每个线程调用 `EventLoop::run()`） |
| `stop()` | 停止所有 EventLoop 并 join 线程 |

### 访问器

| 方法 | 说明 |
|------|------|
| `threadCount()` | 线程数 |
| `eventLoop(index)` | 获取指定线程的 EventLoop |
| `scheduler(index)` | 获取指定线程的 Scheduler |

### 依赖

- `EventLoop`、`Scheduler`
