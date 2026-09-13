# 更新日志

本文件记录每个已发布版本对使用者可见的变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本 2.0.0](https://semver.org/lang/zh-CN/)。

## 版本号策略

- **单一真值来源**：版本号只写在根 `CMakeLists.txt` 的 `project(AsynGyanis VERSION x.y.z)` 里，
  对外导出的 `find_package(AsynGyanis x.y)` 与安装包版本都取自它，不另外维护一份。
- **标签格式**：发布时在本文件与 CMake 版本号一致的提交上打附注标签 `v<主>.<次>.<修订>`（例如 `v1.0.0`）。
- **版本号怎么涨**：由约定式提交推导——`feat` 涨次版本，`fix` 涨修订号，正文含 `BREAKING CHANGE`
  或类型后带 `!` 涨主版本；其余类型（`refactor`/`perf`/`test`/`docs`/`chore`/`build`/`ci`）不单独抬版本。
- **一致性由脚本把关**：`scripts/check-release-version.py` 比对「CMake 版本号 / 本文件最新发布段 / 最新标签」
  三者，不一致即退出码非 0；Linux CI 已接入这一步，避免出现「打了标签但版本号没改」这类漂移。

## [Unreleased]

（暂无）

## [1.0.0] - 2026-09-13

首个版本：从零搭起一套 C++20 协程 + epoll/wepoll 的异步服务器引擎，网络、数据库与格式库三块齐备。

### 新增

- **运行时（Core）**：`Task<T>` 惰性启动的 C++20 协程；每线程一个 `EventLoop`（Linux epoll / Windows
  vendored wepoll）；两级就绪队列的 `Scheduler`（本地队列 + 全局队列，跨线程按归属循环投递）；
  每循环单 timerfd（Windows 为可等待定时器）+ 时间堆；`std::stop_token` 协作式取消；协程帧内存池。
- **网络（Net）**：`TcpServer`/`TcpAcceptor`（`SO_REUSEPORT` 多监听器、连接上限、空闲清扫、优雅 drain）；
  手写 HTTP/1.1 增量解析器（资源上限、分块编码、pipeline）；路由与中间件（精确/参数/通配、洋葱模型）；
  静态文件服务（`mmap` 零拷贝正文、`Date`/`ETag`/`304`/`Range` 条件请求）；SSE 流式响应；
  WebSocket（RFC 6455 握手、帧编解码、UTF-8 校验、分片重组）；HTTP/2（RFC 9113 帧与连接状态机、
  自研 HPACK 含 Huffman、接收窗口流控、流式响应、GOAWAY）；h2c 明文 h2；RFC 8441 扩展 CONNECT（HTTP/2 上的
  WebSocket 隧道）；TLS（最低 1.2、安全等级 2、显式排除弱套件、ALPN 协商 h2、mTLS、证书热轮换）。
- **生产运维**：`/metrics`（Prometheus 文本 0.0.4）与 `/healthz` 内建端点；`HttpServerStats` 统计快照
  （状态码分类、延迟直方图、WebSocket 与 HTTP/2 专有计数）；令牌桶限流中间件（进程级全局 RPS）；
  按来源 IP 的并发连接限额；`HttpServerConfig` 把限额、限流、按 IP 限额与指标开关接到配置文件。
- **数据库（Database）**：SqlSugar 风格 ORM（结构体声明即表结构）；单一实现的标准 SQL 方言层
  （SQLite/MySQL 只覆写引擎知识）；参数化执行；LIFO 连接池；异步执行器（阻塞调用挪出事件循环后投回）；
  `Transaction` RAII 与 `SchemaMigrator` 建表迁移；二进制列的绑定与读取链路。
- **格式与基础（Base / Platform）**：自研 JSON（RFC 8259/6901/6902/7396 + 流式读写）与严格 YAML 1.2，
  含 DOM 增删改查与零拷贝视图；`ConfigManager`（目录递归装载、热重载、schema 校验）；
  结构化日志（6 级、4 种 Sink、`std::format`、源码位置、JSON Lines 格式化器）；异常体系
  （运行期/逻辑错误两根，报错文本全中文）；所有 OS 调用集中到 `Platform`，上层不出现平台宏。

### 性能与工程

- Release 实测基线与门槛脚本（`benchmarks/baseline.json` + `check-baseline.py`），压测脚本入库。
- 热路径微基准（`benchmarks/microbench`）：HPACK 编解码、h1 解析器、帧编解码、日期缓存、请求 id 生成。
- 单请求分配画像压到 33 次 / 816 B（起点 48 次 / 4228 B）。
- Linux CI（GCC + ASan/UBSan + Redis 真机）与 Windows CI（MSVC + ASan）；解析器模糊冒烟测试。

[Unreleased]: https://github.com/Gyanis9/AsynGyanis/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Gyanis9/AsynGyanis/releases/tag/v1.0.0
