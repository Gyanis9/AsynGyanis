# Logger 模块接口文档

## 概述

`Logger` 模块提供了日志系统的核心组件——`Logger` 日志器类和 `LoggerRegistry` 全局注册表。支持多命名日志器、级别过滤、多 Sink 输出、`std::format` 格式化日志以及便捷宏。

## 架构设计

```
Logger (日志器实例)
  ├── 名称 + 级别
  ├── Shell 集合 (多个输出目标)
  └── 日志记录方法

LoggerRegistry (全局单例注册表)
  ├── 按名称管理 Logger 实例
  └── 自动创建 / 注册 / 注销

日志宏 (便捷接口)
  ├── LOG_TRACE ... LOG_FATAL  (根日志器)
  ├── LOG_LOGGER_*             (指定日志器)
  └── LOG_*_FMT                (格式化版本)
```

## Logger 类

### 构造函数

```cpp
explicit Logger(std::string name);
```

**参数**：`name` — 日志器名称，用于在日志输出中标识来源。

### 日志记录方法

#### log（基础版本）

```cpp
void log(LogLevel level, std::string_view message,
         const SourceLocation& location = SourceLocation::current()) const;

void log(LogLevel level, const SourceLocation& location,
         std::string_view message) const;
```

**功能**：按当前日志级别过滤后写入日志事件。

**流程**：
1. `shouldLog(level)` 检查 → 不满足则立即返回
2. 构建 `LogEvent`（含时间戳、线程 ID、源码位置等）
3. `writeToSinks(event)` → 分发到所有 Sink（每个 Sink 再检查自己的级别）

#### logFormat（模板方法）

```cpp
template<typename... Args>
void logFormat(LogLevel level, const SourceLocation& location,
               std::string_view fmt, Args&&... args) const;
```

**功能**：使用 C++20 `std::format` 格式化日志消息。

**格式错误处理**：捕获 `std::format_error`，记录包含错误详情的 ERROR 日志，不会崩溃。

### Sink 管理

| 方法 | 说明 |
|------|------|
| `addSink(sink)` | 追加一个输出 Sink（接管所有权） |
| `clearSinks()` | 清空所有 Sink |

### 级别管理

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `setLevel(level)` | `void` | 设置日志器最低输出级别 |
| `getLevel()` | `LogLevel` | 获取当前级别 |
| `shouldLog(level)` | `bool` | 判断给定级别是否允许输出 |

### 属性访问

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `name()` | `const std::string&` | 获取日志器名称 |
| `flush()` | `void` | 刷新所有 Sink 缓冲区 |

---

## LoggerRegistry 类

### 单例访问

```cpp
static LoggerRegistry& instance();
```

### 日志器管理

#### getLogger

```cpp
Logger& getLogger(const std::string& name);
```

**功能**：按名称获取日志器。不存在时自动创建（惰性初始化）。

**线程安全**：使用独占锁保护 map 的读写。

#### getRootLogger

```cpp
Logger& getRootLogger();
```

**功能**：获取默认根日志器（名称为 `"root"`）。等价于 `getLogger("root")`。

#### registerLogger

```cpp
void registerLogger(std::unique_ptr<Logger> logger);
```

**功能**：注册外部创建的日志器。若同名则覆盖已有实例。`nullptr` 参数被忽略。

#### unregisterLogger

```cpp
void unregisterLogger(const std::string& name);
```

**功能**：注销指定名称的日志器。名称不存在时静默返回。

#### 查询方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `getLoggerNames()` | `std::vector<std::string>` | 返回所有已注册日志器名称 |
| `clear()` | `void` | 清空所有日志器 |
| `forEachLogger(func)` | `void` | 对每个日志器执行回调 |

```cpp
LoggerRegistry::instance().forEachLogger([](Logger& logger) {
    logger.setLevel(LogLevel::WARN);
});
```

---

## 日志宏

### 根日志器宏

使用默认根日志器（`"root"`）输出日志：

```cpp
#define LOG_TRACE(message)  // TRACE 级别
#define LOG_DEBUG(message)  // DEBUG 级别
#define LOG_INFO(message)   // INFO  级别
#define LOG_WARN(message)   // WARN  级别
#define LOG_ERROR(message)  // ERROR 级别
#define LOG_FATAL(message)  // FATAL 级别
```

### 格式化宏（C++20 std::format）

```cpp
#define LOG_INFO_FMT(fmt, ...)   // INFO 级别格式化
#define LOG_ERROR_FMT(fmt, ...)  // ERROR 级别格式化
// 完整的级别列表：TRACE, DEBUG, INFO, WARN, ERROR, FATAL
```

**使用示例**：
```cpp
LOG_INFO_FMT("Server listening on {}:{}", host, port);         // 普通
// 编译期校验：
//   LOG_INFO_FMT("value={}", 42);              // OK
//   LOG_INFO_FMT("broken {}", );               // 编译错误
```

### 指定日志器宏

```cpp
#define LOG_LOGGER(logger, level, message)
#define LOG_LOGGER_INFO_FMT(logger, fmt, ...)
```

**使用示例**：
```cpp
auto& netLogger = LoggerRegistry::instance().getLogger("network");
LOG_LOGGER_INFO_FMT(netLogger, "Connection from {}", remote_addr);
```

### 宏实现细节

所有宏在内层都包含 `shouldLog()` 快速路径检查，低于阈值的日志在编译成几乎零开销：

```cpp
#define LOG_INFO(message) \
    do { \
        auto& __logger = Base::LoggerRegistry::instance().getRootLogger(); \
        if (__logger.shouldLog(LogLevel::INFO)) { \
            __logger.log(LogLevel::INFO, (message)); \
        } \
    } while (0)
```

---

## 使用示例

### 基础日志

```cpp
#include "Base/Logger.h"

// 使用根日志器
LOG_INFO("Application started");
LOG_ERROR("Failed to connect to database");

// 格式化日志
int port = 8080;
std::string host = "0.0.0.0";
LOG_INFO_FMT("Listening on {}:{}", host, port);
```

### 多日志器

```cpp
auto& registry = Base::LoggerRegistry::instance();

auto& netLogger = registry.getLogger("network");
netLogger.setLevel(Base::LogLevel::DEBUG);
netLogger.addSink(std::make_unique<Base::FileSink>("logs/network.log"));

auto& dbLogger = registry.getLogger("database");
dbLogger.setLevel(Base::LogLevel::INFO);
dbLogger.addSink(std::make_unique<Base::FileSink>("logs/database.log"));

LOG_LOGGER_INFO_FMT(netLogger, "Accepted connection from {}", addr);
LOG_LOGGER_INFO_FMT(dbLogger, "Query executed in {}ms", elapsed);
```

### 自定义 Logger 配置

```cpp
auto logger = std::make_unique<Base::Logger>("custom");

// 添加多种 Sink
logger->addSink(std::make_unique<Base::ConsoleSink>(true));
logger->addSink(std::make_unique<Base::FileSink>("logs/custom.log"));

// 设置级别
logger->setLevel(Base::LogLevel::WARN);

// 注册到全局注册表
Base::LoggerRegistry::instance().registerLogger(std::move(logger));
```

## 线程安全性

| 操作 | 线程安全性 |
|------|-----------|
| `Logger::log()` | 安全（Sink 列表使用 `std::shared_mutex` 共享锁） |
| `Logger::addSink()` / `clearSinks()` | 安全（独占锁） |
| `Logger::setLevel()` | 安全（原子操作） |
| `LoggerRegistry::getLogger()` | 安全（独占锁） |
| `LoggerRegistry::forEachLogger()` | 安全（共享锁） |
| `LoggerRegistry::registerLogger()` | 安全（独占锁） |

## 依赖

- `LogSink.h`：日志 Sink 体系
- `LogCommon.h`：日志基础类型
- C++20 标准库（`<format>`、`<shared_mutex>`）
