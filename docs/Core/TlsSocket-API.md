# TlsSocket 模块接口文档

## 概述

`TlsSocket` 是 TLS 加密套接字包装器，将 OpenSSL 的 `SSL_read`/`SSL_write` 与非阻塞 epoll 集成。内部持有 `SSL*` 和底层 `AsyncSocket`，提供协程式 TLS 握手和加密 I/O。

## 核心类

### TlsSocket

**头文件**：`TlsSocket.h` | **实现**：`TlsSocket.cpp`

### 构造函数

```cpp
TlsSocket(SSL* ssl, EventLoop& loop, AsyncSocket socket);
```

**参数**：`ssl` 由 `TlsContext::createSSL()` 创建，所有权转移给 `TlsSocket`。

### 握手

```cpp
Task<> handshake();
```

**功能**：执行 TLS 服务端握手（`SSL_accept`）。

**非阻塞处理**：
- `SSL_ERROR_WANT_READ` → `co_await EpollAwaiter(epoll, fd, EPOLLIN)` 等待对端数据
- `SSL_ERROR_WANT_WRITE` → `co_await EpollAwaiter(epoll, fd, EPOLLOUT)` 等待可写
- 其他错误 → 抛出 `Exception`

### 加密 I/O

```cpp
Task<ssize_t> asyncRecv(void* buf, size_t len);
Task<ssize_t> asyncSend(const void* buf, size_t len);
```

**功能**：通过 `SSL_read`/`SSL_write` 进行加密通信。接口与 `AsyncSocket` 一致。

**非阻塞处理**：同 `handshake()`，处理 `WANT_READ`/`WANT_WRITE`。`SSL_ERROR_ZERO_RETURN` 返回 0（对端优雅关闭）。

### 控制方法

| 方法 | 说明 |
|------|------|
| `close()` | `SSL_shutdown` → `SSL_free` → 关闭底层 socket |
| `fd()` | 获取底层 socket 文件描述符 |

## 使用示例

```cpp
Core::TlsSocket tlsSocket(ssl, loop, std::move(socket));
co_await tlsSocket.handshake();  // TLS 握手
char buf[4096];
ssize_t n = co_await tlsSocket.asyncRecv(buf, sizeof(buf));
co_await tlsSocket.asyncSend("HTTP/1.1 200 OK\r\n\r\n", 19);
tlsSocket.close();
```

## 依赖

- `AsyncSocket` — 底层 TCP socket
- `EpollAwaiter` — 非阻塞等待
- `EventLoop` — 提供 Epoll 引用
- OpenSSL 3.x — 加密库
