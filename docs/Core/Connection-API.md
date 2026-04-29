# Connection 模块接口文档

## 概述

`Connection` 是 TCP 连接基类，封装 `AsyncSocket` 和 `Cancelable`。提供连接生命周期管理（`close`/`isAlive`）和可重写的协程入口（`start`）。派生类（如 `HttpSession`）重写 `start()` 实现具体的协议处理逻辑。

## 核心类

### Connection

**头文件**：`Connection.h` | **实现**：`Connection.cpp`

### 构造函数

```cpp
Connection(EventLoop& loop, AsyncSocket socket);
```

### 生命周期

| 方法 | 说明 |
|------|------|
| `virtual Task<> start()` | 协程入口，基类为空实现 |
| `close()` | 设置 `m_alive=false`、请求取消、关闭 socket |
| `isAlive()` | 查询连接是否活跃 |

### 访问器

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `socket()` | `AsyncSocket&` | 底层 socket |
| `cancelable()` | `Cancelable&` | 取消令牌 |
| `remoteAddress()` | `std::string` | 对端地址字符串 |
| `localAddress()` | `std::string` | 本地地址字符串 |

### 依赖

- `AsyncSocket`、`Cancelable`、`Task<T>`
