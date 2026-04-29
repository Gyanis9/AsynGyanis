# ConfigFileWatcher 模块接口文档

## 概述

`ConfigFileWatcher` 模块提供了跨平台的文件变更监听功能。采用策略模式，根据编译平台自动选择最优实现：Linux 上使用 `inotify`，Windows 上使用 `ReadDirectoryChangesW`，其他平台回退到轮询方式。该模块是配置热加载功能的基础组件。

## 架构设计

```
IFileWatcher (抽象接口)
  ├── InotifyFileWatcher (Linux, inotify 内核机制)
  ├── Win32FileWatcher  (Windows, ReadDirectoryChangesW API)
  └── PollingFileWatcher (通用回退, stat 轮询)

FileWatcherFactory (工厂类)
  └── create() → std::unique_ptr<IFileWatcher>
```

## 核心类型

### FileChangeEvent 枚举

| 枚举值 | 整数值 | 说明 |
|--------|--------|------|
| `Modified` | 0 | 文件内容被修改 |
| `Created` | 1 | 文件被创建 |
| `Deleted` | 2 | 文件被删除 |
| `Moved` | 3 | 文件被移动或重命名 |

### FileChangeCallback

```cpp
using FileChangeCallback = std::function<void(std::string_view file_path, FileChangeEvent event)>;
```

回调函数类型，接收变更文件路径和变更事件类型。

## IFileWatcher 抽象接口

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `start()` | `bool` | 启动监听线程。成功返回 `true`，已运行返回 `true` |
| `stop()` | `void` | 停止监听线程，阻塞等待线程结束 |
| `addWatch(path, recursive)` | `bool` | 添加监听路径。路径不存在返回 `false`；已监听返回 `true` |
| `removeWatch(path)` | `bool` | 移除监听路径。路径不存在返回 `false` |
| `setCallback(callback)` | `void` | 设置文件变更回调函数 |
| `isRunning()` | `bool` | 查询监听器运行状态 |

## FileWatcherFactory

```cpp
static std::unique_ptr<IFileWatcher> create();
```

**功能说明**：根据当前编译平台创建对应的文件监听器实例。无需手动判断平台宏。

**返回值**：
| 平台 | 实现类 |
|------|--------|
| Linux (`__linux__`) | `InotifyFileWatcher` |
| Windows (`_WIN32`) | `Win32FileWatcher` |
| 其他 | `PollingFileWatcher` |

## Linux 实现：InotifyFileWatcher

### 特性

- 基于 Linux 内核 `inotify` 机制，高效且低开销
- 监听 `IN_CLOSE_WRITE` 事件确保文件写入完成后再触发回调
- 监听 `IN_MOVED_TO` 事件支持 vim 等编辑器的原子保存模式
- 独立监听线程，不阻塞调用者
- 内置防抖机制，默认 100ms

### 构造函数

```cpp
InotifyFileWatcher();
```

初始化 `inotify` 文件描述符。失败时抛出 `std::runtime_error`。

### 特有方法

```cpp
void setDebounceInterval(std::chrono::milliseconds interval) noexcept;
```

设置事件防抖时间间隔（默认为 100ms）。同一文件在防抖间隔内的连续变更只会触发一次回调。

### 监听的事件掩码

```
IN_CLOSE_WRITE  |  IN_MOVED_TO  |  IN_DELETE_SELF  |  IN_MOVE_SELF
```

## Windows 实现：Win32FileWatcher

### 特性

- 基于 Windows `ReadDirectoryChangesW` API
- 监听目录级别的文件修改、创建、删除、重命名事件
- 异步重叠 I/O 模式，不阻塞调用线程
- 支持 UTF-8 路径自动转换为 UTF-16（`toWideString` 内部转换）

### 构造与配置

```cpp
Win32FileWatcher();
void setDebounceInterval(std::chrono::milliseconds interval) noexcept;
```

## 回退实现：PollingFileWatcher

### 特性

- 基于 `std::filesystem::last_write_time` 的通用轮询实现
- 可在任何支持 C++17 文件系统库的平台上运行
- 默认轮询间隔 1000ms，可通过 `setPollInterval()` 调整
- 不依赖操作系统特定 API

### 特有方法

```cpp
void setPollInterval(std::chrono::milliseconds interval) noexcept;
```

## 使用示例

```cpp
#include "Base/ConfigFileWatcher.h"

// 创建监听器
auto watcher = Base::FileWatcherFactory::create();

// 设置回调
watcher->setCallback([](std::string_view path, Base::FileChangeEvent event) {
    std::cout << "文件变更: " << path << std::endl;
});

// 添加监听路径
watcher->addWatch("/etc/myapp/config", true);   // 递归监听
watcher->addWatch("/etc/myapp/secrets.yaml");   // 监听单个文件

// 启动监听
watcher->start();

// 在主线程中执行其他工作...

// 停止监听
watcher->stop();
```

## 线程安全性

| 方法 | 线程安全性 |
|------|-----------|
| `start()` / `stop()` | 线程安全（原子标志保护） |
| `addWatch()` / `removeWatch()` | 仅允许在 `start()` 之前调用 |
| `setCallback()` | 仅允许在 `start()` 之前设置 |
| `isRunning()` | 完全线程安全 |
| 回调函数 | 在监听线程中执行，需自行处理线程安全 |

## 生命周期约定

1. 创建 → `addWatch` → `setCallback` → `start` → 运行 → `stop` → 销毁
2. 析构函数自动调用 `stop()`，确保线程安全退出
3. 严禁在回调函数中调用 `stop()` 或 `addWatch()`/`removeWatch()`

## 性能特征

| 实现 | 事件延迟 | CPU 开销 | 适用场景 |
|------|---------|---------|---------|
| inotify | < 10ms | 极低 | Linux 生产环境 |
| ReadDirectoryChangesW | < 50ms | 低 | Windows 生产环境 |
| Polling | 取决于 `poll_interval` | 中 | 开发/回退环境 |

## 依赖

- C++20 标准库（`<filesystem>`、`<chrono>`、`<thread>`、`<atomic>`）
- Linux：`<sys/inotify.h>`、`<poll.h>`
- Windows：`<windows.h>`
