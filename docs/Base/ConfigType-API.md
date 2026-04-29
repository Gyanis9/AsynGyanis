# ConfigType 模块接口文档

## 概述

`ConfigType` 模块是配置系统的基础类型定义模块，提供了配置值类型枚举、类型名称转换、YAML 文件识别和键路径拆分等基础功能。该模块位于 `Base` 命名空间中。

## 核心组件

### 1. ConfigValueType 枚举

定义配置值支持的基础类型。

| 枚举值 | 整数值 | 说明 |
|--------|--------|------|
| `Null` | 0 | 空值，表示无数据 |
| `Bool` | 1 | 布尔类型 |
| `Int` | 2 | 64 位有符号整数（int64_t） |
| `Double` | 3 | 双精度浮点类型 |
| `String` | 4 | 字符串类型 |
| `Array` | 5 | 数组类型（元素为 ConfigValue） |
| `Object` | 6 | 对象类型（键为 string，值为 ConfigValue） |

### 2. typeName()

```cpp
const char* typeName(ConfigValueType type) noexcept;
```

**功能说明**：将配置值类型枚举转换为人类可读的字符串表示。

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `type` | `ConfigValueType` | 需要转换的配置值类型枚举 |

**返回值**：
| 类型 | 说明 |
|------|------|
| `const char*` | 类型名称字符串。值为 `"null"`, `"bool"`, `"int"`, `"double"`, `"string"`, `"array"`, `"object"` 之一。无效枚举值返回 `"unknown"` |

**线程安全性**：完全线程安全（无状态函数）。

**使用示例**：
```cpp
const char* name = Base::typeName(ConfigValueType::Int);
// name == "int"

const char* unknown = Base::typeName(static_cast<ConfigValueType>(255));
// unknown == "unknown"
```

### 3. isYamlFile()

```cpp
bool isYamlFile(std::string_view file_path) noexcept;
```

**功能说明**：判断给定文件路径是否具有 YAML 配置文件后缀（`.yaml` 或 `.yml`）。

**参数**：
| 参数名 | 类型 | 说明 |
|--------|------|------|
| `file_path` | `std::string_view` | 待检查的文件路径字符串 |

**返回值**：
| 类型 | 说明 |
|------|------|
| `bool` | 若后缀为 `.yaml` 或 `.yml` 返回 `true`，否则返回 `false` |

**边界行为**：
- 空字符串：返回 `false`
- 路径长度小于 4 字符：返回 `false`
- 精确匹配后缀：不区分大小写比较
- 仅扩展名（如 `.yaml`）：返回 `true`

**使用示例**：
```cpp
Base::isYamlFile("/etc/config/app.yaml");   // true
Base::isYamlFile("/etc/config/app.yml");    // true
Base::isYamlFile("/etc/config/app.json");   // false
Base::isYamlFile("");                       // false
```

### 4. splitKey()

```cpp
std::vector<std::string> splitKey(std::string_view key, char delimiter = '.');
```

**功能说明**：使用指定分隔符将配置键字符串拆分为路径片段列表。空片段（由连续分隔符产生）会被自动跳过。

**参数**：
| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `key` | `std::string_view` | — | 待拆分的键字符串 |
| `delimiter` | `char` | `'.'` | 分隔字符 |

**返回值**：
| 类型 | 说明 |
|------|------|
| `std::vector<std::string>` | 按分隔符拆分后的字符串片段列表 |

**边界行为**：
- 空字符串：返回空 vector
- 连续分隔符（如 `"a..b"`）：空片段被跳过，结果为 `["a", "b"]`
- 首部分隔符（如 `".leading"`）：跳过空前缀，结果为 `["leading"]`
- 末尾分隔符（如 `"trailing."`）：跳过空后缀，结果为 `["trailing"]`

**使用示例**：
```cpp
auto parts = Base::splitKey("server.host.port");
// parts == ["server", "host", "port"]

auto single = Base::splitKey("single");
// single == ["single"]

auto path = Base::splitKey("a/b/c", '/');
// path == ["a", "b", "c"]
```

## 性能特征

| 函数 | 时间复杂度 | 空间复杂度 |
|------|-----------|------------|
| `typeName()` | O(1) | O(1) |
| `isYamlFile()` | O(1) | O(1) |
| `splitKey()` | O(n) | O(k)（k 为分段数） |

## 依赖

该模块无外部依赖，仅使用 C++20 标准库。
