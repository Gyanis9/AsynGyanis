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

自 2.5.0 起的累计变化（新增 9、变更 1、修复 8）：把已经建好的那道总量防线接到配置文件上并让它对外可读，把命中的路由模式交回业务，给 ORM 补上唯一键冲突的处置档位，让被丢的日志第一次有对外可读的总数，给「不是自己写的文本」一条统一的日志折法，给 `Retry-After` 一个统一的读法，并补上七处会咬人的行为——MySQL 的 DECIMAL 列读不进浮点成员、叫停一条睡下的续期循环要等到下一拍、一批失败出口只说「失败了」而不说哪一步、503 在三条通道上带的响应头不一致、时钟被往回改之后按日期滚动的日志一直写进旧日期的文件、QUIC 的空闲超时配成负数会把事件循环打崩、机构的自由文本能把日志行伪造进本进程，另加 ACME 的退避既不听机构也不听自己配置这一处。

### 新增

- **`Retry-After` 有了统一的读法**：`Net::parseRetryAfter(text, now)` 认 RFC 9110 §10.2.3 的两种写法
  （相对秒数与绝对的 HTTP-date，后者折成「与 now 的差」，已经过去的交 0 秒 = 现在就能再试），读不懂时
  交回**空**而不是 0——0 的含义是「现在就再试一次」，把一条畸形头部折成 0 等于在限流现场把对端的警告
  丢掉并加速撞上去。配套地，`AcmeError` 与 `AcmeReply` 各加一个 `retryAfter` 字段把它带上去
  （两个结构都是本包内构造的聚合，成员加在末尾，按旧头编译的消费者不受影响）。
- **外部文本有了统一的日志折法**：`Base::escapeForLog(text, maximumDisplayByteCount = 48)` 把不是自己写的
  文本折成一行可打印 ASCII——C0 控制字节与 0x7F 写成 `\xNN`，高位字节（UTF-8 的非 ASCII）同样折开，超长时
  截断并留下 `…（已截断）` 标记。为什么需要：日志的一条记录对应一行，原样的 `\n` 让一条记录在采集端变成
  两条（伪造的那半行可以顶着别的时间戳与级别），`\r` 让行首被覆盖，`\x1B` 把转义序列直接送进终端，NUL 让
  按 C 字符串取日志的采集器静默丢掉后半截——而这几个都**看不见**，日志看着完全正常。协议解析层早就挡掉了
  头部与请求目标里的控制字节（`HttpHeaderRules.h` 与 `HttpParser` 的字符集），所以本框架真正需要它的是
  「对端交来的自由文本」那一路。它同时是 h3 错误串那份文件内转义的去处：两处各写一套折法，迟早分叉成
  「一条挡得住 NUL、另一条挡不住」。
- **日志被丢多少，第一次有了对外读数**：`Base::droppedAsyncLogEventCount()`（进程内**所有**
  `AsyncSink` 合计）与 `/metrics` 上的 `asyn_http_log_dropped_events_total`（counter，进程级）。
  此前每个 sink 自己有 `droppedEventCount()`，但只有拿着那个对象的应用代码读得到——而「日志在偷偷丢」
  通常是运维先发现（某段时间的审计记录不见了）。六个丢弃出口里有一条是**对端可驱动**的：JSON 版式对
  非法 UTF-8 整条失败（有人在请求目标里送原始 Latin-1 字节时，访问日志的那一行就没了），worker 咽下
  异常后只记在本 sink 的账上；没有进程级读数，「审计记录被远端消音」与「这段时间根本没人写日志」
  在面板上是同一个形状。六个出口现在都走同一个 `countDroppedEvent()`，两份账（本 sink 与进程）
  不可能分叉。刻意不做成 `ProcessMetricsRegistry` 的登记项：那张表住 Core，而 Base 不依赖 Core。
- **已有的表能按结构体补列了**：`SchemaMigrator::addMissingColumns<T>(pool, &addedCount, &errorText)`
  与纯函数档 `addColumnStatement<T>(dialect, columnName)`。此前本类只会建表与删表——结构体加一个成员，
  已有的表不会跟着变，第一次读就报「列不存在」，而唯一的绕开办法是手写 `ALTER TABLE` 的原语 SQL
  （方言专有，还得自己对齐列名引用与类型映射）。现在「表里已有哪些列」直接问引擎
  （新增 `SqlDialect::columnListingStatement()`：SQLite 走 `pragma_table_info(?)`，MySQL 走
  `information_schema.COLUMNS` 并按 `DATABASE()` 限定库名——不限定就会把别的库里的同名表算进来），
  因此**不需要维护一份「上次建了什么」的账**，重复调用是幂等的。列名比对折成小写：MySQL 的列名
  不区分大小写，而 SQLite 按声明原样给出，不折一次会把同一个列看成两个。
  列定义（引用符、类型映射、可空规则）复用建表那一份出口，两处不可能分叉。
  **刻意的拒绝面**：不可空的新列与主键列都拒——已有行拿不出值可填，两个引擎都会当场拒掉；
  正确表达是把成员声明成 `std::optional<X>`（旧行为 NULL）。布尔值按本仓写侧口径成 1/0，
  NULL 与 List/Hash 参数在 Redis 一侧同样拒发。
- **Redis 接上了参数化执行入口**：`RedisConnection::execute(command, parameters)` 与
  `executeChecked(command, parameters)` 那层带成因的包装从此在 Redis 上不是一条死路。此前这一档由
  基类默认实现回一句「该驱动暂不支持参数化查询（Redis）」，而 Redis 的命令行**本来就是参数数组**，
  只是没有 SQL 那种占位符——按驱动通用接口写的应用（换引擎不改代码那一类）撞上的就是这句拒绝，
  而 `executeCommand(argv)` 那条原生通道就在旁边。现在命令文本按 `execute()` 的同一套规则切词、
  参数逐个转成文本接在后面，一起交给 argv 接口发送，内嵌 '\0' 与空白都不丢。
  两条拒绝面排在发送之前（不需要一条连接就能判）：命令文本里出现 `?` 直接拒——Redis 没有占位符语法，
  静默发出去等于写成一个名叫 "?" 的键。取值口径由新增的 `RedisConnection::argumentText()` 给：
  Int64/Double 十进制文本（Double 用最短可往返表示，不是固定六位小数）、Bool 按 ORM 写侧口径成 "1"/"0"、
  Text/Blob 原样交字节；NULL 与 List/Hash 拒发——它们没有「一个参数」的对应物，猜一个形状就是静默
  改写调用方的意思。`argumentText()` 的定义放在驱动分支之外，与 `parseKeyspaceNotification()` 同一层：
  它是纯字符串工作，没有 hiredis 的构建里也必须可判。
- **唯一键冲突的处置进了 ORM**：`Queryable<T>::insert(row, InsertConflict)` 与
  `insertBatch(rows, InsertConflict)`（`Queryable::InsertConflict` 三档：`Fail` / `Ignore` / `Replace`，
  默认 `Fail` 与不带该参数的旧写法逐字同形，线上字节一分不变）。此前只有 `insert(row)`：想按业务键
  create-or-update 的调用方要嘛先查后插（两条语句之间别人插进来就撞唯一键），要嘛自己写
  `INSERT OR REPLACE` / `REPLACE INTO` 的原语 SQL——那句是方言专有的，等于绕开 ORM 的参数化、
  取值转换与列名引用。意图交进查询树（`QueryNode::insertConflict`），关键词由方言给
  （新增 `SqlDialect::insertKeywordPhrase()`：SQLite 写成 `INSERT OR IGNORE/REPLACE INTO`，MySQL 写成
  `INSERT IGNORE INTO` 与 `REPLACE INTO`），共用渲染层只负责拼在最前面，单行与批量两个方向共用同一份出口。
  **刻意的限度**：`Replace` 在两家引擎上都是「先删同键旧行再插」，不是「按主键只改新行给出的那几列」——
  触发器会多一次 DELETE、自增标识可能换值、新行没给的列回到默认值；要 `ON DUPLICATE KEY UPDATE`
  那种只改指定列的语义，本层没有等价物，得走原语 SQL。
- **命中的路由模式交回了业务**：`HttpRequest::matchedRoute()`（配合 `setMatchedRoute()`，由路由器在
  跑中间件管道**之前**落定，因此访问日志与打点这类横切逻辑也读得到）。此前 `Router` 把注册原文存在
  路由条目自己身上（`PatternRoute::pattern`，注释写着「用于替换判等与诊断输出」）却没有任何出口——
  业务手上只有 `path()`，于是「按路由分组」这一件事只能自己把 URI 归一化一遍：拿实际 URI 当指标标签的
  键，基数等于访问过的 URL 数，一轮 404 扫描就把采集端撑爆，而面板上每个读数看着都正常。
  现在键是有限集合（注册多少条就有多少个值）：精确路由回注册原文，参数化路由回 `"/user/:id"` 这样的
  **模式**而不是实际路径，通配路由回带 `*` 的原文，HEAD 复用 GET 时仍是那条 GET 的模式（方法换了这个键
  不该多出一个桶），未命中（404/405）回空视图——「没匹配到」自己就是一档，不与任何注册模式撞名。
  存的是指向路由表存储的**视图**（每请求零分配，与 `remoteAddress()` 同一形状），因此路由在 `start()`
  之后仍增删条目时要把文本抄一份再留存；跨报文复用请求对象时这条视图随 `reset()` 一起清掉。

- **在途正文总量上限进了 `server` 段**：`memory_budget_bytes`（整机字节数，0 = 不设这道账）。
  `HttpMemoryBudget` 早在三条通道上被消费（预留不上的新请求回 503），但**库内没有任何人构造它**——
  唯一入口是 `HttpServer::setMemoryBudget()`，配置文件里想配也没有键。一道已建好的防线只能靠调用方
  手递，等于按配置文件部署的机器全部跑在「无总量上限」上：`parser_limits.maximum_body_size`（8 MiB）
  限的是**一条**正文，N 条连接各压一条就是 8N MiB 且与连接数同增，而每个分量看着都合规。
  现在装配出口按 `perProcessShare` 把整机数摊到每个 worker 进程（与连接数同向、向上取整），
  新增的 `HttpServerAssemblyContext::sharedMemoryBudget` 让 h1/h2 与调用方另起的 h3 共用同一份账，
  传进来的对象若不是摊后那一份会当场拒（与限额器、限流桶同一条判据）；`HttpServer`/`HttpsServer`
  各补 `memoryBudget()` 读口，生效值能从服务器上问回来。默认值刻意留在 0 = 不限：一个有限的默认值会在
  「升级后什么都没改」的部署上突然开始回 503，而这种默认值最难发现。
  `HttpMemoryBudget::maximumTotalBytes()` 此前无人可读，现在它是那条一致性判据的读口。
- **这道闸自己对外可读**：`HttpMemoryBudget` 在构造时登记两条进程指标——`asyn_http_inflight_body_bytes`
  （Gauge，当前预留的在途正文字节）与 `asyn_http_memory_budget_rejections_total`
  （Counter，因超预算被拒的预留次数）。此前预算只在回 503 那一行日志里出声：配起来之后
  面板看不出它是在兜住峰值还是在误杀正常流量，而「不敢开」与「没开」是同一件事。
  拒绝计数的口径写在头文件里：这是**预留次数**而不是请求条数（一条请求边收边攒可能补几次预留）。
  对象析构时把手注销，不留一条谁也不持有的旧读数；预算从此**不可拷贝也不可移动**——
  读回调捕获的是 `this`，而它本来就按 shared_ptr 在服务器与会话之间共享。

### 变更

- **`reference_server` 的 `--max-inflight-body` 口径由「每进程」改成「整机」**：与本仓库其余限额键一致
  （`maximum_connections` 等同样是整机数、按 `--workers` 摊），命令行与 `server.memory_budget_bytes`
  现在是同一个量的两个来源，命令行优先；启动那行读数改成「整机 X 摊给 N 个进程 → 每台 Y」。
  单进程部署（默认）取值不变。

### 修复

- **ACME 的退避开始听机构的，也终于听自己的配置**：两处都在「看着配好了其实没生效」这一族里。
  ① 机构的 `Retry-After` 被解析出来又当场丢掉——`AcmeErrorKind::RateLimited` 的注释一直写着
  「429 或带 Retry-After 的 503：该退避而不是改配置」，而代码只按状态码分支，`AcmeReply` 里连装它的
  字段都没有（RFC 8555 §6.8 是「机构必须发、客户端必须照办」）。② `Configuration::minimumRetryInterval`
  （默认 1 小时）**在 `src/` 里零消费方**：探针和调用方都能配，行为却恒是「失败后下一秒再试」，
  `backoffGateUnixSeconds()` 用的是「失败时刻 + 1 秒」。现在门槛取
  `max(本地最小间隔, min(机构的秒数, 24 小时))`——24 小时这个上限是刻意的不信任：胡乱答复或已被换掉的
  机构不该能把续期一推再推，推到证书真的过期。`status().backoffUntilUnixSeconds` 那一条对外读数随之
  才第一次说的是真实的下一次尝试时刻。
- **机构返回的自由文本不能再伪造本进程的日志行**：ACME 客户端把 problem document 里的 `detail`（以及
  挑战被判 invalid 时 `error.detail` / `error.type`）原样拼进错误消息，而这条消息最终由
  `LOG_ERROR_FMT("AcmeCertificateManager: {}", message)` 落成**一行**日志。JSON 里的 `detail` 合法地可以
  携带换行（那边写成转义形式，解析回来就是真换行），于是一个不守规矩、被中间人换掉、或单纯话多的机构
  就能在本进程的日志里造出带任意时间与级别的记录。现在这三处都先过 `Base::escapeForLog`（详情给到 512
  字节、URN 给 128）。桩这边加了 `injectedProblemDetail` 档位，用例钉的是两条：消息里不许出现真换行，
  且必须出现可见转义（后者证明是「折起来」而不是「整段丢掉」）。
- **QUIC 的空闲超时配成负数不再把事件循环打崩**：`max_idle_timeout` 此前是一行直转
  （`static_cast<std::uint64_t>(configuration.idleTimeout.count())`）。负数转出来约 1.8×10^19，超过 QUIC
  变长整数的上限 2^62-1，而编码器在为**服务端 Initial** 写这份参数的那一刻抛 `InvalidArgumentException`——
  那发生在事件循环线程上、连接建立的中途，症状是「第一个 QUIC 连接把循环打崩」而不是「配置写错了」。
  现在先过 `isEncodableIdleTimeout`（可编码域 `[0, 2^62-1]`）：域内原样换算，域外按 RFC 9000 §18.2 宣告 0
  （该参数的语义正是「本端不启用空闲超时」），并打一条**按小时限流**的 WARN 点名配置值——每条连接都会
  走这一判据，不限流等于自己把日志刷爆。0 仍然合法：那是显式关闭，不是错误。
- **墙上时钟被往回改之后，按日期滚动的日志不再继续写进旧日期的文件**：`RollingFileSink` 的「要不要重算
  周期边界」原先只看 `now >= 边界`。NTP 阶跃、手工调表、虚拟机恢复都会把时钟退回去，此时上一次算好的
  边界跑到未来最多一整个周期，期间新日期的记录一直续写在旧日期的文件里——文件名与每行自己的时间戳从此
  对不上，按日期找日志的人会找错文件。判据换成 `Detail::shouldRecheckRollingPeriod(now, lastObserved, boundary)`
  （纯函数，脱离时钟直测）：`now < lastObserved` 就是「时钟回跳过」的直接证据，撞上它无条件重算一次。
  重算之后仍由「后缀是否变化」决定要不要真的换文件，所以回拨既不会造出重复的备份名，也不会覆盖已有那一份。
  「周期内不重算」这一半是性能契约（每行只做一次 `time_t` 比较），一并钉住。
- **503 在三条通道上的响应头口径对齐**：HTTP/3 的「全局在途正文预算用尽」出口补上 `Retry-After: 1`
  （h1 在 `HttpSession.h`、h2 在 `overloadedResponse` 上都带着这一项；同一台服务器换一条协议，对端就拿不到
  退避时长）。HTTP/2 在隧道期间回给**第二条**扩展 CONNECT 的那条 503 走的是裸 HPACK 字段表出口，不经过
  响应头自动补齐那一段，因此连 RFC 9110 §10.1.4 对 5xx 硬要求的 `Date` 都没带——现在显式补上。
  刻意**不**给它 `Retry-After`：那条应答说的是「请另开一条连接」，劝对端原地重发只会再撞同一道闸。
  两条**没有改**的判定一并记下：304 不带 `Accept-Ranges` 是规范的禁止而非疏漏（§15.4.5 只允许
  Cache-Control / Content-Location / Date / ETag / Expires / Vary 这几项，`TestHttpServer.cpp` 里早钉着这条），
  隧道 101 不带 `Date` 是 1xx 的豁免。
- **MySQL 的 DECIMAL 列现在读得进浮点成员**：`RowMapper` 的浮点一侧新增十进制小数文本这条支路
  （`Detail::parseDecimalText()`）。旧断言是「文本落到浮点成员一律按列类型错误报告」，理由写着
  「浮点没有引擎给不出该取值只能以文本返回的必需支路」——这条理由对 MySQL 不成立：DECIMAL/NEWDECIMAL
  就是按文本交回的（`MySql/MySqlValueConversion.h`），而 DECIMAL 正是金额与精确量的常用列型。
  后果是同一个结构体在 SQLite 上读得动、在 MySQL 上**逐行**抛 `RowMappingException`，报的还是
  「列声明与成员声明不符」；整型一侧早就收十进制文本，两处不对称本身就是缺口。
  收下之前判两件事：**形状**（自己扫文法，不直接交 `strtod`——它放宽前导空白、认 `inf`/`nan`、
  还支持十六进制与二进制指数）与**有效位数**（按第一个到最后一个非零数字计，超过 `digits10`
  ——double 15 位、float 6 位——会静默改值，一律报错；这与整型「越界报错而不是取整」是同一条判据）。
  溢出与下溢按 `ERANGE` 与收窄越界报出，不会给出 `inf` 或 0。前后置零不携带信息：`"0.0012"` 算 2 位、
  `"10000000000000000"` 算 1 位，都收。
- **叫停续期循环不再要等到下一拍**：`AcmeCertificateManager::runRenewalLoop()` 的停放改成按 1 秒切片
  （`stopRenewalLoop()` 只落一个原子标志，而叫醒这条帧的唯一办法是让它自己醒来）。旧形状下
  「已叫停」要等到 `renewalCheckInterval` 那一拍走完——默认 **12 小时**，两种按文档办事的调用方都会出事：
  等帧退出再销毁对象的一直等（停机挂住），不等就销毁的在几小时后被一条帧踩在已释放的对象上。
  切片只改醒来频次，签发尝试的节拍与失败退避一分没动；`stopRenewalLoop()` 的文档现在写明等待是有界的。

- **一批失败出口开始说清「是哪一步」**（运维可读性整改，六处）：
  `TlsContext::installCertificate()` 原先三处失败都只 `return false`——「证书文件读不到」「不是合法
  PEM」「证书与私钥不配对」这三件处置完全不同的事长得一模一样，而最常见的是第三种；现在每步各出一条
  ERROR，点名是哪一步、哪个文件，并把 OpenSSL 错误栈最外那条原因当场抄回（头文件一直承诺「失败可由
  错误栈取到原因」，但没有任何出口把栈里的文本交给人读）。
  `QuicServer::reloadCertificate()` 的告警原来只带一个 `std::error_code`，而这一次重读会碰五处落点
  （证书、私钥、OCSP 装订、票据密钥、站点证书），现在把本轮要读的两个主文件抄进正文。
  `QuicConnection` 的「配置不完整（缺 SSL_CTX 或报文出口）」改成点名缺哪一件（两处：入站与出站）。
  `Queryable` 的「从连接池获取连接失败，**可能是**池已达上限或连接创建失败」不再猜：把
  `borrowTimeoutCount` 的前后两次读数与池的现况（总/活跃/排队）一起报出——增加了就是等满了截止时刻，
  没增加就是建不出连接；同一族在「问数据库类型」那处出口的「请检查连接池配置」也换成这几个数。
  `InotifyFileWatcher` 构造期那句「inotify 初始化失败」补上 errno 与其文本（ENOSPC 通常是
  `fs.inotify.max_user_instances` 用满，EPERM 是另一回事）。异常类型仍是裸 `std::runtime_error`，
  那是本层「Platform 在 Base 之下」的既有约束，不属这一条。
  `QuicClientConnection` 里那句写着 `QuicConnection:` 的日志改回本件名（按名 grep 的人会翻到另一个
  文件），等级从 DEBUG 抬到 WARN——调用方拿到的只是一句 `false`，被对端拒绝这种形状不该只在调试档看得见。

## [2.5.0] - 2026-10-02

自 2.4.0 起的累计变化（新增 5、变更 4、修复 7、破坏性变更 1）：把协议正确性上「标准说有、实现没有」的
几格补上，让 `logging` 配置里拼错的键第一次出得了声，并把下游按包消费时撞到的七处一并收口
（对端地址进不到业务处理器、包少一个系统库、异常文本带构建绝对路径等）。**含一处公开 API 改名**，见最后一条。

### 新增

- **业务处理器第一次读得到「这条请求从哪来」**：`HttpRequest::remoteAddress()` 交回对端地址（`"IP:Port"`，
  IPv6 侧带方括号）。此前这条信息只活在传输层：`Core::Connection::remoteAddress()` 拿得到，而
  `Router::Handler` 的签名只有 `(HttpRequest &, HttpResponse &)` 两个参数，业务侧没有任何通道把连接身份
  带过去——按来源限流、按地区放行、审计落的是谁全做不了。更难看的是
  `TcpServer::setProxyProtocolRequired()` 的文档一直写着「限额键、`HttpRequest::remoteAddress()`
  与日志都跟着改」，而那个方法从来不存在：PROXY 协议换来的真实来源只有框架自己的限额键吃得到。
  现在 h1/h2/h3 三条通道在派发之前把它落进请求（与 request-id 同一处），来源统一取自
  `Core::Connection::cachedRemoteAddress()`（按连接问一次 getpeername、失败也不抛，整条连接复用同一份文本），
  h3 没有套接字可问，改由 `QuicServer` 在建会话时交一个地址出口下来
  （`Http3Session` 构造函数末尾新增一个可空形参，`QuicConnection` 新增 `remoteAddress()`）。
  取值是副本而不是视图：这条文本指向连接持有的缓冲，而抄进审计队列正是它最常见的用法。另配
  **`HttpRequest::remoteIp()`**（不带端口的那一段）：按来源限流与审计要的就是这个键，而
  `remoteAddress()` 里的端口每条连接都换，直接拿去当键会得到「每条连接一个桶」、限流形同不存在。
  剥法只认本框架 `InetAddress::toString()` 的产出形状——点分四段 `"1.2.3.4:5678"`、带方括号的 IPv6
  `"[fe80::1%3]:8080"`（双栈监听上映射出来的地址也带着括号：`"[::ffff:1.2.3.4]:8080"`），且
  百分号后的作用域号**保留**——链路本地地址在不同网卡上就靠这一段区分来源，剥掉会把两块网卡的流量
  并进同一个键。认不出来时原样交回整条文本而不是切一刀——切错给的是一个静默失效的键，比交回原样难查。
- **日志闸门可以按运行期的 key 分档**：`Base::LogThrottleRegistry`（配套宏
  `ASYN_LOG_THROTTLED_KEYED(key, interval)`）。`ASYN_LOG_THROTTLED` 的状态长在调用点（函数局部 static），
  一个使用处一份，而有一类告警的区分单位是运行期才有的东西——哪个来源 IP、哪台设备、哪个频道——
  它们都从同一句 `LOG_XXX` 出来：共用一份会让第一个坏来源把其余来源的告警一起压掉，各写一份又要
  每人自己搭一遍「表 + 锁 + 上界」，而上界漏写就是一个远端可驱动的内存增长点。表有硬上界
  （`kMaximumTrackedKeys`），满时淘汰最久未触碰的那条：内存封顶换极端基数下漏一条是有意的取舍，
  因此 key 不要取每请求唯一量。分配失败时**放行**而不是压掉——被静默压掉的告警没有任何地方补记。
- **驱动失败有了带成因的执行入口**：`DatabaseConnection::executeChecked()`（两个重载，
  返回 `std::expected<std::unique_ptr<DatabaseResult>, ExecutionFailure>`）。原语一直是
  「空指针 + 事后读 `lastError()`」，于是每处调用都要自己写判空加抄文本，而那份原因**长在连接对象上**——
  连接归还池之后再读，读到的是下一个借用者的失败。包装在 `execute()` 返回那一刻把文本与配对的
  驱动原生码一起抄进返回值，失败处置因此可以放到还连接之后。不改 `execute()` 的签名：三个驱动、
  ORM 层与全部用例都按旧约定写着。

- **`OPTIONS *`（asterisk-form）有去处了**：语法层只把 `*` 这个请求目标放给 OPTIONS（RFC 9112 §3.2.3
  只把它派给 OPTIONS 与 CONNECT，本服务器不代理 CONNECT，所以别的方法带 `*` 当场判畸形请求，
  而不是让它一路走到「没有路由匹配」被报成 404）；路由层把 OPTIONS 的 `*` 按根路径派发——asterisk-form
  说的是「整台服务器」而不是某个资源，站点级注册的 `options("/")` 就是回答 `Allow` 的位置。
- **`logging` 配置段对不认识的字段出声**：`server` / `tracing` / `acme` 三段早就在做未知键拒绝，
  唯独 `logging` 只有逐字段的形态诊断——`max_backups`（正确写法 `max_backup`）、`globl_level`、
  `filename`（正确写法 `base_filename`）这类拼错会整块消失、日志照写旧值，「配置里明明写了保留 30 份、
  目录里却留着 10 份」没有任何一处会说不认识这个键。现在三段位置各自报（`logging` 段、每个 logger、
  每条 sink 按 `console`/`file`/`rolling_file`/`async` 的字段集，`async` 的 `wrapped` 递归判），
  诊断里直接列出这一层认识哪些键。口径是**报而不拒**：sink 的字段集按类型多态，因一个多余字段
  就丢掉整条 sink 比现状更伤。

### 变更

- **版本不合的 WebSocket 握手按 RFC 6455 §4.2.2 应答**：h1 从一律 400 改为 **426 Upgrade Required**
  并带一条 `Sec-WebSocket-Version: 13`（客户端靠这一行决定换一个版本重试，过去它只看到一个与版本无关的
  无声拒绝）；h2 的扩展 CONNECT 仍回 400——426 说的是「请改用 Upgrade」，而 h2 里没有 Upgrade 这套机制可改
  ——但同样补上那条头部。**h3 的扩展 CONNECT 从此会判 `sec-websocket-version`**：写了却不是 13 就拒掉
  （过去这条路径只看「业务有没有登记升级」，一个声明了版本 12 的对端也能把隧道拉起来）。
  隧道形态**不**要求 `sec-websocket-key`、也不要求版本头部存在——RFC 8441 §5 明写用扩展 CONNECT 引导
  WebSocket 的一端「do not do the processing of the Sec-WebSocket-Key and Sec-WebSocket-Accept header fields
  of [RFC6455]」，那套功能已被 `:protocol` 伪头取代。本仓上一版把 key 与版本都当必填加到 h3 上，
  被自家 aioquic 跨实现验收裁判当场抓出（它两条都不带，隧道直接建不起来）：h2 侧同一条既有必填要求
  也一并放宽，且只在对端给了 key 时才回 `Sec-WebSocket-Accept`（没给 key 却回一条由空串算出的 Accept，
  比不回更容易让人以为握手被验证过）。
- **permessage-deflate 的窗口位数按 RFC 7692 协商**：过去只认扩展名、`;` 之后的参数一个都不解析，
  所以对端声明 `server_max_window_bits=9`（它的解压器只开得到 9 位）时本端仍按满档 15 位压缩——
  严格执行的对端会在远距离回溯上当场报错收线。现在两个窗口参数都吃进语义并钳本端两侧的位数，
  提过的在 101 里回显选定位数；取值不在 8..15、带符号、非数字或长到该溢出的，**整条扩展不接受**
  （101 不回扩展头，对端退回明文，连接照常可用），而不是带着一个无法履约的窗口把连接开起来。
  位数从协商结果一路走到 zlib 的 init，复用 `thread_local` 压缩/解压流时发现位数与上次不同会先 End 再重建。
- **`Logger::addSink()` 的形参改成 `std::shared_ptr<LogSink>`**：内部本来就按 `shared_ptr` 的不可变快照
  持有 Sink（读侧无锁遍历的那份），只收 `unique_ptr` 等于在门口把所有权换掉却不把句柄交回去——调用方
  登记之后既不能把同一个 Sink 挂到两个 Logger 上，也不能读它自己的读数（自定义 Sink 的队列水位、
  落盘失败次数都长在它自己的接口上）。**传 `std::unique_ptr` 与 `nullptr` 的旧写法照旧编得过**
  （转换发生在形参上而不是重载决议里，因此没有开两个重载去争一个 `nullptr`——那个形状已被用例钉为
  空操作）。留下句柄意味着可以在运行期继续用它，而 Sink 的 `write()` 本来就由任意打日志的线程调用，
  自定义 Sink 的状态必须线程安全这条不因此放松。
- **协程帧的归属契约写进了声明处**：`Scheduler` 的类注释与 `schedule()/scheduleRemote()` 的形参注释
  明说「调度器从不拥有、也从不销毁任何帧」，队列里那个裸句柄只表示「这一拍要 resume 谁」，因此帧必须
  在被派发之前一直有人持有；`Task` 的析构注释同处交叉指过去（它拆的是帧本身，与停在哪个挂起点无关）。
  本仓自己的两处也按这份契约改成**先入表、后排度**（`TcpServer` 的连接任务与 PROXY 读头任务）：
  反过来时入表那一步抛 `bad_alloc`，队列里就留下一个没人持有、却随时可能被打发的句柄。
  两处文档顺带更正为「空句柄被就地忽略」——旧注释写的是「必须非空」，而实现一直是静默丢掉。

### 修复

- **出站 HTTP 响应解析不再把畸形行当头部收下**：客户端侧的 `HttpResponseParser` 缺冒号时把**整行**当成
  头名塞进结果，上层按名字取头就可能读到一个对端根本没发过的字段；折行（obs-fold，RFC 9112 已废）与
  冒号前带空白/控制字符的头名同样照收。三条判据收进一处 `headerLineIsWellFormed`，并与入站
  `HttpParser::parseFieldLine` 同规则；trailer 段过去只查「有没有冒号」，比头部还宽，现在两处共用同一判据。
- **客户端不再把坏掉的状态码折成一个数**：三条客户端通路对状态码文本的判据此前各不相同且都在折数——
  h1 的状态行取前三字符喂 `atoi`（`HTTP/1.1 abc OK` 得 0、`HTTP/1.1 2000 OK` 得 2000），
  h2 的 `:status` 走 `strtoul`、h3 走 `atoi`（同样的折法）。代价不是「数字差一点」，而是上层拿到一个
  **报文里从没写过的状态码**：按「2xx 才算成功」分支时假号与真号无法区分。现在三处共用
  `parseStatusCodeText`（恰好三位十进制，RFC 9110 §4.1）：h1 判失败、h2 按 RFC 7540 §8.1.2 收口连接、
  h3 按 RFC 9114 §4.1 只结当前这条流（`H3_MESSAGE_ERROR`）。范围仍不判（100..999 之外照原值交回），
  取值是否可用由各路自己的语义决定（隧道只认 2xx）。
- **h3 的 WebSocket 隧道在业务收工时先发一条 Close(1000)**：h1 与 h2 两路都先 `peer.close(1000)` 再收口，
  h3 直接交出 END_STREAM，对端的 WebSocket 层只能报「连接被凭空切断」（按 RFC 9220 §6 属异常断开），
  拿不到本端给的状态码，也分不清「服务器收工」与「中间把连接掐了」。顺序也是语义的一部分：
  收口会把对端对象标记为已闭合，之后那条帧就永远发不出去了。
- **两格被漏掉的拒绝/收口计数补齐**：h3 的隧道在帧解不开（1002/1007/1009）时收口却不记
  `webSocketProtocolErrorCloseCount`——那一格的口径是「对端违反 RFC 6455」，只有 h1/h2 记，
  结果面板上 h3 这条通道恒像「没人违规」；h2 的扩展 CONNECT 在握手不合法时只发 400 应答、
  不落 `bad_requests_total`，而同一条判据在 h1 与 h3 都记。两处都按「每类没交给业务的收口只落这一笔」
  的既有口径补上，帧错误仍不并入 `bad_requests_total`（后者是 HTTP 报文解析失败、回的是 4xx）。
- **框架异常的 `what()` 不再带构建绝对路径**：`[异常] 消息 [文件:行 in 函数]` 那一格原先直接放
  `source_location::file_name()`，也就是编译期展开的整条路径（`G:\Codes\...\Exception.cpp`）。
  本仓的约定是「`what()` 带抛出点、不外回」，但这挡不住下游按自己的判断外回——构建目录就成了白送的
  信息面；同一份日志在不同机器与 CI 之间也对不上。现在只留末段文件名，两条分隔符都认（MSVC 反斜杠、
  GCC 正斜杠）。**完整路径没有丢**：它仍在 `location()` 里，结构化读取的口不受影响。
- **Conan 包补上 `iphlpapi`**：`src/Platform/CMakeLists.txt` 把 `ws2_32 / Mswsock / iphlpapi` 作为
  PUBLIC 依赖链进 Platform，而包这一侧的 `cpp_info.components["platform"].system_libs` 只写了前两项。
  静态库不会把外部依赖带给最终可执行文件，少一项就是消费方链接期一条未解析外部符号——而
  `Platform/IO/NetworkInterface.h` 是公开头、里面直接 `#include <iphlpapi.h>`
  （`if_nametoindex` / `if_indextoname` 在 Windows SDK 里由 iphlpapi 实现，IPv6 的 `%接口名` 写法靠它换算）。
- **Conan 包不再复述 `std::stacktrace` 的支持矩阵**：配方原先写死「Windows 一律没有、Linux 只认
  gcc ≥ 13」，而真正的判据是 `src/Base/CMakeLists.txt` 里那次 `check_cxx_source_compiles`——它是现场探测，
  而 `ASYN_HAS_STACKTRACE` 是 PUBLIC 编译宏，`Base/Exception/StackTrace.h` 按它换 `CapturedStackTrace`
  的**类型**（有栈时是 `std::stacktrace` 的别名，没栈时是一个空的替身类）。这不是推演：本机
  MSVC 的构建命令行里就带着 `-DASYN_HAS_STACKTRACE=1`，而按旧配方打出来的包会告诉消费方「没有栈」，
  于是两边对同一个类拿到两份布局，静态链接还是一片绿灯。现在探测结果由 CMake 落一份标记文件随
  `install` 进包，配方读它（连同 `stdc++exp` 这类只在特定工具链上要链的库一起），矩阵那份复述删掉；
  标记读不出来时**拒绝打包**而不是退回猜的矩阵。

### 破坏性变更

- `WebSocketPeer::setPerMessageDeflateEnabled(bool)` → **`setPerMessageDeflate(std::optional<PerMessageDeflateWindow>)`**。
  迁移：`peer.setPerMessageDeflateEnabled(negotiation.accepted)` 改成 `peer.setPerMessageDeflate(negotiation.window)`
  ——「是否启用」与「两侧窗口位数」本来就是同一个协商结论的两半，分成两个开关就会出现只开一半的形状
  （按压缩发、按明文收），而旧签名也无处携带位数。旧写法之所以不可用：它对协商结果只做了一个布尔，
  无论对端声明多大的窗口都只能按满档压缩。`deflateWebSocketMessage()` 与 `inflateWebSocketMessage()`
  各多一个带缺省值的窗口位数参数（源码兼容，二进制不兼容）。

## [2.4.0] - 2026-10-01

自 2.3.0 起的累计变化（新增 4、变更 2、修复 3）：非 HTTP 那几台通道第一次有自己的对外读数出口——
`Core::ProcessMetricsRegistry` 把六处「账在本进程里、面板上看不见」的计数（证书自动化、dns-01 写入、
UDP 数据报、worker 崩溃、数据库连接池、TLS 握手）登记成 19 条 Prometheus 读数，随 `/metrics` 一起导出，
应用不需要接线。另一半是把三处「对外说的一套、仓库里做的是另一套」对上：Conan 包路线的元数据与
消费者链接、CI 里那道从来不会红的覆盖率门、以及 README/SECURITY 里那些各写一套的计数。
本版**无破坏性变更**。

### 新增

- **进程级指标注册表**（`Core/Metrics/ProcessMetricsRegistry.h`，`Core::ProcessMetricsRegistry`）：
  `registerMetric(name, help, kind, merge, provider)` 交回一个 RAII 把手（析构或移动赋值即注销，
  赋值时先放掉自己原来那条再接过来者的，反过来写就变成「丢一条读数」），`samples()` 按登记先后交回合并后的读数。
  四条登记时的规矩，都在登记那一刻当场拒（`std::invalid_argument`）而不是留到抓取时才看出不对：
  名字必须匹配 `[a-zA-Z_:][a-zA-Z0-9_:]*`（Prometheus 的合法形状，带空格的名字采集侧会整行丢掉）、
  取值回调不能为空（登记了名字却没有读数可取，导出里就留下一条恒为 0 的假读数）、
  help 里的换行按空白折叠（说明会原样进 `# HELP` 一行，带换行会把导出格式切开）、
  同名重复登记只在**种类与并法与说明三项都一致**时才允许（多台同类对象各登记一份是支持的，
  不一致就是两处代码在抢一个名字——同名会把两条读数并成一个数，悄悄把两个不相干的东西加成一条）。
  并法只有两种：`Sum`（计数与在借数这类可加的量）与 `Min`（到期时刻这类「取最先出事的那个」）。
  注册表用**故意不析构的单例**：把手可能持有在静态存储期的对象里，那些对象析构时若注册表已销毁，
  注销就打在死对象上。
- **证书自动化的三条读数**（`asyn_acme_certificate_expiry_seconds` / `asyn_acme_issuances_total` /
  `asyn_acme_failures_total`）：在 `AcmeCertificateManager` 构造时登记，不等第一次签发——常驻进程里这几条
  长期为 0 就是要报的事，而「自动化没跑成」与「自动化还没跑」在面板上是同一个形状，得让「有没有登记过」
  这一格可分辨。到期时刻那条按 `Min` 并（多个管理器取最早那张，因为它会先出事），其余两条按 `Sum`。
  `AcmeManagerStatus` 同步补 `backoffUntilUnixSeconds`：失败退避的门槛时刻，0 表示没在退避中。
  它和续期循环用的是同一个算式（`backoffGateUnixSeconds()`），少了这一步就会出现「判据已经排到两小时之后、
  面板上说下次尝试是马上」——值班据此判断这条自动化是不是躺平了，两个解释比没有解释更糟。
- **dns-01 写入的耗时读数**（`asyn_acme_dns01_records_published_total` / `asyn_acme_dns01_publish_seconds_total` /
  `asyn_acme_dns01_quiet_waits_total` / `asyn_acme_dns01_quiet_wait_seconds_total`）：阿里云云解析的写入器
  自己记录调用条数、花掉的秒数，以及「同名重写前先等机构缓存过期」等掉了多少次与多少秒。
  这一台值得单独记账是因为它的一段等待是**功能而不是停顿**：一张同时含基础域名与通配符的单里两个名字撞
  同一条 `_acme-challenge`，等不满记录的 TTL 就去自证，机构读到的还是上一条答案。没有这两条读数，
  「一次签发跑了十一分钟」看上去像卡住了，实际是等了两个 600 秒。
- **四处「有账没出口」接通**：`UdpServer` 的四条计数（交付给处理器的数据报、发出的、该发没发出去的、
  处理器抛出异常被接住的）、`WorkerSupervisor` 的两条（worker「起来就崩」的累计次数、已放弃补位的槽位数）、
  `ConnectionPool` 的四条（此刻在借的、正在等空闲连接的、历史上创建过的、借出等到截止仍未拿到的）、
  `Core::TlsSocket` 的两条（完成的握手条数、其中按会话恢复完成的条数——两条相除就是本端复用率）。
  四类里每一处都早就在自己的对象里记着账，只是没有一个出口：`asyn_udp_datagrams_unsent_total` 那种
  「该发没发」的数只有在出问题时才需要看，而那时最不该做的事是重启成一个带打印的版本。

### 变更

- `/metrics` 的输出末尾多出注册表那一段，按登记的**完整名字**原样导出、**不套**监听器的 `metric_name_prefix`
  （这些是进程量，加监听器前缀会让同一个数在两个抓取点上长得不一样）。没登记过就没有那一行，而不是一行 0。
  两类读数刻意不接：跨线程不安全的对象（出站连接池明写「协程挂起期间被别的线程驱动会踩坏套接字状态」，
  而抓取跑在另一条线程上）与「读口本身带副作用」的（`WorkerSupervisor::runningWorkerCount()` 会顺手回收子进程）。
- **Conan 那条包路线的自述改成与实际一致**（`packaging/conan/conanfile.py`，四处都不是推测）：
  `license` 从 `"Proprietary"` 改成 `"MIT"` 并把 `LICENSE` 随包交付（`licenses/`）——仓库根那份文本一直是
  MIT；包版本不再写死 `1.0.0`，改由 `set_version()` 现读根 `CMakeLists.txt` 的 `project(VERSION ...)`，
  因为 `scripts/check-release-version.py` 只管「CMake / 更新日志 / 标签」三处，包版本从来不在它范围内，
  「一致性由脚本把关」那句话对这一处并不成立；`description` 去掉早已删除的 wepoll、补上 HTTP/3(自研 QUIC)、
  ACME 与 ORM；C++ 标准不再在两个配方里各重复一份 `"20"`（根 CMakeLists 的 `set(CMAKE_CXX_STANDARD 23)`
  是普通变量，本来就把缓存值盖掉——留着只是第二个答案）。
  README「交付形态」那句「同时认 Conan 的 `shared` 选项」按现实改掉：包路线只出静态
  （`package_type = "static-library"`，配方不提供 `shared` 选项——包这一侧没有会跑共享档的作业，
  加一个没人验过的开关等于造死配置），共享形态走 `-DBUILD_SHARED_LIBS=ON` 直接配 CMake，那条有双端双档门禁。

### 修复

- **证书自动化的到期读数不再在重启后报一段假的「没有证书」**：`AcmeCertificateManager` 构造时先按磁盘上
  那张证书把到期时刻填进内部状态，不必等第一次 `issueIfRequired()`。此前构造完还没跑一轮的窗口里读数是 0，
  而这条读数的报警口径正是「长期为 0」——于是每次计划内重启都会先打一次「证书没了」，而那个文件一直在那里。
  多管理器场景里这一格还会把 `Min` 并出来的整条读数压成 0，盖掉另一张还有八十天的证书。
- **Conan 包消费者链不上**：调用栈这条能力在 CMake 侧是 PUBLIC 的——探测通过后同时给 `Base` 挂
  `ASYN_HAS_STACKTRACE=1` 编译宏与 `stdc++exp` 链接（`src/Base/CMakeLists.txt:25-42`），库内的使用者靠
  传递性自动拿到。包这一侧两遍都没说：少 `defines` 让消费方按「没有栈」的那一份头去编
  （`Exception/StackTrace.h:23/32/54` 三处按这个宏分叉），而 `libBase.a` 里编进去的是另一份形状；
  少 `system_libs` 就是链接期 `undefined reference to __glibcxx_backtrace_simple`。
  这不是推演：本轮按 `conan create packaging/conan` 真跑，消费方冒烟正是挂在链接那一步。
  修好后同一命令全绿——导出为 `asyngyanis/2.3.0`、包内带 `licenses/LICENSE`、冒烟程序链上并跑出读数。
- **覆盖率那道门从来没有变红的能力**：CI 的汇总步骤写成 `gcovr ... --fail-under-line $阈值 --txt | tee ...`，
  作业吃的是 `tee` 的退出码。上一次发布的运行里 gcovr 直接抛 traceback（GCC 把分支命中数报成负数，
  `Base/Log/LoggerRegistry.cpp:95` 那条 `-1992`），整份日志一个百分比都没有，作业与整条 run 照样报绿。
  现在显式取 `PIPESTATUS[0]`（不把判据押在 runner 给不给 `pipefail` 上），再要求「TOTAL 行里真有百分比」
  才算跑过——退出码为 0 而交一张空表是最容易伪装成通过的一档；实测读数一并打进作业摘要。
  负命中按 gcovr 自己提示的口径降级成每文件告警一次，`gcovr` 顺手钉到复核过的 8.6（判据依赖它的输出格式）。

## [2.3.0] - 2026-09-30

自 2.2.0 起的累计变化（新增 7、变更 1、修复 5）：证书自动化补齐第二条自证通道并第一次可整份写进配置
（DNS-01 / RFC 8738，含阿里云云解析的实现与对着 Let's Encrypt 生产机构签出的真证书；`acme` 段的读法与
13+3 个键的拒绝面）；多进程部署的三道整机口径终于按进程摊开（连接数、按来源 IP、限流速率与桶容量）；
运维端点收成三面（Bearer 令牌、只听回环的管理口、多进程各听一个抓取口）；Windows 的 worker 第一次随
master 一起消失而不是变孤儿。本版**无破坏性变更**，但升级时有两处要知道：① 并发限额的默认值从「不限」
改成有限值（每监听器 4096、单来源 256），要旧行为就显式写 0；② 共享形态下 `AcmeCertificateManager`
的构造函数多了第四个参数（默认空的 TXT 动作对），这是**源码兼容但符号不兼容**——换 `.dll`/`.so`
要连着换头文件并按同一套编译器重编消费方，正是 README「交付形态」那条 ABI 边界的日常形态。

### 新增

- **证书自动化的设置第一次可以整份写进配置文件**：新增 `Net/Acme/AcmeAutomationConfig.{h,cpp}`——
  `readAcmeConfiguration(root)` 读 `acme` 段（13 个键加 `dns` 那 3 个，**段内未知键即拒**，与 `server` 段同一套口径），
  交回 `AcmeAutomationConfiguration`：管理器那一整份设置（目录、域名列表、证书/私钥/账户密钥/账户状态四个落点、
  联系人、条款、续期窗口，以及 `renewal_check_interval_minutes` 那档节拍）、自证通道种类、DNS 提供方 / 主域名 / 记录 TTL，
  外加 `buildDns01TxtWriter(loop, cfg)` 按 `dns.provider` 造那对 TXT 动作。**AccessKey 刻意不给配置文件留位置**：
  只从 `ASYN_ACME_DNS_ACCESS_KEY_ID` / `ASYN_ACME_DNS_ACCESS_KEY_SECRET` 读，缺任何一条就在建写入器时拒——
  一把能改域名记录的钥匙进了版本库等于把域名交出去。`dns` 段与 `challenge` 互为条件、两个方向都拒：
  只改其中一格忘了另一格时，机构的答复只会是「查不到/判 invalid」，看不出是配错了。第一个消费方是签发探针
  （`acme_issuance_probe --config <file>`）：文件那份是默认，命令行上**显式给出**的 `--domain` / `--contact` /
  `--challenge` / `--state-dir` / `--dns-zone` / `--dns-ttl` 才覆盖它，并打一行 `CHALLENGE … FROM cli|config`
  说明这次是哪份在生效。
  顺带修掉这条接线引出的判据错位：`--config` 写 `challenge: dns-01` 时，原先排在命令行解析阶段的
  「走 http-01 必须给端口」会先看到默认的 http-01，把一份合法配置挡在门外——判据已挪到通道定下来之后。
  新增 16 例钉取值、边界与全部拒绝面；对着 Let's Encrypt staging 用**纯配置文件**（域名、四个落点、通道全在文件里）
  签出过 `gyanis.space` + `*.gyanis.space` 一张单。**服务端还没吃这一段**（`echo_server` 与装配出口都不读 `acme`）：
  缺的是「换完新证书把那张装回运行中的监听器」那条通路，没接之前把 `acme` 写进部署配置不会让证书自己续。
- **限流的整机口径现在也真的摊到每个 worker 进程**：新增 `perProcessRateLimit(整机速率, 整机突发量, 进程数)`，
  与上一轮的连接数摊分补成同一套规则，但**取整方向刻意相反**——速率按精确除法（0.5 请求/s 不能向上取整成 1，
  那等于让一台进程放行两倍），桶容量向下除后**兜在 1.0**（容量不足一枚令牌的桶一个请求都放不出，
  `TokenBucket` 会直接构造失败）。`rate_limit.requests_per_second` 与 `burst_capacity` 配的是整机的数、
  又起 N 个进程，此前实际放行的是 N 倍。`HttpServerAssemblyContext` 的注释现在写明「传桶就必须是摊后那份」，
  装配出口拿 `TokenBucket` 新增的两个观测读数（`tokensPerSecond()` / `burstCapacity()`，不参与判定）
  与摊分结果比对，不一致当场拒——配置没生效必须响，而不是安静地多放行。
- **运维端点可以只听一个管理口**：`server.metrics_port`（0 = 端点仍留在业务口上，行为与加这两项之前逐字相同）
  与 `server.metrics_address`（默认只听回环 `127.0.0.1`）。非 0 时业务口**不再注册**
  `/metrics` 与 `/debug/loops`，端点归调用方另起的那台监听器——业务口可以继续开 `0.0.0.0`，而内部计数与
  每条循环的状态只听回环。注册逻辑收成唯一一份实现（`registerOperationEndpoints` 模板），因为分两处写
  迟早出现「业务口撤了端点、管理口忘了加闸」那一半。多进程部署配 `WorkerSupervisor::Configuration::workerIndexArgument`：
  master 给每个 worker 传槽位序号（补起崩掉的 worker 沿用同一个号），管理口按「base + 序号」错开，采集端才能
  按进程聚合，不再出现「一次抓取随机命中某台、计数器在两次抓取之间回落」。端口加法走 uint32 再判 65535，
  越界当场拒（回绕会去听一个谁也没配的号）；`metrics_address` 在读配置时就判能否解析成 IP 字面量，
  不留到 bind 那一刻只剩一个平台错误码。
- **证书自动化多了 DNS-01 这条自证通道（RFC 8738）**：`AcmeClient::Configuration::challengeKind` 决定挑哪一种挑战，
  默认仍是 http-01（行为与加这个字段之前逐字相同）。`AcmeDns01TxtWriter` 把「发布 TXT / 撤回 TXT」两格交给调用方——
  做成两个返回 `Core::Task` 的 `std::function` 而不是抽象基类，因为协程不能是虚函数，而各家 DNS 的差异只在
  「调哪个 API、按什么格式签」。仓库内自带第一家实现 `makeAliyunDns01TxtWriter`（阿里云云解析 RPC 风格 OpenAPI）：
  `publish` 报成功之前先用 `DescribeDomainRecords` 确认这条记录在权威侧查得到、再等一段结算时间，
  `withdraw` 反查 RecordId 逐条删且查不到即成功（撤回必须幂等）。凭据由调用方交给 `AliyunDns01Configuration`（引擎侧没有 acme.* 服务端配置段；
  签发探针那两个值走环境变量 `ASYN_ACME_DNS_ACCESS_KEY_ID` / `ASYN_ACME_DNS_ACCESS_KEY_SECRET`，缺任一起在起步时就拒）。
  `AcmeCertificateManager` 按「有没有交来动作对」选通道，一次签发只走一条，且写入之后的**每条**出口都撤——
  含写入自己报失败那一条（控制面超时的那次可能其实已经写成了，留下的 TXT 会让下一轮在同一名字上读到两条答案）。
  新增失败种类 `DnsRecordRejected`；`Core::Digest` 补 `hmacSha1`（签名口径规定用它，两档 HMAC 共用同一份
  EVP_MAC 流程）。**这条不需要任何入站通路**，因此被备案拦截或 80 端口不可达的部署也能自动续期，
  也是通配域名唯一可行的自证方式。
  实测（2026-09-30，容器侧 GCC 13 + ASan/LSan/UBSan 的探针二进制）：对着 Let's Encrypt **生产**机构签出
  `gyanis.space` 与 `*.gyanis.space` 两张，`openssl verify` 对系统信任库通过、有效期 90 天；staging 另签两张并
  跑出一轮真续期（两张不同的链）。四次签发之后权威 NS 上 `_acme-challenge.gyanis.space` 查不到残留记录。
- **整机限额现在会摊到每个 worker 进程**：`applyHttpServerConfiguration` 的 context 新增 `workerProcessCount`，
  `maximum_connections` 与 `maximum_connections_per_ip` 按进程数向上取整摊到每台（`perProcessShare`）。
  每个进程只数自己那份账，配置里的整机数起 N 个进程就是实际放行 N 倍——这一条与「多监听器各持一份限额器
  等于上限乘以监听器数」是同一个坑的另一半。摊分规则只有一处实现，示例报生效值时用的也是它：
  实测 `--workers 2` 配 100 打出「整机 100 摊给 2 个进程 → 每台 50」。
  共享限额器与配置的比对改用摊后的份额，`workerProcessCount=0` 当场拒。
  摊分是近似（内核按连接分散，长连接偏斜时某台瞬时仍可能高于份额），要精确的全局闸需要共享内存或外部存储，
  本版没做，配置文档里也这么写。`TcpServer` 补了 `maximumConnections()` 读数，运维能核对到本台真正卡多少。
- **运维端点可以要令牌了**：`server.ops_bearer_token` 给 `/metrics` 与 `/debug/loops` 挂一道 Bearer 闸门
  （`Net::opsAccessMiddleware`），未授权回 401 并带 `WWW-Authenticate`。`/healthz` 刻意不在保护名单里：
  存活探针要能被编排器无凭据访问，给它加令牌的结局通常是探针长期失败后被人体谅性地关掉，那比暴露几个计数更糟。
  这条只能写在配置文件里（命令行上的令牌会进 shell 历史与进程列表）；比较走新的 `Base::constantTimeEquals`，
  逐字节短路的比法会把「前几位猜对了」泄漏进耗时。空串令牌、非字符串令牌、以及「配了令牌却没开
  `expose_metrics`」都在读配置时当场拒——三者都长得像已加固而实际保护不到任何东西。
- **Windows 上的 worker 会随 master 一起消失**：`Process::spawn` 新增 `LaunchOptions::killWithParent`
  （把子进程挂进一个带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的作业，作业句柄随 `Process::Handle` 活着），
  `WorkerSupervisor` 给每个 worker 默认挂上。此前 master 被硬杀（`Taskkill /F`、OOM、容器被删）后，
  worker 会继续占着端口而无人编排——Linux 侧有 `PR_SET_PDEATHSIG` 兜住，Windows 一直没有等价物。
  主机已处在一个禁止嵌套的作业里时（某些容器与 CI）挂不上：此时**派生照常成功**（「服务起不来」比「保护缺席」
  更糟），但保护缺席会被点名一次（`Handle::killWithParentGuardActive()` 报 false，编排者落一条 WARN）。
  开关刻意默认关：换代交棒要的正是相反的形状——新一代必须活过交棒的那一代，默认打开会把它做成静默自杀。

### 变更

- **并发限额的默认值从「不限」改成有限值**：`server.maximum_connections` 默认每监听器 4096
  （`kDefaultMaximumConnections`），`server.maximum_connections_per_ip` 默认 256
  （`kDefaultMaximumConnectionsPerIp`，这个数给共享出口——运营商级 NAT、企业代理——留了余量：
  默认值误杀真实客户比多放一点负载更糟，与请求速率不给默认值是同一条取舍）。「0 = 不限」这条**语义不变**——显式写 0 仍然是不限，只是默认不再
  是 0。理由是默认值长得像「已经配好了」而实际把连接表整个交给对端，这类事故只能事后发现。方向是往安全侧挪，
  但两处会当场改变行为，升级前要核对：① 依赖「没写就是不限」的部署会开始撞 4096/256；② 传共享限额器给
  `applyHttpServerConfiguration` 时，装配口的「共享实例与配置标量不一致就拒绝」从此有了真实的标量可比
  ——以前标量恒为 0 等于这条检查空转。请求速率与在途正文总量**刻意仍默认不限**：限流的错误取值会误杀真实用户，
  与放行多一点负载不是同一种危害。`echo_server` 启动现在打一行「并发限额（每监听器）：整机 …，单来源 …」，
  报的是生效值而不是配置里写了什么——「没写」与「写了 0」在配置文件里长得一样，只有生效值能分辨。

### 修复

- **GCC 的 Release 档不再在文件身份那一段编译失败**：`Platform::queryFileBasicInfo` 折文件句柄时读的是
  `struct file_handle::f_handle`——glibc 把它声明成长度 0 的数组（容量由调用方在结构体后面另备），
  于是 `-O3` 这一档 GCC 能「证明」按下标读它越界，`-Werror` 直接建不过：
  `error: array subscript index is outside array bounds of 'unsigned char [0]' [-Werror=array-bounds=]`。
  Debug+ASan 那档优化不出这条，所以只有走 Release 的作业（HTTP/3 验收那条）会红。改成一份逐字段对齐的
  镜像结构（`uint32` 容量 + `int32` 类型 + 定长 128 字节数组）：交给系统调用的还是同一个对象，
  而读句柄字节那一侧的下标落在编译器证得出的范围内；`offsetof` 把两边的句柄起始位置钉死，
  ABI 一旦漂移就在编译期红。内核改写过的那个长度按容量钳一道再进下标。
  判据：容器 GCC 13.3.0 以 CI 同一条命令行编 HEAD 那份复现出同一处报错，编新这份 0 诊断 0 告警通过。
- **dns-01 在「基础域名 + 它的通配符」同一张单里不再抢跑**：RFC 8738 把 `example.com` 与 `*.example.com`
  两条自证映射到**同一个**名字 `_acme-challenge.example.com`，而两条的答案不同（token 各自一条）。原流程逐条
  「写 → 自证 → 撤」，撤完立刻为下一条写同名记录，而机构按**那条记录自己的 TTL** 缓存上一条答案，于是第二次自证
  读到的是第一条的值，原文只回 `During secondary validation: Incorrect TXT record "…" found at …`。
  实测这个区上 **3/3 全红**（每轮新建账户，两条自证都必须真走一遍），不是抖动；「把 TTL 调小」这条路在这家走不通——
  阿里云这个区的地板是 600 秒：写 60 秒时 `AddDomainRecord` 六次全回 `The specified TTL is invalid`，写 300 秒同样被
  `QuotaExceeded.TTL` 拒（失败原文带得出 HTTP 400 与那个 Code，配置读的一侧没法预先知道别家的地板，所以照原样交回）。
  同一次跑留下一份对照读数：撤掉之后权威侧（`ns1.alidns.com`）已经答不出这条，递归解析器（`223.5.5.5`）
  在 25 秒之后**仍在答那条已删的 TXT**——缓存确实不在我们这一侧。
  现在写入器记住本进程内真删过记录的名字，`publish` 在同名重写之前先等满那条记录的 TTL
  （`aliyunRewriteQuietPeriod`，上限 15 分钟，免得 `record_ttl_seconds: 86400` 这类手误把一次签发挂死），
  并打一条 INFO 说明在等什么；`AcmeDns01TxtWriter` 的公开契约里补上了这条要求，接别家 DNS 的照做。
  这段等待刻意**不跨进程**，所以接连重跑两次签发之间也要隔过 TTL 才读得到新答案。
  新增两条用例钉算术（刚撤完等满整条 TTL、过一半只剩一半、等过了不再等、天量 TTL 落在上限、时钟读反按 0 处置）。
- 通配域名的下单拦法改了：`createOrder` 的本地判据原文是「本通路只走 HTTP-01，它验不了通配域名」，那是
  只支持 http-01 时留下的形状，dns-01 落地之后就变成了假限制——RFC 8738 开通配正是它的用途。判据现在挂在
  实际挑了哪种挑战上：http-01 照旧拒并改口提示可以换 dns-01，dns-01 下放行 `*.example.com`，
  同时把形状钉死为「`*` 占满最左一段标签」，去掉前缀之后的剩余部分照旧过 LDH 规则。
  两次突变分别归因：把放行条件钉回永远为假时两条新用例同时变红，只撤形状判据时只有「畸形通配被拒」变红。
- `echo_server --h3` 现在会把 `server.maximum_connections` 也交给 HTTP/3 监听器。此前只有两条 TCP
  通道吃这个值，h3 一直用 `QuicServer` 自带的 1024：同一份配置下三条通道的并发口径不一致，而本轮新增的
  h3 满载读数（`asyn_http_over_limit_rejected_connections_total` 与跳变 WARN）会对着一个没人配过的数
  跳变。配置里没写（0 = 不限）时刻意保留 h3 的默认档而不是把它放开——与单来源限额的 `> 0` 判据同一条理由。
  生效值现在会随启动打出（`HTTP/3 的并发连接上限 N`）：实测配置写 2 时打出 2，不写时打出 1024。
- **Linux 原生文件系统上「删掉再同名重建」现在认得出来了**：`Platform::queryFileBasicInfo` 与
  `queryOpenedFileBasicInfo` 的身份标记原本只折 (设备号, inode, ctime)，而 ctime 取自内核的粗粒度时钟
  （HZ=1000 就是一毫秒一粒）。原生 ext4 上实测 50 次紧凑的「删→建→把 mtime 原样搬回去」里 **ctime 逐位
  相同 16 次、inode 复用 50 次**——落在同一粒之内时这个三元组无法区分两个文件对象，而它是映射缓存的命中
  判据之一。现在两条查询口都优先折入 `name_to_handle_at` 给的句柄字节（除 inode 号还带**世代号**，inode
  被回收再发给新文件时必变）与装载点序号：只一次系统调用，不像 `FS_IOC_GETVERSION` 那样为拿一个数再付
  `open`+`ioctl`+`close` 三次。文件系统给不出句柄时（容器 overlayfs 实测恒回 `EOPNOTSUPP`）退回原来的三项
  折法，并按设备号记住这一侧的判定，否则这类文件系统上每次静态文件查询都要白付一次必失败的调用——实测
  283 纳秒，按设备缓存后与裸 `stat` 的差值降到 16 纳秒。
  判据与证伪：新增一条「按路径查与按句柄查必须给出同一个标记」的直测（此前 `queryOpenedFileBasicInfo`
  零直测，而两条路口一旦分叉，每个请求都会被误判成正文已过期且上层全绿看不出来）；原用例
  `FileBasicInfo.DistinguishesARecreatedFileAtTheSamePath` 在 ext4 上从 20 次红 19 变 20 次全绿；两处突变
  分别归因——摘掉句柄项后该用例 20 次红 18，只摘「按句柄查」那一路则只有新用例红。
  **窗口有多窄要说清楚**：端到端没能复现出「缓存端出旧正文」——在同一台原生 ext4 上让服务端
  `--static` 出 64 KiB 正文、客户端紧循环做 300 轮等长原地替换并把 mtime 搬回，修复前的实现也
  300/300 端出新正文、0 次命中旧条目。原因是服务端两次查询之间隔着整个请求往返，两个文件的创建时刻
  因此跨过了那个 tick；只有两次创建落在同一粒之内（如部署工具原子替换后立刻被复查）才会退化。所以这一条
  修的是判据本身的可靠性，而不是一个已观测到的线上故障。Windows 侧不变（身份仍取自创建时间的 100 纳秒
  刻度，NTFS 隧道缓存认不出这种替换那条记档照旧）。

## [2.2.0] - 2026-09-29

自 2.1.0 起的累计变化（新增 4、变更 5、修复 1）：七项投产核对逐项落地——`server` 段的配置有了
唯一装配入口（`applyHttpServerConfiguration`）、整机满载被拒有了读数与跳变告警、HTTP/2 的
SETTINGS 补齐合法上界与「0 一律拒绝」、ACME 缺联系人不再静默、Windows 的 worker 第一次能体面
退出（派生时给独立进程组，停机时向该组发 `CTRL_BREAK`）、示例的 `--config` 会连同 `logging` 段
一起装上。本版**无破坏性变更**：`requestTermination()` 在 Windows 从「一律 false」变成「给了
独立进程组才发得出」，多出来的是成功路径，原有调用点的失败处置一字未改；静态形态的产物与符号
不变。共享形态要注意一处：`Process::Handle` 多带一个字段，换 `.dll` 要连着换头文件并按同一套
编译器重编消费方——这正是 README「交付形态」里那条 ABI 边界的日常形态。

### 新增

- **`server` 段的配置现在只有一个对接点**：`Net::applyHttpServerConfiguration(server, configuration, context)`
  （`Net/Http/HttpServerAssembly.h`）。此前 `HttpServerConfiguration` 的七个字段在库内没有任何消费方，全靠
  调用方逐台手接六七个 setter——`expose_metrics` 就是这么变成「配置里打了勾、`/metrics` 一个都没注册」的死键。
  收进一处之后，`expose_metrics=true` 会真的注册 `/metrics`、`/healthz` 与 `/debug/loops`（三件套同开同关：
  它们都不做鉴权，只开其一会让「抓不到数」与「以为没暴露」互相伪装）。跨监听器共用同一份限额器/限流桶时由
  调用方经 `context` 传入，而**传入的共享限额器与配置标量不一致时当场拒绝**——静默挑一边就是
  「配置写 16、实际跑 64」且完全看不出来那一类事故。示例 `echo_server` 已改走这条路。
- **整机满载第一次有了自己的读数**：`/metrics` 新增 `asyn_http_over_limit_rejected_connections_total`。
  它与既有的 `asyn_http_admission_rejected_connections_total`（按来源 IP 那道闸门）刻意分成两条——
  「某个来源在刷」要收紧限额、「整机容量到顶」要加 worker 或抬上限，处置正好相反，合成一条就分不出该做哪件。
  过去 TCP 侧撞 `maximum_connections` 既无日志也无计数，运维看到的只是「连上就被关」。
- **README 新增「投产前核对」一节**：TLS 下限、ACME 联系人与条款、限额与背压、`/metrics` 接线、日志等级与
  滚动、worker 起法、优雅停机这七项，逐项写清键名、内置默认值、怎么确认它真的生效、以及配错的后果。
  这些位置的共同点是「库不替你决定，但默认值看起来很像已经配好了」——只看配置文件不足以发现，
  所以每条都给了可自己验一遍的读法。
- **Windows 上的 worker 现在能体面退出**：`Process::requestTermination()` 此前在 Windows 一律返回 `false`
  （那边没有信号），编排者只能直接强杀。现在 `LaunchOptions::ownProcessGroup` 让子进程进**自己名下**的进程组，
  停机时向那个组发 `CTRL_BREAK`，装了 `Core::GracefulShutdown` 的 worker 便照常走完 `stop()` / `drain()`。
  `WorkerSupervisor` 派生 worker 时默认就给独立组；宿主没有控制台（服务、被 `DETACHED_PROCESS` 派出来的进程）
  时事件无处投递，会落一条 WARN 再强杀——「优雅」落空这件事不出声，与静默把它换成强杀是同一类事故。
  刻意不给独立组时本方法仍返回 `false`：控制台事件的投递单位是进程组，此时唯一的目标就是宿主自己所在的组。
  突变验证（三条各钉一头）：退回「Windows 一律返回 false」→ 探针外层红；去掉进程组判据 → 外层红，且红的原因是
  探针子进程被自己广播出去的 CTRL_BREAK 带走，正是这道判据要防的那件事；移动构造不带进程组归属权 → 外层红，
  而三条内层探针里只有「移动后仍叫得停」那条红，另两条照旧绿。

### 变更

- 撞并发连接上限而新连接被拒时，TCP 与 h3 两侧都改成**只在跳变上报一条** ERROR/WARN（名额空出来再撞满
  才算新一次），h3 原先每条被拒的 Initial 都报一行，满载时会把日志刷满而那条信息一句没多。
- HTTP/2 的连接层配置校验补齐：`INITIAL_WINDOW_SIZE` 不得超过 2^31-1（线上是有符号 32 位，越界值要么被
  对端按连接错误收场、要么本端把窗口算成负数）；`maximumHeaderListSize` 与 `maximumHeaderBlockByteCount`
  取 0 一律拒绝——0 的语义是「一律拒绝」，而这正是把它误当成「0 = 不限」时会写下的值。
- ACME 新建账户时若 `contactEmailAddress` 为空，现在会落一条 WARN 说明「机构无法在证书到期或账户异常时找到
  你」。仍然放行：不带联系人是 RFC 8555 允许的（带 EAB 的机构常常本来就留空），拒绝会把合法部署挡在门外。
- `echo_server --config` 现在会连同 `logging` 段一起装上（`Base::LoggerConfigLoader`，相对路径按配置文件所在
  目录解析）。此前只有 `server` 段有消费方：部署方在同一份文件里写的日志等级与滚动参数只有 ConfigManager
  知道，症状是「配置写了没生效」而不是报错。
- `echo_server --workers` 的帮助文本按平台改准。Windows 上本示例确实起不了多进程（那边要靠 master 移交监听
  套接字，而示例的 worker 分支没有接管入口），文本却仍写着「靠 SO_REUSEPORT 分摊、SIGTERM 体面退出」——
  现在明说这一档只有 POSIX 有，并把移交形状指向 `samples/core_worker`。

### 修复

- 共享形态之外也暴露的一处运维盲区：`AsyncSink` 的构造/析构回归用例原先靠「30 秒内跑完 2000 次」这条
  绝对时限判定，满载机器上量的是负载而不是「停止请求的唤醒会不会被丢」；改成**停摆检测**（每 5 秒必须
  推进至少一步，另设一条不参与判定的兜底上限）。突变验证：把等待谓词改成读不到停止标记，用例在
  5112 毫秒即红并报出停在第几步；还原后全绿。

## [2.1.0] - 2026-09-29


自 2.0.0 起的累计变化（新增 1、修复 1）：五个模块补上共享库交付形态——一个 CMake 标准开关出
`.so`/`.dll`，导出面按逐类标注决定而不是按公共头全集决定；顺带修掉日志线程 ID 快照在跨模块时被复制成
每模块一份这件事。本版**无破坏性变更**：静态形态的产物、落点与符号一字未动，`find_package` 与 Conan /
vcpkg 的既有用法不受影响；新形态的三条边界（标准库类型跨 ABI、OpenSSL 两侧各一份静态副本、Windows 的
DLL 落点）写在 README 的「交付形态」一节。

### 新增

- **五个模块支持共享库交付形态（`-DBUILD_SHARED_LIBS=ON`）**：此前只有静态库一条路，`add_library` 写死
  `STATIC`，公开头也没有符号可见性标注——想按 `.so`/`.dll` 分发的人得自己补一遍导出宏，而这件事最容易出
  问题的地方恰恰是「漏标一个类」：链接期才炸，且炸在别人的下游。现在认的是 CMake 的标准开关（Conan 的
  `shared` 选项与 vcpkg 的 triplet 直接映射过来，不再造一个本仓专属名），导出面由 `src/AsynGyanisExport.h`
  里的 `ASYN_<模块>_API` 逐类标注决定，配合默认隐藏可见性——**没标注的符号出不去**，因此下一次重构不会
  不知不觉换掉线上符号。三处形状问题只有共享形态才会暴露，都已按判据修掉：嵌套类型不随外层类一起导出
  （`Platform::Process::Handle` 要自己标）、`dllexport` 会把隐式拷贝也实例化一遍（`Router` 装着
  `unordered_map<string, unique_ptr<Router>>`，拷贝赋值在这条路上编不过，于是显式声明拷贝为删除、移动保持
  可用）、整份实现都在头里的类挂上导出宏反而会让使用方去找一个库里根本没有的导入桩（`SchemaMigrator` 的
  私有静态助手）。Windows 侧另补一条落点规则：Ninja 生成器不替你把 DLL 拷到可执行体旁边，缺了就是启动即
  `0xC0000135`，因此共享形态下五个 DLL 与可执行体一起收在 `build/bin/`（静态形态的落点一字未动）。
  边界写在 README 的「交付形态」里：接口上的标准库类型跨 ABI，生产方与消费方要用同一套编译器与 CRT；
  OpenSSL 在库与消费方两侧各有一份静态副本，不要把 `TlsContext::nativeHandle()` 交给自己那份 OpenSSL。
  验证：Windows 静态 `3573/3573` 零告警、ASan 零命中；Windows 共享 `3573/3573` 零告警（跳过数从 73 涨到
  143）；Linux 静态 `3593/3593`、Linux 共享 `3593/3593` 均零 sanitizer 命中（跳过数 70 → 82），
  `libNet.so` 导出 2146 个框架符号。共享形态下 Windows 有 70 条用例按判据新转 SKIP——分配台账用例靠替换
  全局 `operator new` 计数，而 Windows 上这份替换只覆盖可执行体本身，DLL 内的分配不进钩子（读数恒为 0，
  既让「>0」的判据假红也让「==0」的判据假绿）；TLS 直连用例把库里建好的 `SSL_CTX` 交给用例侧另一份
  OpenSSL 的 `SSL_new()`，两份额度的错误队列下测的已不是被测语义。Linux 的动态链接让可执行体里的定义
  覆盖全进程，所以那 58 条分配用例在 Linux 共享形态下仍然量得到，只有 TLS 那 12 条转 SKIP。

### 修复

- **日志的线程 ID 快照在共享形态下每模块一份**：`Base::threadIdString()` 原先是头里的 inline 函数带
  `thread_local` 缓存，静态形态下全进程只有这一份；库出成 DLL 之后每个模块各持一份副本，「整线程一份」
  退化成「每模块每线程一份」，而按指针身份断言共享关系的用例正好撞在这上面。定义移到 `Base/Log/LogEvent.cpp`
  并由 `ASYN_BASE_API` 导出，快照回到每线程一份。

## [2.0.0] - 2026-09-29

自 1.1.0 起的累计变化（326 条：新增 89、变更 27、修复 154、性能 56）：HTTP/3 与 QUIC 的连接外壳改由
自研状态机驱动，ngtcp2 退为测试裁判；证书自动化整层落地（RFC 8555 三层 + h3 证书热轮换 + 拿 Pebble
做的跨实现验收）；Windows 侧补上多进程 worker 的监听套接字移交、零停机换代通道与数据报接管；新增对外的
`UdpServer`、出站 HTTP/2 与 HTTP/3 客户端及三条通路的逐批交付；路由开始按 Host 分站点；配置接环境变量
覆盖；观测面补链路追踪与 `/debug/loops`，三条 HTTP 协议的指标与限额口径对齐；交付形态上补 SBOM、
依赖公告台账、CODEOWNERS 与漏洞响应时限，并把 libFuzzer、ThreadSanitizer、h2spec、Autobahn、aioquic
接成会红的作业。

**本版含破坏性变更**（16 笔带 `!` 的提交，逐条见「变更」段）。对外部代码有直接影响的几处：删掉已随安装
导出的公开类型 `Base::NetworkException`、`Base::ConfigFileException`、`Base::ConfigParseException`，以及
`Config` 侧无调用方的 `getRequired`、`splitKey` 与 `loadTime` 死字段；出站 URL 缺协议名不再静默按 http
发出；MySQL 写语句的行数改报「匹配数」，与 SQLite 同口径；ORM 的引用表名规则收到一处，插入方向不再整块
引用；多处参数误用从「静默容忍」改走用法错误异常（`addWorker`、`WorkerSupervisor` 的配置、传输层参数）；
阻塞任务执行器从 `Database` 上收到 `Core`；日志事件改带时刻、时间戳文本推迟到格式化器；区间约束不再把
「比不出来」当成校验通过。从 1.1.0 升级前请先读「变更」段。

### 新增

- **语句被拒时带出驱动原生码与可重试判定**（`Database::QueryExecutionException::nativeErrorCode()` /
  `isRetryable()`，`Database::DatabaseConnection::lastNativeErrorCode()`）：本框架把
  `QueryExecutionException` 拆成独立类型的全部理由就是「重试语义不同」，可原生码此前只拼在消息
  文本末尾的「（错误码 N）」里——调用方要区分 MySQL 的 1213 死锁（服务端已回滚本事务、就等你重放）
  与 1062 唯一键冲突（重试一万次也不会变好），只能去匹配中文。文本与码现在存在同一个
  `ErrorRecord` 里成对更新，并且**任何只写文本的赋值与 `clear()` 都会把码重置为 -1**，
  因此码永远不会配到上一条错误的文本上。刻意不在读取时现取句柄错误码：MySQL 的语句错误挂在
  *语句*句柄上，连接级 `mysql_errno` 此时读到的可能还是上一条的陈旧码，配错的码比没有码更危险。
  `isRetryable()` 取保守白名单（1205/1213/5/6），默认 false：少重试一次只是一趟往返，
  多重试一条已生效的写语句是二次写入。旧签名保留、消息文本逐字不变。
- **事件循环的自观测快照补两条口径**（`Core::EventLoopSnapshot` 的 `stoppedByFailure` 与
  `failedDispatchCount`，`Core::Scheduler::failedDispatchCount()`）：回答「这条循环是被 `stop()`
  干净停下的，还是被一次逃逸的抛出带走的」，以及「这条循环上有多少次投递抛出被就地收下」。
- **文件变更监听的用户回调失败计数**（`Platform::FileWatcher::failedCallbackCount()`）：回调是
  消费方注册的代码（配置热加载就走这条路），它抛出既不该带走监听线程也不该丢掉同批其余通知；
  Platform 在 Base 之下、没有日志通路可用，因此把「报过」做成调用方主动读的计数。

- **证书自动化（ACME / RFC 8555）落成三层可用的实现**：换证书此前是一件人做的事——到期前登机器、手工交
  CSR、把新链放回原位、重载服务。Let's Encrypt 那张是 90 天寿命，节奏下忘记一次就是一次线上证书告警，
  而「装了 `/metrics` 却没人看」是常态。现在：
  - **密钥层** `Net::Acme::AcmeKeyPair`：账户与域名密钥的生成/落盘（PKCS#8，0600，原子替换）、JWK 紧凑文本、
    RFC 7638 公钥指纹、JWS 签名（RS256/ES256）与 CSR。两处口径值得单记：JWK 的成员顺序是**手写**的字典序而
    不是 JSON 库的排序（指纹要对 RFC 7638 那个官方向量逐字对上才算数）；ES256 的签名要把 DER 拆成 R‖S 两段
    各 32 字节的裸串，**拼成 64 字节之后只编码一次**——JWA 的写法是「先拼字节再编码」，不是「各编一段再拼」，
    后者解出来的长度根本不是 64。
    账户密钥默认 RS256：RFC 8555 §6.2 只强制机构支持 RS256，对着不实现 ES256 的机构选它会得到一条
    「算法不支持」的注册失败，而不是任何降级。
  - **协议层** `AcmeClient`：目录 → 账户 → 下单 → 授权 → 自证 → 定稿 → 取证这一台状态机。nonce 一次性、
    用完即弃，撞 `badNonce` 重取再发且**有重试上限**（无限重试会把一次网络抖动变成永久卡住）；请求正文是
    JWS 的扁平序列化（`{"payload","protected","signature"}` 配 `application/jose+json`）而不是 compact 三段；
    机构的 problem document 折成中文错误并保留它的 `type` 与 `detail`——挑战被判 `invalid` 时说的是「去查路由
    与端口」而不是含糊成「超时、去查网络」。只实现 HTTP-01：目录里没有它时当场拒绝，不会挑一条自己答不了
    的挑战去 POST（`tls-alpn-01` / `dns-01` 写在类注释的能力边界里）。
  - **运维层** `AcmeCertificateManager`：按「到期时刻减现在够不够 `renewBeforeExpiry`」判要不要续（证书文件
    读不出、解析不动或不存在都按「该签」处理——覆盖一份坏文件是恢复动作，不是要人批准的事故），私钥与证书
    原子落盘、写坏不留半张，签好后经 `ReloadHandler` 装回服务。`runRenewalLoop()` 在**没给装回动作时拒绝启动**
    并把原因记进 `status()`：只往磁盘上写而没人装回，是这类自动化最坏的失败形状——证书每月在换、线上身份
    永远是那张旧的。失败按最小间隔退避，不去反复撞机构的速率限制。
  - **h3 那一侧补 `QuicServer::reloadCertificate()`**：证书自动化喂不到 UDP，就会留下「443/TCP 已是新链、
    同一端口的 QUIC 还是旧链」这种一半新一半旧的状态。轮换失败时旧上下文继续服务并告警一条，不会把监听换成
    一份建不出来的上下文。
  - **裁判是一份独立实现的进程内桩机构**（`tests/Net/Acme/AcmeStubAuthority`）：它按请求头里的 JWK 重建公钥来
    验签（与被测实现不同的一条路）、用自己的 JSON 序列化算指纹、nonce 真的一次性、证书真从交上来的 CSR 签出来。
    这样两边错在同一种理解上时会不一致而不是一起绿——故障注入（badNonce、只给别的挑战类型、限流、账户已删、
    域名被拒）也是桩这一侧才造得出的现场。
  - **真机构那条路按环境变量门控**（`tests/Net/Acme/TestAcmeLiveAuthority.cpp`，门控见 README 的「真机用例」）：
    只走「取目录 + 建号 / 复用账户」，验的是桩验不了的那一半——出站 TLS 过真信任库、目录字段以线上实际写法为准、
    我们的 JWS 由一个不是自己写的裁判验签。真签发的判据要一台公网可达且解析到本机的域名，回归环境给不了，
    故不测。账户材料要求落在跨轮次持久的目录里，否则 SKIP：建号在机构侧是有速率配额的公开动作。
  - `Base` 补 URL-safe 无填充的 base64 通道（`base64UrlEncode` / `base64UrlDecode`），JWS/JWK/指纹都走它；
    既有的 `base64Encode` / `base64Decode` 契约一字未动，两条通路共用同一份字母表实现。
  证伪（2026-09-28 这轮在 Windows/MSVC + ASan 上重跑，ACME 一族 39 例，红集逐条可归因）：
  - `maximumNonceRetries` 的默认值由 3 改成 0 —— 只红 `RetriesWhenTheAuthorityReportsBadNonce`；
  - 把「不是 http-01 就跳过」的类型过滤摘掉 —— 只红 `RefusesAChallengeItCannotAnswer`；
  - CSR 的 subjectAltName 只写第一个域名 —— 红密钥层那条 SAN 用例，并连红客户端与运维层各一条端到端
    （两侧都断言签出的证书覆盖下单时要的每一个域名）；
  - 域名标识的字符判定改成永不拒 —— 只红 `RejectsUnusableDomainIdentifiersLocally`；
  - JWS 正文从扁平序列化改回 compact 三段点分串 —— 17 条流程全红：桩机构按请求头里的 JWK 重建公钥验签，
    形状不对就当场拒，协议层与运维层都要过这一层。
  两处过程记录值得留下：字符判定那一版突变写成了恒真条件，跑完零红——按「红集为空说明突变无效」重写后才红；
  还原突变时用 `copy2` 连同旧 mtime 一起复制，ninja 判定无需重编，于是上一批突变还留在链接产物里（表现为一条
  与本批无关的用例变红）。现在一律写完内容再单独抬 mtime，并在每次还原后重跑一遍确认回到全绿。
  Linux 侧另修掉一处「MSVC 放行而 GCC 拒绝」：嵌套聚合体做成员函数的默认实参，要在此处就用上它的默认成员
  初始化，而后者要到本类闭合才就绪（`AcmeStubAuthority::Settings` 与用例里的 `Round` 两处）。


- **交付与供应链这一层有了可跑的门禁（SBOM + 依赖公告台账 + 代码所有权）**：以前只有「依赖装在哪个版本」
  这一份清单（`conandata.yml`），没有「这个依赖的公告在哪、升级有没有走复核」——新加一条依赖可以完全绕过
  安全核对而没人拦。现在：
  - `scripts/generate-sbom.py` 产出 CycloneDX 1.6 清单（组件按 purl 排序、**不带构建时刻**，所以两次生成可
    逐字节比对；另出 `SHA256SUMS`），并在写完自检一遍「组件数与依赖清单条数一致、每条都有版本与 purl」，
    自检不过就删掉产物并非零退出——一份看着有其实是空壳的 SBOM 不能当审计证据交出去。它记的是清单与来源，
    **不是**哈希后的二进制，需要制品级 provenance/签名时仍要另配签名构建。
  - `packaging/dependency-watch.json` + `scripts/check-dependency-advisories.py`：每条依赖登记 https 公告入口
    与 `pinnedVersion`，脚本判三件事——没登记、版本漂移、台账里残留已删依赖。**这条门禁刻意不宣称「已核对无
    CVE」**：自动判定命中要解析各家公告正文的格式，猜错方向的后果是「绿着漏掉一条真漏洞」，比不跑更糟；
    它只保证「入口在、版本对、链接没死」，公告正文由人读（`--fetch` 时不可达只打提示，上游临时挂掉不是
    本仓库的缺陷）。证伪：把台账里 OpenSSL 的版本改成一不一致，脚本即退出码非 0（实测）。
  - `.github/workflows/supply-chain.yml`（`main` 推送 / 手动触发 / 每周一凌晨定时）跑上面两件事并把 SBOM
    作为制品上传（保留 90 天）；作业里另有一道 shell 判据复核「组件数 == 清单条数」，不只信生成脚本自己说通过。
  - `.github/CODEOWNERS` 按目录路由（协议实现、平台层与事件循环、打包与依赖各自点名），并在文件里写清
    每块目录为什么需要特定 owner。
  - `.github/SECURITY.md` 补三节：**严重度分级与响应时限表**（按「默认配置下远程可达」定级，写明这是维护者
    个人承诺而非合同 SLA）、**披露节奏**（私有报告 → 修复走同一套门禁进 `main` → 发 GitHub Security Advisory
    并署名致谢，默认协调窗口 90 天、最长 120 天）、**「我们靠哪些持续验证」表**（每条门禁覆盖什么、在哪个作业
    里跑，防止把「作业存在」当成「门禁跑过」）。
  顺手改掉 `CONTRIBUTING.md` 里「仓库从未整体格式化过、别对既有文件跑 clang-format -i」这句——它与格式门禁
  已存在的事实相反，照着写会把排版绕过去；现按「改动过的文件提交前跑 `--dry-run --Werror`」写。

- **h3 的监听器也能接手了（`Net::QuicServer` 的接手构造与不带地址的 `listen()`）**：上一格补的是数据报
  这一侧的接手动作，而 QUIC 服务端只有「按地址 bind」一种起步方式，于是 Windows 上多进程跑 h3 仍然走不通
  （多个进程各自 bind 同一端口时内核把全部报文交给最后绑上的那一个），零停机换代里新一代也接不了已绑好的
  数据报口。现在 `QuicServer` 收一份 `Platform::DatagramSocket`（自己绑的或 `DatagramSocket::adopt` 交来的），
  端口从交来的那份套接字问回来、本端不再 bind。与 `UdpServer` 同一条纪律：**两种启动顺序不许混用**，
  接手来的调带地址的 `listen()` 会把那份套接字静默闲置，症状与「移交没做成」一模一样，故当场拒。
  接手来的套接字不合格也在构造期拒，且判据排在创建 `SSL_CTX` **之前**——构造期抛出后析构不会跑，
  而那份上下文一旦 `release()` 交接给成员就无人认领（实测把判据挪到那句交接之后，容器 LSan 报出
  `SSL_CTX_new` 那一块 1784 字节的直接泄漏）。
  对外行为由进程外的裁判验：`tests/Tools/QuicProbeServer.cpp` 新增 `--adopt-through-self-channel`，
  它把一份已绑好的数据报套接字沿 `Core::UpgradeChannel` 交回本进程再接手起服务，
  `scripts/quic_cross_check.sh` 的场景三让 aioquic 打同一套判据（握手 + 回显）过去。
  用例三条：接手来的服务端发布出的端口就是交过来那一个（而不是自己再绑的）；混用两种启动顺序两侧都被拒；
  空对象当接手来源被拒。四处证伪各自只红自己那条（把接手那条路改成本端再绑一个端口 → 端口那条红；
  两处拒绝改成放行 → 混用那条红；构造期判据摘掉 → 空对象那条红；把那条判据挪到上下文交接给成员之后 →
  容器 LSan 报泄漏）。
  顺手把两处文档与实现对齐：`Core::UpgradeChannel` 那两条「必须已经 listen() 过」「取回的是已在监听的描述符」
  在数据报走上这条通道后已不成立，改成按类型分别写清。测试侧把 `listen()` 异常取回的驱动协程上收进
  `CoreTestSupport`（`captureCoroutineFailure`），第二处用到它时不再复制一份。

- **一条 UDP 端口可以交给多个进程了（`Platform::DatagramSocket::adopt` + `Net::UdpServer` 的接手构造）**：
  跨进程移交那对函数（`Socket::writeListeningSocketHandoff` / `readListeningSocketHandoff`）的机制本身
  不限套接字类型——Windows 靠 `WSADuplicateSocketW` 换协议信息表、POSIX 靠 `SCM_RIGHTS` 送描述符——
  缺的是数据报这一侧的**接手动作**：交回来的是一枚裸描述符，而 `bindTo` 只会自己建新的。补上
  `DatagramSocket::adopt(int)`（判「是 SOCK_DGRAM」「已经 bind」、置非阻塞、取消随子进程继承），
  以及 `UdpServer` 的接手构造与不带地址的 `listen()`。为什么需要它：Windows 没有 `SO_REUSEPORT`，
  多个 worker 各自 bind 同一端口时内核把全部报文交给最后绑上的那一个，其余进程一个错都不报却永远
  收不到报文，所以那侧的多进程只能「一方绑、交给别人」（TCP 上刚这么做掉，这是同一形状的数据报版）。
  两种启动顺序**不许混用**：接手来的服务端再去 bind 一个端口，会把交过来的套接字静默闲置，症状与
  「移交没做成」一模一样，故当场拒。
  同时把移交头里的地址族与类型变成真闸门：头那两项是交出方从**同一个套接字**问出来的，接收侧过去
  从不过问（文档却写着「据此校验」，是假字段），现在对不上就报 EINVAL 并把重建出来的句柄关掉——
  交一枚「建得出却收不到东西」的句柄比失败难查得多。**线路格式一字未改**，换代是旧进程交给新进程，
  改格式会打断在跑的服务。
  用例七条：整条通道串起来后交过去的描述符在接手方仍收到报文（端口跟着过来）；`DatagramSocket::adopt`
  接管已绑定的口之后照常收发；接管那口的服务端对发往那个端口的报文有回话；伪造一份「头说流套接字、
  实际送数据报」的移交被拒（两条平台各造一次：Windows 的协议信息表伪造不出来，就搬一份真的只改头）；
  三种不合格的描述符都被拒且**拒时不关调用方的句柄**；混用两种启动顺序被拒；「没给处理器就别开端口」
  这条既有出口第一次被直测（`listen()` 是惰性协程，`EXPECT_THROW(server.listen(...))` 只构造了协程帧、
  什么都测不到）。

- **Windows 上的多进程 worker 现在能编排了（`WorkerSupervisor` 的移交模式）**：端口分摊在 POSIX 上靠
  `SO_REUSEPORT`，而 Windows 没有等价物——多个进程各自 bind 同一端口时内核把全部连接交给最后绑上的
  那一个，前面的进程一个错都不报却永远收不到连接，因此本类此前在 Windows 上构造即拒绝。现在换成
  master bind 一次、把那份监听引用逐个复制给 worker：`Configuration::handoff` 给出已经 `listen()` 的
  描述符与「等 worker 连上通道」的预算；每次（重）起都开一条一次性通道，把它的地址追加进 worker
  参数（参数名是公开常量 `Core::kHandedOverListenerArgument`），再按对方进程号交出监听引用；worker 侧
  用 `Core::adoptHandedOverListener()` 取回，然后按各服务器「接管已监听描述符」的构造入口起服务。
  崩一次补一次也就得重新移交一次——这是这套形状比 `SO_REUSEPORT` 多出来的那道账，也是预算必须存在
  的原因（见上一条）。**起来了却没能接手监听的进程不算在线**：收掉它并按一次「起来就崩」记数，
  否则那个名额被一个谁也不服务的进程占着。POSIX 一侧的形状不变，而把 `handoff` 填上会被构造拒掉——
  那边每个 worker 自己 bind，这份描述符没有使用方，留着一个「填了却不生效」的档位比拒绝更坏。
  用例：`tests/Tools/HandoffWorker.cpp` 是 worker 侧夹具（取回监听引用、接受连接、回一行文本），
  Windows 那条端到端用例真的起两个进程、问两次并核回话。判据落在**读到内容**而不是「connect 成功」
  上——回环上连接能建立只说明 backlog 收了这条请求，并不证明真有人在 accept，只判连接成功的用例
  会在「端口还听着而没人接手」时假绿。证伪四处：整段移交不发 → 端到端红（进程都报「在线」却没人
  应答）；通道地址不追加 → 端到端红；交付时目标进程号写错 → 端到端红；去掉 Windows 的构造期拒绝 →
  校验那条红。POSIX 的两条校验用例照常「给了就拒、留空就放行」。

- **交接通道现在能按期报「没人来连」（`Core::UpgradeChannel::waitForPeer(预算)` 与
  `Platform::Socket::waitForAcceptReady`）**：交棒方原来只有一种等法——无限阻塞的 accept。对端是
  **另一个进程**时那一句「阻塞调用」并不无害：worker 起来就崩、参数给错、还没走到连通道那一步，
  任何一种都会把调用方的编排循环冻在等待里，补位与收尾一起停。给出正数预算时，期限内没人连就报
  超时并把这条一次性通道作废（留着它只会有第三个进程挤进一次已经作废的交接）；不给预算仍是原来的
  无限等，零停机换代那条路一字不改。底层是 select 的有界等待，被信号打断时按**剩余**预算接着等——
  编排线程自己就挂着 SIGTERM/SIGINT 的处理函数，一次停止请求不能被读成「对端没来」。预算 0 是
  「只取当前状态」，负数是参数错误，当场拒而不是当成前两种读法之一。
  四条用例：已排队的连接要报得出、且紧接着的 accept 真拿得到它；没人连时在预算内报「没有」；
  带预算的等待不能把本来能成的交接改坏；超时后第二次等直接报「通道已收口」。证伪三处——让等待
  报不出 pending → 正面那两条红；去掉负预算的拒绝 → 参数那条红；超时后不作废通道 → 第二次等
  不再报「已经收口」而红。既有的六条交接用例在三处突变下全部照旧，说明这一层加的是分支而不是
  把原路径改道。

- **新增 `Net::UdpServer`：面向任意来源的数据报服务端**：绑定一条 UDP 端口，把每条报文连同来源
  交给处理器（`MessageHandler` 收 `Core::InetAddress` + 报文视图，交回的字节按来源发出，空字节
  即不作答）；`sendTo()` 允许不等对方先来（公告、通知这类主动下发）。这是能力面上最后一种没有
  对应形状的服务端——此前能对外监听的只有 TCP（h1 / h2 / WebSocket）与 QUIC（h3），无连接的数据报
  协议（发现、遥测、单请求单响应）在框架里表达不出来。
  四处「随手一写会留下难查行为」的地方逐条定死：**报文按到达次序串行处理**——并发派发要把每条报文
  复制一份（每条一次堆分配），而慢处理器配上无限扇出等于把内存交给对端，代价写进类注释的
  `@warning`；**零长报文照常交付**——唤醒、探测这类协议把「有没有发」当全部内容，把它当成一次
  失败的读数就等于让这类协议永远得不到应答；**超限应答整条拒发并计数**——数据报不会被内核切开，
  交出一半比一条也不交更坏；**处理器抛异常与读数报回的 ICMP 类错误都只丢这一条**，不带走收循环
  （与「一个消失的对端不能让整台服务不再接受任何来源」是同一条判据的两侧）。`stop()` 会关掉套接字
  而不是只翻标记：挂在「可读」上的协程不会因描述符关闭而醒。顺带把 `Core::InetAddress` →
  `Platform::SocketAddress` 的换算上收成 `InetAddress::platformAddress()`（QUIC 的监听与出站原本
  各抄一份 memcpy，两处现已改用这个出口）。
  十条用例直测整个公开面，对端是本线程上一条**真实**的 UDP 套接字而不是循环里的封装；证伪一次落
  九个突变（`stop()` 改成只翻标记、摘掉发送侧接异常、缓冲换成固定上限、去掉零长报文的交付、去掉
  一字节占位、处理器看到的来源换成写死的地址、纯函数丢掉两个条件、未监听时报成已发出、摘掉处理器
  接异常）：红的恰好是这九个各自对应的那一条，唯一没被任何突变触及的那条（构造期校验缓冲容量）
  照旧全绿。

- **HTTP/2 出站客户端可以逐批交付响应正文（`Http2ClientConnection::Http2ResponseBodyReceiver`）**：
  `request()` 与 `requestStreamed()` 各多一个可选的接收口，挂上之后这条流的 DATA 不再攒进
  `response.body`，而是按批次交给接收口，且**流控额度按交付进度归还**——一批没交完就不还窗口，
  对端因此被挡在本端宣告的窗口之外，本端缓冲的上界是一档接收窗口而不是正文总长。收口也与 HTTP/1.1
  不同：返回 false 只需 `RST_STREAM` 结掉这一条流（RFC 7540 §5.3.2），连接留着给别的流复用，
  不必像 h1 那样把整条连接关掉。顺带把「已收齐 / 已判死 / 已被本端 RST」那三处的窗口归还改成
  只还连接级：给一条已关闭的流发流级 `WINDOW_UPDATE`，§5.1 判的是 STREAM_CLOSED——本端的收口
  会变成对端眼里的协议错；但这些字节确实占过连接窗口，不还就是让整条连接的额度随每次收口漏下去。
  两条用例：真服务端那条钉「批数大于 1、拼接起来等于整份、只有一批带收尾标记」——8 KiB 的读块与
  65535 的连接窗口让多次交付成为必然，不靠调度运气；脚本对端那条钉**帧序**（RST 之前要有流级
  窗口更新、之后一条都不许有）。证伪三处：摘掉待交与交付 → 两条一起红（对端还等着一档窗口）；
  摘掉那一句 RST → 帧序红；把收口那一批的归还换回双流级 → RST 之后多出一条窗口更新而红。
  与 h3 那一支一起接进 `HttpClient`（见下面「三条通路都支持逐批交付」那一条）：共用一个开关，
  才不至于出现「h2 能逐批、h3 不能」这种按协议分叉的行为。

- **HTTP/3 出站客户端也能逐批交付响应正文（`Http3ClientConnection::Http3ResponseBodyReceiver`）**：
  与 h2 那一支同形的第三个入口。挂上接收口之后 `noteBodyBytes` 不再把正文攒进 `response.body`，
  而是留在该条流的待交缓冲里，交付排在每一轮推动通路之前，**接收额度按交付进度归还**——一批没交完
  就不抬 `MAX_STREAM_DATA`，本端缓冲的上界因此是一档接收窗口而不是正文总长。返回 false 就
  RESET_STREAM + STOP_SENDING 结掉这一条流，链路留着给别的请求用。正文上限改按**累计**收到的字节判
  （只量缓冲的话，交付会不断腾空缓冲，这道闸就只剩「一次能堆多大」）。另补一条口径：协议层报来的
  字节若属于一条本端已经不认的流（请求协程收口之后仍有晚到的），只还额度、**不重立账**——立了就没有
  人来摘它，`inFlightStreamCount()` 再也回不到 0，链路会一直看着被人占用，出站池据此做的复用与收口
  判断跟着失真。两条用例：一条钉「批数 > 1、拼接起来逐字节等于对端答出的那三百 KiB、只有一批带收尾
  标记」；一条钉「交够指定的批数就停、下一条请求在同一条链路上仍答得出来、在途数回得到 0」。
  已知未覆盖：本端结流那一句的效果只在对端看得见（自家服务端这几条路由不阻塞），摘与不摘这一层看不出
  差别，那套动作本身在传输层已有直测（`TestQuicStreamLayer` 的 STOP_SENDING 一组）。

- **出站客户端接上 HTTP/3：`HttpClient::setHttp3Enabled(bool)`（默认关）**。协议层与连接池那两半
  （`Http3OutboundLink`、池里的 `acquireHttp3`/`adoptHttp3`）此前已就位，缺的正是选路这一环。打开之后
  每条 https 出站先试 h3：池里有待命链路就直接复用，没有就探一条（解析地址 → QUIC 握手 → h3 起步），
  探不通、起步没走完、或这条流一个字节都没答过，都**回落 TCP**（h2 还是 h1 由 ALPN 定），调用方拿到的
  响应与协议无关。开关不做 Alt-Svc 缓存：调用方要的是「这个 URL 会走哪条通路」的确定性。
  四条刻意的边界：①明文 `http://` 不走 h3（QUIC 的 TLS 是强制的）；②带 `bodySource` 的流式上传不走
  h3——h3 的出站入口只收整份正文、没有流式出口，判据写坏的后果不是报错而是**静默少发正文**（整块那条
  交的是空串），所以这一条单独钉了用例；③静态的 `get()/post()/send()` 那一支没有承载开关与 TLS 策略
  的地方，恒走 TCP；④**一个端点在一个客户端的存活期内只探一次**——UDP 被中间网络黑洞时「连不上」没有
  快速回音，少了这道记账，一批并发请求会各付一次探测时限（它们还被同一端点的建连资格串在彼此后面）。
  探测时限单独设上界（3 秒）而不是把整条请求的预算交给它：到点就交回 false，剩下的预算仍归那次真正的
  TCP 请求用，这才是「失败回落」这条承诺能成立的地方。响应正文的上限从池的 `Config` 填进 h3 那侧的
  `Config::maximumResponseBodyBytes`，三条通路吃同一份胃口（接错或没接都会让「配小上限仍能收大响应」
  当场报红）。观测面补两条：`idleHttp3LinkCount()` 与 `http3MaximumInFlightStreamCount()`，
  与 h2 那一对同形——一台主机一条链路，所以「链路一条 + 复用度二」才是复用的直接证据。
  端到端判据的手法：摆三种对端形态各照一种情形——只有 UDP 在听（h3 走得通）、只有 TCP 在听（只能靠
  回落）、UDP 端口被一个永不应答的套接字占住（探测只能等满时限）。「这条是谁答的」直接从响应体读出来，
  不必给客户端开观测口；三个形态各自只绑一个内核挑的端口，不让 TCP 与 UDP 去共用同一个号——那需要
  两次 bind 之间没人把这个号拿走，在并行跑的全量门禁里是一处真实竞态（实测确实被同批用例占走过）。

- **三条出站通路都支持逐批交付：带 `responseBodyReceiver` 的请求照样协商 h2、照样复用 h3**。
  此前这样的请求被一道选路的门锁在 HTTP/1.1 上（那道门当时的理由成立：h2 与 h3 还没做流控侧的交付）。
  现在门拆了，`establishSecureConnection()` 不再有两份 ALPN 名单（少一份没人消费的开关），池里的 h2
  待命连接与 h3 链路照常被这类请求取用；交付口径逐条对齐 h1：批次拼起来等于整份正文、交回的
  `HttpClientResponse::body` 为空、只有走到收尾那一批才带 `isLastBatch`。两条端到端用例分别打在
  「ALPN 协商出 h2 的 HTTPS 服务端」与「只听 UDP 的对端」上——**走没走对那条通路不信内部计数**：
  h2 的答没有原因短语（退回 h1 必带 "OK"），h3 那台对端根本没有 TCP 监听（退回就是连不上）。
  收口形状的两处差别各自钉住：h1 那条断言「不还池」（池里空闲数为 0），h2 与 h3 那两条断言
  「连接/链路还在池里且还在途的流数回到 0」。证伪：把 ALPN 改回只声明 `http/1.1` → h2 两条全红；
  让 h2 那一支悄悄忽略接收口 → 批数为 0、整份正文留在响应里，两处一起红。

- **IPv6 的「%作用域」写法在 `Core::InetAddress` 上双向打通**（RFC 4007 §11）。此前 `fe80::1%eth0`
  这类文本一律按「非法 IP」拒收，`sockaddr_in6::sin6_scope_id` 全仓零写入点——于是这台引擎既不能绑定
  某块网卡的链路本地地址，也连不上它：链路本地地址不带作用域时无从确定走哪张网卡，而同一段文本在
  内核与在文本面是两回事。现在 ①`InetAddress(port, "fe80::1%3")` 与 `"fe80::1%eth0"` 都能构造，
  接口名在本机换算成数字索引后写进那个字段；②新增 `scopeId()` 读回它（IPv4 与未带作用域时都是 0）；
  ③新增 `parseLiteral(文本, 端口)`——不抛异常、不查 DNS 的字面量判据，构造函数与它同一份规则，
  不再有「换个入口就换个口径」。作用域号不校验「本机是否真有这个接口号」：校验要枚举全部网卡，
  而绑定与连接本来就会以 `EADDRNOTAVAIL`/`ENODEV` 明确报出来，多一次系统调用换不来更多信息。
  接口名查不到则当场拒绝，文案点名是哪一段不可用——配置文件里写错一块网卡的名字，若悄悄退化成
  「无作用域」，失败就推到一条看不出原因的超时上。OS 调用只落在新增的 `Platform::interfaceIndexOfName()`
  （Windows 侧由 `iphlpapi` 实现；`netioapi.h` 只能经 `iphlpapi.h` 带进来——它按 `__IPHLPAPI_H__`
  有没有定义来分辨用户态与内核态分支，单独包含会走内核态那一路而整页编不过）。

- **多区间 Range 现在回 `206` + `multipart/byteranges`（RFC 9110 §14.7）**。此前一条 Range 里出现逗号
  就整条忽略、回 200 全量——那是规范允许的，但对下载器与图片预览器意味着「为了两段字节重取整个文件」。
  四条判据都钉了用例：
  ①**先合并重叠与紧邻的区间**（§14.2 明给的自由）：`bytes=0-4,3-9` 与 `bytes=0-4,5-9` 都合成一段，
  回到单区间那个形状（顶层 `Content-Range`、不分包），同一份字节不会在包里出现两次；
  ②**只有一段越界时丢掉那一段**，其余照发；全部越界才是 416；
  ③**任何一段写法不合法就整条 Range 当非法**（回 200 全量）——挑「能解的那几段」发出去等于替对端
  猜它原本想要什么；
  ④**两道上限**：合并后段数 > 8、或多段总字节 > 8 MiB 时整条 Range 当没看见。一条请求不该换走
  数倍正文的内存。
  分包正文是拼出来的，零拷贝在这里换不成（每段读完拷进那一块缓冲，代价由上限封顶）。分隔符**固定**
  而不是随机：同一请求两次应答逐字一致，日志与用例才钉得住；正文里撞上它时改判回全量，不换边界重试
  （换边界等于同一条请求给出两份不同报文）。HEAD 的多区间请求只报「GET 会发多大」，长度由分包结构
  算出来、一个正文字节都不读——与真正写进正文的那份只有一处来源。

- **`HttpResponse::sendInformational()`：处理函数能在最终响应之前先写一条 1xx**。此前库内唯一的中间响应
  是会话自己回的那条 `100 Continue`，而且是一条硬编码字面量（`"HTTP/1.1 100 Continue\r\n\r\n"`），
  谁想发 `103 Early Hints`（RFC 8297：最终响应还在生成时就把 `Link: rel=preload` 交出去，浏览器据此
  提前取 CSS/字体）或 `102 Processing` 都没有出口——把状态码设成 103 只会得到一份「以 103 为最终答复」
  的报文，对端等不到后面的答复。现在三条通道同一个入口：h1 是一条独立的状态行报文，h2/h3 是
  **不带 END_STREAM 的 HEADERS 块**（RFC 9113 §8.1、RFC 9114 §5.3.2），处理函数换个协议不必改写法。
  装配方式与流式发送口同形（`setInformationalWriter()` 由会话在路由之前装，`HttpResponse` 不认识 socket），
  没装配时 `sendInformational()` 交回 false 而不是抛；状态码不在 1xx、字段写法会撕裂这条报文时当场抛
  （那是处理器自己的 bug，不该悄悄少发一条中间响应）。中间响应**不碰**本响应的状态码与头部：
  它的字段不会出现在最终答复里，最终答复也照常由本对象给出。
  顺带两件收口：会话自己回的那条 100 改走同一个出口（h1 的线上字节逐字不变，用例钉住），
  而 `status1xxCount` 从此真的有数——此前它在三条通道上都恒为 0，是一份假指标。中间响应只落状态码类
  计数，不落延迟样本：它不是「这条请求的答复」，拿它的时间进直方图会把图写脏。
  端到端证据是三份同名用例（h1/h2/h3 各一份），断言「先到 103、再到 200、Link 只在第一段、正文照旧完整」。

- **`staticDirectoryListing(bool)`：静态目录的 HTML 列表（默认关闭）**，`HttpServer`/`HttpsServer`/
  `QuicServer` 三条通道同一个开关、同一份实现（本体在 `StaticFileService`，列表生成挂在静态请求那
  一条漏斗上）。默认关闭是刻意的：列表会把目录结构、文件名与大小交给任何一句 `GET /assets/` 的探测者。
  打开之后四条口径：
  ①列表**不带验证器**且带 `cache-control: no-store`——被缓存下来的列表会在目录变化之后继续宣称
  「这里有这些文件」，而生成型正文没有可信的 ETag/Last-Modified 来源；
  ②**忽略 Range**并回 `accept-ranges: none`：列表不是长度稳定的表示，给它切片等于承诺一个不存在的长度；
  ③**条目链接一律绝对路径**（`/sub/inner.txt` 而不是 `inner.txt`）：同一棵目录可能挂在 `/sub` 也可能
  挂在 `/sub/` 下，相对链接在前者上会全部指到父目录，点了打不开而列表看着完全正常；
  ④**条目数有上界**（1000），到界就在末尾写明「还有 N 项未列出」，不做半截列表装作是全的。
  名字里的 `&` 等按 HTML 转义、空格与 `%`、`#` 按 URL 编码（`%` 必须先行编码，否则名字里本来就有的
  `%41` 会被解成字母 A，链接指到另一个条目上去）。HEAD 走同一条生成路径，`content-length` 与 GET 实测一致。

- **`TlsContext::loadCertificateForHost()` 与 `HttpsServer::loadCertificateForHost()`：按 SNI 分站点证书**
  （RFC 6066 §3）。与上面的 `Router::virtualHost()` 是配套的两件事：证书按 ClientHello 的 server_name 选、
  路由按 Host 选，两处的键都是同一个小写主机名，因此「这个域名用这张证书」与「这个域名走这组路由」
  不会各配各的。三条刻意的取舍：
  ①**不做通配键**——通配身份本来就该由证书自己的 SAN 负责（一张 `*.example.com` 的证书覆盖全部子域，
  再拿 `*.example.com` 当登记键只会多出一条永不需要的匹配规则），含 `*`、冒号、内部空白的名字当场拒绝；
  ②**没带 SNI 或名字没登记过 → 默认证书**，与 nginx 的 default server 同形，老客户端与直连 IP 的探活照旧连得上；
  ③**OCSP 装订只对默认证书生效**——响应是按上下文挂的，给每个站点配一份响应需要一套「哪张证书配哪份响应」
  的记账，本版没做；登记的站点因此不装订（握手照常），别指望默认证书那份响应被送到别的站点的证书上，
  那是错的凭据。
  登记表存在 SSL_CTX 的 ex_data 里而不是本类成员（与 OCSP 装订数据同一条理由：回调在握手线程上跑，
  解引用一个可能已析构的 TlsContext 会悬垂）；站点上下文按 `shared_ptr` 持有，引用链是
  「SSL→主上下文→登记表→站点上下文」三段。`reloadCertificate()` 连登记的站点一起按原路径重读重建：
  漏掉这一步的表现是「续期成功了，带 SNI 的站点却拿到一张名字不匹配的证书」，比续期失败难查得多。

- **`Router::virtualHost()`：一台服务器按 Host 分站点**。此前所有 Host 都落到同一张路由表上，
  「同一个端口服务多个域名」这件事只能靠使用方自己写中间件扒 Host、再手工转发，而转发之后
  中间件顺序、404/405 判定、流式派发全都得自己复刻。规则三条，都写进用例：
  ①精确主机名优先于通配名，通配名之间取后缀最长者，与注册先后无关；`*.example.com` 收子域但
  **不收** `example.com` 本身；比对键去端口、折小写、去结尾根点，`API.Example.COM:443`、
  `api.example.com.` 与 `api.example.com` 是同一个站点（IPv6 字面量按方括号切，不把括号里的冒号当端口）；
  ②没登记虚拟主机、或请求没带 Host（HTTP/1.0 式请求、直连 IP 的探活）、或 Host 落不进任何站点时，
  一律走根表——单站点行为原样保留，**且一条请求不多付一次主机归一化**（实测派发分配数不变，
  只有协程帧大 16 字节）；
  ③命中的主机**只**走自己的表：根表的路由（含 `staticFileDir()` 那条 `any("*")`）不参与也不兜底，
  主机上没有这条路径就是 404。但根路由的中间件对所有主机都生效（它跑在最外层），各主机自己的
  中间件只对本主机生效（在内层）——鉴权、CORS、访问日志登记在根路由上，加个主机就被绕过是
  安全事故的形状，这一条有端到端用例钉住。命中主机的 404 不再复位响应：外层中间件刚写下的
  CORS 头不能被里层抹掉，而「上一条报文的残留」由会话每轮派发前的复位保证。
  选站只做一层：在主机表上再 `virtualHost()` 当场报用法错误（两层的中间件嵌套语义推不出来）。
  `hasVirtualHosts()` 供观测；`*.example.com` 之外的正则、路径前缀式匹配有意不做——那需要引入
  一套与路径匹配并列的仲裁规则，而两个域名谁更具体这种事，规范里没有一个使用方能一致同意的答案。

- **`HttpRequest::multipartForm()`：上传文件在框架里终于能表达了**。新增 `MultipartFormData`/`MultipartPart`
  （RFC 2046 §5.1 的分隔符规则 + RFC 7578 的表单约定），此前 `formFields()` 按 `&` 切正文，multipart 正文
  只会切出一堆垃圾，「上传一个文件」这件事没有 API。段正文是**指向请求正文的视图**，不另拷一份：
  8 MiB 的上传再拷一遍就是每个大请求一倍的额外内存。三处刻意的取舍：
  ①**行尾只认 CRLF、边界必须落在行首**（正文里出现的 `--边界` 文本只要不在行首就照常算内容）；
  ②**要猜的写法一律整份判失败**而不是将就——`filename*`/`name*`（RFC 2231 的字符集与分片）、
  `Content-Transfer-Encoding: base64`/`quoted-printable`（还原就要新造字节缓冲，与零拷贝口径冲突）、
  缺 `name` 的段、缺结束分隔符、同段重复的 Content-Disposition/Content-Type。宁可调用方看见失败，
  也不把没还原的编码文本当原始字节交出去；
  ③**`filename` 先展开引号转义再剥掉目录部分**，并拒绝控制字符与收成 `.`/`..` 的名字——老 Windows
  客户端会把整条路径写进来，使用方一拼接就穿越到目录外（落盘位置仍须使用方自己决定）。
  媒体类型判定 `contentTypeIs()` 从 `HttpRequest.cpp` 上收到 `HttpHeaderRules`，与 `formFields()`/`jsonBody()`
  同源：两侧对同一份头部给出不同结论时，「服务端认的格式」和「解析器认的格式」会分家。

- **`HttpClient` 默认带一个出站熔断器（行为变化）**：同一端点连续 5 次失败（传输层失败或 5xx）即开闸
  30 秒，开闸期间对新请求直接给出失败原因，不再付 DNS、TCP 与 TLS 握手的钱。原先上游整个塌掉时
  这笔钱每个请求都要重付一遍，而调用方看到的只是「超时」。判据用「连续失败」而不是「失败率」：
  连续口径不需要采样窗口，也不会让一个刚上线、请求量还小的端点被一次失败按比例判成不健康。
  4xx 不算失败（那是使用方的请求有问题，记到上游头上会让一次错误的调用把所有人挡在门外）；
  半开时一次只放一条探测。端点账是有界的（默认 256 个端点，超出按最久未更新淘汰）。
  不想要这层保护可以 `setCircuitBreaker(nullptr)` 关掉；换阈值用 `OutboundCircuitBreaker::Configuration`。
  另补 `setCircuitBreaker()`/`circuitBreaker()` 与可观测读数 `trackedEndpointCount()`/`openEndpointCount()`。

- **Base64 与摘要工具从各自的使用点收上来**：新增 `Base::base64Encode/base64Decode`（RFC 4648 标准字母表，
  解码严格到「填充位必须为 0」——放过非规范编码等于允许同一份凭据有多种「合法」写法，比较时就会判成不相等）
  与 `Core::Digest`（`sha1`/`sha256`/`hmacSha256` 及十六进制输出）。WebSocket 握手原先自带一份 base64 与
  一段 EVP 样板，两处都改成调用这两个入口；摘要工具放在 Core 而不是 Base，因为 Base 不链接 OpenSSL。
  HMAC 走 `EVP_MAC_fetch` 的显式流程，不用 `HMAC()`/`EVP_Q_mac` 一次性接口：前者 3.x 起标弃用，
  后者的参数形状在 3.x 各小版本之间漂过（本机这份第 5 参已是 `OSSL_PARAM*`）。
  摘要向量类用例都另拿一份独立实现（Python `hmac`/`hashlib`）对过——HMAC-SHA-1 与 HMAC-SHA-256 对同一组
  key/data 的前 8 个字符都是 `f7bc83f4`，靠记忆写期望值只会误导后来人。

- **JSON 有了对外的解析与序列化入口，HTTP 两侧都有对应工具**：`Base::parseConfigValue()` 把语法错误
  翻成空 optional 而不是抛异常（外部输入不该靠异常否定一次调用），注释是显式开关（配置文件那条路开，
  HTTP 正文按 RFC 8259 不开）；`Base::serializeConfigValue()` 在 dump 之前先查非有限浮点——nlohmann 缺省
  把 NaN 与 ±Inf **悄悄换成 null**，与本仓「宁可失败也不静默变形」的口径相反（`configValueAs()` 同一条线）。
  对象键按字典序输出（映射是有序 `std::map`），插入顺序不保留，这一点写进用例而不是等人踩。
  HTTP 侧新增 `HttpResponse::setJsonBody()`（一次把正文与媒体类型都设好，无法表示时返回 false 且
  一个字都不改）与 `HttpRequest::jsonBody()`（媒体类型不是 application/json 就交回空，不去猜）。

- **`Core::GracefulShutdown`：停机信号的接管进了库**。此前每个使用方都要自己写一遍
  `std::signal` + 一个全局标志，抄错的后果是「Ctrl+C 之后连接被硬切、在途请求随进程一起丢」，
  而且没人会知道自己抄错了。POSIX 接管 SIGINT/SIGTERM（屏蔽字 + 一个带超时的 `sigtimedwait` 轮询线程），
  Windows 接管控制台的四类关闭事件；收尾动作一律投回构造时给的事件循环执行——它们通常要碰服务器与会话
  状态，而那些状态按本库的线程契约只允许在所属循环上读写。三条刻意的取舍：触发是单向的（连按两次
  Ctrl+C 是催命，第二次交还给系统强杀）；单个动作抛异常就地接住并记 ERROR，后面的照跑；收尾已开始才
  注册的动作按「迟到注册」补投一次而不是静默丢掉。关闭/注销/关机这三类事件里处理器一返回系统就终止进程，
  因此 `requestShutdown(ConsoleEvent)` 原地等收尾跑完（上限 5 秒，超时也要放手）。每进程只装一个观察者，
  第二个如实报 `isInstalled()` 为 false。
  **要在进程内模拟一次真信号请用 `kill(getpid(), SIGTERM)` 而不是 `raise()`**：后者送的是线程定向的
  挂起信号，本类的等待线程取不到（这条区别是容器门禁实测出来的）。
- **`HttpRequest::formFields()`：表单正文有了取值入口**，与 `queryParams()` 共用**同一份**解析
  （同一套分对规则、同一个百分号解码、同一个 `+` 当空格的约定），所以同一段文本从查询串取和从正文取
  必然给出同一张表——写两份实现迟早在这几个细节上漂移。媒体类型不是
  `application/x-www-form-urlencoded` 时交回空表而不是猜：把 JSON 或二进制正文按 `&` 切开拼成
  「参数」等于凭空造出调用方从未提交过的数据。类型参数（`; charset=utf-8`）不参与判定，媒体类型
  本身大小写不敏感。
- **Cookie 有了结构化表示，客户端也有了自己的 Cookie 罐**：`HttpCookie` 负责一条 Cookie 的名字、取值
  与属性（Path/Domain/Expires/Max-Age/Secure/HttpOnly/SameSite），`HttpRequest::cookies()` 读、
  `HttpResponse::setCookie()` 写。以前这两头都得手写 `name=v; Path=/; HttpOnly` 这样的字符串，
  最容易漏的正是 Secure/HttpOnly 几个字，而把 Expires 写成对端不认的日期格式在响应里是**静默**的。
  属性只有显式设过才输出——「没设 Path 就补 Path=/」会静默改变作用域，而作用域是 Cookie 最容易出事的地方。
  新增 `HttpCookieJar`：按 RFC 6265 收发放，域属性必须罩得住请求主机、IP 字面量不收 Domain 属性、
  Secure 的 Cookie 在明文连接上收侧就丢、缺省路径按 §5.1.4 的 removal 规则推、Max-Age 非正数即删除、
  同名同域同路径即替换；存储有界（单域上限先挤本站最旧的，总量上限兜住跨域），因为一个愿意一直回
  Set-Cookie 的对端本可以把客户端内存吃光。`HttpClient::setCookieJar()` 挂上即自动收发，
  调用方自己写的 cookie 头以他为准。
- **实例版本的 `HttpClient::send(url, request, timeout)`**：此前实例只有 `get`/`post` 两个便利入口，
  要发带自定义头部的请求只能退回静态 `send()`，而那条不带池——连接复用与刚挂上的 Cookie 罐一起丢掉。
- **配置文件之上多了一层环境变量**：`ConfigManager::setEnvironmentOverridePrefix("ASYN_")` 打开后，
  `ASYN_SERVER__PORT=9090` 覆盖配置键 `server.port`（`__` 是层级分隔符，段内单下划线保留，字母按 ASCII
  转小写）。容器部署从此不必为改一个端口去挂配置文件。叠加只贴在 `commitConfigData` 这一个出口，
  所以首次加载、reload 与热重载共用同一份口径，不会出现「改一次文件就把部署侧的值悄悄换回去」。
  类型取自文件里已有的值，其次取 schema 声明，两处都没有时按字符串交出**不猜**；对象与数组按 JSON 解析，
  布尔接受 1/0、true/false、yes/no、on/off。值转换失败判整批不应用（半份覆盖比没有覆盖更难查），
  名字畸形映射不出键的那条只忽略自己并出声。整表枚举归位在 `Platform::ProcessInfo`。
  快照带 `environmentOverrideCount()`，运维入口与用例都读得到这轮生效了几条。
- **`/metrics` 多出两族运行期积压读数**：`blocking_task_queue_depth`（gauge）与
  `blocking_task_rejected_total`（counter）。`AsyncExecutor::pendingTaskCount()` 此前只有定义处一个读者，
  而队列开始变长恰恰是「事件循环被拖住」最早的信号——在此之前它在观测面上完全看不见。
  读数并入 `HttpMetricsCollector::snapshot()` 而不是三条通道各自的 `stats()`：明文 HTTP、HTTPS/HTTP2 与
  QUIC 都从这一个漏斗取快照，收在一处就不存在「哪条通道忘了接」。执行器另加一个只在饱和拒绝分支上自增的
  计数（停机期的拒绝不计，那只会让告警在进程退出时自己响一次）。
- **HTTP 日期解析认得两种过时格式了**：RFC 9110 §5.6.7 要求收端兼容 RFC 850
  （`Sunday, 06-Nov-94 08:49:37 GMT`）与 asctime（`Sun Nov  6 08:49:37 1994`），此前只认 29 字节的
  IMF-fixdate，这类 `If-Modified-Since` 会被判「解析不出来」。后果是安全的（按条件不命中回完整表示，
  不会误判成「没变」），但老客户端的条件请求整条白给。两位年份按固定规则折叠（0..69 记 2000 年代、
  70..99 记 1900 年代），不按「离现在最近的世纪」解释——那样同一条头会在某个时刻之后解析出另一个世纪，
  缓存验证器就不复现了。
- **仓库补上了 LICENSE、SECURITY.md 与 Issue 模板**：README 一直自称 MIT 而仓库根没有许可证文件
  （vcpkg 公共注册表因此带不上 copyright）；自研 QUIC/QPACK/HPACK 这些远程可达的解析器此前也没有
  对外报告漏洞的通道。

- **`QuicServer` 也能在启动前换限额了（`setLimits`/`setParserLimits`，与两条 TCP 监听器同形）**：
  此前 h3 的限额只能在构造 `QuicServer` 那一刻由 `Configuration` 定死，而 `HttpServer`/`HttpsServer`
  都有 `setLimits(HttpServerLimits)` 与 `setParserLimits(HttpParserLimits)`——同一份配置喂三条通道时，
  h3 就是那条改不动的。现在 setter 与两条 TCP 侧同语义：**必须 `listen()` 之前调用**，且是
  **整体换代**而不是逐字段合并（限额按 `shared_ptr` 只读交给此后每条连接新建的会话，就地改会让在途
  会话读到半新半旧的组合；解析上限按值存，它只在会话构造那一刻被取走一份副本）。
  顺带把限额的读源收成一处：静态目录的映射条数上限、会话的解析上限与连接级限额此前各自去读
  `Configuration`，现在都读同一份生效值——留着两条读源就是「setter 改了一处、另一处仍旧」的成因。
  用例两条：getter/默认值那条钉「默认读的是 `Configuration` 给的那份」与「换代不留旧字段」；
  端到端那条钉真正的接线——服务端 `listen()` 之前把单连接请求数上限换成 1，第一条请求正常应答、
  第二条落在「本端已不再受理」那一侧。证伪过：把会话那侧的读源改回 `Configuration`，端到端那条
  立刻红（第二条也被服务了），而只看 getter 的那条照绿——这也是为什么两条都要留。

- **出站连接池开始管 HTTP/3 链路（`Http3OutboundLink` + `acquireHttp3`/`adoptHttp3`）**：h3 的会话
  （`Http3ClientConnection`）只**借用**它的 QUIC 连接（`QuicClientConnection`）而不拥有它，所以要把一条
  可复用的 h3 连接交给出站池与多个在途请求共同持有，就得有个把两层绑进同一生命期的东西——分开持有会
  出现「会话还在人手里、底下的 QUIC 连接先被释放」那种踩空。新类型就是这一对：成对构造、成对释放，
  成员声明顺序即销毁顺序的依据（会话必须比它的 QUIC 连接先销毁）。
  池侧与 HTTP/2 那一对同形：一台主机只留一条（第二条链路换不来吞吐，只多占一个 UDP 端口与一份 QPACK
  状态）、取用时判一次健康且**判死即从表里抹掉**（QPACK 动态表与拥塞状态随连接一起作废，不留缓存）、
  不摘走（复用发生在流上）、`closeAll()` 只收空闲的那部分（在途请求还握着引用，掐它等于掐别人的请求）。
  **这一格还只是通路**：`HttpClient` 的选路（显式开关 + 失败回落 TCP）是下一步，所以 `HttpClient`
  本身今天仍只走 h1/h2。
  用例 `Http3OutboundLink.IsPooledAndEvictedLikeTheHttp2Side` 在链路所属的循环线程上跑完整段（池的
  取用/判死/计数不能跨线程读），判四件事：取回的就是放进去那条、第二条链路不占位置**且顶不掉第一条**
  （只数条数看不出覆盖——这一格是靠证伪才发现要补的）、请求收完之后在途归零、关掉之后取既拿到空也
  把表清干净。

- **格式门禁进流水线**：`linux-ci.yml` 多一条 `formatting` 作业，用 pip 上钉死的
  `clang-format==23.1.1`（与本机同一版）对 `git ls-files` 里的全部 C++ 源文件跑
  `--dry-run -Werror`，有差异即失败。版本必须钉住：换一版折行结果就变，红海会与改动无关，那种红
  只会教人忽略红。立门禁前实测过基线：605 个被跟踪的源文件零违规。顺带补上当初全量格式化漏掉的
  三个文件——它们不是「没人改」，而是**长 trailing 注释落在对齐组里，clang-format 一次跑不收敛**，
  得把说明挪到声明上方才写得下（那三处已按此改）。

- **`echo_server --static <目录>`：静态目录一条开关喂三条通道**，h3 上的静态正文从此也有进程外裁判。
  此前 `echo_server` 完全没有静态目录入口（只有 `net_http_demo` 自己建临时目录跑一遍），而
  `scripts/h3_cross_check.sh` 的八条场景也没有一条走静态通路——「静态文件上到 HTTP/3」那次只带了
  会话层直测，也就是拿「测试自己按 RFC 排字节的对端」当证据，看不见跨实现才看得见的事实。现在目录由
  `--static` 给出并同时登记到 h1/h2 与 h3 三个监听器上（与 `--metrics`、`--compress` 同一口径：不给
  某一条通道留后门）；目录不存在时在**碰网络之前**就退出并点名这一项，而不是等某一侧的构造抛出
  「某个监听器起不来」这种要用倒推才读懂的报错。
  跨实现侧补三条场景：静态 GET（正文逐字节、`content-type` 按扩展名推出、`content-length` 与**文件
  实际字节数**同源、`etag` 与 `last-modified` 都在场）、同一条资源的 HEAD（要给出 GET 会发的那份头部，
  线上一个正文字节都不许有）、目录里没有的名字（兜底路由让位给 404，而不是回一份空正文当作命中）。
  长度取自 `wc -c` 而不是写死的字面串——这样「HEAD 与 GET 的 length 同源」才是独立实现在判，而不是
  本端自证。容器实测 11 条场景全过。

- **HTTP/3 与 QUIC 支持双向 TLS（mTLS）**：此前 h3 这条通路两端都缺——服务端从不向对端要证书，
  策略里那几项（`certificateAuthorityFile/Path`、`verifyDepth`、`revocationListFile`）装进信任库也没人
  去查；出站 QUIC/h3 客户端也没有出示自己身份的入口。现在：
  `QuicServer::Configuration::requireClientCertificates` 打开后，本服务端按
  `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` 要求并校验客户端证书，吊销检查也随之从
  「配了但不生效」变成真的参与客户端链；`QuicClientConnection::Configuration::clientCertificateFile`
  与 `clientPrivateKeyFile` 成对给出即在本端出示身份（h3 出站会话走同一份配置，因此
  `Http3ClientConnection` 一并可用）。
  两处配置错误都在**构造期**拒绝而不是留到握手里：要求校验却没给任何信任锚（这种配置在 TLS 里的形态
  不是「退化成不校验」而是**每条握手都失败**，与 `HttpsServer::setClientCertificateRequired()` 同一判据）、
  以及证书与私钥只给一项（只给一项的后果要到握手深处才暴露，失败点离成因隔了一整条握手）。
  **一条容易踩错的判据平面**：TLS 1.3 里客户端收完服务端的 `Finished` 就自认握手完成，它对「服务端
  后来有没有接受我的 `Certificate`」没有任何可见性（服务端拒绝只能靠加密后的 alert 传达）。所以
  「被拒的客户端 `connect()` 返回 false」这种断言在实现完全正确时也会红。这里的用例把拒绝判在
  **服务端平面**：被要求证书却不出示（或出示一张不在信任锚里的）客户端，拿不到任何响应，且服务端的
  请求计数一条也不涨。实测还顺手量掉一个担心：这种被拒的连接**不是**攥在服务端手里等空闲超时——
  本端在几十毫秒内就看到它被收掉（用例把「两秒内收场」钉成判据，请求预算本身是 4 秒）。
  已知边界：`Core::TlsPolicy` 的那三项在 h3 上从此**只有在 `requireClientCertificates` 开着时**才参与
  校验（不开时本服务端连证书都不要），`QuicServer.h` 里原来那句「对本监听器不适用」已随之更正。
  用例 5 条：h3 层「带受信任证书→四个判据全成立（状态码/正文/响应头/服务端计数）」「不给证书→拿不到答
  且服务端计数为 0」「给一张不在信任锚里的证书→同样拿不到答」，QUIC 层「客户端出示受信任证书能握手并
  谈定 h3」「证书与私钥只给一项在构造期拒」，服务端「要求校验却没有信任锚在构造期抛且点名那一项」。
  证伪：把服务端那一刀 `SSL_CTX_set_verify` 摘掉（条件改成不可达）→「不给证书」那条立刻红在
  「拿到了响应 + 计数 1」两处，正例与其余各条照绿。
  门禁（同一份内容）：Windows Debug（MSVC `/W4 /WX` 加 ASan）全目标零告警、ctest 3215/3215；容器 GCC 13
  Debug 加 ASan/LSan/UBSan 全目标零告警、ctest 3224/3224（真库用例按环境 SKIP）、零 sanitizer 命中。

- **出站 HTTP/3 客户端（`Net::Http3ClientConnection`）**：h3 从此两个方向都走得通——此前只有服务端一侧
  （`QuicServer`/`Http3Session`），要拿本框架当 h3 客户端访问别人的服务就只能借外部实现。位置与 h2 侧的
  `Http2ClientConnection` 对应：在一条已握手的 `QuicClientConnection` 上开出三条本端单向流并把 SETTINGS
  送上线（`start()`），之后 `request()` 一问一答、每条请求占一条本端双向流，同一条连接上可以并发提多条；
  `shutdown()` 先发 GOAWAY 再收连接（顺序与 h2 同理：GOAWAY 要让对端看见才有意义）。结论结构体
  `Http3ClientResponse` 的字段形状与 `Http2ClientResponse` 逐字对齐，为的是调用方在两协议之间不写两套判断
  ——两份没有合成一份，因为那是 `Http2` 模块的公开类型，要合得先决定上收到哪一层。本类不自带后台协程：
  等响应时由 `request()` 自己一圈圈推（送已排好的字节 → 收一条报文），谁先收齐谁先返回。
  **一处要说清的口径**：`request()` 的时限到点是**收掉整条连接**而不是只弃这条流——本层不替调用方揣测
  「同一条连接上别的请求还要不要」。复用一条连接时，超时那次会连带让其它在途请求拿不到答案，调用方按
  「对端在我们手里把连接收了」那一支重来即可（判据是响应里的 `isAnyByteReceived`）。
  为了让这条路真能走通，`Http3Connection` 侧补了两处：**角色化**（流号归属、收侧与写侧允许的消息种类三处
  判据按两型分开）、以及出站侧要能把流数据直接转交给协议层（`QuicClientConnection` 的 sink——中间再排一次队
  会把同一批字节存两遍，而且看不见 FIN）。本端自己开的请求流必须**先登记再提交**，否则第一帧回来时无人认领。
  同时修掉一条远端可触发的判读缺陷：QPACK 的头段可能先到、它引用的动态表插入指令排在**之后**才到
  （控制流与编码器流之间传输层不保证先后，RFC 9204 §2.2.1 就是让解码器等）。原先按 END_STREAM 就地收尾会让
  `onRequestEnded` 赶在任何 `onHeaderField` 之前到达，拿「已完」当「已收齐」的那层于是交回一份**零字段的响应**；
  另一种错法是续解时按「此刻是否已收过正文」判这一段是头段还是尾段——正文帧夹在中间先到时，请求头会被当尾段
  判成非法序列（RFC 9114 §4.1），一条答对的响应被本端自己作废。改法是挂起时记下两个事实（这一段还阻塞着、
  它是头段还是尾段），指令补齐后先交付字段、再补上被推迟的那一下收尾。**含一处用例订正**：随本条进来的
  `BodyArrivingWhileFieldSectionBlockedDoesNotTurnTheHeadIntoTrailers` 初稿把「本端还没作答，这条流的两侧没齐，
  状态不该回收」这条判据挂到了 `requestsEnded` 上（与同批另一条用例同文，那边对的是 `streamsClosed`）——
  按头段与正文都已收齐的事实，这里应当断言请求**已经收完并被派发**（`requestsEnded` 一条）而流状态**尚未回收**
  （`streamsClosed` 为空），已按此改正。
  外部裁判与用例：`scripts/quic_outbound_cross_check.sh` 的对端从「aioquic 当客户端」扩成也能当 H3 服务端
  （`tests/Tools/QuicProbeClient.cpp` 是我们这边的被测端），两条通路各留一行事实行、缺一行即 FAIL。
  树内 `Http3ClientConnection.CompletesARequestRoundTripAgainstOurOwnServer` 钉「自家客户端打自家服务端」的
  完整往返（握手 → 起 h3 → 提一条 GET → 逐字核对状态码、响应头字段、正文与服务端的请求计数）。
  证伪分两处做：摘掉续解点的补收尾 → 两条 QPACK 判读用例同时红在「请求收完」那一行；把续解点的身份判据换回
  「此刻是否已收过正文」→ 只有那条正文先到的用例红在「没被判非法序列 + 字段交得出」两行。
  **本轮仍未接上**：`HttpClient` 与出站池要用 h3 还差最后一公里（ALPN/协议选择那一档目前只有 h1/h2），
  现在能拿到的是协议层这一格：能握手、能提请求、能收响应、能礼貌收尾。

- **HTTP/3 从此也有「响应产出预算」这道闸门（`HttpServerLimits::writeTimeout` 在 h3 上真的算数了）**：
  这条通路此前只有读时限（`readTimeout`）在按时限收口，`Http3Session::expireStaleRequests()` 里那句
  「处理器那一头的预算本类不判」就是缺口本身。h1 与 h2 两侧由连接的空闲截止时间同时罩着两头（把请求交给
  业务之前、每次写出之前各刷一次 `writeTimeout`），h3 没有套接字可等，于是两种停摆都没人管：**处理器卡在
  业务自己的等待上**（等数据库、等下游服务、等一把没人放的锁），以及**响应字节出不了发送口**（对端只连不读、
  把流控窗口压死，生产者可一直挂在「等缓冲排空」上，挂多久由对端决定）。内存那一侧早有闸门（每流约 256 KiB
  留存量加传输层的待发上限），缺的是时间这一侧——多条流各占一份就能按对端的意愿长期占住会话记录与在途正文额度。
  落点按**流**而不是按连接：h3 一条连接上的流彼此独立，为一处停摆拆掉整条连接会把同连接上别人在途的请求
  一起带走（与把 431/提交失败收到单流那轮同一判据）。每条进入产出相位的流挂一个截止点，头部上线、正文被
  连接层接走时各往后推一格——只要字节真的在出门就不算停摆；判过点之后按 RFC 9114 §8.1 用
  `H3_REQUEST_CANCELLED` 复位这条流、摘掉它的记账，并在安全点叫醒挂在等空间上的生产者，让它看到「流已关闭」
  而不是挂到连接收口。处理器随后才回来时，它产出的那份响应**不再写上去**（流已复位，再写就是给一条死流攒
  待发字节，排空判定随之永远为真），也不落「已应答」那笔账。`writeTimeout` 为 0 仍是「关掉这项保护」的既有
  口径：连时限表都不建条目。
  **这会让超过预算的 h3 处理器与停住的响应被收口，而此前不会**——h1/h2 上默认已是 60 秒，把 h3 对齐过来
  就是为了让同一份配置在三条通路同解；真有长跑的处理器，把 `write_timeout_ms` 配大或设 0（也可以让处理器
  定期交出一点正文，产出进展会自己续期）。
  用例：`Http3Session` 新增 4 条（处理器超时→复位这条流且迟到的响应不上线；预算之内→照常 200；时限为 0→
  一小时也不收口且响应真的出得去；发送口一个字节都不接→复位并把生产者叫醒）。证伪：把两处共用的
  `nextProduceDeadline()` 改成永不过点→两条收口用例立刻红，两条对照用例照绿。
  口径：这条出口记进 `badRequestCount`，与读时限、413/431 同一类（本端按时限或限额拒绝，不算对端主动取消）。
  门禁（同一份内容）：Windows Debug（MSVC `/W4 /WX` 加 ASan）全目标零告警、TestNet 1463/1463；容器 GCC 13
  Debug 加 ASan/LSan/UBSan 全目标零告警、`ctest -j 8` 3215/3215（68 条真库用例按环境 SKIP）、零 sanitizer 命中。

- **QUIC 出站连接（`Net::QuicClientConnection`）**：自研传输层从此两个角色都走得通——之前只有服务端一侧
  （`QuicServer`/`QuicConnection::accept`），出站方向要连别人的 QUIC 服务就只能借外部实现。
  `QuicConnectionCore` 与流层按 `QuicConnectionRole` 角色化，落差的那一小撮判据集中在六处：Initial 密钥的
  收发方向、TLS 侧以谁的身份建、传输参数里 `original_destination_connection_id` 该不该出现（服务端必填、
  客户端只在接住 Retry 之后才允许，本实现不做 Retry 故必须不写）、§7.3 绑定校验查哪一边、
  HANDSHAKE_DONE 的收发方向（服务端收到即 PROTOCOL_VIOLATION，客户端收到是 §4.1.2 的确认信号之一）、
  以及只约束服务端的那两条（§8.1 反放大上限与地址验证）。客户端另加三条自己的义务：§7.2 的「把回包目的标识
  换成服务端自报的源标识」（只跟一次、只跟长头）、§14.1 的「握手期 Initial 补到 1200 字节」、
  流号低位翻转（本端发起的双向流 0x00/0x04……、单向 0x02/0x06……，`QuicStreamLayer` 里那条
  「是不是本端发起」的判据因此从文件内自由函数变成带角色的成员——十来处共用它，同一个流号在两型眼里归属相反）。
  外壳按**组合**而不是再抄一份：`QuicConnection::connect()` 与 `accept()` 同形（本端签发与对端标识都自己造），
  收发/flush/恢复层定时那一套两型共用。出站 TLS 用 `Core::TlsContext(policy, Role::Client)` 的现成上下文，
  每连接的 SNI、证书校验名与 ALPN 新增为 `QuicClientTlsSettings`（三项都硬要求，落不上去当场抛而不是静默跳过）。
  `Core::AsyncUdpSocket` 补了 `close()`——顺序仍是「先销毁注册对象（它唤醒挂在上面的协程）再关描述符」，
  握手时限（`Core::DeadlineGuard`）靠这条才掐得断。
  用例：`QuicClientConnection` 4 条（与自家服务端做完握手并谈定 h3、证书链可信但名字不符要被拒、
  对端完全不说话时按握手时限收场且耗时落在上下界内、主机名为空在构造期就拒）；
  `QuicConnectionCore.PadsClientInitialDatagramsAndIgnoresTheAmplificationLimit` 钉补足与反放大豁免。
  **跨实现裁判换成外部工具**（这一轮的验收主力，`scripts/quic_outbound_cross_check.sh` + 同名 .py 与
  `tests/Tools/QuicProbeClient.cpp`）：对端是 aioquic 起的 QUIC 服务端，按它自己的解析器回显流数据，
  两边各打事实行、缺一行即 FAIL。两个场景：ALPN 谈得拢（对端要报出握手 + 收满 27 字节，本端要报出回显逐字相同）、
  ALPN 谈不拢（对端要报收到过报文却没报握手完成，本端要自评失败）。
  证伪：不补足 Initial → 那条尺寸用例红；把反放大上限也套到客户端头上 → 尺寸用例与回环握手同时红；
  客户端也把 ODCID 写进参数 → 回环握手红（自家服务端按 §7.3 拒了它）；**去掉 §7.2 的连接标识跟随 →
  树内四条用例全绿、只有外部裁判红**（aioquic 不再认得我们的包，自家服务端却同时认得客户端自造的那个标识）。
  这一条正是「按自家代码互测暴露不出来的缺陷」的实例，记在这里当作后续角色化改动的判据参照。
  两条已知边界：作客户端**不能接住 Retry**（不实现，缓解只到「挑 8 字节以上的随机目的标识」），
  以及本轮**没有把 h3 出站会话接上**——`HttpClient`/出站池要等下一轮的 `Http3ClientConnection`，
  现在拿到的是传输层这一格：能握手、能验身份、能在流上双向送字节。
  一处实现细节值得记：`enableClientPeerVerification()` 必须显式调（`Role::Client` 只摆形状、
  真正把 `SSL_VERIFY_PEER` 打开的是这一句，与 `HttpClient` 同一处）——漏掉时的形态是握手照成、
  证书照收、没人核对身份，本轮就是被那条名字不符的用例抓出来的。


- **远端一句话就能触发的重复告警有了时间闸门**：新增 `Base/Log/LogThrottle.h`（`Base::LogThrottle` 与宏
  `ASYN_LOG_THROTTLED(间隔)`），并把 `TcpServer` 要求 PROXY 协议头时的那几条收口告警接上——开了
  `setProxyProtocolRequired()` 的端口上，每个不打头的连接原本都会留一条 WARN，而日志是同步落盘的。
  两层后果都真出过：日志被同一句话刷满、要看的信号淹在里面；以及热路径上的逐条写把事件循环拖住
  （停摆表现为 backlog 灌满后回 ECONNREFUSED）。压的是重复条而不是这条告警——「有人在打这个端口」本身
  要让运维看见，于是**放行的那一条带上「过去 N 秒内另有几条同类被压掉」**，量级信息不丢。
  间隔 10 秒，判定只用原子量与一次 CAS（没有锁、也没有「按调用点建表」的哈希查找：闸门自己不能成为新的开销）。
  宏的形态是「返回调用点那份闸门的引用」而不是把整条日志包掉——调用方仍自己决定级别与格式化什么，
  于是既不用新增日志宏，也不用给六个级别各配一份带间隔参数的变体。
  接了六处：五条 PROXY 收口 WARN 与那条正文固定的 `catch (...)` ERROR。**带 `what()` 的那条 ERROR 刻意不压**——
  它的价值就在每次可能不同的原因文本上，压掉等于把要查的原因一起丢了。
  `TcpServer::setProxyProtocolRequired()` 的 @note 随之改写（原文承诺的是「各留一条 WARN」）。
  用例：`tests/Base/Log/TestLogThrottle.cpp` 10 条（首条不等窗口即放行、窗口内全压、下一窗口带出条数、
  每窗各计不累加、压制期间不改已交出的读数、间隔非正退回不压、八线程抢同一拍只放行一条、
  两个调用点各一份窗口、宏这一侧状态跨调用留存、宏这一侧读得出条数）；
  `TcpServer.ThrottlesRepeatedProxyRejectionWarnings`（回环上五条不合格连接 → 该调用点这批至多一条，
  且全进程至少一条）。
  证伪：把 `acquire()` 改成永远压在窗口内，Base 五条红 + 端到端那条红在「全进程至少一条」上（0 vs 1）；
  撤掉 `TcpServer.cpp` 首个调用点的包装，端到端那条红在「这批至多一条」上（5 vs 1）；去掉计数清零改成累加，
  `CountsEachWindowSeparatelyRatherThanAccumulating` 单独红；把宏改成全调用点共用一份窗口，三条宏用例红。
  一处**测不出来**、如实记下：把窗口起点的 CAS 换成「先 load 判断、再 store 立窗口」，八线程 × 20 轮 × 10 次
  运行一次都没撞上（load→store 之间只有几纳秒，而线程彼此放开的间隔是它的百倍量级；早先让线程 park 在条件
  等待上由主线程 notify 的写法更测不出来）。那条改动留在实现里是为了语义精确，不当作可测判据写进注释——
  写一条测不出来的断言比不写更容易骗人。
  一条待补：其余协议层里还有哪些告警属于「对端一句话就能触发一次」这一类，本轮没清点（只数了 PROXY 这一组，
  因为它的形态最明确：开了要求的端口上，每条不打头的连接稳定一条）。清点出来再接，比照着一个名字清单铺开更可靠。


- **静态文件服务在 HTTPS 与 HTTP/3 上同样可配**：新增 `Net/Http/StaticFileService.h`，把「静态目录」这项
  配置的本体（配置对象、目录规范化、Cache-Control 的合法性判定、那条只登记一次的 `any("*")` 兜底路由）
  从 `HttpServer` 的方法里收出来，三条服务器共用一份实现：`HttpsServer` 与 `QuicServer` 各新增
  `staticFileDir()`（设与读）和 `setStaticFileCacheControl()`，明文侧的公开接口逐字不变。此前这项能力
  只长在明文 HTTP 上——**明文能配的静态目录，换到 TLS 或 h3 端口上就没有入口**，而 443 才是静态资源的
  常态落点；三段抄三份的漂移形态是「明文侧修了一个路径穿越，TLS 侧没有」，所以这里选的是收本体而不是
  抄第二遍。`QuicServer` 的路由器是外部交来的（本类不持有），因此它的静态配置在 `setRouter()` 之前
  当场抛出并说明顺序，而不是把配置落进一个永远不被服务到的对象里。
  顺带修一处文档与行为不符：**目录此刻不存在**也要在配置时刻关闭静态服务并告警。原先只看
  `weakly_canonical` 的错误码，而它对不存在的路径是按词法规范化安静成功的——「目录名拼错」由此变成一个
  静默的配置错误：`staticFileDir()` 读起来一切正常，而每个请求都回 404。文档承诺的是配置时报出来。
  用例：`HttpsServer.ServesStaticFilesFromAConfiguredDirectory`（回环 TLS 上真取一份文件：200 + 正文 +
  `content-type` + Cache-Control，另钉「不存在的文件由静态服务自己回 404」与读回的目录一致）、
  `Http3Session.ServesStaticFileFromInstalledFallbackRoute`（h3 会话层经字节级对端取回映射正文，
  钉 content-type 与 ETag 同源带出）、`QuicServer.RejectsStaticDirectoryConfigurationBeforeARouterIsAttached`
  （没路由器时拒、有路由器后不存在的目录降级为未启用而不抛）。既有静态用例（`HttpStaticFileTransfer` 5 条、
  `StaticFileMappingCache` 11 条、`HttpServer` 里的若干条）在补上存在性判定之后全部照旧通过。
  证伪：把存在性判定换成永假的条件，QUIC 那条红；把 HTTPS 侧的 `setDirectory` 转发改成空操作，HTTPS 那条
  红（3044 ms 后才失败——它在等一个永远不来的正文）。两处突变互不掩盖。
  一条待补：给 `echo_server` 加 `--static` 并在 `scripts/h3_cross_check.sh` 里加一条静态场景，让 h3 的静态
  正文也过一次进程外裁判；本文件的 h3 用例钉的是「映射正文能不能经 h3 的流通路交出去」这一层，
  帧的合规性由既有 8 条场景走的是同一条 `submitResponse` 通路代验。
- **HTTP/3 的采集账与两条 TCP 通道对齐**：三处原先漏记的地方补上，`/metrics` 里 h1/h2/h3 从此是
  同一套口径。① **耗时直方图**：h3 每条响应现在都落一个样本，起点是「会话收下这条请求」的那一刻、
  终点是「响应排进待发字节」——与 h2 的 `requestReceivedTime` 在同一相对位置。原先的口径是「h3 不参与
  直方图，因为会话没有可信的请求起始戳」，那个理由站不住：请求收齐并交付业务的那一刻就是戳，
  h2 也正是从那一刻起算；把它排除在外的实际后果是混合部署下的延迟读数只覆盖两条 TCP 通道，而看的人
  不知道。② **流式正文路由的请求原先一笔都不落账**（不计请求数、不计状态码、不计耗时）：普通请求的
  记账写在「收齐后派发」那一段里，而流式路由是头部收齐即派发、由本流自己的协程服务到底，绕过了那一段。
  只挂流式上传路由的 h3 服务因此在指标上看着像没有流量，而请求确实被收下并答了。③ **WebSocket 隧道**
  （RFC 9220 的扩展 CONNECT）的应答原先随 `continue` 一起跳过落账：这类服务报出的是「有请求、没响应」，
  而 h2 那一侧是记 101 的。h3 没有 101 这一档，记的就是真实发出的那个 2xx。
  三条派发路径共用一条判据：半途抛异常、头部已上线的流式响应只发了一半，不落账（记成已应答会把
  状态码类与直方图一起写脏）。h3 也补上了每请求一条的 Debug 完成日志（含 request-id 与耗时微秒），
  与 h1/h2 同口径。
  用例：`Http3Session.ReportsRequestsAndStatusClassesToMetricsCollector`（耗时要真量出来：处理器里
  睡 2 毫秒，断言样本数为 1 且总耗时不小于那 2 毫秒）、新增
  `CountsStreamingBodyRouteRequestIntoMetricsCollector`（流式漏斗三笔账）、
  `ClosesTunnelWhenConnectAndEndStreamArriveTogether` 尾部加钉隧道的三笔账。
  证伪（分两次构建，每次只红自己那条）：把流式漏斗的计入与落账去掉、把主循环两处 `recordResponse`
  换回 `countResponseStatus`——同一趟构建里这两条各红各的；再把隧道那处的落账去掉，红的恰好只有
  隧道那条。
  跨协议一致的对外判定仍由进程外的 aioquic 做（`scripts/quic_cross_check.sh`、`scripts/h3_acceptance.py`），
  本文件里的字节级对端只作状态机回归。
- **准入闸门把自己挡掉的连接报出来**：`/metrics` 新增一族计数器
  `admission_rejected_connections_total`，取的是按来源 IP 的并发限额器累计拒绝的条数
  （`PerIpConnectionLimiter::rejectedConnectionCount()`，经新增的
  `TcpServer::perIpRejectedConnectionCount()` 并进 `HttpServer::stats()` / `HttpsServer::stats()` /
  `QuicServer::stats()` 三份快照）。存在的理由很具体：**被挡掉的连接从来不是连接**——它没进在册表、
  没进请求计数、没进状态码分类，在对端那边只是一次重试，在服务端这边原先什么都不留。于是
  「闸门在挡人」「这段时间没流量」「限额配置写错把谁都挡」这三种形态在抓取端看是同一幅景象，
  而只有第一种是想要的。口径三条：只统计**因达到上限而拒**的那些（其它收口路径各自有族）；
  归还名额不往回扣（累计量，速率由两次采样的差值算）；限额器在多条通道间共用一份时报的是
  这道闸门的总量，不是本实例那一份——与限额本身的共用口径一致。
  用例：`PerIpConnectionLimiter.CountsOnlyRefusalsAndKeepsThemAcrossReleases`（放行的不自增、
  归还的不回扣、关掉保护时恒为 0）、`HttpMetricsEndpoint` 的渲染用例（各字段真值那份里加了一行）
  与 `AdmissionRejectionsReachTheServerStatsSnapshot`（回环上真占住一个名额、真被拒一次，再读快照）。
  后者读 `stats()` 而不抓 `/metrics`：抓取自己也要占名额，那样用例会去依赖「服务端有没有先观察到
  第一条连接关闭」这个竞态。
  证伪：把 `HttpServer::stats()` 里那句并进改成恒零，红的恰好是接线那条；把渲染那一行去掉，
  红的恰好是格式那条——两处各管一段，互不掩盖。
- **QUIC 服务端接受同一份 TLS 策略**：`QuicServer::Configuration` 新增 `tlsPolicy`（类型就是
  HTTPS 与出站客户端共用的那份 `Core::TlsPolicy`），构造期经 `applyTlsPolicy()` 落到那份 SSL_CTX 上，
  之后再把最低版本钉回 TLS 1.3。此前 QUIC 侧的上下文是自己一行行搭的：安全等级、TLS 1.3 套件、
  命名曲线、票据开关这几项在 HTTPS 上能配、在 h3 上配不了，正是这份类型当初要收掉的那种漂移。
  两侧边界分别钉住：**合用但本通路用不上的照收**（`cipherList` 是 1.2 及以下的套件串，
  `certificateAuthorityFile/Path`、`verifyDepth`、`revocationListFile` 要的是「校验对端证书」这一步，
  而本服务端不要求客户端证书——同一份策略多半还要交给 `HttpsServer` 或出站客户端用，为这几项报错
  等于逼使用方给每个监听器各写一份）；**本通路满足不了的当场拒**（把最高版本压到 1.2 是一条 QUIC
  做不到的要求，悄悄改回 1.3 就是对配置说谎，构造抛出并点出是版本这一项）。下限低于 1.3 仍按
  「只会更严」处理，不算说谎。
  用例：`QuicServer.RejectsTlsPolicyThatOpenSslOrQuicCannotHonorDuringConstruction` 拿三份各自
  不合用的写法（1.3 套件串、命名曲线、1.2 套件列表）钉「策略真的被施加到上下文上」——只存不施时
  这三份都能起服务；`QuicServer.AcceptsAPolicyWrittenForTheOtherTlsListeners` 钉另一侧。
  证伪：去掉施加那一步，恰好只有前一条红。对外行为（握手仍按 1.3、仍协商 h3）由 `scripts/quic_cross_check.sh`
  交给 aioquic 那侧裁定，不在本仓库里自测。
- **TLS 策略加吊销检查（CRL）**：`Core::TlsPolicy` 新增 `revocationListFile` 与
  `revocationCoversWholeChain`，`applyTlsPolicy()` 把列表读进上下文自己的那份证书存储，再打开
  `X509_V_FLAG_CRL_CHECK`（整条链都查时再加 `_ALL`）。因为服务端与出站客户端共用同一份策略，
  `HttpsServer` 与 `HttpClient` 两侧同时拿到这个能力，不需要各做一遍。三条口径写死在这里：
  **给了文件就等于要求吊销检查**——不留「开了检查却没给列表」那种配置，它在 OpenSSL 侧不是「没查」
  而是每条握手都失败，挂在一个只能同时成立的动作上就写不出这种配置；**查不到发证 CA 的列表时失败即关**
  （`X509_V_ERR_UNABLE_TO_GET_CRL`），因为「列表没同步到就被当作没吊销」比「同步断了先拒掉」危险，
  所以不准备反向开关；**只支持单个文件**（PEM 或 DER，一份里可以放多张 CRL），哈希目录那种布局刻意
  不做——多一处目录约定就多一处 `c_rehash` 忘了跑这类只在运行期暴露的错。默认构造的策略仍然不查吊销，
  行为与本项出现之前逐字相同；列表读不出来时策略施加**当场抛出**并点名是哪一份文件（半生效的 TLS
  策略比启动失败危险得多）。
  用例：`TlsContext.RevocationListRejectsTheCertificateItLists`（同一张叶证书，列表点名它则拒、
  空列表则放行，判据取客户端校验码 `X509_V_ERR_CERT_REVOKED` 而不是「握手没成」）、
  `RevocationCheckingFailsClosedWhenTheIssuingAuthorityHasNoList`（三级链下只给根那份列表，
  配「同一套材料不查吊销时验通」的对照组）、
  `WholeChainRevocationCheckingAlsoRejectsARevokedIntermediateAuthority`（同一份列表文件，
  只查对端那一张放行、整条链都查拦下）、
  `UnreadableRevocationListIsRejectedAtPolicyApplyTime`（路径不存在与内容是垃圾两种都抛）。
  四份材料（根 / 中间 / 叶 / CRL）全部在用例里现造，不留会过期的证书夹具。
  证伪：把打开标志那一步换成长度恒零的等价写法，前三条一起红（第四条不受影响，说明它测的是加载
  而不是标志）；只把 `_ALL` 那一元去掉，红出的恰好只有整链那一条。
- **出站冷池上同时进来的请求共用一次握手**：新增 `Net/Http/Client/HttpOutboundEstablishment.h`
  （`HttpEstablishmentTable` + `HttpEstablishmentAwait`），连接池按端点（主机 + 端口 + TLS 位，键文本
  用单元分隔符而不是冒号）记「这一头正在建连」：第一个到的当领导者，后来的 `co_await` 它结算，醒来
  直接复用结论。此前冷池上并发进来的每条请求各建一条连接——五条并发就是五遍 TCP+TLS+ALPN+h2 前奏，
  而 HTTP/2 的复用恰恰发生在同一条连接上。判据取「请求都还挂在服务器里时，服务器在册几条连接」
  （`activeConnectionCount()`；池里的条数没有鉴别力，五条握手会被折叠成一格）：改动前五条，改动后一条。
  三条边界各由一条用例钉住：① 领导者把结论交进池之后才结算资格——试过「前奏一完就交池并结算」让等待者
  只等一次握手而不是等完领导者整条请求，那一版在 keep-alive 竞态的恢复用例上红（第二条新建的连接立刻读
  失败），机理未查清，因此这里取正确的那一种；② 领导者**每条出口**都要归还资格，包括建连失败那一条，
  漏一处不是退化而是把这个端点的出站请求永久堵死——等建连那一步自己不设时限，只有结算叫得醒它；
  ③ h1 的连接一次只租给一个请求，没有可共享的结论，所以走 h1 之前先把归还做掉，等待者立刻各自去建。
  等待不设时限，但醒来仍按自己的剩余预算判定：预算被这段等待吃光时是一条点明阶段的失败
  （「等同一端点的建连把预算用尽」），不是一次无声挂起。记账表由 `shared_ptr` 持有、等待者也拿一份，
  于是池先于某个挂着的等待者销毁时，后者析构摸的是自己手里这张表（连接池踩过一次这个坑，代价是一起
  真实 UAF）；唤醒一律走 `scheduleRemote` 投回协程自己那条循环，本表不碰套接字。
  用例：`HttpsServer.CoalescesConcurrentColdStartsOntoOneHttp2Connection`、
  `FailingEstablishmentHandsTheEndpointBackToItsWaiters`、
  `ConcurrentColdStartsOnHttp1EachBuildTheirOwnConnection`。
  证伪：删掉建连失败那一条归还，第二条红在 6.02 秒（三条并发只回一条，其余挂在等待里，由用例自带的
  看门狗叫停循环）；删掉 h1 那一条归还，第三条红在 6.01 秒（同样三条只回一条）；两处都删时 h2 的合并
  用例照常绿，说明三条判据各钉各的。
- **服务器可要求 PROXY 协议头，把连接记到真实客户端头上**：新增 `Net/Proxy/ProxyProtocol.h`
  （`frameProxyHeader()` 分帧 + `parseProxyHeader()` 解析，v1 文本行与 v2 二进制块都支持，纯函数不碰
  套接字），`TcpServer::setProxyProtocolRequired(true)` 打开后每条新连接先读一条头，读到的来源地址
  经新增的 `AsyncSocket::setAdvertisedPeerAddress()` 成为这条套接字的对端身份——按来源 IP 的并发限额、
  `HttpRequest::remoteAddress()` 与日志因此都按真实客户端算，而不是「一整个负载均衡器共用一个来源」。
  头可以分片到达（读侧按版本各自的长度上界继续读）；v2 定长段之后的 TLV 会被跳过并计入消费长度。
  三条口径写死在用例里：命令号或地址族不可认（`UNKNOWN`/`LOCAL`/v2 未定义命令）只证明「前面有代理」、
  不交出身份，来源仍按套接字对端记账；开头字节不是头（例如直接发来 `GET /`）当场判死并留一条 WARN，
  既不建会话也不吊着对端；长度字段谎报越过上界（v1 108 B / v2 1 KiB）同样当场判死，不给它撑缓冲。
  读头是一路独立协程，接受循环不会被任何一条连接牵住；**正在读头的连接计入 `setMaxConnections()` 的并发数**
  （否则一批「只握手不发音节」的对端就能把上限整个绕过）。
  一条限制说在前头：**头必须单独成段送到**——代理把「头 + 请求正文」挤进同一次发送时，读掉的尾巴退不回
  内核缓冲、也塞不进会话的读缓冲，这条连接按上一条判死（日志给出多出多少字节）。主流代理都在建连时先把
  头单独写一次，因此实际不会撞上。
  开关默认关闭；**只该在只有代理能连进来的端口上开**——头本身没有任何鉴权。
  用例：`ProxyProtocol.*` 18 条纯解析/分帧（含 10 条必须被拒的畸形形状，其中 `ParsesTheSpecVersionOneExample`
  直接拿规范原文那条例子当判据，不依赖本仓库自编的字节），
  `TcpServer.Attributes…VersionOneHeader/…VersionTwoHeader/ReadsProxyHeaderSplitAcrossSegments/
  LimitsPerProxiedSourceNotPerProxy/DropsConnectionThatSendsNoProxyHeader/
  DropsConnectionWithMalformedProxyHeader/DropsConnectionCarryingBytesAfterProxyHeader` 走真 TCP 端到端。
  证伪：把限额键换回套接字自己的对端，`LimitsPerProxiedSource…` 红（另一个来源被代理地址挡住）；
  去掉「不是头就判死」那条，`DropsConnectionThatSendsNoProxyHeader` 红；让「解析失败」与「头后有尾巴」
  两条出口不关套接字（等价于把连接留在协程帧里等清扫），后两条各红 2.04 秒——按值参数住在协程帧的
  参数区，只在整帧销毁时才析构，所以每条判死出口都得当场 `close()`。
- **出站连接按候选并发试（Happy Eyeballs 口径）**：新增 `Core::connectCandidates()` 与
  `Core::orderForConnectionRace()`，`HttpClient` 的连接段改用它们。此前的做法是「按解析顺序一条条试，
  每条都用整段时限」，于是**一条黑洞地址**（SYN 发出去没人应答，实测 Windows 上拒绝型失败也不给可写
  事件、要等自己的看门狗）会独自把预算吃光，后面的候选根本轮不到——表现就是「双栈主机连不上、纯 IPv4
  主机连得上」。现在：候选按 RFC 8305 §4 的族间交错排序（一族不许占满前几席，同时保留 RFC 6724 的
  偏好：排序结果第一条所属的族仍是首选族），最多四条同时在途，第一个连上的赢、其余当场关掉。
  不做 RFC 8305 §5.4 的 200 ms 固定错峰：框架的定时器等待器没有「提前唤醒」接口，错峰等待一旦被
  收场信号打断不了，最常见的「对端直接 RST」就要白等 200 ms；改成并发首发。代价说清楚——同场并发的
  候选都会真的发出 SYN，输掉的那条对端会看到一次「建立又立刻结束」的连接；条数超过在途上限时，预算按
  「从本波起还有几波」均分，所以第五条一定轮得到，而一两条地址（双栈最常见形状）仍各拿完整预算。
  用例 `ConnectionRace.*` 六条：排序逐位比对、拒绝要往下试、超出上限要补发、黑洞不许拖场、全连不上要
  按时限收场、空候选与零预算不许挂起。把在途上限改成 1 且不切预算（等于退回旧的顺序试法），三条红；
  撤掉族间交错、撤掉补发、撤掉预算均分各让一条红——都已实测。
- **名字解析有了一层进程级缓存**：`Core::AsyncResolver` 此前每条请求都重新问一次 `getaddrinfo`——
  冷启动的连接池、被对端收掉之后的重连、以及每一条新出站请求都要为一台主机付一次「起后台线程 +
  一次 DNS 往返」。现在按「主机+端口」缓存 60 秒：键把主机名按 ASCII 折小写（域名大小写无关，
  不折叠就会让 `Example.COM` 与 `example.com` 各占一格），端口用单元分隔符接在后面（不用 `:`，
  IPv6 文本里全是冒号）。**只缓存非空结果**：`getaddrinfo` 把「解析失败」和「这个域名确实没有记录」
  混成同一个空列表，缓存失败等于把一次抖动粘成 60 秒连不上。TTL 定 60 秒而不是常见默认值那种
  「一天」：`getaddrinfo` 不交出记录的 TTL，只能自己定，而「换 IP 的域名要能自己恢复」比省下几次
  解析值钱。表有条数上界，满了先清过期的、仍满就整表丢——解析必须能继续，缓存不能吃内存。
  IP 字面量本来就不进这套流程（下一条修复里已改成直接构造地址），所以也不占缓存。
  新增一个读数 `AsyncResolver::stats()`（累计查询次数与其中命中缓存的次数）：命中与否是按数判的，
  计时在共享机器上量不出东西；用例 `CachesNameLookupsPerHostAndPort` 因此能同时钉住「同键第二次
  必命中」「换大小写算同一个键」「换端口必重新解析」——把读缓存那段撤掉，命中增量立刻从 2 变 0。
- **出站客户端能流式上传正文**：`HttpClientRequest` 多一个字段 `bodySource`——一段一段把正文交出去的
  异步来源（`HttpBodyChunkSource`，交 `std::nullopt` 表示结束）。填了它就忽略 `body`（`contentType`
  照旧生效），两者同时给属于用法错误，当场抛 `InvalidArgumentException`：两种写法的定界头互斥
  （`Content-Length` 与 `Transfer-Encoding: chunked`），同时挂上等于让对端挑一份信，而挑哪一份由
  中间盒决定。上线的形状按通路而定：HTTP/1.1 是 chunked 分块（RFC 9112 §7.1，末尾补终止块），
  HTTP/2 是分帧的 DATA（HEADERS 不再带 END_STREAM，收尾用一条空的 DATA 带 END_STREAM，§6.1 允许）。
  **两条都是「拉一段、发一段」**：上一段没写上通路（h1 是套接字没收下，h2 是对端的流控窗口还没还）
  就不会叫下一段，于是传大文件时内存里同时只有一份分段，而不是整份先攒进一个字符串再发。
  整体时限覆盖生产正文那一段：来源自己按住不放，到点就是一条失败请求，不会把循环挂住。
  与入站侧的 `HttpRequestBody`（服务端收流式正文）正好是一对镜像。
  用例两条同名前缀的端到端（`HttpOutboundConnectionPool.UploadsStreamedBodyAsChunkedRequest` 与
  `HttpsServer.UploadsStreamedBodyAsDataFrameSequence`），判据用**握手**而不是计时：处理器的交付计数
  抬起之后客户端才肯生产下一段，所以「整份攒成一坨再发」会让服务端只看到 1 批而当场报红——
  不需要靠 sleep 制造重叠，慢机器上也不会假绿。另有一条拒绝用例（两种正文同时给）。三处证伪各自
  变红：撤掉 chunked 分块与泵段（h1 红）、把 END_STREAM 提前打在 HEADERS 上（h2 红）、漏掉收尾那条
  空 DATA（h2 红）。
- **TLS 的可配置面收成一份 `Core::TlsPolicy`，服务端与出站客户端共用**：能配的档位是最低/最高协议版本
  （只认 TLS 1.2 与 1.3 两档——1.0/1.1 由 RFC 8996 列为废弃，留一个能调回去的口子等于把服务端重新暴露给
  已知攻击面，所以不给）、TLS 1.2 及以下的套件列表、TLS 1.3 套件列表、命名曲线/组、OpenSSL 安全等级、
  校验对端证书用的 CA 文件**与 CA 目录**、证书链校验深度、会话票据开关。此前这些全部写死在
  `TlsContext::createHardenedContext()` 里，而 QUIC 服务端与出站客户端又各建各的 `SSL_CTX`——
  改一处策略要翻三处代码，那三处迟早会漂（证书安装与 ALPN 选择已经是两份实现了）。
  **默认构造的策略与改造前逐字等价**（最低 1.2、安全等级 2、那份显式排除弱算法的套件列表、票据开着），
  因此不带参数的老写法不会在升级后换档；这条等价性由 `TlsPolicy.DefaultPolicyKeepsTheHardenedBaseline`
  与 `ServerRoleKeepsTheTls12FloorAndClientRoleDoesNot` 钉住。
- **`HttpsServer` 现在能开双向 TLS**：`loadClientCertificateAuthority()` 与 `setClientCertificateRequired()`
  两个转发口此前只存在于 Core 层，服务器这一侧摸不到——也就是说「谁能连我」这件事在公开 API 上配不出来。
  信任库可以从两处给（构造期的策略，或事后 `loadClientCertificateAuthority()`），两处都算「CA 已就位」，
  不再出现「CA 明明装载了、开启校验却说没 CA」那种自相矛盾；要 CA 目录或校验深度只能走策略这一条。
  证书热轮换时 CA、校验模式、OCSP 与票据密钥都要在新上下文上复现，漏任一项就是一次「看起来成功、
  安全性反而降级」的续期，因此复现只有一条路：按同一角色 + 同一策略重建。
- **`Core::TlsContext` 有了角色**：`Role::Server`（默认）与 `Role::Client`。ALPN 选择回调与 OCSP 装订回调
  只在服务端有意义，挂到客户端上下文上不会报错，只会在对端看来莫名其妙；客户端一侧也不预设套件列表
  （原先就没设过，设了可能把本可以连上的对端拒掉）。服务端角色在未指定下限时补 TLS 1.2 这一档，
  客户端角色不强加下限。
- **出站 HTTPS 客户端自带一份 TLS 上下文，能配策略也能带身份**：新增 `HttpClient(loop, poolConfig, tlsPolicy)`
  与 `setClientCertificate(certificateFile, keyFile)`。此前所有出站 HTTPS 共用进程级那一份 `SSL_CTX`
  （首次请求时惰性创建、之后再没有换的余地）：一个实例要改握手段位，就把同进程里别的客户端一起改了，
  而「服务端要求出示客户端证书」这一类根本没有落脚的地方。现在每个实例一份上下文，策略、信任库与
  身份都挂在实例上。**默认档不变**：不带策略的构造仍然校验对端证书与主机名，只是这档现在可以按实例覆盖。
  信任库的取舍只有一处判据：策略里给了 CA 文件或目录就**只**认那一份，不再补系统信任库——两处都认等于
  把「我只信自己 CA 签的对端」悄悄放宽成「公网根 CA 签的也信」；没给才退回系统信任库
  （Linux 走 `/etc/ssl/certs`，Windows 走系统存储）。校验对端这一侧有意不开「关掉」的口子：不校验等于
  任何受信 CA 给他域签的证书都能冒充目标主机（CWE-297）。静态入口（`HttpClient::get(loop, url)` 那一组）
  仍走进程级默认上下文——它没有承载策略的地方，要按实例配就走带池的构造。
  `HttpsServer.MutualTlsAcceptsClientWithCertificateAndRejectsWithout` 钉住两端各配一半都成立。

- **出站客户端会透明解压响应正文**：`HttpClient` 现在替调用方声明 `Accept-Encoding: gzip, deflate`，
  并按响应的 `Content-Encoding` 把正文解回来，解完把 `content-encoding` 与 `content-length` 两条头部
  一起删掉——前者已兑现，后者的长度描述的是压缩前的字节。口径对齐 Go 标准库：**调用方自己写过
  `Accept-Encoding` 时本端完全不管正文**（既不代加声明也不解），gRPC/对象存储那类要拿原始字节做校验的
  用法因此不受影响。`identity` 与没有编码的响应原样交回；HEAD/204/304 这类空正文也不报错。
  解不了的一律判这次请求失败而不是交回原样字节：交回压缩字节的形状是「200、长度也对、内容是乱码」，
  比一个错误难查一个量级——对端发来本端没请求的编码（`br`）、链式编码、被截断或解坏的流都属这一类。
  解压有硬上界（默认 64 MiB，`inflateHttpBody` 那层还有一道按次调用可传的上界）：deflate 的压缩比
  能上千倍，不设界等于让对端用几百字节撑爆本端内存；到界即失败，不交回前 N 字节。
  两条承载（HTTP/1.1 与连接池里的 HTTP/2）都走这同一处收尾。
  解码函数是本层新增的 `inflateHttpBody`（`Net/Http/Gzip.h`）：gzip 容器与 zlib 流两种头都认
  （建流用 windowBits=15+32；只写 +16 会静默拒收 `Content-Encoding: deflate` 的规范形状）。
  它没有进 `scripts/fuzz-net.sh` 的模糊清单——那要求 /MT 的 zlib，而本仓 Conan 侧只有 /MD 的；
  缓冲算术由用例钉住（上界、截断、垃圾字节各一条）。

- **分块请求的 trailer 字段现在会交给业务**：`HttpRequest` 多出一档独立的 trailer 存储——
  `getTrailerField()` 按名取值、`hasTrailerFields()` 判有无、`forEachTrailerField()` 按线上到达顺序遍历。
  此前 `HttpParser` 把尾部字段校验完语法与上限就整份丢弃（当时的理由是「trailer 里的 `Content-Length`
  会与已解析头部形成两种长度解释」），业务因此永远拿不到校验和、尾随状态这类只可能在正文之后出现的值。
  现在只有真正危险的那几个字段仍然丢弃：`Content-Length`、`Transfer-Encoding` 与其余连接级字段
  （`Connection` / `Keep-Alive` / `Proxy-Connection` / `Upgrade`，RFC 9112 §7.1.1.1 本就禁止出现在尾部），
  其余原样上交，且**永不并入请求头部**——两档各自可读，头部视图里不会出现尾部字段。
  条数与总长两道上限照旧管住尾部（`HttpParserLimits::maximumHeaderCount` 的口径本来就是「trailer 头部同样计入」）。
  RFC 9110 §6.5.1 允许收端忽略「`Trailer:` 声明头未列出」的尾部字段，本实现不采用那条放宽：声明头缺失
  在真实客户端里很常见，据此丢字段等于让业务拿不到数据，而这些字段本就进不了头部、影响不到报文边界。
  那档存储按需创建，不带尾部的请求一次额外分配也不付（每请求分配台账的读数因此不变）。
  **HTTP/2 同一口径**：尾部头块的字段随那条 `END_STREAM` 收口信号一起交出，`content-length` 同样筛掉
  （连接特定字段在 h2 侧本就不是「丢弃」而是流错误，见 RFC 7540 §8.1.2.2）。h2 不为它另开一条事件通道
  ——RFC 9113 §7.1 规定尾部头块必须自带 `END_STREAM`，「正文收齐」与「尾部字段到齐」本就是同一件事。
  **HTTP/3 也一并接上**：QPACK 解出的每个字段现在带着「来自头段还是尾段」交给会话，会话据此分流落档。
  这一条不只是补齐能力，此前尾段字段走的就是普通头部那条路——正文之后到达的值于是有了头部的身份，
  业务按头部读到的东西与线上事实不符（h3 的判定器已按 RFC 9110 §6.5 把尾段里的 `content-length`/`host`
  判成非法，所以那条最危险的路本来就走不通）。

- **出站响应也能写 trailer 了**：`HttpResponse` 补上与请求同一档独立存储——`addTrailerField()` 登记、
  `getTrailerField()` 取值、`hasTrailerFields()` 判有无、`forEachTrailerField()` 按登记顺序遍历。登记过的
  字段在三条出站通路上落到各自的位置：h1 是终止块之后的字段段（`0\r\n` + 字段行 + 空行，
  RFC 9112 §7.1.2），h2 是随 `END_STREAM` 收口的尾部头块（RFC 9113 §7.1），h3 是排在最后一个 DATA 之后的
  尾段字段段（RFC 9114 §4.3）。`Trailer:` 声明由序列化层按登记结果自动生成，三条通路共用同一份拼法，
  业务自设的那一条整体让位（两条并存对端读到的就是两份可能不一致的承诺）。
  拒收而不是静默丢弃：以 `:` 开头的伪头、`content-length`、`transfer-encoding` 与其余连接级字段一律
  `addTrailerField()` 返回 false——写错字段名的业务代码需要当场知道自己没生效。声明只在头部上线前那一刻
  有效，因此要把字段交给对端读，就得在首次 `writeChunk()` 之前登记完（RFC 9110 §6.5.1 允许严格的收端
  忽略未声明的尾部字段）。
  两处刻意的取舍：**业务中途抛异常时不补尾部字段**——那些值是业务对它「算完的那段正文」负责的承诺
  （校验和、行数），正文只发了一半就补上去，对端按字段校验必然失败，而这条连接/这条流本来也不再用；
  **HEAD 的响应不带尾部字段**（按定义没有正文，也就没有「正文之后」），头部的声明仍保留，它描述的是
  同一条报文若以 GET 请求会带回什么。h3 侧顺带修了一处相邻缺陷：越过对端通告的
  `SETTINGS_MAX_FIELD_SECTION_SIZE` 而被拒的那次提交，原先会先把「这个段已经过完」留在流上，于是同一条流
  下一次合规的提交被本端自己的状态判成「第二个头段」——上限那道闸现在排在字段判定之前，拒了就不留任何状态。

- **协议解码器的持续模糊接进流水线**：`linux-ci.yml` 多一条 `protocol-fuzz` 作业，每次推送用 libFuzzer
  向四类解码器（WebSocket 帧、HTTP/2 帧与 HPACK、HTTP/3 帧、QUIC 变长整数）喂 300 秒随机字节，
  崩溃样本作为工件取出，搬进 `tests/Net/Fuzz/TestProtocolFuzz.cpp` 的种子用例即可常驻。此前这条只能
  本机手工跑（`scripts/fuzz-net.sh` 当初是为 Windows 侧写的，一轮 60 秒，跑不跑全看人记不记得），
  而这批解码器的历史缺陷恰好全是越界与释放后读那一类。脚本现在两侧都能跑：Linux 走
  `clang++ -fsanitize=fuzzer,address`，Windows 仍走 `clang-cl` 自建 /MT 闭包（理由见脚本头），
  **两边共用同一份依赖闭包清单**——换编译器不该换覆盖面。

- **日志 sink 多了一个 `formatter` 配置键**：`default` / `color` / `json`，选该 sink 的版式。此前
  `LogSink::setFormatter()` 只在代码里可换，全靠配置起服务的部署拿不到 JSON 日志（采集端最常见的形状）。
  键挂在 sink 上而不是 logger 上，所以同一条 logger 可以并排挂两种版式（控制台走文本、文件走 JSON），
  `async` 包着的那一条同样认。控制台那条路仍走能力兜底（`formatter: color` 经 `setColorEnabled`，
  输出目标不支持 ANSI 时退回纯文本），而显式的 `formatter` 优先于 `color` 键。取值不认识时按
  `default` 处理并往 stderr 说明可选值——与本文件其余容错同一口径。
- **新指标 `asyn_http_write_aborted_connections_total`**：本侧没能把响应完整交给传输层就收口的连接条数。
  两类都算：写出失败（对端带未读数据关闭，内核回 RST），以及收口时仍有字节留在待发缓冲或**流控队列**里
  ——后者更常见也更隐蔽：对端不读也不还窗口时，正文一次都没碰过套接字，写侧不报错、日志里一个字都没有。
  此前这类事件在 `/metrics` 上没有任何出口：`responses_total{status_class="2xx"}` 已经把那次应答算进去了
  （口径是「排入待发字节」），而 `http2_stream_cancelled_total` 的口径是「本端因此未发响应」，两条都对不上号。
  它与 `timeout_closed_connections_total` 是两种毛病：那条是「没人来取」，这条是「取到一半不取了」。
  一条连接至多记一次；只覆盖 h1/h2 的 TCP 侧，QUIC 由传输层自行重传，没有这个形态。
  口径止于本侧：已经被传输层收下、对端却再没读走的字节看不见（几百 KB 的响应能整个塞进环回套接字
  缓冲，随后对端关掉，本侧一次失败都不会遇到），这一类要靠对端侧的指标去发现。
- **`HttpServer::setMetricsCollector()` / `HttpsServer::setMetricsCollector()`**：把一台服务器的统计
  采集端换成现成的那一份，让同一端口上的多台监听器（每事件循环一个）把计数并进同一口径。
  此前 `metricsCollector()` 只能读不能换，共享只到「把本机的采集端借给 `QuicServer`」这一步。
  传空指针按用法错误拒绝（空采集端等于没有计数出口，而全零读数与「没有流量」看不出差别）；
  必须在 `start()` 之前调用，已建立的会话继续用创建时那份。
- **`ConnectionManager::setSharedActiveCountMirror()`**：把在册连接数同步镜像到一个外部原子量上，
  多台管理器指向同一个即得进程总量。镜像与连接表在同一条临界区里增减（重复 `add`、摘除已不在册的
  指针都不会让两者偏离），因此 `HttpServerStats::activeConnectionCount` 现在来自采集端而不是
  某一台的连接表——单台部署读数与过去逐位相同。

- **`Http2Connection` 的请求与正文向量有了回收口**：新增 `recycleRequests()` 与
  `recycleReceivedData()`，约定与既有的 `recycleOutgoingBytes()` 一条不差——上层遍历完把向量还回来，
  本端只留容量、清掉剩下的空壳元素；期间已另攒出新事件时这份缓冲丢弃而不是覆盖。
  此前 `takeRequests()` / `takeReceivedData()` 用 `std::exchange` 把向量连同容量一起交出，每次取用
  都要重新向堆要一块。`Http2Session` 的三处消费点（收请求、收正文、隧道期内的收正文）已跟着改，
  实测收一条 h2 请求的分配从 15 次 / 1621 字节降到 14 次 / 1325 字节（Release，一千次原值）。
- **`HttpRequest::reserveHeaders(fieldCount, byteCount)`**：给「一条请求从零装配头部」的调用方一个
  一次留够容量的入口（按整块交换头部的那两条路径不需要它）。HTTP/3 收请求头正是这条形状：请求对象
  随流新建，头部一条一条 `addHeader` 进去。实测同一条 10 头部的请求，不预留要 24 次分配 / 2588 字节，
  先按 4 条 / 128 字节留一档是 12 次 / 2080 字节（Release，按一千次原值计，判据是
  `HotPathAllocations.HttpRequestHeaderAssemblyAllocations`）。留的量刻意取小：比典型请求少留一点，
  超出照常按倍扩容，不让每一份在途请求都多养一段空缓冲。顺带把 h3 的 `:path → uri` 从「再抄一条串」
  改成 `adoptStagedUri()` 换缓冲。
- **响应压缩可以交给工作线程做**：新增 `compressionMiddleware(completionLoop, executor, options)`
  一版重载，把「压完整块正文」这一步交给 `Core::AsyncExecutor` 的工作线程，压完再回到发起请求的那条
  事件循环续上（`scheduleRemote`）。判断、阈值、编码协商、ETag 降级与头部改写全部与就地版共用同一份
  实现体，两条路径的产物逐字节相同——所以换执行位置不是换语义。原来的
  `compressionMiddleware(options)` 签名与行为都不变，仍然在调用协程所在线程上压。
  为什么要有这一版：HTTP 三条路径的处理器协程都跑在事件循环线程上，一条 256 KiB 正文的 gzip 要占住
  循环 4.4 ms，这期间同一条循环上的其他连接什么都做不了。
  不是所有正文都值得外派：`CompressionOptions` 新增 `offloadMinimumBodySize`（默认 8 KiB），不足它的
  仍在循环线程上压。一次「工作线程 → 循环」的投递加唤醒实测 9.7 µs（热循环下的下限，整跳约两倍），
  而 4 KiB 正文压一次 zstd 只要 8.1 µs、gzip 22.6 µs——门槛以下外派反而把响应和循环一起拖慢。
  前提与限制：外置要求「会话收尾会等处理器协程跑完」——h1 与 h2 的请求路径按结构成立，HTTP/3 则由
  承载层在摘掉连接之前叫醒并等完会话里分离的派发协程（`Http3Session::abandonPendingStreams()`）。
  `echo_server` 的明文、TLS 与 h3 三条通道默认都走工作线程，`--compress-sync` 逐档切回就地版做对照。
- **阻塞任务执行器上收到 Core**：`Core::AsyncExecutor`（原 `Database::AsyncExecutor`）是通用基础设施，
  承载一切「不能在被调用处立刻完成」的活——阻塞式驱动调用与整块 CPU 运算。头文件在
  `Core/Coroutine/AsyncExecutor.h`。
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

- **出站请求可带任意方法与附加头部**：新增 `Net::HttpClient::send(loop, url, HttpClientRequest, 时限)`，
  返回 `std::expected<HttpClientResponse, std::string>`——失败值是一句中文原因，并点明断在哪一段
  （解析地址 / 建立 TCP 连接 / TLS 握手 / 写出请求 / 读响应）。`HttpClientRequest` 带 `method`、`body`、
  `contentType` 与 `headers`：方法名与头部名必须是 HTTP token（RFC 9110 §5.6.2），值里不许有 CR/LF 等
  控制字符，`Host` / `Content-Length` / `Connection` 三项由客户端按这次请求自己写——占用它们等于让调用方
  撕裂请求行，因此当场按 `InvalidArgumentException` 拒收，不转义也不静默删。HEAD 的应答按 RFC 9112 §6.3
  在头块之后结束。静态的 `get()` / `post()` 口径不变（失败返回空并把原因记进 ERROR 日志），只是内部改走
  同一条路；`post()` 在正文非空却没给媒体类型时补 `application/x-www-form-urlencoded`。
  协商到 HTTP/2 时附加头部同样生效，名字在这一侧折成全小写（RFC 7540 §8.1.2 的硬要求）。

- **出站请求可走 HTTP/2，且连接按主机复用**：`HttpClient` 现在自带一个出站连接池
  （`Net::HttpOutboundConnectionPool`），HTTPS 握手时按 ALPN 通告 `h2,http/1.1`，协商到 h2 就改走帧
  （新增 `Net::Http2ClientConnection`：客户端前奏、SETTINGS 取值法则、GOAWAY、HPACK、按流的发送窗口
  与头块暂存、多路复用的驱动租约），协商不到仍走 HTTP/1.1，调用方一行都不用改。
  为什么要做：此前每条出站请求都重做一遍 TCP+TLS，而全仓根本没有 h2 客户端——「入站收 h2、出站也发
  h2」的反向代理与网关形态写不出来。
  复用的口径分两条，刻意不合并：h1 的连接按「一次租一个请求」出借（并看 `Connection` 头部），h2 的连接
  由池与调用方**共同持有**、按流复用，一台主机只留一条（第二条连接换不来更多吞吐，只多占一个描述符与
  一张 HPACK 动态表），也不按空闲时限收——对端还认它就一直用，能不能用只在取用时判 `isHealthy()`。
  使用者要留意三点差别：①协商到 h2 时 `HttpClientResponse::reasonPhrase` 是空的（h2 响应里没有原因
  短语这一项），状态码与头部照常；②对端在我们手里空闲期间把连接收掉是 keep-alive 的固有竞态，此时
  一个字节都没收到的那条会重开一条再来一次，收到过就不重发（那已经是响应本身出了问题，重发会把非幂等
  请求做两遍）；③`closeIdleConnections()` 只收空闲的那部分，正被人用的连接不受影响。
  观测口三个：`idleConnectionCount()`、`idleHttp2ConnectionCount()`、`http2MaximumInFlightStreamCount()`
  ——最后这个是「最忙一条连接上同时在途的流数」，与连接条数一起读才判得出复用是否落在同一条通路上。
  两件没做，用到时别当它存在：`MAX_CONCURRENT_STREAMS` 本端不代作节流（几条并发就占几条流，越限时由
  对端回 REFUSED_STREAM，本端既不排队也不重发）；冷池里的并发请求仍各开一条连接（要合并得让池挂一个
  「正在握手」的占位，目前是晚到的那条自己作废）。
  本端自己有一条额度：`Http2ClientConnection::Config::maximumOpenedStreamCount`，缺省就是 §5.1.1 给
  客户端流号的上界（一条连接 2^30 条流）。见顶之后的请求当场被拒（一个字节都没发出去，故按「可以重来
  一次」那一支处理，换一条连接），并在最后一条流收齐时交代一条 NO_ERROR 的 GOAWAY 主动退场——不这样
  流号就会回绕成小号甚至偶数号，之后每条请求都被对端判 PROTOCOL_ERROR；一条待命连接按一万请求每秒约
  30 小时就到界。想按「每条连接服务 N 条请求就换」做轮换的调用方可以直接把它填小。
  两条「对端认不认」法则也钉在这一层：头块按对端通告的 `SETTINGS_MAX_FRAME_SIZE` 切片，越界就拆成
  HEADERS + CONTINUATION、最后一片带 END_HEADERS（§4.2 的单帧负载上界与 §6.10 的续帧法则）——过去一段
  大头部会整塞进一条 HEADERS，对端按帧尺寸错误把连接收掉。正文之后的尾部头块（trailers）是**接**在响应
  头部后面而不是把它清掉：只有带 `:status` 的那一段才算「一个新的响应头部」，否则 gRPC 那类
  「正文 + trailers」的响应只剩尾部字段，业务读 `content-type` 会读到空。

- **零停机换代的编排进了库：`Core::UpgradeChannel` 与 `Core::adoptHandedOverListener()`**。此前这条
  链的机制只有 `samples/core_upgrade` 里那一份私有实现（建通道、连通道、收尾删文件、重试节奏），
  别的应用要用只能抄示例——而抄走的那份不会跟着库演进。现在两侧各剩一个入口：交棒方
  `open()` 拿通道与地址文本、`waitForPeer()` 等新一代连上（顺手收掉监听端，一条通道只交给一个
  新一代）、`handOffListener()` 把已在监听的描述符交出去；新一代一句 `adoptHandedOverListener(地址, 预算)`
  拿回一个仍在监听的描述符，直接交给各服务器那条「接管已监听描述符」的构造入口。
  平台层新增的是**通道本身**（`Platform::Socket::openHandoffChannel` 等四个入口）：POSIX 用 AF_UNIX
  流套接字——内核只在 unix 域里随 SCM_RIGHTS 送描述符——并按 0700 那一档落权限（这条通道交出去的是
  监听套接字的一份引用，那个窗口里谁连上来都能拿走一份），Windows 用回环 TCP（载荷是
  `WSADuplicateSocketW` 换出的协议信息，普通字节流即可承载，可连范围就是本机）。
  套接字文件在收口时删除（bind 前也先 unlink：上一次被强杀留下的残文件会让下一次换代以
  `EADDRINUSE` 失败，看着像端口冲突而其实只是个文件）。
  示例改为使用库 API，容器里的端到端判据不变：`RESULT core_upgrade PASS 6 gated 0`、全程 22 次探测零失败。
  仍限 POSIX 的是**整条换代链**在 Windows 上的事件后端那一环（监听句柄与完成端口的关联不可解除，
  新一代再挂就被拒），与本次收口的原语无关——细节见 `Core/Process/UpgradeChannel.h` 的注释。

- **链路追踪的「记一节」与「送出去」：`Net::Tracer` + `Net::Span` + 两种出口**。W3C 那一半（traceparent/tracestate
  的解析、生成与中间件）早已就位，缺的是把一段时间与维度记下来再交给采集端。三条刻意的形状：
  ①**采样判定分两半**——上游判定过的（traceparent 的采样位）一律照办，只有开新链路才按 `sampleRatio` 判，判据是
  trace-id 文本的哈希落在 65536 个桶里的哪几个，于是同一个 trace-id 在任何进程都落同一个桶，不会出现「服务端采了、
  客户端丢了」的半截链路；没采的请求拿到的是一节**非记录的替身**，写入全是空操作但标识仍然合法、可以原样往下传，
  处理器因此不必到处判空。②**事件循环线程只做「往缓冲里放一条」**——攒批与出口都在本类自己的出口线程上，缓冲有
  条数上界，越界只丢新到的并计数（丢最旧的会剪掉已经开始出口的那一批的开头）；`flush()` 等的是「本次受理的都处理完」
  而不是「缓冲空了」，在途那一批还没落地时返回等于骗调用方。③**丢了都少都要报**——`exportedSpanCount()` /
  `droppedSpanCount()` / `exportFailureCount()` 与守恒式「交出 + 丢弃 == 受理」一起看，出口整批拒收只算一次失败
  （按批计不按条），一个出口都没挂时攒到的节照样丢掉并计数而不是白占缓冲。出口两种：`FileSpanExporter` 一节一行
  JSON（自描述、`jq` 直接读，行尾恒为 LF 不翻 CRLF）；`formatOtlpTracesJson()` 交出一份 OTLP/HTTP 的 JSON 正文
  （64 位时间写成字符串、枚举按 OTLP 的编号发、根节省略 `parentSpanId`）。一节的标识沿用
  `Net::Traceparent` 那套定长小写十六进制，另给了 `generateSpanIdentifier()`：新开一节只要一个新段标识，
  不该再白造一个用不上的 trace-id。

- **OTLP/HTTP 的传输那一半：`Net::OtlpHttpSpanExporter`**。`POST <endpoint>` 发 `application/json`，
  对端是自家 HTTP 服务器扮的假采集端逐字段验的（正文、媒体类型、鉴权头部、非 2xx 怎么算账）。交付是
  **异步**的：`exportSpans()` 只把渲染好的正文放进本出口自己的有界队列就算收下，真正的请求由出口自带的
  一条事件循环线程发出去——否则采集端一慢，Tracer 的出口线程就整个排在一次网络往返后面。代价写进契约里：
  「收下」只到投递为止，网络层的损失看出口自己的 `deliveredBatchCount()`/`failedBatchCount()`/
  `droppedBatchCount()`，不体现在 Tracer 那三个数上。收尾（`shutdown()` 与析构）先把队列发完再走，期限
  `shutdownTimeout` 到点还压着的按丢弃计数——进程退出时既不该丢最后那批链路，也不该永远等下去。
  两处刻意的当场拒绝：地址为空或不成形、附加头部占用了 `Host`/`Content-Length`/`Connection`/`Content-Type`
  这些本出口自己会写的名字（含 CR/LF/NUL 的值同样），都在构造期抛出而不是留到运行期默默变形。

- **入站一侧接上链路：`Net::tracingSpanMiddleware()`**。这条中间件按请求上的上下文开一节 SERVER
  （名字只取方法原文——路径是无界的，进名字会把检索侧的分组打碎），写下 `http.request.method`、`url.path`、
  `http.response.status_code` 三条维度，5xx 判 Error 而 4xx 不判（那是按业务规则给出的正常答复），
  处理器抛出也照样把这一节带着失败结局交出去再原样回抛。**它还把请求上的 traceparent 改写成本节的段标识**——
  否则处理器与出站客户端从头部读到的上下文是「我们上面那一跳」，整条链路就少一环；看不懂的更高版本原样转发，
  一个字都不动（W3C §3.5）。顺带上收：`HttpMethod` 补 `methodKeyword()`（与既有的 `methodFromString()` 配成
  一对），Router 里那份私有的 Allow 头映射改为委派它——同两张表各抄一份迟早会分叉。

- **出站一侧接上链路：`HttpClientRequest::traceContext`**。填了这一个字段，traceparent 的线上形态就由
  客户端在这一处生成（那 55 字节不必每个调用点各拼一份，拼错的后果是整条链路在对端断开）；与手写的
  traceparent 同时给出属于用法错误，当场抛出而不是替调用方挑一个上级；两处都不给时行为与从前完全一致。
  静态 `send()` 与带池的实例通路两个入口都接了，各钉一条用例——三条承载（h1/h2/h3）读的是同一份 headers，
  判「两个入口都接上」比再去搭一套 h2 对端更贴得住这个改动。

- **链路有了配置入口与部署落点：`tracing.*` 段与 echo_server 的接线**。读出是一层
  （`readTracingConfiguration`：未知键即拒并点名到 `tracing.otlp.url` 这样的完整路径、类型不符不取整不回落、
  `enabled=false` 时其余一概不校验），装配是另一层（`buildTracer`：配了哪些出口就挂哪些，服务名缺失或采集端
  地址畸形都当场抛出而不是留一条永远发不出的路）。echo_server 读同一份配置文件的 `tracing` 段，开了就在
  三条通道的路由上各挂一节 SERVER，`GET /trace` 回显的 span-id 与落盘那行的 `spanId` 是同一个——
  「头部指回本节」这条承诺由此能在现场核对。没开 `--trace-context` 也照记，只是启动时提示一句
  「本节按请求头上原样的上下文起」。

- **事件循环的停顿第一次能隔着线程看见：`GET /debug/loops` 与工作段告警**。此前「哪条循环被处理器占住了」
  在现场没有任何读数：`/healthz` 只答得出「这个进程还肯应答」，而真停住的那条循环恰恰是答不出来的——
  观测面因此必须**读原子量，而不是把问题投进那条循环里等它回话**。每条循环按相记账（`Working` 与
  `WaitingForEvents` 分开：等事件的那一相再久也只是空闲），于是一次抓取报出的是这一相已持续多久、跑完过
  多少条工作段、最慢一条多久、门口还堆着几件外来投递。表是**进程内**的（每工作线程一条、链路出口自己那条、
  客户端临时起的那条都在同一张表里），所以一条端点就够，不必按服务器逐个接线；同一个阈值（100 毫秒）在
  循环侧另落一条 ERROR，每段各报一条、不按调用点合并——八条循环一起卡时，压成一行就看不出卡的是哪条。
  三件刻意的取舍：①**不抓别的线程的调用栈**（那要挂起对方，且在 ASan/TSan 下不可靠），相位 + 时长 + 积压
  足够指认出「这条循环上还没跑完的那件事」；②观测槽位满（1024 条）时不拦构造，只把差额记进
  `unregisteredLoopCount`——少一条记录总好过让一条能跑的循环建不起来；③记账只在换相时各取一次时刻
  （一轮两次），不额外加锁、不进任何临界区。`Scheduler::remotePendingCount()` 是这次唯一新开的跨线程读取口：
  它只读两条跨线程队列各自的原子计数，本地就绪队列那份账仍只有循环线程自己数得清。

- **进程常驻内存进指标端点：`asyn_http_process_resident_memory_bytes`**。运行期内存采样的第一档：
  `Platform::ProcessInfo::residentMemoryBytes()`（Windows 取工作集，POSIX 取 `/proc/self/statm` 的常驻页
  乘页尺寸，读不出按 0 交回「不知道」），并进既有的那条**进程级读数漏斗**（与阻塞队列深度同一处、
  同一理由：三条通道都从那里取快照，收在一处就不存在「哪条通道忘了接」）。这一列看的是**趋势**而不是
  活跃分配量——分配器留下的空闲页仍算常驻（glibc 就不把页面还给内核），所以「掉下来的那段」才算真还回去了。
  刻意不做的是分配器自己那档统计：`ASYN_WITH_MIMALLOC` 默认关，接上来在这道门禁里既编不到也验不到，
  等于交出一份没有消费者的读数。

### 变更

- **删除三个 src 内零抛出点的公开异常类**（`Base::NetworkException`、`Base::ConfigFileException`、
  `Base::ConfigParseException`，连带安装出去的三个头）：判定依据是「全仓 `src/` 内既不被抛也不被
  捕」——`SystemException` 已覆盖带 `std::error_code` 的系统失败并按需携带上下文，socket 侧实际
  抛的就是它；配置侧的文件读写与 YAML/JSON 语法错误从一开始就走 `ConfigLoadResult::errors`
  （一次加载收集全部错误，而不是撞到第一条就抛出），那两个类型从未成为任何路径的出口。
  它们各有完整的实现与钉行为的用例，所以删之前把这些覆盖点逐条对账：拷贝语义与「消息+字段保真」
  的用例改挂到仍有两个字段的 `ConfigValidationException` 上（覆盖不消失），层次可捕获性列表里
  去掉两项。**这是移除已导出的公开类型**：外部代码若曾 `catch (const Base::NetworkException &)`
  或构造它们，需改捕 `Base::SystemException` / `Base::Exception`（后两者一直是这些类型的基类，
  所以「一句 catch 兜住框架错误」的既有写法不受影响）。

- **实现细节命名空间统一成 `Detail`**：同一层此前有两种写法——Base/Database 与三个同名目录写 `Detail`，
  Base/Core/Net 的 12 个文件写 `detail`。往大写收而不是往小写收，理由有两条：`.clang-tidy` 里
  `readability-identifier-naming.NamespaceCase` 早已钉成 CamelCase，而本仓的规矩是「文件夹名与命名空间对应」
  （`src/Base/Log/Sinks/Detail/` 就是这么来的）。改动是纯改名（29 个文件、110 行），不动任何行为；
  `nlohmann::detail::` 那处第三方引用原样保留，名为 `detail` 的形参、局部量与 ACME problem document 里的
  `"detail"` 字段都不在改动范围内。
  **对消费者的影响**：这些命名空间按设计不属于公开 API，但确实出现在安装出去的头文件里（例如
  `std::unique_ptr<EVP_PKEY, Detail::EvpKeyDeleter>` 这样的删除器类型）。若有人直接用过
  `Core::detail::`、`Net::detail::` 或 `Base::detail::`，要改成大写形式。

- **HTTP/3 上被挡下的请求不再计入 `requests_total` 与状态码类**（`Http3Session`）。旧行为：h3 把 431/414/413/503
  这四条「没交给业务」的收口既记进 `bad_requests_total`，也记进请求数与 `responses_total{status_class}`；
  而 HTTP/1.1 与 HTTP/2 那两路只记前一笔。后果是同一个客户端同样的越界请求，抓一次 `/metrics` 看到的
  `requests_total` 取决于对端选了哪种协议——三条通道共用一份采集端是既定设计，跨协议对照就此失配。
  新行为按 h1/h2 的口径统一：**`requests_total` 与状态码类、耗时直方图只记真交给业务的那批**，被挡下的那些
  在 `bad_requests_total` 里留一笔（该族一直导出）。要知道「服务挡下了多少」读后者。h3 的 431 与 414 两条
  路径此前零直测，这次一并补上（状态码、不交给业务、以及这笔账）。不是 API 破坏：字段与家族名都没动。

- **出站响应可以边收边处理：`HttpClientRequest::responseBodyReceiver` + `Net::SseEventParser`**。此前
  `HttpClientResponse::body` 是整份 `std::string`，三条通路都要到正文收齐才交回——大响应把整块压进内存，
  而 `text/event-stream` 那种不结束的流**根本回不来**。现在挂上接收口就是逐批交付：每批交完之前不去读
  下一批通路字节，背压因此落在接收口自己身上（TCP 窗口与流控自己闭合）。四条口径：①设了接收口，交回的
  `response->body` 为空、字节都在批次里，`isLastBatch` 为真才算走到头，中途失败不再交最后一批（成败看
  返回的响应是否为空）；②返回 false 就是主动收口，头部仍完整交回，而这条连接**当场关掉**——剩下的正文
  留在通路里，还池会让下一条请求把它当自己的响应头读；③挂接收口时本端不再代加 `Accept-Encoding`、也不做
  透明解压（交出去的必须是对端发的原样字节，半截压缩流解不开）；④收口的形状按通路而分：h2 与 h3
  返回 false 只结那一条流（RFC 7540 §5.3.2、RFC 9114 §4.4），连接与链路留在池里给下一条用；
  HTTP/1.1 没有「流」可结，只能当场关掉这条连接、不还池——剩下的正文留在通路上，还池会让下一条
  请求把它当自己的响应头读。
  `SseEventParser` 按 WHATWG §9.2.6 把任意字节边界上的流还原成事件（CRLF / LF / 单独 CR 三种分隔符、
  冒号后一个空格属于分隔符、多条 `data` 按换行拼接、注释与未知字段忽略、含 NUL 的 `id` 与非数字的 `retry`
  整条忽略），收线时把没有空行收尾的最后一条补交出来。端到端那条用的是自家 `SseStream` 写出来的流，
  写侧与读侧对同一份格式的理解因此被同一条用例钉住。

- **出站 URL 必须有协议名，缺了不再静默按 http 处理**（`Net::parseUrl`，**含破坏性变更**）。
  `127.0.0.1:8080/x`、`api.example.com` 这类写法此前能拆出「主机 + 端口」，代价是 scheme 落在默认值上：
  想连 TLS 端口的地址被明文发出去，而且失败长得跟「连上了没人答」一样难分。现在一律抛
  `InvalidArgumentException`，原因里写明「这里不替调用方补默认协议：补错一次就是把 TLS 整段绕过去」。
  同一函数对「写错的端口」早已是拒绝而非回落 80，两条口径这次对齐。直接给 `parseUrl` 传裸 `host:port`
  的调用方要改成 `http(s)://…`；给 `HttpClient::get/post/send` 传完整 URL 的用法不受影响。

- **`Http3ClientConnection::isHealthy()` 从此把「流号余量」算进健康**：一句「还能不能提请求」的问答
  必须包含这件事——客户端流号严格递增，名额用尽之后没有合法的新号可提（RFC 9000 §2.1），这条链路
  只会回「请换一条连接重来」。出站池读的正是这个判定，此前它只看连接层有没有断，于是流号用尽的链路
  会一直留在待命表里，每次取用都先撞一次失败，调用方看到的只是莫名变慢。同时给它补了直测：
  `maximumOpenedStreamCount` 此前是一个零直测的配置项。

- **`InetAddress::ip()` 在带作用域的 IPv6 地址上输出「%接口号」后缀**，`toString()` 随之给出
  `[fe80::1%3]:1234`。旧行为是只报地址本体（`inet_ntop` 本就不输出作用域），后果很具体：两块网卡上
  同名的 `fe80::` 对端在文本面撞成同一个串，而拿 `ip()` 当键的消费方（`PerIpConnectionLimiter` 的
  来源限额、QUIC 的对端键）会把两个来源并成一个。数字形式是 RFC 4007 §11 认可的写法，也能被本层
  `parseLiteral()` 原样读回，所以这份文本仍然解析得动；输出数字而不是接口名，是因为 `ip()` 在日志与
  指标路径上，每次换成名字都要多一次系统调用，而名字本身是另一份本机状态。
  `InetAddress::resolve()` 同步改为先走本层字面量解析：`"fe80::1%5"` 不再交给 `getaddrinfo`——两平台对
  「带 %作用域的字面量是否受理、接口名查不到时报哪个码」判定不一致，而调用方已经把地址写在脸上。
  `AsyncResolver` 里那份自己抄写的字面量解析（只认无作用域的写法）随之删除，两条入口共用同一判据。

- **破坏性变更：`Router::hasStreamingRoute()` 多了一个 Host 参数**。这个入口是给会话在「头部收齐、
  正文还在路上」时判派发时机用的，而按 Host 选站之后，同一条 `(方法, 路径)` 在不同站点可以一个是
  流式注册、一个是普通注册：只看方法与 URI 就答不对了。漏传 Host 的后果不是报错而是行为退化——
  主机表上的流式路由被判成普通路由，会话等整份正文收齐才派发，而处理函数只读 `bodyStream()`，
  那条请求的正文就没人收。三条通道（h1 用 `HttpParser::host()` 的解析进度读数、h2/h3 用请求上已
  补齐的 host）都改了调用点；直接调用这个入口的使用方需要一并补上第三个实参，确实没有主机信息时
  传空视图，语义是「按默认站点判」。

- **时限看门狗从 `Net` 上收到 `Core`**：`Net::RequestDeadlineGuard`（含 `watchRequestDeadline()`）改名为
  `Core::DeadlineGuard` / `Core::watchDeadline()`，头文件落到 `Core/Coroutine/DeadlineGuard.h`，
  `Net/Http/Client/RequestDeadlineGuard.h` 删除。它只依赖事件循环、定时器与「被监视对象有 `close()`」
  这一条，本就不是 HTTP 的东西；出站连接竞赛要给每条候选挂一只看门狗，正好落在 `Core` 这一层。
  行为与调用方式不变，只是名字与位置——留两份一样的看门狗是更糟的选择。

- **包路线只留 Conan**：根 `vcpkg.json`、自带端口与版本注册表（`packaging/vcpkg/`）、以及为 vcpkg
  的 brotli 端口导出名兜底的 `cmake/Findbrotli.cmake` 一并删除。留两条清单等于把同一批依赖记在两个
  地方，改依赖时漏改一边就悄悄分叉，而 CI 只看 Conan 这一条——没有闸门看的第二清单不如没有。
  依赖事实只有 `conandata.yml` 一份；`brotli::brotli` 由 Conan 直接给出，不再需要退化路径。

- **出站请求的时限现在也管住 TCP 连接那一段**：`HttpClient` 的 `requestTimeout` 过去只覆盖握手、发送、
  收完响应三段，连接那一步交给内核的 SYN 重试兜底。这在 Windows 上是死等：完成端口后端把「可写」当作
  连接完成的信号，而连一个没人听的端口时，系统 2 秒内就把连接拒了、却再没有可写事件把挂在上面的协程
  叫醒（实测：连刚关掉的监听端口，调用方的协程无限期停在那里，5 秒的时限到了也不回）。现在这一段同样
  由本层的看门狗掐，到点即失败并给出「建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时」。只有域名
  解析仍在时限之外——那一步在解析器的工作线程上，超时语义由系统解析器定。缺省时限仍是 30 秒。
- **移除 nghttp3：h3 会话用例的对端改成测试自己按 RFC 排字节，跨实现判定集中到 `scripts/h3_cross_check.sh`**：
  这层第三方实现此前只被测试链接，当「独立客户端」与自研服务端对打（42 条用例经它的帧与 QPACK 编解码往返）。
  同一个理由它也得走：链接进来的裁判与被测代码同属一次构建，改实现的人顺手就能把裁判改松，双方共享的误解
  在这里照不出来。现在测试侧的对端 `Http3ClientPeer` 自己按 RFC 9114 §6.2/§7.2 排帧、并在 SETTINGS 里通告
  QPACK 动态表容量 0（服务端因此只用静态表与字面量作答），头块与响应借用本层助手编解——**于是树内那 42 条
  判的是会话状态机与业务映射，不再自称跨实现**。字节合不合规范改由进程外的 aioquic 判：新脚本起一份
  `echo_server --h3 --compress`，跑 8 条场景（GET 逐条核 `content-type`/`content-length=2`/`date`、HEAD 带长度
  但不带正文、SSE 分趟到达且不带长度、大正文跨帧拼回、声明 gzip 就得真压上并带 `vary`、扩展 CONNECT 隧道
  回显），CI 的 `http3-acceptance` 作业由原来两条内联调用换成这份脚本。`h3_acceptance.py` 为此新增
  `--head`/`--stream`/`--expect-header` 三种判据；HEAD 场景上免去 aioquic 那处长度核对——它的编解码器不记
  请求方法，会把合规的 HEAD 响应判成「缺正文」而作废连接（RFC 9110 §9.3.2 允许），线上长度与 content-length
  是否同源改由探针自己核一次，两条路径不能一条严一条松。README 依赖表与 `conandata.yml` 同步少一项。
- **移除 vendored 的 ngtcp2，QUIC 的跨实现验收改由进程外的独立实现裁定**：这份第三方库此前只被测试
  链接，用来搭一个「独立客户端」与自研服务端对打。放在同一棵树里有两类改不掉的毛病——双方共享的误解
  在这里露不出来，而改实现的人顺手就能把裁判改松。现在被测端换成 `tests/Tools/QuicProbeServer.cpp`
  （把握手、ALPN、流收尾、drain、单来源限额、半开连接的回收打成 stdout 事件行），判据由 aioquic 的
  解析器给出：`scripts/quic_cross_check.sh <构建目录> <带 aioquic 的 python>`，Linux CI 已接进
  `http3-acceptance` 作业。树内只保留不需要对端的两条构造期判据（票据密钥与证书校验的抛出形态、
  TLS 上下文不泄漏）。对使用者有一处可见影响：库包少了一份第三方源码，以及它自带的 OpenSSL 版本门槛
  （那份 vendored CMake 要求 3.5+/quictls 才认 QUIC 接口，配套的配置期 FATAL_ERROR 随它一起消失）。
- **出站请求的四处收紧**（都在 `HttpClient` 与 `Http2ClientConnection` 这一层）：
  ① `Host` 与 `:authority` 现在带上**不等于协议默认端口**的那个端口——连到哪个端口一直由 URL 决定，
  变的是这一串字符：按 Host 分站点的对端此前会把 8443 上的服务认成默认站点（RFC 9110 §7.2、
  RFC 9113 §8.3.1 同一条法）。② 请求已经整个写上通路而对端没答完时**不再重发**：过去只看「有没有
  收到过字节」，把这一种也当成空闲期被对端收掉的连接重来一次，于是非幂等请求被悄悄做两遍；现在
  「发出过 / 收到过」两位一起看，发出过就直接判失败。③ HTTP/2 上**没等到 END_STREAM 的响应不再算
  收齐**：状态码解出来了但流没走完（对端半路收口、被请求时限掐掉）时，过去会把 200 连着半截正文
  当成一次成功出站交出去；现在算失败，但保留已解出的状态码与正文，排查时看得出是截断而不是没收到。
  ④ `HttpOutboundConnectionPool::Config::maximumIdlePerEndpoint` 填 0 的含义定死为「不池化」
  （用完当场收口）——填 0 原先落在一条空表 erase 的未定义行为上。
- **出站 HTTP/2 不再接受「响应收齐之后」的帧**：一条流双向 END_STREAM 之后就进入 §5.1 的「closed」态，
  对端再发 DATA 或 HEADERS 都是违规。过去出站侧把后到的 DATA 当正文续段接上、把后到的 HEADERS 当尾部
  头块接上——调用方拿到的正文比流上宣告过的长，头部名单里也可能多出对端塞进来的字段，而本端一处都不
  报错。现在按连接错误 `STREAM_CLOSED` 收口并交代一条 GOAWAY，与入站侧同一条法。真正的尾部头块
  （trailers）不受影响：它本来就带在自己那段 END_STREAM 之前。
- **HTTP/2 客户端不再在流号见顶后继续提流**：`Http2ClientConnection::Config::maximumOpenedStreamCount`
  的合法区间是 `1..2^30`（RFC 7540 §5.1.1 给客户端流号的上界），填 0 或更大都在构造期抛
  `InvalidArgumentException`。到界之后的请求当场被拒回（一个字节都没发出，故调用方按「可以重来一次」
  处理，换一条连接即可），连接会在最后一条流收齐时交代一条 NO_ERROR 的 GOAWAY 主动退场。

- **破坏性变更：`WorkerSupervisor` 的「配置不成立」改走用法错误那条异常分支**。可执行文件路径为空、
  `workerCount < 2`、轮询间隔或收尾期限不大于 0、本平台不支持多进程（Windows 缺 `SO_REUSEPORT`）这四类拒绝
  此前抛 `CoreException`（运行期故障链），而构造函数的 `@throws` 一直承诺的是 `Base::LogicException`——代码
  与自己的文档相反。留在运行期链上的代价是让调用方把「自己配置填错了」当成可重试的故障。迁移：原先
  `catch (Base::Exception)` / `catch (CoreException)` 兜这几条的代码改捕 `Base::LogicException`（或直接
  `catch (const std::logic_error &)`，它与 `InvalidArgumentException` 同支）；真正的运行期故障（拉起进程失败等）
  仍走 `CoreException`，不受影响。
- **破坏性变更：`AsyncSocket` 的「参数取值非法」改走用法错误那条异常分支**。单次收发长度超 `INT_MAX`、
  聚合发送段数为 0 或超平台上限、零拷贝发送的源描述符非法与待发字节数为 0，此前都抛 `Base::SystemException`
  （运行期故障链），而可替换的另一半 `TlsSocket` 早就在抛 `Base::InvalidArgumentException`
  （`std::invalid_argument` 分支）。`HttpSession` 用 `requires` 探测能力后把两者当同一种东西用，同一条误用
  走两条分支的后果是：只 `catch (Base::Exception)` 的调用点会把用法错误当成可恢复故障再试一遍，而按运行期
  故障统计的出口让真正的坏参数在指标里隐身。迁移：原先按 `Base::SystemException` 捕获这几条拒绝的代码，
  改成捕获 `Base::InvalidArgumentException`，或直接 `catch (const std::logic_error &)`（与 `InvalidArgumentException`、
  `LogicException` 同支）。正常收发行为不变，仓库内没有任何调用点会命中这几条守卫——传下去的长度都来自固定
  缓冲区大小。对端重置、等待期间套接字被关、地址取不到、文件比声称的长度短这类**运行期**故障仍留在
  `SystemException` 上：它们的真值要到内核执行的那一刻才知道，重试有可能变好。
- **破坏性变更：移除 `Base::splitKey()`（点分键拆分工具）**。配置系统按「扁平键 + 前缀匹配」工作
  （`get`/`getSection`/`validateRegisteredSchema` 都不切分键），这个工具在本仓库里没有任何调用方，
  却带着一条会咬人的语义：连续分隔符与首尾分隔符产生的空片段被**静默丢弃**，于是 `"a..b"`、`".a"`、
  `"a."` 全都归并成同一个路径——谁将来拿它去做嵌套查找，就把三种写错的键当成了同一个。迁移：
  需要按段处理自己用 `std::ranges` 切；要读一段配置用 `getSection(prefix)`，要取单个值直接用点分键 `get(key)`。
  同时删掉它带在 `ConfigValueType.h` 里的 `<vector>` 依赖与该文件的 9 条用例（`TestBase` 577 → 568 例）。

- **破坏性变更：`Http2Request` 的普通头部改用 `HttpHeaderFieldStore`（与 `HttpRequest` 同一套存储）**。
  字段类型由 `std::vector<HpackHeaderField>` 换成 `HttpHeaderFieldStore`，随之移除只在向量形状上成立的
  `findHeaderValue()`（返回 `const std::string *`，而新存储把名值写在同一条字节缓冲里、只记偏移，
  没有可交出去的 `std::string`）。迁移：按名取值写
  `requests[i].headerFields.firstValueView("x-request-id")`，返回 `std::optional<std::string_view>`，
  语义就是原来那条「按到达顺序取第一条命中」；整表遍历写
  `requests[i].headerFields.forEachField([](std::string_view name, std::string_view value) { ... })`。
  读到的内容、顺序与「可重复头部不合并」都不变，两点差别是取值返回视图而非指针、比较大小写不敏感。
  为什么值得动契约：h2 每条请求原先要把头部装两份 owning 串（HPACK 解出来一份、请求里再逐条抄一份），
  接线层 `mapToHttpRequest()` 还要抄第三份；现在请求里这块存储由 `adoptStagedHeaders()` 整块换给
  `HttpRequest`，第三份不复存在，`:path` 与 uri 之间也改成 `adoptStagedUri()` 的缓冲交换。配套给
  `HttpHeaderFieldStore` 加了 `reserve(fieldCount, byteCount)`：装配前就知道有几条、多少字节的地方
  一次留够，不必让记录表与字节缓冲各自按倍扩容。实测同一条 7 头部的 GET，收方向的分配从
  18 次 / 2824 字节降到 15 次 / 1621 字节（Release，按一千次原值计，判据见
  `tests/Net/Http/TestHotPathAllocations.cpp`）。
- **破坏性变更：`LogEvent::timestamp` 由文本改成时刻**。字段类型从 `std::string` 换成
  `TimestampMoment`（即 `std::chrono::system_clock::time_point`），配套的 `currentTimestamp()` 一并
  移除——事件只带时刻，文本由各格式化器在 Sink 写入线程上渲染。迁移：自建 `LogFormatter` 的实现里
  把 `event.timestamp` 换成
  `std::array<char, kTimestampTextBufferSize> buffer{}; const auto text = formatTimestampText(buffer, event.timestamp);`
  再把这个视图拼进自己的版式；输出的文本形态、字段顺序与本地时区口径都不变，`"YYYY-MM-DD HH:MM:SS.mmm"`
  还是那一串。之所以值得改契约而不是留一份兼容文本：产生日志的线程常常就是事件循环线程，
  让它为每行日志取一块堆是不划算的（见「性能」那条）。
- **破坏性变更：`WorkerSupervisor::run()` 回报「这次编排算不算成了」**。签名由 `void` 变成
  `[[nodiscard]] bool`：true 是按停止请求收口，false 是整池 worker 都因「起来就崩」被放弃而提前退出。
  此前两种收场都不留痕迹，`echo_server --workers N` 在整池起不来时仍以退出码 0 结束，只看退出码的
  进程管理器与脚本分不出来。迁移：不关心结果的调用点写 `static_cast<void>(supervisor.run());`，
  关心的调用点拿返回值决定退出码。仓库内 5 处调用点已跟着改——`samples/main.cpp` 据此报「服务未运行」
  并以 1 退出，`samples/core_worker` 的两段各断言一种收场，`TestWorkerSupervisor` 三条用例改成
  对返回值断言。
- **破坏性变更：`AsyncExecutor` 从 Database 迁到 Core**。头文件 `Database/Pool/AsyncExecutor.h` →
  `Core/Coroutine/AsyncExecutor.h`，类型 `AsynGyanis::Database::AsyncExecutor` →
  `AsynGyanis::Core::AsyncExecutor`；连带 `Queryable::useAsyncExecutor()` 的形参类型与
  `Queryable::asyncExecutor()` 的返回类型一起换了限定名。迁移只需改 include 路径与限定名：
  `submit()` 的签名、提前丢弃 Task 时的作废语义与「恢复落在指定循环」的契约都没动。
  搬动的原因是它是通用基础设施（阻塞式驱动与整块 CPU 活都要用），留在 Database 里就只有那一侧能受益，
  而让 `Net` 反向依赖 `Database` 不合本项目层次。执行器抛出「已停止」时的异常类型也从
  `DatabaseException` 换成 `Base::LogicException`——那是调用方的用法错误，按规范属于 `logic_error` 分支。
- **示例结论行分开「执行过的步」与「因环境不齐备跳过的步」**：`RESULT <名字> PASS|FAIL <步数>`
  现在是 `RESULT <名字> PASS|FAIL <步数> gated <跳过数>`。原先跳过的步什么都不留，于是无凭据的
  机器与有凭据的机器给出的是同一条结论行，`run_samples.py --repeat` 的步数一致性检查也就无从区分；
  现在矩阵多一列门控数，且同一示例在重复运行里门控数抖动同样算失败。自己解析结论行的脚本要跟着
  认这个新字段（缺省按旧格式解析仍可通过：`gated` 是可选段）。
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

- **Windows 上多进程编排那句拒绝语把去路一并说出来**。原文写的是「Windows 不支持多进程 worker 模型」，
  读起来像「Windows 跑不了多进程」，而实际不成立的只是本类这套「每个 worker 各自 bind 同一端口」的内核分摊。
  现在这句话点明缺的是 `SO_REUSEPORT` 的等价物，并给出当下可用的形状（`workers=1` + 多工作循环 + 接受分发）；
  类文档另记一条：Windows 上缺的是这套编排而不是交接通道——master 独占监听、经 `Core::UpgradeChannel`
  逐个移交那份套接字的原语已在库里（零停机换代在用），按这个形状编排 worker 还需另设计，本类未实现。

### 修复

- **ACME 的每条请求都带上客户端标识**（`Net::Acme::AcmeClient`）：RFC 8555 §6.1 要求每条请求带
  `User-Agent`，Boulder 与 Pebble 对缺它的请求一律回 400 `malformed`（Pebble 的原文是 "All requests MUST
  include a User-Agent header"）。此前取目录那一步就没带，等于这台客户端对着真机构**第一步就断**；
  而进程内的桩不看这条头，所以自研那一侧一路全绿——这是跨实现验收才浮得出来的形状。现在目录 GET、
  newNonce 的 HEAD 与每一条签名 POST 都过同一个入口补上（漏一处就等于那条通路用不了，故不各处写一遍）；
  桩搬来同一条判据，用例钉住「一轮完整流程里没有任何一条请求缺标识」。
  发现途径是 `scripts/acme_pebble_cross_check.sh`（对面换成 Pebble，另一套独立实现）。

- **机构复用已 valid 的授权时，不再去触发那条挑战**（`Net::Acme::AcmeCertificateManager`）：续期时机构常
  把上一轮做好的授权直接挂进新订单（Boulder 会，Pebble 默认按概率会），而对状态已是 valid 的挑战再发一次
  触发不是幂等，是 400「Cannot update challenge with status valid, only status pending」。原实现每一轮都
  无条件挂令牌并 POST，于是续期会稳定失败在自证那一步——同一份发现来自上面那条 Pebble 验收的第二轮。
  现在授权本身是 valid 就跳过这一格自证（不挂令牌也不 POST），桩补上「复用授权 + 拒绝对 valid 挑战再触发」
  两档把这条出口钉住：用例断言复用那一轮令牌一次都没被再取走、也没有一条对 valid 挑战的触发。

- **一条坏投递不再打死整条事件循环**（`Core::Scheduler` / `Core::EventLoop::run()` /
  工作线程与若干后台线程入口）：抛出处在派发层、兜底却在循环层——`run()` 的 try 罩住整轮迭代，
  捕获后重抛；`ThreadPool` 的线程体接住、记一条 ERROR、然后返回。jthread 没有重启函数，于是
  `threadCount()` 照样报原数而那条循环再也不驱动任何东西：连接仍挂在活的 epoll 里、没人读，
  连负责收空闲连接的清扫协程也在同一条死循环上。`run()` 的注释说这是「快速失败，由
  WorkerSupervisor 重启 worker」，但那台 supervisor 编排的是 worker **进程**（fork+exec），
  线程池的工作线程不在它监管里——设计假设与部署形态不一致。现在守卫下移到派发单元
  （`Scheduler` 12 处「执行别人投进来的东西」统一到同一个 `runGuarded`，全部跑完不丢同批其余；
  `IoWatcher::handleEvents` 逐侧兜住），`run()` 不再向调用方抛——它 global 有 20 多处直接跑在
  裸 `std::thread` 入口上（含 samples 与测试夹具），让异常从那里穿出去就是 `std::terminate`。
  顺带修掉同函数内的自我矛盾：`runAll()` 第一阶段的本地排空完全没守卫，而第二趟同一份本地排空
  是守卫住的——同一条循环因此对本地投递与跨线程投递有两种失败语义。
- **异常不再穿过 C 边界与 `noexcept` 边界**：OpenSSL 的 SNI 回调（按 ClientHello 里的
  server_name 换站点上下文）此前非 `noexcept`、体内既分配站点键字符串又构造 `shared_lock`，
  而输入是远端可控的——栈从那里展开要穿过 C 帧，是未定义行为而不是「异常被上层接住」；现在整份
  实现按 `noexcept` 写死并自带兜底，失败时交回默认证书（与「没带 SNI」两条出口同一语义）。
  同形的三处 `noexcept` 边界（`IoWatcher::handleEvents` 的裸 `resume()`、连接池
  `expireTimedOutWaiters`/`ResumeTicket::resumeOnce` 的裸恢复、MySQL 语句缓存的 `try_emplace`）
  各自补上守卫；其中 `cacheStatement` 没接管时原先会漏掉那条已 prepare 的语句句柄，改为回报接管
  结果并由调用方的作用域兜底收尾（在函数内直接 close 就是用后释放——调用方手里那份还要继续绑定执行）。
  另外修掉两处随行的形状问题：`expireTimedOutWaiters` 用两条平行向量分次 `push_back`，中间一次
  分配失败会让长度错位、而投递按 tickets.size() 索引 loops 是越界读；`AsyncResolver` 的分离线程体
  原先 `freeaddrinfo` 与唤醒都在抛出点之后，漏唤醒就是等待方永久挂起。
- **业务处理器抛异常时三条协议都不再静默**：`HttpSession`/`Http2Session`/`Http3Session` 里
  「头部已上线改不了状态码」那条流式分支一直有日志，而最常走的**非流式**分支是
  `response.reset()` + 500 之后直接往下走，服务端一句成因都不留——对端只该看到 500（不外泄内部
  原因是对的），但运维侧连「是哪一句抛了、抛在哪」都查不到，异常携带的抛出点调用栈也就白采。
  现在同一行带上 request-id、trace id、路径与原因，出错的一行能翻回那条链路。响应行为一字未改。
- **三处丢原因的静默吞补上告状**：连接池创建连接失败原先 `catch(...)` 直接 `return nullptr`
  （驱动报的鉴权失败/主机不可达/URL 写错是这里唯一持有过原因的地方）、探活查询抛出无声按
  不可用处理、TLS 建虚拟主机上下文失败丢掉具体成因。
- **日志文件打开失败改走框架捕获面**（`Base::Log::FileSink` 构造）：此前抛裸
  `std::runtime_error`，恰好从框架对外承诺的 `catch (const Base::Exception &)` 那个面上漏出去，
  调用方按承诺写的一句 catch 接不到；改抛 `Base::SystemException` 并带上 errno。

- **h3 的在线连接数拿到循环外面来读是一次数据竞争**（`Net::QuicServer::connectionCount()` 与走它的
  `stats()`）：那张连接表只归事件循环线程，而采集端与探活工具都在别的线程上读这个数——外部裁判实测到
  一边在读红黑树的计数、一边收包路径正在摘除已收口的连接，动的正是同一个 8 字节位置。声明处此前写的是
  「加锁读的近似值」，而实现里既没有锁也没有原子量。现在按 h1/h2 那一侧的同一条口径办：表增删的两处各带
  一条 relaxed 加减，读数取自这份原子镜像——「这一刻大约多少条在册」这条承诺因此是实的（最多滞后一次
  变更），表本体依旧只在循环线程上被碰。一条用例把两侧都钉住：登记刷不上非零、摘除回不到零各判一次。

- **`Set-Cookie: sid="abc123"` 这类引号包起来的取值过去整条不见了**（`Net::HttpCookie` 的两条解析入口）：
  RFC 6265 §4.1.1 允许取值写成 quoted-string（值里带分隔符时包引号是第二种合法形状，Jetty 与一些 CDN
  就这么发），而本层先验字符、后看形状——引号本身不是合法的裸取值字符，于是 `parseSetCookie` 直接返回空、
  `parseCookieHeader` 跳过那一条。调用方看到的是一声不响的丢失：客户端侧「Set-Cookie 收到了、jar 里却没有」，
  宿主那边就是登录态反复失效。现在两条入口都先按 §5.1.2 解开引号（含 `\'` 式转义：反斜杠不留在值里，
  后一个字符照字面收），再拿裸取值的字符规则过一遍。
  **刻意的拒绝面**（写在文档里）：解开之后仍含裸写法容不下的字符（空格、引号）时整条不收——本层不保存
  需要转义才发得出去的值，那要求渲染侧再包一层引号，而多数对端不解。单侧引号不算 quoted-string，
  内层没转义的引号也走同一道字符闸门（不在解析里特判）。
  用例两条：`UnwrapsQuotedStringValueAndKeepsBackslashEscapes`（含属性段不受影响、`"a\\b"` 读成 `a\b`、
  `"a\b"` 读成 `ab`、空引号读成空串）与 `UnwrapsQuotedValuesInRequestCookieHeader`（两条入口对同一形状
  给同一个答案）。证伪（各红各的断言）：整个解开动作去掉 → 两条红在主断言（`sid="abc123"` 直接不收）；
  把转义还原摘掉 → 红在三处转义断言（读出 `"a\\\\b"` 这类带反斜杠的值）。

- **Cookie 的报错文案与说明把「=」写成了被排除的字符，而两端都允许它**（`Net::HttpCookie`）：
  `isValidValue` 按 RFC 6265 §4.1.1 的 cookie-octet 放行 0x2D..0x3A 与 0x3C..0x7E，'=' 就在里面；
  两条解析入口也都只按**第一个** '=' 切分，取值里再出现的 '=' 原样保留。构造函数那句「不能含空格、
  控制符与 \" ; = ,」因此会把调用方支去净化一条本来就合法的 Cookie 取值。反方向也不齐：
  `parseCookieHeader` 的说明写着「带空格的取值按原样收下」，而空格实际被 `isValidValue` 判不合格、
  **那一条**会被跳过（其余各条照常交出来）。两处都按实现改齐，并把一处刻意的放宽写进文档：0x5C
  （反斜杠）规范不让而本层放行——它不动任何头部结构，挡下来只会让一条真实存在的 Cookie 存不进来。
  `isValidName` / `isValidValue` 这一对公开谓词此前零直测，现在按字符表逐字节钉住（名字 = RFC 7230
  的 tchar 全集、含空名字必须拒；取值 = cookie-octet 的四段，CR/LF、'"'、';'、','、空格、控制符、
  DEL 与 0x80 以上逐个点名挡下），Cookie 头的 '=' 与空格语义各补一条断言。
  证伪（三处互不重叠，同一趟构建 25 条里只红这 3 条）：往实现的 token 表里加 ';' → 名字那条红在
  「非 token 字符被放了进来」；把 0x5C 按 RFC 排掉 → 取值那条红在区间循环；改成按最后一个 '=' 切分 →
  解析那条红在取值断言。

- **一台监听器只能启动一次（`Net::UdpServer` 与 `Net::QuicServer` 的重复 `listen()` 现在会被拒）**：过去
  对一台已在监听的服务端再调一次 `listen(地址)`，它会安静地再绑一个端口并把 `m_socket` 换成新的一份，而
  收循环（QUIC 还有一条定时器协程）仍挂在旧的那份上——旧对象一被销毁，原先那个端口就再没人读。两侧实测
  的形态都是「端口换了、原服务不再应答」，Windows 与容器 ASan 下都没有报 use-after-free，也就是说这台
  监听器看起来还在服务。现在两条 `listen()` 入口共用一份 `requireFreshStart()`：端口非 0 就抛出并点名
  是哪个端口，要换端口请新建一台。接手模式下那一次重复的无参 `listen()` 也归这条判据管——它过去的报错是
  「这台服务端不是接手构造出来的」，把调用方的重复启动说成了配置错误。
  用例两条（`UdpServer.RejectsASecondListenWhileAlreadyServing`、
  `QuicServer.RejectsASecondListenWhileAlreadyServing`）各钉两侧：第二次启动必须被拒、端口不许变、
  原服务照常应答。证伪（两侧都跑过）：摘掉那四处 `requireFreshStart()` 调用，UDP 那条红在三处（没被拒、
  端口从 56714 变成新绑的一个、随后的往返拿不到应答），QUIC 那条红在两处。
  顺带把两处文档按实现改齐：`QuicServer::stop()` **不**关套接字，只置停止标记（它可能从别的线程被调用，
  而套接字与注册对象只归所属循环线程），要真正把服务停下来得停止循环或在循环线程上销毁本服务端——旧文档
  写着「关掉套接字让收循环退出」，照着写的调用方会以为调一次 `stop()` 端口就释放了；`UdpServer` 那两条
  `listen()` 的说明也注明「抛出发生在协程首次恢复时」，惰性协程不是调用当场。

- **数据报接管把「句柄不是套接字」单列一档**（`Platform::DatagramSocket::adopt`）：过去问不出 `SO_TYPE`
  与问出来不是 `SOCK_DGRAM` 合成同一个 `not_supported`。前者多半是调用方把一枚文件描述符当成移交结果
  用了，下一步要换传进来的东西；后者是交出方送错了类型，下一步要查对面那一格。并档还会把前者读成「本平台
  不支持接管」而去换平台。现在前者报 `std::errc::not_a_socket`（新增判据常量 `PlatformError::kNotASocket`
  负责跨平台的错误码比对），问不出别的成因时照原样交出平台码。
  用例一条（`DatagramSocket.ReportsADistinctCauseWhenTheHandleIsNotASocket`，只在 POSIX 上判：Windows 的
  整数句柄空间里造不出这一档，普通文件的 CRT 描述符与 SOCKET 不同域，而本层 `close()` 走 `closesocket`）。
  证伪：把两档并回一个分支，本条红（读出 `not_supported` 而期望 `not_a_socket`）。

- **换代交接通道的三处失败读数不再拿残值**（`Platform::Socket` 的移交读写与有界等待）：
  ①「对端把通道关了」过去会被报成任意别的成因——Windows 侧的逐段读写把 `recv`/`send` 返回 0 当成失败
  直接交出槽位里的值，而 0 是一次**成功**的调用，实测它会把 last-error 清成 0，于是交出去的原因是
  「没有错误」；POSIX 侧 `recvmsg` 返回 0 会落进「头没收全」那一档，报出来像格式错。两侧现在都给
  `PlatformError::kConnectionReset`（新常量，Windows 取 WSAECONNRESET、POSIX 取 ECONNRESET）。
  读这一侧的是编排线程，它照报出的码选下一步：看成格式错会去重发一条永远收不到的消息，看成连接被复位
  才知道新一代根本没起来。
  ②预算为 0 的 `waitForAcceptReady()` 过去可能一次都不 poll 就报「没人来连」：判定用的是两次取时刻之差，
  只要这两句之间被抢占满一毫秒，截断出来的剩余就是负数——而 0 预算的语义正是「只取当前状态」。现在
  「预算用尽」的结论排在至少取样一次之后，负剩余一律夹到零超时。
  ③POSIX 上通道文件过去直接写在临时目录里、靠 `umask` 收紧权限：那个调用是**进程级**的，会把同一时刻
  别的线程新建的文件（日志、临时文件、票据密钥）权限一并改掉。现在通道文件放在一个 `mkdtemp` 出来的
  0700 私有目录里，门禁落在目录上，收尾连目录一起删掉（不再一代堆积一个空目录），而固定路径带来的
  「先 unlink 再 bind」也随之作废——目录是新的，撞不上上一次运行的残留。
  用例三条：先污染错误码槽位再制造通道 EOF，断言报的是「连接被复位」（Windows 侧把那两处 0 字节的出口
  改回「照抄槽位」即红，实测读出来的是 0 而期望 10054；POSIX 侧删掉 `recvmsg` 返回 0 那一支也红在同
  一句，读出 22（EINVAL）而期望 104）；队列里已有连接时零预算取样必须报「有」，紧接着的 `accept` 真拿
  得到（把「预算用尽」的判定排回取样之前即红，且只红在这一句）；通道目录对组内与其他人三位全空、收通道
  后目录不再存在（把目录换成临时目录里的固定名字即红在前半——权限读出 0o55，而 `UpgradeChannel` 那条
  「两条通道地址互不相同」同时红；只删文件不删目录即红在后半）。

- **关掉出站 QUIC 连接时把套接字一起关掉**（`QuicClientConnection::close()`）：原先它只置两个标记，
  而叫醒「挂在数据报读上等下一条报文」的协程靠的是销毁注册对象——关掉描述符本身不做这件事（同一个
  理由写在 `AsyncUdpSocket::close()` 与本源文件的析构里）。本类又不自带后台协程，于是对端一旦不再
  说话，那条等待就没人收得回来：请求时限到点也只是把连接判死，等它的协程仍旧停在那里。实测过一次：
  满载并行跑全量时，一条 h3 出站请求挂在读上停过 25 秒不返回，而那段时间对端确实没再发任何东西。
  叫醒机制本身由新用例 `AsyncUdpSocket.CloseWakesCoroutineBlockedOnReceive` 钉住（全程没人发报文，
  因此「它醒了」只可能来自 close()；把 `m_watcher.reset()` 摘掉本条红，实测如此）。至于出站连接这一处
  调用，回环环境钉不出它——任何一条入站报文都算一次叫醒，把这两行摘掉现有用例照样绿，所以这里如实
  记下而不假称有用例。另给 h3 出站补上「对端收了请求不答话时限必须收回等待」那条直测
  （`Http3ClientConnection.ReclaimsTheRequestWhenTheDeadlinePassesWhileThePeerIsSilent`，摘掉
  `request()` 里的 `DeadlineGuard` 它会挂满 8 秒变红）——此前这条通路对「请求时限到点」零直测。

- **HTTP/3 上大于一个接收窗口的正文两头都走不通**：症状是「小响应全通、大响应全等满时限」——实测
  一条 600 KiB 的 h3 响应，出站客户端**一个正文字节都拿不到**。两处各是一半：
  ①发送侧把整段正文编成了**一条** DATA 帧，而接收方要凑齐一整帧才交货、才归还额度，一帧比对方肯给的
  流窗口（本端自己宣告的是每条 256 KiB）还大就永远凑不齐，于是「本端等对端发、对端等本端还窗口」。
  现在按 16 KiB 拆成多条 DATA 帧（RFC 9114 §4.1.2 里正文本就是若干 DATA 帧的拼接，收尾只跟最后一片），
  每一帧都能在一档窗口里发完，额度按消耗持续推进——这条不只救出站客户端，自家服务端给任何对端发大文件
  此前同样过不去。②出站侧从不归还 DATA 载荷的额度：协议层把这一截明确留给接收方决定何时还
  （`Http3Connection::creditConsumedBytes` 只就地归还非载荷字节），服务端会话那侧照还了，出站这一侧漏了。
  改为「拷进本端缓冲即归还」，越界与已判死那两条出口也一并还账（只还连接级，理由与 h2 那侧同）。
  用例 `Http3ClientConnection.CreditsTheReceiveWindowForBodyBeyondTheAdvertisedStreamWindow` 逐字节
  比对那三百 KiB（大过一档 256 KiB 的流窗口即可）。证伪分两处：摘掉拆帧 → 零字节到手；摘掉归还 →
  攒满一档窗口之后等满时限。

- **`QuicServer::Configuration::maximumConnections` 的 0 现在是不限，不再是「一个也不收」**：判定原先写作
  `在线数 >= 上限`，所以 0 会把每一条新连接都挡在门外，而 HTTP 侧的 `maximumConnections` 一直是 0 = 不限——
  同一份配置在两条服务路径上是两把尺。症状很难自查：监听照常、ALPN 正常协商，日志还会打一句「在线连接已达
  上限 0」，配置者却以为自己设的是「不设限」。默认值仍是 1024（每条 QUIC 在线连接都常驻一份 TLS 会话、流表
  与拥塞状态，而且它没有内核接受队列可挡），但默认值的理由与 0 的口径一起写进了字段说明。
  判定抽成 `admitsNewConnection(在线数, 上限)` 直测：真要把一条连接端到端地拒掉得先跑完一次握手，
  那不该是用例的前提。

- **流式上传（`HttpClientRequest::bodySource`）在 HTTP/2 上不再丢掉 `content-type`**：h2 那一支备附加字段时
  写的是「有整块正文才带媒体类型」（`!request.body.empty() &&`），而流式上传的 `body` 天生为空——于是
  正文一段一段发出去了，媒体类型却没上线，服务端按缺省类型处理这份正文。h1 那一支是两条分支各自判的
  （chunked 与 Content-Length 都带媒体类型），所以同一个请求换个承载协议结果不同。现在 h2 与 h3 共用
  同一份附加字段构造，判据与 h1 对齐（带正文就算，整块与流式都算）；两条承载各钉一条用例，服务端把
  实际收到的 `content-type` 记出来当判据——只钉 h2 不够，共用一份构造之后 h1 的口径也被它管着。

- **出站 HTTP/3 客户端补上响应正文上界（`Http3ClientConnection::Config::maximumResponseBodyBytes`）**：
  出站三条通路里，h1 与 h2 早就共用一份「本端愿意收多大的响应」（`HttpOutboundConnectionPool::Config`，
  默认 8 MiB），而刚并入的 h3 一侧没有这道闸——正文长度由对端发多少 DATA 决定，连它声明的
  content-length 都防不住撒谎的对端，形态是「一个恶意/失常的服务端能让本端一直分配内存直到耗尽」。
  语义与另两条通路对齐：越界时**只结这一条流**（交出 RESET_STREAM 与 STOP_SENDING，连接留给别的请求用），
  失败原因点名该调哪一项（`Config::maximumResponseBodyBytes`，填 0 表示不限），且**不把半份正文交回
  调用方**——那一半连一个完整的字段段都不构成。默认档与两条通路同值，这条由
  `HttpResponseParser` 里那条「三条通路的默认档不许分叉」的用例盯着（此前只盯两条）。
  顺带给 `QuicClientConnection` 补了「收口单条流」的出口（此前只有开流口，越界的这条只能连连接一起放掉）。
  用例 `Http3ClientConnection.CapsResponseBodyAtTheConfiguredLimitAndKeepsTheConnection` 一次验三件：
  越界那次拿到失败结论且不交正文、链路仍然健康、紧随其后的正常请求照答。证伪分两处：摘掉上限判定 →
  前三行红（拿到 200 与 200 KiB 正文）；把越界处置换成「连连接一起判死」→ 后两行红（同连接的下一条
  请求被牵连）。两处红集不重叠，才算各自钉住。

- **Windows 上从源码编译 Platform 直接失败，而报错全在 SDK 头里**：本轮全量格式化把 `Platform/Platform.h`
  里那四行系统包含按字母重排了，`mswsock.h` 就此排到 `winsock2.h` 与 `windows.h` 之前。这几份 SDK 头都
  不自带所需的基础类型——`mswsockdef.h` 里 `ULONG`、`BOOL` 那一套要靠先包含进来的 `winsock2.h`、
  `windows.h` 提供，顺序一乱它自己就解析不了。症状因此不落在我们任何一行源码上：`mswsockdef.h(93):
  error C3646: “Status”: 未知重写说明符` 之类成片冒出，14 个 Platform 源文件一起倒下，看见的人第一反应
  是「SDK 或工具链坏了」，还会去改 SDK 版本。
  两步收口：四行恢复成有约束的书写顺序（`winsock2.h` → `ws2tcpip.h` → `mswsock.h` → `windows.h`），并在
  其上写明这个顺序为什么不能按字母排；`.clang-format` 关掉 `SortIncludes`（原先没写这个键，取的是默认的
  「排」）——clang-format 判断不了「谁必须先被包含」，只恢复顺序不留这条配置，下一次格式化照样排回去。
  门禁（同一份内容）：Windows Debug（MSVC `/W4 /WX` + ASan）全目标零告警、`ctest -j 14` 3202/3202 全绿；
  容器 GCC 13 Debug + ASan/LSan/UBSan 全目标零告警、`ctest -j 8` 3211/3211 全绿（68 条真库用例按环境
  SKIP）、零 sanitizer 命中。

- **一个消失的对端不再带走整台 QUIC 服务器**：`Core::AsyncUdpSocket::asyncReceiveFrom()` 此前把平台层
  报错按硬失败抛出，而抛出的异常落不进正在 await 它的那层——本框架里被调度器恢复的协程抛异常只会被记
  一行「协程有异常没人接住」然后丢弃，`QuicServer::listen()` 的收包循环当场消失。形态是端口还在、进程
  还在、日志里就那一行，此后不再接受任何来源。触发面是远端的：Windows 会在下次读数上把
  `WSAECONNRESET [10054]` 递给套接字，**未连接的也一样递**（一条监听套接字服务所有来源，所以一次
  「对端已经没了」的 `sendto` 换来的 ICMP 回声就能打死全局），实测现场是 `echo_server --quic` 配
  aioquic 客户端的连接轮换。
  契约改成按值交出：`DatagramReceiveResult` 新增 `socketErrorCode`，`receivedByteCount` 为 -1 时用它分
  两种收场——**带码**＝对端不可达那一类回声，套接字本身还好好的，该继续读；**码为 0**＝等待可读期间
  本端被关（`stop()`、握手时限掐断），调用方收手。抛出只留给调用方自己写错的那两类（缓冲为空或容量为 0、
  套接字已被移动走）。两条消费循环（`QuicServer` 的收包循环与 `QuicClientConnection` 的握手/推进循环）
  按这个码分流，带码那一支跳过本轮继续读，并接上一轮那套日志闸门：10 秒窗口放行一条、附上「另有几条同类
  被压掉」（这类错误的发生率由对端控制，逐条落盘既刷日志又把同步写压回热路径）。
  出站一侧的 `pumpOnce()` 两种「没有报文」都直接收手而不判死连接：它一轮只读一条，而这条连接的裁判是
  QUIC 自己的丢包与空闲计时器，不是 ICMP 回声。
  用例 `AsyncUdpSocket.ReportsPeerUnreachableReceiveErrorAsACodeInsteadOfThrowing`：占一个回环端口再关掉，
  发一条过去，先拿 `select` 等那条 ICMP 真递到本端（只问可读、不吃掉错误），判据因此不会在「拿到错误」
  与「暂无数据」之间漂移；断言三件——不抛、报 -1 时必须带码、递回错误不该把本端套接字一起判死。
  证伪：把 `co_return` 改回「按硬失败抛出」，本条立刻红（10054 的文本落进 `failureMessage`）。
  端到端复核同一件事：改前的服务端日志里有那行「协程有异常没人接住」且此后新连接全部无响应，改后没有那一行，
  同一个服务端实例在一个对端消失之后照样给 aioquic 客户端回 200。

- **Windows 上「连接被拒」终于会叫醒等待方：出站连接改走 ConnectEx**：此前 `AsyncSocket::asyncConnect()`
  跟 POSIX 一样是「非阻塞 `connect()` + 等可写」，而 Windows 的完成端口**不会**为连接中的套接字报可写——
  零字节 `WSASend` 探针连投递都上不去（实测 `WSAENOTCONN` 贯穿整个连接期，重试 160 次跨 4 秒一次都没成功），
  于是等待方永远醒不来，只能靠上层看门狗收场：报出来的原因是「等待期间套接字被关闭」，而不是「对端拒绝」。
  现在连接是一次真正的重叠操作：`ConnectEx` 投递（未绑定时先按目标的地址族补一次任意地址绑定——
  实测不绑直接 `WSAEINVAL`），完成通知带着结果回来，错误码用 `WSAGetOverlappedResult` 从完成包里译出来
  （此时 `SO_ERROR` 仍是 0，实测确认，所以不能问内核）。POSIX 分支逐字不动。
  实测口径：回环上连一个刚关掉的监听端口，新路径约 **2.05 秒**按时醒来并交出 `WSAECONNREFUSED`；
  同一场景退回旧写法是 15 秒超时不醒（用例突变验证）。真实远端被拒时完成时刻跟着 RTT 走，收益更大；
  黑洞地址仍由预算收场，不劣化。
  用例 `AsyncSocket.ReportsRefusedConnectThroughTheIoBackend` 判错误身份而不判墙钟毫秒——
  被拒的完成时刻由内核决定，钉毫秒等于赌调度运气。
- **Windows 事件后端被两条线程同时使用时当场抛出，而不是把堆写坏**：`Iocp` 的注册表与三张待办表都是
  无锁容器，而「后端只由它所属事件循环的那条线程碰」此前只是一句文档约定，违约的现场是另一处
  `std::vector` 的控制块被打乱、由 AddressSanitizer 报成 `negative-size-param`——报错位置离肇因隔着几层。
  现在五个公开入口（注册/改关注/注销/取接入套接字/等待）都先抢一个独占标记：**同时在场**才拒
  （同一条线程的重入也算），抛出 `Base::LogicException` 并带上两侧线程指纹与操作名；两条线程**先后**
  使用是既有的良性形态（注册推迟到第一次等待才做，本机实测有 972 处），不受影响。
  同一轮里修掉了真正的违约方：测试助手 `EventLoopThread::runToCompletion` 在调用线程上首启协程，
  而那条路径会就地注册观察者（`AsyncSocket::ensureWatcher()` → `Iocp::addFileDescriptor`），
  此刻循环线程正堵在 `wait()` 里——已改成投给循环线程（见 149a580，同一探针从 24 条并发降到 0）。
  用例 `Epoll.RejectsConcurrentUseFromAnotherThread` 双向钉住：并发要拒、顺序交接要放行。
  证伪：把抛出摘掉，被拒次数从「若干」变 0，用例红。反过来若按「换了线程」判定，本机实测那 972 处
  先后交接会全部被拒——所以判据刻意只取「同时在场」，这一条也写进了用例的第二半段。
- **出站响应正文的上界两条通路都有了这个开关，此前 HTTP/2 一侧根本没有闸**：HTTP/1.1 走
  `HttpResponseParser`，写死 8 MiB 且没地方改——一次 10 MiB 的下载就会失败，报的还是那句笼统的
  「响应不合规范或超出本端上限」；HTTP/2 一侧则完全无界，`Http2ClientConnection` 对每条流的正文缓冲
  来多少收多少，且收完立刻归还流控窗口，于是**对端可以决定本进程分配多少内存**。现在上界是池配置里的
  一项 `HttpOutboundConnectionPool::Config::maximumResponseBodyBytes`（默认 8 MiB，与两条通路原本的
  默认档同值；填 0 表示不限），`HttpClient` 建连接时把它同时交给 h1 的解析器与 h2 的连接配置。
  越界的处置分两处：h1 判这次请求失败；h2 只结这一条流（RST_STREAM 带 `CANCEL`）并保持连接可用——
  头块存不下会把两边的 HPACK 动态表错开、必须收整条连接，而正文越界是本端的胃口不是对端犯了法，
  收连接等于把一个 DoS 防护做成自我伤害。两条通路都**不交半截正文**：越过就整个拒掉，因为
  「200、长度也对、内容少一截」比一个错误难查一个量级。h2 一侧另补两处：已判死的流再来的 DATA
  只还窗口不入库（对端收到 RST 之前那几段照样会到），以及窗口照常归还（收下不还会让连接级窗口
  一路漏下去）。失败原因现在点名是哪一处：h1 由解析器记下「越的是正文字节」这一位——Content-Length
  那条路在头部阶段就按声明值拒了，一个正文字节都没收，只看已收字节数会说「没越界」。
  用例四处：`HttpOutboundConnectionPool.BoundsResponseBodyByConfiguredLimit`（h1 端到端，越界拒 /
  填 0 放开）、`HttpsServer.EnforcesTheConfiguredResponseBodyLimitOverHttp2`（h2 端到端，验的是
  **接线**而不是阈值本身）、`Http2ClientConnection.RejectsResponseBodyBeyondTheClientLimitWithoutClosingTheConnection`
  （留在内存里的正文不超过上限、连接还算健康）、`HttpResponseParser.ResponseBodyDefaultsAgreeAcrossBothTransports`
  （三处默认档必须相等——协商出哪条协议不该改变本端的胃口）。三处各自撤掉都实测能把自己那条判红。
- **出站客户端与 `TcpClient` 连不上纯 IPv6 目标**：三处各自都能单独造成这件事。
  ①`Core::AsyncSocket::create` 的默认档是 AF_INET，而这两处出站连接用的都是默认档——解析器把 IPv4
  排在候选表前面，所以双栈目标上看着一切正常，只有 AAAA 记录的主机（或直接用 IPv6 字面量）永远连不上：
  拿 AF_INET 的套接字去 connect 一个 `sockaddr_in6`，内核只会以「协议族不符」收场。现在两处都按
  **那条候选地址自己的协议族**建套接字。
  ②`Core::AsyncResolver` 对所有输入都走 `getaddrinfo` 且 hints 带着 AI_ADDRCONFIG——那一项按
  「本机有没有配到该族的**非回环**地址」筛结果，于是只有 `::1` 可用的容器/主机上，连
  `getaddrinfo("::1", …)` 都会直接 EAI_ADDRFAMILY（实测），调用方写在脸上的地址反倒解析不出来。
  现在**字面量不进 getaddrinfo**：按 `inet_pton` 直接构造地址（顺带省掉一次线程往返）。名字查询仍带
  AI_ADDRCONFIG，那本来就是它的用途。
  ③测试夹具的 `queryBoundPort` 原先用 `sockaddr_in` 问监听端口，而 Windows 上缓冲区偏小对 v6 套接字是
  **直接失败**而不是截断，端口读成 0，用例随后连向端口 0——表现出来还是「IPv6 连不上」，与所测的东西
  毫无关系（实测挂过 100 秒以上）。
  三条新用例让服务器只监听 `::1`（IPv6 是唯一那条路）：一条解析器字面量、一条 `TcpClient`、一条
  出站客户端 + 池（顺带钉住 IPv6 上 keep-alive 与分组键同样成立）。三条在容器里都能被撤掉②单独证伪。
- **出站请求自动重来一次的判据改成「方法是否幂等」，HTTP/1.1 与 HTTP/2 同一口径**：此前只有 h2 认
  「请求已整个写上通路」这一位，h1 只认「有没有读到响应字节」——而写进一条对端已经收掉的连接在本端
  看着是**成功**的，于是 h1 那条池化的 POST 会被换一条新连接重交一遍（非幂等请求做了两遍）。反过来，
  h2 那一刀切的「已发出就交出失败」把 keep-alive 的固有竞态透给了调用方：服务端在空闲期把连接收掉，
  本端下一条 GET 本该悄悄换一条重来，实际拿到的是空响应。现在两条通路共用同一判据：读到过响应字节就
  不重来（那已是响应本身出了问题，重来不换一个答案）；没读到过时，请求没写出去就重来，已整个写出去则
  只有幂等方法重来（GET/HEAD/OPTIONS/PUT/DELETE/TRACE，RFC 9110 §9.2.2；RFC 9112 §9.3.2 给的自动
  重试许可本就只覆盖幂等方法），表外方法一律按不幂等处置。三条用例把这张表钉满：h1 与 h2 各一条
  `DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection`（撤掉闸门就能看到服务端第二次
  进入），加一条 `PooledClientRecoversWhenTheHttp2ConnectionWasClosedByThePeer`（退回旧规则就拿不到
  恢复）。顺带查清一件事：原有的 `PooledClientDoesNotReplayASentRequestThatTimedOut` 证伪不了这道闸门
  ——时限放弃那条路上整体预算已经用尽，重开连接先被预算挡住，撤掉闸门它照样绿；那条的说明已改成只
  声明它实测到的部分。
- **出站 HTTP/2 连接上「晚到的响应」不再躺到时限**：这条连接的读通路同一时刻只由一个请求协程驱动，
  它交还驱动权时不会叫醒挂在 `StreamAwaiter` 上的其它请求——而那些等待者的「此刻没人驱动，我该自己去读」
  判据只在被恢复的那一刻才会重新看一次。于是当某条响应比当前驱动者的最后一趟到达得晚（服务端先把这条
  连接放着、过一会儿才答），字节就留在套接字里没人取，那条请求一路等到自己的时限才失败。现在交还驱动权
  会把等待者叫一遍，看到驱动权空着的那位自己去读。
- **一条 TLS 连接上「读写各一个协程」不再互相撕开记录**：`SSL_write` 返回 `WANT_WRITE` 之后那条记录
  留在 SSL 内部等重试，OpenSSL 要求这期间**不许**插一次 `SSL_read`——插进去会由读侧替写侧把待发记录冲出去，
  写侧随后又按同一份数据重试一遍，同一段明文在线上有两份，对端按记录边界解析当场错位。TLS 1.3 让这个窗口
  更常出现（服务端握随后要发 NewSessionTicket，读侧的 `SSL_read` 会顺手把这些字节写出去）。现在写侧挂起时
  立一面旗，读侧看到旗就让轮一次；成功收下记录、改等可读、以及函数出口都会把旗落下。
- **JSON 里超出 64 位的整数字面量不再悄悄变成 double**：同一个数值写成 YAML 会被明确拒绝，而 nlohmann
  在词法阶段就把超出 `[INT64_MIN, UINT64_MAX]` 的整数字面量折成 double——精度当场丢失、不留标记，之后
  `getInt` 只能按「类型不符」回落默认值，而加载照旧报成功（同一份配置换个后缀就换行为）。现在解析前先扫
  一遍文本并拒绝，报出越界的那个字面量与字节位置；带小数点或指数的浮点写法、以及字符串里的长数字都不受影响。
  浮点侧同一条扫描现在还管**下溢**：`1e-400` 原先被 nlohmann 悄悄折成 0.0（而 YAML 会拒），
  而 0 通常意味着「把那个机制关掉」；次正规数（`5e-324`、`1e-309`）等合法写法照常收下。
- **配置里的空键段不再塌缩成别的键**：`{"": {"port": 8080}}` 与顶层 `port: 9090` 原先落成同一个扁平键，
  按展开顺序后写者静默覆盖前者（带点号的键早就拒了，空键段从那道判定旁边走了过去）；而 `{"": 5}` 会把空键
  塞进生效快照——`has("")` 报有值，`setValue("")` 却拒收空键，那条记录只能读、永远改不动。现在两种形态
  都判加载失败并说清原因（整份配置不提交，与点号键同一口径）。
- **格式化失败的降级记录不再被自己的等级挡掉**：`logFormat()` 系列在格式串写错时降级为一条诊断记录，
  但那条记录原先固定打成 `Error` 并再过一次 `shouldLog`。阈值高于 Error 的记录器（只在 Fatal 上出声的那种）
  于是连「格式串写错了」都不留痕迹——代码里明明有日志调用，屏幕上一条也没有，且没有任何一处解释。
  现在等级取「本次等级与 Error 中更高的一档」，降级记录必然通过与本次调用相同的那道判定；
  阈值在 Error 及以下时行为逐位不变。
- **`base_filename` 带目录段时活动文件与备份不再分家**：`directory / base_filename` 的两条路径语义
  （右操作数带目录段时走进子目录、是绝对路径时整体替换左操作数）把一次构造拆到了两个目录——滚动照旧
  发生，被滚走的备份却永远清不掉（清理只扫 `directory:` 那一层），日志目录无界增长。构造参数现在
  统一只取文件名段，这也正是该参数在头文件里早就写下的承诺。要把日志放到子目录，请写进 `directory:`；
  读配置那一层会为这次折叠出声（报出原始写法与实际取到的文件名），不静默改掉落盘位置。
- **等级名的小写写法不再被当成拼错**：`logLevelFromString()` 原先大小写敏感，`level: error` 判成不认识并回落为
  INFO——而日志配置里其余取值全是小写（`type: file`、`policy: size`、`overflow_policy: drop_oldest`），小写等级
  才是最自然的写法。回落的后果不是噪音而是超量：一份「只留错误日志」的配置实际在打全量 INFO，正文里不该外泄的
  字段随之写进文件。现在 TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF 的任意 ASCII 大小写写法都解析为同一等级；
  折叠只按 ASCII 码位做，不调 `std::tolower`（后者查当前 C locale 的表，土耳其语环境下 `'I'` 折不出 `'i'`）。
  长度不等先判掉，因此全角与中文写法仍走「诊断 + 回落」这条老路。
- **控制台写不出去时不再一声不响**：`ConsoleSink` 原先写完不看流状态。标准输出被重定向到已退出的
  读取端（`prog | head -1`、父进程先走）时，插入与刷新都不抛异常，只把 `badbit` 立起来，而 iostream
  此后每次 `<<` 都是空操作——整块日志就此消失，头文件却还写着「write() 返回时该行已刷新落地」。
  现在写完与刷完都核一次流状态，本次故障的第一条经**另一条**标准流出声（拿根日志器报自己会让
  `write()` 递归回来），成功写过一次就重新武装；两条流各记各的「已报」，互不顶掉。
- **io_uring 与完成端口后端不再交回「没人要的就绪」**：两个后端与 epoll 后端在两条约定上各有偏离。
  其一是无效描述符：`addFileDescriptor(-1, ...)` 原先报成功（轮询提交进环后内核回一份错误完成），
  注册表里留下一条永远等不到正常通知的记录。其二是关注位已改掉之后：io_uring 的提交推迟到 `wait()`
  才做，完成端口清零掩码也不回收已投出的探针，于是旧那一份轮询结果被原样上报——上层会把它缓存成
  「已就绪」，下一次等待凭空醒一次；写侧更糟，一份陈旧的可写会让人以为发送缓冲已经排空。
  现在两个后端都只上报此刻仍被关注的那几位（错误与挂断仍照常透出）。部署在 Linux io_uring 路径上、
  或 Windows 完成端口上的服务，此前观察到的「空闲连接偶发被无端唤醒」少了一类来源。
- **预 1970 且不足一毫秒的时刻不再渲染出不合版式的时间戳**：日志时间戳把「整秒」向下取整、把毫秒残差
  单独逐位写出。整秒那一步用的是向下取整，毫秒那一步用的是朝零截断，两者在负时刻上方向相反：落在
  `(-1ms, 0)` 这类「比零更负、但不足一毫秒」的时刻上，残差算出来是 **1000** 而不是 `[0,1000)` 内的值，
  逐位写的三位毫秒随之溢出——实测渲染出 `1900-01-00 00:00:00.:00` 这样带一个 `:` 的串，按固定宽度解析
  时间戳的采集端会把这一行判成格式错。改为两侧都向下取整（换算到毫秒仍是除法，极端时刻不会溢出）。
  常规挂钟（1970 之后）与既有极端值用例的读数逐字不变。
- **只发过无法解密报文的 QUIC 对端不再能永久占住路由表**：一条连接的空闲截止时刻原先只在「报文解密成功」
  那一刻才亮——那是反放大的要求，解不开的报文不能拿来续命。代价是只发一份看起来合法的 Initial 就消失的对端
  留下的表项**从来没有**截止时刻，而服务端定时拍唯一的收口通道是「到期时刻已过」，`QuicConnection::nextExpiry()`
  报 `time_point::max()` 就等于永不检查：条目连同它的 TLS 会话一直留在表里。现在截止时刻在建状态机那一刻
  就按本端宣告的 idleTimeout 算起——解不开的报文依旧不算活动（不续命），但这条表项的寿命有了上界。
- **按时间滚动时不再拿「没验过的备份名」去改名**：同一周期内已有备份时依次试探 `.2`、`.3` ……旧写法的
  循环在数到最后一个候选名之前就先退出，那一个的名字从没被验证过就直接用于改名——崩溃循环里同一周期
  反复滚动到用满 1000 个候选名时，历史的一段被悄悄覆盖。现在逐个验到底，全被占时出一行诊断；探测同时
  改用 `error_code` 重载（抛出重载会让本轮滚动半途而废：活动文件已关、重开被跳过，而那一条事件在异步
  包装下除了计数没有任何痕迹）。
- **异步日志里被下游写失败吃掉的事件现在计得出来**：`AsyncSink` 的 worker 线程会咽下下游 `write()`
  抛出的异常（不咽就让 `std::jthread` 入口的未捕获异常终止整个进程），但那条事件既没落地也不计入
  `droppedEventCount()`——而异步路径上这条异常一行诊断都不发（在日志库内部再打日志会递归回自己），
  于是「一共丢了多少」在这个唯一的观测面上恒为 0。现在它与另外三种 worker 侧没能落地的情形同口径计入。
- **拿 `LogLevel::Off` 当消息等级的日志不再落出一行 `?????`**：等级过滤原先只管阈值一侧（阈值是 Off 就全关），
  消息一侧写的是 `level >= threshold`，而 `Off` 是枚举里数值最大的一档，因此 `log(LogLevel::Off, ...)`
  在任何阈值下都放行，落出来的行等级标签是 `?????`——采集端按等级解析会读到一个不存在的等级（静默变形）。
  现在 `Off` 两侧都不放行；判定收进 `logLevelPassesFilter()` 一条函数，Logger 与 Sink 共用（此前两处各抄一份，
  只改一处就会给出不同答案）。数值超出已知范围的等级**仍放行**：宁可留一行带 `?????` 的记录，也不把日志静默吞掉。
  相应地，「阈值 Info + 消息 Off → 放行」这条旧断言按新语义改写为拒绝。

- **Windows 上「在监听却不应答」这类故障现在留得下话**：完成端口投不出探针的原因（本机拿不到 AcceptEx、
  `getsockname` 失败、`WSASocketW` 撞 `WSAEMFILE`、AcceptEx 投递失败）此前被拼成中文文案后原样丢掉——
  所有调用点都传空指针，日志里一个字都不留。现在原因由后端自己记一条 WARN，并且只在某个方向「从正常转为
  失败」的那一次记：待重投表每轮 `wait()` 都要重投，每轮一条会把事件循环线程变成日志写手。
  `connect()`/`listen()` 之前那种「此刻武装不上」仍然静默重试（每条出向连接都要经过一次，告警只剩噪声）。
  同时补上一处不对称：探针自身报出「可重试」错误码时，读与写两个方向都只返回失败、不排进重投表，
  那一方向从此再没人武装；现在按类别交给重投表（暂时状态）或合成就绪事件（硬错误，等待方自己去拿真实错误码）。

- **文件日志刷不出去时不再静默**：短写入只进流缓冲，`writeLine` 当场报「写成功了 N 个字节」，真正把字节
  推给设备的是 `flush()` 那一次同步——ENOSPC、配额耗尽、设备只读都藏在这一步，而 `flush()` 原先不看流
  状态也不上报。最需要它的那条恰好是 Fatal：打完就 abort，没有下一条日志去触发写路径的诊断，于是最后一条
  记录整条不见而现场全静默。现在写与刷共用同一条一次性上报口径（只走标准错误）。

- **某个日志器的配置写成标量时报诊断，并且不动这个日志器**：`logging.loggers.app: "on"`（少写了下面那层
  结构）原先两头不着——字段读不出来因而没有任何一处报错，而取 logger 那一步会顺手把它的级别改成
  `global_level`。一行写错的字段换来一次没人报告的级别变更：原本设成 ERROR 的日志器开始收 Info。现在形态
  判据排在取 logger 之前，跳过该条目并报出实际形态。

- **滚动失败不再只留下一个反复变空的日志文件**：`RollingFileSink` 按大小滚动时要把活动文件改名成备份，
  改名失败（目标序号被占、目录只读、跨卷）就退化成「清空活动文件」——不清零则滚动条件恒成立、文件在原地
  无限增长。这个兜底原先一声不响：现场只看得到日志反复变空，没有任何一处解释为什么，也没有一处指出该清
  哪个目标。现在改名与连带的清空都各报一条到 std::cerr（这一层不能用根日志器报自己，那是让 write() 递归）。

- **`setValue()` 改掉一个键的形态时段里的旧键一起让位**：文件加载早就按「后写的说话」定形，程序侧写入却
  只是往快照里塞一个新键，于是 `server` 可以是 9090 而 `server.port` 也还在。这种快照两边各自都「对」、合起来
  没有一句真话：`getSection("server")` 只扫 `server.` 前缀，刚设进去的那个值整段看不见（日志装配正是只经
  `getSection` 取配置），而 `has()`/`get()`/`keys()` 又都报它存在。现在两条通道共用同一份定形逻辑，每次取代
  报一条中文日志；`getSection` 那道「既是值又是表就抛」的守卫保留为不变式自检。

- **没挂回调的热重载失败不再无声**：`enableHotReload()` 允许不传回调（「重载照跑」），而重载结果原本来就只有
  这一个去处。于是某个配置文件被写成非法 YAML 时，它的全部键从新快照里整份缺席，热重载开关照旧是开的，
  现场只看得见「改了配置没反应」——连「重载坏了」都判不出来。现在没有回调可通知时逐条把失败原因落到日志，
  文案带上文件名与出错行列。挂了回调的口径不变（结果照常交给回调，不重复播报）。

- **schema 的区间校验不再把超出 int64 的无符号值读成负数**：原生解析把非负整数放进 `number_unsigned`，
  而 `is_number_integer()` 对两种整数都成立——取数值时先按有符号读，`max_connections: 9223372036854775808`
  就成了 `-9223372036854775808`。于是「高于上限 1000」判不出来，只给下限时反倒报出一条「低于下限
  -9223372036854775808」的假话；同一个值真正被取用时按严格取用又被拒绝，调用方静默回落到默认值——
  校验报通过、运行用默认，两头都对不上。现在先判无符号那一支。

- **压缩没有收益时不再换表示**：HTTP 响应压缩与 WebSocket permessage-deflate 此前只看「正文够长、
  内容类型不在黑名单里」，从不比压缩前后的实际字节数，于是把变大的结果照发上线。实测形状：一条
  `hello` 压成 7 字节（+40%）还置上 RSV1，把一次 inflate 摊给对端；空消息 0 变 1 字节；15 字节的
  `{"type":"ping"}` 变 17 字节。HTTP 侧一份 4 KiB 高熵正文压成 4119 字节（+0.56%）却带着
  `content-encoding: gzip` 发出，对端多解一次、链路多跑几字节、缓存里还多一份比原文更胖的变体。
  RFC 7692 §7.3 对本端这种「两侧都禁用上下文接管」的形状说得很直接：端点很容易判断某条消息压了
  会不会更短——判据摆在手边却被丢掉了。现在两处都按实际字节数决定，不更短就发未压缩帧 / 不发编码头。
  刻意**没有**加「短消息一律不压」的长度阈值：200 字节的重复文本在空字典下仍能压到 6 字节（−97%），
  按长度一刀切会把这类真收益一起砍掉。

- **`enableHotReload()` 返回 false 的每一条路都说清为什么**：没设配置目录锚点、`FileWatcher::create()` 返回空、
  给目录挂监视失败、监视线程起不来、装配抛异常 —— 五条路原先都只回一个 `false`。「改了配置没反应」这种现场里，
  「锚点没建立」与「监视器起不来」的修法完全不同，而调用方拿不到任何区分依据。现在每条各报一条诊断，
  能带目录的带上目录（按 UTF-8 报，与其余通道同一口径），异常那条带上 `what()`。声明处的 `@return` 同时写明
  「返回 false 的每一条路都会另报诊断」。用例把「回 false 且报了一条说明配置目录的诊断」钉住（在容器里以
  HEAD 的实现跑过：改前该用例红，因为一条诊断都没有）。Windows Debug `TestBase` 574 例全绿、示例 `base_log`
  23 步全过；容器 ASan/UBSan/LSan 574 例零命中。全工程回归另跑一份：HEAD 导出的干净树、五模块全量构建零告警、
  ctest 2859/2859 通过、0 条 sanitizer 命中。

- **可选 io_uring 后端在无限等待里不再把自己睡死**。关注位变化（例如写侧从「发满了」变成「又有窗口」）
  此前只有在该轮等待被别的唤醒打断时才可能被发现；循环因此能带着一个已经就绪的方向睡下去，直到下一次
  定时器到期才醒。表现为客户端侧「连上了却收不到响应」，且只在使用 io_uring 后端时出现。改为睡前把提交
  与收割循环跑到「没有新的待办」为止，并且出圈时仍把悬着的提交项发布出去。
- **可选 io_uring 后端一轮等待也按 1024 条事件截断**，与 epoll、IOCP 两个兄弟后端同口径；此前它没有每轮
  上界，洪泛时会一直取到内核没有事件为止，同批里其它注册的响应被拖长。落地缓冲同时改为一次定容，
  不再每轮按事件数重分配。
- **TLS 会话上下文关闭重新协商**（`SSL_OP_NO_RENEGOTIATION`）。此前对端可以在握完手之后反复发起
  重新协商，每一次都是本端的一次公钥运算，等于用一条已认证的连接换取本端 CPU——是远程可触发的放大面。
- **零拷贝发送的两种「文件越界」不再共用一句报错**。sendfile 返回 0 只表示到了文件末尾，但起点越界
  （一个字节都没上线，属调用方算错了偏移）与长度越界（前缀已经上线，文件比声称的长度短）此前都报
  「源文件在发送期间被截断，请检查静态目录的写入方」。前半句在起点越界时是错方向的猜测，而这条文案
  会原样进服务器日志、往往是运维唯一的线索。现在分开报，并交代已上线的前缀不可整块重发。
- **`AsyncSink::flush()` 不再被持续的生产者饿死**：等待条件原先是「待落地数清零」，可只要有别的线程
  还在写日志，清零就永远不成立——实测同一线程之外的生产者持续写入时，一次 `flush()` 等了 1.9 秒仍未返回，
  而它真正该等的账只有「进场那一刻队列里的 64 条 × 每条 200µs ≈ 13ms」。改为进锁时先记下受理水位
  （`m_acceptedCount`），等的是「已结数追平水位」：后来者的账不是本次要等的账。事件离开队列的每一条路
  （worker 写出、被下游等级过滤丢掉、被 DropOldest 淘汰）都走同一处核销，少一次就有 flush 永远等不到的账，
  多一次就会让 flush 提前返回。唤醒按等待者计数把门，没有 flush 在等时不碰条件变量。**行为口径变化**：
  并发写入下 flush() 可能更早返回，但它保证的部分不变——本次调用之前受理的日志都已交给下游；
  原先「连后来者一起等」的语义本来也不可信（等谁写完没有上界）。上一轮挂在 Fatal 上的刷新走的正是这条
  通道，因此那一步现在有了确定的上界。用例 `AsyncSinkFlush.WaitsOnlyForEventsAcceptedBeforeTheCall`
  同时钉正反两面：生产者仍在跑时 flush 必须已返回、且等待远小于观察窗口。
  Windows Debug `TestBase` 574 例全绿（含打乱顺序三个种子）、示例 `base_log` 23 步全过；容器 GCC 零告警、
  ASan/UBSan/LSan 574 例零命中，TSan 私有容器另跑 3 个随机种子、0 条数据竞争告警。

- **`LOG_FATAL` 那一条不再留在缓冲里随进程一起丢**：`Logger::writeToSinks()` 分发完就返回，而 FileSink
  走的是带缓冲的流——打致命错误的调用方往往接着就 `abort()`/退出，不会替日志系统补那次 flush，于是
  「最想知道的最后一条」留在用户态缓冲里没了。分发循环因此只对 Fatal 这一条在写完后立刻刷新收下它的
  Sink（AsyncSink 的 flush 会等到队列里的欠账都落地，语义正是「Fatal 之前的都已落下」）；常规等级不付
  这笔钱（FileSink 的 flush 是一次 FlushFileBuffers/fsync）。用例 `Logger.FatalLineFlushesEverySinkThatTookIt`
  同时钉住反方向：Info 不刷、没收下这条的 Sink 也不被顺带刷。改前实测两个 Sink 的 flush 计数都是 0。
  Windows Debug `TestBase` 573 例全绿、Release 分配台账与 `Logger` 段全绿、微基准日志四例读数不变
  （`log-filtered-out` 仍 1.4 ns）、示例 45 步全过；容器 GCC 零告警、ASan/UBSan/LSan 573 例零命中。

- **多份配置互相改写形态时不再留下读不回来的幽灵键**：扁平键模型下「后一份文件把一段表写成一个值」
  （或反向）会让两种形态同时留在快照里——实测那份配置有 4 个键，其中 `server` 与 `server.port` 并存，
  而 `getSection()` 对这种键一律抛 `ConfigValidationException`：加载报成功，日志装配却整段停摆，
  运维看不出是哪两份文件打架。合并现在按「后写的文件说话」定形——被淘汰的旧叶子/旧整段一并摘掉，
  并把每一次淘汰报到日志（点名被淘汰的键与动手的那份文件），一条取代只报一条。**行为口径变化**：
  以前这种形态能加载成一份「读不出来但存在」的快照，现在快照自洽且有一条诊断可查。
  `setValue()` 不走合并规则（程序侧写入是显式行为），读侧那道守卫因此保留并被单独钉住。
  新增 3 条用例（标量取代表、表取代标量、setValue 仍能造出冲突）并改写 1 条：旧断言
  `GetSectionReportsLeafVersusGroupCollision` 钉的是「加载沉默、冲突留给读时抛」→ 新语义「加载期定形 + 报一条」
  → 依据是本模块既定的「容错必须可见」与「禁止静默变形」。Windows 侧 `TestBase` 572 例全绿（569 + 3）、
  示例 `base_config`/`base_log` 45 步全过；容器 GCC 构建零告警、ASan/UBSan/LSan 下 572 例（1 例平台性跳过）零命中。

- **schema 的区间约束不再「比不出来就当通过」**：`runSchemaValidation` 原先只在取值是数值时才做区间比较，
  非数值直接跳过——最现实的漏检现场是 schema 只声明 `min/max` 不声明类型（这正是「只想限范围」的常见写法），
  而 YAML 把端口写成带引号的 `"8080"`：类型这关本就不设限，区间那关又因为「字符串没法比」一声不吭地放行，
  运维以为限幅生效，取值方要在运行期才失败。现在取值不是有限数值（非数值类型、NaN、±Inf）时判为不合格并说明
  「区间无法判定」——与同一个函数里 NaN 那支早已采用的 fail-safe 口径对齐。**行为口径变化**：既有 schema 若给
  字符串/布尔/空值配了区间，之前静默通过、现在会报一条（本仓库的产品 schema 无此形态，示例与用例已核）。
  用例侧把旧断言 `RangeIsSkippedForNonNumericTypes`（钉的正是「跳过即合法」）改写为两条：非数值 + 区间必须报，
  以及「只声明区间的 schema 遇到带引号的数字」这一现场形状；两条实测都在改前变红。Windows 侧 `TestBase` 569 例
  全绿（568 + 2 − 1）、示例 `base_config` 22 步全过，容器 GCC 全量构建零告警、ASan/UBSan/LSan 下同样 569 例
  （568 通过 + 1 例平台性跳过）零命中。

- **`--metrics` 与 `--workers N` 同时打开时会在启动说明里写清口径**：多个 worker 进程共用一个监听
  端口时，一次抓取只命中其中一个进程，计数器照样会在两次抓取之间变小——共用采集端这条通道止于进程
  边界，跨进程只能按进程各自暴露抓取端点再由采集侧汇总。原先这条限制只在头文件的告警段里，运维侧
  看到的是「流量忽大忽小」，不会想到是被摊在了一个进程上。
- **同一端口上多台监听器的 `/metrics` 不再报出会变小**：`echo_server` 这类「每事件循环一个监听器
  同绑一个端口」的形状下，每台服务器各持一份采集端，一次抓取只命中其中一台——请求条数只报得出
  1/N，两次抓取之间读数还能回落（实测同一份指标从 1 变 0），采集侧的 `rate()` 与告警因此全部失真。
  示例现在让所有监听器与 h3 共用一份采集端，活跃连接数由各台的管理器镜像到同一份上（`stats()`
  改读采集端，单台部署的读数与过去逐位相同）。
- **配置快照提交后的 schema 校验不再压在写锁里做**：`commitConfigData()` 原先在串行化写者的那把锁内
  跑校验，而每条违规要 `std::format` 一次并走一遍 Sink 写入——热重载撞上写坏的配置时，业务线程的
  `setValue()` 会整批卡在写锁上等这一轮日志落完。校验改成对着自己手里那份已发布的快照判，
  与后来者替换快照不冲突。顺带把「目录里一份配置文件也没有」那条出口并到同一条提交通道：
  它原先自己换快照、绕过了这道校验，必需键就此静默缺失而不留一行日志。新增
  `ConfigManagerTest.SchemaViolationLoggingDoesNotHoldTheWriterLock`（在 root 上挂一个闸口 Sink，把提交
  线程精确停在「写那条校验错误日志」上，再放 `setValue()` 进来抢锁）与
  `LoadFromEmptyDirectoryStillRunsTheRegisteredSchema`。
- **热重载起不了线程时不再带走监视线程**：启动一轮重载任务的路上有两处会失败的分配（任务节点、
  `std::jthread` 的线程栈与系统线程数上限）。异常原先顺着调用方逃出去，而调用它的是文件监视线程与
  重载线程——逃出线程函数即 `std::terminate`。更麻烦的是「本轮已排上」的标记停在真，之后每条变更只
  记下「还欠一轮」而没人接力，热重载到此永久停摆，现场看着像监视失灵而不是起线程失败。现在启动失败
  就在启动入口内收口：记一行错误日志、把标记让回去并留下欠账，本轮让给下一次事件；文件变更入口整段
  再兜一层，它才是这条路径的线程边界。这条路径要有线程创建失败才命中，进程内没有可注入的接缝，
  因此没有配对用例。
- **日志器入退休表失败时不在映射里留空槽**：注销与清空都先把映射里的值移空、再放进退休表，而中间那
  一步可能要分配。一旦失败，映射里就留下一个空槽，而 `getLogger()` 对表里的值直接解引用（它只保证
  查得到，不判空）——一次分配失败被升级成解引用空指针。两处都改成「入表成功后才交接所有权」；
  清空另把两张表的位置一次留够，使循环里不剩分配点，要抛就抛在循环之前，映射还是原样。
- **在自己的热重载回调里关闭热重载不再让进程倒下**：`disableHotReload()` 收尾时要 join 每一条重载
  线程，而回调本身就跑在重载线程上——从自己线程里 join 自己按标准是 `resource_deadlock_would_occur`
  （Linux 上是永久自等待），异常顺着回调出去即 `std::terminate`。现在收尾跳过「就是本线程」的那条任务，
  把它留回待收尾表，由下一次启停或进程退出接回。新增
  `ConfigManagerTest.DisablingHotReloadFromWithinItsOwnCallbackDoesNotAbort`。
- **只含字节顺序标记的配置文件按空文件处理**：UTF-8 字节顺序标记原先在「是不是空文件」这道判定之后
  才被剥掉，于是那份只带标记的文件被判成语法坏。现在进解析器前先剥标记，判定看的是剥完之后的正文。
  新增 `ConfigManagerTest.LoadFromDirectoryAcceptsByteOrderMarkOnlyJsonAsEmpty`。
- **相对路径不再被当成配置目录锚点**：从一组文件推出的公共父目录只在绝对路径上成立——两份相对路径的
  公共前缀可能只是「当前工作目录之下某个碰巧同名」的目录，落了锚点之后 `reload()` 会去扫一个语义完全
  不同的目录。含相对路径的列表现在不因此改锚点，仍沿用上一次的目录。新增
  `ConfigManagerTest.LoadFilesWithRelativePathKeepsTheConfigDirectoryUnset`。
- **按大小滚动的字节账按真正落盘的字节算**（Windows）：`FileSink` 的行写入原先交回行缓冲的长度，而文本
  模式会把每个 `'\n'` 翻成 `"\r\n"`，带调用栈的行每帧还有一个 `'\n'`。按大小滚动的阈值直接累加这个返回值，
  报小了活动文件就会系统性超出上限才滚。现在交回落到磁盘上的真实字节数（本平台按换行数补上多出的
  `'\r'`），与 `std::filesystem::file_size` 的增量逐行相等。新增
  `FileSink.WriteLineReportsBytesActuallyLandedOnDisk`。
- **滚动文件 Sink 不再因一次重开失败就永久停产**：一次滚动先把活动文件句柄关掉，再新建
  `FileSink`——而它在打不开目标时抛异常（被杀软或备份代理短暂独占、磁盘写满、网络盘失联），
  于是那句赋值从不发生，指针停在上一步刚置空的空上。此后按大小的滚动判据要求活动文件非空，
  **连触发重开的机会都没有了**：写日志走到「有没有活动文件」那一句直接返回，本 Sink 在零告警、
  零异常的状态下断掉整条日志链路直到进程退出——Logger 那侧只在抛出时上报，而它已不再抛。
  现在写入入口发现活动文件为空就按每秒一次的节奏补一次重开（故障通常秒级自愈，而每行都试会把
  故障期间的每次写日志变成一次打开文件的系统调用）；仍开不开照旧抛出，让既有的 Sink 异常上报
  路径继续出声。新增 `RollingFileSink.ResumesWritingAfterTheActiveFileFailedToReopen`：
  把活动文件置为只读、并用两个非空目录堵住滚动那一步的重命名，使只读位真的挡得住重开；
  在只读位拦不住写打开的环境（容器内以 root 运行）该用例如实跳过。
- **滚动保留数与单文件大小都补齐了上界，备份序号不再按 32 位整数走**：顺移备份的序号原先
  转成 `int` 再从最高位倒着走，而 `3000000000` 这类仍在 `size_t` 里的取值按 `int` 解释是负数，
  起点就小于 1，整个顺移循环一步不跑，随后把活动文件重命名进 1 号位时直接把它盖掉——
  填了「保留 N 份」实际只剩 1 份，丢的还是最旧那份的历史内容。序号改为一趟走 `std::size_t`。
  同时给 `RollingFileSink` 立 `kMaximumBackupFileCount`：这个值决定每次滚动要顺移多少个序号
  （逐个查存在性），不设上界就等于让滚动握着本 Sink 的锁做上千万次目录项查询，与隔壁
  `max_size_mb` 当初夹下界防的是同一件事；配置侧共用同一个常量，两处解析不会各说一遍。
  `max_size_mb` 也补了上界——它只夹过下界，而换算成字节要乘 1024×1024，`2^44 MiB` 在 `size_t`
  里正好回绕成 0，于是「已写字节 >= 阈值」恒真、每写一行滚一次，症状与填 0 一模一样而配置里
  报出来的数却大得离谱。新增 3 条用例（天文数字保留数下三代内容仍分别落在 2 号备份、1 号备份
  与活动文件里；两个越界取值各钉一条诊断与生效值）。
- **异步 Sink 的刷新不再握着队列锁做下游落盘**：`flush()` 等在「待落地数为 0」上，返回时却仍
  持有队列锁去转调下游的 `flush()`——那一句可能是 `FlushFileBuffers` 或一次标准输出刷新，
  而所有写日志的线程取的都是同一把锁，于是一块忙盘或一个失联的网络盘能把业务线程整体停摆
  几百毫秒。锁本护不到它：被包装的 Sink 自构造起不再换指，工作线程早就在锁外写它，
  `stop()` 也是 join 之后在锁外刷新。新增 `AsyncSink.ProducerIsNotQueuedBehindTheDownstreamFlush`：
  用一个只在刷新里占位 500 ms 的下游造出确定窗口，判据取一对不可能同时误命中的时限
  （占位 500 ms、写入须在 100 ms 内返回），未修版本实测等满 494 ms。
- **多个 Sink 的过滤阈值在派发中途变化时，不再有人收到被搬空的事件**：日志器派发一条事件时
  先数「有几个 Sink 愿意收」再投一遍，`shouldLog` 因此每 Sink 每行被问两次。判据取的是过滤
  结果，而阈值可以在两遍之间被并发的 `setLevel()` 改掉：第一遍只数到一个，于是按「只有一个收」
  把事件本体移交给前一个 Sink，第二遍后一个 Sink 也放行了，读到的却是已被搬空的消息——
  记出一条只剩时间戳与级别的空日志。交出本体这条省拷贝优化本来就只服务于「日志器上只挂一个
  异步 Sink」，判据因此改看快照里 Sink 的个数（它随快照一起发布、这一趟内不会变），判定与
  投递也就合成了一遍。新增 `LoggerTest.LaterSinkNeverReceivesAnEventWhoseBodyWentToAnEarlierSink`。
- **配置：显式文件列表复用既有锚点时沿用该目录原本的递归口径**：`loadFiles()` 把推出的公共父
  目录只当作 `reload()` 与热重载的锚点，落锚点时却无条件把递归口径写成 `true`。锚点正好就是
  快照已有那个目录时，这等于在没人改过文件的前提下换了一副面孔：先 `loadFromDirectory(dir, false)`
  建立只看顶层的配置，再 `loadFiles({dir/a.yaml})`，之后一次 `reload()` 就凭空多出子目录里的键。
  改成锚点目录未换时沿用快照已有的取值，只有确实落到一个新目录上才按递归起步。
  新增两条互为对照的用例：复用同一锚点时 `reload()` 不得多出子目录的键；换上新锚点时按递归重扫
  （后者用来挡住「无条件沿用」这一反向改错）。
- **HTTP/3 不再因一条来路不明的 Section Ack 杀掉整条连接**：放弃一条响应流时本端会抹掉它待确认的头块
  记录，而对端那条 Section Ack 很可能早已上路——两条指令各走自己的单向流、彼此没有先后保证
  （RFC 9204 §2.1），「Ack 骑在收口之上」与「对端记账错乱」在本端看到的是同一副样子，区分不了。原先
  一律判 DecoderStreamError，于是任何客户端每条连接取消一次请求就能把这条连接打掉。现在按忽略处理且
  不动任何计数；重复确认同一类同样无从分辨，按同一口径忽略。用例侧改了三处旧断言（「被放弃的流再发
  Ack 即错」「没发过头块的流不该有 Ack」「重复 Ack 即错」），新语义统一为「忽略且计数不动」。
  真机口径由新增的 `scripts/h3_cancellation_probe.py` 给出：连发 80 条以 RESET_STREAM 或 STOP_SENDING
  收场的请求之后，收尾那条 GET 仍要拿到 200 且服务端零告警。
- **QUIC 流层补上以打断收场的那两类流**：上一条只认「FIN 收齐并交付完」，于是被对端 `RESET_STREAM`
  打断的请求流、被对端 `STOP_SENDING` 叫停的响应流各自留下一条永远摘不掉的记录——后者正是客户端
  不再读响应时最常见的收场。入站侧的结清判据改成「上层报回来的 + 复位时作废的 == 见过的最大偏移」：
  复位会把乱序缓存里那段不连续的尾部一起丢掉，上层永远不会为它报回额度，不记这笔作废量就永远凑不齐。
  出站侧认两条收尾之路：FIN 已确认，或本端那份 `RESET_STREAM` 已落了定（还在途就摘会把重发的归属弄丢）。
  新增 4 条用例：被复位的入站流可摘且迟到帧与重复复位都不许激活它、额度只报回一半时不许摘、
  被叫停的响应等复位确认后可摘且迟到额度与再次叫停不许把记录建回来、五十条以打断收场的请求读数仍为 0。
- **QUIC 流层摘掉已收口的对端流**：入站与出站两张表此前一辈子不 `erase`，一条 HTTP/3 请求留下一条入站
  加一条出站记录（实测 100 条往返留下 200 条），每包编帧要扫的长度也随连接时长一起涨。涨到多少后来量过了
  （容器 GCC 13 Release、绑核，只调 `collectFrames`）：开着回收时连接年龄 0 / 100 / 1000 / 5000 / 20000
  条请求分别是 7.3 / 6.3 / 6.3 / 6.3 / 6.7 纳秒，去掉回收是 9.5 / 917 / 12556 / 71290 / 823700 纳秒
  ——超线性，因为两张 `std::map` 的节点铺开后再也放不进缓存。摘记录的难处不在
  省下的那点字节，而在迟到帧的归属：按新流重建一份会带着全新的接收额度，把对端合法的重传判成
  FLOW_CONTROL_ERROR。判据取「同类型流号只增不减」（RFC 9000 §2.1）——双向、单向各记一条边界，
  边界之下又不在表里的号一律按已作废的流忽略。回收要三件都齐：收齐且交付完、上层把额度原数报了回来
  （`releaseReceiveWindow` 找不到条目会把这份额度直接丢掉，连接级窗口就此停住）、且不再欠对端任何一帧；
  发送侧只摘已确认的 FIN，判丢会把它退回未收尾。本端发起的流不摘：数量与连接寿命无关，为它们留一张
  作废名单反而更贵。
  新增 6 条用例：一次完整往返之后两侧记录都摘掉、100 条往返的读数仍为 0（去掉回收这一步读数是 200，
  且这 6 条全部转红）、入站侧先结清不许挡住同一条流的响应、迟到帧与 STOP_SENDING 都不许激活已作废的流、
  收与发两侧各自判定（额度没还清时只摘得出站那一侧）、判丢把 FIN 退回未收尾时出站记录不得被摘。
- **HTTP/3 流式路由答完无正文响应后把收尾字节交给传输层**：无正文的应答（204/304、HEAD）只由
  `submitResponseHead` 记下「本端已收尾」，关闭通知留到 flush 里 `noteLocallyFinishedStream` 才发得出，
  而流式服务路径答完之后从不 flush——于是 `onStreamClosed` 永不到来，摘除点的另一半条件（`isStreamClosed`）
  一直不成立，这条流式记录连同它占着的窗口额度挂到连接收口，`hasOutstandingWork()` 也随之永久为真，
  按条数上限排空就成了等不到的死路。非流式路径由 `serveRequest` 收尾统一 flush，这里补上同样的一步。
  新增 `Http3Session.ReapsStreamingRecordAfterBodylessResponse`：流式 POST 带 2 KiB 正文、处理器只回 204，
  断言若干拍之后不再有在途工作；去掉这一步 flush 时该断言转红。
- **HTTP/3 摘掉流式请求记录时归还它占着的接收窗口**：承载层把 DATA 载荷的额度归还明确交给上层
  （`creditConsumedBytes` 只补帧开销、把载荷扣掉），h2 侧由 `finishStreamingRequestBody()` 兑现，h3 的流式记录
  却是在收尾步骤里直接 `erase_if` 摘掉的——处理器不读正文（提前作答、判错早退）时那些字节就永久留在
  连接级 MAX_DATA 里，攒够一轮整条连接再也收不进东西。摘除点现在补上「按已消费处理」，与 h2 同一件事。
  新增 `Http3Session.ReturnsWindowForBodyAbandonedByStreamingHandler`：流式 POST 带 4 KiB 正文、处理器一个字节
  都不读，断言归还量不少于正文长度；消融（去掉摘除点的归还）读数只有 51 字节——那是帧开销，正好是
  承载层愿意替上层还的那部分。
- **QUIC 退休包号空间时，拥塞层的在途账一起销**：恢复层与拥塞层各记一本在途字节，`discardSpace()` 原先只清
  恢复层那本——被退休空间里未确认的包此后既不会被确认也不会判丢，却永久留在 `bytesInFlight` 里。Initial 空间
  几乎总在握手期就退休（解到对端 Handshake 报文即退休，RFC 9001 §4.9.1），于是这条连接之后每次算
  `remainingByteBudget()` 都要少一截，拥塞窗口被一次性扣走且永不归还。消融读数：握手完成后
  `bytesInFlightByteCount()` 停在 1284 而不是 0。修法是在退休处按既有 `unacknowledgedPackets()` 取回这批包
  交给拥塞层销账；**只销账不涨窗**（退休一个空间不是「网络变好」的信号，当成确认处理会让窗口凭空多涨一段）。
- **DNS 解析结果投回循环前先确认等待方还在**：解析线程原先无条件解引用发起方的 `EventLoop` 去
  `postRemote`，而等待中的协程帧随循环一起销毁时，那次投递就是在碰一块已释放的调度器——窗口宽达整段
  阻塞的 `getaddrinfo`（毫秒级）。现在先取 `callerHandle`：等待器析构时必置空，非空才是「帧还在、
  循环也还在」的信号，取不到就不投。顺带把这条路径上「分离线程抛异常即 `std::terminate`」的口子补成
  记一条 ERROR，并把跨线程投递的生存期契约写到声明处（`Scheduler::scheduleRemote`、
  `AsyncExecutor` 的析构顺序、`AsyncResolver::resolve` 的 `@warning`）。
  没有新增用例：这条窗口需要注入缝才能钉住，本类没有可卡的点；已有的 6 条解析用例与两套全量门禁
  覆盖行为不变。剩下的「判空与入队之间」那几纳秒要彻底封死，得给循环加一道与析构互斥的存活门闩
  （执行器与解析器共用），那是跨模块改动，另轮再做。
- **304 也按「这一请求本会被压缩」改写验证器**：压缩中间件原先对无正文的状态码整段早退，于是同一请求的
  200 给 `etag: W/"…"` + `vary: accept-encoding`，而命中缓存验证的 304 给的是**强** ETag 且不带 Vary。
  RFC 9110 §15.4.5 要求 304 回带 200 本该给出的那些头部，§8.8.1 禁止给被转换过的表示配强校验器——两头不
  一致会把缓存引向错的变体（压缩副本与未压缩副本并成一份）。现在 304 走同一份改写（协商结果非 identity
  才降级并补 Vary）；判不准「本来会不会压成」时只往多降级一侧偏，弱校验器不会合并变体，强校验器会。
  新增用例 `CompressionMiddleware.WeakensValidatorAndKeepsVaryOnNotModified` 同时钉住对照面（只接受
  identity 的客户端拿到的 304 不降级、不加 Vary）。证伪：把早退改回原样，该用例当场红。
- **空文件上语法非法的 `Range` 不再被判 416**：`parseRange` 把「表示大小为 0」放在解析区间之前短路成
  Unsatisfiable，于是 `Range: bytes=abc` 这种根本读不出区间的请求头也拿到 416 + `content-range: bytes */0`。
  RFC 9110 §14.2 要求收端无法解析的 Range 一律忽略、按 200 全量回——416 只留给「合法但无可满足区间」。
  判序换过来之后，空表示上的合法区间（`bytes=0-10`、`bytes=-5`）仍判 416，且后缀形态不会先算出回绕的末端。
  新增用例 `HttpServer.IgnoresMalformedRangeOnEmptyFileAndRejectsOnlyValidUnsatisfiableOne`；
  证伪：把早退搬回去，该用例的 200 断言当场读到 416。
- **WebSocket 的「有一帧在写」标记改由作用域守卫复位**：`sendFrame()` 原先在 `co_await` 前后各写一次那个
  布尔量，而发送回调是可以抛的（写路径的框架异常正是从这次挂起点穿出）——异常展开跳过复位那一行，标记就
  永久停在 true。会话收尾按它决定「要不要补发 Close 帧」，于是这条连接此后再也没有关闭握手：对端只看到
  连接被直接结束，拿不到任何状态码，浏览器侧一律记成异常断开。新增一条契约用例（假发送回调抛异常，
  断言标记已复位）；证伪：撤掉守卫改回两行赋值，该用例当场读到 `isWriteInFlight() == true`。
- **HTTP/2 不再把对端跳过的流号上的帧当成「记录已挤出的迟到帧」咽下**：`m_highestPeerStreamId` 是**上水位**而不是
  「开过的流」的集合，而 §5.1.1 只要求新流号严格递增、允许跳号——于是对端先开流 7，再往从未开启的流 3 发
  DATA/RST_STREAM/WINDOW_UPDATE，三条路径都判 `streamId <= 最大值` 成立而静默忽略（DATA 还要把连接级窗口还回去），
  本端已用过的最大流号越推越远，之后**每一个被跳过的奇号都永久免检**。HEADERS 那条路径判得对（回 PROTOCOL_ERROR），
  正是这种不一致说明漏判是无意的。现在只有「确实挤掉过终止记录」（账本不再完整、无从证明）才允许宽容忽略；
  一条记录都没挤掉时按 §5.1「idle」段回 PROTOCOL_ERROR。代价如实写明：一次挤出之后本端就永久退回宽容——
  要精确到底只能永久保留全部流号，那是随连接时长线性增长的内存。证伪：去掉新增的 `&& m_hasEvictedTerminatedStreamRecord`
  判据，新用例 `RejectsFramesOnStreamIdentifiersThePeerSkipped` 三条连接全部转红（返回 `NeedMore` 而无错误码）；
  另一条 `StillIgnoresFramesOnStreamsWhoseRecordsWereEvicted` 钉住挤出的那一档不变——先断言流 1 的记录确实已不在账本里，
  实现里的窗口值将来调大时它不会静默退化成测「保留记录」那一档。
- **Base：`loadFiles()` 不再把解析中途失败那份文件的半截键提交进快照**。`ConfigLoadResult` 的契约
  写的是「失败文件里的键从快照中消失」，按目录加载那条已经用「每份文件一张临时表」兑现了它，
  `loadFiles()` 却把整份累计表直接交给每份文件去摊。于是列表里一份文件因键名带点号而抛错时，
  它已经展开的那半截键留在累计表里，只要同批还有别份成功就跟着一起提交——调用方只看到一条失败
  提示，实际却已应用了一半新配置。证伪：改回共用累计表，新用例
  `LoadFilesDoesNotCommitHalfFlattenedFailedFile` 当场读到 `alpha == 1`（该键属于那份判失败的文件）；
  按目录加载的既有用例两轮都绿，说明这条契约此前只覆盖了两个入口中的一个。
- **Base：目录扫描被中断时不再把「少了几份文件」当成全量提交**。`scanConfigFiles()` 遇到不可读的
  子孙目录、或某个条目连属性都取不到时会中途跳出，原先它仍把已经扫到的那份残缺清单交回上层，
  `loadFromDirectory()` 于是按全量替换快照——那几份没扫到的文件的键就此静默消失，调用方只看目录树
  根本发现不了。现在返回 `std::expected`：扫描失败即本轮加载失败、给出「哪个目录、什么原因、本轮
  只扫到几份、该怎么修」的中文文案，旧快照原样留着。两个细节：单条目属性改走带 `error_code` 的
  `is_regular_file()` 重载（无 `ec` 的那一份会抛 `filesystem_error`，会从「加载配置」里逃到调用方手上）；
  循环**之外**还要再判一次错误码，因为迭代器在最后一个条目之后才出错时，循环体内的检查永远跑不到。
- **Base：`reload()` 与热重载的递归口径跟着快照走**。两条路径此前都把递归硬编码成 `true`，于是调用方
  以 `loadFromDirectory(dir, false)` 建起来的配置，在一次 `reload()`（或子目录里任何一次改动叫醒的
  热重载）之后会凭空多出子目录里的键——没人改过文件却换了配置。递归标志改为记进快照
  （`ConfigData::configDirectoryRecursive`），重扫与挂监听共用同一个口径。证伪：把 `doReload()` 改回
  硬编码 `true`，新用例 `ReloadKeepsTheNonRecursiveScopeOfTheOriginalLoad` 报「凭空多出子目录里的键」。
- **Base：并发启停热重载不再撕裂监视器对象**。`enableHotReload()` 写 `m_fileWatcher`（普通
  `unique_ptr`）、`disableHotReload()` 读它并 `reset`，二者此前只靠一个原子布尔互相「打招呼」——
  那个布尔护得住开关，护不住对象本身。新增控制面锁 `m_hotReloadControlMutex` 串行化这两个入口；
  该锁总在最外层，监听线程的回调只碰 `m_reloadTasksMutex`，因此不与写锁成环。撕裂指针那类竞态
  只有 TSan 看得见（本机 WSL2 内核上 TSan 起不来，已实测确认），新用例
  `ConcurrentEnableAndDisableHotReloadEndsInOneConsistentState` 钉的是「终态一致、关掉后还能再开起来」
  这一下界，跨线程证据要由 CI 的 TSan 作业提供。
- **Platform：关闭一条监视不再可能把调用线程永久钉住**。`closeEntry()` 取消未完成的重叠读之后，
  用 `GetOverlappedResult(bWait=TRUE)` 等它的完成包，那是无界等待；而本模块早已实测到「目录被改名
  走开、之后不再往里写入时 `CancelIo` 不给完成」——两者相遇，调用 `stop()` 的线程就再也不回来。
  表现在外是热重载的 `enableHotReload()`/`disableHotReload()` 在并发启停下卡几十秒（另一次实测里
  ctest -j14 中同型形状跑了 562 秒），以及进程退出被同一处钉住。现在改为：`CancelIo` 之后有界等
  该条目的事件（200 ms），再用 `bWait=FALSE` 取结果（这一句本身不阻塞）；超时就直接关句柄，
  已取消的 IRP 交给内核在句柄回收时了结。宁可少拿一次完成包，也不把调用方的线程搭进去。
- **Base：极深嵌套的配置文档改判加载失败，不再让进程当场倒下**。实测：一份两万多层的 JSON
  （`{"k":{"k":…}}`）会让读取它的那个进程直接消失——nlohmann 的解析器自己是状态机，扛得住的是解析，
  扛不住的是随后那个几万层 DOM 的递归析构，以及我们把嵌套结构摊平成点分键的那趟递归。现在 JSON 侧也
  挂上深度闸门（挂在解析回调里，DOM 建到第 129 层就抛出），与 YAML 侧共用同一道上限
  （`kMaximumDocumentDepth = 128`，原先只写着 YAML 一名，故连带把翻译单元内的异常类改名为
  `DocumentConversionException`）。两条新用例各在同目录放一份层数正常的对照文件并断言它照常读入，
  以免「拒绝」其实是「我造的文本本身不合法」；证伪就是撤掉闸门——JSON 那条会让测试进程直接消失。
- **Base：`getSection()` 遇到「同一个名字既配成标量、又当分组」时报错，不再静默丢一边**。段落还原先逐个
  写叶子、再按分组递归覆盖回去，于是 `server.port: 8080` 与另一份文件里的 `server.port.forwarded: true`
  同时存在时，`getSection("server")` 里根本没有 `port`，而 `getInt("server.port")` 照旧给 8080——
  同一个键两套答案，配置文件本身看不出任何错。现在这种形状抛 `ConfigValidationException`，点名到真实键
  并给出改法（改其中一条的名字，或再下一层段落）；`echo_server` 读 `server` 段那条路径本来就在
  `catch (std::exception)` 里报「配置读取失败，服务未启动」，于是退化成一个带原因的启动失败。
  新用例 `GetSectionReportsLeafVersusGroupCollision` 同时钉住同目录另一段照常还原；证伪：摘掉那道
  `contains` 判定，用例落红在「必须报错，而不是丢掉其中一个」。
- **Base：日志器退休时当场交还 Sink，文件句柄与后台线程不再等 purge**。`registerLogger` 同名覆盖、
  `unregisterLogger`、`clear()` 三条路原先把日志器连着它的 Sink 一起留在退休表里，于是反复重配日志会把
  打开的日志文件句柄与 `AsyncSink` 的工作线程一路攒到进程退出——除非调用方记得手动
  `purgeRetiredLoggers()`，而「记得」本身就是陷阱（临时目录删不掉那次事故就是这么来的）。退休表要护住的
  只是 `getLogger()` 交出去的裸引用，Sink 从不靠它：在途写入者握着快照的强引用，交还之后要等最后一份
  引用释放才真正销毁。现在退休那一刻就 `clearSinks()`（放在注册表锁**外**做，`AsyncSink` 析构要 join），
  表里只剩外壳。契约随之明确：注销之后再经旧引用写入的日志不落地，即「注销即停止输出」。两条新用例分别
  钉住「不必 purge 就释放」与「写入者还站在 Sink 里时不许把 Sink 抽走」（后者用 promise 造出确定的重叠，
  不赌调度）；证伪：把交还那一步摘掉，两条一起落红。`base_log` 示例改成「注销后立刻删文件」并在 Windows
  实测通过。Debug（含 ASan）521 例、Release 516 例全绿。
- **Base：只有注释行的 YAML 不再被判成坏文件**。空文件一直是「不产出配置项、也不算失败」，而一份
  只写着 `# 说明` 的占位文件解析出来是「无内容」的空文档，走的却是「根节点必须是映射，实际为 null」
  那条报错：它进 `failedFiles`、整轮加载随之报 `success=false`，调用方按「配置有问题」处理。发布流程
  里放一份只写说明的 `overrides.yaml` 是常规做法，报它失败等于逼运维删掉那份说明。现在空文档（注释行、
  显式 `~`/null）与空文件同口径，标量根与数组根照旧拒绝——放行判据是「文档没有内容」，不是「读不出来
  的都算空」。新用例把占位文件与一份真配置放同目录，断言照常收进 `loadedFiles` 且不贡献键；证伪是
  撤掉放行：报「只写注释的文件被判成了坏文件」，并读到 loadedFiles 只有 1 份。
- **Base：滚动清理切备份名中段前先判长度**。备份名要求文件名以「name.」开头、以原扩展名结尾，随后
  按下标切出中间那段——两者之和可以比文件名本身还长（大小策略留下的 `app.log` 交给按天策略清理时，
  长度 7 小于前缀 4 加后缀 4），无符号减法回绕成天量长度，切出的视图越过串尾读堆。本机没报出来只是因为
  遍历恰好在 `'\0'` 那一步判假提前退出；换分配器就可能把无关文件误认成备份删掉。判据取「长度不够就
  不是本策略的备份名」。**这条没有可证伪的用例**（触发要求同一基础名先后跑过两种策略，而清理只在滚动
  那一刻执行），按算术缺陷处理，形态一侧仍由既有的时间戳备份清理用例守着。
- **Base：`enableHotReload()` 重复调用时，这一轮的回调与防抖间隔照样生效**。「已在监听」那条早退
  原先直接 `return true`，把这一轮传进来的 `std::function` 整个丢掉——调用方拿到 true，却永远等不到
  自己新给的接线；想换接线只能先 `disableHotReload()` 再 enable，而返回值与签名都没提示这一层。现在
  回调快照在判定是否已启用**之前**发布（它本就是原子装的 `shared_ptr<const function>`，读侧
  release/acquire 取用），防抖间隔跟着换（`FileWatcher` 里那是原子量，运行中可写），监视器不重起。
  新用例先等第一次接线出声、再换回调并断言第二次出声；证伪是把发布挪回 CAS 之后——那条等待 8 秒超时落红。
- **Base：异步日志不再把唤醒错发给「还在等空位」的写入者**。生产者（Block 策略下等位）与消费者
  （worker 等事件）原先共用一条条件变量，两侧都用 `notify_one`：入队那次唤醒有概率落在某个等空位的
  写入者身上，它复检「还有空位」为假便再睡下，唤醒就此被吞掉——事件已在队列里、worker 还在睡。之后
  每条写入都要付满 Block 的 200 ms 上界并计入丢弃，`flush()` 更等不到 pending 归零。现在按谓词拆开：
  入队只叫「有事件」那条，出队只叫「腾了位」那条，停止时两条都 `notify_all`（停止标记仍在同一把锁下
  发布）。**本机没有确定复现的时序**（`notify_one` 选谁由实现决定），因此按「已知反模式 + 循环验证」
  处理：AsyncSink 与 Logger 两组用例在 Debug（含 ASan）下连跑 40 轮全绿，全量 Debug 523 例、
  Release 518 例全绿。
- **Platform：本地时间折不出挂钟时交回零值日历，不再把 -1 填满的结构交给调用方**。
  `PlatformTime::localTime()` 的契约写着「转换失败返回零值结构」，实现却没看返回码：MSVC 的
  `localtime_s` 失败时把整个 `tm` 填成 -1，超出 CRT 支持范围的时刻因此在 Windows 上渲染出
  「1899-00--1 -1:-1:-1.477」这种带负号与空字段的时间戳，而 glibc 侧同一输入给的是全零日历——
  只有跨平台比对照才看得出来。姊妹出口 `utcTime()` 早已判返回值，本地时间这条漏了。现在两侧都判
  失败并交回零值结构。两条新用例分别钉失败侧字段与消费侧版式；后者在修好之前是红的，失败信息里
  打出的正是那串 `1899-00--1`。
- **Base：时间戳的亚秒残差不再经由两个 time_point 相减求得**。那条减法的公共单位是两者中更细的
  一个（本平台是纳秒或 100 纳秒），整秒那一侧要先乘到该单位才可比：在 `time_point::min()` 上这是
  `-9223372037 * 1000000000`，超出 `long` 范围，容器 UBSan 实测两条报告都落在 `chrono.h` 里。改成
  先 `duration_cast<milliseconds>` 取毫秒刻度、再减去整秒×1000——一路只有除法与窄一级的乘法。
  证据口径如实写明：两补数回绕在 gcc/x86 上恰好算回正确值，因此这条由「同一套源码换掉这一处实现
  前后，UBSan 从两条报告变零条」判定，不拿用例冒充证伪。
- **TLS 会话释放之后的收发不再把原因推给对端**：`close()` 会释放底层 SSL 对象，此后
  `handshake()`/`asyncReceive()`/`asyncSend()` 仍把空指针交给 OpenSSL。实测（临时摘掉闸门跑新用例）
  OpenSSL 3 不崩溃而是返回失败，错误队列里留下的是 `error:00000000:lib(0)::reason(0)` 这种没有内容的
  原文，而三条路径的兜底文案一律写着「多半已被对端关闭或 TLS 会话已失效」——真实起因往往是我方
  空闲清扫或优雅收口关掉了连接，排查的人会顺着错误文案去查对端。现在闸门放在每条循环的开头，
  一处实现覆盖三个入口，协程在任何让出点（等可读、等可写、为反方向让出一次调度）被关停后回到循环
  都会被复查；`asyncSend` 原先只有 `WANT_WRITE` 那条分支做了复查，`WANT_READ` 那条漏了。
  顺带统一两处文案归属：读路径的「等待可写期间套接字被关闭」写成了「TLS 握手失败」，
  写路径的「等待可读期间套接字被关闭」写成了「TLS 读取失败」。
  用例 `TlsSocket.EverySslEntryPointRejectsAReleasedSession` 双向验过：摘掉闸门它红（三条入口都拿不到
  本端原因那句），装回全绿。
- **聚合发送的快照不再把空段交给一次提交**：`VectoredSendCursor::snapshotPending()` 的契约是
  「跳过已发完与零长度的段」，但实现只在首段那一处判了，游标之后的段按整段原样拷贝，夹在中间的
  空段于是照原样进一次 `WSASend`/`writev`。危害不在字节正确性（空段不写任何字节），而在提交位：
  一次提交就 16 个位子，空段占掉一位会把真正待发的段挤出去，多付一次系统调用。
  框架自己的 HTTP 出口避开了空段（无正文时只提交头部一段），但 `asyncSendVectored()` 是对外接口。
  新用例把空段夹在两段真实数据之间，并额外用「容量刚好等于真实段数」那一档钉住占位；
  摘掉那句跳过判定后该用例当场报「交出 4 段而非 3 段」（已实测证伪）。
  既有的 `ZeroLengthSegmentsAreSkipped` 只把空段放在第 0 段，恰好落在首段裁剪分支顺手处理掉的
  位置上，所以这条缺口一直没被覆盖到。
- **按来源 IP 的限额不再把同一来源的两种地址写法当成两个来源**：双栈监听器（本框架显式关掉
  `IPV6_V6ONLY`，好让一个端口同时接住两族）上，IPv4 客户端的对端地址族是 AF_INET6，`InetAddress::ip()`
  因此给出 `::ffff:a.b.c.d`（Windows 与 glibc 实测同一形式），而纯 IPv4 监听器给的是 `a.b.c.d`。
  两类监听器共用一份限额时——正是多监听器共享计数的推荐接法——同一来源各占一格，「单个来源」的上限
  实际翻倍。现在记账与查询都先折掉映射前缀：只有前缀之后确实是合法点分四段时才折，`::ffff:1:2` 这类
  非 IPv4 文本保持原样，免得把不认识的写法强行并格。用例
  `PerIpConnectionLimiter.MappedIpv4FormSharesSlotWithPlainForm` 钉住两写法共用一格，
  `ReleasedSlotIsReusableAndFoldingStaysStrict` 钉住归还后换写法能重新进入、以及折键的边界；
  `TcpServer.DualStackListenerAccountsIpv4PeerAsDottedSource` 走真实接受路径端到端复核：服务器绑 `::`、
  客户端连 127.0.0.1，去掉折键后它红在「点分那一格读到 0 条」，即这条前缀确实出现在对端地址里、
  也确实另占了一格。
- **HTTP/1.1 的在途正文预算撑不住「处理器还在跑」这段时间**：预留量每轮按解析器缓冲的正文数补，而
  收齐那一轮解析器已把正文移交给请求对象，缓冲量随即归零——正文还占在内存里、额度却已经还清，于是这笔
  跨连接的总量上限在最该生效的那段时间里等于没设：第二条连接照样能塞满自己的正文并拿到 200（改动前实测：
  预算 100 字节、两条连接各压 90，第二条无人拦，此刻额度读数是 0 而不是 90）。改为按「解析器缓冲」与
  「请求对象已持有正文」的较大者预留，与 h2/h3 的「已持有 + 新到」同一口径——那两个分支一直是对的，
  漏的只有 h1，而它自己的注释写的就是「应答完归还」。用例
  `HttpMemoryBudgetTest.RejectsSecondConnectionBodyWhileFirstIsStillHeld` 钉住这一条；对照用例
  `ReturnsQuotaBetweenKeepAliveRequestsOnOneConnection` 钉住反方向：额度也不能拖到应答写完都不归还，
  否则同一条 keep-alive 连接发第二个正文，会被自己上一单的额度误判超预算。
- **master 被强杀后多进程 worker 不再变成孤儿**：Linux 上 `Process::spawn()` 的子进程现在带着
  `PR_SET_PDEATHSIG`（父进程一退出就收 SIGTERM，非 setuid 的 exec 之后仍然有效），并在设置前后各查一次
  `getppid()` 补上「fork 到设标记之间父进程已退出」这段窗口。实测（容器，`--workers 3` 起服务后
  `kill -9` master）：改动前 3 个 worker 仍活着、端口上仍挂着 3 个监听器，再没有谁管它们；改动后活进程
  0 个、该端口监听 0 个（留下的只有等 pid 1 收尸的僵尸，那是容器 init 的行为，不是本层的）。
  信号选 SIGTERM 而非 SIGKILL，worker 仍有机会把在途请求做完。
- **析构兜底也会等强杀的 worker 被收尸**：`~WorkerSupervisor()` 原来 `forceTermination()` 之后直接释放
  句柄，丢掉 pid 等于留下没人收的僵尸；现在与收尾路径共用同一段有界等待
  （`waitForForcedTerminationsToLand()`），两处不再各写一份轮询。
- **共用一个 UDP 端口的多个监听器不再只有最后一个收得到报文**：`DatagramSocket::bindTo()` 原先只设
  SO_REUSEADDR，内核让每个监听器都「绑定成功」，却把全部报文交给最后绑上的那一个——`--workers 3 --h3`
  时前两个 worker 一句错误都不报、一条报文也收不到（容器内核实测 24 条流的分布是 0/0/24，补上
  SO_REUSEPORT 后变成 9/6/9）。TCP 侧的 `TcpAcceptor` 早就设了这个选项，缺的是数据报这一半。用例
  `DatagramSocket.SharedPortSpreadsDatagramsAcrossListeners` 钉在「收到过流量的监听器个数 > 1」而不是
  「绑得上」——后者删掉这次改动也照样成立。
- **分离协程里没人接住的异常落到错误日志，启动失败不再静默**：`Task` 的 promise 只把异常存下来，被
  `schedule()` 投递又没人 await 的协程抛出异常就等于彻底消失，而 `TcpServer::start()` 与
  `QuicServer::listen()` 的绑定失败正好是这个形态：日志照旧写「server started」、退出码 0，端口上却没人
  守着（拿一个不属于本机的地址即可复现）。现在协程终结点上若异常没有等待者就记一条带抛出点与调用栈的错误；
  `echo_server` 另在启动后按有界轮询确认每台监听器真的进入监听态，确认不到就报「服务未运行」并以退出码 1
  收场。判据见 `Task.DetachedTaskReportsUnhandledExceptionToLogger`，对照例
  `Task.AwaitedTaskRethrowsWithoutReportingToLogger` 保证有人接住的异常不会被重复报。
- **在途正文预算不再因一次错账翻转成「放行一切」**：`HttpMemoryBudget::release()` 原来是无符号
  `fetch_sub`，归还多于账目时把已预留量绕成天文数字，而 `tryReserve()` 判的是「上限减当前值」——
  跟着回绕之后剩余额度变成巨大，这道跨连接的限额当场失效（比账目偏小严重得多）。现在按 0 收住并记
  一条中文警告（这条日志本身就是「某处重复归还」的证据）。用例 `OverReleaseIsClampedAndKeepsTheLimit`
  钉住：多归还之后账目为 0、上限依旧拒绝对端的下一个字节。
- **HTTP/3 排队中与正在服务的正文都算进在途预算**：额度原来随「请求被排进待派发队列」就归还
  （`IncomingRequest` 在 `enqueueRequest()` 末尾析构），于是排着的正文与正在跑处理器的正文都不再被记账，
  多条流各自压一份正文就能把实际占用推过上限。现在额度随待派发记录一起活着、接到服务作用域里，这一条
  应答完才归还，与 h1「应答写完后归还」、h2「记录摘掉时归还」同口径。判据见
  `Http3Session.KeepsInflightBudgetHeldWhileRequestIsQueuedAndServed`（排队时与处理器进门时各查一次占用）。
- **接手一条取不到对端地址的描述符不再带走整台服务器**：`TcpServer::takeOverConnection()` 里
  `remoteAddress()` 在 `getpeername` 失败时会抛，而它当时排在保护 `createConnection()` 的 try 之外——
  一条坏描述符就能让接受循环退出，之后所有来源都没人接。现在两者同处一段，异常只丢这一条连接并记中文
  错误；`adoptConnection()` 也如实返回 false（原来即便因过载或超限丢弃也报「已接手」，与它的文档契约相反）。
- **HTTP/3 的连接收口会等完业务处理器**：QUIC 连接被空闲超时（或 CONNECTION_CLOSE）收掉时不会逐条
  流发 RESET_STREAM / STOP_SENDING，会话因此收不到「这条流结束」的信号——承载层摘掉 `Http3Session`
  的那一刻，挂在 `bodyStream()->readNext()` 或隧道 `receive()` 上的处理器协程帧被连着销毁，等待之后
  的收尾代码不再执行，与 HTTP/1.1、HTTP/2 两处修过的是同一族缺陷。新增
  `Http3Session::abandonPendingStreams()`：把这些流走一遍与「对端取消」同样的回收（只省去已经无处
  可发的 RESET/STOP）并当场唤醒等待者；承载层每拍调一次，等 `hasOutstandingWork()` 归零才摘连接。
  这条前提同时也是 h3 能走「压缩交给工作线程」的原因——处理器可能在恢复动作已投递给循环之后再挂起。
- **HTTP/2 的 WebSocket 隧道收尾会等完业务处理器**：RFC 8441 扩展 CONNECT 建起来的隧道把业务协程
  作为局部 `Core::Task<>` 启动后从不等待它——隧道读循环一退出（对端直接断开、传输层失败、发送失败
  都算），那个局部 Task 就随隧道协程一起析构，而 `Task` 的析构是无条件 `destroy()`。挂在
  `receive()` 上的处理器因此被连帧一起丢弃：等待之后的代码与帧内对象的收尾全部不执行，而任何已经
  投递给事件循环的恢复动作会指向已释放的帧。现在与 HTTP/1.1 侧同一顺序收口——先置关闭、唤醒等待者，
  再等业务跑完，然后才发本侧的 END_STREAM（业务自己的末帧因此还出得去）。
- **Windows 文件监视不再静默丢事件**：多个线程同时往一个被监视的目录里写文件时，通知条数超过接收
  缓冲区能装下的量就会被直接丢弃，而 Windows 不一定报 `ERROR_NOTIFY_ENUM_DIR`——实测 4 KiB 缓冲下
  1200 个文件会不声不响地少掉 17~19 个（三次运行各自如此），热重载因此停在旧配置且无人知晓。
  缓冲区改为 64 KiB 后连跑 10 次一条不丢。代价是每条监视一份缓冲区、常驻内存按监视目录数线性增长
  （配置目录这类用法以 KiB 计）。
- **Windows 文件监视不再被「搬走的目录」挡住**：被监视的子目录改名走开时，它自己那条
  `ReadDirectoryChangesW` 的句柄会跟着目录一起搬走——条目既不会读失败也不会自己消失，只会按注册时
  的旧前缀派发路径（派发出去的位置上坐着的其实是另一个新目录），并且占住这个键，使原地重建出来的
  同名目录在 `addWatch()` 上被「已在监听集合」挡下、返回 true 而什么都不监视。现在按父目录报出的
  旧名/新名配对，让监视跟着目录走，重建出来的同名目录由按秒节拍的自愈复查补挂；递归监视枚举出来的
  子目录也一并纳入这份清单，因此 `removeWatch()` 会连整棵子树一起撤销。改名与重建被通知缓冲区溢出
  拆进两批时配不上对，目录被搬出递归范围后仍按新位置继续监视，是本次留下的已知边界。
- **`addWatch()` 指向普通文件时当场报失败**：`CreateFileW` 带着 `FILE_FLAG_BACKUP_SEMANTICS` 打开
  普通文件是成功的，拒的是后面的 `ReadDirectoryChangesW`，而原先丢弃这次投递的结果、条目照样登记。
  这样的条目既不在等待集合里也没有人会再给它投递一次，于是永久占住该路径：同一路径换成真目录后
  `addWatch()` 被去重挡下并返回 true，配置热重载因此静默失效（`ConfigManager` 拿到的只是个假的成功）。
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
  `IN_IGNORED` 的同一方式摘掉映射（递归根交给自愈复查补挂）。Windows 上同一形状的症状由另一条修法解决：
  监视条目按父目录报出的旧名/新名配对换键跟随目录，原地重建的同名目录再交给按秒节拍的自愈补挂，
  两侧各自有用例钉住（Linux `RecreatedDirectoryCanBeWatchedAgain`、Windows
  `RenamedAwayWatchedDirectoryIsRewatchedAfterInPlaceRecreation`）。
- **文件监视：换掉 inode 的单个文件会被重新挂上监视**（仅 inotify 侧）。inotify 的 watch 挂在 inode 上，
  监视对象自己消失时内核补一条 `IN_IGNORED`、本端随之清掉路径映射；调用方只监视这一个文件（父目录不在
  监听集合里）时，没有任何人会再为这条路径调 `addWatch()`，编辑器式原子保存之后的变更从此永久丢失。
  原先按秒节拍的自愈只复查「以递归方式注册的根」，现在它走的是一份明确的自愈清单：记下调用方请求过的
  每一条路径（含非递归的目录与被删后又放回来的目录），按登记时的 `recursive` 值补挂。递归枚举出来的
  子目录不进这份清单——它们失挂时父目录会收到 `IN_CREATE` 并由那条事件补挂，收进来只会把每秒一次的复查
  变成上千次系统调用。用例先确认单文件监视本身有效，再删掉文件、换上新 inode 反复写入；判据只数
  `Modified` 事件，因为删除自身还会留下一条 `Deleted`，把它算进来等于「监视失效也能通过」。Windows 不
  监视普通文件；另一处两侧不对称（非递归的目录监视在 Windows 不被自愈补挂）由下方
  「非递归的目录监视在 Windows 也会被自愈补挂」那条收口。
- **改名落位的文件变更在 Windows 上按 `Created` 上报**，与 Linux 同口径。`FileChangeType::Created` 的定义是
  「文件被创建或原子替换后落位」，Linux 的 `IN_MOVED_TO` 走的正是这一条；Windows 却把
  `FILE_ACTION_RENAMED_NEW_NAME` 与 `FILE_ACTION_MODIFIED` 合并映射成 `Modified`。同一个「写临时文件再改名
  覆盖」的发布动作因此在两台机器上给出不同类型，按 `Created` 分支的消费方在 Windows 上走不到；递归监视给
  新目录补挂监视的判据同样只看 `Created`，改名落位的目录因此不在补挂之列（实测从监视树外移入时内核报的是
  `FILE_ACTION_ADDED`，那一路径本来就被覆盖，用例钉住了这一点）。
- **非递归的目录监视在 Windows 也会被自愈补挂**，与 Linux 同口径。自愈原先只复查「递归监视覆盖到的目录」，
  调用方按 `recursive=false` 注册的那一条不在其中：目录被删掉再放回时旧监视随句柄失效，重建出来的目录没有
  人会再调 `addWatch()`，其内部变更从此不上报——「把一个目录换掉后再放回来」正是常见的发布动作。现在调用方
  点过名的路径另记一份清单，按它当时的 `recursive` 值补挂（给一条只要一层的请求按递归挂上会让监视范围越滚
  越大），目录被改名搬走时清单跟着搬到新位置、旧位置刻意留着以便原地重建时再补一条。用例
  `NonRecursiveWatchIsRewatchedAfterDirectoryIsReplaced` 两侧都编译：改前 Windows 报红、Linux 报绿（Linux
  侧在本轮的自愈清单改造后已覆盖这一条），改后两侧皆绿。
- **把配置文件改名挪走会触发热重载**（Windows）。消费方原先只接受 `Modified`/`Created`/`Deleted` 三种事件，
  而 Windows 把「改名走开」报成 `Moved`（源路径）、Linux 的同一条动作被映射成 `Deleted`：Windows 上旧名被
  类型判据滤掉、新名因不是配置后缀被扩展名判据滤掉，一次通知都不算数，配置因此停在已经消失的那份上直到
  下一次改动。现在 `Moved` 也算一次配置变更，两侧行为一致。
- **`ProcessInfo::applicationDirectory()` 不再交出一个被截断的目录**（POSIX 侧）。`readlink("/proc/self/exe")`
  在缓冲不够时返回的是「填满的字节数」且不补零终止，原实现按 `size - 1` 传缓冲又不过判这一步，于是路径
  超过 4095 字节时返回一份被剪短的字符串，它的 `parent_path()` 指向一个不存在的位置。现在按整个缓冲长度
  请求，返回值为满即判定截断、按「取不到」处理（本函数不抛异常，与 Platform 其它封装一致）。这条只有
  POSIX 分支，构造不出确定复现的路径长度，改动以「容器可编译、逻辑正确」验收。
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
- **Database：连接池不再握着空闲栈的锁去断开连接**：`disconnect()` 是一次会阻塞的系统调用（驱动里
  还要发一条 Quit 并等它走完），而两处丢弃连接的出口把它留在了 `m_mutex` 的临界区内——`tryAcquireInternal()`
  在锁内弹出后立刻判过期、判存活，不合格就在锁内断开；`returnConnection()` 的「超过最大存活期」与
  「`maximumLifetimeSeconds == 0`（一归还就过期）」两条分支同样在锁内断开，后者意味着每次归还都触发。
  后果是队头阻塞：一次慢关闭（对端不应答、驱动超时）会把所有 `acquire()` 快路径、`tryAcquire()`、
  `idleCount()`/`totalCount()` 与后台驱逐一起堵在那把锁上。这条纪律其实早已写在 `healthCheckLoop()` 里
  （「锁内只做摘出与计数，disconnect 必须留到锁外」），只是那两个出口没照做，而 `acquire()` 的等待路径
  反而是对的——同一件事在三处给了两种答案。现在两条出口都改成锁内只判定与摘出、出锁再断开，
  `closeTrackedConnection()` 的契约也写明调用方不得持有 `m_mutex`。
  回归用例 `ReturnPathDoesNotHoldTheIdleStackLockAcrossDisconnect` 与
  `AcquirePathDoesNotHoldTheIdleStackLockAcrossDisconnect` 各钉一条出口：打桩连接的 `disconnect()` 停在
  门闩里，另一个线程去读统计。重叠条件由用例自己造（只在门闩确认「确实停在断开里」之后才去碰锁），
  因此不赌调度。容器 GCC + ASan/UBSan 下先证伪：撤掉修复时两条都红在「观察者 200 ms 内读不到锁」，
  装回修复后 60 次连跑零红。协程等待表那把 `m_asyncMutex` 仍会罩住一次断开——取出与入表必须同锁
  才不漏唤醒，这笔代价留在该锁内（它只挡异步等待路径，不再挡整个池）。
- **HTTP/3 的响应头部与 h1/h2 逐字同源**：同一份业务代码此前换个协议会得到不同的响应头。
  多条 `Set-Cookie` 只发得出第一条（`HttpResponse` 的头视图是「一名一值」，逐条取值要走
  `headerValues()`）；不补 `date`（RFC 9110 §6.1 要求源服务器给出）；有正文却没设媒体类型时
  不补 `text/plain`；越界的状态码原样交给连接层，而 `:status` 必须是三位十进制，于是
  整条响应的字节都发不出去，对端干等到超时（现改回 500 并记日志，与 h2 同口径）。
  普通响应与流式响应头这两处采集原本各写一遍、口径已经漂移，现收成一个。
- **示例程序里 h3 不受限流、也不占在途正文预算**：`--rate-limit` 的令牌桶只挂在两条 TCP
  通道的路由上，同一来源走 h3 就不限；`QuicServer::Configuration::memoryBudget` 会被转交给
  每个 h3 会话，样本却没给，于是 `--max-inflight-body` 对 h3 完全不生效。两处都接上同一份对象。
- **HTTP/3 的出站正文不再能被慢读者无限堆进本端内存**：会话侧那道背压闸门
  （`kStreamingResponseBufferByteCount`，256 KiB）按「交给传输层即视为排空」记账，而 QUIC 流层的
  `writeStreamData` 无条件整段收下——排空只发生在编帧那一刻，编帧要等对端给窗口。于是对端只要
  **只收不授窗口**（慢读者或恶意连接），正文就一段一段堆在流层的待发队列里，一字节也没上线，
  而闸门看到的待发字节始终是 0，生产者因此永不挂起。同样的线在 h2 侧早有 1 MiB 上界、WebSocket
  入站有 16 MiB 上界，只有这一条漏了。现在流层给每条出站流的队列设 1 MiB 上界并**部分接收**
  （返回被收下的字节数，收尾标记只在整段收下时落定），HTTP/3 连接层把没收下的余数连同 END_STREAM
  留在自己的缓冲里等续交，会话那道闸门因此量的是「本端还占着多少字节」而不是「交出去多少」。
  对端只回窗口更新、不发数据时不会触发任何接收回调，故承载层在队列被排空后主动催一次续交——
  少了这一笔，挂在闸门上的生产者会一直睡下去（丢唤醒）。
  单流那道闸还要乘上并发流数（本端默认 100 条），逐流各卡一点就绕得开它，因此两条传输各再补一道
  **连接级总量闸**（同取 8 MiB）：QUIC 在流层按各流待发队列之和拒收，HTTP/2 在会话侧按各流排队
  正文之和拒收；h2 那两处写正文的出口（流式正文与 WebSocket 隧道帧）改走同一个判据方法，
  免得同一阈值在两个消费点各自解析成两样。
- **HTTP/2 的超大请求头只作废那一条流，按 431 应答**：HPACK 解码器过去把「超出本端头部上限」和
  「压缩上下文解坏了」当成同一件事——越限时立刻返回失败并置粘滞标记，连接层于是对两种失败一律发
  GOAWAY。粘滞是真问题：解码器一早就退出，头块剩下的表示没被处理，那些带增量索引的条目没进动态表，
  两端的表就此错开，此后每个头块都会解成压缩错误——所以当时的判断「只能终止连接」在自己那一版里
  是自洽的。错在把上限判断放在了插表之前。RFC 9113 §10.5.1 给的处置本就轻得多：收不下这一条头块
  就回 431（RFC 6585），并且**头块必须处理完以保证连接状态一致**。现在解码器把两类失败分开：
  越限时记下原因、到此为止不再收字段，但把剩余表示照常解完（动态表照旧演进），且**不置粘滞标记**；
  连接层据 `isLimitExceeded()` 分流——压缩错误仍按 §4.3 终止连接（那是唯一自洽的收场），
  越限则把这条请求交给会话层按 431 应答并沿用 413 那套「应答完请对端别再发正文」的出口。
  改前的后果是可以远程发的：一条超大头部带走整条连接，同一条连接上别人正在跑的请求一起陪葬，
  而这正是长连接与多路复用最该有的反面。curl（nghttp2 1.59）实测在改前拿到的是连接被切断（退出码 56）。
  尾部头块越限另走一支：响应已经发出、没有 431 可回，按流错误（ENHANCE_YOUR_CALM）收掉这一条流。
  新增 `Hpack.KeepsDynamicTableSyncedWhenHeaderLimitsRejectTheBlock`（判据是「被拒绝块里最后插入的那项
  随后能按索引解回来」，表若没同步这一步就会解成压缩错误）、
  `Http2Connection.RejectsHeaderListBeyondTheAdvertisedLimitOnThatStreamOnly` 与
  `Http2CleartextSession.Answers431ForOversizedRequestHeadersAndKeepsConnection`（真回环 socket 上越限
  请求回 431、随后那条正常请求仍拿到 200）。三条用例在把实现改回旧口径后全部转红。
  `Hpack.RejectsHeaderListAndFieldLengthLimits` 的断言由「失败必粘滞」改为「只有压缩错误粘滞」。
- **构造失败的 QUIC 服务端不再漏掉整份 TLS 上下文**：`QuicServer` 的构造函数第一件事就是
  `SSL_CTX_new`，其后还有四道检查会抛（限 TLS 1.3、证书链、私钥、私钥与证书是否配对），而抛出去之后
  析构函数根本不会跑——存在成员里的那份裸 `SSL_CTX *` 就此无人认领。量级是一份上下文连带证书与私钥
  约 16.6 KiB：`net_http3_demo` 自检 14 步全绿的最后一次跑里，LSan 报了 38 744 字节 / 549 次分配，
  三处 `SSL_CTX_new` 全落在构造上。按「证书轮换失败就重试」的部署写法，这是随重试次数线性增长的泄漏。
  改成先由一个局部 `unique_ptr` 守卫持有，四道检查全过才 `release()` 交接给成员。
  新增 `QuicServer.ReleasesTlsContextWhenCertificateValidationFailsDuringConstruction` 直测三个抛点
  （缺证书文件、缺私钥文件、两个文件都在但不配对——最后一道是构造里唯一的配对检查）：「抛」由用例钉，
  「漏」由容器的 LSan 门禁钉。把所有权交接挪回守卫之前（等于旧写法）时该用例断言全过、而进程以
  LSan 报出 99 611 字节 / 1468 次分配非零退出——这条用例的证据力在退出码里，只盯 `[ PASSED ]` 会漏过去。
- **示例的启动期报错不再依赖 `<print>`**：`samples/common/SampleSupport.h` 用 `std::print` 打两条
  命令行报错，而 `<print>` 在 GCC 13 上还不存在（标准库实现要到 GCC 14 才补齐）。本仓库拿 ubuntu24
  容器（GCC 13.3）当 LSan/UBSan 门禁，于是 `echo_server` 与三个 `net_*_demo` 在那台工具链上
  **整体构建不过**，进程外探针（h2c 对手、h3 验收与取消）只能跑在旧二进制上、量不到改后那一半。
  文案改由 `std::format` 拼、`printStartupError` 单点输出到 stderr（`<format>` 两侧都有），
  输出字节与退出码逐字不变（Linux 上实测：缺取值与越界取值两条文案照旧、退出码照旧是 2）。
  这条仍是「C++23 里也不是所有设施都能用」的老问题：判据是 `__cpp_*` 特性宏与工具链实测，不是标准版本号。
- **响应头越过对端通告的头列表上限时只作废那一条流**：本端编响应头之前完全不看对端的
  `SETTINGS_MAX_HEADER_LIST_SIZE`，而这一项约束的正是「对端将收到的头列表」（RFC 9113 §6.5.2）。
  一条路由交出超长的响应头（塞大量 `set-cookie` 就会）即违反对端策略，而多数客户端对此类响应的处置是
  收掉整条连接——同一条连接上别人在途的请求一起陪葬，一处业务配置失误被放大成服务端断连。
  现在按 §6.5.2 的算式（每个字段 `名长 + 值长 + 32`，`:status` 这一项也算）在编帧前判一次：
  越限即由连接层按 `INTERNAL_ERROR` 中止这一条流，并给上层一个新结论 `HeaderListTooLarge`，
  连接照旧服务其它流。对端没通告这项时不判定（初值是「不限」，且规范把它定为建议值），行为逐字节不变。
  会话层为这条新增 `StreamFailed` 出口并单独记一行错误日志——不复用 `StreamCancelled`，
  否则会把本端的失误写成「对端取消了这条流」。新增
  `Http2Connection.RefusesResponseHeaderListBeyondThePeerAdvertisedLimit`（断言恰好一条 RST_STREAM、
  没有 GOAWAY、同连接另一条流照常 `Sent`、越限那条随后 `StreamNotWritable`）与
  `Http2Connection.StillSendsOversizedResponsesWhenThePeerAdvertisesNoHeaderListLimit`；
  把守卫条件改成恒假（等于旧写法：不看对端通告照发）时前一条转红。
  容器（GCC 13.3，ASan/LSan/UBSan）实测 `TestNet` 1240 例全绿、零告警、sanitizer 命中 0，
  h2c 对手探针 `scripts/h2_adversarial_probe.sh` 十项照旧全绿。
- **日志配置的「类型不符」与「非法枚举」不再静默按默认值生效**：`LoggerConfigLoader` 对越界取值
  （`max_size_mb`、`max_backup`、`queue_size`、`overflow_policy`）都会报出非法值与生效值，但同一份
  配置里另有四条通道一声不响——可选字段**类型不符**（YAML 给数字或布尔加了引号，于是 8192 变 1024、
  写 no 的 `color` 变彩色、写 false 的 `truncate` 变追加）、`policy` 拼错按 size 滚动、
  `global_level` 配错类型按 INFO，以及必填键把「存在但类型不符」报成「缺少字段」（照着提示补键反而
  补不对）。现在类型不符会连「实际类型 vs 期望类型」一起报出（类型名复用 `typeName`，与 ConfigManager
  的诊断同一套词汇），必填键区分两种失败原因；日志目录与文件路径全程按 `std::filesystem::path`
  传递，去掉中途 `.string()` 那一次本地代码页往返（落在代码页外的字符会被换成 `?`）。
  新增 5 条用例各钉一处，均走既有的标准错误捕获夹具；把 `policy` 的诊断改回静默赋值，只有那一条转红。
- **`SystemException` 不再把 `errno` 冒充 Win32 错误码**：隐式错误码那条构造函数读 `errno` 却配
  `std::system_category()`，而 Windows 上这个类别按 **Win32 码**查表——更要紧的是 kernel32 与 winsock
  的失败根本不写 `errno`，于是 `what()` 给出的是一句与本次失败无关（或干脆是「操作成功完成」）的描述。
  同一条教训本仓早已写在 `AsyncSocket` 的注释里（实测发送失败被报成「[112] There is not enough space
  on the disk」），只是没接到这条通道上。现在 errno 与 `std::generic_category()` 配成自洽的一对
  （经新增的 `SystemException::lastErrnoErrorCode()` 单点读取），四个「刚做完 Win32/socket 调用」的
  抛出点改为显式传码：`Iocp` 的创建完成端口与等待完成端口（后者上一行刚取到 `GetLastError()`，
  抛出去却报 errno）、`AsyncSocket::remoteAddress()` 与 `localAddress()` 的 `getpeername`/`getsockname`。
  `NetworkException` 不显式传码的那条重载顺带补上对端地址——同一次失败不该因调用方手上有没有错误码
  而给出两种文本。新增两条用例分别钉「类别与取值配对」与「两条重载都带对端」，前者在 Linux 上两个
  类别文本相同、只有 Windows 侧能证伪；同时把「采到的是打印点的栈」那条**假断言**改成按帧身份判
  （旧写法把 `captureStackTrace(1)` 改成 0 也照样全绿，改后同一处改动立即转红）。
- **schema 的区间约束不再让 NaN 与 ±Inf 冒充「已校验」**：给某个键设了上下限，而取值是非有限数时
  一次比较都成立不了——NaN 与任何界限比较两侧都是假，既不算低于下限也不算高于上限；只设一侧界限的话，
  另一侧的无穷大天然落在界内（正无穷过下限、负无穷过上限）。于是这类取值带着「schema 已通过」的标签
  流到下游，而 YAML 里写 `.nan`、`.inf` 就能造出这种取值：这是一处 fail-open 的质量门。
  现在声明了任一界限且取值非有限即记一条违规（判据取「宁可多检不漏检」那一侧，与 SQLite 驱动
  拒绝 NaN/Inf 同一口径）；没设界限的键不受影响，本项只管「设了界限却什么都比不出来」这一种漏检。
  新增 4 条用例分别覆盖 NaN 带两侧界限、正无穷只给下限、负无穷只给上限、无界限的 NaN 照旧放行；
  把守卫停用后前三条转红而第四条仍绿。
- **响应提交失败的牵连面从整条连接收到那一条流**：`Http2Connection` 的响应入口把两类不同的失败混在
  同一个结论（`Rejected`）里——「对端还在等，本端给的状态码或响应头不合规」与「本端已经收尾后又多了
  几次调用」。会话据这个结论走「连接不可用」出口：`servePendingRequests` 直接返回 false，外层 `break`
  之后连待发字节都不再写出——本端一处写错的参数就此放大成全连接断连，同一条连接上别人在途的请求一起丢。
  现在前者由连接层按 INTERNAL_ERROR 中止那一条流（RST_STREAM 已排队，对端不必等到超时），会话给它的
  结论是 `StreamFailed` 并记一行错误日志；后者落回「该流不再需要响应」（`StreamNotWritable`）且一个字节
  都不写——已经交付完整的响应不该被事后补的 RST 拆掉（RFC 9113 §5.1）。顺带修掉上一轮留下的静默变形：
  `HeaderListTooLarge` 先把原因 `std::move` 进连接层、再拿它填调用方的 `errorText`，调用方读到的是空串
  （原因只剩 `lastStreamErrorMessage()` 那一份），现在先交给调用方再交出所有权；头文件里「返回非 Sent
  时不写入任何字节」这条承诺也已按实现改准（它上一轮就已与实现不符）。
  用例 `Http2Connection.RejectsIllegalResponseOnThatStreamOnly` 让六条独立流各撞一类非法响应
  （状态码两侧越界、头名大写、连接特定头、头值含控制字符、调用方自塞伪头），逐条断言恰好一帧
  RST_STREAM(INTERNAL_ERROR)、`errorText` 非空且含定位片段，最后一条新流照常收到 200、全程无 GOAWAY；
  另加 `ExtraCallsOnALocallyFinishedStreamChangeNothing` 钉住「多余调用不写字节」。把新增的两处
  `failStream` 摘掉（等于旧写法）时前一条转红。容器实测 `TestNet` 1241 例全绿、零告警、零 sanitizer，
  h2c 对手探针十项全过。
- **HTTP/3 也守对端通告的头段上限，且本端答不出时只作废那一条流**：`applyPeerSettings` 原先把对端的
  `SETTINGS_MAX_FIELD_SECTION_SIZE` 直接丢掉，注释写的理由是「响应头都由本类生成，远低于上限」——这个前提
  在 h2 那一轮已被证伪：响应头来自路由与业务，几条 `set-cookie` 就能超。不判就把处置权交给对端，而它常见的
  做法是收掉整条连接。现在按 §4.2.2 的算式（每个字段行名长 + 值长 + 32，用的就是解码侧那个函数，两侧同口径）
  在编码前判一次，越限即拒绝作答且一个字节都不上线（半个头段上线会让两端的 QPACK 状态错开）。
  另一半是收场：`handleResponseSubmissionFailure` 对一条还活着的流原先调 `markBroken`，承载层随之
  `closeNow` 整条 QUIC 连接——一处本端失误带走同连接上别人在途的请求。现在走本仓既有的本地中止口径
  （`RESET_STREAM` + `STOP_SENDING` 加丢掉本会话记账），只有连接层自己判了协议错才作废整条会话。
  对端没通告这项时不判定（§7.2.4.1 的默认值就是不限）。
  用例：`Http3Connection.RefusesResponseFieldSectionBeyondThePeerAdvertisedLimit`（越限被拒、连接未判死、
  同一条流随后交出合规响应）、`...StillSubmitsLargeFieldSectionsWhenThePeerAdvertisesNoLimit`，以及走真
  nghttp3 客户端的 `Http3Session.ResetsOnlyTheStreamWhoseResponseCannotBeSubmitted`（断言会话没被判死、
  那条流被交代了一次 `INTERNAL_ERROR` 复位；测试侧的对端夹具因此新增「通告一个小的头段上限」这一格）。
  分别摘掉这两处判据时，对应的两条用例各自转红。容器（GCC 13.3 + ASan/LSan/UBSan）实测 `TestNet`
  1244 例全绿、零告警、零 sanitizer。
- **流数上限补上 RFC 9000 §4.6 的 2^60 界，并把「两类流各记各的账」钉住**：这条界是 MUST——流号按
  「上限 × 4 + 该类首号」算，上限再大就会算出变长整数表达不了的流号。本端原先两处都不设界：传输参数表把
  `initial_max_streams_bidi` / `_uni` 的上限写成「变长整数最大值」，`onMaxStreamsFrame` 更是任何取值都收。
  后果不是「对端多给了额度」这么无害——本端照着它去开流，发出去的就是自己违反协议的流号。现在帧侧以
  `FRAME_ENCODING_ERROR`（0x07）收口连接、参数表侧按既有的取值越界判错，两处共用同一个新常量
  `kQuicMaximumStreamLimitValue`。顺带补上这条入口的直测：`onMaxStreamsFrame` 是三个 `MAX_*` 处理里唯一
  从未被用例直接碰过的，方向取反（单向的通告去抬双向额度）在旧覆盖下完全看不出来。新增
  `QuicStreamLayer.MaxStreamsFramesApplyToTheirOwnStreamClass`（单向↔双向互不影响，且不增大的通告按 §4.6
  忽略、不收回已有额度也不报错）与 `...RejectsAbsurdStreamLimitsAndKeepsTheAdvertisedCredit`（2^60+1 被拒、
  额度不变；恰好 2^60 合法），传输参数侧在 `RejectsOutOfRangeIntegerValues` 里两档各加一条边界用例。
  三处突变——方向三元符取反、去掉帧侧上界、参数表回到旧上界——各自让对应用例转红。容器实测 `TestNet`
  1246 例全绿、零告警、零 sanitizer。
- **滚动日志的文件名不再被本地代码页改写**：`LoggerConfigLoader` 早已把日志目录全程按 `std::filesystem::path`
  传递，唯独 `base_filename` 仍按窄串交给 `RollingFileSink`，而它在类内与目录拼接、拼备份名、比对目录项时
  要把这段窄串再解释一次——Windows 上这一步走本地代码页，落在代码页外的字符变成 `?`，滚动日志就此写到
  另一个名字上（同一份配置在 file sink 那条通道是对的）。更糟的是清理旧备份时把目录项名字也转回窄串比较：
  两处各自失真恰好互相掩护——备份认不出来、`max_backup` 形同虚设、日志目录无界增长，而日志里一切正常。
  现在基础文件名按路径的原生刻度持有，主名与扩展名的切分、活动名与备份名的拼装、清理时的识别统一走同一处
  切分，只含 ASCII 的序号与时间后缀直译进原生刻度（`std::format` 不吃宽格式串，故这些名字按刻度手工拼）。
  口径变化只有一处：只含前导点的名字（`.log`）其备份名由 `.1.log` 变成 `.log.1`，与标准库把前导点当隐藏名
  而非扩展名的解释一致，活动名与备份名同源切分因此仍认得全。另新增 `Platform::FileSystem::utf8FromPath`
  （`pathFromUtf8` 的反向出口），把留在诊断文本里的三处 `path::string()` 换掉：装配那一处在 Windows 上对
  代码页外的名字**直接抛出**，一句「日志写到哪了」的说明会毁掉整次配置装配，`FileSink` 的另两处则让一次
  报错变成二次抛出。新增 `FileSystem.Utf8FromPathRoundTripsPathFromUtf8`、`Utf8FromPathDescribesNameOutsideCodePage`
  与 `LoggerConfigLoader` 的两条端到端用例（file / rolling_file 各钉住「代码页外的名字落到真名」），其中
  rolling 那条在改前为红。容器（GCC 13.3 + ASan/LSan/UBSan）实测 `TestBase` 561 例通过 / 1 例按环境如实跳过、
  `TestPlatform` 184 例全绿、零告警、零 sanitizer；Windows 侧 `TestBase` 562 例全绿、`base_log` 示例 23 步全 PASS。
- **配置的加载失败不再因文件名过代码页而变成抛出**：`loadFiles()` 与 `loadFromDirectory()` 把路径写进
  `loadedFiles`、`failedFiles`、`errors` 时就地用 `path::string()`，Windows 上它按本地代码页转换，落在代码页外
  的字符**直接抛** `std::system_error`——而二十一处转换点里有四处正落在 `catch` 块内。后果是一份语法有错的
  配置只要文件名带表情符号或别种文字，调用方拿到的就不是「一条失败结果」而是一次逃逸的异常，
  「全部失败也返回 `ConfigLoadResult`」这条契约当场失效。现在本模块的文本通道统一转 UTF-8（新增的
  `pathText` 走 `FileSystem::utf8FromPath`），与 `errors` 里本就是 UTF-8 的中文文案同刻度；**口径**随之写明：
  `ConfigLoadResult` 的三个列表都是 UTF-8 文本，非 ASCII 路径的取值因此从 ANSI 变成 UTF-8（这种名字此前要么
  变形、要么抛出）。`enableHotReload()` 交给 `FileWatcher` 的那一处刻意保留窄串：那条接口整条都是原生窄串
  刻度，换成 UTF-8 会让今天能正常热重载的中文配置目录对不上号——要覆盖代码页外的目录名得连 `FileWatcher`
  的接口一起换成 path，另列一轮。新增 `ConfigManagerTest.FileNamesOutsideLocalCodePageAreReportedInsteadOfThrowing`
  （断言不抛，且文件名原样出现在 `errors` 与 `failedFiles` 里），实测改前红；Windows 侧 `TestBase` 563 例全绿。
- **热重载的接力不再被并发收尾吃掉**：一轮重载任务交还执行权之后，它的收尾代码其实还在跑，这期间
  另一轮完全可以起完又跑完。旧写法把「还要不要再补一轮」记在一面**共享**的脏标记上——停在中途的旧
  轮次会先把这个标记消耗掉，再因为抢不到执行权而放弃接力，于是那笔变更既没人接力、也没人重读，配置
  永久停在旧值直到用户下一次改动（现场只表现为「改了配置没反应」）。旧注释写的「抢不到 pending 说明
  别的触发者已经接手，它那轮同样会看到 dirty」恰好与实现相反：dirty 已在同一步被消耗。现在改用
  `Base/Config/Detail/ReloadRoundGate`——「单调递增的世代号 + 一把执行权」，每轮只与**自己开始时**那份
  快照比对，比对不消耗任何共享信息，因此并发收尾互不吃账；世代号改在抢权之前递增，正在跑的那轮不会
  刚好比对在这次记录之前。新增 `TestReloadRoundGate` 5 条：一条按七步确定性调用顺序复现该交错（不依赖
  线程调度），一条把旧的双旗写法原样搬来当对照、证明这条交错真的会丢，另有一条四线程用例压
  「同一时刻至多一轮占着执行权」的安全不变式。Windows 侧 `TestBase` 568 例全绿。
- **程序改配置不再绕开 schema 校验**：`setValue()` 与文件加载是两条快照提交路径，过去只有一条执行
  已注册 schema——同一个写错的值（例如往整数键里塞字符串）落在文件里会报一条 ERROR，落在程序里却
  什么都不报，而此后每次 `getInt()` 都静默回落到默认值，现场只剩「改了配置没反应」。现在两条提交路径
  共用同一段校验：`validateRegisteredSchema` 支持「只判某一个键」，单键提交因此既不会把别人欠的账
  （必需键缺失）重播一遍，也不必整表复制 schema。schema 仍是建议性约束——只报告、不阻断，违规值照常
  生效。新增两条用例（一条钉「该报的报了没有」，一条钉「不该报的别报」），实测摘掉那次调用后前一条
  变红；Windows 侧 `TestBase` 570 例全绿，容器 TSan 同数零告警、ASan/UBSan/LSan 对配置段 116 例零命中。
- **异步日志队列的容量补上界**：`queue_size` 过去只钳下界（0 会让三种溢出策略各自退化成错误语义），
  上界却没人管。一条事件在队列里占 `sizeof(LogEvent)`（实测 112 字节），容量乘过去就是下游卡住时最多
  占住的内存——`queue_size` 多打几个 0，本该「按策略丢弃或阻塞」的背压就变成了 OOM。现在上界由
  `AsyncSink::kMaximumQueueMemoryBudget`（64 MiB）除以单条事件的大小算出（本机读数 599186 条，
  `LogEvent` 以后加字段会自动收紧），配置侧与构造侧各钳一次、共用同一对常量，钳制照 `max_backup` 的
  口径报出「非法取值 + 要求区间 + 钳制结果」。
- **sink 条目不是对象时会报出来**：`sinks:` 下少写一个 `-`（整条被写成标量）过去一声不吭地跳过，
  运维看到的是一份「配置里有两条 sink、实际只挂上一条」的现场。本文件的口径是「容错必须可见」
  （`overflow_policy`、`max_size_mb`、缺字段、类型不符都报），这条拒绝路径漏了，补上报出实际形态。
  新增 `AsyncSinkClampsOversizedQueueSizeAndReportsIt`，并给既有的标量条目用例补上诊断断言——两条
  实测都在改前变红；Windows 侧 `TestBase` 571 例全绿、示例 `base_log`/`base_config` 45 步全过，
  容器 GCC 编同一套零告警、ASan/UBSan/LSan 对日志段 114 例零命中。
- **日志装配不再拼两份快照**：`LoggerConfigLoader::loadFromConfig()` 过去是「`keys()` 取一份快照挑出
  日志器名单，之后每个 `getOptional()` 再各取一份」——中途插进来的一次热重载能把「A 的等级来自上一版、
  A 的 sinks 来自这一版」拼成一份装配结果，而一次读取才是配置唯一的一致视图边界。现在整段 `logging`
  经 `getSection()` 一次取回（它只读一份快照），日志器名字也不再从扁平键反推，直接走树里的子对象。
  段落形状不可装配时（`loggers` 不是对象；或同一个名字既配成标量又是更长键的第一段——后者只有
  `setValue()` 造得出来，文件加载那侧自己就拒了）报诊断并**一个日志器都不动**：清空 sink 会让该日志器
  此后静默丢日志，而 `getSection` 抛的 `ConfigValidationException` 也不该逃出配置装配路径。新增
  `LoggersSectionThatIsNotAnObjectLeavesExistingSinksAlone` 与 `ContradictorySectionIsReportedInsteadOfThrowing`，
  两条换回旧实现都变红；Windows 侧 `TestBase` 576 例全绿，容器 GCC 同套零告警、ASan/UBSan/LSan 命中 0，
  示例 `base_log`/`base_config` 45 步全过。

### 性能

- **QUIC 一条连接上的出流开始轮着喂，短响应不再排在最后**：出包预算（受拥塞窗口与报文尺寸限制）原先
  每回都从流号最低那条开始分，而它往往正是最早那条大响应——一包只够喂一条时，后开的短流要等前者完全
  排空才见得到预算。现在记下「上一包喂到了谁」，下一包从它之后起手，走到表尾再回头。
  h2 侧刻意没做同一种游标：那条通道的公平本来就由分流的流级额度给（低号流额度用尽就换下一条），而它的
  出帧顺序是「谁先写完谁先进发送缓冲」的一根 FIFO，加游标只能重排被窗口卡住的那一小段，收益不足以让一个
  拿不出判据的状态位进库。判据：三条各排 400 字节的待发流 × 每包 120 字节预算，三包之内三条都要有字节上线；
  把游标冻回原地，这条用例立刻单独红，其余 61 条不受影响。

- **QUIC 监听器空闲时不再按固定节拍白轮**：定时器驱动原先每 10 毫秒醒一次，用来落实各连接的 PTO、
  握手与空闲超时，h3 请求的读时限，以及「业务在收报文之外写下的响应」那一档补刀——**连接表空着时
  也照醒**。现在改为按「下一次真正有东西可查的时刻」睡：早于节拍的截止按时到（原来最多晚一个节拍），
  晚于节拍的仍按节拍到（补刀与读时限没有可查的截止时刻，只能靠轮询兜），零连接时退到 1 秒一档。
  进程外读数（`benchmarks/h3_syscall_ab.sh`，容器、4 连接 × 25 请求、`--threads 1`，同一份负载两跑）：
  `epoll_wait` 41913 → 2126 次（−95%），而数据面的 `recvfrom` 445 → 417、`sendto` 402 → 379 基本不变
  ——省下来的全是与流量无关的空转唤醒。代价写在 `runExpiryTicker` 的说明里：刚建起来的第一拍最多延后
  1 秒（那一拍要办的事在收到报文时已经顺手办过了），此后自动回到配置的节拍。
- **同一条 HTTP/2 连接上的请求不再互相排队（队头阻塞消除）**：会话原先在一轮里「读一条流、把它的处理器跑完、
  写出响应」，处理器一挂起——流式正文、定时退避、等外部条件——整条连接就停在那儿，多路复用的第二条请求只能
  等第一条返回。现在处理器按流各起一条分离协程，循环只做收帧、派发与叫醒，在途请求数等于对端开的流数而不是
  恒为一。读数：`HttpsServer.PooledClientMultiplexesConcurrentRequestsOnOneConnection`（两条并发请求共用池里
  一条 h2 连接）从 10 秒等不到响应到 459 毫秒通过，最忙那条连接上两条在途流同时在飞。
  随这条改动落三处必要的收尾，三处都是「按流分协程之后新出现的等待窗口」，串行时代不存在：
  ①响应字节由每条流的协程自己写出——它跑完时主循环多半正停在「等对端再发点什么」上，而它等的就是这条响应；
  ②在飞处理器的收口改挂在计时器上拍轮，而不是 bare 地把协程排回本地队列——后者让 `Scheduler::runAll()`
  永远走不到定时器那一段，业务等的 400 毫秒定时器实测一万轮也等不来（表现为进程忙循环、连接停不下来）；
  ③忙计数由处理器自己结清——否则跑完最后一条响应的连接永久算「忙」，`TcpServer::drain()` 每轮都跳过它，
  收尾 GOAWAY 发不出去，等满期限被强关，对端只收到裸 TCP 关闭、拿不到 RFC 9113 §6.8 那条「哪些请求已生效」
  的通告。③由 `Http2CleartextSession.SendsGoAwayAfterHandlerThatResumedOutsideTheLoop` 钉住：按旧实现跑红
  1165 毫秒（正好是 drain 的整个宽限期），而既有的 `SendsGoAwayBeforeGracefulShutdown` 挡不住这种写法——
  它的处理器不挂起，会话在同一轮里就把记录摘了、忙标记也就复位了。
- **带默认值的取值通道不再为「判类型」先拷整棵子树**：`getInt/getBool/getDouble` 与 `get<T>(key, 默认值)`
  原先经 `getOptional` 把命中键的整个 `ConfigValue` 深拷出来再判类型——用一个 `getInt` 去读一张 200 成员的
  表，光这次「类型不符、返回默认值」就要先拷出 805 次分配、35 KB（MSVC 实测）然后整份丢掉。现在按引用查
  快照做判定，只有真正交出去的那个值才拷；`getString/getText` 早已是这条口径，本次把标量通道对齐。
  `getOptional` 自身的契约（交回一份深拷贝的子树）不变。
- **QUIC 出站方向的帧序列不再逐包碰堆**：每个出站包都要把这一包的帧编进一块缓冲，原先这块缓冲
  在出包循环里逐包新建（从空串长到一包大小要几回几何扩容），而 ACK 帧的区间换算又先把它算出的
  (gap, 区间长度) 对物化成一个临时 vector——每个带确认的包因此固定付 5 次分配（Windows Debug+ASan
  台账读数）。现在缓冲改成按线程复用（每包开始时清空），区间对改由相邻区间当场算出、校验整体先于
  写入：稳态一次都不碰堆。零分配判据打在 `QuicFrameAllocations.FrameAssemblyIntoReusedBufferDoesNotAllocate`
  的复用侧，对照侧只打印；线上字节由 `TestQuicFrame` 的 §19 逐字段比对与 ngtcp2 真机握手/发包共同看住。
- **QUIC 每条报文的固定开销降下来**：出站一侧，AEAD 与头部保护的 `EVP_CIPHER_CTX` 改为按线程复用
  （取用即重置），缓冲按精确长度一次预留，交给待发队列的是报文本体而不是逐字节副本——容器
  Debug+ASan 下组一条 1200 字节的 Initial，C++ 侧分配从每次 3 次到 2 次、申请字节 1255 到 1224，
  每包的两次 OpenSSL 对象创建与销毁归到每线程一次。入站一侧，进门先付的那两份整包缓冲（解头部保护
  用的可写副本与 AEAD 的明文缓冲）同样改为按线程复用：收一条 1132 字节的报文从每次 2 次分配 /
  2236 字节降到 0 次——这两份缓冲在判断「这条报文解不解得开」之前就得分配出来，与报文合不合法无关。
  时间读数落在同一台机器的负载带里（8.6~10.9 微秒），因此不主张百分比。读数分别打在
  `QuicPacketBuilderAllocations` 两条用例与 `QuicConnectionCore.ReceivePathScratchBuffersAreReusedAcrossDatagrams` 上。
- **可选 io_uring 后端的一次 `wait()` 也不再按在册描述符数取堆**。票据到注册记录的映射原先是节点式
  映射表，水平触发下每轮「完成时摘除 + 重投时登记」都要为每条在途轮询付一次 48 字节的分配（实测 32 条
  注册下 32 次/轮）。现在票据本身编码「第几个槽位、第几代」，完成通知据此直接定位，槽位只在一支空闲栈
  里来回取还：容量长到位之后稳态零分配。三条后端的这一条判据由同一条用例钉住（`wait()` 稳态零分配），
  另加两条按规模冲刷的用例（200 条注册反复改关注位、8 条长期挂着的轮询被成千张新票据挤过位置）。
- **Windows 完成端口后端的一次 `wait()` 稳态不再碰堆**。水平触发是靠「本轮把探针重投一次」模拟的，
  因此只要有人注册着，待重投表与待合成表每轮都非空；这两张表原先整张换到局部变量里遍历、函数返回时
  连缓冲一起销毁，下一批入表又要从零长起。实测 32 条常驻注册下每轮多付 12 次分配，改成按下标消费
  本轮份额后归零，并新增一条分配台账用例把「稳态每轮零分配」钉住（同一用例在 epoll 后端读数也是 0）。
  收益随注册数与轮询频率放大：空转的循环不再逐轮向堆申请指针表。
- **可选 io_uring 后端的一次 `wait()` 不再随已注册描述符数线性变贵**。此前每轮等待都要重扫一遍全部注册
  来决定该补投哪些一次性轮询，C10K 形状的监听器（大量空闲连接 + 稀疏事件）因此把 CPU 花在扫描上；改为
  完成处理时把需要补投的描述符记进一张工作清单，等待路径只排空清单。实测 4096 个已注册描述符相对 128 个
  的每轮开销从约 2900 倍回到同档，并新增一条按规模比对的用例把上界钉在 8 倍以内（超过即报红）。
- **被等级挡下的那条日志不再为正文取堆**：`Logger::log()` 以 `string_view` 收调用方的文本，却先把正文拷成
  `std::string` 再去问等级，于是一条注定丢弃的 TRACE/DEBUG 记录也要为消息体取一块堆——按 INFO 跑的生产进程里，
  满代码库的 TRACE/DEBUG 调用走的正是这条被丢弃的路径，而它们一分钱都不该付（走宏的调用方在宏里就滤了，
  直接调公开入口的没有那层保护）。过滤因此排到拷贝之前，`logWithStackTrace` 那条携带外部栈的重载同改。
  实测（40 字节消息、Release、同一台机）：一千次调用的分配从 1000 次 / 48000 字节降到 0 次 / 0 字节，
  而改前这条与真写出去那条的读数一模一样；微基准 `log-filtered-out` 28.6 → 1.4 ns（三跑同为 1.4，登记为吞吐
  下限门禁，拿改前的结果去跑门禁是 0.05×、当场报红）。用例 `FilteredOutLineIsAllocationFree` 把放行形状当对照
  一起钉住——只留「零分配」那一半，读数为 0 也可能只是因为消息短进了小串内联。Windows 侧 `TestBase` 577 例全绿、
  示例 `base_log`/`base_config` 全过，容器 GCC 同套源码零告警、ASan/UBSan/LSan 下日志与配置段 154 例零命中。

- **JSON 版式一条日志的开销降到原来的四成时间、一成分配**：`JsonFormatter` 原先只实现 `format()`，
  Sink 走的是基类那条「先 `format()` 出一份临时串、再整行 append」的通道，那条临时串按字符几何增长
  要重分配七八次；而每条日志又要从零建一份字段对象（五到八对键值）。两处都改了：`formatInto()` 被
  覆写，用同一份序列化器把对象直接追加进调用方的缓冲（Sink 的行缓冲跨行留着容量）；字段对象改成
  **线程局域复用**，槽位已是字符串时原地赋值，不出现的可选键（logger、stackTrace）显式摘除。
  转义表、键序与非法 UTF-8 的抛点都由库自己那一份序列化代码负责，产物与 `dump()` 逐字节相同——
  用例把这条钉成「自身解析后重新序列化是恒等」的不动点判据，另两条钉住「缓冲里已有的内容保留」与
  「抛出时一字未增」，第四条按四种形状交替断言键集合不串味。
  实测（Release / MSVC）：微基准 `log-format-json` **1529.4 → 644.3 ns/行（−58%）**；分配读数
  （`AllocationProbe`、一千行同一条事件）Sink 通道 **约 24000 → 2000 次（每次 560 字节）**，
  独立 `format()` 出口 **23000 → 3000 次（每次 736 字节）**；文本版式的同一条事件仍是 1 次 / 112 字节。
  实现上取的是 nlohmann 的 `detail::` 序列化器（`dump()` 没有「追加到既有缓冲」的公开出口），版本由
  Conan 锁住；接口真漂移时是编译不过，不会静默改变日志内容。复用的代价是每线程留着一份小对象与
  「历史上最长那条消息」的缓冲容量，不随日志量增长。
- **`getString()` 取一份字符串只碰一次堆**：取用通道原先是 `get<std::string>(key, std::string(默认值))`——
  默认值先进来一份，`getOptional()` 再把整份 `ConfigValue` 深拷进 optional，最后从那份副本里拷一次字符串
  交回调用方。实测（Release、`AllocationProbe`、一千次取一个 40 字节的键）：**4000 次分配 / 每次 176 字节
  → 1000 次 / 每次 48 字节**，与「只把返回串本身拷一份」这条参照读数持平。改为与 `getText()` 同口径按
  视图直查、命中时直接从快照交出一份拷贝；严格类型口径不变（只有字符串值算取到，缺键与类型不符都回落
  默认值）。新增 `tests/Base/Config/TestConfigHotPathAllocations.cpp`，判据只在 Release 下钉。
- **QUIC 流层的两条待取队列不再空载预订堆块**：交付队列与被取消流号队列都是「每条连接一份」的
  `std::deque`，光默认构造就要一块索引表加一个 512 字节的节点，而这两条多数时刻是空的或只有一两项。
  它们只有「尾加、从头取」两种用法，改由流层自持一个读位置队列：取走只推进读位置，读到过半才把
  前面那段一次擦掉。需要 `push_front` 的那条（出站待发队列）仍留 `std::deque`，不勉强。实测流层的
  构造成本 **4 次分配 / 1120 字节 → 0 次 / 0 字节**，「建起来再收一条 16 字节正文的请求」
  **8 次 / 1408 字节 → 5 次 / 328 字节**（分配 −37%、字节 −77%）。新增
  `tests/Net/Quic/Streams/TestQuicStreamLayerHotPathAllocations.cpp` 两条形状，另加两条「取一个再塞
  一个新的」交错用例钉住顺序；把回收游标故意挪错一格，既有的 `DeliversInOrderBytesAndMarksFinal`
  与新增的交错用例同时转红。
- **日志版式直接渲进 Sink 自己的行缓冲，落盘一条少取两次堆**：`LogFormatter` 此前只有
  `format()` 一条出口，交回的是 `std::format` 造出的新串——而 `std::format` 交出结果本身就要
  取两次堆，Sink 想连换行整行写一次还得再把它搬进自己的缓冲。控制台与文件这两条路径每行都付
  这笔钱，且全程握在本 Sink 的互斥锁里。接口新增按追加语义写进调用方缓冲的 `formatInto`，
  `LogSink` 配一条同样选择格式化器的 `formatEventInto`；自带版式的两个格式化器覆写了它
  （`format()` 反过来变成「本函数 + 一个空串」，两条路径共用同一份版式），默认实现转调
  `format()` 再追加，因此自定义格式化器不改也能走通。
  实测（Release、`AllocationProbe`、一千行、走控制台的一条）：**2010 次 / 454122 字节 →
  1010 次 / 310122 字节**，即每条日志少一次分配、字节 −31.7%；剩下的 1000 次正是「一条日志的
  事件构造」那一段的参照读数，说明格式化 + 写出这一段现在完全不取堆。新增的读数把判据写成
  「不超过同步参照 + 1/64 的增长余量」而不是钉死实测值，避免绑住 STL 的扩容因子。
  输出逐字未变：44 条格式化器用例与文件类 Sink 的行数守恒用例一条没改，Debug 与 Release
  两侧全绿。
- **文本版式一行不再经 `std::format` 组整串**：`DefaultFormatter` 与 `ColorFormatter` 原先用
  `std::format_to` 配 `back_insert_iterator` 拼一行——它事先不知道要写多少，缓冲按几何级数边写边搬，
  而这些字段的形状与顺序本就是固定的。改成入口一次 `reserve`（框架开销加各变长字段的实际长度）、
  再逐字段追加。实测（Release）：`log-format-default-into` **301.2 → 33.3 ns/行（−89%）**、
  `log-format-default-copy` **333.0 → 61.9 ns（−81%）**，分配数不变（一行仍只付消息体那一块）。
  定宽字段的上收到 `Formatters/PaddedFieldText.h` 一处，两条版式共用。对拍用例
  `LineAssemblyMatchesFormatSpecByteForByte` 的期望值仍按原格式串现算，它当场抓出一个会写错日志的口径：
  `std::format` 的 `{:<N}` 量的是**显示宽度**（中日韩字符算两格）而不是码元数——`源.cpp:4271` 是
  12 字节 / 11 格，按字节数补齐会少一个空格。因此整段 ASCII 才自己补，掺了非 ASCII 就退回
  `std::format` 那一份宽度表。
- **`FileSink` 在 Windows 上按二进制写、自己补行尾**：日志文件原先以文本模式打开，行尾交给 C 运行库
  翻译，而那条翻译路径逐字符走。同一段 172 字节的正文实测 **306.7 → 192.8 ns/行（−37%）**
  （探针跑两百万行，两种写法的产物 SHA-256 相同）。改成二进制打开后由本类把每个 `'\n'` 补成 `"\r\n"`：
  绝大多数行只有行尾那一个换行，改末位字符即可，带调用栈的行（每帧一个换行）才整段展开。顺带把
  「报回的字节数」从「缓冲长度 + 换行个数」的补偿式改回缓冲长度本身，按大小滚动的阈值少一层换算。
  新增 `FileSink.WritesPlatformNativeLineEndingsToDisk` 逐字钉住落盘字节（注掉补齐调用即在 Windows
  转红），既有的「报数等于磁盘增长」那条继续守着字节账。
- **HTTP/2 连接的固定开销去掉一块空转的终止流水账**：`Http2Connection` 记「哪些流已终止」用的是
  `std::deque`，而 libstdc++ 的 deque 光默认构造就要一块索引表加一个 512 字节的节点——一条连接一个
  字节还没读写就先占 576 字节。这张表只有「尾部追加」与「从头摘旧」两种动作用法，换成 `std::vector`
  即可；摘旧记录攒成整段一次擦除，而不是逐条 `erase(begin())`（稳态下每条流终止都要挤掉一条，逐条擦
  等于每回把整张表往前搬一遍）。实测每条 h2 连接的构造成本 **6 次分配 / 1728 字节 → 4 次 / 1152 字节**，
  握手到收口 **11 次 / 2034 字节 → 9 次 / 1458 字节（−28%）**。新增两条形状把「构造」与「握手」分开量，
  读数按打印留在用例里（这两条不设硬阈值，它们的存在理由就是把改动前后的对照数字钉在那里）；
  挤出记录之后的宽容判据由既有的两条用例覆盖，改动前后同样全绿。
- **Windows 的定时等待不再被系统时钟整拍抬高一档**：定时器描述符原先走 TimerQueue 定时器
  （`CreateTimerQueueTimer`），到期按 15.6 ms 的系统节度取整——实测等 20 ms 要 29–31 ms 才醒、等 50 ms
  要 62 ms，退避与限速这类短定时的尾延迟成倍。现在换成高精度可等待定时器
  （`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`）加线程池等待，同一条等待实测 21 ms 醒来。顺带把一次「武装」
  的内核开销压掉：旧走法每次都要先删掉上一个待决定时器再建一个新的，那一步实测 0.7–21 µs 的大幅抖动
  （删除落在进程共享的默认定时器队列上，等的可能不是自己的回调），新走法只需 `SetWaitableTimer` 改期，
  实测 261 ns。高精度档要 Windows 10 1803 以上，拿不到时退回普通档——精度回到老行为，而不是让定时器
  不可用。`cancel()` 因此不再阻塞等回调，契约相应改成「不再有新到期；取消瞬间在途的回调仍可能落一个
  字节」，读端多醒一次时并无到期项，按新堆顶重新武装即可。析构反过来必须严格：先阻塞注销等待、再关
  描述符，否则描述符号被别的套接字复用之后，那次残留的写就成了往陌生连接里塞字节。
- **HTTP/2 会话的接收缓冲同样按实际读量翻倍**：与上面那条 HTTP/1.1 的改法同一把尺子——h2 会话一建立就
  `resize` 出全额 8 KiB 的接收缓冲，而浏览器类连接绝大多数时间在空闲，这 8 KiB 是白占的。现在从 2 KiB 起步，
  只有「上一趟把整个窗口读满」才翻倍，封顶仍是既有的 8 KiB 档：持续送正文的连接最多两趟就回到老尺寸，
  整条连接上摊不出几次多余的读，而空闲连接每条少占 6 KiB。跨读的半帧本就由帧解码器自己缓冲，
  因此这条路径不是新开的——大正文上传与「对端只读不授窗口」两条既有用例就是它的回归判据。
- **空闲连接的接收窗口按实际读量翻倍**：每条连接一份 8 KiB 接收窗口，是每连接内存里最大的一块——
  直方图实测 1000 条空闲连接的分配总量 14.75 MB 里 8.24 MB 就是它，而这些连接一次也读不满 2 KiB。
  现在窗口从 2 KiB 起步，只有「上一趟把整个窗口读满」（内核里还有字节没取走）才翻倍，翻倍到既有的
  8 KiB 档为止：忙连接第一两拍就回到老尺寸，稳态的系统调用次数一条不多——同一套保活压测改前 203
  次 recvfrom + 201 次 sendmsg，改后一字不差。实测 1000 条空闲连接的分配总量降到 8.65 MB（−41%），
  10 万条连接省下约 600 MB。跨多趟读取的大头部块另加一条端到端用例守着（改动前只有超过 8 KiB 的
  头部才走得到这条路，现在 2 KiB 就走到）。
- **Base：日志事件改带时刻，时间戳文本推迟到格式化器**（配套的契约变更见「变更」那条）。产生日志的
  线程常常就是事件循环线程，而它原先每写一行都要先造一份 23 字符的时间戳文本——长过小串内联，
  因此是一整块堆，外加一次 `localtime` 折算与一轮格式化处理。现在事件只带 8 字节的时刻，文本由三条
  版式（Default/Color/Json）在 Sink 写入线程上渲染进调用方的栈缓冲，历法换算按整秒缓存在本线程，
  一秒内的连续日志只折一次日历——与调用栈「带原始帧、解析推迟到 Sink」是同一条路子。
  Release 实测（`LogHotPathAllocations`，共用新上收的 `AllocationProbe`）：一行日志在 Logger 侧的分配
  **2 → 1 次**，剩下的那一次就是消息体本身（48 B）；渲染本身 **一千次共 0 次**；消融对照「把时刻落成
  owning 文本」一千次共 1000 次，即被去掉的正是这一次。顺带修掉同一处算法在预 1970 时刻上的缺陷
  （朝零截断让 1969-12-31 23:59:58.5 报成 23:59:59 且毫秒残差为负、逐位写出 `+0` 这类垃圾），
  证伪方式是把实现改回截断算法，那条用例立刻以 `+0` 对 `500` 落红。
- **Base：日志事件按右值交给 Sink，异步队列不再自己复制一份**：`LogSink` 多一个 `write(LogEvent &&)`
  出口（默认原样转调左值版本，只实现左值版本的 Sink 行为不变），`Logger` 在「只有一个 Sink 肯收」时
  交出事件本体，异步那条就只剩移动；多于一个 Sink 时各给只读引用，否则后面的 Sink 会记空行。
  这条改动落地时异步那条仍是 2007 次/千行（同步 1000 次），当时据此判断「还有一次复制没定位到」，
  并把用例降级为只报读数——**那个判断是错的**：多出来的既不是复制，也不是当时猜的「`std::deque`
  每 16 个事件一块」，真正的来源见下一条。
- **Base：异步队列改由 AsyncSink 自管槽位，投递一条日志不再取堆**：给分配探针
  （`tests/TestSupport/AllocationProbe`）加了「按申请大小分桶」的直方图，同一枚二进制里同步与异步两条
  逐桶相减，定位到多出来的是一千块 **136 字节** 的分配，而 `sizeof(LogEvent)` 正好 136——出自
  `std::queue<LogEvent>` 底层的分块规则：**MSVC 的 `std::deque` 对超过 16 字节的元素按「一块只装一个
  元素」分配**，于是每投递一条日志就是一趟 malloc（同一支探针在容器 GCC 13 / libstdc++ 上是 512 字节
  一块装 3 个元素，一千次 push 仍要 341 次分配，所以这不是只为 Windows 做的优化）。
  槽位现在收进一个带队首下标的 `std::vector<LogEvent>`：入队只在倍增那一次取堆（倍增到配置容量为止，
  峰值内存因此不超过调用方要的队列规模），取出只推进下标，已消费的前缀攒够才整体回收。
  Release 实测：**2007 → 1007 次/千行**（多出的七次是 8→1024 那一串倍增），摊回每次投递只剩消息体那一次
  分配，与同步同价。累计申请字节反而从 200334 涨到 324586：倍增的每一代都算一次申请（旧块当场释放但计入
  申请总量），常驻峰值两边相当（都要装下在队的那些 136 字节事件），换来的是少一半的分配器往返与连续布局。
  用例改钉阈值「一千行 ≤ 1000 + 1000/32」，并留一条同二进制消融例（往
  `std::queue<LogEvent>` 里连推一千次，读数 2007 次/200334 字节）作为依据；两条 `static_assert` 钉住
  前提——`LogEvent` 的移动构造与移动赋值必须 nothrow，否则倍增会退成逐个深拷贝。
  验收：Debug（含 ASan）TestBase 514 例 + 异步/落盘/配置相关 220 例连跑 15 轮全绿零报告；
  容器 GCC 的 ASan+LSan 下同一批 237 例连跑 6 轮零报告；跨线程证据仍只能由 CI 的 TSan 作业提供
  （本机 WSL2 内核上 TSan 起不来，已实测确认）。
- **HTTP/1.1 的头部名值收进一条字节缓冲**：`HttpHeaderFieldStore` 的权威记录原先是「每条头部两个
  `std::string`」，解析器还要先把它们逐条攒成一份 owning 暂存、收齐时再逐条移动进请求对象——一条
  十个头部的请求要解两次大小写、构造二十个串、走两趟容器增长。现在名与值一次性写进同一条
  `std::vector<char>`，记录本身只是「偏移 + 长度」，小写折叠在写入那一趟顺带做完；解析器的暂存也
  换成同一个存储，报文收齐时两条缓冲在解析器与请求对象之间整块交换（`adoptFrom`，只换容器不拷字节），
  两边的容量都留给下一条报文复用。交出 owning 容器的 `fields()` 口子换成按序回调的 `forEachField`
  （`HttpResponse` 的序列化跟着走它，`HttpRequest::addHeader` 的参数改成视图）。
  实测（改动前后两枚二进制同批交替三跑，新二进制另跑九次）：解析一条 h1 请求 **727.5 → 510.6 ns（−29.7%）**，
  装 10 条头部 280.1 → 208.9 ns，四条头部读路径快 8.6%~24.4%；每请求分配 **6 → 2 次**（剩下的两次是
  URI 与正文的缓冲），装 10 条头部 **4 → 0 次**。响应头序列化不受影响（+0.4%）。
- **解析器交给请求的 URI 与正文也改成整块交换**：上条剩下的两次每请求分配出在 `setUri(std::move(...))`
  与 `setBody(std::move(...))`——移动把解析器暂存缓冲连着容量一起交出去，下一条报文只能重新取一块。
  新增 `adoptStagedUri()` 与 `adoptStagedBody()`，与头部的 `adoptStagedHeaders()` 同形：两条缓冲交换，
  覆盖语义不变（被换下的旧正文照旧作废，作废内容由解析器清零后接着用），只是容量留在两侧。
  实测（改动前后两枚二进制同批交替三跑，新二进制另跑六次）：解析一条 h1 请求 **510.6 → 424.3 ns（−16.9%）**，
  每请求分配 **2 → 0 次**、113 → 0 字节——h1 解析到这一步的稳态成本不再碰堆。
  顺带把「稳态零分配」那几条判据从摊平读数改成一千次的原值：摊平是整除，「每次 0 次」允许一千次里
  藏住 999 次分配。换原值判据后立刻量到首轮一千次里有 20 次暖期扩容（四份容器按倍长到位的成本），
  用例因此显式暖身 64 轮再测，稳态读数 0 才是可信的。
- **路由派发不再为每个候选现造一张参数表**：`Router::route` 的模式层原先每扫一条候选就现造一张
  `unordered_map` 收参数，另外不管命中哪一层都还要第二张「选中的参数」表。MSVC 上构造一张空哈希表
  就要两次堆分配（单文件探针实测：1000 张空表 2000 次分配、每次 104 字节），于是这笔固定成本随
  模式路由条数线性放大，命中精确路由也白付一次。现在收集表进到模式层才建、逐候选 `clear` 复用，
  命中那条当场把参数提交给请求（第二张表消失）；流式探测只要「命中与否 + 是不是流式」，落点改成
  空指针，不收集就不建表。
  实测（改动前后两枚二进制同批交替三跑）：命中精确路由的派发 **203.6 → 146.5 ns（−28.0%）**、
  扫过模式候选并收下 `:id` 那条 **448.3 → 345.0 ns（−23.0%）**、流式探测命中 `:id` 的
  **71.7 → 23.5 ns（−67.2%）**；每请求分配 **2 → 0 次**（精确）与 **8 → 4 次**（模式层命中）。
  派发两例已入微基准基线，分配数由 `HotPathAllocations` 按形状钉住。
- **每请求最后一块堆分配也没了（request-id 就地落定）**：会话在派发前要给每条请求落定
  `<前缀>-<16 位序号>` 标识（21 字节，长过小串内联）。旧写法每条现造一个串再交给请求，而请求把
  手上那块还掉、把新那块连着容量一起接过来——实测每请求 1 次分配 / 32 字节。现在生成走「写进调用
  线程自己的复用缓冲」，`HttpRequest::setRequestId` 改为按视图接收并原地写入（源码级兼容，传
  `std::string` 照旧可编），两侧容量都留着；「客户端自带值可采信就沿用、否则生成」这条判定由两条
  入口共用同一份实现。
  消融实测（同一枚二进制两例对照）：落定一条请求的 id **39.7 → 14.4 ns（−63.7%）**，每请求分配
  **1 → 0 次**。配上前面两条，h1 一条 GET 在框架侧（解析 + 落定 id + 派发到精确路由）的稳态分配
  已经归零。
  三条协议的会话（h1/h2/h3）都改走这一条入口，但**只有 h1 省下那一次取还**：h1 的请求对象按连接
  复用，而 h2/h3 的请求对象随流新建，那块 21 字节的缓冲仍要现取一次（要省下它得把整条请求对象
  池化，是另一件事）。改齐的实际理由是「客户端自带值能不能采信」这条判定只剩一份实现——三处各写
  一遍的话，其中一处漏掉长度或字符集校验就是响应拆分与日志错位的入口。
- **IOCP 后端每轮等待不再按连接数付费**：`Iocp::wait()` 每轮都要调
  `harvestSyntheticErrorEvents()` 取「探针投递当场撞上硬错误」的那些方向，而它原先遍历整张注册表
  逐个判断 `readyDirections` 是否非零——绝大多数轮次一个都没有，于是常态轮次也在按连接数付费。
  改法与同文件 `noteArmFailure` + `m_pendingArmRetry` 是同一套路：加一张待合成脏表与入表标记位，
  取时先整表换出再遍历，表空直接返回（稳态一次堆块也不碰）。注销路径必须把这张表和另外两张
  待处理表一起摘干净，否则状态在 `delete` 之后仍被引用。
  一次性探针实测（三遍交替取中位数，每轮 `wait(0)` 的耗时）：注册 1 个描述符 444.4 → 433.5 ns
  （噪声内），512 个 **1705.3 → 377.7 ns（−77.9%）**，2048 个 **6678.1 → 365.0 ns（−94.5%）**。
  改前每多注册一个描述符约多付 **3.0 ns/轮**，改后与连接数无关——按 1 万连接外推，
  单这一处从每轮约 30 µs 降到 0.37 µs。
- **连接收口不再先清零那块入站清理的落点缓冲**：`AsyncSocket::close()` 丢给
  `discardUnreadInboundData()` 的 `std::array<char, 8192>` 带 `{}` 值初始化，于是每次收口先
  memset 掉 8 KiB——而这块缓冲只作为 `recv` 的落点、内容从不被读，且绝大多数连接收口时接收队列
  本来就是空的（第一轮 `recv` 直接以 WOULD_BLOCK 收场）。隔离计时（只把 `close()` 放进计时区，
  三遍交替取中位数）：**8398.7 → 8243.0 ns，约 1.9%**，三遍方向一致。
  量法值得记一笔：把「造套接字 + 注册」混进计时区时，同一改动的读数会掉进噪声里
  （24585 → 23823，看着像没变）——省一小段纯 CPU 工作的改动必须把计时区收到被改那句所在的那次调用上。
- **SQLite 驱动把写语句的编译挪出了每次调用**：`execute()` 过去每来一条语句就
  `sqlite3_prepare_v2` 一次、跑完再 `sqlite3_finalize` 一次，而「编译」在一条参数化语句的成本里
  占大头——单文件探针实测：只编译再释放 2613 ns、编译+绑定+推进+取列 3219 ns、复用已编译游标只要
  605 ns。现在跑完的**写**语句按 SQL 文本留在 `m_statementCache` 里复用（游标收尾统一改成
  `sqlite3_reset`：既把它放回可重跑状态，也仍然报得出 DEFERRABLE 外键这类推迟到收尾才浮现的错误，
  原来只在 `sqlite3_finalize` 上露面的那格返回码没有丢）。查询语句不进这张表——它的游标所有权要
  移交 `SqliteResult`，与「调用结束即回表」不是同一套生命周期，这一半留给下一轮。
  表有容量上限 64，到顶整表清空而不做 LRU：会涨到上限的负载说明「同一句 SQL 被反复执行」这个前提
  已经不成立，缓存对它本来就没收益，换来的是游标数有常数上界。
  **关键约束**：缓存的键只有 SQL 文本，而游标属于具体的那个 `sqlite3` 句柄，因此 `disconnect()`
  必须在换句柄之前先清表。撤掉这一步的实测表现不是崩溃也不是 ASan 报告（`sqlite3_close_v2` 会把
  还有未 finalize 游标的连接标成 zombie，内存仍在），而是重连后 step 以
  「not an error（错误码 0）」这种毫无意义的原因失败——正是只能靠用例拦住的那类静默错误，
  `ReconnectDoesNotReuseStatementsFromTheClosedHandle` 钉住它。
  `ReusedWriteStatementAppliesEveryRoundOfParameters` 钉住复用的另一半：游标确实被 reset 回可重跑
  状态、上一次的绑定不会残留成这一次的取值（按五轮之后每行的最终落库值核对，不按返回码）。
  消融实测（改动前后两枚二进制同批交替各三跑，对照组 `pool-acquire-return` −0.1%、
  `request-id-land-into` +0.0% 都在噪声内）：按主键改一行 **1284.5 → 684.6 ns（−46.7%）**；
  按主键查一行 1616.7 → 1664.4 ns（+3.0%，查询这条**没有**变化，符合预期）。
  容器 GCC + ASan/UBSan 下 Database 467 例零失败零报告。
- **Redis 驱动组装 argv 的堆块按批归零**：`executeCommand()` 过去先把调用方给的 `string_view` 参数逐条
  落成 `vector<std::string>` 再交给 `redisCommandArgv`，而 argv 接口要的就是「指针 + 长度」——按长度取值时
  内嵌 `'\0'` 与零终止符无关，那次复制只是在 hiredis 自己那一次之外**又搬了一遍**。现在参数按
  `span<const string_view>` 直接送到底（`buildArgumentViews` 改为按区间取模板，切词路径、视图路径与
  管道三条来源共用同一份实现）；管道发送阶段那对「指针 + 长度」暂存表也从循环体里提到循环外跨条复用，
  `buildArgumentViews` 进门先 `clear()`，容量因此逐条继承。
  读数只量被删掉的那一步（MSVC `/O2` 单文件探针，三遍方向一致；端到端一次真机往返是数十微秒量级，
  单条命令的百分比淹没在往返里，因此这里不给百分比）：三个短参数 **31.6 → 1.2 ns**，其中一个取值是
  4 KiB 正文时 **69.7 → 1.2 ns**（多出来的那部分是那次堆分配加整块复制）；管道里按条复用形态
  **52.7 → 6.0 ns**，一段 1000 条的管道即少约 47 µs 纯 CPU 与 2000 次堆操作。
  复用暂存表唯一的新增风险是「上一条的参数尾巴接到下一条」，而且它**未必报错**：实测把 `clear()` 撤掉，
  接在 5 参数 `MSET` 后面的 `GET k` 仍拼得出一条合法的 `MSET kA 1 kB 2 GET kA`——服务端回 OK，并真的写进
  一个名叫 `GET` 的键。`PipelineAcrossRisingAndFallingArgumentCountsKeepsCommandsIntact` 因此按
  5/2/2/7/5 的参数条数逐条核对**取值**而不是只核对「没报错」；撤掉 `clear()` 后该用例连同既有两条管道
  用例一起转红。容器 GCC + ASan/UBSan 下真机 Redis 15 例全绿，Database 整套 468 例 446 通过、22 例门控
  跳过、零失败零报告。
- **SQLite 的只读查询不再被执行两遍**：`SqliteResult` 构造时为了给 `rowCount()` 一个精确值，会整趟
  `sqlite3_step` 走完再 `sqlite3_reset`；调用方随后 `next()` 又把同一条查询从头跑一遍，第二遍还额外
  做全部列的类型转换。现在预扫描顺手把行值收进快照，遍历只读快照、不再回到游标。快照有行数上限
  （`SqliteResult::kMaximumMaterializedRowCount`，256）：超过就整份作废、退回游标遍历，因此
  「读一行就丢弃结果集」的用法不会为整表正文付内存。`rowCount()` / `isEmpty()` 依旧精确——
  计数与存值分开，超限的行只数不存。带返回列但非只读的语句（`INSERT ... RETURNING`）本来就不
  预扫描，这条路完全不变，重跑会改数据。
  拆成三段计的成本（容器 GCC `-O2` 单文件探针，同一张 4096 行表，三遍方向一致）：单行主键查询
  「只推进不取值」412 ns、「推进加取值」586 ns、「读快照」1.9 ns，调用方读满时 **998 → 588 ns
  （−41%）**；256 行列表分别是 23.4 µs / 59.5 µs / 0.79 µs，读满时 **82.9 → 60.3 µs（−27%）**。
  代价如实写明：只看 `rowCount()` / `isEmpty()` 而不读列的调用方，现在要为最多 256 行付转换成本
  （上界是单行 +174 ns、256 行 +36 µs），旧路径的预扫描只数不转。
  回归用例在快照上限的两侧各钉一条（256 行与 257 行逐行比对全部行名）：把「超限即作废快照」那条
  判据撤掉后，257 行那条静默少读 1 行而完全不报错；另有一条钉住快照结果集 `reset()` 之后第二遍
  与第一遍逐行一致。容器 GCC + ASan/UBSan 下 Database 471 例全绿（Redis 与 MySQL 真机用例同时
  开启、零跳过），LSan 报告与改动前逐字一致。
- **SQLite 的查询游标也进语句缓存**：上一轮之后，只读查询的行在构造期就整份物化，游标此后不再被
  任何人引用——正是回收的时机。`execute()` 如今在这条路上把游标交回 `m_statementCache`，下一次同
  文本查询整趟跳过 `sqlite3_prepare_v2` 与「还有没有第二条语句」的探测。所有权按结果集形态分三路：
  行已物化 → 交还缓存（命中来的那条本就在表里，只摘掉结果集的释放责任）；行没跑完（超过快照上限、
  或 `INSERT ... RETURNING` 这种带写副作用的） → 先把键从表里摘走再随结果集走，避免结果集 finalize
  之后表里剩一个已释放地址；构造期预扫描报错 → 同样先摘键再交出 nullptr。
  **一处可见的语义收窄**：行已整份物化的查询结果 `nativeHandle()` 现在给出 `nullptr`（原先给出那条
  游标）。基类文档本来就写明「写回执为 nullptr」，即返回值允许为空，解引用前必须判空的调用方不受影响；
  原先默认「查询结果一定有游标」的调用方需要改判空。列名与列序不受影响——快照模式下列名表与行值
  一起存下了，`columnName/columnNames/columnIndex` 走的是缓存。
  消融实测（容器 GCC Release，改动前后两枚二进制同批交替各 7 跑，取安静窗口的中位数；对照组
  `pool-acquire-return` 106.1 → 107.4 ns 在噪声内）：按主键查一行 **1439.7 → 422.7 ns（−70.6%）**，
  吞吐 69.5 万 → 236.6 万 op/s；与上一轮的「少跑一遍」合计，这一条查询从本轮开始前的 1366~1427 ns
  降到 378~524 ns 区间。`benchmarks/microbench-baseline.json` 里记的是 MSVC 读数，待本机构建恢复后重录。
  新增两条用例：重复执行同一条查询时第二遍结果逐行不变（并能在第一份结果仍打开时重遍历它），
  以及超限的两位结果集各握一条游标互不借用。容器 GCC + ASan/UBSan 下 Database 473 例全绿、
  真机 Redis 与 MySQL 同时开启零跳过，LSan 回到 6328 B / 7 块的上游基线。
- **MySQL 驱动的预处理语句按文本复用**：带参数的执行路径过去每次都是 `mysql_stmt_init` +
  `mysql_stmt_prepare`、返回前再 `mysql_stmt_close`——编译与关闭各是一趟网络往返
  （COM_STMT_PREPARE / COM_STMT_CLOSE）。现在连接用 `m_statementCache` 按语句文本持有已预编译的语句
  （上限 64 条，到顶整表清空，与 SQLite 那条同一取舍），命中后只跑 execute + fetch。
  真机实测（容器 → MySQL 26.7.0 走 docker 桥，同一条 `SELECT ... WHERE id = ?` 重复 300 次取最快一轮）：
  **每次重编 414 µs → 复用 197 µs（−52.4%）**，五遍比值一致。
  所有权规则与 SQLite 的写语句缓存同一套：预处理成功即由表接管，此后的失败路径一律「弃掉这条」而不是
  本地 close；`disconnect()` 里也必须先 `clearStatementCache()` 再 `mysql_close`——mysql_close 不替我们
  关语句，留着就是服务端语句与客户端内存双双泄漏。**刻意不做失败自动重试**：服务端拒绝的一次若重跑
  一遍可能就是二次写入，那代价远大于多一趟往返，所以失败即弃、下一次自然重新预处理。
  没收益的那一半也写明：一条语句文本只用一次的负载拿不到任何好处（反而多一次哈希与一个键字符串），
  稳态下反复执行同本文本的 ORM 流量才是目标场景——同一套 22 个真机用例的整套用时改前 3.2~4.2 s、
  改后 3.2~3.5 s，正因为那些用例每条文本只跑一两次，且建连与建表删表都走文本协议。
  `ReusedStatementServesFreshRowsAndRecoversFromFailure` 钉住复用的两条静默风险：必须读到改动后的
  最新数据，以及一次失败（本地拒绝与服务端主键冲突各一次）之后同一条文本仍可执行。容器 GCC +
  ASan/UBSan 下 Database 474 例全绿、真机零跳过，LSan 与改动前逐字一致；本机 MSVC `/W4 /WX` 单 TU
  编译通过（C++20 异质查表的 `unordered_map` 在两个标准库上都成立）。
- **方言层的标识符渲染改成直写缓冲，一条查询少十次堆分配**：`StandardSqlDialect` 过去每渲染一个字段引用就
  `splitQualifiedName()` 切出一个 `std::vector<std::string_view>`（**每个标识符一次堆分配**），再把
  `quoteIdentifier()` 返回的临时串 `+=` 进 SQL 文本；而 `quoteIdentifier()` 里那句 `reserve(size*2+2)` 让
  `balance` 这种 7 字节的名字也越过短字符串缓冲、多建一份堆缓冲；SQL 文本本身从空串按 15→31→63→127 逐次翻倍。
  三者叠起来，一条「5 列 + 1 个等值条件 + LIMIT 1」的 SELECT 光是翻译就要 **12 次分配、236 ns**。
  现在切段与判定都在串上就地扫（`isQuotableQualifiedName()` 先判、`appendFieldReference()` 再逐段直写目标缓冲），
  `quoteIdentifier()` 退成 `appendQuotedIdentifier()` 的一层薄壳（转义规则只有一份），文本按内容估上界一次性
  `reserve()`，参数个数由条件树精确算出后定容（`countConditionParameters()` 与 `appendCondition()` 的分支一一对应）。
  对照阶梯（容器 GCC 13 `-O3`，内存 SQLite，2 万次取五遍最快，全局 `operator new` 计数）：
  **`translate()` 239 → 113 ns（−52.7%）、12 → 2 次分配**；ORM `first()` 1260 → 1135 ns（−9.9%）、24 → 14；
  `insert()` 1513 → 1329 ns（−12.2%）、29 → 17；`count()` 17 → 10；`toList()` 20 行 7122 → 6975 ns（−2.1%）、52 → 39。
  归因按成对 A/B 做：改前的头 + 改前的库、改后的头 + 改后的库各自成套编出两个二进制，交替跑三轮，
  控制例（池租约 92 ns / 驱动 floor 648 ns / 只建查询树 23 ns）三轮都不动，差值才算落到翻译层。
  顺带删掉一处白做：`count()` / `countAsync()` 原先走 `resolvedQueryNode()` 把全部列名展开一遍，
  紧接着又整段被 `{"COUNT(*)"}` 顶掉——现在直接拷 `m_queryNode` 再覆盖，产出文本逐字不变。
  **产出文本必须逐字不变**是这一轮的硬约束：方言用例全部断言整段 SQL 文本，`users.*`、`COUNT(*)`、
  含空格列名、内部引号翻倍转义都在其中；点号切空段（`users.`、`.id`、`a..b`）仍判「不是限定名」走表达式分支。
  两条新用例各自可证伪：把「空段可引用」改判为真，`QualifiedNameWithEmptySegmentIsPassedThrough` 报出
  `SELECT "users"."", ""."id", "a"."."."b"`；撤掉引号翻倍，`LongIdentifierWithQuoteIsQuotedVerbatim` 转红。
  容器 GCC + ASan/UBSan 下 Database **476 例全绿**（原 474 + 本轮 2 条）、真机 MySQL/Redis 零跳过，
  LSan 仍是 6328 B / 7 块的上游基线；本机 MSVC `/W4 /WX` 下单 TU（方言实现与一条 ORM 用例）编译无告警。
  剩下的 ORM 开销在查询树副本与结果映射那一半，不在翻译层；MySQL 侧读的是网络往返主导的路径，这一轮的收益被往返盖住。
- **SQLite 结果集快照摊成行优先的一整块缓冲**：`m_materializedRows` 原本是
  `vector<vector<DatabaseValue>>`——**每行一次堆分配**，256 行的快照就是 257 次，外层还要再吃倍增扩容。
  现在按「行优先、行内按列序」摊平（第 r 行第 c 列落在 `r * 列数 + c`），行首下标由 `next()` 推进的游标算出。
  行数单独记一个 `m_materializedRowCount`，不用 `cells.size() / 列数` 反推：写回执的列数是 0，
  那个除法会炸，而快照与写回执共用这一段代码。
  对照阶梯（与上一条同一套 A/B 接法，头与库各自成套——类布局变了，只换库会让探针里的成员偏移对不上）：
  驱动层单行取数 6→**5** 次分配、647→624 ns；ORM `first()` 14→13；`toList()` 20 行 39→**19**、
  50 行 69→**19**（**分配数从与行数成正比变成常数**）。**耗时两档都测不出差异**（14.1~14.5 µs 与
  14.2~15.8 µs 互相盖住），每格的文本拷贝与 RowMapper 才是这一档的主导——所以这条按「分配次数与驻留内存」
  记账，不当成墙钟收益卖。
  本轮不加新用例：把行距改错（`(游标-1)*列数+下标` 改成 `(游标-1)+下标*0`）会被多条既有用例抓住
  （ORM 端到端读写、事务可见性、批量插入分块、快照上限两侧整表比对），实测确认后原样还原。
  容器 GCC + ASan/UBSan 下 Database 476 例全绿、真机零跳过，LSan 仍是 6328 B / 7 块的上游基线；
  本机 MSVC `/W4 /WX` 下驱动 TU 与其用例 TU 均无告警。
- **连接池停摆不再等满后台线程的 1 秒睡眠分片**：健康检查线程按 1 秒为一片 `sleep_for`，靠「醒来
  时发现停止标志」退出，于是 `~ConnectionPool()` 的 join 平均要等半个分片、最坏等满一整秒。按数据源
  或租户各建一个池的服务里，这笔延迟在重启与扩缩容时按池数成倍放大；测试侧同样是每条用例都白付一次
  停摆等待。改成用条件变量睡、并由 `stop_callback` 在停止请求那一刻叫醒（分片粒度与每秒一次的异步
  超时推进都保持不变）。实测 Database 整套用时 **54.4 s → 9.2 s**（本机 MSVC 465 例）；容器 GCC +
  ASan/UBSan 下同一套 **54.4 s → 1.4 s**。`ConnectionPool` 单套件从 7083 ms 降到 869 ms。
  回归用例 `ShutdownDoesNotWaitForASleepChunk` 按「连续建拆六次、每轮先停 100 ms 再拆」量总时长，
  阈值 1500 ms：退化实现每轮还要再等约 900 ms 的睡眠剩余（六轮 5.4 s 起），新实现只花在停留上
  （约 600 ms），两个数量级分得开。那 100 ms 的停留是判据成立的前提——不等的话析构可能抢在健康线程
  真的进睡之前跑完，连退化实现也会立刻返回，用例就量不到任何东西。
- **连接池那张「按裸指针记创建时刻」的表整个删掉**：存活期过期要判「这条连接建立了多久」，而
  `DatabaseConnection` 是驱动提供的类型、池无法给它加字段，于是池另开了一张 `unordered_map` 按连接
  指针存创建时刻，并由**第三把锁**守着——归还时查一次（一次加锁 + 一次哈希），建连时插一次（再一次
  加锁 + 一个节点分配），丢弃时删一次。现在这份时刻改由连接自己携带
  （`establishedAt()` / `markEstablishedAt()`，池在 `connect()` 成功后盖一次戳），整张表与那把锁一并
  删除，`IdleEntry` 也只剩归还时刻一个字段。存活期判定原本在取出与归还两处各写了一遍，现收成一个
  `isPastMaximumLifetime()`。
  消融实测（改动前后两枚二进制同批交替各三跑）：保活复用的「取出 + 归还」一整趟 **108.3 → 97.6 ns
  （−9.9%）**，`maximumLifetimeSeconds` 为 0 那条「每次归还都丢弃、每次取出都新建」的抖动形态
  **222.5 → 154.3 ns（−30.7%）**。同批对照里 `request-id-land-into` 完全持平，另两例 −3.7% / −6.9%
  属跨二进制的代码布局漂移，因此复用形态那 −9.9% 里只有一半可归给本次改动。
  顺带消掉一类隐患：那张表以裸指针为键，任何一条不经 `closeTrackedConnection()` 的连接销毁路径都会
  留下指向已释放对象的键，而同一大小类里刚释放的地址极易被下一条连接复用，于是新连接会继承旧连接的
  创建时刻、被立刻判成过期。当前所有销毁路径都还规规矩矩地先删键，所以这条**今天并不触发**——它属于
  「以后加一条丢弃出口就会踩」的那类结构，删掉表就没有这个坑了。
- **HTTP/2 一条响应的 HEADERS 帧不再把头块拷三遍**：`emitHeaderBlock` 原先把已编好的头块 `substr` 成
  owning 片段、再攒进一个 `body` 串、再拼进「帧头 + 负载」临时帧串、最后搬进待发缓冲——四次搬运里只有
  最后一次是必须的。改法与上面 DATA 帧那条出口同形：新增 `appendHttp2HeadersFrame(bytes, 片段视图, ...)`
  让连接把头块直接拼进待发缓冲，标志位的拼法收进文件内一处 `headersFrameFlags`，`encodeHttp2HeadersFrame()`
  的无优先级分支改为委托它（带 5 字节优先级字段的那条形态保留原样）。消融实测（144 字节的真实响应头块，
  即连接首帧上按字面量编码的 7 个字段）：**147.8 → 19.8 ns/条响应（−86.6%）**，每条响应少两次堆块取还。
  线上字节由新用例按 RFC §6.2 的原始位值逐字钉住（END_STREAM/END_HEADERS 四种组合、目标缓冲非空只能追加、
  流号为 0 时抛错且不留半帧），且与结构体那条入口的产出互比相等。
- **HTTP/2 出站 DATA 帧不再为算长度先拷一遍载荷**：`pumpSendQueue` 原先把待发缓冲里的一段正文交给
  `encodeHttp2DataFrame()`，后者先拼出一份「帧头 + 载荷」的临时串，再由连接整段搬进待发字节——中间那份
  临时串对一条帧只起「先攒起来好算长度域」的作用，而长度在动手前就知道。新增 `appendHttp2Frame(bytes, ...)`
  作为同一套布局的「目标缓冲已在手」出口（`encodeHttp2Frame()` 改为委托它，帧的写法与校验仍只有一份），
  连接侧 `appendOutgoingFrame()` 直接把帧拼进待发缓冲。消融实测（16 KiB 载荷 = 对端默认通告的
  `SETTINGS_MAX_FRAME_SIZE`）：**270.4 → 92.1 ns/帧（−66%）**，随载荷线性；每条数据帧还少一次堆块取还。
  线上字节由新用例逐字钉住（与临时串出口、与 `encodeHttp2DataFrame()` 三条互比，含二进制含 NUL 的载荷、
  目标缓冲非空只能追加、校验不过时一字节不加）。
- **流式响应的分块帧改用响应自带的缓冲**：`HttpResponse::writeChunk()` 原先每段都新建一个串装
  `<十六进制长度>\r\n<数据>\r\n`，每个正文段付一次堆块加一次整段拷贝——SSE 这类「小段、高频」的
  出口上，这笔分配会盖过组帧本身的工作量。组帧挪到 `HttpChunkFrame.h` 的 `appendChunkFrame(frame, data)`，
  与解析侧的 `chunkFramePayload()` 同处一文件、严格互逆（一帧的布局只有一处定义），帧缓冲挂在响应
  对象上跨段复用。Release 实测每次操作从 1 次分配 / 272 字节降到 **0 次 / 0 字节**，读数由
  `HotPathAllocations.ChunkFrameAppendAllocations` 钉住。端到端吞吐不变（`router-streaming-get`
  0.96x、`router-streaming-post-param` 0.99x，都在跨运行噪声内）：省下的是每段一个堆块。
- **QUIC 的定时节拍不再按在线连接数复制标识**：`runExpiryTicker` 原先每拍先把所有连接的标识
  （20 字节连接标识的十六进制文本，超出 SSO 即落堆）拷成一份快照，再逐条按串查表——防的是
  「挂起期间连接被摘走」，而摘除已收口连接的唯一一处见到 `ActivityGuard` 就跳过，节拍循环在每次
  挂起前都已挂上守卫，这份防不住真实风险的拷贝因此是纯开销：它只随在线数走、与流量无关。
  实测同样键形与容器下两份工作量（Release /O2）：快照 + 逐条查表 111 纳秒每连接每拍，直接遍历
  4 纳秒——1000 条连接、10 毫秒一拍，省下约 107 微秒与每秒十万次字符串分配与比较。
- **压缩挪出事件循环线程后，同循环的小请求不再等大正文**：`--threads 1` 起服务、一路客户端持续要
  gzip 版 `/big`（256 KiB）、另一路量 `/bench` 的往返延迟，Release 实测：压缩留在循环里做时小请求
  p50 5,930us、p95 6,300us、max 12,637us；交给工作线程后 p50 21us、p95 55us、max 284us，
  而那条重活本身 10 秒 1,629 → 1,596 条（-2%）。代价是一份正文副本（按值交给工作线程）与一次
  跨线程恢复，量级在几十微秒内。`echo_server --compress` 的三条通道（明文、TLS 与 h3）默认都走这条路，
  `--compress-sync` 保留就地压作对照。
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
- **协程帧池一档的扩容步长不再被另一档的高水位带大**：`CoroutinePool::refillLocalCache()` 的翻倍基数
  原先取「两档合计的已切分块数」，而大档块规格是小档的 8 倍（256 B 与 2048 B）。被小帧跑热过的池第一次
  碰到大帧，就要一次切出与小帧同数的**大块**：实测「先要 4000 块小帧、再要一块大帧」这一场景里那一笔是
  8192 块 × 2048 B = 16 MiB 的连续申请，既是一记缺页停顿，也一口吃满两档共享的 16384 块预算，此后另一档
  只能落到全局堆。改成各档自记块数、按本档历史翻倍，同一场景的第一步降到 256 块。
  回归用例 `TierExpansionStepFollowsItsOwnHistoryNotTheOtherTiers` 按「本档相邻两次扩容的比值」判定
  （实测比值 2，退化实现 4~32），因此共享单例先前喂过多少块都不影响结论；换回改动前，该用例在独立进程与
  整文件两种跑法下都转红。写用例时想给池开一个独立实例才查出：每线程缓存 `threadCache()` 只按线程分、
  不按池分，第二个实例会把别家的块发给调用方、归还时又被当成外来指针交给 `::operator delete`——「进程内
  唯一实例」因此写成构造函数的 `@warning`，而不是放开可见性。
- **取消一条定时等待不再按在途定时器数量付费**：`TimerQueue::remove()` 原先线性扫描定位被取消的等待器，
  再 `make_heap` 整堆重建，单条 O(N)。超时看门狗是「每请求一条等待器」的形状，批量断连（客户端掉线、连接
  被强关）因此退化成 O(N²)：Debug/ASan 下逐条取消 1000 个定时器要 **299.5 ms**、4000 个要 **6554 ms**
  （单条 300 µs → 1.64 ms，条数翻 4 倍而单条贵 5.5 倍）。改成让等待器自带堆下标、上下浮自己实现
  （`std::push_heap` 那一系不给「指针一动就回写下标」的钩子，而没有下标就只能靠扫描定位），单条降到
  O(log N)：同一量法 **3.6 ms / 16.2 ms**（单条 3.6 µs → 4.0 µs，基本与 N 无关），4000 条这一档快 **405 倍**。
  堆顶那一步也改走同一条摘除路径，登记顺序与截止顺序错开时不再需要整堆重建。
  回归用例 `InterleavedCancellationsKeepDeadlineOrder` 把 24 条等待的登记顺序与截止顺序彻底错开、每隔两条
  取消一次，断言剩余者按截止先后醒来、被取消者一个都不醒；抽掉下标回写后该用例转红（报「取消后堆里剩的项数
  不对」），整套定时器用例在本机连跑 40 遍稳定。
- **一趟 `runAll()` 不再把跨线程投递吃到见底**：调度器第二阶段原先整表换出全局队列与回调表、
  循环吃到空为止。投递方可以长期不断流（压缩外派的执行器完成回调、别的循环移交过来的连接），
  于是这一趟**永不返回**，事件循环再也回不到 `epoll_wait`——同一条循环上的套接字一个事件都收不到。
  实测：持续投递下旧实现单次 `runAll()` 在 25 秒内没有返回（探针进程实际挂了 4.5 分钟以上才被中止），
  改后同一场景 **1.29 ms 返回、本趟正好做满上限 256 件**。上界写在
  `Scheduler::kMaximumRemoteItemsPerPass`，超出的投递分趟取用：计数按本批实际取走数递减而不是清零，
  因此 `hasWork()` 仍报待办，循环下一趟带着 0 超时立刻接着取，不会丢也不会误判空闲去睡。
  回归用例 `SingleRunAllPassIsBoundedOnRemoteQueue` 一次攒三倍上限的投递，断言单趟不超过上限、
  剩余仍算待办、分趟最终一条不丢；换回改动前该用例转红（报「768 vs 256」）。
- **线程池自动档按「本进程实际可用的核数」定容**：`IoContext()`/`ThreadPool()` 的线程数缺省取
  `std::thread::hardware_concurrency()`，它只看机器——容器里 `--cpus` 走 CFS 配额、`--cpuset-cpus`
  走许可核集合，两边它都不看。2 核配额的 Pod 上因此会起满宿主核数条事件循环，每条自带一份 epoll
  与定时器描述符，多出来的那十几条既跑不到、又白占内存与文件描述符，还把上下文切换拉满。
  新增 `Platform::CpuAffinity::recommendedWorkerCount()`：机器核数、许可核集合、cgroup 配额
  （v2 的 `cpu.max` 与 v1 的 `cfs_quota_us`/`cfs_period_us`）三者取最小，下限 1；配额除不尽时
  向上取整（1.5 核给 2 条、0.5 核给 1 条，绝不折成 0）。配额按**本进程所在的分组**读
  （`/proc/self/cgroup` 取相对路径），只读根分组会把 systemd `CPUQuota=` 这类子分组的限额错当成不设限。
  绑核路径本来就在读许可集合，两处口径一致了。
  `AsyncExecutor`（压缩外派用的阻塞任务执行器）的自动档一起改：它的工作线程干的是纯 CPU 活，
  按宿主核数起会在配额内把 CPU 从事件循环手里抢走，正好伤到外派压缩要保护的那一方。
  实测（宿主 20 核，`ubuntu:24.04` 新容器）：不限时 `hardware=20 / 许可=20 / 推荐=20`
  （与旧行为一致，裸机与不受限容器没有任何变化）；`--cpus=4` 时 `推荐=4`——把配额读取摘掉的突变版
  在同一容器里给 20，用例即报「限到 4 核，推荐值却更高」（20 vs 4），换回本版 `CpuAffinity` 8 例全绿，
  `IoContext`/`ThreadPool`/`AsyncExecutor`/`Timer` 共 33 例也在 `--cpus=4` 下全绿。
  Windows 没有 cgroup，只按前两者收敛。
  用例 `CoresFromCgroupQuotaRoundsUpAndTreatsUnlimitedAsNoConstraint` 钉折算表（含「不设限不得变成 1 核」
  这一类把常态当故障的方向性错误），`CgroupPathFromProcRecordHandlesBothVersionsAndMalformedLines`
  钉分组路径的 v1/v2、缺前导斜杠、行尾 `\r`、残缺行与空文本，
  `RecommendedWorkerCountRespectsThisProcessQuotaAndAllowedSet` 由用例自己重读 cgroup 再算一遍当独立判据；
  `ThreadPool` 与 `IoContext` 两条自动档用例的断言从 `== hardware_concurrency()` 改成
  「≥1 且 ≤许可集合」与「等于 `recommendedWorkerCount()`」，依据就是受限环境下两者必须不等。

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

[Unreleased]: https://github.com/Gyanis9/AsynGyanis/compare/v2.5.0...HEAD
[2.5.0]: https://github.com/Gyanis9/AsynGyanis/compare/v2.4.0...v2.5.0
[2.4.0]: https://github.com/Gyanis9/AsynGyanis/compare/v2.3.0...v2.4.0
[2.3.0]: https://github.com/Gyanis9/AsynGyanis/compare/v2.2.0...v2.3.0
[2.2.0]: https://github.com/Gyanis9/AsynGyanis/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/Gyanis9/AsynGyanis/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/Gyanis9/AsynGyanis/compare/v1.1.0...v2.0.0
[1.1.0]: https://github.com/Gyanis9/AsynGyanis/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Gyanis9/AsynGyanis/releases/tag/v1.0.0
