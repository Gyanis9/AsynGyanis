# BufferPool 模块接口文档

## 概述

`BufferPool` 是固定大小字节缓冲池。预分配 `bufferCount` 个 `bufferSize` 字节的缓冲区，通过 acquire/release 管理。线性扫描分配（O(n)），适用于中等数量的固定大小缓冲区。

## 核心类

### BufferPool

**头文件**：`BufferPool.h` | **实现**：`BufferPool.cpp`

### 构造函数

```cpp
BufferPool(size_t bufferSize, size_t bufferCount);
```

### 方法

| 方法 | 说明 |
|------|------|
| `acquire()` | 获取空闲缓冲区索引，满时返回 -1 |
| `release(index)` | 释放缓冲区 |
| `data(index)` | 获取缓冲区指针 |
| `bufferSize()` | 单个缓冲区大小 |
| `bufferCount()` | 缓冲区总数 |

### 使用示例

```cpp
Core::BufferPool pool(4096, 16);
int idx = pool.acquire();
void* buf = pool.data(idx);
// ... 使用 buf ...
pool.release(idx);
```
