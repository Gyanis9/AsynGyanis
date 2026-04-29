# LoggerConfig 模块接口文档

## 概述

`LoggerConfig` 模块提供从配置系统（`ConfigManager`）加载日志配置的功能。允许通过 YAML 配置文件集中管理所有日志器的级别和输出目标（Sink）配置，无需硬编码。

## YAML 配置结构

```yaml
logging:                        # 配置根键（可自定义）
  global_level: INFO            # 全局默认日志级别
  loggers:                      # 日志器配置字典
    root:                       # 根日志器
      level: DEBUG
      sinks:
        - type: console
          color: true
    network:                    # 自定义日志器
      level: TRACE
      sinks:
        - type: rolling_file
          base_filename: network.log
          directory: logs
          policy: daily
          max_backup: 7
    database:                   # 带异步的日志器
      level: INFO
      sinks:
        - type: async
          queue_size: 2048
          overflow_policy: block
          wrapped:
            type: file
            path: logs/db.log
            level: WARN
```

## LoggerConfigLoader 类

### loadFromConfig

```cpp
static void loadFromConfig(const std::string& config_prefix = "logging");
```

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `config_prefix` | `const std::string&` | `"logging"` | 配置根键前缀 |

**功能**：从 `ConfigManager` 读取配置并初始化日志系统。行为如下：

1. 读取 `<prefix>.global_level` 确定默认日志级别（缺失默认 `INFO`）
2. 扫描 `<prefix>.loggers.*` 发现所有日志器名称
3. 若无 `loggers` 配置，则创建默认 root logger（`ConsoleSink` + 色）
4. 对每个日志器：设置级别 → 清空旧 Sink → 创建新 Sink
5. 配置优先级：logger 级别 > `global_level`；Sink 级别 > logger 级别

## 支持的 Sink 类型

### console

```yaml
- type: console
  color: true          # 可选，默认 true。是否启用彩色输出
  level: INFO          # 可选，Sink 级别过滤
```

### file

```yaml
- type: file
  path: logs/app.log   # 必需，日志文件路径
  truncate: false      # 可选，默认 false。是否截断模式
  level: INFO          # 可选，Sink 级别过滤
```

### rolling_file

```yaml
- type: rolling_file
  base_filename: app.log        # 必需，基础文件名
  directory: logs               # 可选，默认 "logs"
  policy: size                  # 可选，默认 "size"。选项: size / daily / hourly
  max_size_mb: 10               # 可选，默认 10 (MB)。仅 size 策略有效
  max_backup: 10                # 可选，默认 10。最大备份文件数
  level: INFO                   # 可选，Sink 级别过滤
```

### async

```yaml
- type: async
  queue_size: 1024              # 可选，默认 1024
  overflow_policy: block        # 可选，默认 "block"。选项: block / drop
  level: INFO                   # 可选，Sink 级别过滤
  wrapped:                      # 必需，被包装的真实 Sink 配置
    type: file
    path: logs/async.log
```

> 注意：`async` 不支持嵌套（`wrapped` 中不能再包含 `async`）。

## 优先级规则

```
Sink 级别 > Logger 级别 > global_level
```

- 每个 `LogEvent` 首先经过 `Logger::shouldLog()` 过滤
- 通过后再由每个 Sink 各自的 `sink->shouldLog()` 过滤
- Sink 的 `level` 键可选择性覆盖其默认级别阈值

## 键格式说明

配置系统使用扁平化键（点号分隔）存储配置。`LoggerConfigLoader` 通过前缀扫描发现日志器配置：

```
# 配置键示例
logging.global_level           → "INFO"
logging.loggers.root.level     → "DEBUG"
logging.loggers.root.sinks     → [{type: file, path: logs/root.log}, ...]
logging.loggers.network.level  → "TRACE"
logging.loggers.database.level → "INFO"
```

因此，当通过 `ConfigManager::keys()` 枚举键时，所有日志器配置以 `logging.loggers.` 为前缀。

## 使用示例

### 基础使用

```cpp
#include "Base/LoggerConfig.h"
#include "Base/ConfigManager.h"

int main() {
    // 1. 加载配置目录
    auto& cfg = Base::ConfigManager::instance();
    cfg.loadFromDirectory("./config");

    // 2. 从配置初始化日志系统
    Base::LoggerConfigLoader::loadFromConfig();

    // 3. 直接使用日志宏
    LOG_INFO("日志系统已初始化");
    LOG_DEBUG_FMT("配置了 {} 个日志器",
                  Base::LoggerRegistry::instance().getLoggerNames().size());
}
```

### 自定义前缀

```yaml
# config/logging.yaml
myapp_logging:
  global_level: ERROR
  loggers:
    root:
      level: WARN
      sinks:
        - type: console
          color: true
```

```cpp
// 使用自定义前缀
Base::LoggerConfigLoader::loadFromConfig("myapp_logging");
```

### 完整生产配置

```yaml
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: console
          color: true
        - type: file
          path: logs/root.log
    network:
      level: DEBUG
      sinks:
        - type: async
          queue_size: 4096
          overflow_policy: block
          wrapped:
            type: rolling_file
            base_filename: network.log
            directory: logs
            policy: hourly
            max_backup: 48
    database:
      level: INFO
      sinks:
        - type: file
          path: logs/database.log
          level: WARN          # 仅记录 WARN 及以上
    security:
      level: TRACE
      sinks:
        - type: rolling_file
          base_filename: security.log
          directory: logs
          policy: daily
          max_backup: 90
```

## 容错行为

| 场景 | 行为 |
|------|------|
| YAML 中无 `logging` 配置 | 创建默认 root logger（控制台、彩色） |
| 无 `loggers` 节 | 创建默认 root logger |
| `global_level` 无效值 | 默认为 `INFO` |
| logger `level` 无效值 | 使用 `global_level` |
| Sink `type` 未知 | 输出警告到 `stderr`，跳过该 Sink |
| `async` 的 `wrapped` 配置缺失 | 跳过该 Sink |
| `rolling_file` 的 `policy` 无效值 | 默认为 `size` |
| 文件路径无效 | `FileSink` 构造函数抛出异常 |

## 线程安全性

`loadFromConfig()` 必须在单线程环境中调用（通常在 `main()` 启动阶段）。之后日志器配置不应再被修改。

## 依赖

- `ConfigManager.h`：配置系统
- `Logger.h`：日志器注册表
- `LogSink.h`：日志 Sink 体系
- C++20 标准库
