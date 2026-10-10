# AsynGyanis

> 基于 C++23 协程与水平触发事件循环（Linux epoll / Windows 完成端口 / 可选 io_uring）的跨平台高性能异步服务器引擎 —— 网络（TCP / UDP / HTTP/1.1 / HTTP/2 / HTTP/3 / WebSocket / QUIC）、数据库（ORM / 连接池 / 三方驱动）与原生格式库（nlohmann_json / yaml-cpp）

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://en.cppreference.com/w/cpp/23)
[![Linux](https://img.shields.io/badge/platform-Linux-orange)](https://kernel.org)
[![Windows](https://img.shields.io/badge/platform-Windows-blue)](https://microsoft.com/windows)
[![Linux 门禁](https://img.shields.io/github/actions/workflow/status/Gyanis9/AsynGyanis/linux-ci.yml?branch=main&label=Linux%20%E9%97%A8%E7%A6%81)](https://github.com/Gyanis9/AsynGyanis/actions/workflows/linux-ci.yml)
[![Windows 门禁](https://img.shields.io/github/actions/workflow/status/Gyanis9/AsynGyanis/windows-ci.yml?branch=main&label=Windows%20%E9%97%A8%E7%A6%81)](https://github.com/Gyanis9/AsynGyanis/actions/workflows/windows-ci.yml)

## 特性

**运行时（Core）**

- **三后端统一事件循环** — Linux epoll、Windows 完成端口（IOCP，完成通知翻译成 epoll 事件位）、Linux 可选 io_uring（`ASYN_WITH_IO_URING`）；语义一律水平触发 + 按需摘除关注位
- **C++20 协程** — `Task<T>` 惰性启动，`co_await` 挂起与恢复；等待描述符就绪、定时到期、跨线程投递都是可等待对象
- **每线程一个事件循环** — `IoContext` 持有 `ThreadPool`，每个工作线程绑定独立的 `EventLoop`
- **两级就绪队列调度** — `Scheduler` 本地队列 + 全局队列，跨线程投递按归属循环投递
- **协作式取消与优雅启停** — `std::stop_token` 贯穿，`stop()` 后各线程收敛退出
- **多进程 worker** — `WorkerSupervisor` 拉起 N 个 worker 同端口服务、崩溃即补位；进程间不共享状态。
  分摊形状按平台分叉：POSIX 上每个 worker 自己 bind 同一端口（`SO_REUSEPORT`），Windows 上没有那个
  选项、多个进程各自绑只会让最后绑上的一个收到全部报文，于是改由 master bind 一次、把套接字逐个复制
  给 worker。数据报这一侧的接手入口是 `UdpServer` 与 `QuicServer` 的接手构造（外加不带地址的
  `listen()`），交回来的裸描述符由 `Platform::DatagramSocket::adopt` 包成本层持有者语义；两种启动顺序
  不许混用——接手来的服务端再去 bind 会把交过来的那份静默闲置，端口上看着在监听却收不到报文
- **TLS** — 基于 OpenSSL 的非阻塞 `SSL_read` / `SSL_write` 与事件循环集成
- **进程级指标注册表** — `ProcessMetricsRegistry::registerMetric(...)` 交回 RAII 把手（析构即注销），
  `/metrics` 末尾按登记的**完整名字**原样导出；库内非 HTTP 的通道（ACME、TLS 握手、worker 崩溃，
  以及 Net 的 UDP、Database 的连接池）在对象构造时各自登记，应用不需要接线
- **自研内存与缓冲** — 协程帧内存池（`CoroutinePool`，重载 `operator new` 接入 `Task`）；可选 mimalloc 接管全局分配（`ASYN_WITH_MIMALLOC`）

**网络（Net）**

- **HTTP/1.1** — 手写增量解析器（含资源上限与分块编码）、Keep-Alive 持久连接
- **HTTPS** — TLS 握手 + ALPN 协商（h2 / http/1.1）、mTLS、证书热轮换
- **HTTP/2** — RFC 9113 帧与连接状态机、自研 HPACK（含 Huffman）、接收窗口流控、流式响应、GOAWAY；h2c 明文与 RFC 8441 扩展 CONNECT 隧道
- **出站 HTTP 客户端** — `HttpClient` 带按主机归组的 Keep-Alive 连接池，TLS 侧 ALPN 通告 `h2,http/1.1`；
  协商到 h2 时多条请求并发复用同一条连接（头块按最大帧负载切片、按流发送窗口、见顶前主动换连接），
  已经写上通路而没等到答完的请求**不重发**（非幂等请求做两遍比失败更坏）；`send()` 接受任意方法与
  附加头部，失败交回一句点明断在哪一段的中文原因（`std::expected`）；`HttpClientResponse::retryAfter()` 把 `Retry-After` 的两种写法（相对秒数与绝对的
  HTTP-date）折成「还要等多久」，读不懂交回空而不是 0 秒；`resolveUrlReference()` 把 `Location` 的相对写法（`/login`、`../x`、`?page=2`、
  `//cdn/x`）按 RFC 9110 §5.3 折成绝对 URL，非 http(s) 协议名、URL 内嵌凭据与含空白/控制字符的引用一律交回空而不是编造一个下一跳；
  出站响应的**尾部字段**与响应头部各存一张表（`HttpClientResponse::trailers` 与 `trailerValue(name)`），h1 的 chunked 尾段、h2 的尾段头块、h3 的尾段三条通道都收得下——
  混成一张表时消费方读不出「这是收完正文才知道的那个结果」，而按名回查单值视图又会把两段读出同一个答案；
  **过渡响应（1xx）**三条通道也一律「照解不照收」：h1 按 RFC 9112 §6.4 丢弃后接着解析、h2/h3 同样丢弃且不占「一条流唯一的最终响应」那个位置，
  带 END_STREAM 的 1xx、最终状态码之后不带 END_STREAM 的头块、只发过 1xx 就收尾这三种形状按 RFC 9113 §6.2/§8.1 落成**流错误** PROTOCOL_ERROR 而不是打死整条连接；
  失败时交回的那句原因点名真正断在哪一段——本端判定的畸形响应不再被说成「对端复位了这条流」，而对端复位时它给的应用层错误码也一路交上来；
  规范保证「这条请求没被处理过」的那几种收法（h2 的 REFUSED_STREAM 与 GOAWAY 通告值之上、h3 的 GOAWAY 与 H3_REQUEST_REJECTED）会让**非幂等**方法也敢换条连接重来一次，
  其余形状一律按「可能已经执行过」处置，绝不重发
- **HTTP/3 + QUIC** — 自研 QUIC 传输层（RFC 9000/9001：握手、流与流量控制、丢包恢复、NewReno 拥塞控制、持久拥塞重启与 ECN 处置（RFC 9002 §7.1/§7.6：出包带 ECT(0)、确认里报三个 ECN 计数、对端报的 CE 一涨就按一次拥塞事件降窗，不等判丢；Windows 既读不到 IP 头的 ECN 字段也没有按报文设它的入口，外壳把这条平台事实带进状态机，那一侧从一开始就不带标、也不报计数，见 `Platform::DatagramSocket::supportsPerDatagramEcnField()`）、1-RTT 密钥更新）+ 自研 HTTP/3 会话（帧层、QPACK 含动态表、流式正文、GOAWAY 优雅排空、RFC 9220 隧道）；同一个端口号的 UDP 上提供 h3。出站一侧也在：`HttpClient::setHttp3Enabled`（默认关；每条 https 出站先试 h3，探不通、起步没走完或那条流没答话都回落到 TCP，明文与流式上传恒走 TCP）。收包与发包都按批交付：一次就绪用 `recvmmsg` 收多条、一轮 flush 攒出的多个报文用 `sendmmsg` 交出（Windows 两个方向都没有批量入口、按逐条退化，交付语义同形而系统调用次数不同，见 `Platform::DatagramSocket::receiveBatch` 与 `sendBatch`）；报文大小不齐，所以发送侧不套 `UDP_SEGMENT`——GSO 要整批等长，为凑等长去补 PADDING 是把系统调用省在发包侧、把字节加在线路上
- **WebSocket** — RFC 6455 握手与帧编解码、文本帧的 UTF-8 两个方向都把关（对端发来非法序列的按 1007 收口，
  本端要发非法的那条当场拒——自己不收的帧不发）、分片重组、有界收帧队列、permessage-deflate（RFC 7692，
  按对端声明的窗口位数协商，本端无法履约就不接受该扩展而不是带着解不开的窗口开连接；要约里出现没定义的
  参数名或同名参数重复也按 §9.1 婉拒）；对端为什么关掉这条连接交回业务——`remoteCloseCode()` 读出对端 Close
  帧里的状态码原值、`remoteCloseReason()` 读出它给的原因文本（非法 UTF-8 的原因不外交给业务，本侧 `close()` 给出非法原因则当场拒——自己不收的帧不发）；
  h1 升级与 h2/h3 隧道共用协商，版本不合按 §4.2.2 回 426 并指明本端支持的版本；主题扇出归 `WebSocketHub`——订阅是
  RAII 句柄（析构即除名，不留悬垂对端指针）、一条连接同一时刻只有一个写者、每个成员自带待发队列的字节上界，
  越界丢**最新**的一条并从 `droppedMessageCount()` 读得到；文本与二进制两种帧各走各的通道（`publish()` /
  `publishBinary()`），队列里每条自带帧类型；被队满挡下的条数挂在 `/metrics` 的 `asyn_websocket_hub_dropped_messages_total`
  上（进程内多个集线器求和），构造时就登记，不等第一次丢弃才出现；对端收口或写失败而整队作废的条数是**另一本账**
  （`abandonedMessageCount()` / `asyn_websocket_hub_abandoned_messages_total`）——合成一条就分不出「该调上界」与「只是断连」
- **路由与中间件** — 精确匹配、参数化路径（`:id`）、通配符（`*`）、洋葱模型；命中的模式原文经 `HttpRequest::matchedRoute()` 交回业务与中间件，按路由分组打点不必自己再拼一遍
- **安全响应头** — `securityHeadersMiddleware()` 一处挂载给每条响应挂上 `X-Content-Type-Options: nosniff`、
  `X-Frame-Options`、`Referrer-Policy`，按需再加 CSP / Permissions-Policy / Cross-Origin-Resource-Policy；
  `Strict-Transport-Security` 只在**加密连接**上发（`HttpRequest::overTls()` 报的是本端这一跳的事实，
  刻意不看 `X-Forwarded-Proto`——那句谁都能写）。取值含 CR/LF/NUL 在构造期就抛（那类文案常由配置拼出来），
  配了 `preload` 却没配 `includeSubDomains` 同样当场拒绝；已经存在的头不覆盖，业务与外层中间件是取值的主人
- **HTTP Basic 认证（RFC 7617）** — `basicAuthMiddleware()` 按路径名单挂一道闸，凭据判定交给 `verify`
  （查表、查库、比对哈希都行，引擎不猜口令存在哪里）。默认**不在明文连接上收 Basic 凭据**：回的是 403 而不是
  401，因为 401 那句「请给凭据」等于邀请客户端把口令发进一条不加密的信道；判据取 `HttpRequest::overTls()`
  这一跳的本端事实，不看反代声明的头，要放开就显式置 `requireSecureTransport=false`。同名多条 `Authorization`
  按未授权处理，口令按**第一个**冒号切分（user-id 里不允许出现冒号，含冒号的口令因此能用而用户名不能），
  `realm` 含引号或 CR/LF/NUL 在构造期就抛——那是要拼进响应头的文本，留着就是拆分口子
- **观测与限额** — `/metrics`（Prometheus 文本 0.0.4）、`/healthz`、`/readyz` 与 `/debug/loops`（进程内每条事件循环一行的 JSON，看哪条被处理器占住）内建端点、状态码与延迟直方图统计、令牌桶限流、按来源 IP 并发限额
- **响应压缩** — gzip / zstd / br 协商（含 WebSocket 的 permessage-deflate）
- **按线程一个监听 socket** — `SO_REUSEPORT` 由内核分摊连接，避免 accept 单点
- **接受分发（跨平台多核扩展）** — 一个监听器接受、按轮转把连接交给 N 个工作循环，不依赖
  `SO_REUSEPORT`；Windows 上这是唯一可用的多核形态（`ConnectionDistributor` + `TcpServer::startAccepting()`）
- **接受连接的套接字调参** — `TcpServer::setSocketTuning()` 一处配置同时下发到监听套接字与每条接受的连接：
  收发缓冲上限、Linux 的延迟接受、TFO 队列长度，以及 **TCP 保活**（`SO_KEEPALIVE` 加空闲／间隔／次数三格时刻表）。
  保活兜的是半开会话：对端断电、拔线、中间 NAT 提前回收映射都不会发 FIN，没有它这条连接会一直占着描述符与
  每连接状态。不设就是系统默认（Linux 要空闲两小时才探第一次），设了才由内核在连续探测失败到达上限时报错回收
- **静态文件服务** — `staticFileDir()` 一行接入；条件请求一并给出（`ETag` + `Last-Modified`，
  `If-None-Match`/`If-Modified-Since` 成立回 304、`If-Match` 不成立回 412，判定顺序按 RFC 9110 §13.2.2），
  两种验证器比较规则公开在 `HttpConditionalValidators.h`，业务侧要判 PUT/PATCH 的前提读同一份；
  `staticPrecompressedVariants()` 打开后按 `Accept-Encoding` 直接发构建时压好的那份副本（`.zst` / `.br` /
  `.gz`，默认关闭），正文、长度、区间与验证器一律取自**实际发出去那一份**，所以两份表示各有自己的 ETag，
  跨变体的条件请求不会互相命中；开着这一格就无条件发 `Vary: Accept-Encoding`
- **大文件上传落盘** — 流式路由（`postStreaming()` / `putStreaming()`）边收边交，`RequestBodySpool::capture()`
  再把这条正文一段段写进临时文件：内存里永远只有当前那一段，写动作经 `Core::AsyncExecutor` 离开事件循环线程。
  超过上限或正文没收齐就**拒掉并删掉半份文件**（不把半份上传交给业务，也不在盘上留没人收的尾巴）；
  `release()` 之前文件随对象析构消失。注意它不改协议判定——`server.parser_limits.maximum_body_size`
  那道声明长度的闸照旧先落，要接更大的上传仍是先抬那一项，抬起来之后不再按连接吃内存才是这一格负责的
- **证书自动化（ACME / RFC 8555）** — `AcmeCertificateManager` 走完目录、账户、下单、自证、定稿与
  取证这一整台状态机：私钥与证书原子落盘（私钥 0600），到期前自主续，签好后经回调装回服务；
  协议层（`AcmeClient`）与密钥层（`AcmeKeyPair`：JWK、RFC 7638 指纹、JWS、CSR）都能单独用。
  h3 那一侧由 `QuicServer::reloadCertificate()` 承接同一个轮换动作，443/TCP 与同一端口的 UDP 不会一张新一张旧。
  自证支持 HTTP-01 与 DNS-01（RFC 8738）两种，一次签发只走一条：默认 HTTP-01，构造时交来 `AcmeDns01TxtWriter`
  就改走 DNS-01——后者**不需要任何入站通路**，被备案拦截或 80 端口不可达的部署也能自动续期，且这是通配域名
  （`*.example.com`）唯一可行的自证方式。TXT 写入动作交给你接的那家 DNS，仓库内自带阿里云云解析的实现。
  tls-alpn-01 仍不做：机构只给那一种时当场判失败并说明要哪一种，不会挑一条答不了的挑战去 POST

**数据（Database）**

- **SqlSugar 风格 ORM** — 结构体声明即表结构，`insert` / `toList` / `first` / `count` / `update` / 删除 / 批量插入；唯一键冲突可指定跳过或覆盖（`Queryable::InsertConflict`，关键词由方言给）；已有表可按结构体补上缺的列（`SchemaMigrator::addMissingColumns`，现有列直接问引擎，重复调用幂等）；非有限的浮点取值（NaN、±Infinity）在绑定前就拒并点名列名——它们落库之后是哪个数由引擎决定，而 `insert()` 会照样报成功，空值请写 `std::optional`（绑成 SQL NULL）
- **SQL 方言层** — 查询树渲染与参数收集只有一份实现（`StandardSqlDialect`），SQLite 与 MySQL 各自只覆写引擎知识；写语句与事务语句一律由方言生成，ORM 不含 SQL 拼接
- **参数化执行** — 取值一律以绑定参数送出，不拼进 SQL 文本（含引号、`--`、分号的文本只会被当作数据）
- **大结果集可以逐行取** — `DatabaseConnection::executeStreaming()` 把行留在服务端，每调用一次 `next()` 才取一行
  （MySQL 文本协议走 `mysql_use_result`），读一千万行与读十行不再是同一个内存量级。代价明写在契约里：结果集必须比
  连接短命、它消费完之前那条连接不再接受新命令、`rowCount()` 回 0 表示「未知」、不能重扫；没实现这条入口的驱动
  明确回「不支持」，不偷偷退化成整份预读
- **高性能连接池** — LIFO 复用、惰性创建、双机制清理（空闲回收 + 上限保护）；借出的连接可以
  `PooledConnection::discard()` 主动丢弃（驱动侧报过错、事务半路失败那一类脏会话），名额照旧腾出来
- **异步执行器** — 数据库阻塞调用挪出事件循环线程，完成后经 `Scheduler::scheduleRemote` 投回指定 `EventLoop`
- **事务与建表迁移** — `Transaction` RAII（析构未提交自动回滚），同一笔事务内可回退的段由保存点给出
  （`savepoint()` / `rollbackToSavepoint()` / `releaseSavepoint()`——两个引擎都不吃嵌套 BEGIN，
  回退到保存点不会结束事务）；`SchemaMigrator` 从表结构生成 DDL

**基础（Platform / Base）**

- **原生格式库** — JSON 与 YAML 直接使用 [nlohmann_json](https://github.com/nlohmann/json) 与 [yaml-cpp](https://github.com/jbeder/yaml-cpp) 的接口（DOM、Pointer/Patch、多文档、事件），不再自研解析与值模型
- **配置管理** — YAML/JSON 加载、目录递归装载、热重载（inotify / ReadDirectoryChangesW）
- **结构化日志** — 6 级、4 种 Sink（控制台/文件/滚动/异步）、C++20 `std::format`、源码位置
- **平台隔离** — 跨平台的系统能力收在 `Platform`（进程与信号、文件监听、文本编码、套接字地址、原子写）；两处例外是有意的：事件循环的三套后端与多进程看护直接打 Win32/POSIX（`Iocp` / `Epoll` / `Uring`、`WorkerSupervisor`、`GracefulShutdown`），它们与循环生命周期同生死，再抽一层只多一次间接

## 架构

![AsynGyanis 模块分层、挂载点与后端选择](assets/diagrams/png/layered-architecture-light.png)

> 交互版：[layered-architecture.html](assets/diagrams/layered-architecture.html)

| 模块 | 库 | 职责 |
|------|----|------|
| `Platform` | `AsynGyanis::Platform` | 描述符 / socket / 事件通知 / 定时器 / 文件监听 / 原子写 / 编码转换 / 进程与时间 |
| `Base` | `AsynGyanis::Base` | 日志、配置、异常层次、JSON/YAML 原生库的传递依赖 |
| `Core` | `AsynGyanis::Core` | 事件循环、协程运行时、socket、TLS、多进程编排 |
| `Net` | `AsynGyanis::Net` | TCP 服务基类、HTTP/1.1/2/3、WebSocket、QUIC、路由与中间件、ACME |
| `Database` | `AsynGyanis::Database` | 连接抽象、连接池、SQL 方言、ORM、建表迁移 |

模块内的子目录（如 `Base/Log/Sinks`、`Core/EventLoop`）**不引入新的命名空间**：命名空间一律到模块名为止（`AsynGyanis::Base`、`AsynGyanis::Core` …），include 路径从 `src/` 起算（`#include "Core/EventLoop/EventLoop.h"`）。

## 快速开始

### 前置依赖

- **CMake** ≥ 3.20、**Conan** ≥ 2.0（依赖与打包只走 Conan 这一条路线）
- **编译器**：MSVC ≥ 19.40 / GCC ≥ 13 / Clang ≥ 17（需支持 C++23 标准）
- **系统**：Windows ≥ 10 或 Linux（事件后端：Linux epoll、Windows 完成端口；Linux 另可用 `ASYN_WITH_IO_URING` 换 io_uring，需内核 5.6+）

第三方依赖由 `conan_provider.cmake` 在 CMake 配置阶段自动安装（`conan install --build=missing`），无需手工执行。

包管理**只维护 Conan 这一条路线**：`conandata.yml` 是唯一的一份依赖事实。曾加过根 `vcpkg.json`
与自带端口（`packaging/vcpkg/`），已删除——两条清单要并行维护同一批依赖，改依赖时漏改一边就会
分叉，而 CI 只跑 Conan，那份第二清单没有闸门看。同理删掉了为 vcpkg 的 brotli 端口导出名兜底的
`cmake/Findbrotli.cmake`：Conan 的 brotli 直接给 `brotli::brotli`，不需要退化路径。

依赖清单只有一份 `conandata.yml`（配 `packaging/conan/conanfile.py` 这份配方），改依赖只改这里，
CI 也只跑这一条路线。MySQL 驱动是配方里的 `with_mysql` 选项（默认关）：不带它时相关入口给一条
中文错误而不是编不过，因此「少一个可选依赖」从来不是构建失败的理由。

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

`release` 预设同样可用（关闭 sanitizer、开启优化，并保留能解析调用栈的调试信息）。

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

### 真机用例（数据库与证书机构）

依赖真实服务端的用例一律**环境变量门控**，口令无默认值、缺失即整组 `GTEST_SKIP`（不是失败），因此没有服务端的机器上仍然全绿：

| 服务 | 环境变量 | 用例位置 |
|------|---------|---------|
| MySQL | `ASYN_MYSQL_TEST_HOST/PORT/USER/PASSWORD/DATABASE` | `tests/Database/MySql/TestMySqlIntegration.cpp` |
| Redis | `ASYN_REDIS_TEST_HOST/PORT/USER/PASSWORD/DATABASE` | `tests/Database/Redis/TestRedisIntegration.cpp` |
| ACME 机构 | `ASYN_ACME_TEST_DIRECTORY_URL` / `ASYN_ACME_TEST_CONTACT_EMAIL` / `ASYN_ACME_TEST_ACCOUNT_STATE_DIR` | `tests/Net/Acme/TestAcmeLiveAuthority.cpp` |

ACME 那一条只走「取目录 + 建号 / 复用账户」，不签发证书（真签发要一台公网可达且解析到本机的域名，
HTTP-01 那条路在回归环境给不了）。三条变量缺一不可，其中 `..._ACCOUNT_STATE_DIR` 是**账户材料的持久目录**：账户建在机构侧
是有速率配额的公开动作，没有跨轮次持久的地方就把指针写进环境变量，一次回归多出一个真账户。
填 staging 端点（`https://acme-staging-v02.api.letsencrypt.org/directory`）不计入生产配额。
真签发的证据在 `tests/Tools/AcmeIssuanceProbe`：DNS-01 档不需要入站通路，2026-09-30 用它对着 Let's Encrypt
生产机构签出过 `gyanis.space` 与 `*.gyanis.space` 两张（`openssl verify` 对系统信任库通过），凭据走
`ASYN_ACME_DNS_ACCESS_KEY_ID` / `ASYN_ACME_DNS_ACCESS_KEY_SECRET`。

## 运行示例

![多进程 worker 的启停、补位与监听移交](assets/diagrams/png/worker-handoff-workflow-light.png)

> 交互版：[worker-handoff-workflow.html](assets/diagrams/worker-handoff-workflow.html)

`samples/ReferenceServer` 随构建一起编译（默认每线程一个监听 socket）：

```bash
./build/debug/samples/ReferenceServer --port 8080 --threads 4                 # HTTP
./build/debug/samples/ReferenceServer --https --cert cert.pem --key key.pem   # HTTPS
# 同一个端口号的 UDP 上再提供 HTTP/3（QUIC 自带 TLS，故需与 --https 同用）
./build/debug/samples/ReferenceServer --https --cert cert.pem --key key.pem --h3
# 一个监听器 + N 个工作循环，靠用户态分发而非 SO_REUSEPORT（Windows 多线程请用这个）
./build/debug/samples/ReferenceServer --port 8080 --threads 4 --dispatch-accept
# 每条工作循环线程绑一枚逻辑核（按线程池下标顺序占核，减少调度迁移；容器里按 cpuset 放行的核算）
./build/debug/samples/ReferenceServer --port 8080 --threads 4 --pin-threads
# N 个 worker 进程服务同一个端口，崩溃即补位（进程间不共享状态）
./build/debug/samples/ReferenceServer --port 8080 --workers 4
# 配置里的 acme 段开着就装常驻续期循环：签完的新证书热装回本进程每一台 TLS 监听器，
# 证书身份以 acme 的落点为准；多 worker 时签发只归槽位 0 那个进程，其余进程各跑一条跟盘协程
./build/debug/samples/ReferenceServer --config server.yaml --https
./build/debug/samples/ReferenceServer --help                                  # 全部参数
```

内建端点：`GET /`、`GET /json`、`GET /bench`、`GET /big`（256 KiB 可压缩正文，`--compress` 的验收对象）。

### 按模块的自检示例

`ReferenceServer` 是部署形态；能力按模块拆成了 12 个各自自检的程序（下面这张表就是那 12 行；条数以
`python scripts/run_samples.py --build build/debug --list` 现枚举为准——Windows 这一档只有 11 条自检程序
加 `ReferenceServer`，`CoreUpgrade` 只在 POSIX 构建），每个程序逐步打印 `✓`/`✗`，
并在 stdout 上留一行 `RESULT <名字> PASS|FAIL <步数> gated <跳过数>` 供脚本判定（退出码 0 表示全绿）：
`<步数>` 只算真正执行过的步，`gated` 单列因环境不齐备（真机凭据缺席这类）而跳过的步——
两者分开，「真机跑过」与「真机没跑」才不会给出同一条结论：

| 程序 | 覆盖 | 自检步数 |
| --- | --- | --- |
| `BaseLog` | Sink（控制台/文件/滚动/异步）、格式化器、注册表、配置驱动装配、异常带栈 | 25 |
| `BaseConfig` | 多文件加载与优先级、目录递归、点分路径取值、模式校验、热重载 | 29 |
| `PlatformPrimitives` | 描述符与套接字工具、通知器、定时器、内存映射、数据报、进程起停、编码与时间 | 36（Windows）/ 38（POSIX） |
| `CoreLoop` | 事件循环与调度器、定时器、IO 监听器、解析器、UDP/TCP 协程收发、协程池、取消令牌 | 14 |
| `CoreTls` | 证书装载与热替换、OCSP、私有 CA、回环握手与会话恢复 | 10 |
| `CoreWorker` | WorkerSupervisor 的构造期拒因；Windows 上另验「不给移交档位即拒且拒因点名叫 handoff」与「填了档位即放行」，POSIX 上验补位、崩溃上限、自行收手，以及「按请求收口」与「整池被放弃」两种收场各报对一次 | 6（Windows）/ 12（POSIX） |
| `CoreUpgrade` | 零停机换代：把正在监听的套接字交给新一代进程，端口全程不关（Windows 受完成端口的句柄级关联限制，只在 POSIX 构建） | 6（POSIX） |
| `NetHttpDemo` | HTTP/1.1 路由与中间件、静态文件目录、解析上限、分块与 SSE、WebSocket（回显与主题扇出的文本/二进制两种帧）、下一跳地址的解析、四类限额、接受分发、指标与健康端点、优雅收口 | 47 |
| `NetHttpsH2Demo` | 证书受信与不受信的对照、ALPN 协商 h2、h2c 明文、多路复用、GOAWAY 排空、解析上限 | 29 |
| `NetHttp3Demo` | QUIC 服务端的证书校验、UDP 起停与指定端口、乱码与畸形长头容错、定时驱动、统计、排空 | 14 |
| `NetUdpDemo` | UdpServer 的两种起步方式（按地址 bind / 接手别人绑好的口）、逐条交付与零长报文、主动下发、统计、处理器抛异常后的存活、两种顺序混用被拒、收口叫醒挂着的协程 | 13 |
| `DatabaseDemo` | SQLite 文件库/内存库、方言、ORM、事务、blob、参数绑定、连接池、异步链路；MySQL/Redis 按环境变量门控 | 82 + 2 门控（本机无凭据）；设了凭据时这两组真机步骤会展开成更多步 |

表内步数是两侧各自实测：Windows 一轮 `run_samples.py --repeat 2` 逐对一致，POSIX 侧在容器
`ubuntu24` 里跑同一份源码。两台的差只来自平台专属步骤（本轮逐条比对过步骤名）：`PlatformPrimitives` 在
POSIX 多三条（`sendfile` 零拷贝两条、fork 成功而 exec 失败的一条），在 Windows 多一条（启动不存在的
程序给出无效句柄而不是抛异常）；`CoreWorker` 在 Windows 只验构造期拒因，POSIX 才验补位、崩溃上限与
自行收手。`DatabaseDemo` 的两种写法是同一条命令的两种环境：设了 `ASYN_MYSQL_TEST_PASSWORD` 与
`ASYN_REDIS_TEST_PASSWORD` 之后，MySQL 与 Redis 两组步骤真跑（各自展开成多步），`gated` 归零。

一把跑完并汇总成矩阵（示例清单从构建目录里扫出来，新增程序不必改脚本）：

```bash
python scripts/run_samples.py                        # 全部跑一遍
python scripts/run_samples.py --only NetHttpDemo --repeat 3   # 重复跑，抓靠时序侥幸的用例
python scripts/run_samples.py --build build/release --timeout 300
```

要看「还有哪些代码没被触达」，用覆盖率构建（只在 GCC/Clang 侧，MSVC 没有 gcov 兼容工具链）：
就地重配一次并跑完测试与示例，再用 `gcovr` 出报告——没被触达的行与分支就是清单上的欠账，
示例里挂掉的每一步也都对应一处「公开面没被从库外驱动过」的地方。

```bash
cmake --preset debug -DASYN_ENABLE_COVERAGE=ON     # 就地重配，会触发一次全量重建
cmake --build build/debug -j 6
ctest --test-dir build/debug -j 6
python scripts/run_samples.py --build build/debug --timeout 300
pip install gcovr && gcovr --root . --filter 'src/' --print-summary
```

每次 `linux-ci.yml` 的 `coverage` 作业跑完都会把**当轮实测**打进作业摘要（阈值 60%，判据是 gcovr 自己的
`--fail-under-line`）；2026-09-30 在 `develop` 上的一次读数是**行覆盖 77%（29453/38230）**——这一档不带
MySQL/Redis 真库分支，也不跑示例，所以它与下面那次手工全量的口径不同，两个数都别拿去替对方说话。
2026-09-20 在 ubuntu24（GCC 13，ASan/UBSan 与 gcov 同开）跑完整套测试与全部示例后的实测：
**行 76.2%（19828/26034）、函数 83.9%、分支 44.6%**；分模块行覆盖 Net 89.2%、Core 86.4%、
Platform 86.2%、Base 71.9%、Database 54.2%。完全没被执行的只有 2 个文件——`MySqlResult.cpp`
（那个镜像里没有 MySQL 客户端库，驱动整块没进编译）与一个异常类的头；Database 偏低是两处门控
（MySQL/Redis 真机）与 ORM 模板未实例化的组合。也就是说：**能被从库外驱动到的公开面，示例现在都能触达**。
（用例条数只在下文「测试与验证」那一节按 ctest 名单现数一次，这里不再复写一份：写死的数会在下一次加用例那天
变成假话，而顶部徽章上那个总数就是那一行的 Windows 侧条数。要知道 Debug 与 Release 的条数本来就不同——
日志格式化的布局用例按 `NDEBUG` 分支编译，Debug 编进 8 条 `DebugBuild*`、Release 编进 3 条 `ReleaseBuild*`，
跨配置比数量前先看清是哪一种构建类型。）

## 代码示例

以下示例均取自 `samples/Net/ReferenceServer.cpp` 与 `tests/`，是当前代码里真实可编译的用法。

![一次请求从 accept、TLS/ALPN 到响应写出](assets/diagrams/png/http-request-sequence-light.png)

> 交互版：[http-request-sequence.html](assets/diagrams/http-request-sequence.html)

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

![配置、日志、指标与追踪四条数据流](assets/diagrams/png/config-log-dataflow-light.png)

> 交互版：[config-log-dataflow.html](assets/diagrams/config-log-dataflow.html)

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

### 证书自动化（ACME）

![ACME 证书从下单到续期的生命周期](assets/diagrams/png/acme-certificate-lifecycle-light.png)

> 交互版：[acme-certificate-lifecycle.html](assets/diagrams/acme-certificate-lifecycle.html)

```cpp
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Acme/AcmeCertificateManager.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Quic/QuicServer.h"

#include <expected>
#include <string>
#include <utility>

using namespace AsynGyanis;

/// 证书与私钥的落点：三台服务与自动化共用这两条路径，轮换按路径重读所以不必重启
constexpr const char *kCertificateFile = "certs/fullchain.pem";
constexpr const char *kPrivateKeyFile  = "certs/privkey.pem";

Core::Task<void> startCertificateAutomation(Core::EventLoop &loop)
{
    const auto httpAddress  = Core::InetAddress::resolve("0.0.0.0", 80);
    const auto httpsAddress = Core::InetAddress::resolve("0.0.0.0", 443);

    Net::HttpServer  http(loop, *httpAddress);
    Net::HttpsServer https(loop, *httpsAddress, kCertificateFile, kPrivateKeyFile);

    Net::QuicServer::Configuration quicConfiguration;
    quicConfiguration.certificateFile = kCertificateFile;
    quicConfiguration.privateKeyFile  = kPrivateKeyFile;
    Net::QuicServer h3(loop, std::move(quicConfiguration));

    Net::AcmeCertificateManager::Configuration acme;
    acme.domainNames              = {"shop.example.com"};
    acme.certificateFile          = kCertificateFile;
    acme.privateKeyFile           = kPrivateKeyFile;
    acme.accountKeyFile           = "certs/acme-account-key.pem";   // 不存在则新生成并写为 0600
    acme.accountStateFile         = "certs/acme-account.json";      // 记账户 URL，续期沿用同一个账户
    acme.directoryUrl             = "https://acme-v02.api.letsencrypt.org/directory";
    acme.contactEmailAddress      = "mailto:ops@example.com";
    acme.isTermsOfServiceAccepted = true;   // 接受条款是有法律含义的动作，库不替调用方默认

    // 签好之后装回服务：443/TCP 与同一端口的 UDP 各轮换一次，两条都成功才算装上
    Net::AcmeCertificateManager automation(loop, acme, [&] {
        const bool isTlsReloaded  = https.reloadCertificate();
        const bool isQuicReloaded = h3.reloadCertificate();
        return (isTlsReloaded && isQuicReloaded) ? std::expected<void, std::string>{}
                                                 : std::unexpected(std::string{"新证书没装回服务"});
    });

    // HTTP-01 的自证路由挂在明文 80 上（机构按域名解析到这台机器取令牌），须在 start() 之前注册。
    // 改走 DNS-01 就不需要这一步：构造 automation 时把第四个参数交给 makeAliyunDns01TxtWriter(loop, ...)
    // 那类动作对（发布与撤回 TXT），管理器会挑 dns-01 挑战并在每条出口之后把记录撤干净
    automation.registerChallengeRoutes(http.router());

    auto httpAccept = http.start();
    loop.scheduler().schedule(httpAccept.handle());
    co_await h3.listen(*httpsAddress);

    // 首轮：磁盘上那张还够用就不去打扰机构，只把它的落点与到期时刻交回来
    auto issued = co_await automation.issueIfRequired();

    // 常驻续期：协程帧交给调用方持有到退出，退出前先 stopRenewalLoop()
    auto renewal = automation.runRenewalLoop();
    loop.scheduler().schedule(renewal.handle());
    co_return;
}
```

`status()` 是跨线程可读的运维读数（上次签发结果、失败原因、到期时刻、失败退避门槛、当前暂存的令牌数，
以及 `isReloadPending`——「盘上已是新的一张、线上还没换上」这一格单独有出口，因为到期判据看不见它），
适合按实例查的口；四条计数（`asyn_acme_certificate_expiry_seconds` / `asyn_acme_issuances_total` /
`asyn_acme_failures_total` / `asyn_acme_reload_pending`）则在构造时就登记进 `Core::ProcessMetricsRegistry`，`/metrics` 末尾自动带出、
不需要应用接线，两条通道读的是同一份原子量。`runRenewalLoop()` 在没有装回服务动作时**拒绝启动并把原因记进 `status()`**，
不会静默地只往磁盘上写——磁盘上的证书每月在换、线上身份永远是那张旧的，是这类自动化最坏的失败形状。
下一轮什么时候再试由 `nextRenewalDelay()` 一处算出（取「查到期节拍」与「失败退避」里更晚的那个，
但欠着一次装回时压回短片：那种情况下等到 12 小时不是保守而是漏掉），循环自己与运维读数读的是同一个出口。

### 证书自动化的配置段（acme）

上面那七八个字段此前只能逐台手接，现在 `acme` 段可以整份交给 `Net::readAcmeConfiguration(root)`：

```json
{
  "acme": {
    "enabled": true,
    "directory_url": "https://acme-v02.api.letsencrypt.org/directory",
    "domains": ["shop.example.com", "*.shop.example.com"],
    "certificate_file": "certs/chain.pem",
    "private_key_file": "certs/domain-key.pem",
    "account_key_file": "certs/acme-account-key.pem",
    "account_state_file": "certs/acme-account.json",
    "contact_email": "ops@example.com",
    "tos_accepted": true,
    "challenge": "dns-01",
    "renew_before_expiry_days": 21,
    "dns": { "provider": "aliyun", "domain": "shop.example.com", "record_ttl_seconds": 600 }
  }
}
```

三条口径值得单独说：

- **凭据不认配置文件**。`dns.provider` 只说明用哪一家的动作，AccessKey 一律从
  `ASYN_ACME_DNS_ACCESS_KEY_ID` / `ASYN_ACME_DNS_ACCESS_KEY_SECRET` 读，缺任何一条就在
  `buildDns01TxtWriter()` 当场拒——一把能改域名记录的钥匙进了版本库，等于把域名交出去。
- **`dns` 段与 `challenge` 互为条件**，两个方向都拒：写了 `dns` 却没走 dns-01 是有人改了其中一格忘了另一格，
  按字面继续跑会让人以为 TXT 在写。
- **段内未知键即拒**（13 个键 + `dns` 那 3 个），与 `server` 段同一套规矩；键名打错不该安静地按默认跑。

消费方有两处。签发探针 `acme_issuance_probe --config <file>` 以文件那份为默认，命令行上**显式给出**的
`--domain` / `--contact` / `--challenge` / `--state-dir` / `--dns-zone` / `--dns-ttl` 才覆盖它，并打一行
`CHALLENGE … FROM cli|config` 说明这次是哪份在生效。`ReferenceServer --config <file>` 则把这一段装成
**常驻续期**：本进程每一台 HTTPS 监听器与 h3 都在装回名单里，签完的新证书当场 `reloadCertificate()` 换上，
启动日志直接给出「装回目标 N 台 HTTPS 监听器 + HTTP/3 在/不在」——那个 N 按真能转成 TLS 服务器的对象数，
不按 `--https`/`--h3` 两个开关猜，所以「开关说了而对象不在」这一格当场可见。

三条边界都写成启动即拒（或启动即说清），因为它们全是「配了但不会生效」的形状：

- **开着 `acme` 时证书身份以 `acme` 的落点为准**：`--cert/--key` 与它不一致会被顶掉并打一行 WARN 说明顶掉了
  什么。两边不同解就等于签完装不回去——`reloadCertificate()` 按监听器原来那条路径重读，新那张写在别处。
- **`http-01` 在本示例里装不下去**：它要一台公网可达的明文监听器，而这里一个端口只服务一种协议，开了
  `--https` 就没有明文口，于是启动被拒并指回 `dns-01`；`http-01` 的一次性签发仍归探针（它自带明文对端）。
- **多 worker 进程：签发只归槽位 0 那个进程，其余进程跟盘**。机构的速率限制按账户计而不是按进程计，
  N 份管理器各撞一次额度毫无意义；而签发进程落盘是原子替换，「那张文件的身份变了」就是它唯一需要发出的
  通知——其余进程用 `Net::followCertificateRotation` 每 15 秒读一次 (字节数, 修改秒, 身份标记) 三元组，
  任一不同就重装本进程的 TLS 监听器。跟盘的进程因此**不需要**那对能改域名记录的凭据，凭据只进真正下单的
  那一个进程；而装配判据挂在「本进程会不会自己跟盘」这件能力上，不挂在进程数上——两条机制都没装的部署
  照样在启动那一刻被拒。

首次签发不在服务侧：落点上还没有可加载的身份时 `ReferenceServer` 启动即失败并说明「先用
`acme_issuance_probe` 跑一次」，而不是把一句 OpenSSL 的 PEM 解析错误留给人猜。停机排在停服务器之前：
先叫停 ACME 的常驻协程（续期循环与跟盘协程各一条），再等它们**真的**退出（有界——停放一片 1 秒，等待上限
5 秒；正在进行的那一轮签发刻意不打断，半途掐掉会留下半个订单状态），帧不会带着一个已销毁的管理器被丢下。判据本体在
`Net::validateAcmeAssembly`，自己的装配出口可以直接复用它。

> **一份单里同时写 `example.com` 与 `*.example.com` 时，第二次自证会先等满记录 TTL。**
> RFC 8738 让这两条自证落在**同一个**名字 `_acme-challenge.example.com` 上，而两条的答案不同；机构按
> 那条记录的 TTL 缓存答案，先撤后写之间没等满就会读到上一条，原文只回
> `Incorrect TXT record "…" found at _acme-challenge.example.com`。实测这个区把 TTL 压到 60 或 300 秒都会被 API
> 直接拒（`QuotaExceeded.TTL`，地板是 600 秒），所以「调小 TTL」这条路在免费区上不成立——
> 写入器改成记住刚撤过的名字、同名重写之前等满 TTL（上限 15 分钟）。这段等待只在进程内记，
> 所以**接连重跑两次签发之间也要隔过 TTL**，否则机构缓存的还是上一轮那条。

## 模块概览

**支持范围**：目前只支持 Linux 与 Windows——顶层 `CMakeLists.txt` 对其他系统（含 macOS/BSD）在
配置阶段直接 `FATAL_ERROR`，`Core` 的事件后端只有 epoll、io_uring（编译期可选）与 IOCP，没有 kqueue。

**交付形态**：默认静态库，`-DBUILD_SHARED_LIBS=ON` 让五个模块各出一份 `.so`/`.dll`。这条开关是 CMake
直接配的（双端、静态与共享两档都有门禁）；**Conan 那条包路线目前只出静态包**——配方写着
`package_type = "static-library"` 且不提供 `shared` 选项，因为包这一侧没有会跑共享档的作业，加一个
没人验过的开关等于造死配置。导出面由 `src/AsynGyanisExport.h` 的 `ASYN_<模块>_API` 逐类标注决定，
配合隐藏可见性预设——没标注的符号出不去，所以下一次重构不会不知不觉换掉线上符号。三条边界要知道：

- 接口上的 `std::string` / `std::vector` 跨 ABI 边界，生产方与消费方必须是同一套编译器、同一份 CRT
  与同一套标准库配置；要一份能被别的工具链装载的二进制，得先把接口上的标准库类型换掉。
- OpenSSL 静态链接在库与消费方两侧各有一份，**别把 `TlsContext::nativeHandle()` 交给消费方自己的
  OpenSSL 代码**——两份实例各有错误队列，跨实例的 `SSL_CTX`/`SSL` 用法不是可依赖的接口。
- Windows 上共享形态把 DLL 与可执行体一起收在 `build/bin/`（Ninja 生成器不替你做这一步，缺了就是
  启动即 `0xC0000135`）；静态形态的落点一字未动。

### Platform — 平台底层（目标 `AsynGyanis::Platform`）

| 分类 | 内容 |
|------|------|
| 描述符与 IO | 文件描述符 RAII、socket 初始化与选项、事件通知（Linux eventfd / Windows socketpair） |
| 文件系统 | 文件读写、原子写、目录遍历、文件监听（inotify / ReadDirectoryChangesW） |
| 系统 | 错误码与可读原因、进程与目录、环境变量、本地时间、UTF-8 ↔ UTF-16 与代码页 |

### Base — 基础设施（目标 `AsynGyanis::Base`）

| 分类 | 内容 |
|------|------|
| 异常 | `Exception` 层次（携带 `source_location`），配置与网络各自的错误类型 |
| 日志 | `Logger` / `LoggerRegistry`、6 级 `LogLevel`、`LogEvent`、`LogMacros`、4 种 `LogSink`（控制台/文件/滚动/异步）、三种格式化器（`formatter` 按 sink 配：`default` / `color` / `json`，写错按 default 处理并打一行诊断）、从配置装载日志设置 |
| 配置 | `ConfigManager`（多文件/目录装载、热重载、严格类型化取值）、文件监听；`ConfigValue` 即 nlohmann_json 文档 |
| 格式 | JSON 与 YAML 使用 nlohmann_json / yaml-cpp 的原生接口，由 `Base` 传递依赖（见「外部依赖」） |

### Core — 异步运行时（目标 `AsynGyanis::Core`）

![事件循环一轮与协程调度归属](assets/diagrams/png/runtime-kernel-sequence-light.png)

> 交互版：[runtime-kernel-sequence.html](assets/diagrams/runtime-kernel-sequence.html)

| 子目录 | 内容 |
|--------|------|
| `EventLoop/` | `IoContext`（运行时入口）、`EventLoop`、`IoWatcher`、`TimerQueue` / `Timer`、三后端 `Epoll`（Linux）/ `Iocp`（Windows）/ `Uring`（可选）、`ConnectionDistributor`（接受分发） |
| `Coroutine/` | `Task<T>`、`Scheduler`（本地队列 + 全局队列）、`ThreadPool`、`CoroutinePool`、`Cancelable`、`AsyncExecutor`（阻塞/CPU 密集活挪出循环线程） |
| `Socket/` | `AsyncSocket`、`AsyncUdpSocket`、`AsyncResolver`、`VectoredSendCursor`、`InetAddress`、`Connection`、`ConnectionManager` |
| `Tls/` | `TlsContext`、`TlsSocket` |
| `Process/` | `WorkerSupervisor`（多进程 worker 的启停与看护） |
| `Exception/` | Core 侧异常类型 |
| `Crypto/` | 摘要与 HMAC（`Digest`：`hmacSha1`/`hmacSha256` 等，ACME JWS 与云解析签名用的那层 OpenSSL 胶水；没有独立的 `Hmac` 头文件） |
| `Metrics/` | `ProcessMetricsRegistry`（进程级读数的登记处：RAII 把手 + 同名并法，由 Net 的 `/metrics` 渲染点按完整名字导出） |

### Net — 网络应用层（目标 `AsynGyanis::Net`）

![自研 QUIC 连接的相位、空间与恢复](assets/diagrams/png/quic-connection-lifecycle-light.png)

> 交互版：[quic-connection-lifecycle.html](assets/diagrams/quic-connection-lifecycle.html)

| 子目录 | 内容 |
|--------|------|
| `Tcp/` | `TcpAcceptor`（`SO_REUSEPORT` 监听）、`TcpStream`（`readExact` / `readUntil` / `writeAll`）、`TcpServer` |
| `Udp/` | `UdpServer`（一条端口面对任意来源：逐条交付报文、按来源回包、主动下发） |
| `Http/` | `HttpRequest` / `HttpResponse` / `HttpMethod`、`HttpParser`（手写增量解析）、`Router` 与 `Middleware`、`HttpSession` / `HttpServer`、`HttpsServer`（TLS 一侧不另设会话类：`HttpsServer` 统一建 `Http2/` 里的 `Http2Session`，握手完成后按 ALPN 结果跑 h2 循环或同一份 HTTP/1.1 事务循环）、`FileSender`（静态文件）、`SseStream`、`HttpMetricsEndpoint`、`HttpMemoryBudget`、压缩协商（`Gzip` / `Compression`）、`Client/`（`HttpClient`、`HttpOutboundConnectionPool` 与响应解析器） |
| `Http2/` | `Http2Session` / `Http2Connection`、`Http2ClientConnection`（出站一侧的帧与 HPACK）、`Http2Frame`、`Hpack`（含 Huffman） |
| `Http3/` | `Http3Session` + 自研帧层 / QPACK / `Http3Connection`（含 RFC 9220 隧道） |
| `Quic/` | 自研 QUIC 传输层：`Codec/`（变长整数、报文头、帧、传输参数）、`Crypto/`（密钥调度、头/包保护、TLS 胶水）、`Recovery/`（RFC 9002 丢包恢复、NewReno、ECN 验证与持久拥塞重启）、`Streams/`（流与流量控制）、`QuicConnectionCore`（状态机）、`QuicPacketBuilder`、`QuicServer` / `QuicConnection`（数据报路由与外壳） |
| `WebSocket/` | `WebSocketHandshake` / `WebSocketFrame` / `WebSocketPeer`、`WebSocketUtf8`、`PerMessageDeflate` |
| `Acme/` | `AcmeKeyPair`（账户与域名密钥、JWK 与 RFC 7638 指纹、RS256/ES256 的 JWS 签名、CSR）、`AcmeClient`（RFC 8555 状态机：目录 / 账户 / 下单 / 自证 / 定稿 / 取证）、`AcmeHttp01ChallengeStore`（令牌暂存与路由注册）、`AcmeCertificateManager`（到期判定、原子落盘、常驻续期循环与装回服务的回调） |

### Database — 数据访问（目标 `AsynGyanis::Database`）

![一次 ORM 查询从表达式到行对象](assets/diagrams/png/orm-query-sequence-light.png)

> 交互版：[orm-query-sequence.html](assets/diagrams/orm-query-sequence.html)

| 子目录 | 内容 |
|--------|------|
| `Common/` | `DatabaseConnection` / `DatabaseResult` / `DatabaseValue` 抽象、`ConnectionConfig`、`DatabaseType`、`DatabaseFactory` |
| `Dialect/` | `SqlDialect` 契约、`StandardSqlDialect`（共用渲染）、`SqliteDialect`、`MySqlDialect`、`SqlStatement`、`ColumnType`、`DialectRegistry` |
| `Pool/` | `PoolConfig`、`PooledConnection`（RAII 租约）、`ConnectionPool`、`Transaction` |
| `Queryable/` | `Column` / `TableSchema` / `QueryNode` / `Expression`、`Queryable<T>`、`RowMapper`、`SchemaMigrator` |
| `Sqlite/` `MySql/` `Redis/` | 三种驱动实现（可选依赖缺失时退化为「每个入口给中文错误」的桩） |

## 目录结构

```
AsynGyanis/
├── CMakeLists.txt          # 顶层：C++23 设置 + sanitizer / mimalloc / io_uring 开关 + add_subdirectory
├── CMakePresets.json       # debug / release 预设（debug 带 AddressSanitizer）
├── conanfile.py            # 依赖清单由 conandata.yml 驱动
├── conandata.yml           # 第三方依赖与版本
├── conan_provider.cmake    # CMake 侧自动触发 conan install
├── cmake/                  # 包配置模板（`AsynGyanisConfig.cmake.in`）、编译期特性头模板、库形态与导出宏装配
├── assets/diagrams/        # 架构图：候选 JSON、可探索 HTML 与预览 PNG
├── samples/                # 按模块拆开的自检示例 + ReferenceServer（部署形态），总跑见 scripts/run_samples.py
├── benchmarks/             # 性能基线与门禁脚本、热路径微基准、进程外压测脚本
├── packaging/conan/        # Conan 库包配方与消费方冒烟测试
├── scripts/                # 发布版本一致性门禁、示例总跑、跨实现验收探针（QUIC/h3/WS/h2/ACME）
├── src/
│   ├── Platform/           # 平台底层（进程 / 文件监视 / 文本编码 / 地址）：IO / FileSystem / System
│   ├── Base/               # Coding / Config / Exception / Log
│   ├── Core/               # Coroutine / Crypto / EventLoop / Exception / Metrics / Process / Socket / Tls
│   ├── Net/                # Acme / Http / Http2 / Http3 / Proxy / Quic / Tcp / Tracing / Udp / WebSocket
│   └── Database/           # Common / Dialect / Pool / Queryable / Sqlite / MySql / Redis
└── tests/                  # 与 src 逐级对齐的 GoogleTest 测试；另有 TestSupport/（共用的循环与夹具）、
                            # Tools/（进程外探针）、Net/Fuzz/（模糊目标）、Core/fixtures/（真机脚本）
```

## 外部依赖

| 库 | 版本 | 用途 |
|----|------|------|
| [nlohmann_json](https://github.com/nlohmann/json) | 3.12.0 | JSON 值模型与解析/序列化（Base 公开接口） |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.9.0 | YAML 解析（配置加载） |
| [OpenSSL](https://www.openssl.org/) | 3.6.2 | TLS/HTTPS，兼作 QUIC 的加密胶水 |
| [zlib](https://zlib.net/) / [zstd](https://facebook.github.io/zstd/) / [brotli](https://github.com/google/brotli) | 1.3.1 / 1.5.7 / 1.1.0 | 响应正文与 WebSocket 压缩 |
| [mimalloc](https://microsoft.github.io/mimalloc/) | 3.5.1 | 可选全局分配器（`ASYN_WITH_MIMALLOC`） |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | 单元测试 |
| [SQLite3](https://www.sqlite.org/) | 3.51.3 | 嵌入式数据库驱动 |
| [hiredis](https://github.com/redis/hiredis) | 1.3.0 | Redis 客户端 |
| [libmysqlclient](https://dev.mysql.com/doc/c-api/) | 8.1.0 | MySQL 客户端 |

- YAML 与 JSON 使用 nlohmann_json 与 yaml-cpp（均为必选依赖，随 `Base` 公开传递其头文件与链接）。YAML 转配置值模型的口径：引号标量按字符串、`!!str/!!int/!!float/!!bool/!!null` 之外的自定义标签直接报错、重复键报错、别名展开设深度与节点总数上限。
- [wepoll](https://github.com/piscisaureus/wepoll) 的**头文件**（`src/Core/EventLoop/wepoll.h`）随仓库分发，供
  Windows 完成端口后端（`Iocp.cpp`）借用 `epoll_event` 与事件位定义；AFD 轮询实现已删除，不再参与轮询。
- SQLite3 为必选；hiredis 与 libmysqlclient 为**可选**：探测不到时对应驱动退化为报错桩，不会让配置阶段失败。

## 投产前核对

下面这些项都是「库不会替你决定，但配错了要出事故」的那一类。每条都写了默认值与**怎么确认它真的生效**——
静默保持默认值看起来总像是配置成功了，所以别只看配置文件，要读回来或抓一次端点。

| 核对项 | 键 / 入口 | 默认值 | 怎么确认生效 | 配错的后果 |
| --- | --- | --- | --- | --- |
| TLS 下限 | `Core::TlsPolicy::minimumProtocolVersion`（出站走 `HttpClient(loop, poolConfig, tlsPolicy)`） | 服务端 TLS 1.2；QUIC 恒 1.3；**客户端角色不补下限**（刻意：替调用方发明下限会把本可以连上的对端拒掉） | `TlsContext` 建好后读 `SSL_CTX_get_min_proto_version`，或抓一次握手看协商版本 | TLS 1.0/1.1 没有档位可填（RFC 8996 已废弃）。要给出站也钉下限，就显式传 `minimumProtocolVersion` |
| ACME 联系人 / 条款 | `AcmeCertificateManager::Configuration::contactEmailAddress` / `isTermsOfServiceAccepted` | 联系人为空；条款未接受时**新建账户直接拒绝** | 看 `status()` 与账户 URL 是否落盘 | 没有联系人 = 机构无法在到期或账户异常时找到你；90 天寿命的证书漏续一次就是一次线上告警 |
| `acme` 配置段 | `Net::readAcmeConfiguration(root)` + `Net::buildDns01TxtWriter(loop, cfg)` + `Net::validateAcmeAssembly(cfg, facts)`；消费方是签发探针 `acme_issuance_probe --config <file>` 与 `ReferenceServer --config <file>`（后者装常驻续期循环，签完的新证书热装回本进程每一台 TLS 监听器） | 整段缺失 = `enabled` 为 false，谁都不去签；`challenge` 默认 `http-01`、`dns.record_ttl_seconds` 默认 600（下限也是 600：这一家实测拒更小值，配低了在**读配置**时就拒，不留到第一次签发才在 API 上报 `The specified TTL is invalid`）、`renew_before_expiry_days` 30、`renewal_check_interval_minutes` 720 | 探针打一行 `CHALLENGE <种类> PROVIDER … ZONE … TTL … FROM cli\|config`，`FROM config` 才说明文件里那份在生效；`--domain` / `--contact` / `--challenge` 显式给出时才覆盖文件。服务侧另打一行「证书自动化：开（… 装回目标 N 台 HTTPS 监听器 + HTTP/3 在/不在）」，N 按真能转成 TLS 服务器的对象数，不按开关猜 | 段内未知键当场拒（13 键 + `dns` 那 3 键）；`dns` 段与 `challenge: http-01` 同时出现两边都拒；**AccessKey 刻意不认配置文件**，只从 `ASYN_ACME_DNS_ACCESS_KEY_ID` / `_SECRET` 读，缺一条就在建写入器时拒——能改域名记录的钥匙进版本库等于把域名交出去；服务侧的三条边界同属「配了不生效」这一族，一律启动即拒或启动即说清：开着 `acme` 时证书身份以 `acme` 落点为准（`--cert/--key` 不一致会被顶掉并打 WARN）、`http-01` 在本示例没有公网明文口可用（一个端口只服务一种协议，拒并指回 `dns-01`）、多 worker 进程里签发归槽位 0、其余进程靠 `Net::followCertificateRotation` 跟盘（两条都没装的部署照样拒）|
| 限额与背压 | `server.parser_limits.*`、`server.limits.*`、`maximum_connections`、`maximum_connections_per_ip`、`memory_budget_bytes`、`rate_limit.*` | 头部 100 条 / 单值 8 KiB / 头块 64 KiB / 正文 8 MiB、入站 WebSocket 消息 8 MiB（`maximum_websocket_message_size`：一个数同时管单帧声明长度、分片重组后的总长与 permessage-deflate 的解压输出上限，0 = 不限；三条通道——h1 的 101 升级、h2 的扩展 CONNECT、h3 的 CONNECT 隧道——吃同一份配置）；**条数、请求目标长度与单条头名/头值长度这三道闸门在三条通道上同一判据**（h1 由解析器判，h2/h3 在 intake 判，伪头也算一条；条数与头块字节都按**整条报文累计**——尾部头块那场加在头部之上一起判，h2/h3 的流式路由是明写的例外：请求已派发，越限只拦得住尾字段而回不出 431；h2 的两条长度闸门由 HPACK 解码器执行，上限取自 `parser_limits` 而非该层自己的常数；头块**字节**数三条通道都判这一把尺——h2 在 intake 按名与值的净字节判，另有 `setHttp2Configuration()` 那个随 SETTINGS 通告出去的值按 §6.5.2 的「每条 +32」并排判，取更紧的一方生效，两者出厂同为 64 KiB）；空闲 75s、读写各 60s；**并发默认是有限值**：每监听器 4096、单来源 256（写 0 才是显式不限）；`requests_per_second` 与在途正文总量仍默认 0 = 不限——限流给默认值会误杀真实用户，方向不对 | `/metrics` 的 `asyn_http_admission_rejected_connections_total`（按 IP 挡）与 `asyn_http_over_limit_rejected_connections_total`（整机满）；开了总量账之后还有 `asyn_http_inflight_body_bytes`（当前在途正文字节）与 `asyn_http_memory_budget_rejections_total`（被预算拒掉的预留次数）；令牌桶打开后超限回 429；`ReferenceServer` 启动打两行读数报的才是生效值：「并发限额：整机 … 摊给 N 个进程 → 每进程 …」与「并发限额（每监听器生效值）：整机 … 摊给 N 个进程 × 每进程 L 条监听器 → 每台 …」——前者是一个进程里多台共用的那份账按的分母，后者是每台自己计数的那几项按的分母，只印一行就会有一行是假的；**分母也在 `/metrics` 上**（`asyn_http_maximum_connections` 与 `asyn_http_maximum_connections_per_ip`，两条 gauge，0 分别是「不设这道限」与「那道闸门没装」）——只有分子时「离上限还有多远」这类预警画不出来，得等到真拒了连接才看得见；两条分母是**本监听器**的口径，而分子 `active_connections` 在多台监听器共用一份采集端时是**相加的那一份**，两者不同源，别直接相除当负载率 | 显式写 0 = 不限是把内存和连接表交给对端，要写就得写下理由；单来源那道 256 是给共享出口（运营商级 NAT、企业代理）留的余量——真被撞到的部署应按实测并发抬高它，而不是把闸门关掉；`--h3` 那侧另有一份 `QuicServer` 自带的默认 1024（单位连接更贵，配额刻意不同），示例只在摊后的份额为正时覆盖它。**多进程时配置写的是整机口径**：装配出口按 `workerProcessCount × listenersPerProcess` 向上取整摊到每台（`perProcessShare`，两个分母各管一类：每台自己计数的限额乘台数，而一个进程里多台共用的那份共享对象只按进程数摊——乘错了会把闸门压到 1/L），本台真正卡多少可问 `TcpServer::maximumConnections()`；启动时打**两行**读数，因为两类限额的分母不同：一行是「整机 100 摊给 2 个进程 → 每进程 50」（一个进程里多台共用的那份共享账按它），另一行是「整机 100 摊给 2 个进程 × 每进程 4 条监听器 → 每台 13」这种每台真正卡多少；`rate_limit` 走同一套摊分但**取整方向相反**——速率精确除（0.5 请求/s 不能被抬成 1），桶容量向下除后兜在 1.0（容量不足一枚令牌的桶一个请求都放不出），而递进装配出口的桶若不是摊后那一份会当场拒；`memory_budget_bytes` 与连接数同向（向上取整摊到每台），装配出口把摊后的那一份落到 `setMemoryBudget()`，示例再把同一个对象递进 `QuicServer::Configuration::memoryBudget`，于是三条通道共用一份账——命令行 `--max-inflight-body` 是它的覆盖，口径同为整机 |
| `/metrics` 接线 | `applyHttpServerConfiguration()` + `server.expose_metrics`（令牌：`server.ops_bearer_token`；程序侧 `enableMetricsEndpoint()` / `enableHealthEndpoint()` / `enableReadinessEndpoint()` / `enableLoopDiagnosticsEndpoint()`） | 关（一个端点都不注册）；不开令牌时四面都不鉴权 | 直接 `curl` 四个端点：`/metrics`、`/healthz`、`/readyz`、`/debug/loops`；配了令牌后要带 `Authorization: Bearer <token>` 才回 200 | `/metrics` 与 `/debug/loops` 读得到内部计数与每条循环的状态，开到 `0.0.0.0` 就是公开暴露；`ops_bearer_token` 给这两个加 Bearer 闸门（`/healthz` 与 `/readyz` 刻意不挡——存活探针与就绪探针都要能被编排器无凭据访问，给探针加令牌只会让人把探针关掉）。`/readyz` 只问「还在不在接新连接」：`stop()` / `drain()` 之后回 503 + `retry-after: 1`，正在排空在途请求的那段就是编排器把这台摘出负载的窗口，而这段里 `/healthz` 仍回 200（进程还在转，把它当存活失败杀掉才会真丢请求）。令牌只能写在配置文件里：命令行上的令牌会进 shell 历史与进程列表。来源本身的收口要靠只听回环的管理口（见下一行） |
| 非 HTTP 那侧的读数出口 | `Core::ProcessMetricsRegistry::registerMetric(...)`（RAII 把手，析构即注销），渲染点在 `/metrics` 末尾按登记的**完整名字**原样导出 | 各模块的对象构造时就登记，不需要应用接线；没有登记过就没有那一行（不是 0） | 抓一次 `/metrics` 看名字在不在：`asyn_acme_certificate_expiry_seconds` / `asyn_acme_issuances_total` / `asyn_acme_failures_total`（证书自动化）、`asyn_acme_dns01_*`（dns-01 写入的条数与花掉的秒数）、`asyn_udp_*`（数据报四条计数）、`asyn_worker_crashes_total` / `asyn_worker_slots_given_up`、`asyn_db_pool_*`（在借 / 等待 / 当下持有 `connections_held`（旧名 `connections_created_total` 是同值别名）/ 已丢弃 / 借出超时 / 会话没复位干净 `session_reset_failures_total`——它是「已丢弃」的异常子集，另两条去向属正常生命周期）、`asyn_executor_pending_tasks` / `asyn_executor_rejected_total`（阻塞任务执行器：排队条数与因队满被拒的条数，每台执行器各报一份、抓取时求和；后一条涨了说明该降并发或加工作线程，提交方当场收到的是异常）、`asyn_tracing_exported_spans_total` / `asyn_tracing_dropped_spans_total` / `asyn_tracing_export_failures_total`（链路追踪：已交给出口的与被丢的都按**条**计，出口整批没收的按**批**计，最后一条与前两条口径不同、不能相减；而「已交给出口」是**已提交**不是已送达，采集端那一段的得失看出口自己的读数）、`asyn_tls_handshakes_total` / `asyn_tls_session_reused_total`、`asyn_quic_datagram_batches_total` / `asyn_quic_datagrams_received_total`（h3 收包侧：批次调用数与其中交付的报文条数，两笔相除才是「一次就绪收了几条」——只报条数看不出负载，只报批次数看不出规模；Windows 上没有批量入口、按逐条退化，比值照读，省下的系统调用数只在 Linux 侧发生）、`asyn_quic_datagram_batches_sent_total` / `asyn_quic_datagrams_sent_total`（h3 发包侧同形两笔：一轮 flush 攒出的多个报文按窗交出，每窗一次调用；两笔相除读的是「一次调用交了几条」，报文大小不齐所以不套 `UDP_SEGMENT`） | 三条口径容易读错：① 这些名字**不套**监听器的 `metric_name_prefix`，抓哪台都是同一份进程量；② 同名多实例按登记时给的并法合（计数求和；到期时刻取**最早**那张，因为它是会先出事的那个——但证书那条走的是「跳零求早」：某一格还没签出时交回的是 0，含义是「读不出」而不是「最早」，让它参与求早会把另一张的真值盖成一条假告警）；③ 长期为 0 就是要报的事——证书自动化没跑成与还没跑，从面板上看是同一个形状；到期时刻那条在构造时就按磁盘上现有那张填过了，所以计划内重启不会先报一段假的 0，它读出 0 就是那条路径上真没有读得出的证书。**有两类读数刻意不接**：跨线程不安全的对象（出站连接池明写「协程挂起期间被别的线程驱动会踩坏套接字状态」，抓取在另一条线程上）与「读口本身带副作用」的（`WorkerSupervisor::runningWorkerCount()` 会顺手回收子进程）——接出口之前先问这两条 |
| 运维端点的监听面 | `server.metrics_port`（0 = 端点留在业务口上）+ `server.metrics_address`（默认 `127.0.0.1`） | `metrics_port` 为 0（不另起管理口，行为与加这两项之前逐字相同）；`metrics_address` 只听回环 | `metrics_port` 非 0 时业务口**不再注册那四个端点**（`/metrics`、`/healthz`、`/readyz`、`/debug/loops`，打过去回 404，这是刻意的反向断言），要抓数得打 `metrics_port + 本进程序号`；多进程下 `ReferenceServer` 由 master 用 `--worker-index` 把序号传下去，逐台各听一个口 | 只配 `metrics_address` 而 `metrics_port` 仍为 0 = 什么都没挪；把 `metrics_address` 写成 `0.0.0.0` 又不配令牌，等于把内部计数与每条循环的状态公开到所有网卡；`metrics_port` 越界（含加序号后超 65535）在读配置与启动两处都当场拒——端口静默回绕会去听一个谁也没配的号，症状只是「Prometheus 抓不到数」；管理口那台服务器只发 HTTP/1.1（业务口开了 h2c 也不带过去）——抓取端本来就是 h1，运维面上多一条协议栈只是多一处可被打的入口，用例 `Http2CleartextSession.RefusesH2cPrefaceWhenCleartextSupportIsOff` 钉住没开 h2c 的端口收到 `PRI * HTTP/2.0` 时按 HTTP/1.1 语法判 400 而不是发 SETTINGS 接走 |
| 日志等级与滚动 | `Base::LoggerConfigLoader` 的 `global_level` 与 `sinks`（`rolling_file`：`directory`/`policy`/`max_size_mb`/`max_backup`） | 未配置前 root 是 Trace 且**零 sink → 全部丢弃**；`global_level` 缺失回落 INFO；滚动按 `size`、单文件 10 MiB、留 10 份 | `LoggerRegistry` 的 sink 快照；`AsyncSink::droppedEventCount()`（每条通道各计各的）与 `Base::droppedAsyncLogEventCount()`（进程级合计）；`FileSink::skippedLineCount()`（文件没打开或流已失效时按行累计的「落不下盘」数）；开着 `/metrics` 时合计落在 `asyn_http_log_dropped_events_total`——队列满按策略丢、Block 策略等位超时、下游落地抛异常三种口径都算进去 | 越界值会被钳制并打到 `stderr`（不中断启动）；`policy` 拼错会回退成 `size` 并说明原因——启动日志要留着看；**本实现不认识的字段（含 `max_backups` 这类拼错的键）同样打一行 `stderr`，并报出这一层认识哪些字段**：`server` / `acme` 段是未知键当场拒，`logging` 这一段刻意只报不拒（sink 的字段集按类型多态，因一个多余字段就丢掉整条 sink 比现状更伤）；`ReferenceServer --config` 会连同 `logging` 段一起装上（不装就只有 `server` 段生效） |
| worker 起法 | `Core::WorkerSupervisor::Configuration` | `workerCount` 必须 ≥ 2；崩溃窗口 3s、连续 5 次「起来就崩」不再补；`shutdownTimeout` 10s | 构造期就校验：Windows 缺 `handoff`、POSIX 给了 `handoff` 都直接抛 | Windows 上 worker 靠 master 移交监听描述符（不是 `SO_REUSEPORT`），配错的表现是「只有一个进程收得到连接」；`ReferenceServer --workers` 只走 POSIX 那条（Windows 上缺移交档位，构造即抛），移交形状见 `samples/Core/Worker.cpp`（目标名仍是 `CoreWorker`）；master 被硬杀时 worker 随作业对象一起被终止（Windows `killWithParent`、POSIX `PDEATHSIG`），主机不让挂作业时保护缺席会落一条 WARN |
| 优雅停机 | 各服务器的 `stop()` / `drain(timeout)`；`WorkerSupervisor` 的 `shutdownTimeout` | `drain` 的时长由调用方给（库不设默认）；到点后强关并在途请求作废 | 停机时观察：在册连接归零、`/metrics` 的丢弃计数不再涨 | 超时给小了会掐断在途长请求；worker 的体面退出在 POSIX 是 SIGTERM，Windows 没有信号——编排者给每个 worker 独立进程组再发 `CTRL_BREAK`（`Process::requestTermination()`），宿主没有控制台时发不出去，会记一条 WARN 再强杀 |
| 编译与链接加固 | CMake 开关 `ASYN_ENABLE_HARDENING`（默认 ON）；门禁 `scripts/check-hardening.py` | 开。GCC/Clang 落 `-fstack-protector-strong`（每个非 sanitizer 档）+ Release 的 `-D_FORTIFY_SOURCE=2` + 链接侧 `-z relro,-z now`；MSVC 编译与链接都带 `/guard:cf`（只进 Release）。**开 sanitizer 时整体不叠**（配置期打一行说明） | 现跑一次那道门禁：`python scripts/check-hardening.py --build <构建目录>`。它的期望从**该构建目录自己的 `CMakeCache.txt` 现读**，先核 `compile_commands.json` 里每个 `src/` 翻译单元带着该带的开关，再核产物镜像头（ELF 的 `GNU_RELRO`/`BIND_NOW`/不可执行栈，PE 的 NX/DYNAMIC_BASE/CFG）；「配了没重新 configure」「找不到 readelf」「这一档根本没链接出产物」都退 2，不会被读成通过 | 加固只落**本仓自己的产物**，且全部 `PRIVATE`：链接期策略（`-z now`、CFG）刻意不推给消费者——静态库的消费者可能用 `--no-pie`、纯静态 glibc 或自己一套 CET 策略。所以**最终可执行体那侧要自己开**：只在库里加选项而可执行体没带链接期 CFG / FULL RELRO，读到的是「库编过了」而线上镜像照样没有那道防线 |

配置键到服务器的对接只有一处：`applyHttpServerConfiguration(server, configuration, context)`（`Net/Http/HttpServerAssembly.h`）。
`server` 段的键此前只能靠调用方逐台手接六七个 setter，`expose_metrics` 就是这样变成了「配置里打了勾、
端点一个都没注册」的死键；现在装配收进这一处，并且当传入的共享限额器与配置标量不一致时**当场拒绝**
——多条通道共用一份限额器时，静默挑一边会让「配置写 16、实际跑 64」完全看不出来。

装配入口覆盖的是明文与 TLS 两条 TCP 通道；**HTTP/3 不在它里面**（`QuicServer` 用的是自己那份
`Configuration`）。示例侧仍逐项接同一份配置——解析上限、连接级限额、单来源限额、限流桶、在途正文
预算与并发上限，生效值随启动打出，免得「同一份配置在 h3 上是另一个数」只能靠线上反推。

三条形态边界（无 macOS/BSD、共享库的 ABI 与 OpenSSL 双副本、CI 触发面因免费分钟数收到 `main` + 手动
触发）分别写在上面的「交付形态」与下面的「测试与验证」里，这里不重复。

## 测试与验证

一笔提交要过的闸门：本地串行四道 → CI 十五条作业并行铺开（Linux 十二条 + Windows 三条，构建那一档按三分片展开成十七个作业实例）→ 发布与供应链。

![一笔提交要过的验证闸门](assets/diagrams/png/verification-gate-workflow-light.png)

> 交互版：[verification-gate-workflow.html](assets/diagrams/verification-gate-workflow.html)

- **GoogleTest**（`gtest_discover_tests`，每个用例独立进程），测试目录与 `src` 逐级对齐
- 当前规模（**最近一轮 `develop` 的 `97b14c3f` 只核到结论与时长**，2026-10-10 手动触发，h3 监听协程的定时帧归属修好之后：Linux 十四条与 Windows 三条全 `success`，其中 `ThreadSanitizer：线程契约` 由上一轮的 X 转绿（11m46s）；runner 到 results-receiver 的作业日志接口这一轮整片取不到读数，所以下面那些**条数是上一轮 `b8aad4e2` 的**（`main` 仍停在 2.6.0 的 `a9837e4b`，下一次发版才跟上）。本轮的真读数只有本机同档——Windows Debug（含 ASan）ctest 4097 条 0 失败、97 条按 SKIP 记账、128.04 秒，示例矩阵 12 次运行 0 失败，容器 Debug + ASan/UBSan 的 QUIC 与 HTTP/3 两族 475 例全过、零 sanitizer 命中、零告警：**Windows `构建与全量用例（MSVC + ASan）` 4068 条全绿、零告警**（93 条按 SKIP 记账、ctest 实测 499.03 秒——CI 里没有真机凭据，MySQL 与 Redis 那几族和 ACME 的实机签发都跳过；同档本机 Debug + ASan 那一轮是 4077 例全过、92 条 SKIP，差的 9 条正是 CI 没建真机驱动的那几族）；Linux 三条构建分片 `Database` 647、`Platform / Base / Core` 1293（按模块三段：243 + 657 + 393）、`Net` 2142，**合计 4082 条全绿、零告警、零 sanitizer 命中**（GCC 13 + ASan/LSan/UBSan，`-Wall -Wextra -Werror`）；覆盖率档那一轮把整树一次跑完，同样 4082 条全绿、82 条按门控 SKIP、170.75 秒。示例矩阵 Linux 13 次运行 0 失败、Windows 12 次运行 0 失败（少一条 `CoreUpgrade`，它只在 POSIX 侧构建）。其余作业逐条数过、**0 条非 `success`**：`HTTP/3 跨实现验收：aioquic` 11 条场景全过；`规范裁判：h2spec 与 Autobahn` 常规与 `--strict` 各一轮「全部核对通过」，Autobahn 515 条、FAILED 0；`ACME 真机构验收：Pebble` 8 个场景全过；`协议解码器持续模糊` 合计 18,147,206 次执行、零崩溃；`ThreadSanitizer：线程契约` 筛面命中 1475 / 全量 4082；`覆盖率门禁` 总行 77.56%、总函数 86.33%、总分枝 61.99%（可执行行 41683）；`格式门禁：clang-format` 参与检查的跟踪源文件 724 个、复杂度基线 47 项越地板、图引追溯门 237 条 `sources` + 115 条正文引用 0 违例；Windows 静态档 194/194 个翻译单元标记齐全、本仓解析失败 0；共享形态（`BUILD_SHARED_LIBS=ON`）4068 条全绿。再往前那一轮 `25d715a4` 两面门红在**同一个成因**：Linux 的格式档与 Windows 静态档的「全仓格式检查」都退 123，落点是上一笔刚提交的 `src/Net/Http/RequestBodySpool.cpp`——钉版格式化的结果没进那一笔提交，而那一轮的提交信息按「本轮改过哪些文件」的逐文件复检写了「全树 0 违例」。修法是两半：把那份文件按钉版重排，并把本机这条判据改成与 CI 同形状的整树跑（清单取自 `git ls-files`、先自证条数不低于 500）；从 `b8aad4e2` 起两面门重新绿。更早两次红在另一处：runner 到 `auth.docker.io` 的请求整片超时，Redis 与 Autobahn 两个裁判镜像没拉下来，门禁本身没跑（重跑一次仍超时）——于是给两处拉取各加三次带时限的尝试与退避；这类红的形状是「一步都没跑成」而不是「某项判据失败」，判据本身没被改过。裁判的先决条件是「服务端真的活着」：跑之前先取 healthz 与 /metrics 自证，跑完再拿「打死监听端口必全线红」当反向对照。版本号三处一致（2.6.0，本机跑 `scripts/check-release-version.py`：CMake 版本号 / 更新日志最新发布段 / 最新标签同为 2.6.0）。
- 零编译器告警是提交判据；Debug 构建在 AddressSanitizer 下跑通且无报告
- 真机套件：MySQL 45 例、Redis 31 例（两族都按 ctest 名单现数；覆盖认证、参数化往返、事务、批量插入、异步读写链路、管道与回复类型映射）
- **CI 触发面**：四条工作流（Linux CI / Windows CI / 发布门禁 / 供应链）都只在 `main` 推送与手动触发上跑，
  `develop` 不消耗分钟数——要看某个提交就 `gh workflow run linux-ci.yml --ref develop`（按**文件名**触发，
  作业名已是中文；`--ref` 只认分支/标签，直接给提交号会报 `No ref found`）。两条构建作业还带
  `paths-ignore: '**.md'`：纯文档改动不会拉起一次几十个 runner 分钟的构建（所以改版本号那一笔必须动到
  `CMakeLists.txt`，否则它会跟着文档一起被跳过）。每条作业覆盖什么、最近一次真实运行，
  记在 `.github/SECURITY.md` 的「我们靠哪些持续验证」表里（含 h2spec、Autobahn、libFuzzer、TSan、aioquic 互操作、Pebble 的 ACME 跨实现验收）
- **ACME 的跨实现验收怎么跑**：`scripts/acme_pebble_cross_check.sh`，对面换成 Pebble（Let's Encrypt 官方
  那套 ACME 测试服务端，Go 实现，与本仓不同源代码）。它不需要公网机器：把镜像里的 `app` 与 `test/` 取到
  一处目录（`docker create ghcr.io/letsencrypt/pebble:latest` 再 `docker export` 解包即可），脚本会用
  自己的端口（默认 24000/25000，令牌端口 15002，都能用环境变量覆盖）现造一份配置起对面，域名用
  `127.0.0.1.sslip.io` 这类通配解析——对面按它解析回来就是本机，于是整条 HTTP-01 在回环上走通，
  签出的链还要拿对面**本次启动**的根（管理接口 `/roots/0`）用 openssl 验一遍。
  为什么值得跑：进程内的桩与实现同源、一起被改，两边错在同一种理解上时会一起绿；这一轮它就抓出了两个
  桩不会拒的缺陷（请求缺 `User-Agent`、机构复用已 valid 的授权时又被触发一次挑战）
- **供应链**：`scripts/generate-sbom.py` 出 CycloneDX 1.6 清单（含 `SHA256SUMS`），
  `scripts/check-dependency-advisories.py` 钉「`conandata.yml` 的固定版本与 `packaging/dependency-watch.json`
  的公告台账同解」；两者由 `supply-chain.yml` 作业跑（`main` 推送、手动触发、每周一凌晨），SBOM 作为制品保留
  90 天。它交的是**依赖清单与来源**，不是签名后的二进制——要制品级 provenance 需另配签名构建
- 代码所有权见 `.github/CODEOWNERS`（按目录路由，协议层与平台层的改动自动点名）；漏洞响应时限与披露节奏见
  `.github/SECURITY.md`

## 性能

表中读数来自**未开** `ASYN_WITH_MIMALLOC`、**未开** `ASYN_WITH_IO_URING` 的 Release 构建（epoll / 完成端口后端 +
系统分配器）。Release 也**不开全程序优化**：`f37d3035` 实地开过 `/GL` + `/LTCG` 做对照，hpack-decode 快
12.6%、http2-frame-decode 快 14.9%、http-date-format 快 17.3%，而 http1-parse-request 反慢 3.5%、hpack-encode
慢 0.6%——收益是散的；而且 `/GL` 编出的静态库是 CGALLIB（同批实测 `Net.lib` 达 232 MB），非 `/LTCG` 的消费者
链不动它，本仓对外交付的恰恰是静态库与 Conan 包。这一档的口径记在 `benchmarks/microbench-baseline.json` 的
`build` 字段。把这两句写在这里是为了别让「Release」被读成「全部性能开关都开了」：没测过的收益不写，测过但
划不来的也不开。

两档都默认关，各有明确理由，不是没来得及打开：

- **`ASYN_WITH_MIMALLOC`**：它与 sanitizer **互斥**（mimalloc 会遮蔽 ASan/LSan/TSan 的分配拦截，配置阶段直接
  `FATAL_ERROR` 拦住），而本仓库每一笔提交都在 sanitizer 下过门禁。默认打开它等于默认关掉内存与线程门禁——
  代价远大于未实测的分配收益。要用它请在**生产构建**里显式开：`cmake -DASYN_WITH_MIMALLOC=ON`，并另行安排
  不带 sanitizer 的验证轮。接管是全局的（`Core` 以 PUBLIC 链入 `mimalloc-static`，最终可执行文件的
  `malloc/free` 整个换掉），包消费方要能在 Conan 缓存里找到它。
- **`ASYN_WITH_IO_URING`**：容器与部分宿主内核的 seccomp 会挡 `io_uring_setup`（`EPERM`），默认打开会让
  「在这台机器上建不起事件循环」成为常态失败；而且它只改后端选择（`Epoll.h` 里 `using Epoll = Uring`），
  三后端共用一套契约、多数断言在 epoll 下恒绿，所以对齐是靠 CI 里那条 `io-uring-compile` 作业**带着用例**
  跑出来的（内核不放行时跳过运行，不放行就等于没测）。要评估收益请在支持它的环境里显式打开。

与那两档相反，**`ASYN_ENABLE_HARDENING` 默认开**：它是部署形态该有的底色，关掉才需要理由。
GCC/Clang 侧是 `-fstack-protector-strong`（每个非 sanitizer 档都带）+ Release 的 `-D_FORTIFY_SOURCE=2`
+ 链接侧 `-z relro,-z now`；MSVC 侧编译与链接都带 `/guard:cf`，只进 Release。三条口径：只落在
本仓自己的目标上（与告警门同一份清单，将来 vendor 的第三方源码不参与）；全部 `PRIVATE`，
不把链接期策略推给消费者（`-z now` 与 CFG 归最终可执行体的所有者决定）；**开 sanitizer 时自动不叠**
——ASan/UBSan/TSan 自带一套栈与内存检查，叠上来之后一次 trap 是谁报的就说不清了，配置期会打一行说明。
配套门禁 `scripts/check-hardening.py` 判两层：`compile_commands.json` 里每个 `src/` 翻译单元都要带着
该带的开关，产物镜像头要真带上 `GNU_RELRO`/`BIND_NOW`/不可执行栈（PE 侧是 NX/DYNAMIC_BASE/CFG）。
它的期望从**构建目录自己的 `CMakeCache.txt` 现读**，因此「配了没重新 configure」「找不到 readelf」
「这一档根本没链接出产物」都是退 2 而不是被读成通过。接线在 Linux 覆盖率档与 Windows 静态档。

单进程、同机回环，客户端与被测服务共享同一台机器。这类数字只能用于**同一台机器上的前后对比**：
换一次会话、换个邻居负载都能差出近一倍，跨机器比没有意义，因此这里不写「比谁快」的结论。

`ReferenceServer`（Release：MSVC `/O2`、不开全程序优化、无插桩），`--threads 4`，2026-09-24 实测，三轮取中位：

| 场景 | 中位吞吐 | 三轮范围 | p50 | p95 |
|------|----------|----------|-----|-----|
| HTTP/1.1 保持连接（8 线程 × 8 连接） | 54,804 请求/s | 49,106 ~ 55,232 | 131.9 µs | 234.1 µs |
| HTTP/1.1 短连接 churn | 2,646 连接/s | 2,493 ~ 2,657 | 363.3 µs | 14.9 ms |

三轮合计 38,400 条保持连接请求与 12,000 条短连接，**失败 0**；服务端句柄数 184 → 收尾 220~253
（峰值 384），工作集 10.4 → 13.0 MiB 上下，跑完不漂移。保持连接那一路的 p50 主要不是单次服务耗时
而是排队延迟（64 条并发连接压在 4 个工作线程上，Python 客户端自身也是瓶颈之一），所以它适合看前后
变化，不适合当单次请求延迟的绝对值。

```bat
cmake --build build/release --target ReferenceServer
set ASYN_SOAK_BUILD=release && benchmarks\run-soak.bat
```

机器可读基线与各阶段的离散范围在 `benchmarks/baseline.json`；改动前后各跑一次再用
`benchmarks/check-baseline.py` 比对——吞吐劣化超过 0.60×、p50 劣于 1.50×、p95 劣于 2.00×、p99 劣于 2.50×
判回归（`maximum` 只录不判：单次调度抖动就能让它翻倍）。基线里缺哪一档判据会当场报红而不是静默跳过——
「测量脚本已经产出 p99、门禁还只判 p50/p95」这种两头各写一份的漂移就是这么堵住的。
Linux 侧的同一件事走 `benchmarks/run-soak.sh`，比对它自己的 `benchmarks/baseline-linux.json`；
两边绝不互比：同一份代码在容器里的 keepalive p50 差着四五倍，差的是虚拟机回路与宿主后台状态，不是代码。
基准不进 CI（同机压测会把 CI 机器自己变成噪声源）。

测试硬件：Intel i5-14600KF（20 逻辑核）、32 GB 内存、Windows 11（10.0.26200）、NVMe；
Linux 那一档在同一台机器的 ubuntu24 容器里录（GCC 13.3，`-O3 -DNDEBUG`，不带插桩）。

## 编码规范

代码风格以根目录 `.clang-format` 为基线，全仓已按该配置格式化过，CI 有**格式门禁**作业（clang-format 版本钉到与本机同版）：新增或改动的代码提交前先跑一遍 `clang-format`，排不开就直接红。`.clang-tidy` 的检查在 CI 里仍以报告形式跑，不阻塞合入。下表是仓库层面的约定要点，新增代码与既有代码保持一致：

| 规则 | 说明 |
|------|------|
| 命名 | 类/文件/目录 PascalCase，函数与参数 camelCase，成员 `m_` / 静态 `s_`，常量 `kPascalCase` |
| 注释 | 全中文 Doxygen；`override` 方法必须独立完整注释；实现体关键位置写「为什么」 |
| 错误 | 报错文案全中文且写清「原因 + 替代做法」；禁止静默失败与静默变形 |
| 测试 | 每个功能都有用例；依赖外部服务的用例一律环境变量门控，凭据只从 `ASYN_*_TEST_*` 环境变量进——仓库里出现的口令全是写死的假口令，没有一把能打开真服务 |
| 目录 | `tests` 逐级镜像 `src`；CMakeLists 分层聚合（叶子目录 append 到 `GLOBAL PROPERTY`） |
| 文件头 | Doxygen 块里的 `@version` 是**这一份头/类自己的版本**，与根 `CMakeLists.txt` 的 `project(VERSION ...)` 无关：所以绝大多数是 1.0.0，被破坏性改过的那份会看到 2.0.0（`HttpParser.h`）。`@copyright Copyright (c) .` 那行的年份是模板留下的空位，持有者与年份只在 `LICENSE` 里写一次（2026 Gyanis），不在每个头里重复一份真源 |
| 提交 | 中文提交信息讲清「为什么」，一次提交只讲一件事 |

## 版权

Copyright (c) 2026 — MIT License
