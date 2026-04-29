# Cancelable 模块接口文档

## 概述

`Cancelable` 是基于 `std::stop_source` 的协作取消混入类。提供请求停止、查询停止状态和获取 `stop_token` 的接口。被 `Connection` 类继承使用。

## 核心类

### Cancelable

**头文件**：`Cancelable.h`（Header-only）

### 方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `requestStop()` | `bool` | 请求停止，幂等 |
| `isStopRequested()` | `bool` | 是否已请求停止 |
| `stopToken()` | `std::stop_token` | 获取停止令牌（用于轮询/回调） |
| `stopSource()` | `std::stop_source&` | 获取底层 stop_source 引用 |

### 使用示例

```cpp
Core::Cancelable c;
c.requestStop();
assert(c.isStopRequested());
auto token = c.stopToken();
assert(token.stop_requested());
```
