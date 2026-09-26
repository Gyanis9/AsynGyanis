---
name: 缺陷报告
about: 报告行为不正确、崩溃、泄漏、告警或性能回归
title: "[Bug] "
labels: bug
---

<!-- 安全类问题不要走这里，见 .github/SECURITY.md。 -->

## 现象

一句话说明「预期是什么、实际是什么」。

## 环境与配置

- 提交：`git rev-parse --short HEAD`
- 平台与编译器版本：
- 构建配置：Debug / Release，`CMAKE_BUILD_TYPE`，是否开 `ASYN_WITH_IO_URING` / `ASYN_WITH_MIMALLOC` / sanitizer
- 线程数与 worker 配置（如果是服务器侧问题）：

## 复现步骤

1.
2.
3.

优先给**能一次性跑完的复现体**：一条最小用例、一段 `benchmarks/` 里的客户端脚本、或原始字节。
「偶发，跑十次挂一次」这类请一并说明跑了多少轮、挂了几次。

## 实际输出

贴上完整的报错文本与日志（保留原始中文，不要转述）。崩溃请附调用栈。

## 已尝试的排查

- 是否在容器 `ubuntu24` 的 ASan/LSan/UBSan 下复跑过：
- 是否在 Windows Debug（MSVC `/W4 /WX` + ASan）下复跑过：
- 相关用例名（`ctest -R <正则>` 能选中的那种）：

## 影响面

挡住你了还是只是难看？有没有可绕过的路子？
