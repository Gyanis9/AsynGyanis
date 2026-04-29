# ConfigValue 模块接口文档

## 概述

`ConfigValue` 模块是配置系统的核心数据容器，基于 `std::variant` 提供了类型安全的配置值封装。支持七种基础类型（Null、Bool、Int、Double、String、Array、Object），并提供强弱两种类型访问方式以及嵌套结构的便利操作。

## 支持的类型

| ConfigValueType | C++ 类型 | variant 索引 | 说明 |
|-----------------|----------|-------------|------|
| `Null` | `std::nullptr_t` | 0 | 空值 |
| `Bool` | `bool` | 1 | 布尔值 |
| `Int` | `int64_t` | 2 | 64 位有符号整数 |
| `Double` | `double` | 3 | 双精度浮点数 |
| `String` | `std::string` | 4 | 字符串 |
| `Array` | `ConfigArray`（`std::vector<ConfigValue>`） | 5 | 配置值数组 |
| `Object` | `ConfigObject`（`std::map<std::string, ConfigValue, std::less<>>`） | 6 | 配置对象（支持异构查找） |

## 构造函数

### 默认构造与各类型构造

```cpp
ConfigValue() noexcept;                         // 构造 Null
explicit ConfigValue(std::nullptr_t) noexcept;  // 构造 Null
explicit ConfigValue(bool v) noexcept;          // 构造 Bool
explicit ConfigValue(int v) noexcept;           // 构造 Int（int → int64_t）
explicit ConfigValue(int64_t v) noexcept;       // 构造 Int
explicit ConfigValue(double v) noexcept;        // 构造 Double
explicit ConfigValue(const char* v);            // 构造 String
explicit ConfigValue(std::string v) noexcept;   // 构造 String
explicit ConfigValue(ConfigArray v) noexcept;   // 构造 Array
explicit ConfigValue(ConfigObject v) noexcept;  // 构造 Object
```

**注意事项**：
- `int` 构造函数自动提升为 `int64_t`
- `const char*` 构造函数非 `noexcept`（需分配 `std::string`）
- `std::string` 构造函数使用移动语义
- 所有构造函数均为 `explicit`，需显式转换

### 拷贝与移动

```cpp
ConfigValue(const ConfigValue&) = default;            // 拷贝构造
ConfigValue(ConfigValue&&) noexcept = default;         // 移动构造
ConfigValue& operator=(const ConfigValue&) = default;  // 拷贝赋值
ConfigValue& operator=(ConfigValue&&) noexcept = default; // 移动赋值
```

## 类型查询接口

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `type()` | `ConfigValueType` | 获取当前值的类型枚举 |
| `is<T>()` | `bool` | 检查是否为指定类型 |
| `isNull()` | `bool` | 检查是否为 Null |
| `empty()` | `bool` | 判断值是否为空（Null、空字符串、空数组、空对象返回 true） |

```cpp
ConfigValue v(42);
v.type();           // ConfigValueType::Int
v.is<int64_t>();    // true
v.isNull();         // false
v.empty();          // false

ConfigValue empty_str(std::string(""));
empty_str.empty();  // true
```

## 强类型访问（as 系列）

类型不匹配时抛出 `ConfigTypeException`。

### 模板方法

```cpp
template<typename T>
const T& as() const;  // 获取常量引用，类型不匹配时抛出异常

template<typename T>
T& as();              // 获取可变引用，类型不匹配时抛出异常
```

### 具名方法

| 方法 | 返回类型 | 抛出条件 |
|------|---------|---------|
| `asBool()` | `bool` | 非 Bool 类型 |
| `asInt()` | `int64_t` | 非 Int 类型 |
| `asDouble()` | `double` | 非 Double 类型 |
| `asString()` | `const std::string&` | 非 String 类型 |
| `asArray()` | `const ConfigArray&` | 非 Array 类型 |
| `asObject()` | `const ConfigObject&` | 非 Object 类型 |

```cpp
ConfigValue v("hello");
v.as<std::string>();  // "hello"
v.asString();         // "hello"
v.asInt();            // 抛出 ConfigTypeException
```

## 安全访问（get 系列）

类型不匹配或键不存在时返回 `std::nullopt`，不抛出异常。

### 模板方法

```cpp
template<typename T>
std::optional<T> get() const noexcept;
```

### 具名方法

| 方法 | 返回类型 |
|------|---------|
| `getBool()` | `std::optional<bool>` |
| `getInt()` | `std::optional<int64_t>` |
| `getDouble()` | `std::optional<double>` |
| `getString()` | `std::optional<std::string>` |
| `getArray()` | `std::optional<ConfigArray>` |
| `getObject()` | `std::optional<ConfigObject>` |

```cpp
ConfigValue v(42);
auto val = v.getInt();    // std::optional<int64_t>{42}
auto str = v.getString(); // std::nullopt
```

## 带默认值访问（Or 系列）

类型不匹配时返回默认值，不抛出异常。

| 方法 | 返回类型 | 默认值参数 |
|------|---------|-----------|
| `valueOr(T&& default)` | `std::decay_t<T>` | 泛型默认值 |
| `boolOr(bool default)` | `bool` | `default` |
| `intOr(int64_t default)` | `int64_t` | `default` |
| `doubleOr(double default)` | `double` | `default` |
| `stringOr(const std::string& default)` | `std::string` | `default` |

```cpp
ConfigValue v("text");
int64_t result = v.intOr(100);  // 返回 100（类型不匹配，使用默认值）
```

## 嵌套对象和数组访问

### 对象访问

```cpp
// 检查键是否存在（仅 Object 类型有效）
bool contains(std::string_view key) const noexcept;

// 通过键访问成员（键不存在抛出 ConfigKeyNotFoundException）
const ConfigValue& operator[](std::string_view key) const;

// 安全按引用访问
std::optional<std::reference_wrapper<const ConfigValue>> get(std::string_view key) const noexcept;

// 类型安全访问
template<typename T>
std::optional<T> get(std::string_view key) const noexcept;
```

### 数组访问

```cpp
// 通过索引访问元素（越界抛出 ConfigKeyNotFoundException）
const ConfigValue& operator[](size_t index) const;

// 获取语义大小
size_t size() const noexcept;
// String: 返回字符串长度
// Array:  返回元素个数
// Object: 返回键值对数量
// 其他类型: 返回 0
```

### 底层 variant 访问

```cpp
const VariantType& variant() const noexcept;  // 获取常量引用
VariantType& variant() noexcept;              // 获取可变引用
```

## 使用示例

### 基本类型构造与访问

```cpp
ConfigValue port(8080);
ConfigValue host(std::string("localhost"));
ConfigValue debug(true);
ConfigValue pi(3.14159);

if (auto p = port.getInt()) {
    std::cout << "Port: " << *p << std::endl;
}
```

### 数组操作

```cpp
ConfigArray arr;
arr.push_back(ConfigValue(1));
arr.push_back(ConfigValue("two"));
arr.push_back(ConfigValue(true));

ConfigValue v(std::move(arr));
for (size_t i = 0; i < v.size(); ++i) {
    std::cout << "[" << i << "]: " << typeName(v[i].type()) << std::endl;
}
```

### 嵌套对象操作

```cpp
ConfigObject inner;
inner["host"] = ConfigValue(std::string("db.local"));
inner["port"] = ConfigValue(3306);

ConfigObject outer;
outer["database"] = ConfigValue(std::move(inner));

ConfigValue config(std::move(outer));

// 链式访问
auto db_host = config["database"]["host"].asString();  // "db.local"

// 安全访问
if (auto port = config.get<int64_t>("database.port")) {
    std::cout << "Port: " << *port << std::endl;
}
```

## 性能特征

- `type()`：O(1)，variant 索引访问
- `is<T>()`：O(1)，`std::holds_alternative`
- `as<T>()` / `get<T>()`：O(1)，variant 访问
- `operator[](key)`：O(log n)，有序 map 查找
- `operator[](index)`：O(1)，vector 下标访问
- `size()`：O(1)
- 拷贝/移动：依赖底层 variant 的拷贝/移动开销

## 依赖

- `ConfigType.h`：类型枚举和工具函数
- `Expection.h`：异常类
- C++20 标准库（`<variant>`、`<optional>`、`<functional>`、`<map>`）
