# AsyncEventLoop 模块接口文档

## 概述

`AsyncEventLoop` 是基于 `io_uring` 的异步事件循环模块。采用单线程事件循环模型（one loop per thread），通过回调机制分发已完成的异步 I/O 操作。内部使用 `eventfd` 实现跨线程安全唤醒。

## 核心类型

### AsyncOpType 枚举

| 枚举值 | 整数值 | 说明 |
|--------|--------|------|
| `Read` | 0 | 异步读取操作 |
| `Write` | 1 | 异步写入操作 |
| `Accept` | 2 | 异步接受连接 |
| `Connect` | 3 | 异步发起连接 |
| `Timeout` | 4 | 超时操作 |
| `Fsync` | 5 | 异步文件同步 |
| `Close` | 6 | 异步关闭文件描述符 |
| `NoOp` | 7 | 空操作（仅用于唤醒事件循环） |

### AsyncCompletion

```cpp
struct AsyncCompletion {
    AsyncOpType type;     // 操作类型
    int         result;   // >= 0 成功，< 0 为 -errno
    uint64_t    opId;     // 操作 ID
    void*       userData; // 用户自定义数据
};
```

### AsyncCallback

```cpp
using AsyncCallback = std::function<void(AsyncCompletion)>;
```

## API 参考

### 构造函数

```cpp
explicit AsyncEventLoop(unsigned queueDepth = 256, unsigned flags = 0);
```

**参数**：

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `queueDepth` | `unsigned` | `256` | io_uring SQ 条目数（2 的幂） |
| `flags` | `unsigned` | `0` | io_uring 初始化标志 |

**异常**：`io_uring_queue_init` 或 `eventfd` 创建失败时抛出 `std::runtime_error`。

### 析构函数

```cpp
~AsyncEventLoop();
```

自动停止运行中的事件循环并释放资源。

### 生命周期管理

#### run

```cpp
void run();
```

**功能**：启动事件循环，阻塞当前线程直到 `stop()` 被调用。

**行为详情**：

1. 循环执行：提交延迟操作 → `submitAndWait(1)` → 收割完成事件
2. 收到 `stop()` 信号后退出循环
3. 被 `EINTR` 中断时自动重试
4. 其他错误时退出循环

#### runOnce

```cpp
void runOnce();
```

**功能**：执行一次非阻塞迭代：提交所有延迟 SQE 并收割已完成的 CQE。

**适用场景**：集成到其他事件循环中，或实现轮询模式。

#### stop

```cpp
void stop();
```

**功能**：异步通知事件循环停止。线程安全，可从任意线程调用。通过向内部 `eventfd` 写入数据唤醒阻塞中的 `run()`。

---

### 异步 I/O 操作

#### read

```cpp
uint64_t read(int fd, void* buf, unsigned nbytes, off_t offset,
              AsyncCallback cb, void* userData = nullptr);
```

**参数**：

| 参数名 | 类型 | 说明 |
|--------|------|------|
| `fd` | `int` | 文件描述符 |
| `buf` | `void*` | 读缓冲区指针。调用者须保证缓冲区在操作期间有效 |
| `nbytes` | `unsigned` | 读取字节数 |
| `offset` | `off_t` | 文件偏移量（-1 表示当前位置） |
| `cb` | `AsyncCallback` | 完成回调。操作完成时调用，`completion.result` 为读取字节数或 `-errno` |
| `userData` | `void*` | 用户自定义数据（透传至回调的 `completion.userData`） |

**返回值**：`uint64_t` — 操作 ID，可用于后续 `cancel()`。

#### write

```cpp
uint64_t write(int fd, const void* buf, unsigned nbytes, off_t offset,
               AsyncCallback cb, void* userData = nullptr);
```

参数与 `read()` 相同。回调中 `completion.result` 为写入字节数或 `-errno`。

#### timeout

```cpp
uint64_t timeout(uint64_t ms, AsyncCallback cb, void* userData = nullptr);
```

**参数**：

| 参数名 | 类型 | 说明 |
|--------|------|------|
| `ms` | `uint64_t` | 超时毫秒数 |
| `cb` | `AsyncCallback` | 超时完成回调。正常超时 `completion.result == 0`；取消 `completion.result == -ECANCELED` |
| `userData` | `void*` | 用户自定义数据 |

#### cancel

```cpp
bool cancel(uint64_t opId);
```

**功能**：取消一个正在执行的异步操作。

**参数**：`opId` — 由 `read()`/`write()`/`timeout()` 返回的操作 ID。

**返回值**：

- `true`：取消请求已提交
- `false`：SQE 不可用（队列已满）

**重要说明**：取消是异步的，被取消的操作将通过其回调以 `result == -ECANCELED` 通知。并非所有操作类型都支持取消（取决于内核和操作状态）。

---

### 访问器

#### context

```cpp
[[nodiscard]] IOUringContext& context() noexcept;
```

**功能**：获取底层 io_uring 上下文引用。可用于直接操作 SQE/CQE 或集成 epoll。

#### isRunning

```cpp
[[nodiscard]] bool isRunning() const noexcept;
```

**功能**：查询事件循环是否正在 `run()` 中执行。

---

## 内部机制

### eventfd 唤醒

事件循环使用 `eventfd` 实现 `stop()` 的安全跨线程唤醒：

1. 构造时创建 `eventfd` 并提交初始读取 SQE
2. `stop()` 调用时间 `eventfd` 写入数据
3. `run()` 中的 `submitAndWait()` 因 eventfd 变为可读而被唤醒
4. 收到 eventfd 完成事件后重新提交读取，准备下次唤醒

### 延迟操作队列

当 SQ 已满时，新的操作请求被存入 `m_deferredOps` 暂存队列，在下次 `run()` 或 `runOnce()` 迭代时优先提交。

### 操作生命周期

```
read()/write()/timeout()
  → nextOpId()          (分配操作 ID)
  → registerPending()    (注册回调)
  → getSqe() + fillSqe   (填充 SQE)
  → submit()             (提交至内核)
  → harvestCompletions() (收割 CQE)
  → 查找 pending ops     (通过 opId)
  → 调用回调             (分发 AsyncCompletion)
```

## 使用示例

### 基本事件循环

```cpp
#include "Base/AsyncEventLoop.h"

Base::AsyncEventLoop loop;

// 提交操作
loop.timeout(1000, [](const Base::AsyncCompletion& comp) {
    if (comp.result == 0) {
        std::cout << "1 秒超时已触发" << std::endl;
    }
});

// 运行事件循环（阻塞）
loop.run();
```

### 管道读写

```cpp
Base::AsyncEventLoop loop;
int pipefd[2];
pipe(pipefd);

// 写入数据
const char* msg = "hello";
write(pipefd[1], msg, strlen(msg));

// 异步读取
char buf[64];
loop.read(pipefd[0], buf, sizeof(buf), 0,
    [](const Base::AsyncCompletion& comp) {
        if (comp.result > 0) {
            std::cout << "读取了 " << comp.result << " 字节" << std::endl;
        }
    });

// 在另一个线程运行
std::thread t([&]() { loop.run(); });

// 等待一段时间后停止
std::this_thread::sleep_for(100ms);
loop.stop();
t.join();
```

### 取消超时操作

```cpp
Base::AsyncEventLoop loop;

uint64_t opId = loop.timeout(5000, [](const Base::AsyncCompletion& comp) {
    if (comp.result == -ECANCELED) {
        std::cout << "超时已被取消" << std::endl;
    }
});

// 立即取消
loop.cancel(opId);
```

## 线程模型

| 操作 | 线程约束 |
|------|---------|
| `run()` / `runOnce()` | 同一次只允许一个线程调用 |
| `read()` / `write()` / `timeout()` | 可在 `run()` 线程或其他线程调用 |
| `stop()` | 任意线程安全 |
| 回调函数 | 在 `run()` / `runOnce()` 线程中执行 |
| `cancel()` | 任意线程，但可能需 `run()` 线程协同提交 |

## 性能特征

- SQE 获取/填充：O(1) 无系统调用（共享内存）
- 事件收割：O(已完成的 CQE 数量)
- `stop()` 唤醒延迟：< 1ms（eventfd 内核路径）
- 回调分发：O(1) 哈希查找

## 依赖

- `IOUringContext.h`：io_uring RAII 封装
- `liburing`：Linux io_uring 用户态库
- C++20 标准库
- Linux `eventfd(2)`
