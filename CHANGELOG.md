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

### 新增

- **报错可携带调用栈**：框架异常（`Base::Exception` 及其用法错误分支的 `LogicException` /
  `InvalidArgumentException`）在构造时捕获抛出点调用栈（只存原始帧，约 0.55 µs），符号解析推迟到
  日志落地时进行——事件循环线程不会因为「日志带栈」而去读调试信息（配合异步 Sink 时解析落在
  工作线程）。日志侧新增四个宏：`LOG_ERROR_EXCEPTION(e, fmt, …)` / `LOG_LOGGER_ERROR_EXCEPTION(logger, e, fmt, …)`
  （记录异常并附上它的抛出点栈）与 `LOG_STACK(level, msg)` / `LOG_LOGGER_STACK(logger, level, msg)`
  （记录当前位置的栈）。文本格式化器在消息后追加「调用栈:」块，JSON 格式化器给出独立的
  `stackTrace` 字段；异常来自框架家族之外时照常记录、不带栈（不补采打印点的栈充数）。
  `std::stacktrace` 不可用的平台由构建期探测（`ASYN_HAS_STACKTRACE`）自动退化为空实现。
  框架内 16 处关键错误路径（事件循环、连接池线程、配置热重载、TCP 服务器的接受/建连/清扫/关闭、
  h2 握手失败、h1/h2/h3 的 WebSocket 业务异常、示例程序的启动失败）已改用该宏，日志文案保持不变。

## [1.1.0] - 2026-09-16

自 1.0.0 起的累计变化：HTTP/2 与 HTTP/3 补齐流式收发、隧道与流回收，新增出站客户端与多进程 worker、
可选 io_uring 事件后端与可选 mimalloc，格式层改用 nlohmann_json 与 yaml-cpp。**本版含破坏性变更**
（`Base/Format/` 整体移除、`AsynGyanis::wepoll` 目标不再导出、`WebSocketPeer::close()` 的非法用法
改抛异常），从 1.0.0 升级前请先读「变更」段。

### 变更

- **JSON/YAML 改用 nlohmann_json 与 yaml-cpp，自研格式层整体移除**（破坏性）：`Base/Format/` 全部下线
  （值模型 `FormatValue`、JSON 解析/输出/Pointer/Patch/流式、YAML 1.2 解析/输出/事件、`FormatError`、
  `ValueAccessError`），`ConfigValue` 现在就是 `nlohmann::json`。迁移：值访问改 `at()` / `get<T>()` / `is_*()`，
  JSON 解析与序列化用 `nlohmann::json::parse` / `dump()`，JSON Pointer、Patch、Merge Patch 与 SAX 用 nlohmann 实现，
  YAML 多文档与事件用 yaml-cpp；`ConfigManager::get<T>()` 类型不匹配改抛 `ConfigValidationException`
  （原来的 `ValueAccessError` 随格式层删除）。三处原生口径差异要留意：正整数落 `number_unsigned`；
  `empty()` 只对 `null` 与空容器为真（空字符串不算空）；`get<T>()` 会做算术互转，配置层另提供不取整、
  不回绕的严格取用（`configValueAs`）。依赖随之新增两项：`nlohmann_json` 3.12.0（头文件随 `Base` 公开传递）
  与 `yaml-cpp` 0.9.0（只在实现里使用，静态库符号经 `Base` 传递）。配置加载把 YAML 文档转成 JSON 值模型时，
  标量按 1.2 核心 schema 判定（引号标量一律字符串、`yes/no/on/off` 是字符串），重复键、复杂键与自定义标签
  直接报错，别名展开设 128 层与 20 万节点上限。

- **Windows 事件后端换成完成端口（IOCP）**：`Epoll` 在 Windows 上改为 `using Epoll = Iocp`，事件由完成
  通知翻译而来（读方向 1 字节 `MSG_PEEK` 探针、写方向零字节 `WSASend`、监听描述符用 `AcceptEx`）；
  `IoWatcher` / `EventLoop` / `AsyncSocket` 的接口与语义都不变，Linux 侧仍是 epoll。wepoll 不再参与轮询，
  只保留 `epoll_event` 与事件位定义（要回到旧实现，把 `Epoll.h` 的平台分支与 Core 的平台源列表对调即可）。
  随之而来的一条语义变化：**接受分发的连接在移交前不注册**（完成端口绑过一次就换不了属主），
  `AsyncSocket` 的注册因此推迟到第一次等待——对使用者不可见，但自定义循环与套接字装配方式时要知道。
- **wepoll 并入 `Core` 库，不再单独导出目标**：`AsynGyanis::wepoll` 从 `find_package` 导出的目标集里消失，
  `epoll_*` 符号现在就在 `AsynGyanis::Core` 里。Windows 侧的事件通知是 Core 的实现细节，不该出现在对外接口上。
  迁移：此前显式链过 `AsynGyanis::wepoll` 的工程删掉那一行即可，其余无需改动。
- **WebSocket 本端 `close()` 的用法错误改为抛异常**：`WebSocketPeer::close(code, reason)` 此前把不可上线的
  状态码（1005/1006/1015 哨兵值、1016–2999 未注册段）与超长的关闭原因照原样发出去，对端只能按协议错误
  收口；现在这两种用法错误当场抛 `Base::InvalidArgumentException`（原因上限 123 字节）。可用的取值是
  1000–1003、1007–1014 与 3000–4999。收帧侧口径对称：对端用非法状态码收口时本端按 1002 回敬。
- **事件语义统一为水平触发 + 按需摘除**：一次就绪上报不再重新武装（去掉 ONESHOT 的一发即消），
  「不关注」显式写进内核；IOCP 侧在入睡前重武装已消费的方向，与 epoll 每轮重取就绪等价。
  对使用者接口不变，但自定义循环装配时要知道关注位的账本在 `IoWatcher` 里。
- **`writeTimeout` 的语义明确为「响应产出预算」**：处理器执行与等待可写的整段时间都计入它
  （进入路由前会按它刷新一次空闲时限），慢处理器不再被空闲清扫误杀，超预算的连接仍会被收口。
  配置键名不变，含义以 `HttpServerLimits` 头文件注释为准。

### 新增

- **HTTP/3 与 QUIC 服务端**：vendored 的 ngtcp2 1.25.0 做传输层（`QuicServer` 按连接标识路由数据报、
  迁移与流控），nghttp3 1.12.0 做会话层（`Http3Session`：QPACK、控制流 SETTINGS、流式请求正文、
  流式响应与 SSE、RFC 9220 扩展 CONNECT 隧道）；请求映射与响应回写接进既有 `Router`，处理器一份代码
  三种协议共用。`echo_server --h3` 在同一个端口号的 UDP 上提供 h3（QUIC 自带 TLS，需与 `--https` 同用）；
  h3 的请求数与状态码类计入 `/metrics`。
- **多进程 worker 模型**：`Core::WorkerSupervisor` 拉起 N 个 worker 进程服务同一个端口、崩溃即补位，
  进程间不共享状态；`echo_server --workers N` 开启（默认 1 = 单进程），响应里带 pid 便于核对。
  与既有的「每线程一个监听 socket」和「接受分发」是三种并列的多核形态。
- **出站客户端**：明文与 HTTPS 的 HTTP 客户端（`Net/Http/Client/`）、异步 DNS 解析
  （`Core/Socket/AsyncResolver`）与 `TcpClient`，构成完整的客户端侧。
- **HTTP/2 补强**：流式请求正文、按流并行的收发、隧道期间同连接其它流改为就地服务（不再一律 503）。
- **Linux 可选 io_uring 事件后端**：`ASYN_WITH_IO_URING=ON` 时 `Epoll` 别名改指 io_uring 实现
  （一次性 POLL_ADD 复刻水平触发语义，注销走墓碑表延迟摘除）；需要内核 5.6+，Windows 上配置期直接报错。
  接口与语义和 epoll / IOCP 两后端保持一致。
- **可选 mimalloc 全局分配器**：`ASYN_WITH_MIMALLOC=ON` 时由 mimalloc 接管整个进程的 malloc/free
  （生产构建用；与 sanitizer 互斥，配置期拦住同开）。
- **HTTPS 侧的指标与健康检查端点**：`--https` 下 `/metrics` 与 `/healthz` 此前打不开，现已与明文侧同口径。
- **监听套接字支持 TCP Fast Open**（Linux，`TcpAcceptor` 配置项开启）。
- **零停机重启的接手侧（监听器移交）**：`TcpAcceptor` / `TcpServer` / `HttpServer` / `HttpsServer`
  新增「用已经在监听中的套接字构造」的入口。监听套接字可以交给 supervisor 持有（Linux 的 socket
  activation）或由上一代进程交出来，新一代接手它继续服务，端口全程不关，配合既有的 `drain()` 与
  GOAWAY 就是零停机接力；关闭自己那一份描述符仍由接手方负责（旧进程收口时正该如此）。
- **WebSocket 扩展压缩（RFC 7692 permessage-deflate）**：帧层只在协商过该扩展时接受 RSV1，且仍只允许它
  出现在数据消息首帧；握手协商成功时回一行 `Sec-WebSocket-Extensions`；对端提供该扩展即接受
  （回复里两条 `no_context_takeover` 意味着每条消息重置压缩上下文，本端因此不保存跨消息的 z_stream）。
  h1 的升级握手与 h2 的扩展 CONNECT 隧道（RFC 8441）共用同一份协商实现。
- **Conan 库包（`packaging/conan`）**：把五个静态库与公开头文件打成 `asyngyanis/<版本>` 包，消费方
  `requires("asyngyanis/1.0.0")` 后用 CMakeDeps 生成的配置 `find_package(AsynGyanis)` 并链
  `AsynGyanis::Platform/Base/Core/Net/Database` 即可。组件间的依赖关系、外部依赖（zlib/openssl/
  sqlite3/hiredis，可选 libmysqlclient）、Windows 的 wepoll 与 Winsock、以及 MSVC 需要的 `/utf-8`
  都在配方里如实声明——静态库不会替消费方带出这些，漏一项就是链接期「无法解析的外部符号」。
  `conan create packaging/conan` 会连同消费方冒烟测试（真的 find_package + 链接 + 运行）一起跑。
  为打包新增两个开发者开关 `ASYN_BUILD_TESTS` / `ASYN_BUILD_SAMPLES`（默认开，关掉后只构建库本体与安装规则）。

### 修复

- **HTTP/1.1 客户端**：`Transfer-Encoding: chunked` 的响应此前永远收不了尾（所有分块响应都会被判失败）；
  同时补齐定界语义——Content-Length 与 Transfer-Encoding 并存按非法拒绝（不再挑一个信）、长度严格解析、
  chunked 大小写不敏感、1xx/204/304 一律无正文、HTTP/1.0 无定界头时按连接关闭定界。
- **HTTP/2**：入站请求校验 `content-length` 的取值与重复一致性，并在派发前与实收正文字节数比对
  （不一致回 400）；此前这个头完全不看，与 HTTP/1.1 侧的严格口径分裂。
- **HTTP/3 / QUIC**：WebSocket 隧道上行归还 QUIC 接收额度（此前超过流窗口即被流控卡死）、
  补齐「零字节 + FIN」的 END_STREAM（否则流式响应与隧道的收尾要等空闲超时）、
  流关闭归还 MAX_STREAMS、被对端重置的流唤醒等待中的生产者、隧道以 END_STREAM 收口、
  会话不可用时收口连接而不是让请求静默挂起。
- **WebSocket**：收帧队列加上界（消费者跟不上时按 1008 收口，不再无界吃内存）；校验对端 Close 帧的
  状态码与原因文本、本端 `close()` 也拒绝不可上线的状态码；放行插在分片消息中间的控制帧（RFC 6455 §5.4）。
- **WebSocket 会话收口**：业务协程不再被无声销毁——收尾时先唤醒挂在收消息/写等待上的业务、
  关套接字让写等待以失败结束，再等业务跑完才结束会话，那句 `false` 与业务的收尾代码都会到位。
- **Windows 事件后端（IOCP）**：补齐注销与收尾路径（取消在途探针、排空完成通知、AcceptEx 失败后
  关掉并清空接受套接字），此前一次接受失败会让监听器永久不再接受连接。
- **进程编排（Windows）**：创建子进程只继承三个标准句柄，此前会把父进程所有可继承句柄
  （含监听/连接套接字）复制进每个 worker。
- **数据库**：连接池不再偶发永久挂起（异步获取的丢唤醒窗口）、不再在析构路径上自死锁、
  池销毁后归还连接不再释放后使用；异步获取与同步获取共用同一超时；建连移出池主锁。
  执行器停机后提交任务改为显式失败；`DatabaseFactory` 不再把「填了主机账号却没填端口」的配置
  静默当成本地 SQLite。
- **日志**：`block` 溢出策略不再无限期阻塞调用线程（下游卡住时按丢弃计数）；`max_size_mb` 为 0
  不再退化成每写一行滚动一次；滚动日志不再误删同前缀的无关文件（如 `app.audit.log`）；
  注销日志器不再让使用中的引用悬垂。
- **配置**：`setValue` 与加载/热重载、`clear()` 串行化到同一把写锁，写入不再被整份快照覆盖。
- **文件监听**：Linux 上按单个文件监听不再静默失效；补齐创建/删除事件与新建子目录的注册；
  事件队列溢出时派发一次重扫信号，而不是静默丢。原子写补上落盘屏障（fsync 文件与目录）与唯一临时名。
  递归监视覆盖「监视开始后才建出来的子目录」；被内核摘除监视的目录（删掉再放回来）能重新挂上，
  Windows 侧根目录被替换后按节拍重挂。
- **HTTP 语义对齐 RFC**：304 响应带上等于文件大小的 `Content-Length`（RFC 9110 §8.8.2 / 9112 口径），
  且不再自动补正文长度之外的矛盾头。
- **HTTPS 与 HTTP/2**：TLS 上的 HTTP/1.1 回落路径接上全局正文预算（此前只有明文端生效，TLS 侧可无界
  堆正文）；h2 的终止流记录被挤出上限后，合规客户端迟到的 `RST_STREAM` / `WINDOW_UPDATE` 不再被判成
  空闲流而回 `PROTOCOL_ERROR`、把整条连接 GOAWAY 掉。
- **HTTP/3 流回收**：被对端 RESET / STOP_SENDING 的流此前永不回收（流表与在途计数只增不减），
  现在按取消收口并唤醒等待中的生产者；在途正文超过预算时明确回 503 而不是无界接收。
- **WebSocket 收帧上界计入帧开销**：空帧此前不占额度，可以靠海量空帧绕过队列上界；现在每帧的
  64 B 编解码开销一并计入。
- **连接池析构不再丢等待者**：池销毁时同步等待获取连接的线程此前会永久阻塞，现在被唤醒并拿到
  「池已停止」的明确失败；异步等待者的恢复改走票据，不再 resume 已释放的协程帧。
- **配置与日志装载不再部分提交**：某个文件解析失败时，此前已摊平进全局表的那部分键会留下来
  （半份配置生效）；现在整批回滚。日志配置的 `sinks` 字段类型写错时保留现有 Sink，不再清成零个。
- **io_uring 后端**：注销描述符后同一个 fd 号立即重注册此前会被延迟摘除挡下（新注册收不到事件），
  现在注销当场释放 fd 键、在途轮询转墓碑表收尾；槽位发布与注销的竞态一并修掉。
- **多进程 worker（Windows）**：worker 被强杀后父进程此前可能拿着未回收的句柄继续记账，
  现在有界等待回收再释放。
- **交接失败不再漏描述符**：`ConnectionDistributor` 的分配失败不再让 noexcept 函数 terminate，
  失败路径上也会关闭描述符（此前每丢一条连接漏一个文件描述符）。
- **零长数据报允许发送**，与接收侧口径一致；`TcpClient::connect` 的 host 改按值接收（惰性 Task 下
  不再悬垂）；`AsyncSink` 不再旁路被包装 sink 的 level 过滤。

### 性能

- **HTTP/2 发送路径**：待发队列改游标推进（不再每帧 `erase(0,n)` 搬移整段剩余缓冲），写完的缓冲回收复用；
  响应头组装按条数预留容量，发正文不再整段白拷一次。
- **HPACK 动态表改 deque**：头插不再整体后移。
- **HTTP/3**：拼分片不再先零填充一遍缓冲；只在有新连接标识签发时才重扫 SCID 表。
- **HTTP/1.1**：响应头的单值视图改为惰性构建（不按请求遍历全部头）。
- **事件循环**：完成通知的合并索引改扁平表；TLS 让出间隔 1 ms 提到 5 ms，停顿期间不再近千赫兹唤醒。
- **日志**：根日志器热路径只读裸指针缓存。
- **Linux 静态文件**：`sendfile` 零拷贝发送。

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

[Unreleased]: https://github.com/Gyanis9/AsynGyanis/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/Gyanis9/AsynGyanis/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Gyanis9/AsynGyanis/releases/tag/v1.0.0
