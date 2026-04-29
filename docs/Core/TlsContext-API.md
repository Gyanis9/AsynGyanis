# TlsContext 模块接口文档

## 概述

`TlsContext` 是 OpenSSL `SSL_CTX` 的 RAII 封装。管理 TLS 服务器上下文生命周期，提供证书/私钥加载和每连接 SSL 对象创建功能。

## 核心类

### TlsContext

**头文件**：`TlsContext.h` | **实现**：`TlsContext.cpp`

### 构造函数

```cpp
TlsContext();
```

**功能**：调用 `SSL_CTX_new(TLS_server_method())` 创建上下文。自动禁用 SSLv2/SSLv3，启用部分写入和移动缓冲区支持。

### 证书加载

```cpp
bool loadCertificate(const std::string& certFile, const std::string& keyFile);
```

**参数**：PEM 格式证书文件和私钥文件路径。

**返回值**：证书、私钥加载且私钥校验通过时返回 `true`。

### SSL 对象创建

```cpp
SSL* createSSL(int fd);
```

**功能**：为已连接的 socket 创建 SSL 对象。调用 `SSL_new(m_ctx)` + `SSL_set_fd(ssl, fd)`。

**返回值**：新 SSL 对象（所有权转移给调用方，由 `TlsSocket` 管理）。

### 访问器

```cpp
SSL_CTX* nativeHandle() const noexcept;
```

## 使用示例

```cpp
Core::TlsContext ctx;
ctx.loadCertificate("cert.pem", "key.pem");
SSL* ssl = ctx.createSSL(clientFd);
```

## 依赖

- OpenSSL 3.x（`ssl.h`、`err.h`）
- `Base/Exception.h`
