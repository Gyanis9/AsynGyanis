# AsynGyanis

> 基于 C++20 协程与 epoll/wepoll 边缘触发的跨平台异步服务器引擎 —— 网络（TCP / HTTP / HTTPS）、数据库（ORM / 连接池 / 三方驱动）与原生格式库（nlohmann_json / yaml-cpp）

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Linux](https://img.shields.io/badge/platform-Linux-orange)](https://kernel.org)
[![Windows](https://img.shields.io/badge/platform-Windows-blue)](https://microsoft.com/windows)
[![Tests](https://img.shields.io/badge/tests-1944-brightgreen)]()

## 特性

**运行时（Core）**

- **epoll / wepoll 边缘触发 I/O** — 统一事件循环；Linux 用 epoll，Windows 用 vendored 的 wepoll，两边语义一致
- **C++20 协程** — `Task<T>` 惰性启动，`co_await` 挂起与恢复；等待描述符就绪、定时到期、跨线程投递都是可等待对象
- **每线程一个事件循环** — `IoContext` 持有 `ThreadPool`，每个工作线程绑定独立的 `EventLoop`
- **两级就绪队列调度** — `Scheduler` 本地队列 + 全局队列，跨线程投递按归属循环投递
- **协作式取消与优雅启停** — `std::stop_token` 贯穿，`stop()` 后各线程收敛退出
- **TLS** — 基于 OpenSSL 的非阻塞 `SSL_read` / `SSL_write` 与事件循环集成
- **自研内存与缓冲** — 协程帧内存池（`CoroutinePool`，重载 `operator new` 接入 `Task`）

**网络（Net）**

- **HTTP/1.1** — 手写增量解析器（含资源上限与分块编码）、Keep-Alive 持久连接
- **HTTPS** — TLS 握手 + HTTP over TLS
- **路由与中间件** — 精确匹配、参数化路径（`:id`）、通配符（`*`）、洋葱模型
- **按线程一个监听 socket** — `SO_REUSEPORT` 由内核分摊连接，避免 accept 单点
- **接受分发（跨平台多核扩展）** — 一个监听器接受、按轮转把连接交给 N 个工作循环，不依赖
  `SO_REUSEPORT`；Windows 上这是唯一可用的多核形态（`ConnectionDistributor` + `TcpServer::startAccepting()`）
- **静态文件服务** — `staticFileDir()` 一行接入

**数据（Database）**

- **SqlSugar 风格 ORM** — 结构体声明即表结构，`insert` / `toList` / `first` / `count` / `update` / 删除 / 批量插入
- **SQL 方言层** — 查询树渲染与参数收集只有一份实现（`StandardSqlDialect`），SQLite 与 MySQL 各自只覆写引擎知识；写语句与事务语句一律由方言生成，ORM 不含 SQL 拼接
- **参数化执行** — 取值一律以绑定参数送出，不拼进 SQL 文本（含引号、`--`、分号的文本只会被当作数据）
- **高性能连接池** — LIFO 复用、惰性创建、双机制清理（空闲回收 + 上限保护）
- **异步执行器** — 数据库阻塞调用挪出事件循环线程，完成后经 `Scheduler::scheduleRemote` 投回指定 `EventLoop`
- **事务与建表迁移** — `Transaction` RAII（析构未提交自动回滚）、`SchemaMigrator` 从表结构生成 DDL

**基础（Platform / Base）**

- **原生格式库** — JSON 与 YAML 直接使用 [nlohmann_json](https://github.com/nlohmann/json) 与 [yaml-cpp](https://github.com/jbeder/yaml-cpp) 的接口（DOM、Pointer/Patch、多文档、事件），不再自研解析与值模型
- **配置管理** — YAML/JSON 加载、目录递归装载、热重载（inotify / ReadDirectoryChangesW）
- **结构化日志** — 6 级、4 种 Sink（控制台/文件/滚动/异步）、C++20 `std::format`、源码位置
- **平台隔离** — 所有 OS 调用集中在 `Platform`，上层不出现平台宏与 Win32/POSIX API

## 架构

模块分层与依赖方向（箭头表示「依赖」）：

```
                 samples（示例程序）
                        │
   ┌────────────────────┼────────────────────┐
   │                    │                    │
  Net ────── Core ─── Database ──────────  Base
   │         │            │                  │
   └─────────┴────────────┴──────► Platform ◄┘
                                  （唯一允许直接触碰 OS 的模块）
```

| 模块 | 库 | 依赖 | 职责 |
|------|----|------|------|
| `Platform` | `libPlatform.a` | — | 描述符 / socket / 事件通知 / 定时器 / 文件监听 / 原子写 / 编码转换 / 进程与时间 |
| `Base` | `libBase.a` | Platform, nlohmann_json, yaml-cpp | 日志、配置、异常层次、JSON/YAML 原生库的传递依赖 |
| `Core` | `libCore.a` | Platform, Base, OpenSSL | 事件循环、协程运行时、socket、TLS |
| `Net` | `libNet.a` | Core, llhttp | TCP 服务基类、HTTP/HTTPS 服务、路由与中间件 |
| `Database` | `libDatabase.a` | Core, sqlite3, hiredis, libmysqlclient | 连接抽象、连接池、SQL 方言、ORM、建表迁移 |

模块内的子目录（如 `Base/Log/Sinks`、`Core/EventLoop`）**不引入新的命名空间**：命名空间一律到模块名为止（`AsynGyanis::Base`、`AsynGyanis::Core` …），include 路径从 `src/` 起算（`#include "Core/EventLoop/EventLoop.h"`）。

> `asserts/` 下的 5 张架构图绘于重构之前，与当前模块划分（尤其是新增的 `Database`、Core 的四目录拆分、`Base/Format` 已整体移除）已不一致，**待重绘**；上表是当前状态的准确描述。

## 快速开始

### 前置依赖

- **CMake** ≥ 3.20、**Conan** ≥ 2.0
- **编译器**：MSVC ≥ 19.40 / GCC ≥ 13 / Clang ≥ 17（需支持 C++20 协程）
- **系统**：Windows ≥ 10 或 Linux（依赖 epoll；Windows 侧由 vendored wepoll 提供等价能力）

第三方依赖由 `conan_provider.cmake` 在 CMake 配置阶段自动安装（`conan install --build=missing`），无需手工执行。

### 构建与测试

```bash
# Linux
cmake --preset debug
cmake --build build/debug -j"$(nproc)"
ctest --test-dir build/debug --output-on-failure
```

```bat
:: Windows（需在已加载 vcvars64 的环境中执行）
cmake --preset debug
cmake --build build/debug -j 14
ctest --test-dir build/debug --output-on-failure
```

`release` 预设同样可用（关闭 sanitizer、开启优化与 LTO）。

**关于 Debug 预设的 AddressSanitizer**：`debug` 预设开启 `ENABLE_SANITIZERS`，MSVC 下为 `/fsanitize=address`（GCC/Clang 上额外带 UBSan）。构建时会把 ASan 运行库拷到可执行文件旁，因此测试可脱离 VS 环境直接运行；容器注解因与未插桩的 gtest 存在 ABI 标记冲突而关闭（原因与出路写在根 `CMakeLists.txt` 注释里）。**ASan 会显著抬高每帧栈开销**，写深递归用例时要按这个预算来。

### 作为依赖使用（find_package）

安装后（`cmake --install build/release --prefix <前缀>`，或直接用 Conan 包）外部工程即可消费：

```cmake
find_package(AsynGyanis REQUIRED)                    # 请求全部五个模块
find_package(AsynGyanis COMPONENTS Net REQUIRED)     # 只要 Net：自动带出 Core/Base/Platform 及其外部依赖

target_link_libraries(app PRIVATE AsynGyanis::Net)
```

请求部分组件时，包配置只为**被请求组件及其传递依赖组件**拉取外部依赖——只要 `Base` 就不会去找 OpenSSL/zlib。
每个组件的外部依赖清单由各模块在配置期自行登记（`AsynGyanisPackageDependencies_<组件>`），因此不会与实际情况漂移；
组件名写错会被 `check_required_components` 当场挡下。

Debug 包的接口带着 ASan 与容器注解开关（Debug 配置）：消费方链接后**运行需要 ASan 运行库 DLL**；
不想带这些依赖就用 `release` 预设产出的包。

### 真机用例（数据库）

依赖真实服务端的用例一律**环境变量门控**，口令无默认值、缺失即整组 `GTEST_SKIP`（不是失败），因此没有服务端的机器上仍然全绿：

| 服务 | 环境变量 | 用例位置 |
|------|---------|---------|
| MySQL | `ASYN_MYSQL_TEST_HOST/PORT/USER/PASSWORD/DATABASE` | `tests/Database/MySql/TestMySqlIntegration.cpp` |
| Redis | `ASYN_REDIS_TEST_HOST/PORT/USER/PASSWORD/DATABASE` | `tests/Database/Redis/TestRedisIntegration.cpp` |

## 运行示例

`samples/echo_server` 随构建一起编译（默认每线程一个监听 socket）：

```bash
./build/debug/samples/echo_server --port 8080 --threads 4                 # HTTP
./build/debug/samples/echo_server --https --cert cert.pem --key key.pem   # HTTPS
# 一个监听器 + N 个工作循环，靠用户态分发而非 SO_REUSEPORT（Windows 多线程请用这个）
./build/debug/samples/echo_server --port 8080 --threads 4 --dispatch-accept
./build/debug/samples/echo_server --help                                  # 全部参数
```

内建端点：`GET /`、`GET /json`、`GET /bench`。

## 代码示例

以下示例均取自 `samples/main.cpp` 与 `tests/`，是当前代码里真实可编译的用法。

### HTTP 服务（多线程，每线程一个监听 socket）

```cpp
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

void setupRoutes(Net::Router &router)
{
    router.get("/", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
    {
        response.setStatus(200);
        response.setHeader("Content-Type", "text/plain");
        response.setBody("Hello World");
        co_return;
    });
}

int main()
{
    Core::IoContext context(4);                                    // 4 个工作线程
    auto             address = Core::InetAddress::resolve("localhost", 8080);
    Core::ThreadPool &pool   = context.threadPool();

    // 每个线程一个 HttpServer，靠 SO_REUSEPORT 由内核分摊连接
    std::vector<std::unique_ptr<Net::TcpServer>> servers;
    for (std::size_t index = 0; index < pool.threadCount(); ++index)
    {
        Core::EventLoop &loop   = pool.eventLoop(index);
        auto             server = std::make_unique<Net::HttpServer>(loop, *address);
        setupRoutes(server->router());

        // accept 是协程：挂到该线程的调度器上，随事件循环推进
        auto acceptTask = server->start();
        loop.scheduler().schedule(acceptTask.handle());

        servers.push_back(std::move(server));
    }

    pool.start();
    // ... 等待退出信号 ...
    context.stop();
    return 0;
}
```

### ORM：结构体即表结构

```cpp
#include "Database/Pool/ConnectionPool.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

struct User
{
    std::int64_t               id;
    std::string                name;
    std::optional<std::string> note;   // optional 成员 ⇒ 该列可空
    std::int64_t               age;
};

// 表结构的唯一真值来源：ORM 的读写与建表迁移都用它
template<>
struct AsynGyanis::Database::Queryable::TableSchema<User>
{
    static constexpr std::string_view kTableName = "users";
    static constexpr auto kColumns = std::tuple{
        Column(&User::id,   "id"),
        Column(&User::name, "name"),
        Column(&User::note, "note"),
        Column(&User::age,  "age"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

void useOrm(AsynGyanis::Database::ConnectionPool &pool)
{
    using namespace AsynGyanis::Database;
    using namespace AsynGyanis::Database::Queryable;

    SchemaMigrator::createTable<User>(pool);          // DDL 由 TableSchema 生成

    Queryable<User> insertQuery(pool);
    insertQuery.insert(User{.id = 1, .name = "张三", .note = std::string("首条"), .age = 18});

    Queryable<User> query(pool);
    const std::vector<User> adults =
        query.where(Column(&User::age, "age") >= std::int64_t{18})
             .orderBy(asc("age"))
             .limit(10)
             .toList();

    Queryable<User> updateQuery(pool);
    updateQuery.update(User{.id = 1, .name = "张三", .note = std::nullopt, .age = 19});
}
```

把阻塞链路挪出事件循环线程：同名的 `toListAsync` / `firstAsync` / `countAsync` / `insertAsync` / `insertBatchAsync` / `updateAsync` / `executeNonQueryAsync` 接受一个 `Core::EventLoop&` 作为恢复目标，`co_await` 它们即可。

### JSON 与 YAML：使用原生库接口

```cpp
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

const auto document = nlohmann::json::parse(R"({"port":8080})");
const int port = document.at("port").get<int>();
const std::string compact = document.dump();
const std::string pretty = document.dump(2);

const YAML::Node configuration = YAML::Load("port: 8080");
const int yamlPort = configuration["port"].as<int>();
const std::string yamlText = YAML::Dump(configuration);
```

链接 `AsynGyanis::Base` 即可获得两库的传递依赖。JSON Pointer、Patch、Merge Patch 与 SAX 直接使用 nlohmann_json；YAML 多文档与事件接口直接使用 yaml-cpp。原生接口分别抛 `nlohmann::json::exception` 与 `YAML::Exception`，配置加载边界将错误汇入 `ConfigLoadResult::errors`。

`ConfigValue` 是 `nlohmann::json` 的别名：成员读取用 `at()`，取值用 `get<T>()`，类型判定用 `is_*()`；不再提供自研 DOM、解析选项或兼容接口。几处与旧值模型不同、容易踩的口径：原生 JSON 的正整数是无符号类型（`number_unsigned`）；`empty()` 只对 `null` 与空容器为真，**空字符串不算空**；`get<T>()` 允许算术类型互转，配置层另有严格取用（`configValueAs`，不取整、不回绕）。配置加载把 YAML 文档转换为 JSON 值模型时，标量按 YAML 1.2 核心 schema 识别（引号标量一律按字符串、`yes/no/on/off` 是字符串），自定义标签、重复键与复杂键直接报错。

### 配置与日志

```cpp
#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"

AsynGyanis::Base::ConfigManager configuration;
configuration.loadFromDirectory("config", /*recursive=*/true);
const int port = configuration.get<int>("server.port", 8080);

AsynGyanis::Base::Logger &rootLogger = AsynGyanis::Base::LoggerRegistry::instance().getRootLogger();
rootLogger.addSink(std::make_unique<AsynGyanis::Base::ConsoleSink>());
rootLogger.setLevel(AsynGyanis::Base::LogLevel::Debug);

LOG_INFO_FMT("listening on port {}", port);
```

## 模块概览

### Platform — 平台底层（`libPlatform.a`）

| 分类 | 内容 |
|------|------|
| 描述符与 IO | 文件描述符 RAII、socket 初始化与选项、事件通知（Linux eventfd / Windows socketpair） |
| 文件系统 | 文件读写、原子写、目录遍历、文件监听（inotify / ReadDirectoryChangesW） |
| 系统 | 错误码与可读原因、进程与目录、环境变量、本地时间、UTF-8 ↔ UTF-16 与代码页 |

### Base — 基础设施（`libBase.a`）

| 分类 | 内容 |
|------|------|
| 异常 | `Exception` 层次（携带 `source_location`），配置与网络各自的错误类型 |
| 日志 | `Logger` / `LoggerRegistry`、6 级 `LogLevel`、`LogEvent`、`LogMacros`、4 种 `LogSink`（控制台/文件/滚动/异步）、两种格式化器、从配置装载日志设置 |
| 配置 | `ConfigManager`（多文件/目录装载、热重载、严格类型化取值）、文件监听；`ConfigValue` 即 nlohmann_json 文档 |
| 格式 | JSON 与 YAML 使用 nlohmann_json / yaml-cpp 的原生接口，由 `Base` 传递依赖（见「外部依赖」） |

### Core — 异步运行时（`libCore.a`）

| 子目录 | 内容 |
|--------|------|
| `EventLoop/` | `IoContext`（运行时入口）、`EventLoop`、`Epoll`（wepoll 后端）、`EpollAwaiter`、`Timer` |
| `Coroutine/` | `Task<T>`、`Scheduler`（本地队列 + 全局队列）、`ThreadPool`、`CoroutinePool`、`Cancelable` |
| `Socket/` | `AsyncSocket`、`VectoredSendCursor`、`InetAddress`、`Connection`、`ConnectionManager` |
| `Tls/` | `TlsContext`、`TlsSocket` |

### Net — 网络应用层（`libNet.a`）

| 子目录 | 内容 |
|--------|------|
| `Tcp/` | `TcpAcceptor`（`SO_REUSEPORT` 监听）、`TcpStream`（`readExact` / `readUntil` / `writeAll`）、`TcpServer` |
| `Http/` | `HttpRequest` / `HttpResponse` / `HttpMethod`、`HttpParser`（llhttp）、`Router` 与 `Middleware`、`HttpSession` / `HttpServer`、`HttpsSession` / `HttpsServer`、`FileSender`（静态文件） |

### Database — 数据访问（`libDatabase.a`）

| 子目录 | 内容 |
|--------|------|
| `Common/` | `DatabaseConnection` / `DatabaseResult` / `DatabaseValue` 抽象、`ConnectionConfig`、`DatabaseType`、`DatabaseFactory` |
| `Dialect/` | `SqlDialect` 契约、`StandardSqlDialect`（共用渲染）、`SqliteDialect`、`MySqlDialect`、`SqlStatement`、`ColumnType`、`DialectRegistry` |
| `Pool/` | `PoolConfig`、`PooledConnection`（RAII 租约）、`ConnectionPool`、`Transaction`、`AsyncExecutor` |
| `Queryable/` | `Column` / `TableSchema` / `QueryNode` / `Expression`、`Queryable<T>`、`RowMapper`、`SchemaMigrator` |
| `Sqlite/` `MySql/` `Redis/` | 三种驱动实现（可选依赖缺失时退化为「每个入口给中文错误」的桩） |

## 目录结构

```
AsynGyanis/
├── CMakeLists.txt          # 顶层：C++20 设置 + sanitizer 开关 + add_subdirectory
├── CMakePresets.json       # debug / release 预设（debug 带 AddressSanitizer）
├── conanfile.py            # 依赖清单由 conandata.yml 驱动
├── conandata.yml           # 第三方依赖与版本
├── conan_provider.cmake    # CMake 侧自动触发 conan install
├── samples/                # echo_server（随构建编译，示例即被验证）
├── asserts/                # 架构图（绘于重构前，待重绘）
├── src/
│   ├── Platform/           # 平台底层（OS 调用的唯一出处）
│   ├── Base/               # Config / Exception / Log
│   ├── Core/               # Coroutine / EventLoop / Socket / Tls
│   ├── Net/                # Tcp / Http
│   └── Database/           # Common / Dialect / Pool / Queryable / Sqlite / MySql / Redis
└── tests/                  # 与 src 逐级对齐的 GoogleTest 测试
```

## 外部依赖

| 库 | 版本 | 用途 |
|----|------|------|
| [nlohmann_json](https://github.com/nlohmann/json) | 3.12.0 | JSON 值模型与解析/序列化（Base 公开接口） |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.9.0 | YAML 解析（配置加载） |
| [OpenSSL](https://www.openssl.org/) | 3.6.2 | TLS/HTTPS |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | 单元测试 |
| [SQLite3](https://www.sqlite.org/) | 3.51.3 | 嵌入式数据库驱动 |
| [llhttp](https://github.com/nodejs/llhttp) | 9.3.0 | HTTP/1.1 解析 |
| [hiredis](https://github.com/redis/hiredis) | 1.3.0 | Redis 客户端 |
| [libmysqlclient](https://dev.mysql.com/doc/c-api/) | 8.1.0 | MySQL 客户端 |

- YAML 与 JSON 使用 nlohmann_json 与 yaml-cpp（均为必选依赖，随 `Base` 公开传递其头文件与链接）。YAML 转配置值模型的口径：引号标量按字符串、`!!str/!!int/!!float/!!bool/!!null` 之外的自定义标签直接报错、重复键报错、别名展开设深度与节点总数上限。
- [wepoll](https://github.com/piscisaureus/wepoll) 的**头文件**（`src/Core/EventLoop/wepoll.h`）随仓库分发，供
  Windows 完成端口后端（`Iocp.cpp`）借用 `epoll_event` 与事件位定义；AFD 轮询实现已删除，不再参与轮询。
- SQLite3 为必选；hiredis 与 libmysqlclient 为**可选**：探测不到时对应驱动退化为报错桩，不会让配置阶段失败。

## 测试与验证

- **GoogleTest**（`gtest_discover_tests`，每个用例独立进程），测试目录与 `src` 逐级对齐
- 当前规模：**1944 个用例**（其中 34 个是真机门控用例，无凭据即 SKIP）
- 零编译器告警是提交判据；Debug 构建在 AddressSanitizer 下跑通且无报告
- 真机套件：MySQL 21 例、Redis 13 例（覆盖认证、参数化往返、事务、批量插入、异步读写链路、管道与回复类型映射）

## 编码规范

完整规范见个人 skill `cpp20-coding-standards`（命名、Doxygen 中文注释、错误契约、测试、目录与构建、验证纪律）。仓库层面的要点：

| 规则 | 说明 |
|------|------|
| 命名 | 类/文件/目录 PascalCase，函数与参数 camelCase，成员 `m_` / 静态 `s_`，常量 `kPascalCase` |
| 注释 | 全中文 Doxygen；`override` 方法必须独立完整注释；实现体关键位置写「为什么」 |
| 错误 | 报错文案全中文且写清「原因 + 替代做法」；禁止静默失败与静默变形 |
| 测试 | 每个功能都有用例；依赖外部服务的用例一律环境变量门控、仓库零明文凭据 |
| 目录 | `tests` 逐级镜像 `src`；CMakeLists 分层聚合（叶子目录 append 到 `GLOBAL PROPERTY`） |
| 提交 | 中文提交信息讲清「为什么」，一次提交只讲一件事 |

## 版权

Copyright (c) 2026 — MIT License
