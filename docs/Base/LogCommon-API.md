# LogCommon 模块接口文档

## 概述

`LogCommon` 模块定义了日志系统的基础类型和公共工具函数。包括日志等级枚举、时间戳格式化、线程 ID 获取、源码位置封装、日志事件结构体以及终端颜色支持等。

## 核心类型

### LogLevel 枚举

日志等级，优先级从低到高：

| 枚举值 | 整数值 | 说明 | 典型用途 |
|--------|--------|------|---------|
| `TRACE` | 0 | 跟踪级别 | 最细粒度的调试信息，函数进入/退出追踪 |
| `DEBUG` | 1 | 调试级别 | 开发调试信息 |
| `INFO` | 2 | 信息级别 | 一般运行时信息 |
| `WARN` | 3 | 警告级别 | 潜在问题，不影响正常运行 |
| `ERROR` | 4 | 错误级别 | 错误但系统可继续运行 |
| `FATAL` | 5 | 致命级别 | 严重错误，系统可能无法继续 |
| `OFF` | 6 | 关闭 | 完全禁用日志输出 |

> **注意**：在 Windows 平台上，`ERROR` 宏可能与 `windows.h` 冲突。本模块在定义 `LogLevel::ERROR` 前会自动 `#undef ERROR`。

## 公共函数

### 1. logLevelToString

```cpp
const char* logLevelToString(LogLevel level) noexcept;
```

**功能**：将日志等级转换为固定宽度的字符串表示（5 字符，不足右填空格）。

**返回值映射**：
| 输入 | 输出 |
|------|------|
| `TRACE` | `"TRACE"` |
| `DEBUG` | `"DEBUG"` |
| `INFO` | `"INFO "` |
| `WARN` | `"WARN "` |
| `ERROR` | `"ERROR"` |
| `FATAL` | `"FATAL"` |
| 其他 | `"?????"` |

### 2. logLevelFromString

```cpp
LogLevel logLevelFromString(std::string_view str);
```

**功能**：将字符串转换为日志等级。不区分大小写吗？**否，严格区分大小写**。

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `str` | `std::string_view` | 日志等级名称 |

**返回值**：匹配的 `LogLevel`。不匹配任何已知名称时返回 `LogLevel::INFO`。

### 3. currentTimestamp

```cpp
std::string currentTimestamp();
```

**功能**：生成当前时间戳字符串。

**格式**：`YYYY-MM-DD HH:MM:SS.mmm`（23 字符，精确到毫秒）。

**示例输出**：`2026-04-28 20:30:45.123`

**平台适配**：
- Linux：使用 `localtime_r`（线程安全）
- Windows：使用 `localtime_s`（线程安全）

### 4. threadIdString

```cpp
std::string threadIdString();
```

**功能**：获取当前线程的标识字符串。

**平台适配**：
- Linux：通过 `std::ostringstream` 格式化 `std::this_thread::get_id()`
- Windows：使用 `GetCurrentThreadId()` 并格式化为十进制数字

## SourceLocation

```cpp
struct SourceLocation {
    const char* file_name;
    int line;
    const char* function_name;
};
```

### 构造函数

```cpp
// 默认构造（全空）
constexpr SourceLocation();

// 显式构造
constexpr SourceLocation(const char* file, int l, const char* func);

// 从 std::source_location 构造
constexpr explicit SourceLocation(const std::source_location& loc);
```

### 静态工厂方法

```cpp
static constexpr SourceLocation current(
    const std::source_location& loc = std::source_location::current());
```

**功能**：捕获当前调用点的源码位置信息。

### 实例方法

```cpp
const char* shortFileName() const noexcept;
```

**功能**：从完整文件路径中提取文件名（去除目录部分）。支持 Unix (`/`) 和 Windows (`\`) 路径分隔符。

## 便捷宏

```cpp
#define LOG_SOURCE_LOCATION() Base::SourceLocation::current()
```

捕获当前源码位置，用于日志记录。

## LogEvent

```cpp
struct LogEvent {
    LogLevel       level;
    std::string    timestamp;
    std::string    thread_id;
    SourceLocation location;
    std::string    logger_name;
    std::string    message;
};
```

日志事件的完整封装，包含：

| 字段 | 类型 | 说明 |
|------|------|------|
| `level` | `LogLevel` | 日志等级 |
| `timestamp` | `std::string` | 格式化时间戳 |
| `thread_id` | `std::string` | 线程标识 |
| `location` | `SourceLocation` | 源码位置 |
| `logger_name` | `std::string` | 日志器名称 |
| `message` | `std::string` | 日志消息内容 |

## 颜色支持（`color` 命名空间）

### 终端控制序列常量

| 常量 | 值 | 颜色 |
|------|-----|------|
| `RESET` | `\033[0m` | 重置 |
| `RED` | `\033[31m` | 红色 |
| `GREEN` | `\033[32m` | 绿色 |
| `YELLOW` | `\033[33m` | 黄色 |
| `BLUE` | `\033[34m` | 蓝色 |
| `MAGENTA` | `\033[35m` | 洋红 |
| `CYAN` | `\033[36m` | 青色 |
| `WHITE` | `\033[37m` | 白色 |
| `BRIGHT_*` | 高亮色 | 高亮变体 |

### getColorForLevel

```cpp
const char* getColorForLevel(LogLevel level);
```

**功能**：根据日志等级返回对应的 ANSI 颜色转义序列。

**映射关系**：
| LogLevel | 颜色 |
|----------|------|
| `TRACE` | `BRIGHT_BLACK`（灰色） |
| `DEBUG` | `CYAN`（青色） |
| `INFO` | `GREEN`（绿色） |
| `WARN` | `YELLOW`（黄色） |
| `ERROR` | `RED`（红色） |
| `FATAL` | `BRIGHT_RED`（亮红色） |

### terminalSupportsColor

```cpp
bool terminalSupportsColor();
```

**功能**：检测当前终端是否支持 ANSI 颜色输出。

- Windows：始终返回 `true`（Windows 10+ 支持虚拟终端处理）
- Linux：检查 `TERM` 环境变量是否包含 `"color"` 子串

## 使用示例

```cpp
#include "Base/LogCommon.h"

// 日志等级转换
auto level = Base::logLevelFromString("WARN");
std::cout << Base::logLevelToString(level); // "WARN "

// 时间戳
std::string ts = Base::currentTimestamp(); // "2026-04-28 20:30:45.123"

// 源码位置
auto loc = LOG_SOURCE_LOCATION();
std::cout << loc.shortFileName() << ":" << loc.line;

// 构造日志事件
Base::LogEvent event(
    Base::LogLevel::ERROR,
    Base::currentTimestamp(),
    Base::threadIdString(),
    Base::SourceLocation::current(),
    "my_logger",
    "An error occurred"
);

// 颜色
if (Base::color::terminalSupportsColor()) {
    std::cout << Base::color::getColorForLevel(Base::LogLevel::WARN)
              << "Warning!" << Base::color::RESET << std::endl;
}
```

## 依赖

- C++20 标准库（`<format>`、`<chrono>`、`<source_location>`、`<thread>`）
- Windows：`<windows.h>`
