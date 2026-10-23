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
  另记一条给下一次发布：远端已无标签，但这台机器上仍留着那 13 个旧附注标签作谱系，`check-release-version.py`
  读的是本地 `git tag`——新线第一个标签打上之前，标签那一项会拿旧线的最高号来比，那条红是残留而不是漂移。

## [1.1.0] - 2026-10-23

自 1.0.0 起的累计变化（新增 4、修复 4，两个数按本段 `### 新增/修复` 两段的顶层条目现数得出）：一次以「超时与取消可制造、失败路径可信」为主的版本——WebSocket 客户端接收有了时限，且本端关闭能唤醒挂起的读；DNS 解析器开放可替换的查找实现，解析卡死终于能在进程内复现；入站请求把处理截止时刻交给业务，网关可以把剩余预算传给下游；HTTP/1.1 出站请求可以用停止令牌取消；QUIC 流控在额度用尽时的重传停摆、Redis 词法错误回显凭据、请求复位残留 TLS 身份与协程池顶档装不下路由帧四处缺陷一并修掉。**没有破坏性变更，源码级公开签名保持兼容。**

### 新增

- **WebSocket 客户端接收超时与本地关闭唤醒**（`78b67529`）：新增 `WebSocketClient::receiveFor(timeout)`——到点返回 `ReceiveTimedOut`，只停止等待，不消费已到的帧、不关闭连接，下一次 `receive()`/`receiveFor()` 仍能继续收；本端 `close()` 通过停止源唤醒挂起读并返回 `ReceiveInterruptedByLocalClose`，连接保留给下一次 `receive()` 读对端那条 Close 回帧。超时后的挂起读保留在客户端上，析构时随客户端一起释放。
- **可替换的名字查找**（`905fdca8`）：`AsyncResolver::setLookupFunction()`/`clearLookupFunction()` 与 `activeLookupCount()`；默认仍走 `getaddrinfo`，调用方可在启动阶段注入自己的查找实现。新增两条判据：阻塞 lookup 下 50 ms 解析返回空且名额归还；`HttpClient` 在 DNS 卡住时按请求时限失败，恢复后仍能发下一条请求。
- **入站请求处理截止时刻**（`e073642e`）：`HttpRequest::setDeadline()`/`deadline()` 由 `timeoutMiddleware` 在进入业务链时落定，`reset()` 清空；网关处理器可把 `request.deadline()` 直接填进 `HttpClientRequest::deadline`，下游慢于入站预算时上游先失败，不再让超时沿调用链放大。
- **HTTP/1.1 出站请求取消**（`5a6d2f98`）：`HttpClientRequest::cancelToken` 接受 `std::stop_token`；慢响应等待由看门狗轮询，停止信号到达后关闭当前连接并交出「请求已取消」，并发闸排队同样可取消。取消的连接不还空闲池，后续请求仍可新建；HTTP/2/3 的流级取消尚未接入，仍按各自时限收场。

### 修复

- **QUIC 流控重传在额度用尽时停摆**（`9d3e28ca`）：高水位以内的旧字节重传不再被「新额度」闸门挡住；流额度恰好用尽时判丢的段仍能补发。1.2 MiB 的 h3 响应跨过 1 MiB 单流待发队列边界后不再截断或停住。
- **Redis 命令词法错误回显凭据**（`5db5f7e0`）：`lastError()` 对切词失败的原文只保留命令词，AUTH 口令、SET 值与 URL 令牌不再进日志与告警；命令名仍保留，现场可定位。
- **请求复位残留 TLS 对端身份**（`bcdc9b9e`）：`HttpRequest::reset()` 清空 `m_tlsPeerIdentity`，复用请求对象时不会把上一条连接的证书身份带到下一条请求。
- **协程池顶档装不下路由派发帧**（`0ebfa1c3`）：`Router::route` 的帧在 MSVC 14.51 Release 下已到 3248 字节，顶档从 3072 提到 4096，路由派发不再每请求回退全局堆；池上限按顶档算约 64 MiB。
