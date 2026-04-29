# CoroutinePool 模块接口文档

## 概述

`CoroutinePool` 是 thread-local 协程帧内存池。使用 slab 分配策略：预分配大块内存，通过自由链表分配固定大小块。超过 `blockSize` 的分配请求回退到全局 `operator new`。

## 核心类

### CoroutinePool

**头文件**：`CoroutinePool.h` | **实现**：`CoroutinePool.cpp`

### 单例访问

```cpp
static CoroutinePool& instance();
```

返回 thread_local 单例。每个线程有独立的内存池。

### 构造函数

```cpp
explicit CoroutinePool(size_t blockSize = 256, size_t initialBlocks = 64);
```

### 分配/释放

| 方法 | 说明 |
|------|------|
| `allocate(n)` | 分配 n 字节。n > blockSize 时回退到 `::operator new` |
| `deallocate(p, n)` | 释放内存。n > blockSize 时回退到 `::operator delete` |

### 查询

| 方法 | 说明 |
|------|------|
| `blockSize()` | 配置的块大小 |
| `allocatedCount()` | 当前从池中分配的块数 |
