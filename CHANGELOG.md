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
  三者，不一致即退出码非 0；发布门禁（`release-gate.yml`）已接入这一步（它不被路径过滤跳过），避免出现「打了标签但版本号没改」这类漂移。
- **本文件从 2026-10-19 起重记**：此前的 1.0.0～4.0.0 各段连同远端标签与 Release 一并作废并从本文件删净，
  那一段演进留在 `git log` 的发布提交里。所以下面第一个 `## [x.y.z] - 日期` 就是新线的头一笔；在它写出来之前，
  本文件没有任何条目可读，`check-release-version.py` 会按「读不到发布段」退 2——那是它在说实话，不是脚本坏了。
  另记一条给下一次发布：远端已无标签；旧线的 13 个附注标签已在本机改名为 `old-line-v*` 保留谱系，
  一致性脚本只认 `vX.Y.Z`，不会拿它们比较。新线从 v1.0.0 起，三处版本号按 CMake 与本节比较。

## [1.0.0] - 2026-10-23

新引擎线的首个发布（新增 9、修复 7，两个数按本段 `### 新增/修复` 两段的顶层条目现数得出）：一次以「超时与取消可制造、失败路径可信」为主的版本——WebSocket 客户端接收有了时限，且本端关闭能唤醒挂起的读；DNS 解析器开放可替换的查找实现，解析卡死终于能在进程内复现；入站请求把处理截止时刻交给业务，网关可以把剩余预算传给下游；h1/h2/h3 出站请求都可以用停止令牌取消；QUIC 流控在额度用尽时的重传停摆、Redis 词法错误回显凭据、请求复位残留 TLS 身份与协程池顶档装不下路由帧四处缺陷一并修掉。这是新引擎线的第一个版本，没有需要兼容的旧发布；后续版本按语义化版本推进，版本号仍以 CMake 与本节为同源真值。

### 新增

- **WebSocket 客户端接收超时与本地关闭唤醒**（`78b67529`）：新增 `WebSocketClient::receiveFor(timeout)`——到点返回 `ReceiveTimedOut`，只停止等待，不消费已到的帧、不关闭连接，下一次 `receive()`/`receiveFor()` 仍能继续收；本端 `close()` 通过停止源唤醒挂起读并返回 `ReceiveInterruptedByLocalClose`，连接保留给下一次 `receive()` 读对端那条 Close 回帧。超时后的挂起读保留在客户端上，析构时随客户端一起释放。
- **可替换的名字查找**（`905fdca8`）：`AsyncResolver::setLookupFunction()`/`clearLookupFunction()` 与 `activeLookupCount()`；默认仍走 `getaddrinfo`，调用方可在启动阶段注入自己的查找实现。新增两条判据：阻塞 lookup 下 50 ms 解析返回空且名额归还；`HttpClient` 在 DNS 卡住时按请求时限失败，恢复后仍能发下一条请求。
- **入站请求处理截止时刻**（`e073642e`）：`HttpRequest::setDeadline()`/`deadline()` 由 `timeoutMiddleware` 在进入业务链时落定，`reset()` 清空；网关处理器可把 `request.deadline()` 直接填进 `HttpClientRequest::deadline`，下游慢于入站预算时上游先失败，不再让超时沿调用链放大。
- **名字缓存有效期可配置**（`afbfaf25`）：`AsyncResolver::setCacheTimeToLive()`/`cacheTimeToLive()`，默认 60 秒；对之后写入的缓存条目生效，非正拒绝。过期路径用 200 ms 有效期 + 注入 lookup 直测，撤掉 `readCache` 的过期检查时用例红。
- **WebSocket 客户端发送时限**（`788f8773`）：`Configuration.sendTimeout`（默认 30 秒）给单帧写出设上限；`AsyncSocket::asyncSend`、`TcpStream::write`、`TlsSocket::asyncSend` 与 `HttpOutboundConnection::send` 新增带 `std::stop_token` 的路径，令牌停止时从等可写返回，且「调用方取消」与「套接字被关闭」分开上报；代理泵通路的 drainer 也把令牌带到外层隧道写出。对端不读时不再永久挂住：1 MiB × 256 帧的背压用例在 100 ms 收口，摘掉令牌路径时用例红。
- **出站请求取消**（`5a6d2f98`、`2c305a0c`）：`HttpClientRequest::cancelToken` 接受 `std::stop_token`；H1 关闭当前连接，H2 用 RST_STREAM(CANCEL)、H3 用 RESET_STREAM 与 STOP_SENDING 只结一条流，兄弟请求与连接保留。socket、stream、TLS、代理与池化连接新增 `waitReadable(stop_token)`，阻塞泵直接等可读或取消唤醒，不再每个节拍新建定时器与协程；域名解析、TCP 建连、TLS 握手（直连、代理外层与隧道内层）与代理 CONNECT 应答也接受同一令牌；流式正文在段落边界观察取消，下一段不再向来源索取；TLS 读取把令牌带到「凑齐一条记录」的内层等待上，卡在半条记录也能被叫醒，且取消不把别的流还要用的连接判死。可读等待在已关闭通路上交回不可读，不再把异常穿透到调用方。未带令牌的请求仍走原来的阻塞泵。取消的连接不还空闲池，并发闸排队同样可取消。

- **数据库池的取消与借出原因**（`227cdd43`）：`ConnectionPool::acquireAsync(loop, stop_token)` 与带绝对截止时刻的重载；`PoolAcquireFailure` 区分超时、停摆、等待名额满、分配失败与取消；`DatabaseConnection::requestQueryCancel()` 在 SQLite 上以 `sqlite3_interrupt` 兑现；`PooledConnection::execute*` 接收停止令牌，令牌在结果集（含流式 `next()`）存活期间一直有效，取消过的连接按不可信丢弃。入站 `HttpRequest::cancelToken()` 可直接传入，200 ms 入站时限打断数据库长查询的网关用例已在 TestNet 落地。
- **WebSocket 业务可读 TLS 对端身份**（`227cdd43`）：`WebSocketPeer::peerIdentity()`/`WebSocketClient::peerIdentity()`；h1/h2/h3 服务端把连接身份带进升级后的 peer，客户端在 TLS 握手后抓取服务端证书身份，指纹可用测试侧 OpenSSL 独立复核。
- **日志 Sink 的可注入入口**（`227cdd43`）：`FileSink(std::ostream&)`（不接管所有权）与 `RollingFileSink(..., Clock)` 让写失败与跨午夜滚动在任意平台确定性复现；旧构造保持原行为。

### 修复

- **QUIC 流控重传在额度用尽时停摆**（`9d3e28ca`）：高水位以内的旧字节重传不再被「新额度」闸门挡住；流额度恰好用尽时判丢的段仍能补发。1.2 MiB 的 h3 响应跨过 1 MiB 单流待发队列边界后不再截断或停住。
- **Redis 命令词法错误回显凭据**（`5db5f7e0`）：`lastError()` 对切词失败的原文只保留命令词，AUTH 口令、SET 值与 URL 令牌不再进日志与告警；命令名仍保留，现场可定位。
- **请求复位残留 TLS 对端身份**（`bcdc9b9e`）：`HttpRequest::reset()` 清空 `m_tlsPeerIdentity`，复用请求对象时不会把上一条连接的证书身份带到下一条请求。
- **协程池顶档装不下路由派发帧**（`0ebfa1c3`）：`Router::route` 的帧在 MSVC 14.51 Release 下已到 3248 字节，顶档从 3072 提到 4096，路由派发不再每请求回退全局堆；池上限按顶档算约 64 MiB。

- **诊断文本回显凭据**（`227cdd43`）：访问日志与 `HttpClient` 的 URL、失败原因、兜底 `what()` 统一脱敏 userinfo 与敏感查询参数（token/access_token/api_key/password/secret/sig/signature/auth 等），查询串里的令牌不再进日志、投递记录与错误面板。
- **h2 客户端惰性协程的悬垂入参**（`9a8f3085`）：`Http2ClientConnection::request/requestStreamed` 按 `const&` 收 `extraHeaders`/`responseReceiver`/`bodySource`，传 `{}` 临时量时首次 resume 读到已销毁对象；改为按值收，视图参数在声明处写明存活期。TSan 下同一用例由 20/20 崩转为 0/20。
- **Windows 句柄失败的错误分类**（`227cdd43`）：`SOCKET` 无符号使 `listener < 0` 永不成立，socket 失败会漏检并被下一步 `bind` 报成 `WSAENOTSOCK`；改用 `INVALID_SOCKET` 比较，并在关闭已建句柄前保住失败码；测试注入点覆盖监听/客户端/accept 三条失败出口。
