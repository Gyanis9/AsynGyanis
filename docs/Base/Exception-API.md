# Expection 模块接口文档

## 概述

`Expection` 模块定义了配置系统使用的异常类层次结构。所有异常均继承自 `std::runtime_error`，并自动携带源码位置信息（通过 `std::source_location`），便于问题定位和调试。

## 异常类层次结构

```
std::runtime_error
  └── ConfigException                 (配置模块基础异常)
        ├── ConfigFileException       (文件读取异常)
        ├── ConfigParseException      (YAML 解析异常)
        ├── ConfigKeyNotFoundException (配置键未找到异常)
        ├── ConfigTypeException       (类型转换异常)
        └── ConfigValidationException  (配置验证异常)
```

## 核心异常类

### 1. ConfigException

**基类**：`std::runtime_error`

**用途**：配置模块所有异常的基础类。封装了异常消息和触发异常的源码位置信息。

**构造函数**：
```cpp
explicit ConfigException(const std::string& message,
                         const std::source_location& loc = std::source_location::current());
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `message` | `const std::string&` | 异常描述消息 |
| `loc` | `const std::source_location&` | 触发异常的源码位置，默认为调用点 |

**成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `what()` | `const char*` | 获取格式化异常消息（含文件名、行号、函数名） |
| `location()` | `const std::source_location&` | 获取源码位置信息 |

**消息格式**：
```
[ConfigException] <message> [<file>:<line> in <function>]
```

---

### 2. ConfigFileException

**基类**：`ConfigException`

**用途**：表示配置文件读取/访问失败。

**构造函数**：
```cpp
ConfigFileException(const std::string& file_path, const std::string& reason,
                    const std::source_location& loc = std::source_location::current());
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `file_path` | `const std::string&` | 出错的文件路径 |
| `reason` | `const std::string&` | 失败原因描述 |
| `loc` | `const std::source_location&` | 源码位置 |

**特有成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `filePath()` | `const std::string&` | 获取出错的配置文件路径 |

---

### 3. ConfigParseException

**基类**：`ConfigException`

**用途**：表示 YAML 配置文件解析失败。

**构造函数**：
```cpp
ConfigParseException(const std::string& file_path, const std::string& reason,
                     const std::source_location& loc = std::source_location::current());
```

**参数**：同 `ConfigFileException`。

**特有成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `filePath()` | `const std::string&` | 获取解析出错的 YAML 文件路径 |

---

### 4. ConfigKeyNotFoundException

**基类**：`ConfigException`

**用途**：表示请求的配置键在配置数据中不存在。

**构造函数**：
```cpp
explicit ConfigKeyNotFoundException(const std::string& key,
                                     const std::source_location& loc = std::source_location::current());
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `key` | `const std::string&` | 缺失的配置键名 |
| `loc` | `const std::source_location&` | 源码位置 |

**特有成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `key()` | `const std::string&` | 获取缺失的配置键名 |

---

### 5. ConfigTypeException

**基类**：`ConfigException`

**用途**：表示配置值的实际类型与期望类型不匹配。

**构造函数**：
```cpp
ConfigTypeException(const std::string& key,
                    const std::string& expected_type,
                    const std::string& actual_type,
                    const std::source_location& loc = std::source_location::current());
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `key` | `const std::string&` | 发生类型不匹配的配置键 |
| `expected_type` | `const std::string&` | 期望的类型名称 |
| `actual_type` | `const std::string&` | 实际的类型名称 |
| `loc` | `const std::source_location&` | 源码位置 |

**特有成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `key()` | `const std::string&` | 获取出错的配置键 |
| `expectedType()` | `const std::string&` | 获取期望类型名称 |
| `actualType()` | `const std::string&` | 获取实际类型名称 |

---

### 6. ConfigValidationException

**基类**：`ConfigException`

**用途**：表示配置值验证失败（如数值超出范围、格式不正确等）。

**构造函数**：
```cpp
ConfigValidationException(const std::string& key, const std::string& reason,
                          const std::source_location& loc = std::source_location::current());
```

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `key` | `const std::string&` | 验证失败的配置键 |
| `reason` | `const std::string&` | 验证失败原因 |
| `loc` | `const std::source_location&` | 源码位置 |

**特有成员函数**：
| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `key()` | `const std::string&` | 获取验证失败的配置键 |

## 使用示例

```cpp
#include "Base/Expection.h"

void validatePort(int64_t port) {
    if (port < 1 || port > 65535) {
        throw Base::ConfigValidationException(
            "server.port",
            "端口号必须在 [1, 65535] 范围内，实际值: " + std::to_string(port)
        );
    }
}

void useConfig(const Base::ConfigManager& cfg) {
    try {
        auto port = cfg.getRequired<int64_t>("server.port");
        validatePort(port);
    } catch (const Base::ConfigKeyNotFoundException& e) {
        std::cerr << "缺少必需配置项: " << e.key() << std::endl;
        std::cerr << "位置: " << e.what() << std::endl;
    } catch (const Base::ConfigTypeException& e) {
        std::cerr << "类型错误: 期望 " << e.expectedType()
                  << "，实际类型 " << e.actualType() << std::endl;
    } catch (const Base::ConfigException& e) {
        std::cerr << "配置异常: " << e.what() << std::endl;
    }
}
```

## 性能特征

- 异常构造包含字符串拼接和 `std::source_location` 捕获，开销中等（约数百纳秒）
- 通过 `what()` 获取消息时已完成格式化，无需额外开销
- 所有异常均可通过基类 `std::exception` 或 `ConfigException` 捕获

## 依赖

该模块仅依赖 C++20 标准库（`<source_location>`、`<stdexcept>`、`<sstream>`）。
