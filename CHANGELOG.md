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

- **示例按模块拆开，并配总跑脚本**：`samples/` 新增 `base_log`、`base_config`、`platform`、`core_loop`、
  `core_tls`、`core_worker`、`net_http_demo`、`net_https_h2_demo`、`net_http3_demo`、`database_demo` 十个
  各自自检的可执行程序（`echo_server` 保持部署形态）。每个程序逐步打印 `✓`/`✗`，并在 stdout 留一行 `RESULT <名字> PASS|FAIL <步数>`，
  退出码即结论；`scripts/run_samples.py` 从构建目录扫出示例清单、跑完并汇总成「模块 × 示例」矩阵，
  任一失败、超时或没有结论行都以非零退出。拆的过程同时是对公开面的清点，实测到这些事实：框架没有
  HTTP/2 与 QUIC 客户端（TLS/h2 示例因此自带一个回环客户端才能驱动服务端，h3 示例只能验到握手之前的那半张脸），
  h1 不发 `100 Continue` interim 响应，请求目标过长按 431 收口而不是 414，HTTP/2 的并发流与头块上限只通告不可配，
  异步数据库链路「在哪条线程上恢复」没有契约（实测不在事件循环线程上），
  `QuicServer::stop()` 只置标记不碰套接字（静默端口上的 `listen()` 不会自己退出，过早销毁对象是 use-after-free）。
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
- **HTTP/3 认下 `Expect: 100-continue`**：请求头收齐且声明了正的 `content-length` 时，先交一条
  不带 END_STREAM 的 `:status 100` 头块（RFC 9114 §5.3.2）再等正文，与 h1/h2 同一时机——此前
  严格按 RFC 9110 §10.1.1 等 100 才发正文的对端，只能靠自己的 expect 超时兜底，上传类请求
  凭空多等一截。没声明长度的请求不回 100（这一刻判不出正文会不会来，不凭空塞信息性响应）。
- **HTTP/3 的 WebSocket 隧道接上 permessage-deflate 协商**：与 h1 升级、h2 隧道共用同一份
  `negotiatePerMessageDeflate`，接受时把选定参数写进 200 应答头，并按同一结论打开对端对象的
  压缩开关。此前 h3 隧道是裸的：对端提了扩展也收不到结论，只能全量走明文，而按压缩发过来的帧
  会被本端当成 RSV1 违规判成协议错误。同时补上两处与另两条通道不一致的统计：隧道里的
  WebSocket 帧计数（原先没拿到采集端，恒为零）与升级成功计数。
- **HTTP/3 有关停通告与优雅收口**：`Http3Connection::beginGracefulDrain()` 在控制流上发 GOAWAY
  （通告值按 §5.2 取「最后一条已受理流之后的下一条客户端双向流号」，一条都没受理过时为 0），
  通告之后到达的新请求流一律不处理、只归还接收额度；`QuicServer::drain(超时)` 与
  `TcpServer::drain()` 同形状——先只挡新连接（不能直接 `stop()`，那会让收报文的循环退出、
  在途请求再也收不到后续报文），再发 GOAWAY、等在途做完，到期兜底强关。示例程序的关停路径
  现在把 h3 一并排空，不再只 `stop()` 了事。
- **HTTP/3 落定并回显 request-id**：与 h1/h2 同一份 `HttpRequestIdGenerator`（`HttpServer`/
  `HttpsServer` 新增 `requestIdGenerator()` 取值口，`QuicServer::Configuration` 转交给每个会话），
  业务从 `request.requestId()` 读得到、响应回 `x-request-id`；派发前先回显一次，流式响应的头部
  才带得上（它的头是在处理器写第一块时上线的），处理器之后再过一次兜住 `reset()`。
- **HTTP/3 认下两条连接级限额**：`maximumRequestsPerConnection`（答完这么多条就发 GOAWAY 排空，
  「已通告且手上没活」的连接由承载层收掉）与按来源 IP 的并发连接上限（`PerIpConnectionLimiter`
  与两条 TCP 通道共用一个实例，名额凭据与连接同寿命）。此前 h3 两条都没有：长连接可被无限期
  复用，同一来源换走 QUIC 就绕过了 `--max-connections-per-ip`。
- **QUIC 服务端主动收口时会发 CONNECTION_CLOSE**：新增 `QuicConnection::closeNow(错误码, 原因)`——
  原先只有 `requestClose()`（只置标记、一个字节都不发），对端收不到任何告知，只能等自己的空闲
  超时才发现连接没了（RFC 9000 §10.2 的 SHOULD）。`drain()` 的兜底收口、单连接排空完成、
  HTTP/3 会话不可用这三条路径现在都带原因收口。
- **QUIC 流层能收口单条流**：新增 `QuicStreamLayer::resetStreamSending()` 与 `stopStreamReceiving()`，
  分别编出 RESET_STREAM 与 STOP_SENDING。此前这两个方向只能「本端自己丢掉队列」，对端一个字节也
  收不到，只能干等那条流。两份宣告都按 RFC 9000 §13.3 记了「内容一次定稿、在途不重发、判丢补发
  同一份、确认即落定」这笔账。反方向也补齐了：收到对端的 STOP_SENDING 时按 RFC 9000 §3.5 回一条
  RESET_STREAM（错误码照抄），否则对端永远等不到那条流的终局信号。
- **HTTP/3 会把「这条流我们不做了」说给对方听**：`Http3Session` 新增 `StreamAborter` 接缝，
  `QuicServer` 把它接到 `QuicConnection::abortStream`。两处落地：GOAWAY 通告之后到达的新流按
  RFC 9114 §5.2 用 `H3_REQUEST_REJECTED` 取消（此前只是不回字节，对端要等连接收尾才知道结果）；
  被 413/431/414/503 拒掉的请求在响应完整交出后用 `H3_NO_ERROR` 请对端停发剩余正文（与 h2 同一处置）。
- **HTTP/3 认下收请求的时限**：`Http3Session::expireStaleRequests(本拍时刻)` 由 `QuicServer` 的到期
  节拍按拍调用，一条流在 `HttpServerLimits::readTimeout` 内没有新的请求字节就收口它
  （`H3_REQUEST_CANCELLED`）。h1/h2 撞到这个时限是掐掉整条连接，这里只处置那一条流——多路复用是 h3
  的常态，一个慢客户端不该连坐同连接上的其它请求。**处理器相位刻意不判**：流式响应与 WebSocket
  隧道本来就该长期挂着，会话没有「业务产出了新字节」这个钩子去刷那份预算，误伤比收益大。

- **静态文件的映射缓存**：`HttpServerLimits::maximumMappedStaticFiles`（默认 64，0 即关闭）决定同时保留多少份
  已建好的文件映射，`Net::StaticFileMappingCache` 按「规范化路径 → 映射」做有界 LRU。命中判据是本次查到的
  大小、修改整秒与**文件身份标记**三项全等，所以文件被截断、换掉、甚至原地换成等长内容都不会命中旧映射。
  身份标记取自同一次底层查询（Windows 用创建时间的 100 纳秒刻度，POSIX 用 `st_ino`），不多花一次系统调用。
  **Windows 上本项一律按关闭处理**（建静态配置时降级并记一条 INFO）：文件只要挂着活动映射，既不能被就地
  截断、也不能被 `rename` 覆盖（实测 `ERROR_ACCESS_DENIED`，用例撞出来过），而「写临时文件 + rename」正是
  静态资源发布的常规做法——为了省一次映射把发布方式挡掉，代价倒挂。

### 变更

- **HTTP/3 这一层改为仓库自研，不再链接第三方实现**：`src/Net/Http3` 现包含 h3 帧编解码、
  QPACK（静态表与动态表、编码器流与解码器流上的全部指令、阻塞头段的挂起与续解）、连接状态机
  与错误码表，判据取自 RFC 9114 与 RFC 9204。对使用者可见的行为不变——同一个 `Router`、
  同一套流式正文/SSE/RFC 9220 WebSocket 隧道，`--h3` 的用法一字未改；变化在依赖侧：
  `AsynGyanis::Net` 不再需要 nghttp3，包消费方少一条 `find_dependency`，Conan 的 `net` 组件
  也不再声明 `nghttp3::nghttp3`（nghttp3 目前只留在测试里当跨实现裁判）。
  一处刻意的口径差异：请求头部畸形（RFC 9114 §4.1.2）现在只作废该流并按 400 应答，
  不再像此前那样把整条 QUIC 连接判死。
- **QUIC 传输层同样改为仓库自研，`Net` 不再链接 ngtcp2**：`src/Net/Quic` 现包含变长整数、报文头与
  帧的编解码（RFC 9000 §16/§19）、包保护与密钥调度（RFC 9001）、TLS 胶水、发包组包器、连接状态机、
  恢复层（RTT/PTO/判丢与探测超时/NewReno 与 §8.1 的反放大上限）和流层（双向与单向流、连接级与
  流级流量控制、1-RTT 密钥更新）。`QuicServer` 的接法与 `--h3` 一字未改；变化在依赖与接口侧：
  公开头不再暴露 ngtcp2 类型，消费方不必再拿它的头路径与 `NGTCP2_STATICLIB` 宏（ngtcp2 目前只留在
  测试里当跨实现裁判，与 nghttp3 同一个道理）。**含破坏性变更**：`QuicConnection::Configuration` 的
  `statelessResetSecret` 与 `onConnectionIdIssued`、以及 `QuicConnection::sourceConnectionIds()` 已删除，
  直接搭过这层外壳的使用者要改掉那三处引用——本实现不做无状态重置，也不签发额外连接标识，
  路由表里除本端标识之外只剩客户端最初选定的那个目的标识。刻意未做的还有路径迁移、0-RTT、
  RETRY 与 DATAGRAM 帧。

### 修复

- **异步数据库不再恢复已释放的协程帧**：`AsyncExecutor::submit()` 把等待中的协程句柄按裸指针
  记在共享状态里，工作线程做完任务后无条件把它投回事件循环。调用方在任务完成前销毁 `Task` 时
  帧已随等待体析构，循环随后 resume 的就是一块已释放的内存——确定性释放后使用，`Queryable` 的
  全部 `*Async` 都走这条路（连接池侧早已用「取走即作废」的票据钉住同一场景，执行器侧漏了）。
  现在句柄存在等待体与队列闭包共享的原子字段里：等待体析构时清空，工作线程取到空即当无事发生，
  同一个句柄也只可能被恢复一次。新增三条用例，摘掉清空后其中两条当场以访问违例崩在 resume 上。
- **静态文件映射缓存不会再登记与自身键矛盾的条目**：查询与建映射之间文件被改大时，缓存照抄了
  调用方那份较小的 stat 读数当命中判据，于是留下一条「键说 S1、正文能给 S2」的条目；日后一次把
  文件截断回 S1 的查询（落在同一秒内即三项判据全对上）会命中它，按映射长度发正文就踩到已随截断
  解除的页——POSIX 上是 SIGBUS，Linux 的 `sendfile` 侧则发出比 `Content-Length` 少的字节。
  判据改由映射自身的长度成形，不变式落在持键的一侧，新调用方也无法登记出自相矛盾的条目。
- **静态文件映射缓存的身份判据不再只看 inode**：inode 号会被回收——把文件删掉再同名重建（最常见的
  发布动作之一），文件系统往往把刚释放的那个号原样发给新文件（容器内 overlayfs 实测两次同为 245657），
  于是「大小 + 修改秒 + inode」三项全对，缓存端出上一个文件的字节直到进程重启。POSIX 的标记改折
  `(设备号, inode, ctime)`：设备号管住「同一路径换挂到另一个文件系统」，ctime 由内核在 inode 变更时
  刷新、任何用户态 API 都设不了，回收来的 inode 必然带一个新的。用例把修改时间从旧文件原样搬到
  新文件上再断言身份不同；把 ctime 从判据里摘掉，它在容器里当场变红。
- **文件监视：被监视目录改名走开后原地重建，同名目录可以被重新监视**（仅 inotify 侧）。内核这时只投
  `IN_MOVE_SELF`、不补 `IN_IGNORED`，wd 仍被持有，于是路径映射不会自己消失；调用方对同一原路径再
  `addWatch()` 会撞上按路径去重的那条分支——返回 true 却不注册，新目录的事件从此永久丢失。现在按
  `IN_IGNORED` 的同一方式摘掉映射（递归根交给自愈复查补挂）。Windows 有同一形状的症状，但一次
  「按句柄实际落处判陈旧、命中则重新注册」的修法仍未投递事件，成因未定位，故用例只在 Linux 编译。
- **`removeWatch()` 撤掉的递归根不会被自愈悄悄加回来**：递归根另存一份清单用于「目录被删又放回」时
  补挂，撤销时只清了监视表、没清这份清单，于是下一拍自愈把它重新挂上——`removeWatch()` 返回 true、
  句柄重开、回调照旧派发，等于把调用方的显式撤销否决掉。撤销与内部清理改走同一个私有入口，只有
  前者会摘清单（后者正依赖清单补挂，一并摘掉会让「删掉又换新放回」的自愈失效）。
- **文件读写的长度形参超限改为一律拒绝**：`FileDescriptor::read/write` 在 Windows 上把 `size_t` 长度
  直接窄化给 `recv`/`send`（形参是 `int`），4 GiB 会回绕成正数 0——读侧撞上「对端关闭」的含义、写侧
  报「发了 0 字节但成功」，而同层的 `Socket`/`DatagramSocket` 早已判界。现在与它们同口径：超限即
  `WSAEMSGSIZE` 失败，宁可拒绝也不静默截断。
- **原子写的临时文件不再跨进程撞名**：临时名原来只拼一个进程内的原子计数器，注释却承诺「并发写同一
  目标时各写各的临时文件」。跨进程时这句话不成立——每个进程都从 0 开始，两个写者算出同一个
  `<目标>.tmp.0`，于是交叉写同一份临时文件、再把两边内容夹杂的结果 rename 成目标，恰好发生在
  「多个进程各自保存同一份配置」这条最需要隔离的路上。名字改为 `<目标>.tmp.<进程号>.<计数器>`：
  进程号分开跨进程，计数器分开同进程内的并发。用例从失败路径的回显里读出临时名并断言含进程号，
  把进程号去掉即当场变红（报出的临时名正是 `child.txt.tmp.1`）。
- **HTTP/2 服务过静态文件后立即归还该文件的映射**：会话的响应对象按连接复用，而正文在发出时
  已按帧拷进这条流的待发队列，映射与此后的发送无关；此前要到下一条请求开头才随 `reset()` 一起
  解除。于是 h2c 上取过一次静态文件、连接转入空闲之后，那份映射与它的文件句柄仍被攥着——
  POSIX 上是一条 fd 与一段地址空间预留按空闲连接计（Windows 侧自静态正文改为堆读取后不再有映射
  可攥；HTTP/1.1 侧早已在发送完成后解除，两条协议从此一致）。新增一条 h2c
  静态文件端到端用例，替换文件时刻意不发第二条请求，因此它同时是这条修复的证伪器。
- **静态文件的错误响应不再带着表示头部**：表示头部（`accept-ranges`/`etag`/`last-modified`，
  206 还含 `content-range`）原先在取映射之前写好，而取映射这一步自己就可能把状态改成 500 或 413
  （文件在 stat 之后被删或改了权限、映射长度复查越界），产出「`HTTP/1.1 500` 带
  `Content-Range: bytes 0-9/1000` 与 ETag」这种自相矛盾的报文：状态码说是故障，头部却说正文是
  某个区间的那段字节，而验证器描述的是一个根本没发出去的表示。改为在写任何表示头部之前就把映射
  拿稳，此后到设置正文之间不再存在改状态的分支。
- **静态文件的 If-Range 日期验证器改按「相等」放行**：此前判据是「Last-Modified 不晚于该日期」，
  于是当文件的修改时间比客户端那份拷贝**更早**时（从备份恢复、时钟回拨这类把时间拨回去的情形），
  服务端仍会把 Range 当成对同一份表示的请求回一个 206 字节段——客户端会把它拼进自己那份
  已经不同的缓存，正文就此被污染。RFC 9110 §14.22 要的是「与当前表示的验证器相等」，
  不相等即忽略 Range、按 200 下发完整表示（新增用例钉住「相等→206、更晚→200、更早→200」三种形状）。
- **HEAD 不再被兜底路由抢走**：`Router` 原来「先按 HEAD 匹配一遍、没中再按 GET 匹配一遍」，于是二级
  通配路由里的 `any("*")`（`HttpServer::staticFileDir()` 正是这么挂的）会在第一遍就把
  `HEAD /业务路径` 抢去，落到静态目录的 404 上——同一请求换成 GET 却正常，等于「HEAD 与 GET 只差
  有没有正文」这条语义（RFC 9110 §9.1）被破坏。复用判据现在落在**同一级之内**：本级先按显式放行 HEAD
  的路由严格匹配，全级都没中才按 GET 复用；级别之间的优先级因此与 GET 完全一致，而显式注册的
  `head()` 仍在任何注册顺序下赢过同路径的 `get()`。
- **HTTP/3 的 413/503 不再等对端收尾**：正文越界或全局在途预算不足时，请求当场排进待派发并立刻应答。
  此前这两个响应排在「请求收齐」之后——客户端只要分批 dribble 且不发 END_STREAM，响应就永远不来，
  这条流就这么挂着（h2 的 `isReadyToServe` 早就带上了这个判据）。
- **Database：不再把故障静默吞成正常结果**。`SchemaMigrator::tableExists()` 把「游标推不动」「计数列不是
  整型」与「表确实不存在」一并返回 `false`，与该接口自述的「`false` 的两重含义用 errorText 区分」
  相矛盾——现如实写入 errorText；`resolveDialect()` 的 `catch (std::exception)` 会把「该类型没有方言」
  这类用法错误也降级成可恢复的查询失败——现不再接住，用法错误照常外抛。
- **Database：方言层与 `toSql()` 的 `default` 分支改为抛 `Base::LogicException`**（消息带枚举值）：操作符与
  连接类型遇到未登记的枚举值时此前静默回落成 `=` / `INNER`，会生成语义错误的 SQL。
- **Database：`Queryable::countOn()` 在计数列不是整型时抛 `RowMappingException`**（原静默返回 0，调用方会
  误以为「一行都没有」），异常文本带列名与实际类型。
- **Database（MySQL 驱动）**：影响行数不再直接转换 `mysql_affected_rows` 的出错哨兵 `(my_ulonglong)-1`
  （调用方会读到 −1 行），归零为「未知」；连接配置字段含内嵌 `NUL` 时，在 `mysql_real_connect`
  （只接受零终止字符串）之前本地拦下，不再静默截断成认证失败。
- **Database：`resetSessionState()` 不再可能 terminate**：SQLite/MySQL 在 `noexcept` 复位路径上调用会构造
  `std::string` 的 `rollback()`，分配失败即 terminate，现显式接住。
- **HTTP/3 的响应头部与 h1/h2 逐字同源**：同一份业务代码此前换个协议会得到不同的响应头。
  多条 `Set-Cookie` 只发得出第一条（`HttpResponse` 的头视图是「一名一值」，逐条取值要走
  `headerValues()`）；不补 `date`（RFC 9110 §6.1 要求源服务器给出）；有正文却没设媒体类型时
  不补 `text/plain`；越界的状态码原样交给连接层，而 `:status` 必须是三位十进制，于是
  整条响应的字节都发不出去，对端干等到超时（现改回 500 并记日志，与 h2 同口径）。
  普通响应与流式响应头这两处采集原本各写一遍、口径已经漂移，现收成一个。
- **示例程序里 h3 不受限流、也不占在途正文预算**：`--rate-limit` 的令牌桶只挂在两条 TCP
  通道的路由上，同一来源走 h3 就不限；`QuicServer::Configuration::memoryBudget` 会被转交给
  每个 h3 会话，样本却没给，于是 `--max-inflight-body` 对 h3 完全不生效。两处都接上同一份对象。

### 性能

- **头部存储补两条「不拷贝」的读取出口**：`HttpHeaderFieldStore` 新增 `firstValueView()`（交出存储内取值
  的视图，不分配）与 `contains()`（只问存在性），owning 的 `firstValue()` 改由视图版派生——两条入口共用
  同一次查找，「同名取首条」「大小写不敏感」这类口径不会再各修一侧而分叉。此前只要读一个值就得拷一份：
  `x-request-id` 的采信判定先拷一个可选串、再拷一份返回值（同一请求两次堆分配），而 `Connection`/
  `content-encoding` 这类「只看设过没有」的判定写的是 `getHeader(x).has_value()`，为一次布尔把整个取值
  拷出来。三协议会话的 request-id 补齐与保活判定、CORS 预检、压缩中间件共七处存在性判定，加上
  request-id 的采信判定，一起改走新入口。同一份二进制内 A/B
  （微基准新增 `header-first-value-view` / `header-contains`，与已有的 `header-first-value`、
  `header-refill-only` 逐对）：减掉 281.6 ns 的「装 10 条头部」本底后，owning 查询 70.2 ns、视图查询
  13.1 ns（−81%）、存在性判定 15.6 ns（−78%）。视图只在本存储下次改写前有效，这条约束写在声明处的
  `@note` 上；证伪是把视图版的比较换成大小写敏感，新增的存储用例与 `HttpRequest` 用例连同既有的
  「读路径不要求先归一化」用例一起变红。**刻意不动的相邻形状**：`Accept-Encoding`/`Expect`/`Range`
  这些要读值的入口仍走 `getHeader()`——非可重复头部同名多条时它的契约是按 RFC 7230 §3.2.2 以 ", " 合并，
  换成「取首条且不拷贝」会静默改掉协商与定界的语义，那是比几十纳秒更贵的东西。
- **Windows 的静态正文改为一次读取，不再为每个请求建映射**：映射的卖点是零拷贝，但本平台没有任何零拷贝
  发送可用（`TransmitFile` 在非阻塞套接字上仍会阻塞线程），把正文交给内核那次拷贝照付，映射只多付整段
  缺页——同一份文件两条路的实测中位数为 4 KiB 17.2–18.4 / 12.7–15.1 µs、64 KiB（含首触）29.4–31.1 /
  15.5 µs、1 MiB 343 / 46.7 µs，**差距随尺寸线性放大**。另一半收益不在耗时上：活动映射期间发布方无法
  替换文件（实测「写临时文件 + rename 覆盖」以 `ERROR_ACCESS_DENIED` 失败、就地截断以
  `ERROR_USER_MAPPED_FILE` 失败，而 `FILE_SHARE_DELETE` 只换得来「删除」），读完即走的堆缓冲没有这条
  占用，静态映射缓存在本平台因此也不再是需要的事。POSIX 一行未动——那里 `sendfile` 是真零拷贝。新增
  `Platform::readFileContents(path, offset, length)` 承担这条路径（按长度取字节、二进制安全，短读以实际
  长度表达而不是报错或补零），200/206 的正文长度与 HEAD 的 `content-length` 收敛成同一个数。紧接的一笔
  把这条路上剩下的那次堆分配也省掉：正文读进**响应自己的**堆缓冲（`HttpResponse::prepareBodyBuffer(length)`，
  与 `setBody`/`setOwnedBody` 共用同一套前置不变式），响应按连接复用，第二条请求起只是覆写既有容量。
  实测收益从几十 KiB 起显形（64 KiB 15.98 → 14.44 µs、256 KiB 21.21 → 17.23 µs；4 KiB 以内持平——
  小块分配走热分配器），保留它更重要的理由是「正文多大都不再新增堆压力」。
- **POSIX 侧静态文件不再每请求重建映射**：`mmap-open-and-close` 实测 **17.2 µs/请求**（打开文件 + 建立视图 +
  解除），是静态路由上最大的单项每请求固定开销，也是上一轮把元数据三次并一次之后剩下的那块。启用映射缓存后
  命中路径只剩一次加锁的哈希查表与 LRU 搬动（亚微秒级），那 17.2 µs 整体消失；同一份映射可以同时服务多条
  在途响应，淘汰不会把还在发送的页抽走（`HttpResponse` 的映射正文已改成共享持有）。Windows 侧自上面那条
  起不再为静态正文建映射，这台缓存对它就只剩 POSIX 才有的意义。
- **静态文件每请求的元数据查询从三次并成一次**：`is_regular_file` / `file_size` / `last_write_time` 各自都要
  把路径重新打开查一遍（Windows 上是三轮 `CreateFileW` + `CloseHandle`），静态目录服务每个请求都要这三样。
  新增 `Platform::queryFileBasicInfo()` 用一次 `GetFileAttributesExW` / `stat` 同时给出类型、大小与修改时间，
  逐项取值与那三个函数一致，故 ETag 与 Last-Modified 不会因这次改造而漂移；顺带让「大小」与「修改时间」
  必然来自同一个版本（分三次查时，中间被改写会拼出一对来自不同版本的验证器，原先靠两处 500 分支兜）。
  实测每请求 **27871.3 → 9000.9 ns（−67.7%）**，都发生在事件循环线程上。
- **HTTP 响应头块组装少拷一到两遍**：h2 一条响应原先要把头部拷两遍（建响应的单值视图 → 按名回查每头
  全部值 → 为把 `:status` 插到最前再整表拷一次），h3 是同一形状的一份。三条协议现在统一按
  `HttpResponse` 权威记录的**设置顺序**单趟取完（顺带修掉「同名多条被归组、次序随哈希表变」的跨协议
  不一致）。实测：组头块 h2 314.7 → 42.1 ns、h3 950.4 → 375.7 ns；h2 的正文长度校验也不再为一条取值
  构造整列 string。
- **HTTP 日期格式化**：`formatHttpDate` 不再走 `std::format` 排七个字段（269 → 47.3 ns），并给「此刻」
  那一份加按秒缓存——Date 头每条响应的真实代价降到 16.7 ns，静态文件的 Last-Modified 同样受益。
- **HTTP/3 出站帧**：DATA/HEADERS 的载荷不再为了算帧头里的 Length 先拷进一份临时串，正文段少搬一遍
  （16 KiB 一段 769.3 → 94.4 ns，且随正文大小线性受益——静态文件首当其冲）。
- **HPACK 静态表查名**：由「每写一个头把 61 项线性扫两遍」改成编译期算好的同名段表 + 二分，
  一次定位同时给出精确索引与仅名索引（编码 754.8 → 527.3 ns）。
- **Database：MySQL 列值解析改走 `std::from_chars`**：不再为每个列值先落一份 `std::string`，也不再受
  `LC_NUMERIC` 影响（小数点、科学计数法与余文仍然一律拒绝）；Redis 参数切词改用
  `std::string_view::contains`，`Queryable` 的列名渲染与 `Transaction::lastError()` 改返回常引用。

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
