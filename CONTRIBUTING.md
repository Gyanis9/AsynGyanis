# 贡献指南

感谢你考虑为 AsynGyanis 做出贡献。

## 开发环境

- **编译器**: GCC 13+ 或 Clang 17+（需支持 C++20 协程）
- **构建工具**: CMake >= 3.20, Conan >= 2.0, Ninja（推荐）
- **平台**: Linux kernel >= 3.9（epoll 支持）

```bash
# 初始化开发环境
python3 -m venv .venv
.venv/bin/pip install conan
.venv/bin/conan profile detect --force

# Debug 构建（含 ASan/UBSan）
cmake --preset debug
cmake --build build/debug -j$(nproc)
cd build/debug && ctest --output-on-failure

# Release 构建（含 LTO）
cmake --preset release
cmake --build build/release -j$(nproc)
```

## 开发流程

1. **Fork & Clone**: 从 `main` 分支创建功能分支
2. **编码**: 遵循 `.clang-format` 风格（`clang-format -i src/**/*.{cpp,h}`）
3. **静态检查**: 运行 `clang-tidy -p build/debug src/<changed-file>`
4. **测试**: 确保 `ctest --output-on-failure` 全部通过
5. **提交**: 使用中文提交信息，格式为 `<类型>: <简短描述>`
6. **PR**: 提交 Pull Request，填写模板，等待审核

## 提交规范

- `fix`: Bug 修复
- `feat`: 新功能
- `perf`: 性能优化
- `refactor`: 重构
- `build`: 构建/CI/CD
- `docs`: 文档
- `test`: 测试

## 代码风格

- 默认不写注释（WHY 而非 WHAT）
- 优先使用 RAII 管理资源
- 协程接口使用 `Core::Task<T>` 返回类型
- 异常使用 `Base::Exception` 体系
- 日志使用 `LOG_*_FMT` 宏

## 模块架构

```
src/Base/    — 基础设施：配置、日志、异常
src/Core/    — 异步运行时：epoll、协程、TLS、调度器
src/Net/     — 网络层：HTTP/HTTPS、路由、中间件
tests/       — 单元测试（Catch2）
samples/     — 示例程序
```

## 行为准则

本项目遵循 [Contributor Covenant](https://www.contributor-covenant.org/)。
