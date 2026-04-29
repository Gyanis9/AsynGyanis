# ConnectionManager 模块接口文档

## 概述

`ConnectionManager` 是全局连接追踪器，管理所有活跃连接的 `shared_ptr<Connection>`。支持优雅关闭（批量请求停止）和阻塞等待所有连接完成。

## 核心类

### ConnectionManager

**头文件**：`ConnectionManager.h` | **实现**：`ConnectionManager.cpp`

**内部状态**：`unordered_set<shared_ptr<Connection>>` + `shared_mutex` + `condition_variable_any`

### 方法

| 方法 | 说明 |
|------|------|
| `add(conn)` | 添加连接到集合（const ref 避免移动） |
| `remove(conn*)` | 通过原始指针查找并移除，通知条件变量 |
| `activeCount()` | 当前活跃连接数（共享锁） |
| `shutdown()` | 对所有连接调用 `cancelable().requestStop()` |
| `waitAll()` | 阻塞直到集合为空 |

### 线程安全

- `add`/`remove` 使用独占锁
- `activeCount`/`shutdown` 使用共享锁
- `waitAll` 使用共享锁 + 条件变量
