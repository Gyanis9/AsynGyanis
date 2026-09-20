# 贡献指南

感谢你考虑为 AsynGyanis 做出贡献。

## 开发环境

- **编译器**: MSVC ≥ 19.40 / GCC ≥ 13 / Clang ≥ 17（需支持 C++23 标准）
- **构建工具**: CMake >= 3.20, Conan >= 2.0, Ninja（推荐）
- **平台**: Windows ≥ 10（事件后端为完成端口）或 Linux kernel >= 3.9（epoll；`ASYN_WITH_IO_URING` 可换 io_uring，需内核 5.6+）

```bash
# 初始化开发环境（Linux；Windows 侧 pip install conan 后同样可用）
python3 -m venv .venv
.venv/bin/pip install conan
.venv/bin/conan profile detect --force

# Debug 构建（含 ASan/UBSan）
cmake --preset debug
cmake --build build/debug -j$(nproc)
cd build/debug && ctest --output-on-failure

# Release 构建（开优化，并带上能解析调用栈的调试信息）
cmake --preset release
cmake --build build/release -j$(nproc)
```

```bat
:: Windows（需在已加载 vcvars64 的环境中执行）
pip install conan
conan profile detect --force
cmake --preset debug
cmake --build build/debug -j 14
ctest --test-dir build/debug --output-on-failure
```

第三方依赖由 `conan_provider.cmake` 在配置阶段自动安装（`conan install --build=missing`），无需手工执行。

## 开发流程

1. **Fork & Clone**: 从 `main` 分支创建功能分支
2. **编码**: 新增代码对齐 `.clang-format` 与周围既有风格。仓库从未整体格式化过，**别对既有文件整文件跑 `clang-format -i`**：一刀切会改出几百个文件的无关 diff，把真正的改动淹掉
3. **静态检查**: 运行 `clang-tidy -p build/debug src/<changed-file>`
4. **测试**: 确保 `ctest --output-on-failure` 全部通过
5. **提交**: 约定式提交，类型英文小写、描述与正文中文（见下文「提交规范」）
6. **PR**: 提交 Pull Request，填写模板，等待审核

## 提交规范

格式为约定式提交：`<类型>[可选 范围][!]: <描述>`。类型前缀英文小写，描述与正文用中文；正文讲「为什么」而不是复述 diff，一次提交只讲一件事。

- `feat`: 新功能（涨次版本）
- `fix`: Bug 修复（涨修订号）
- `perf`: 性能优化
- `refactor`: 重构
- `build`: 构建/依赖/打包
- `ci`: CI/CD 作业
- `docs`: 文档
- `test`: 测试
- `style`: 不影响语义的格式调整
- `chore`: 其余杂项
- `revert`: 回滚某次提交（正文须引用被回滚的提交）

破坏性变更二选一（也可并用）：类型后加 `!`（如 `feat!:`），或正文写 `BREAKING CHANGE: <说明>` 脚注；两者都会涨主版本。版本号与 CHANGELOG 的一致性由 `scripts/check-release-version.py` 把关。

## 代码风格

- 注释遵循 C++23 编码规范：文件头块（`@file`/`@brief`/`@author`/`@date`/`@version`/`@copyright`）**只写在 `.h` 上**，`.cpp` 不写文件头；类与公开方法写完整中文 Doxygen（`@param`/`@return`），成员变量行尾用 `///<`
- 子类每个 `override` 必须独立书写完整中文注释，`@details` 说明与父类的行为差异，禁止「同上/继承自父类」占位
- 优先使用 RAII 管理资源
- 协程接口使用 `AsynGyanis::Core::Task<T>` 返回类型
- 异常使用 `AsynGyanis::Base` 下的异常体系（`Base/Exception/`）
- 日志使用 `LOG_*_FMT` 宏（`Base/Log/LogMacros.h`）
- 平台相关操作一律封装在 `AsynGyanis::Platform`，Base 及以上模块不出现平台宏与系统 API

## 模块架构

```
src/Platform/ — 平台底层封装（OS 调用的唯一出处）：IO / FileSystem / System，Linux 与 Windows 分别实现
src/Base/     — 基础设施：Log（日志）、Config（配置）、Exception（异常）
src/Core/     — 异步运行时：EventLoop（三后端 + IoWatcher + TimerQueue）、Coroutine、Socket、Tls、Process、Exception
src/Net/      — 网络应用层：Tcp / Http（含 Client）/ Http2 / Http3 / Quic / WebSocket
src/Database/ — 数据访问：Common / Dialect / Pool / Queryable / Sqlite / MySql / Redis
tests/        — 单元测试（GoogleTest），目录与 src 逐级对齐
samples/      — 示例程序（echo_server，随构建编译）
```

命名空间一律到模块名为止（`AsynGyanis::Base`、`AsynGyanis::Core` …），子目录不引入新命名空间；include 路径从 `src/` 起算（`#include "Core/EventLoop/EventLoop.h"`）。模块分层与链接依赖见 README 的「架构」一节。

## 行为准则

本项目遵循 [Contributor Covenant](https://www.contributor-covenant.org/)。
