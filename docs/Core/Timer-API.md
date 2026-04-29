# Timer 模块接口文档

## 概述

`Timer` 是基于 `timerfd_create` 的可等待定时器。返回 `EpollAwaiter` 对象，可在协程中通过 `co_await` 挂起指定时长。

## 核心类

### Timer

**头文件**：`Timer.h` | **实现**：`Timer.cpp`

### 构造函数

```cpp
explicit Timer(EventLoop& loop);
```

创建一个 `timerfd`（`CLOCK_MONOTONIC`、`TFD_NONBLOCK | TFD_CLOEXEC`）。

### waitFor

```cpp
EpollAwaiter waitFor(std::chrono::milliseconds duration) const;
```

**功能**：设置定时器并返回一个 `EpollAwaiter`，在 duration 毫秒后触发。

### 使用示例

```cpp
Core::Timer timer(loop);
co_await timer.waitFor(5000ms);  // 挂起 5 秒
```

## 依赖

- `EventLoop`、`EpollAwaiter`
- `<sys/timerfd.h>`
