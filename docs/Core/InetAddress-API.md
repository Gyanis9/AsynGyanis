# InetAddress 模块接口文档

## 概述

`InetAddress` 是 IPv4/IPv6 统一网络地址封装类。内部使用 `sockaddr_storage` 存储，支持 DNS 解析、多种构造方式和比较操作。

## 核心类

### InetAddress

**头文件**：`InetAddress.h` | **实现**：`InetAddress.cpp`

| 成员 | 类型 | 说明 |
|------|------|------|
| `m_addr` | `sockaddr_storage` | 地址存储 |
| `m_addrLen` | `socklen_t` | 地址结构长度 |

### 构造函数

```cpp
InetAddress();                                                  // 默认: 0.0.0.0:0
explicit InetAddress(uint16_t port, std::string_view ip = "0.0.0.0");
InetAddress(std::string_view ip, uint16_t port);
explicit InetAddress(const sockaddr_in& addr);
explicit InetAddress(const sockaddr_in6& addr);
InetAddress(const sockaddr_storage& addr, socklen_t len);
```

### 静态工厂方法

| 方法 | 说明 |
|------|------|
| `localhost(port)` | `127.0.0.1:port` |
| `any(port)` | `0.0.0.0:port`，监听所有接口 |
| `resolve(host, port)` | DNS 解析，返回 `std::optional<InetAddress>` |

### 查询方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `family()` | `int` | `AF_INET` 或 `AF_INET6` |
| `ip()` | `std::string` | 点分十进制或冒号十六进制 |
| `port()` | `uint16_t` | 端口号（主机字节序） |
| `addr()` | `const sockaddr*` | 底层地址指针 |
| `addrLen()` | `socklen_t` | 地址结构长度 |
| `toString()` | `std::string` | `ip:port` 格式，IPv6 带方括号 |

### 比较运算符

```cpp
bool operator==(const InetAddress& other) const;
bool operator!=(const InetAddress& other) const;
```

## 使用示例

```cpp
#include "Core/InetAddress.h"

auto addr = Core::InetAddress::resolve("localhost", 8080);
if (addr) {
    std::cout << addr->toString();  // "127.0.0.1:8080"
}
```

## 依赖

- `Base/Exception.h`
- `<sys/socket.h>`、`<netdb.h>`、`<arpa/inet.h>`
