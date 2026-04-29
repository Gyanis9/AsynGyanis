# LogSink 模块接口文档

## 概述

`LogSink` 模块提供了日志系统的输出目标（Sink）架构。定义了格式化器接口和多种具体的日志 Sink 实现，包括控制台输出、文件输出、滚动文件输出以及异步写入。

## 架构设计

```
LogFormatter (抽象格式化器)
  ├── DefaultFormatter   (纯文本格式化)
  └── ColorFormatter     (彩色终端格式化)

LogSink (抽象输出目标)
  ├── ConsoleSink        (标准输出/错误输出)
  ├── FileSink           (文件输出)
  ├── RollingFileSink    (滚动文件输出)
  │   └── RollingPolicy  (Size / Daily / Hourly)
  └── AsyncSink          (异步包装器)
      └── OverflowPolicy (Block / Drop)
```

## LogFormatter 接口

```cpp
class LogFormatter {
public:
    virtual ~LogFormatter() = default;
    virtual std::string format(const LogEvent& event) = 0;
};
```

### DefaultFormatter

**输出格式**：
```
<时间戳> <线程ID> [<级别>] [<日志器名>] <文件名:行号> <消息>
```

**示例输出**：
```
2026-04-28 20:30:45.123 140123456789 [INFO ] [myapp] main.cpp:42 Server started
```

### ColorFormatter

**输出格式**：与 `DefaultFormatter` 相同，但日志级别和关键字段使用 ANSI 颜色高亮。

## LogSink 基类

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `setLevel(level)` | `void` | 设置此 Sink 的最低输出级别 |
| `getLevel()` | `LogLevel` | 获取当前日志级别阈值 |
| `shouldLog(level)` | `bool` | 判断给定级别是否满足输出条件 |
| `setFormatter(fmt)` | `void` | 设置自定义格式化器 |
| `write(event)` | `void` | 写入日志事件（纯虚） |
| `flush()` | `void` | 刷新输出缓冲区（纯虚） |

### 级别过滤逻辑

```cpp
bool shouldLog(LogLevel level) const {
    return level >= getLevel();  // 等级 >= 阈值才输出
}
```

默认级别阈值为 `TRACE`，允许所有级别通过。

## ConsoleSink

### 构造函数

```cpp
explicit ConsoleSink(bool enable_color = true);
```

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `enable_color` | `bool` | `true` | 是否启用彩色输出 |

### 输出规则

| 级别 | 输出目标 |
|------|---------|
| `WARN` 及以上 | `stderr`（标准错误） |
| `INFO` 及以下 | `stdout`（标准输出） |

### 特有方法

```cpp
void setColorEnabled(bool enabled);
```

**功能**：动态切换控制台彩色输出。内部自动切换 `ColorFormatter` / `DefaultFormatter`。

**线程安全**：使用 `std::mutex` 保护，保证多线程写入的原子性。

---

## FileSink

### 构造函数

```cpp
explicit FileSink(const std::filesystem::path& file_path, bool truncate = false);
```

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `file_path` | `const std::filesystem::path&` | — | 日志文件路径 |
| `truncate` | `bool` | `false` | `true` 截断模式，`false` 追加模式 |

**自动行为**：
- 自动创建父目录（递归）
- 打开失败抛出 `std::runtime_error`

### reopen

```cpp
void reopen(const std::filesystem::path& new_path);
```

**功能**：关闭当前文件并重新打开新的日志文件（追加模式）。

---

## RollingFileSink

### RollingPolicy 枚举

| 枚举值 | 整数值 | 说明 |
|--------|--------|------|
| `Size` | 0 | 按文件大小滚动 |
| `Daily` | 1 | 每日滚动 |
| `Hourly` | 2 | 每小时滚动 |

### 构造函数

```cpp
RollingFileSink(const std::string& base_filename,
                const std::filesystem::path& directory,
                RollingPolicy policy,
                size_t max_size_bytes = 10 * 1024 * 1024,
                size_t max_backup_files = 10);
```

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `base_filename` | `const std::string&` | — | 基础文件名（如 `app.log`） |
| `directory` | `const std::filesystem::path&` | — | 日志目录 |
| `policy` | `RollingPolicy` | — | 滚动策略 |
| `max_size_bytes` | `size_t` | `10MB` | Size 策略的阈值 |
| `max_backup_files` | `size_t` | `10` | 最大备份文件数 |

### 滚动行为

#### Size 模式
当前日志文件大小超过 `max_size_bytes` 时，将当前文件重命名为 `name.<N>.ext`（N 为递增序号），创建新日志文件。

#### Daily 模式
检查当前日期是否与上次滚动日期相同。日期变更时，将当前文件重命名为 `name.YYYY-MM-DD.ext`。

#### Hourly 模式
与 Daily 类似，但时间后缀精确到小时：`name.YYYY-MM-DD_HH.ext`。

### 备份清理

滚动后自动检查备份文件数是否超过 `max_backup_files`。超出时按最后修改时间升序删除最早的文件。设置 `max_backup_files=0` 时不限制备份数（不执行清理）。

---

## AsyncSink

### OverflowPolicy 枚举

| 枚举值 | 整数值 | 说明 |
|--------|--------|------|
| `Block` | 0 | 队列满时阻塞调用线程 |
| `Drop` | 1 | 队列满时直接丢弃新日志 |

### 构造函数

```cpp
AsyncSink(std::unique_ptr<LogSink> wrapped_sink,
          size_t queue_size = 1024,
          OverflowPolicy policy = OverflowPolicy::Block);
```

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `wrapped_sink` | `std::unique_ptr<LogSink>` | — | 被包装的真实 Sink |
| `queue_size` | `size_t` | `1024` | 队列最大容量 |
| `policy` | `OverflowPolicy` | `Block` | 溢出策略 |

### 内部线程模型

- 构造时启动后台工作线程（`workerLoop`）
- 工作线程持续从队列取日志并写入 `wrapped_sink`
- 析构时调用 `stop()`，排空队列后退出线程
- `flush()` 阻塞等待队列清空后刷新下游 Sink

### 生命周期方法

```cpp
void stop();
```

**功能**：停止后台线程，排空队列中的所有待处理日志。

> **重要**：若需在程序退出前确保所有日志已写入，请显式调用 `stop()` 或 `flush()`。

---

## 使用示例

### ConsoleSink

```cpp
Base::ConsoleSink console(true);  // 彩色输出
console.write(logEvent);
```

### FileSink

```cpp
Base::FileSink file("logs/app.log", false);  // 追加模式
file.write(logEvent);
file.reopen("logs/app_new.log");
```

### RollingFileSink

```cpp
// 按大小滚动，每 100MB 切分，保留 5 个备份
Base::RollingFileSink rolling("app.log", "logs/",
    Base::RollingPolicy::Size, 100 * 1024 * 1024, 5);

// 每日滚动
Base::RollingFileSink daily("daily.log", "logs/",
    Base::RollingPolicy::Daily, 0, 30);  // 保留 30 天
```

### AsyncSink

```cpp
// 异步写入文件
auto file = std::make_unique<Base::FileSink>("logs/async.log");
Base::AsyncSink async(std::move(file), 2048);

async.write(logEvent);     // 非阻塞入队
async.flush();             // 等待队列清空
async.stop();              // 优雅关闭
```

### 自定义格式化器

```cpp
class JsonFormatter : public Base::LogFormatter {
    std::string format(const Base::LogEvent& event) override {
        return R"({"level":")" + std::string(logLevelToString(event.level)) +
               R"(","message":")" + event.message + R"("})";
    }
};

auto file = std::make_unique<Base::FileSink>("logs/app.json");
file->setFormatter(std::make_unique<JsonFormatter>());
```

## 线程安全性

| 组件 | 线程安全性 |
|------|-----------|
| `ConsoleSink` | `std::mutex` 保护 write/flush |
| `FileSink` | `std::mutex` 保护所有操作 |
| `RollingFileSink` | `std::mutex` 保护 write/flush/roll |
| `AsyncSink` | 内部队列由 `std::mutex` + `std::condition_variable` 保护 |
| 格式化器 | 无状态/不可变，天然线程安全 |

## 依赖

- `LogCommon.h`：日志基础类型
- C++20 标准库（`<fstream>`、`<queue>`、`<condition_variable>`）
- `<filesystem>`：文件系统操作
