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

下面第 2 到第 4 项那几道门（格式、复杂度、图追溯、台账日期、公开接口面、构建、用例、示例矩阵）由一条命令
一次跑完，判据形状与 CI 一致：

```bash
bash scripts/run-gates.sh                                # 全部八道，debug 预设
bash scripts/run-gates.sh --preset release               # 换 Release 档
bash scripts/run-gates.sh --gate format --gate arch-trace  # 只跑点名的几道，改一处时不必等整条链
```

每道门印一行既有形状的判据（`COMPLEXITY_GATE=OK`、`ARCH_TRACE_GATE=OK`、`API_GATE=OK` 这三条由门禁脚本
自己印，其余由这条命令按同一形状印），原始输出进 `build/gate-logs/<门名>.log`，判红的那道门把输出整片贴出来；
最后一行给总判据与机读的 `build/gate-logs/run-gates.json`（每道门的命令、退出码、耗时、日志位置）。
阈值不在这里抄第二份：地板与下限只在各门禁脚本与工作流那一处。格式档的 clang-format 版本现读
`.github/workflows/linux-ci.yml` 里钉的那一版，本机对不上就换容器里那份（`--format container
--format-container <容器名>`），点名要本机那份时（`--format local`）版本不符就退，不偷偷换裁判。
退出码：0 全绿、1 某道门判红（构建红时后面的用例与示例没有产物可跑，链在此停住）、2 前置缺件——
「没跑成」不许读成「没问题」。Windows 侧要先 `call vcvars64` 再进来，否则构建那一档红在编译参数上。
覆盖率、TSan、模糊、交付物、供应链那几档各有前置（服务、容器、插桩构建），不在这条命令里：跑完它不等于 CI 全绿。

1. **Fork & Clone**: 从 `main` 分支创建功能分支
2. **编码**: 新增代码对齐 `.clang-format` 与周围既有风格。全仓已按该配置排过一遍，CI 有**格式门禁**
   （clang-format 版本钉到 23.1.1）：改动过的文件提交前跑 `clang-format --dry-run --Werror <文件>`，
   有差异就 `-i` 排齐再提。判据以 CI 同版为准——版本不同折行结果就变，那种红与你的改动无关
3. **静态检查**: 运行 `clang-tidy -p build/debug src/<changed-file>`。另有一条有牙齿的判据：
   `python3 scripts/check-complexity-baseline.py` 按文件记**函数长度列表**：超过地板（100 行）的每个函数都要
   登记，登记过的文件按列表降序**按位比对**（第 i 长者不得涨过基线第 i 长）、**不得多出新的超地板函数**，
   **只准降不准升**（它挂在 CI 的格式作业而不是 tidy 档——tidy 那一档按设计不阻塞，挂上去等于没有门）。基线要重录就跑 `--write`，但那等于公开承认「这一档我放松了」
   还有一条管架构工件的：`python3 scripts/check-arch-traceability.py` 把 `assets/diagrams/*.json` 里每一条
   `路径:行号` 当场回读——`sources` 挂的与卡片正文手写的都算（正文里 `:123`、`.cpp:123`、`、123` 这三种续写按
   前一个完整路径继承，只写文件名时按仓库内唯一同名文件补全，重名不猜）；`.github/SECURITY.md` 全篇与 CHANGELOG
   最新发布段里的手写引用同判。路径不存在、行号越过文件末尾、引到空行都判红。它管不住「行还在、说的已经不是
   那件事」（那由它打印的 stale 清单交给人回图时复核），但「引用落得下一行真实代码」这条底线不该靠人记
   还有一条管公开接口面的：`python3 scripts/check-api-baseline.py` 把「安装出去的每一份公开头里的声明」固化成
   `packaging/api-baseline.txt`（一行一条「头文件路径<TAB>规范化声明」，整体排序，不含时间戳与绝对路径），
   改动让 diff 非空就判红。覆盖面与排除名单从 `CMakeLists.txt` 里 `install(DIRECTORY src/ …)` 那一段现读，
   不在脚本里抄第二份名单——安装规则一改，基线跟着变。删除或改签名算破坏：除了重录基线（`--write`），
   还要把 CMake 版本号的 MAJOR 抬上去，两处都留痕才放行；只抬版本而接口面一字未改同样判红（那是版本笔误）。
   它管的是源码声明面（签名与增删成员），管不住布局与大小变化，理由见脚本文件头。
   这道门挂在 CI 的格式作业与发布门禁两处，只读源码、不建不链
4. **测试**: 确保 `ctest --preset debug`（在仓库根调用；并行度自己给 `-j`）全部通过
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

## 评审与代码所有权

`.github/CODEOWNERS` 按目录标了 owner：改动 `src/Net/`（协议实现，输入直接来自公网）、`src/Platform/`+`src/Core/`
（事件循环与协程帧的跨线程销毁纪律）、`packaging/`（打包与依赖）时，评审要求由仓库设置里的 branch protection
决定，本文件只负责「改了这里就自动找谁」。新增顶层目录不必先改 CODEOWNERS——兜底 owner 覆盖全仓。

## 依赖、SBOM 与安全公告

- **加/升第三方依赖要走四处**：`conandata.yml` 的固定版本、`packaging/dependency-watch.json` 的公告入口与
  `pinnedVersion`（台账每条还要带 `license` / `purlName` / `homepage` 三栏：SBOM 的上游正名、官网与许可只从这一份读，缺 license 会被 SBOM 自检判红）、README「外部依赖」表的版本、以及交付配方 `packaging/conan/conanfile.py` 里写版本的四处；
  可选依赖还要另动根 `conanfile.py` 的选项（要给降级路径）。忘掉其中一处会被当场判红：
  `python scripts/check-dependency-advisories.py` 钉的是「四处同解」——缺登记、版本漂移、台账残留已删依赖、交付配方与真源不一致都算；这条不与构建混跑，几秒钟就能暴露。
- **SBOM**：`python scripts/generate-sbom.py --out sbom` 产出 CycloneDX 1.6（组件按 purl 排序、
  不带构建时刻，所以两次生成可逐字节比对）与 `SHA256SUMS`。它记的是**清单与来源**，不是哈希后的二进制；
  需要制品级 provenance/签名时另配，别把这份 SBOM 当成那一层证据。
- **供应链作业**：`.github/workflows/supply-chain.yml`（`main` 推送、手动触发、每周一凌晨定时）跑上面三件事（第三件是 2026-10-12 起的 gitleaks 全树密钥扫描；豁免面在 `.gitleaks.toml`），
  并把 SBOM 作为制品上传（保留 90 天）。
- **漏洞响应**：见 `.github/SECURITY.md` 的严重度时限表与披露节奏；「我们靠哪些持续验证」那张表列了每条门禁
  覆盖什么、在哪个作业里跑——新加门禁时要一并更新它，否则「最近一次真实运行」又要靠人记。

## 代码风格

- 注释遵循 C++23 编码规范：文件头块（`@file`/`@brief`/`@author`/`@date`/`@version`/`@copyright`）**只写在 `.h` 上**，`.cpp` 不写文件头；类与公开方法写完整中文 Doxygen（`@param`/`@return`），成员变量行尾用 `///<`
- 子类每个 `override` 必须独立书写完整中文注释，`@details` 说明与父类的行为差异，禁止「同上/继承自父类」占位
- 优先使用 RAII 管理资源
- 协程接口使用 `AsynGyanis::Core::Task<T>` 返回类型
- 错误处理按**失败形态**分通道，不按模块各造一套（下表是唯一的归属定案处）：

  | 失败形态 | 走哪条通道 | 具体类型 |
  |---|---|---|
  | 对端可控：坏报文/坏帧/超限 | `std::expected<T, E>`，E 为 `{kind enum, message}` | `QuicDecodeError`、`Http3FrameError`、`QpackError`、`Http2FrameErrorKind`、`HttpParseErrorKind`；h1 解析另有 `ParseStatus` |
  | 系统调用失败 | 异常 + OS 码 | `Base::SystemException`（带 `std::error_code`）、`Core::CoreException` |
  | 调用方把接口用错（值非法/状态不允许） | 异常，落在 `std::logic_error` 分支 | `Base::InvalidArgumentException`、`Base::LogicException` |
  | 初始化/配置/不可恢复故障 | 异常，落在 `std::runtime_error` 分支 | `Base::Exception` 家族、`Base::ConfigValidationException`、`Database::DatabaseException` 家族 |
  | 语句被驱动拒绝 | 异常 + **驱动原生码** | `Database::QueryExecutionException::nativeErrorCode()` / `isRetryable()` |
  | 平台层可恢复失败 | `std::expected<T, std::error_code>` | `Platform::IO` 各接口 |

  两条根（`std::runtime_error` 与 `std::logic_error`）**刻意不合并**：`catch (const Base::Exception &)`
  是「可恢复故障」的捕获面，把调用方的 bug 一起吞掉更糟。三条链分属标准库两条分支、共享不了异常
  基类，但抛出点快照与调用栈共享同一个非异常基类 `Base::Detail::ExceptionPayload`——新增第四条链
  不必再改 `tryStackTrace()`，而漏改原本的后果是**静默丢栈**（日志照出，只是不带栈，不报错）。
  也不设统一的大 `ErrorCode` 枚举：协议层按 RFC 上线码切分、OS 层按 `std::error_code`、驱动层按
  原生码，三套语义空间强行合并只会造出一个没人能用的中间枚举；统一的是**形状与命名规则**。
- 每个进 `std::expected` 的框架错误类型，在自己的头里带一对结果别名：`XxxExpected<Value>` 与 `XxxStatus`
  （后者即 `std::expected<void, XxxError>`）。名字取错误类型去掉尾部的 `Error`/`Status`/`Failure` 再加后缀，
  **没有真实消费方的那一半不定义**。签名与 `@return` 一律写别名；载荷还是裸 `std::string` 或 `std::error_code`
  的通道暂时没有别名，等各自的错误类型落地再补。用例侧判成功分支统一用 `tests/TestSupport/ExpectedAssertions.h`
  的 `ASSERT_HAS_VALUE` / `EXPECT_HAS_VALUE`——光板一句 `ASSERT_TRUE(x.has_value())` 红的时候不报失败载荷。
- 异常**不得跨线程/跨回调传播**：事件循环的派发单元（`Scheduler` 的每处投递与恢复、
  `IoWatcher::handleEvents` 的每次 `resume`）、会话回调、线程池任务都必须就地收下抛出的
  异常，转成日志 + 计数（`Scheduler::failedDispatchCount()`、`FileWatcher::failedCallbackCount()`）
  或连接收口，不能让它沿调用点逃到线程入口。`EventLoop::run()` 因此**不向调用方抛**
  （它的调用点大多是裸 `std::thread` 入口）；被收下的失败要可观测，故有
  `EventLoopSnapshot::stoppedByFailure`。
- 返回 `Task<std::expected<...>>` 的协程**不得让异常逃过 await 点**：调 Core 里声明 `@throws` 的接口
  （`Core::Digest`、`Core::Random` 这一族）要么就地转成失败分支，要么用该接口的不抛变体。异常在协程体内抛出会被
  存进 `Task` promise 的 `std::exception_ptr`，直到最外层读结果时才重抛——那时它已经离开了本该收口的那一层，
  而这一层的调用方按「拿到 unexpected」写着
- 析构函数、`noexcept` 函数、C 回调（含挂进 OpenSSL 的函数指针）**禁止抛**：C++ 异常穿过
  C 帧是未定义行为。`noexcept` 函数内部捕获不违反本条，恰是实现它的手段。
  注意 MSVC 的 C4297 会让 `noexcept` 函数里出现显式 `throw` 时直接编译失败（`/W4 /WX` 下即门禁红）。
  这类函数里现拼错误文本（`std::format`、串接、`std::error_category::message()`）只欠 `bad_alloc` 一档——本机
  MSVC STL 与容器里的 libstdc++ 实测对未知错误码都不抛；这一档本仓接受，要给 `noexcept` 加保护的是上一条那种
  **自己声明会抛的接口**，不是内存分配失败。
- 第三方原生库（yaml-cpp / nlohmann_json）的异常只在配置加载边界捕获并汇入
  `ConfigLoadResult::errors`；驱动侧（SQLite/MySQL/Redis 均为 C API）不抛，失败一律转成
  `expected`/错误文本 + 配对的原生码。
- 热路径（事件循环 pump、协议解码、定时器、日志格式化、每请求派发）**不靠抛异常报错**——
  这些位置本来就应当没有 throw 点，改动前后都按这条审
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
samples/      — 示例程序（ReferenceServer，随构建编译）
```

命名空间一律到模块名为止（`AsynGyanis::Base`、`AsynGyanis::Core` …），子目录不引入新命名空间；include 路径从 `src/` 起算（`#include "Core/EventLoop/EventLoop.h"`）。模块分层与链接依赖见 README 的「架构」一节。

## 行为准则

本项目遵循 [Contributor Covenant](https://www.contributor-covenant.org/)。
