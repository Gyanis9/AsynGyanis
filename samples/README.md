# echo_server — 多线程 HTTP/HTTPS 示例

基于 AsynGyanis 框架的全功能 HTTP/HTTPS 服务器示例，支持 SO_REUSEPORT 内核级负载均衡。

## 命令行参数

```
echo_server [--host localhost] [--port 8080] [--threads 0]
            [--https] [--cert cert.pem] [--key key.pem]
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--host` | `localhost` | 监听地址（支持 IPv4/IPv6 域名解析） |
| `--port` | `8080` | 监听端口（HTTP 和 HTTPS 共用） |
| `--threads` | `0` | 工作线程数，`0` 表示使用全部 CPU 核心 |
| `--https` | 关闭 | 启用 TLS/HTTPS 模式 |
| `--cert` | `cert.pem` | TLS 证书文件路径（PEM 格式） |
| `--key` | `key.pem` | TLS 私钥文件路径（PEM 格式） |

## 端点

服务器注册了三个测试端点，适用于性能基准测试：

| 方法 | 路径 | 响应 |
|------|------|------|
| GET | `/` | `Hello World` |
| GET | `/json` | `{"status":"ok","version":"1.0.0","server":"AsynGyanis"}` |
| GET | `/bench` | `OK` |

## 运行

### HTTP 模式（默认）

```bash
# 使用全部 CPU 核心，监听 localhost:8080
./build/release/samples/echo_server

# 监听所有接口，指定 4 个线程
./build/release/samples/echo_server --host 0.0.0.0 --port 8080 --threads 4
```

### HTTPS 模式

```bash
# 1. 生成自签名证书（测试用）
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj "/CN=localhost"

# 2. 启动 HTTPS 服务器
./build/release/samples/echo_server --https --cert cert.pem --key key.pem

# 3. 测试（-k 跳过证书验证，仅用于测试）
curl -k https://localhost:8080/bench
```

## 压测

### HTTP 基准测试

```bash
# ApacheBench
ab -n 1000000 -c 200 http://localhost:8080/bench

# wrk
wrk -t4 -c200 -d30s http://localhost:8080/bench
```

### HTTPS 基准测试

```bash
ab -n 500000 -c 200 https://localhost:8080/bench
```

### 典型性能（Linux, 4 线程, 200 并发）

| 模式 | QPS | 说明 |
|------|-----|------|
| HTTP `/bench` | ~32,000 req/s | 纯 epoll 边缘触发 |
| HTTPS `/bench` | ~2,800 req/s | TLS 握手 + AES 加解密开销 |

## 架构

```
主线程                                                   工作线程 × N
  │                                                         │
  ├── IoContext(threads)                                    │
  │     └── ThreadPool                                      │
  │           ├── EventLoop[0] ◀──── std::jthread #0 ──────┘
  │           │     ├── Epoll                             每个线程独立事件循环
  │           │     ├── Scheduler                         协程调度 + 工作窃取
  │           │     └── HttpServer / HttpsServer
  │           │           ├── TcpAcceptor (SO_REUSEPORT)   内核级负载均衡
  │           │           └── ConnectionManager
  │           │                 └── HttpSession / HttpsSession
  │           │                       ├── HttpParser (llhttp)
  │           │                       └── Router → Handler
  │           └── EventLoop[N] ...
  │
  ├── pool.start()         启动所有工作线程
  ├── std::cout            打印启动信息
  └── while(g_running)     等待 SIGINT/SIGTERM
        └── ctx.stop()     优雅关闭
```

## TLS 架构

```
HttpsServer (继承 TcpServer)
  └── m_tlsContext (SSL_CTX)
        ├── SSL_CTX_new(TLS_server_method())
        ├── 加载证书/私钥 (PEM)
        └── createSSL(fd) → SSL* + SSL_set_fd

接受连接时:
  TcpAcceptor::accept() → AsyncSocket
    → HttpsServer::createConnection(socket)
        → SSL* = m_tlsContext.createSSL(socket.fd())
        → TlsSocket(ssl, loop, socket)
        → HttpsSession(loop, tlsSocket, router)
            → start()
                → tlsSocket.handshake()    TLS 握手 (SSL_accept)
                    WANT_READ  → EpollAwaiter(EPOLLIN)
                    WANT_WRITE → EpollAwaiter(EPOLLOUT)
                → HTTP keep-alive 循环
                    tlsSocket.asyncRecv()  SSL_read
                    tlsSocket.asyncSend()  SSL_write
                → tlsSocket.close()        SSL_shutdown + close
```

## 信号处理

| 信号 | 行为 |
|------|------|
| `SIGINT` (Ctrl+C) | 设置 `g_running = false`，优雅退出 |
| `SIGTERM` | 同上 |
| `SIGPIPE` | 忽略（防止写入已关闭连接时崩溃） |
