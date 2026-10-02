# 安全策略

本项目是一个网络服务器引擎：HTTP/1.1、HTTP/2、HTTP/3（含自研 QUIC 传输层与 QPACK 编解码）、
WebSocket、TLS，以及 MySQL / SQLite / Redis 驱动。**其中 QUIC、QPACK、HPACK、各类协议解码器
都是本仓库自己实现的**，它们的输入直接来自公网，因此我们把远程可达的解析缺陷当作最高优先级。

## 支持哪些版本

| 版本 | 是否支持 |
|------|----------|
| `develop` 分支最新提交 | ✅ |
| 最近一个发布标签（`v*`） | ✅ |
| 更早的标签 | ❌ |

## 报告漏洞的方式

请**不要**开公开 Issue。走以下任一条通道：

1. 首选：本仓库的 **Security → Report a vulnerability**（GitHub 私有漏洞报告），可以附复现代码、
   并在结论后决定是否给你署名致谢。
2. 退路：开一个只写「请通过私密渠道联系」的 Issue，剩下的沟通走私密渠道进行。

## 报告里请尽量带上

- 触发它的**最小输入**：原始字节（十六进制或 pcap / 抓包脚本都行），以及打到哪个端口、哪条协议路径
  （h1 / h2 / h3 / WebSocket）。
- 复现方式：能不能用仓库里的裁判脚本或压测脚本重放（`benchmarks/`、`scripts/`）。
  协议一致性的外部裁判有 `h2spec` 与 Autobahn 两套，进 CI 的那两个作业能直接指出是哪一条规则。
- 构建配置：Debug/Release、开没开 sanitizer、编译器与版本。
  内存类问题请在 **ASan/LSan/UBSan** 下复跑一遍——容器侧的门禁作业就是这么跑的。
- 你的机器与内核：Linux 侧请注意 io_uring 是编译期开关（`ASYN_WITH_IO_URING`，默认关）。

## 我们会做什么

- 三个工作日内回复确认，并给出初步影响判断。
- 确认成立后：先补一条**能证伪**的回归用例（即把修改动回去就必须变红），再修，再决定是否发安全公告。
- 如果这条缺陷已经被钉进过基线或示例，我们会一并改，并逐条报「旧断言 → 新语义 → 依据」。

## 严重度与响应时限

时限从**收到可复现报告**那天算起，是维护者个人的公开承诺，不是合同 SLA；需要商业支持协议请另谈。

| 严重度 | 典型形态 | 确认与初步判断 | 补丁或可用缓解 |
|--------|----------|----------------|----------------|
| 严重 | 远程可达的内存破坏、认证/校验绕过、请求走私、响应拆分、解密或密钥泄露 | 3 个工作日 | 14 个自然日；期间给出能落地的缓解配置 |
| 高 | 远程可达的拒绝服务（无界队列、限额绕过、连接/描述符泄漏）、越权读写他人会话 | 5 个工作日 | 30 个自然日 |
| 中 | 需要本地或特权配置才能触发；诊断面泄露敏感值（日志、错误文案带凭据） | 10 个工作日 | 下一个次版本 |
| 低 | 只影响可用性、告警质量或文档正确性 | 按普通缺陷流程 | 按普通缺陷流程 |

- 「已确认但未修」的条目会记在 `packaging/dependency-watch.json` 之外的公开 Issue 上打 `security` 标签，
  并在里程碑里给预计修复版本——不悄悄降级成「不修」。
- 判定严重度时以**默认配置**为准：把限额调大、关掉准入闸门、自签证书不校验属于使用方配置，
  按配置后果处理（见文末「不在漏洞范围内的事」）。

## 披露节奏

1. 报告进来后先在 GitHub 的 **private vulnerability reporting** 线程里沟通，不开公开 Issue。
2. 修复以普通提交进 `develop`，用例与证伪一起提（安全修复与缺陷修复走同一套门禁，不做暗分支）；
   在补丁进入 `main` 之前，我们不公开任何细节，也请报告方配合不抢先公开。
3. 补丁进 `main` 并发标签后，由维护者发一条 **GitHub Security Advisory**（含受影响版本范围、
   修复版本、CVSS 自评分与一句「为什么严重」），报告者可署名致谢。
4. 默认协调窗口 90 天：到期若补丁仍未落地，我们会主动与报告方商定公开方式，不把决定权留给单方面披露。
   报告方要求延后时最长到 120 天。

## 我们靠哪些持续验证

安全面不是靠人工审出来的，靠下面这些会红的门禁。「最近一次真实运行」一栏是为了避免把
「作业存在」当成「门禁跑过」——只在 `main` 推送与手动触发上跑，`develop` 不消耗分钟数。

| 门禁 | 覆盖什么 | 位置 | 最近一次真实运行（2026-10-02：`main` @ `05a4e1c` 的推送把 **Linux 十四条作业全部跑完并全绿**（三条分片、TSan、七档模糊、覆盖率、h2spec/Autobahn、aioquic 互操作、格式、Linux clang-tidy、io_uring 与 MySQL 两个可选档、示例矩阵、Pebble 验收）；Windows 侧两条作业同在 `eb2e936` 这一笔推送里全绿：`build-and-test`（含改名后的「示例矩阵」那一步）与新增的 `static-analysis` 四步全过。本轮新接的三条——示例矩阵、ACME 真机构验收、Windows 格式与静态档——都是**第一次在 CI 里真实跑完**，此前它们只在本地与容器跑；`main` 上的 **v2.4.0 `75930f7`** 四条也全绿。作业在界面里已是中文名，本表仍按 job id 引用它们） |
|------|----------|------|--------------------------------|
| 全量用例 + ASan/LSan/UBSan（Linux） | 内存安全、泄漏、未定义行为；协议解码器的对抗输入 | `linux-ci.yml` 的 `build-and-test` | `05a4e1c` 三分片全绿（`0f78db9` 那一轮的数据库分片跑了两小时以上还没出结果、也没有失败原文——运行中作业的日志接口回 `BlobNotFound`，取不到尾部；同一分片在 `05a4e1c` 正常时段内跑完，判为 runner 侧停摆、不复现，下次再撞要先按用例名取卡住现场）。本机容器同档 3601 例通过、71 例按门控 SKIP（共 3672 条）、零告警、零 sanitizer 命中 |
| 全量用例 + ASan（Windows/MSVC） | 完成端口、ConnectEx、多进程移交等平台特有路径 | `windows-ci.yml` | `eb2e936`（推送触发）全绿：3647 条 100% 通过；本机 Debug（含 ASan）3579 例通过、77 例 SKIP（共 3656 条）、零告警 |
| h2spec / Autobahn | HTTP/2 与 WebSocket 的规范一致性（第三方裁判逐条判据） | `protocol-conformance` 作业 | `0f78db9`：h2spec 常规 146/146、`--strict` 147/147（明文与 TLS 各一轮，两次都「全部核对通过」）；Autobahn 那一族也核对通过（计数由裁判脚本自己数）。2026-10-03 在本地对 `build/debug` 的 `reference_server` 复跑一次：裁判自数 **339 条、FAILED 0**（默认排除 9.1.6/9.2.6 两条超容量的），报告落 `/tmp/autobahn-ws-reports`——这一轮是给本批改过的 WebSocket 扇出（含二进制帧）拿独立实现侧的证据。跑这条脚本时**别从外面设 `MSYS_NO_PATHCONV=1`**：那条探测用 `-o /dev/null`，设了它 native curl 会把 `/dev/null` 当真实文件去建而必然假失败，报出来的是「服务端没起」。**上一笔 `e74eeac` 曾报 146 里 1 失败**（6.9.2「SETTINGS 把窗口调成负数」那一条，Expected DATA / Actual Connection closed），`0f78db9` 同一档位 146/146 复不上——本仓代码在这两笔之间没动过 HTTP/2 那层，`Http2Connection.h:537` 也明写着流级发送窗口「可为负、等 WINDOW_UPDATE 救回」。归因未定，下一次再红就按 6.9.2 抓那条连接的关闭现场（先取服务端那侧的关闭原因，别先改代码） |
| libFuzzer 七档解码器 | h1/h2/h3/WS 帧与 HPACK，外加 QUIC 报文头、帧序列、传输参数（UDP 上不看密钥就能走进去的那一段）；每档是否**真的在被推** | `protocol-fuzz` 作业（七档各 90 秒一轮，`scripts/check-fuzz-report.py` 按档核对） | `0f78db9`：单档计数最大的 worker 分别是 WebSocketFrame 1,439,089 / Http2Frame 1,887,326 / Http3Frame 1,906,324 / HpackBlock 1,015,881 / QuicPacket 4,803,460 / QuicFrameSequence 1,208,312 / QuicParameters 6,144,394，合计 18,404,793 次执行，**每档「串到别类」都是 0**（这行读数同时证明「每档独立预算」真生效）；零崩溃，语料制品留 14 天。把 QUIC 三档接进来的第一件事就是抓出一处真实缺陷：帧解码失败时半截产出还留在调用方复用的 scratch 缓冲里（`decodeQuicFrames` 的出参那一份），已修并用两条突变钉住。**这一格 2026-09-30 曾被判 `cancelled`**：30 分钟上限里将近 28 分钟耗在 runner 装 clang-19 的 apt 下载上（慢是「连接卡住但没报错」那种），与代码无关；上限已放宽到 60 分钟并给 apt 加了重试与单次超时 |
| ThreadSanitizer | 事件循环与协程唤醒的跨线程契约 | `thread-sanitizer` 作业 | 首跑 1300 例报出 1 处真竞争（h3 在线连接数的跨线程读），修后复跑全绿；第二处（WS 集线器）由同一条作业浮出、按用例违反线程契约收掉。`0f78db9` 复跑：筛面命中 **1325 / 全量 3663**（下限 1200），全绿——这组数与 `2d0bce6` 那轮逐字相同，说明筛面没被这轮的改名与新增示例动到。筛面按 21 个类名匹配，其余用例不在 TSan 档跑，所以新增一个跨线程面若不含这些词就一条都不跑；那种红现在看得见 |
| aioquic 跨实现验收 | 自研 QUIC 栈与另一套实现的互操作 | `http3-acceptance` 作业 | 全绿。h3 那一条现在是 11 个场景（条数由脚本现数，不再写死），含本轮接进来的**取消通路**：用 aioquic 以 RESET_STREAM 与 STOP_SENDING 打断 16 条流之后，收尾那条 GET 仍拿 200、全程不被连接级错误打断 （`scripts/h3_cancellation_probe.py` 09-28 就写好了，此前任何流水线都不调它） |
| 依赖公告台账与 SBOM | 每条第三方依赖的公告入口、固定版本、可交付清单 | `supply-chain.yml` | 两个作业（5 步 + 3 步）全绿，SBOM 制品随 `main` 推送上传（保留 90 天） |
| 示例矩阵（库外消费者的形状） | `samples/` 下每个自检程序各跑一遍、按退出码判；顺带让 LSan 跟着 Debug 档看示例自己漏没漏 | `linux-ci.yml` 的 `samples` 作业（全量 Debug 构建 + `scripts/run_samples.py`）与 `windows-ci.yml` 里 `build-and-test` 的「示例矩阵」一步 | `05a4e1c`：Linux 13 行全 PASS（Base 2、Core 4、Database 1、Net 5、Platform 1，作业自己打「共 13 次运行，失败 0 次」）；`eb2e936`：Windows 那一步 12 次运行、失败 0 次（少一条 `core_upgrade`，它只在 POSIX 构建）。这两条作业此前在 `.github` 里**一条引用都没有**，改坏一个示例不会让任何作业变红。假绿由脚本自己兜：一个可执行体都没找到退 2；ninja 清单里有、构建目录里没产物，当场报「它这一次根本没跑」。第一次真实跑就咬出一条红，但红在裁判自己身上——Windows 那张中文表头在 runner 的 cp1252 代码页上抛 UnicodeEncodeError，十二个示例早跑完了、判定整张丢（本机是 936 所以看不见），两个流钉成 UTF-8 后复跑转绿 |
| 格式门禁（钉版 clang-format 23.1.1） | 新增或改动的代码不符合基线即红；**输入清单非空**也在判；Linux 与 Windows 两侧同件同判据 | `formatting` 作业（Linux）与 `windows-ci.yml` 的 `static-analysis` 作业（Windows） | `0f78db9` Linux 全绿，作业打印参与检查的跟踪源文件 707 个（下限 500）；Windows 侧 `static-analysis` 作业 @ `eb2e936`（push 触发）打印的也是 707 个——同一件钉版 23.1.1（pip 装），CRLF 工作树没造成假违规（`.clang-format` 没锁行尾，clang-format 按文件自身的行尾派生）。改这一条之前的形状是 `git ls-files … \| xargs -0 clang-format`：清单一旦为空，`xargs -0` 会用零参数调用 clang-format，它读空 stdin 退出 0——glob 失配会让这道门变成「什么都没查还报绿」 |
| 覆盖率门禁（分模块 + 函数 + 分支） | 库本体（`src/`）的行覆盖**按模块各钉一条下限**，另判总函数与总分枝——只看总数时 Database 这种低档会一直隐身 | `coverage` 作业（gcovr 出数 + `scripts/check-coverage-gate.py` 判线） | `0f78db9`：Net 90.04（下限 85）、Platform 86.18（80）、Core 85.05（80）、Base 75.83（70）、Database 50.50（45）；总行 77.12（60）、总函数 85.37（75）、总分枝 62.55（50），可执行行总数 38240。**这道门 2026-09-30 之前不会红**——步骤吃的是管道末端 `tee` 的退出码；现在显式取 `PIPESTATUS[0]`，并对「CSV 空表 / `src/` 下一条记录都没有 / 可执行行总数低于 20000 / 声明了下限却查无此模块」四种情形退 2，而不是交出一张空表被读成「覆盖率没问题」 |
| 性能基线（同机前后对比，不进 CI） | 吞吐与 p50/p95/p99 四档的比值线；`maximum` 只录不判；基线里缺哪一档判据就报红，防的是「测量侧已经产出、门禁却看不见」 | `benchmarks/check-baseline.py` 配 `baseline.json`（Windows）与 `baseline-linux.json`（容器，入口是 `benchmarks/run-soak.sh`） | 2026-10-01 两份基线各自重录（每项三次取中位数、离散范围如实记下）并跑通：Windows 五项、Linux 五项，违规 0。判据本身用突变验过——从基线里删掉 `p99Microseconds` 会让这道门立刻报红；两边绝不互比，绝对读数差四五倍是虚拟机回路与宿主状态，不是代码 |
| clang-tidy（两侧） | 命名与静态检查；Windows 侧还顺手当第二双眼睛看 MSVC 不报的那几类 | `clang-tidy` 作业（Linux，`run-clang-tidy` 走 Debug 编译数据库）与 `windows-ci.yml` 的 `static-analysis` 作业（Release 编译数据库，逐翻译单元跑） | 两侧跑过且当前绿，但**按设计不阻塞发现项**（存量告警未清零前转阻断只会淹掉真信号）。Windows 那一侧的形状是本轮新建的，`eb2e936`（push 触发）读数：**src 下 194 个翻译单元，本档 Release 编译数据库里有 191 个**（不在的 3 个——`Epoll.cpp`、`Uring.cpp`、`InotifyFileWatcher.cpp`——是 POSIX 专有的，Windows 档根本不编，喂给 clang 只会报一片 `clang-diagnostic-error`，属范围错不是代码错，这 3 个由 Linux 那条覆盖）；标记 191/191（每件单独起一次、各留一条 `TIDY-PROCESSED <文件> rc=<码>`，缺一件就判红——批量 `xargs -n 8` 那版里「一条诊断都没有」「一个都没跑成」「xargs 被信号打断只剩半截」长得一模一样）；**本仓代码解析失败 0 个**、依赖头解析失败 2 个（`Gzip.cpp` 与 `PerMessageDeflate.cpp` 栽在 zlib 的 `zconf.h:486 'unistd.h' file not found`：那份头在「没有 _WIN32」的假设下走 POSIX 分支，Windows 侧改不动它，列进摘要不判红）、非零退出 8 个、诊断 276 行。为什么不拿退出码判红：本仓给 MSVC 开了 `/WX`，clang 读成 `-Werror`，于是 `unused-const-variable` / `unused-lambda-capture` / `unused-private-field` 这些 clang 独有的类别以 error 的面目出现、退出码非 0——那是发现项。驱动模式踩过一次反例：显式加 `--extra-arg=--driver-mode=cl` 会让 191 个里 190 个报解析失败（`std::optional`、`std::string_view` 全都找不到），那是 cl 风格的 `-std` 与 `/external:I` 一起没传到位，与运行库无关——默认档才是这台 runner 上能用的那一档 |
| 可选档专项编译 | MySQL 驱动能编（`-DDATABASE_WITH_MYSQL=ON`）、`io_uring` 后端能编 | `mysql-driver-compile`、`io-uring-compile` 作业 | `03c5bf5` 全绿；这两档不在主构建里，缺了就只能等发布后才知编不过 |
| 版本三处同解 | 根 `CMakeLists.txt` 的版本 / 更新日志最新发布段 / 最新标签必须一致 | `release-gate.yml` 的 `scripts/check-release-version.py` | 每次 `main` 推送都跑；不一致即红（包版本自 2.4.0 起由 Conan 配方的 `set_version()` 现读 CMake，不再写死第二份） |
| ACME 证书自动化 | 密钥层（JWK/JWS/CSR）、RFC 8555 状态机、续期循环与 h3 热轮换；进程内桩机构是**独立实现**的裁判（按 JWK 重建公钥验签、nonce 真的一次性、证书真从 CSR 签出），另有 `scripts/acme_pebble_cross_check.sh` 把对面换成 Pebble（LE 官方的 ACME 测试服务端）走完整条签发 | `tests/Net/Acme/`（随上面两条全量用例作业跑）+ `linux-ci.yml` 的 `acme-pebble` 作业（拉 `ghcr.io/letsencrypt/pebble` 镜像、只编签发探针，然后跑那条脚本） | `0f78db9`：`acme-pebble` 作业全绿，脚本自己打「全部 8 个场景通过」（对面起来取到目录、JWS 与 CSR 被接受并签出一张链、授权复用那条出口没被踩、探针退出码 0、两张证书都走了装回服务的动作、续期拿到另一张证书、链能验到对面本次启动的根、SAN 就是下单的域名）；桩机构那一族 2026-10-03 按 `ctest -N -R Acme` 现数 **90 例**（这一栏上一轮记的是 73 例，已经旧了——本批往 ACME 接了跟盘协程、装回重试与对外读数），Windows 侧实跑 100% 通过、1 例按门控 SKIP（真机构那一条要公网域名）。同日在容器的 `build-asan` 上对 Pebble 复跑那条跨实现脚本：**全部 8 个场景通过**，其中「两张证书都走了装回服务的动作」与「续期拿到的是另一张证书」两条，正是本批那两处改动的对面视角。这一轮就是它抓出两个缺陷：请求缺 `User-Agent`（真机构一律 400）、机构复用已 valid 的授权时又被触发一次挑战。退出码分三档：0 全过、1 某场景没过（被测面的问题）、2 前置缺件（拿不到裁判——镜像没拉到、探针没编出来、缺 openssl、端口被占、**通配解析没通**；最后这条本轮补上，解析不出来时对面回连取令牌必然失败，红会落在需要回连的场景上、读起来像实现有问题） |

「最近一次真实运行」这一栏是硬要求，不是装饰：上面这几条里有五条（h2spec、Autobahn、libFuzzer、
TSan 的复跑、格式门）在 2026-09-28 之前**只是存在过、从没真的跑完过**——红的都是工具链缺件，
而被"作业存在"掩盖着。补这一栏就是为了让这种状态一眼可见。

## 不在漏洞范围内的事

- 对本库配置的有意放宽（例如你把限额调到很大、关掉准入闸门、自签证书不校验）导致的后果。
- 需要攻击者已经能改你的构建产物或依赖清单的场景。
- 单纯「性能不如预期」——那请开普通 Issue，我们把它当缺陷处理但不走安全流程。
