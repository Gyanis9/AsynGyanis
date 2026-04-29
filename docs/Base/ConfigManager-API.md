# ConfigManager 模块接口文档

## 概述

`ConfigManager` 是配置系统的核心管理器组件。提供从 YAML 文件目录递归加载配置、线程安全的读写访问、热加载支持以及类型安全的配置值访问等功能。采用 Meyers' Singleton 模式确保全局唯一实例，使用 `std::atomic<std::shared_ptr<>>` 实现无锁读取。

## 核心架构

```
ConfigManager (单例)
  ├── 配置加载: loadFromDirectory / loadFiles / reload
  ├── 热加载: enableHotReload / disableHotReload
  ├── 配置访问: get / getOptional / getRequired / get<T>
  ├── 类型访问器: getBool / getInt / getDouble / getString
  ├── 查询: has / keys / dump / loadedFiles / configDirectory
  └── 验证: validateRequired
```

内部使用原子 `shared_ptr<ConfigData>` 实现配置快照的无锁热替换，读取操作完全无锁。

## 数据模型

### ConfigLoadResult

```cpp
struct ConfigLoadResult {
    bool success;
    std::vector<std::string> loaded_files;   // 成功加载的文件路径
    std::vector<std::string> failed_files;   // 加载失败的文件路径
    std::vector<std::string> errors;         // 错误信息列表
    std::chrono::steady_clock::time_point timestamp;  // 加载时间戳
    explicit operator bool() const noexcept;  // 等价于 success
};
```

### HotReloadCallback

```cpp
using HotReloadCallback = std::function<void(const ConfigLoadResult& result)>;
```

## API 参考

### 1. 单例访问

```cpp
static ConfigManager& instance() noexcept;
```

**说明**：获取全局唯一的配置管理器实例。线程安全的 Meyers' Singleton 实现。

---

### 2. 配置加载

#### loadFromDirectory

```cpp
ConfigLoadResult loadFromDirectory(const std::filesystem::path& config_dir,
                                   bool recursive = true);
```

**功能**：从指定目录递归扫描并加载所有 `.yaml` 和 `.yml` 文件。

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `config_dir` | `const std::filesystem::path&` | — | 配置目录路径 |
| `recursive` | `bool` | `true` | 是否递归扫描子目录 |

**返回值**：`ConfigLoadResult`，包含加载结果详情。

**错误处理**：
- 目录不存在：`success=false`，`errors` 包含原因
- 路径不是目录：`success=false`
- 空目录：`success=true`，`loaded_files` 为空
- YAML 解析错误：记录在 `errors` 中，不影响其他文件加载

#### loadFiles

```cpp
ConfigLoadResult loadFiles(const std::vector<std::filesystem::path>& file_paths);
```

**功能**：加载指定文件列表中的配置文件。

**参数**：`file_paths` — 配置文件路径列表。空列表直接返回 `success=true`。

#### reload

```cpp
ConfigLoadResult reload();
```

**功能**：使用当前配置目录重新加载所有配置。

**前置条件**：必须已调用 `loadFromDirectory()` 设置配置目录，否则返回 `success=false`。

---

### 3. 配置访问

#### 泛型 get<T>

```cpp
template<typename T>
T get(std::string_view key, T&& default_value) const noexcept;
```

**功能**：获取指定类型的配置值，键不存在或类型不匹配时返回默认值。

```cpp
auto port = cfg.get<int64_t>("server.port", 8080);
auto name = cfg.get<std::string>("app.name", "default");
```

#### get (返回 ConfigValue)

```cpp
ConfigValue get(std::string_view key) const;
```

**功能**：获取原始 `ConfigValue`，键不存在时抛出 `ConfigKeyNotFoundException`。

#### getOptional

```cpp
std::optional<ConfigValue> getOptional(std::string_view key) const noexcept;
```

**功能**：安全获取配置值，键不存在返回 `std::nullopt`。

#### getRequired

```cpp
template<typename T>
T getRequired(std::string_view key) const;
```

**功能**：获取必需配置值。键不存在抛出 `ConfigKeyNotFoundException`，类型不匹配抛出 `ConfigTypeException`。

#### 便捷类型访问器

| 方法 | 返回类型 | 默认值 | 说明 |
|------|---------|--------|------|
| `getBool(key, default)` | `bool` | `false` | 获取布尔值 |
| `getInt(key, default)` | `int64_t` | `0` | 获取整型值 |
| `getDouble(key, default)` | `double` | `0.0` | 获取浮点值 |
| `getString(key, default)` | `std::string` | `""` | 获取字符串值 |

---

### 4. 配置查询

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `has(key)` | `bool` | 检查配置键是否存在 |
| `keys()` | `std::vector<std::string>` | 返回所有配置键（字典序排序） |
| `dump()` | `std::unordered_map<std::string, ConfigValue>` | 导出当前配置快照副本 |
| `loadedFiles()` | `std::vector<std::string>` | 返回已加载文件路径列表 |
| `configDirectory()` | `std::filesystem::path` | 返回当前配置目录 |
| `clear()` | `void` | 清空所有配置数据 |

---

### 5. 热加载

#### enableHotReload

```cpp
bool enableHotReload(HotReloadCallback callback = nullptr,
                     std::chrono::milliseconds debounce_ms = 500ms);
```

**功能**：启用文件变更监听，配置变更时自动重载。

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `callback` | `HotReloadCallback` | `nullptr` | 重载完成回调 |
| `debounce_ms` | `std::chrono::milliseconds` | `500ms` | 防抖延迟 |

**返回值**：`true` 表示启用成功；`false` 表示失败（未设置配置目录或不支持的平台）。

**实现说明**：使用 `ConfigFileWatcher` 监听配置文件变更，在防抖间隔后异步执行重载。重载在独立线程中执行，不阻塞监听线程。

#### disableHotReload

```cpp
void disableHotReload();
```

**功能**：关闭热加载监听并释放相关资源。幂等操作。

#### isHotReloadEnabled

```cpp
bool isHotReloadEnabled() const noexcept;
```

---

### 6. 验证

```cpp
std::vector<std::string> validateRequired(const std::vector<std::string>& required_keys) const;
```

**功能**：校验必需配置键是否全部存在，返回缺失键列表。

## 线程安全性

| 操作 | 线程安全性 |
|------|-----------|
| 读取操作（`get`/`has`/`keys` 等） | 完全无锁，多线程安全 |
| `loadFromDirectory`/`reload` | 使用 `std::shared_mutex` 保护写入 |
| `enableHotReload`/`disableHotReload` | 原子操作保护 |
| `clear()` | 原子替换 |

## 使用示例

### 基础使用

```cpp
#include "Base/ConfigManager.h"

auto& cfg = Base::ConfigManager::instance();

// 加载配置
auto result = cfg.loadFromDirectory("./config");
if (!result) {
    for (const auto& err : result.errors) {
        std::cerr << "配置错误: " << err << std::endl;
    }
    return -1;
}

// 读取配置
auto host = cfg.get<std::string>("server.host", "0.0.0.0");
auto port = cfg.get<int64_t>("server.port", 8080);
auto debug = cfg.getBool("debug.enabled", false);

// 验证必需配置
auto missing = cfg.validateRequired({"database.url", "database.username"});
if (!missing.empty()) {
    std::cerr << "缺少配置项: ";
    for (auto& k : missing) std::cerr << k << " ";
    std::cerr << std::endl;
}
```

### 热加载

```cpp
cfg.enableHotReload([](const Base::ConfigLoadResult& result) {
    if (result.success) {
        std::cout << "配置已热更新，加载 "
                  << result.loaded_files.size() << " 个文件" << std::endl;
    } else {
        std::cerr << "配置热更新失败:" << std::endl;
        for (auto& err : result.errors) std::cerr << "  " << err << std::endl;
    }
});
```

## 依赖

- `ConfigValue.h`：配置值类型
- `ConfigFileWatcher.h`：文件监听器（热加载依赖）
- `yaml-cpp`：YAML 文件解析
- C++20 标准库
