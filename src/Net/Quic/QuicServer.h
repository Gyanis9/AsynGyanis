/**
 * @file QuicServer.h
 * @brief QUIC 服务端：一条 UDP 端口上承载多条连接（自研状态机 + OpenSSL 的 QUIC TLS 胶水）
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/StaticFileService.h"
#include "Net/Http3/Http3Connection.h"
#include "Net/Http3/Http3Session.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/QuicConnection.h"
#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include "Platform/IO/DatagramSocket.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/types.h>

namespace AsynGyanis::Net
{
    class Router;

    /**
     * @brief 地址验证令牌的签发器（定义见 `QuicAddressValidationToken.h`）
     * @details 这里只前向声明：令牌的算法与密钥都关在本服务端里，读 Retry 这一格的用法不需要
     *          把 HMAC 的细节带进每个 include 本头文件的地方
     */
    class QuicAddressValidationTokenCodec;

    /**
     * @brief QUIC 服务端：绑定一条 UDP 端口，把报文按连接标识分派给各自的连接
     *
     * @details 与 TCP 侧的分层对应关系：`TcpAcceptor` ↔ 本类（接入），`TcpStream` ↔ `QuicConnection`
     *          （一条连接的收发）。差别在于 QUIC 的连接标识在报文头里而不是四元组：同一个端口上
     *          任意多个对端都按报文里的目的连接标识路由，因此这里维护「连接标识 → 连接」的表，
     *          键是本端生成的 SCID（对端报文里的 DCID）。
     *
     * @warning 线程契约与 TCP 侧一致：本对象与它管的连接都只在所属事件循环线程上创建、使用与销毁。
     */
    class ASYN_NET_API QuicServer
    {
    public:
        /**
         * @brief 服务端配置
         */
        struct Configuration
        {
            std::string certificateFile; ///< 服务器证书（PEM）
            std::string privateKeyFile;  ///< 私钥（PEM）
            /// 会话票据密钥文件（可空：留空即 OpenSSL 默认，每个 SSL_CTX 一份随机密钥）。每份是
            /// 48 或 80 字节的二进制内容，首份用于签发、其余只用于解开轮换窗口内的旧票据。
            /// 多进程 worker 要共享恢复能力就得各进程装同一份：QUIC 的恢复走 TLS 1.3 票据，
            /// 密钥不共享时客户端第二次连接被分到别的进程只能退回全量握手
            std::vector<std::string> sessionTicketKeyFiles;
            /**
             * @brief TLS 握手策略：与 HTTPS、出站客户端共用同一份 `Core::TlsPolicy`
             *
             * @details 存在的理由是「一份策略喂三个监听器」：安全等级、TLS 1.3 套件、命名曲线、票据
             *          开关这些项，过去 HTTPS 侧改一处、QUIC 侧再改一处，两边迟早会漂。
             * @note 这里有几项对本监听器不适用，因为 QUIC 只跑 TLS 1.3、且本服务端不要求客户端
             *       证书：`cipherList`（1.2 及以下的套件串）、`certificateAuthorityFile/Path`、
             *       `verifyDepth`、`revocationListFile`/`revocationCoversWholeChain`。它们被原样带上
             *       而不报错，正是因为同一份策略多半还要交给 `HttpsServer` 或出站客户端用——
             *       在那两侧它们是生效的。「填了字段但这一侧用不上」与「这一侧支持但没生效」是两件事，
             *       后者才需要拒绝。
             * @note 版本区间由本层管到底：策略的下限低于 1.3 时这里钉回 1.3（只会更严，不算说谎），
             *       而把上限压到 1.3 以下是一条 QUIC 满足不了的配置，构造当场抛。
             * @note `cipherList`（1.2 及以下的套件串）对本监听器恒不适用——QUIC 只跑 TLS 1.3。
             *       其余那几项（`certificateAuthorityFile/Path`、`verifyDepth`、
             *       `revocationListFile`/`revocationCoversWholeChain`）是否生效取决于下面的
             *       `requireClientCertificates`：不开它，本服务端根本不会向对端要证书，那些字段装进
             *       信任库也没人去查；开了它们才真的参与校验（吊销检查同样只在这条开着时对客户端链生效）。
             *       被原样带上而不报错，是因为同一份策略多半还要交给 `HttpsServer` 或出站客户端用，
             *       在那两侧它们本就生效。
             * @see Core::TlsPolicy、requireClientCertificates
             */
            Core::TlsPolicy tlsPolicy;
            /**
             * @brief 是否要求并校验客户端证书（双向 TLS / mTLS）
             *
             * @details 与 `HttpsServer::setClientCertificateRequired()` 同一档能力，只是这里在构造期一次
             *          定：h3 的连接由数据报路由而来，没有「先监听再改配置」的中间态，改校验模式只能整台换。
             * @note 为真时 `tlsPolicy.certificateAuthorityFile` 或 `certificateAuthorityPath` 至少要给一项，
             *       否则构造当场抛——「要求校验却没有信任锚」在握手里的形态是每条连接都失败，与其留一条
             *       永远握不成的监听器，不如在启动时就点名。与 HTTPS 侧同一判据（那边是拒绝启用而非静默放行）。
             * @note 开启后对端不出示证书即被拒（`SSL_VERIFY_FAIL_IF_NO_PEER_CERT`），不退化成「可选校验」。
             */
            bool requireClientCertificates{false};
            /// 同时在线连接上限；显式写 0 = 不限，与 `HttpServerConfig::maximumConnections` 同口径
            /// （那边 0 就是不限，抄过来时若把 0 当成「一个也不收」，症状是一台监听正常却谁也连不上）。
            /// 默认取 1024 而不是 0：QUIC 的每条在线连接都常驻一份 TLS 会话、流表与拥塞状态，
            /// 而且它没有内核接受队列可挡——新连接一握手就是本端的账，所以本端自己先设一道有限值。
            /// HTTP 侧的默认值如今也是有限值（`kDefaultMaximumConnections`），两处的数不同是刻意的：
            /// h3 的单位连接比一条 TCP 连接贵，配额不该一样
            std::size_t               maximumConnections{1024};
            std::chrono::seconds      idleTimeout{30};           ///< 空闲超时：超过即由传输层收口
            std::string               applicationProtocol{"h3"}; ///< 必须协商出的 ALPN；本实现只服务 h3（RFC 9114 §3.2），配成别的值在构造期即被拒
            std::chrono::milliseconds expiryTickInterval{10};    ///< 定时器驱动的节拍（见 runExpiryTicker 的说明）
            /// 每条连接用哪一家拥塞控制算法：缺省 NewReno（与这一格落地之前的部署逐字一致），
            /// CUBIC 见 RFC 9438。逐连接透传给传输状态机，理由与取值见
            /// `QuicConnectionCoreConfiguration::congestionAlgorithm`
            QuicCongestionAlgorithmKind congestionAlgorithm{QuicCongestionAlgorithmKind::NewReno};
            /// h3 会话的请求解析上限（正文总量上限等），与 h1/h2 同一套配置。
            /// 不设置时用 HttpParserLimits 的默认值——不能没有上限：一条 POST 就能把内存吃光
            HttpParserLimits parserLimits{};
            /// 本端在 HTTP/3 SETTINGS 里公布的能力，也是 QPACK 两侧与单帧缓冲的实际约束
            /// （RFC 9114 §4.2.2、RFC 9204 §5、RFC 9220 §3.1）。此前服务端一侧只能吃编译期默认值：
            /// 连接层早就按这一份配置接 QPACK、限帧长、决定认不认扩展 CONNECT，缺的只是交进来的那道口
            Http3Connection::LocalSettings http3Settings{};
            /// 在途正文字节的全局预算（可空：空表示不受约束）。与 HTTP 侧共用同一份账——
            /// h3 的正文同样驻留在进程内存里，只限「单条流」挡不住 100 条流各压 8 MiB
            std::shared_ptr<HttpMemoryBudget> memoryBudget;
            /// 统计采集端（可空：空表示 h3 流量不采集）。
            /// 与 HTTP 侧共用同一个实例，那台服务器的 /metrics 就一并覆盖 h3（见
            /// HttpServer::metricsCollector()）；单独采集时用 stats() 读本服务端的快照
            std::shared_ptr<HttpMetricsCollector> metricsCollector;
            /// request-id 生成器（可空：空表示 h3 不为请求落定 id）。与 HTTP 侧共用同一个实例：
            /// 同一来源不管走 h1/h2 还是 h3，日志与 x-request-id 里的前缀都指向同一台服务器
            std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator;
            /// 连接级限额（可空）：目前用到「单连接最多处理多少条请求」——达到后 h3 会话发 GOAWAY
            /// 排空，在途请求答完再由本服务端收掉这条连接。空闲时长不在此列：QUIC 自带
            /// `idleTimeout`，那是传输层的收口时刻，与 HTTP 侧的 keep-alive 空闲不是一回事——
            /// `HttpServerLimits::idleTimeout` 承诺的「WebSocket 帧间空闲」在 h3 上也由这一把钟兜：
            /// 隧道挂在一条双向流上，而那条流随整条连接一起收
            std::shared_ptr<const HttpServerLimits> serverLimits;
            /// 单来源并发连接上限的限额器（可空：空表示不按来源限制）。与两条 TCP 通道共用一个实例时，
            /// 同一来源不管从 TCP 还是 QUIC 进来都算在同一个名额里
            std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter;
            /**
             * @brief 地址验证令牌的有效期（秒）；0 表示不开这项保护（缺省，逐字保持这一格落地之前的行为）
             *
             * @details 开了之后，每个不带令牌的新连接 Initial 都先吃一条 Retry（RFC 9000 §8.1.2）：本端
             *          不为它建任何状态，等它带着令牌重来才建。这一格的存在理由是公网 UDP 端口上的成本
             *          不对称——一条 Initial 就能让本端建一份 TLS 会话、一张流表与一套拥塞状态，而发的人
             *          不必是自己。测试计划里「放大/反射字节比 ≤3」与「Initial 洪水」两格都靠它收口。
             * @note 开了之后自家出站客户端连不上：本实现的客户端不处理 Retry 报文（见 §17.2.5.2 那格
             *       未落地），所以要由能接 Retry 的客户端来连（跨实现核对用的 aioquic 就能）。
             * @note 令牌由 HMAC 算出，密钥见 `addressValidationKey`。有效期就是密钥轮换的窗口：
             *       旧密钥签出的令牌最长活到这里，之后就没人再认它。
             */
            std::chrono::seconds addressValidationTokenLifetime{0};
            /**
             * @brief 令牌 HMAC 的签发密钥；留空＝启动时随机自造一把
             *
             * @details 多进程 worker 必须配同一份（同一台机器上的几份进程共享一把密钥，或从同一份
             *          密钥文件读）：各进程随机自造时，客户端带着 A 签的令牌撞到 B 必然验不过，症状是
             *          「同样的握手偶尔要重来一次」——与 `sessionTicketKeyFiles` 那格同一条道理。
             *          低于 16 字节在构造期即被拒（RFC 9000 §8.1.4 的「难猜」要求）。
             */
            std::vector<std::uint8_t> addressValidationKey{};
            /**
             * @brief 无状态重置的静态密钥（RFC 9000 §10.3.2）；留空＝本实例启动时随机自造一把
             *
             * @details 用在哪：一条目的标识不属于任何在册连接的短头报文会换回一条重置（§10.3），
             *          令牌就是拿这把密钥与「报文里那个目的标识」现算的（§10.3.2：丢掉了状态的端点
             *          用同一个方法算出对端认得的令牌）。不共享的代价比地址验证那次更隐蔽：
             *          多进程 worker 各自一把随机密钥时，撞到别的工作进程那份旧连接的报文只能继续
             *          不理，而客户端认不出「连接没了」，只能等它自己的空闲超时。至少 16 字节
             *          （§8.1.4 的「难猜」口径，构造期即拒）。
             * @note 这条密钥与连接自己的签发令牌是同一把：连接把令牌发在 NEW_CONNECTION_ID 与
             *       `stateless_reset_token` 传输参数里，进程没了之后服务端再按同一算法算出来，
             *       两边才对得上
             */
            std::vector<std::uint8_t> statelessResetKey{};
        };

        QuicServer(Core::EventLoop &eventLoop, Configuration configuration);

        /**
         * @brief 换一把令牌签发密钥：新令牌按新密钥签，旧密钥留着验它签过的那批
         *
         * @details 轮换不需要停监听，也不需要清任何连接表——令牌本身带着签发时刻与有效期，旧密钥的读者
         *          最多再存在一个有效期。这是运维侧「密钥泄露了就换一把」的入口；本实现不把密钥写进磁盘，
         *          也不做定时自动轮换：那是调度方的事，交给它按自己的窗口调这里。
         * @param newSigningKey 新密钥，至少 16 字节
         * @throws Base::InvalidArgumentException 用法错误：这一项保护没开（`addressValidationTokenLifetime`
         *         为 0 时本服务端不签令牌，轮换无从谈起）、或新密钥短于下限
         */
        void rotateAddressValidationKey(std::span<const std::uint8_t> newSigningKey);

        /**
         * @brief 建一台接手别人已经绑好的数据报套接字的服务端（一条 UDP 端口交多个进程的那一步）
         * @param eventLoop 所属事件循环
         * @param configuration 配置（按值收：此后不再读调用方那份）
         * @param adoptedBoundSocket 已经 bind 过的数据报套接字，所有权随之转移；本对象析构或 stop()
         *        会关掉它——交出去的那一方此后不该再碰它
         * @throws Base::InvalidArgumentException 交来的套接字无效（`bindTo` 失败或已被移走的那份空对象）
         * @throws Base::SystemException 证书/私钥那几项加载失败（与按地址构造同一条口径）
         * @details 存在的理由：Windows 没有 `SO_REUSEPORT` 的等价物，多个进程各自 bind 同一端口时内核
         *          把全部报文交给最后绑上的那一个，其余进程一个错都不报却永远收不到报文；零停机换代里
         *          新一代也要在同一端口上接手已绑的口。两条路都缺这一格，因为按地址构造只会自己 bind。
         * @note 套接字从哪来由调用方决定：`Platform::DatagramSocket::adopt` 把别的进程交过来的描述符
         *       包成本层的持有者语义，它会区分「描述符无效」「不是套接字」「不是 SOCK_DGRAM」「还没 bind」四种不合格
         * @note 与按地址构造的差别只在端口从哪来：本构造不会去 bind，端口从交来的套接字问，
         *       `listeningPort()` 报的是真值
         */
        QuicServer(Core::EventLoop &eventLoop, Configuration configuration, Platform::DatagramSocket adoptedBoundSocket);

        ~QuicServer();

        QuicServer(const QuicServer &) = delete;

        QuicServer &operator=(const QuicServer &) = delete;

        /**
         * @brief 绑定端口并开始服务（在所属事件循环上跑，直到 stop()）
         * @param localAddress 本地地址（端口给 0 表示由内核分配）。按值收：本方法是惰性协程，
         *        收引用会把「调用方传的临时量」留到协程恢复时再用，那是悬空引用（实测 ASan 报过
         *        stack-use-after-scope）
         * @return Core::Task<> 停止时完成
         * @throws Base::InvalidArgumentException 本对象是接手构造出来的（端口已经定在交过来的套接字上，
         *         那种情形请调不带地址的 listen()）、或这台服务端已经在某个端口上监听（一台只能启动一次）
         * @throws Base::Exception 证书/私钥加载失败、套接字绑定失败等启动期就该拦住的问题
         * @note 本方法是惰性协程：抛出发生在首次恢复（await 或投递给循环）那一刻，不在调用处
         */
        [[nodiscard]] Core::Task<> listen(Core::InetAddress localAddress);

        /**
         * @brief 在接手来的套接字上开始服务（在所属事件循环上跑，直到 stop()）
         * @return Core::Task<> 停止时完成
         * @throws Base::InvalidArgumentException 本对象不是接手构造出来的：那种情形要调
         *         listen(地址)，由本端自己 bind；或这台服务端已经在某个端口上监听（一台只能启动一次）
         * @note 本方法是惰性协程：抛出发生在首次恢复（await 或投递给循环）那一刻，不在调用处
         * @note 交付口径与 listen(地址) 完全一致，差别只在端口与套接字都不是本端建的，因此这里
         *       不会有「绑定失败」那一类出口
         * @warning 本协程的帧必须活到 `stop()` 之后（与其它循环对象同一条销毁纪律）
         */
        [[nodiscard]] Core::Task<> listen();

        /**
         * @brief 请求停止：只置停止标记，收循环与定时器在下一次醒来时据此退出
         * @details 不在这里关套接字、也不销毁注册对象：本方法可能从别的线程调用，而那两者只归所属事件
         *          循环线程——在途探针还没收回来时销毁注册对象，循环会踩到已释放的对象。因此要真正把服务
         *          停下来，得停止事件循环或在循环线程上销毁本服务端，那两条路径都会关掉描述符，挂在读数
         *          上的协程于是收回。
         * @note 与 `UdpServer::stop()` 的差别就在这一格：那一台的 stop() 会顺手关掉套接字并叫醒等待者，
         *       所以它要求在所属循环线程上调用（或把调用投递过去）
         */
        void stop() noexcept;

        /**
         * @brief 设置收到流数据时的直通回调（只在没接 HTTP/3 路由器时生效）
         * @param handler 回调
         */
        void setStreamDataHandler(QuicConnection::StreamDataHandler handler);

        /**
         * @brief 把 HTTP/3 接到既有路由器上
         * @param router 路由器；必须活得比服务端久（本类只存指针）
         * @note 接上之后，每条连接在收到第一条流数据时自动建立 HTTP/3 会话：控制流与两条 QPACK 流
         *       由本类替它开出来，h3 请求按既有路由与处理器派发——业务代码与 h1/h2 是同一份。
         *       不接路由器时服务端只做传输层，流数据交给 setStreamDataHandler() 的直通出口
         */
        void setRouter(Router &router) noexcept;

        /**
         * @brief 用配置里记下的路径重载证书，成功则整台换用新的 TLS 上下文
         * @return true 新证书已生效（此后新建的 h3 连接用它）；false 表示证书或私钥读不出来、
         *         不配对，此时旧上下文原样继续服务
         * @details 存在的理由与 `Core::TlsContext::reloadCertificate()` 同一条：证书自动化的标准动作是
         *          「新证书覆盖到原路径」，路径就是身份。h3 一侧此前只能重启进程才换证书，于是同一份
         *          自动化能喂饱 443 上的 TCP 与 UDP，却单缺 h3 那一条通道。
         * @note 在途连接不受影响：OpenSSL 的 SSL 对象持有自己那份上下文引用，最后一个引用消失前不会
         *       释放，因此换下来的旧上下文会活到最后一条用它建起来的连接收尾
         * @note 本方法可从运维线程调用（它与 routeDatagram 的读点之间由一把锁定序）；新建 SSL 只发生在
         *       循环线程上，因此锁的持有时长只是取一份 shared_ptr 拷贝
         * @see Core::TlsContext::reloadCertificate(), HttpsServer::reloadCertificate()
         */
        bool reloadCertificate();


        /**
         * @brief 换一份连接级限额：与两条 TCP 监听器同一个形状，取值 0 的字段表示关闭对应保护
         *
         * @details 存在的理由与 `HttpServer::setLimits()` 同一条：`Configuration` 里那份限额要先交给
         *          `HttpMetricsEndpoint`/`healthCheckMiddleware` 之类共用对象，而 h3 的限额此前只能在建
         *          服务端那一刻定死——同一份配置喂三条通道时，h3 就成了那条改不动的。
         * @note 与两条 TCP 侧同一口径：必须在 listen() 之前调用。限额按 shared_ptr 只读交给此后
         *       每条连接新建的会话，已经建立的会话继续用它构造那一刻那一份（换一份新对象而不是改写
         *       原对象，是为了不让在途会话读到半新半旧的组合）。
         * @see HttpServerLimits, setParserLimits(), limits()
         */
        void setLimits(HttpServerLimits limits);

        /// 当前生效的连接级限额（构造时 `Configuration::serverLimits` 那份，或最后一次 setLimits()）
        [[nodiscard]] HttpServerLimits limits() const;

        /**
         * @brief 换一份请求解析上限（头部条数、正文总量、请求目标长度那一组）
         * @param limits 新的解析上限；取值 0 的字段表示关闭对应保护（见 `HttpParserLimits`）
         * @note 同 `setLimits()`：必须在 listen() 之前调用，此后新建的会话才看得到这份新值。
         *       按值保存而不是共享指针——解析上限只在会话构造那一刻被取走一份副本，之后没有读者
         * @see HttpParserLimits, setLimits(), parserLimits()
         */
        void setParserLimits(HttpParserLimits limits);

        /// 当前生效的解析上限
        [[nodiscard]] HttpParserLimits parserLimits() const;

        /**
         * @brief 设置（或关闭）静态文件目录：第三条通道也接同一份静态服务实现
         * @param directoryPath 静态文件根目录（UTF-8 文本），相对或绝对均可；空串表示关闭
         * @details 与 `HttpServer::staticFileDir()` / `HttpsServer::staticFileDir()` 共用
         *          `StaticFileService`：目录在此刻规范化、Cache-Control 的非法字符当场忽略并告警、
         *          那条 `any("*")` 兜底路由只登记一次。零拷贝发送是 Linux 明文侧的形态，h3 上走的是
         *          映射正文交给连接层这条路，语义相同。
         * @throws Base::InvalidArgumentException 还没 setRouter() 时调用：兜底路由要有地方登记，
         *         而本服务端的路由器是外部交来的（不持有），先给路由器再配静态目录是唯一说得清的顺序
         * @note 必须在 listen() 之前调用：路由登记发生在配置时刻，之后每条新连接才看得到这条兜底
         */
        void staticFileDir(const std::string &directoryPath);

        /// @brief 读回当前生效的静态根目录（UTF-8 文本）；未启用时为空串
        [[nodiscard]] std::string staticFileDir() const;

        /// @brief 设置静态文件响应的 Cache-Control 值；空 optional 表示不发这条头
        void setStaticFileCacheControl(std::optional<std::string> cacheControl);

        /**
         * @brief 读回当前生效的静态文件 Cache-Control 值
         * @details 与 `HttpServer::staticFileCacheControl()` / `HttpsServer::staticFileCacheControl()` 同口径：
         *          写侧三个通道都有，读侧此前只在这两个上有——运维要问「这条通道当下发的是哪条 Cache-Control」
         *          时在 h3 上问不出，而上面那个 `staticFileDir()` 读口就在旁边，缺这一格没有道理。
         * @return std::optional<std::string> 已设置的值；未设置或从未配置静态目录时为空 optional
         */
        [[nodiscard]] std::optional<std::string> staticFileCacheControl() const;

        /**
         * @brief 设置请求打到静态目录时是否生成 HTML 目录列表
         *
         * @details 默认关闭：列表会把目录结构、文件名与大小交给任何一句 `GET /assets/` 的探测者，
         *          而多数部署里静态目录只是资源仓库。打开之后列表不带验证器且带
         *          `cache-control: no-store`（被缓存的列表会在目录变化后继续宣称「这里有这些文件」），
         *          Range 对它一律忽略，条目数有上界。完整语义见 `StaticFileService::setDirectoryListing()`。
         * @param enabled true 列出目录条目；false（默认）按 404 处理
         * @note 必须在 start() 之前配置，与静态目录本身同一时限
         * @see staticFileDir(), StaticFileService::setDirectoryListing()
         */
        void staticDirectoryListing(bool enabled);

        /**
         * @brief 设置静态服务要不要按 `Accept-Encoding` 挑选同目录的预压缩副本
         * @details 与 `HttpServer::staticPrecompressedVariants()` 同一语义、同一份实现
         *          （本体在 `StaticFileService`）：三条通道若在表示协商上给出不同答案，
         *          表现就是换个协议换个形状。默认关闭。
         * @param enabled true 参与变体协商；false（默认）只发请求路径本身那份
         * @note 必须在 start() 之前配置，与静态目录本身同一时限
         * @see staticFileDir(), StaticFileService::setPrecompressedVariants()
         */
        void staticPrecompressedVariants(bool enabled);

        /**
         * @brief 设置 RFC 9842 的压缩字典：静态资源按 `Available-Dictionary` 协商发 `.dcz` 副本
         * @details 与 `HttpServer::staticCompressionDictionary()` 同一语义、同一份实现（本体在
         *          `StaticFileService`）。本通道天然满足「安全上下文」那条门（h3 恒为 TLS）。
         * @param relativePath 字典文件相对静态根的路径；空串表示关闭
         * @param matchPattern `Use-As-Dictionary` 的 match 取值；空串一并表示关闭
         * @note 必须在 start() 之前配置，与静态目录本身同一时限
         * @see staticFileDir(), StaticFileService::setCompressionDictionary()
         */
        void staticCompressionDictionary(const std::string &relativePath, const std::string &matchPattern);


        /**
         * @brief 当前在线连接数
         * @details 读的是连接表的原子镜像：表本体只归事件循环线程，外部线程直接取它的 size()
         *          就是数据竞争（采集端与探活工具都在循环外读这个数）
         * @return std::size_t 这一刻的在册连接数；增删由循环线程刷新，读数最多滞后一次变更
         */
        [[nodiscard]] std::size_t connectionCount() const noexcept;

        /**
         * @brief 取「尚未完成握手」的在线连接数
         * @details 与 connectionCount() 同一条跨线程口径（读原子镜像，relaxed）：两者之差就是
         *          已验证的那部分。未验证连接与已验证连接共用同一份 maximumConnections 是刻意的
         *          （理由见 QuicConnection.h 的刻意清单），本读数把「名额被谁占着」变成看得见的数——
         *          它在涨而 connectionCount() 不动，说明来的是只发 Initial 不推进握手的流量
         * @note 镜像在每拍末尾、摘除收口之后重算（完成点埋在状态机内部，散布加减迟早漏一处；
         *       排在摘除之后，最后一条离表当拍就归零）：连接在表时最迟一个节拍（默认 10ms）内反映；
         *       空表期间新到的连接最坏等满一拍空闲上界（kIdleTickerSleep，1s）才被算上
         * @return std::size_t 未完成握手的在册连接数
         */
        [[nodiscard]] std::size_t handshakingConnectionCount() const noexcept;

        /**
         * @brief 收包侧「一次批次交付」的两笔读数：批次数与报文条数
         * @details 这两个数要一起看才有含义：`datagramCount() / datagramBatchCount()` 就是
         *          「一次就绪平均收了几条」。Linux 侧走 `recvmmsg`，这个比值随链路负载上去；
         *          Windows 侧没有批量入口（逐条 `recvfrom` 的退化档），比值恒为 1——
         *          把两侧写成同一个承诺就会说谎，因此这里只交读数不交结论。
         *          原子量因为采集端与用例都在循环外的线程上读
         * @return std::uint64_t 自本对象建立以来的累计值
         */
        [[nodiscard]] std::uint64_t datagramBatchCount() const noexcept;

        /// 收包侧交付的报文条数累计（含被当作空报文丢弃的零长报文，那也是一次真实的读数）
        [[nodiscard]] std::uint64_t datagramCount() const noexcept;

        /**
         * @brief 发包侧的两笔读数：批次调用数与其中交出的报文条数
         * @details 与收包那两条同形，判的也是同一件事：一轮 flush 攒出的多个报文一次交出去。
         *          一轮最多攒 64 个，而单次批次上限是 8 个，所以长轮要分几窗——比值读的是
         *          「一次系统调用平均交几条」，不是「一轮一条」。
         * @return std::uint64_t 自本对象建立以来的累计值
         */
        [[nodiscard]] std::uint64_t datagramBatchSendCount() const noexcept;

        /// 发包侧交给内核的报文条数累计
        [[nodiscard]] std::uint64_t datagramSentCount() const noexcept;

        /**
         * @brief 取本服务端实际生效的最大并发连接数
         * @details 与 `TcpServer::maximumConnections()` 同一条问句：`connectionCount()` 给的是分子，
         *          没有这一句就算不出「这台 h3 是不是已经贴着上限跑」。报的是构造时下发到本台的值
         *          （h3 的整机摊分由装配方负责，与配置里的整机数可能差几倍，见 Configuration 的说明）；
         *          0 表示本台不设这道限
         * @return std::size_t 最大并发连接数
         */
        [[nodiscard]] std::size_t maximumConnections() const noexcept;

        /**
         * @brief 取本服务端挂着的按来源 IP 的限额对象（没设为空指针）
         * @details 这条读口与 TCP 侧同名同义：限额常是多台共用的一份，交回的就是那一份，
         *          调用方因此能问出真正生效的每来源上限，也能把它继续挂到另一台监听器上
         * @return const std::shared_ptr<PerIpConnectionLimiter> & 持有中的那份对象
         */
        [[nodiscard]] const std::shared_ptr<PerIpConnectionLimiter> &perIpConnectionLimiter() const noexcept;

        /**
         * @brief 取本服务端的统计快照（h3 会话的请求数、状态码类、耗时、单流取消与在线连接数）
         * @details 与 HttpServer::stats() 同一形态与同一套指标口径，只是数据来自 h3 会话：
         *          请求数、状态码类与耗时直方图都由各会话累加（h3 的耗时起点是会话收下这条请求的
         *          那一刻，详见 Http3Session 构造函数的说明）；本服务端额外就地补上在线连接数
         * @return HttpServerStats 快照；未配置采集端时除在线连接数外各计数为零
         * @note 可从任意线程调用（各项计数都是原子量，在线连接数取自连接表的原子镜像）
         */
        [[nodiscard]] HttpServerStats stats() const;

        /**
         * @brief 本端实际绑定的端口
         * @return std::uint16_t 端口；尚未绑定时为 0
         * @note 可从别的线程读：绑定成功时才写入非 0 值，因此读它等同于问「监听起来了吗」
         */
        [[nodiscard]] std::uint16_t listeningPort() const noexcept;

        /**
         * @brief 优雅收口：先挡新连接，给已有连接发 GOAWAY，等在途请求做完，到期再兜底强关
         * @details 与 `TcpServer::drain()` 同一形状。这里不能直接 `stop()`：收报文那条循环会
         *          随之退出，在途请求的后续报文与 ACK 再也进不来，「等它做完」就无从谈起。因此第一段
         *          只置「不再接受新连接」的标记，h3 侧则在各连接的控制流上发一条 GOAWAY（RFC 9114 §5.2）
         *          告诉对端别再发新请求；等待期间的驱动仍由定时循环与本协程的轮询完成。
         * @param drainTimeout 最长等待时长；非正数表示不等，直接收口全部连接
         * @return Core::Task<> 连接已清空或期限到时完成
         * @note 线程约束同 `stop()`：必须在运行本服务器的那个事件循环线程上调用（外部线程请走
         *       `scheduler().scheduleRemote()`），本协程要遍历并关闭连接
         */
        [[nodiscard]] Core::Task<> drain(std::chrono::milliseconds drainTimeout);

    private:
        /// 排空期间的轮询间隔：决定「没有在途工作」多快被发现，代价是等待期间多几次唤醒
        static constexpr std::chrono::milliseconds kDrainPollInterval{50};

        /**
         * @brief 两条公开构造的共同实现：先判接手来的套接字，再建 TLS 上下文
         * @param eventLoop 所属事件循环
         * @param configuration 配置（按值收：此后不再读调用方那份）
         * @param adoptedBoundSocket 接手模式给那份已绑好的套接字，按地址模式给 `std::nullopt`
         * @throws Base::InvalidArgumentException 接手来的套接字无效——这一判据排在创建 TLS 上下文之前，
         *         抛出去时手上还没有需要归还的资源（构造期抛出后析构不会跑，而上下文一旦交接给成员就无人认领）
         * @throws Base::SystemException 证书/私钥那几项加载失败
         */
        QuicServer(Core::EventLoop &eventLoop, Configuration configuration, std::optional<Platform::DatagramSocket> adoptedBoundSocket);

        /**
         * @brief 已经在监听就拒绝再一次启动（两条 listen 入口共用同一条判据）
         * @throws Base::InvalidArgumentException 本服务端已经在某个 UDP 端口上服务
         */
        void requireFreshStart() const;

        /**
         * @brief 把监听这条数据报通道要的两项套接字选项接上：ECN 字段可见性与「不要在 IP 层分片」
         * @details 两项都在这里一次定论并记进成员，之后各连接只读成员不再碰套接字：ECN 决定收报文时
         *          报不报拥塞标记（§13.4.1），DF 决定允不允许各条连接发路径 MTU 探针（RFC 9000 §14）。
         *          设不上都不是错误（Windows 就没有 ECN 那个入口），所以只把结论与原因记成一行日志，
         *          不改控制流。
         * @pre `m_socket` 已建好（两项选项都经它下发）
         */
        void applyDatagramSocketOptions();

        /**
         * @brief 两条 listen 的共用主体：在已就绪的套接字上问回端口、建封装并跑收循环
         * @details 端口与本地地址都从套接字问回来（接手来的口没有别的来源），把「端口从哪来」与
         *          「收到报文后做什么」分开，才不会出现两种形状各有一份派发的分叉。
         * @pre `m_datagramSocket` 有效且已 bind（由两条 listen 入口各自保证）
         * @return Core::Task<> 收循环退出（stop() 之后）时完成
         */
        [[nodiscard]] Core::Task<> serveOnBoundSocket();

        /**
         * @brief 定时器驱动：按「下一次真正有东西可查的时刻」检查各连接的到期
         * @details QUIC 的 PTO/空闲超时/握手超时都要在「没有报文到达」时也准时触发，因此不能只靠
         *          收到报文时顺手处理。原来是一律按固定节拍（默认 10ms）轮询——简单可预期，代价是
         *          到期处理最多晚一个节拍，且在线连接为零时也在轮。现在按
         *          `nextTickerWakePoint()` 那条算法定睡眠点：早于节拍的截止按时到，晚于节拍的仍按
         *          节拍到（needsFlush 那一档补刀与 h3 请求的读时限没有可查的截止时刻，只能靠轮询兜），
         *          零连接时退到 kIdleTickerSleep。
         * @note 本协程由收报文那一路就地起第一拍（不投调度器）并在 stop() 之后 co_await 到它退出：
         *       它的帧因此从启动到结束只由那一条协程拿着，队列里不会出现指向已销毁帧的句柄（TSan 抓到过
         *       投调度器那一版的这条）。代价是收报文那一路的收尾最晚延后一拍——这只延后监听协程自己完成
         *       的时刻，循环若在收尾前就停了，两帧一起由调用方销毁，不会把谁卡住
         * @return Core::Task<> 停止时完成
         */
        [[nodiscard]] Core::Task<> runExpiryTicker();

    protected:
        /// 连接表空着时一拍睡多久：一台没有在线连接的监听器没有任何到期要落实，节拍纯粹是白烧唤醒。
        /// 实测（benchmarks/h3_syscall_ab.sh，负载跑完之后那段空档）空闲节拍占掉整场画像里
        /// 四万次 epoll_wait 的绝大部分，按 10 毫秒节拍折算就是每秒 100 次唤醒、每次约两趟等待。
        /// 取 1 秒而不是无限：新连接的头一拍最多延后这一档，之后节拍自动回到 expiryTickInterval。
        static constexpr std::chrono::milliseconds kIdleTickerSleep{1000};

        /**
         * @brief 在线连接数达到这个上限时是否还容得下新连接（纯换算，不读状态）
         * @param onlineConnectionCount 当前在线连接条数
         * @param maximumConnections 配置的上限；0 = 不限
         * @return true 可以为一新连接建状态
         * @note 单列成纯函数是为了能确定性地把「0 是不限」这条钉住：真要端到端地拒一条连接，
         *       得先跑完一次握手，那不该是用例的前提
         */
        [[nodiscard]] static bool admitsNewConnection(std::size_t onlineConnectionCount, std::size_t maximumConnections) noexcept;

        /**
         * @brief 单来源准入的一次判定：名额租约与「被拒」两个出口
         */
        struct PerIpAdmission
        {
            std::optional<PerIpConnectionLimiter::Lease> lease{};          ///< 拿到的名额租约；未配置限额器或取不到来源键时为空
            bool                                         isRefused{false}; ///< 被单来源闸拒绝，调用方应立即收手
        };

        /**
         * @brief 尝试占用一个单来源名额
         * @details 被拒的连接不进别名索引，对端握手重传会以新 Initial 反复走到这里——被拒时的告警在
         *          本方法内按时间窗压住（理由见 .cpp 里窗口常量的说明）
         * @param peerAddress 对端地址（键取纯 IP，与 TcpServer 同一口径）
         * @return PerIpAdmission lease 非空＝拿到；isRefused＝被拒；两者皆空＝不适用（无限额器或取不到
         *         来源键），照常放行
         */
        [[nodiscard]] PerIpAdmission admitPerIpConnection(const Platform::SocketAddress &peerAddress);

        /**
         * @brief 算出定时器驱动下一次该在什么时候醒来（纯换算，不读状态）
         * @details 单列成一张纯函数是为了能被确定性地测出来：读时钟的分支在测试里只能靠等，
         *          而「什么时候该醒」这件事恰恰是这条改动的全部判据。放在 protected 而不是 private，
         *          是为了让用例按派生类转发的既有办法直接调它（不必构造一台带证书的服务端）。
         * @param hasConnections 当前是否还有在线连接（零连接时不看截止时刻，直接按空闲上界睡）
         * @param earliestExpiry 全部连接里最早的交易截止时刻；没有可查的截止时传 `time_point::max()`
         * @param now 计算时刻
         * @param tickInterval 配置的节拍上限（`Configuration::expiryTickInterval`）
         * @return 下一次唤醒的绝对时刻，恒不早于 now、恒不晚于 now + max(tickInterval, 空闲上界)
         */
        [[nodiscard]] static std::chrono::steady_clock::time_point nextTickerWakePoint(bool hasConnections, std::chrono::steady_clock::time_point earliestExpiry,
                                                                                       std::chrono::steady_clock::time_point now, std::chrono::milliseconds tickInterval);

    private:
        /**
         * @brief 新建连接前那道地址验证的三档出口
         * @details 有值的是「这一档怎么走」而不是「成不成功」：放行、回 Retry、丢弃各算一种，
         *          三档在读数与日志上是三件事，混成一档就看不出端口上到底来的是什么流量
         */
        enum class QuicAddressValidationDecision
        {
            Proceed,       ///< 令牌验过（或这项保护没开）：照旧建连接
            RetrySent,     ///< 没带令牌：已回一条 Retry，本条数据报到此为止
            TokenRejected, ///< 带了令牌但验不过：已计数并丢弃
        };

        /**
         * @brief 确保静态目录配置本体与 "*" 兜底路由已建立（幂等）
         * @details 与两条 TCP 服务器同一个形状：建立动作在 StaticFileService::install() 里，
         *          这里只递本服务端的路由器与映射缓存上限。路由器还没接上时当场抛出——
         *          本类的路由器是外部交来的（不持有），顺序说不清就不能装作配好了。
         */
        void ensureStaticFileSettings();

        /**
         * @brief 取全部在线连接里最早的交易截止时刻（PTO、握手与空闲超时都记在连接自己身上）
         * @details 定时驱动据此决定下一次什么时候醒。空表返回 `time_point::max()`，调用方据此判「没有可查的截止」。
         * @return 最早的截止时刻；没有在线连接时为 `time_point::max()`
         */
        [[nodiscard]] std::chrono::steady_clock::time_point earliestConnectionExpiry() const;

        /**
         * @brief 按目的连接标识把报文交给对应的连接；不认识的长头 Initial 则开一条新连接
         * @param peerAddress 来源地址，按值收（理由同 `QuicConnection::handleDatagram()` 的说明）
         * @param datagram 报文净字节，指向收包缓冲；本方法会带着它一路 co_await 到连接侧，
         *                 调用方必须让它活到本协程完成
         * @note 本方法是协程：连接侧的 handleDatagram 是惰性协程，必须被真正 co_await。早先写成
         *       static_cast<void>(handleDatagram(...)) 等于构造完就把 Task 丢掉、协程从不恢复——
         *       读报文与写出都没发生，服务端一条报文也发不出去（实测：客户端收不到任何回包）。
         *       分成「排进就绪队列 + 由对象持有」那条路也能走通，但收报文本来就是串行的，
         *       直接 co_await 更简单，也少一份任务表的记账
         */
        /**
         * @brief 按目的连接标识把一个数据报派给对应的连接，没有就按新连接处理
         * @param peerAddress 来源地址
         * @param datagram 数据报字节
         * @param ecnCodepoint 这条数据报的 IP ECN 字段；空表示本端读不到（见核心的同名参数）
         * @return Core::Task<> 派发结束时完成；本方法不产出值
         */
        [[nodiscard]] Core::Task<> routeDatagram(const Platform::SocketAddress peerAddress, std::span<const std::uint8_t> datagram,
                                                 std::optional<std::uint8_t> ecnCodepoint = std::nullopt);

        /**
         * @brief 收下这条「目的标识本端还不认识」的数据报：先验来源地址，再占单来源名额，再建连接入表
         * @details 从 `routeDatagram` 里分出来，是因为这一串各有不往下走的出口：地址验证没过（回过 Retry
         *          或丢弃）、单来源名额已满、以及 `QuicConnection::accept` 判这条报文不可接受——三段合在
         *          路由里读起来就是四五种「什么都没发生」。分出来还保住一条顺序：先验地址再占名额，
         *          因为名额一旦拿走就得连着连接一起还，而一条 Retry 不建任何状态
         * @param peerAddress 来源地址
         * @param destinationConnectionId 报文里的目的标识（字符串视图，指向上面的数据报字节）：按别名登记时用
         * @param header 解好的包头：令牌、目的标识与源标识都从它取，按值收是因为惰性协程不留引用
         * @param datagram 数据报字节，交给新建的连接解
         * @param ecnCodepoint 这条数据报的 IP ECN 字段；空表示本端读不到
         * @return Core::Task<> 建连接并把这一报文派发结束时完成；本方法不产出值
         */
        [[nodiscard]] Core::Task<> acceptNewConnection(const Platform::SocketAddress peerAddress, std::string_view destinationConnectionId, QuicPacketHeader header,
                                                       std::span<const std::uint8_t> datagram, std::optional<std::uint8_t> ecnCodepoint);

        /**
         * @brief 对「版本本端不支持」的长头报文回一条版本协商包（RFC 9000 §6.1、§17.2.1）
         * @details 只在报文头解不开时调它：解得开的报文要么是本端支持的版本（走正常路由），
         *          要么是违反 v1 硬规则的坏包（那些规则与版本无关，回版本协商反而误导对端）。
         *          本端不为这条流量留任何状态，因此一个数据报最多回一条（§17.2.1 的 MUST NOT）。
         * @param peerAddress 来源地址，回包就打给它
         * @param datagram 收到的数据报字节
         */
        Core::Task<> respondToUnsupportedVersion(const Platform::SocketAddress peerAddress, std::span<const std::uint8_t> datagram);

        /**
         * @brief 新建连接之前先按 RFC 9000 §8.1.2 验一次来源地址
         *
         * @details 三档出口：报文带着本端签的令牌且验过＝放行建连接（并把令牌里恢复的原始目的标识交给外壳）；
         *          没带令牌＝回一条 Retry 并把这条数据报用掉；带了但验不过（被改过、搬来别处、或已过期）＝丢弃并计数。
         *          验不过时不再回第二条 Retry：§8.1.2 明写「对一份来自 Retry 的令牌，服务端只能拒或放行」，
         *          而对端此时已认定自己验过地址了，再回一条只会让它以为还在第一轮。
         *          按 §8.1.2 的 SHOULD 本应回一条 INVALID_TOKEN 收口，那要先为这条连接算出 Initial 密钥——
         *          本端刻意不在建连接之前算任何东西（那正是 Retry 要省掉的成本），因此走同节允许的另一条：
         *          丢弃、让对端自己的握手超时。这一格必须有读数，否则它与「静默丢包」看不出区别。
         *          回出去的 Retry 按 §17.2.5.1 把线上目的标识填成这条 Initial 的源标识：对端按那一格认领
         *          回包，填错（含一律填空）就把对端自报过标识的那一类客户端全挡在门外。
         * @param peerAddress 来源地址：令牌把它绑进认证码
         * @param header 这条 Initial 的包头：令牌字段、目的标识与源标识都从它取
         * @param retryContext 出参：放行时填好交给 `QuicConnection::accept` 的凭据
         * @return Core::Task<QuicAddressValidationDecision> 三档出口之一；本方法要 co_await 一次发包
         */
        [[nodiscard]] Core::Task<QuicAddressValidationDecision> validateNewConnectionAddress(const Platform::SocketAddress &peerAddress, const QuicPacketHeader &header,
                                                                                             QuicConnection::RetryContext &retryContext);

        /**
         * @brief 把一条 Retry 报文打回来源地址
         * @param peerAddress 来源地址
         * @param retryPacket 已组好的 Retry 报文
         * @return bool 整条是否交给了内核；只有交出去才计一笔
         */
        [[nodiscard]] Core::Task<bool> sendRetryPacket(const Platform::SocketAddress peerAddress, std::vector<std::uint8_t> retryPacket);

        /**
         * @brief 构造期把两把密钥定死：地址验证令牌的签发器，以及无状态重置的静态密钥
         * @details 排在建 TLS 上下文与登记 /metrics 把手之前：两处都会抛（配置错、随机源取不到字节），
         *          而构造期抛出后析构不跑——排在后面就会留下没人认领的 SSL_CTX 或度量把手
         * @throws Base::InvalidArgumentException `statelessResetKey` 短于 16 字节
         * @throws Base::SystemException 需要随机自造密钥而系统随机源取不到字节
         */
        void resolveProtocolKeys();

        /// 把「签发即登记别名索引、撤回即摘除」两条回调接进连接配置（§5.1.1：签了就得认）
        void wireAliasConnectionIdCallbacks(QuicConnection::Configuration &configuration);

        /**
         * @brief 对一条「目的标识不属于任何在册连接」的短头报文回一条无状态重置（RFC 9000 §10.3）
         *
         * @details 这是本端丢掉连接状态之后的最后一招：不建任何状态，凭「报文里的目的标识 + 静态
         *          密钥」现算令牌（§10.3.2），对端收到即可立刻收场，不必等自己的空闲超时。三条尺寸
         *          判据一次走完：比触发报文小（§10.3.3 的 MUST：循环里尺寸一路衰减，本端因此
         *          不必维护防循环的状态）、不小于 21 字节（图 10：38 位不可预测位 + 16 字节令牌）、
         *          也不超过本端数据报基准尺寸（不替对端放大字节）
         * @param peerAddress 来源地址，回包就打给它
         * @param datagram 触发的数据报（拿它的长度定重置尺寸）
         * @param destinationConnectionId 触发报文里的目的标识（令牌就按它派生）
         */
        [[nodiscard]] Core::Task<> sendStatelessReset(Platform::SocketAddress peerAddress, std::span<const std::uint8_t> datagram,
                                                      std::span<const std::uint8_t> destinationConnectionId);

        /**
         * @brief 把一个数据报交给一条已在册的连接，随后把它的 HTTP/3 会话推一步
         * @details 三处调用点（按本端标识命中、按别名命中、刚刚新建的那一条）要做的是同一件事，
         *          写成一处而不是三份：那条 `ActivityGuard` 少了任何一份都会让定时循环在 await 期间
         *          把连接摘掉，而别名表存的是裸指针（见 `routeDatagram` 里那一格）
         * @param connection 要收这个数据报的连接
         * @param peerAddress 来源地址
         * @param datagram 数据报字节
         * @param ecnCodepoint 这条数据报的 IP ECN 字段；空表示本端读不到
         * @return Core::Task<> 报文交付与会话推进结束时完成；本方法不产出值
         */
        [[nodiscard]] Core::Task<> serveMatchedConnection(QuicConnection &connection, const Platform::SocketAddress peerAddress, std::span<const std::uint8_t> datagram,
                                                          std::optional<std::uint8_t> ecnCodepoint);

        /**
         * @brief 给一条正要新建的连接摆好外壳要的那些出口与回调
         * @details 单独成一条是因为这张配置里的每个回调都要抓住 `this`（流数据、流收口、发送空间
         *          可用、单条与批次两条发送出口），写在收报文的协程里会让那个函数长到读不出「路由」
         *          这条主线，而它自己一次 await 都没有。TLS 上下文在这里取一次快照，连接建好后
         *          由它自己持有 OpenSSL 的引用（与 `reloadCertificate()` 的定序同在这一处）
         * @return QuicConnection::Configuration 交给 `QuicConnection::accept` 的那份配置
         */
        [[nodiscard]] QuicConnection::Configuration configurationForAcceptedConnection();

        /**
         * @brief 把已收口的连接摘出路由表
         */
        void reapClosedConnections();

        /**
         * @brief 带原因收口所有还开着的连接，并立刻清理路由表
         * @note 与 drain() 的三条出口配合：无论等到什么程度，返回后本服务器不再持有连接。
         *       收口报文（CONNECTION_CLOSE）先刷出去再摘连接，否则对端只能等自己的空闲超时
         */
        [[nodiscard]] Core::Task<> closeAllOpenConnections();

        /**
         * @brief 取（必要时创建）某条连接上的 HTTP/3 会话
         * @param connection 目标连接
         * @return Http3Session& 该连接的会话
         * @note 懒建：h3 的流量必然在握手之后，第一条流数据到达时建正好；提前建反而要额外记握手状态
         */
        Http3Session &http3SessionFor(QuicConnection &connection);

        /**
         * @brief 找出某条连接上的 HTTP/3 会话
         * @param connection 目标连接
         * @return Http3Session* 会话；该连接还没有会话时为空
         */
        [[nodiscard]] Http3Session *findHttp3Session(const QuicConnection *connection) noexcept;

        /**
         * @brief 驱动某条连接上的 HTTP/3 会话：派发排队的请求并把响应写出去
         * @param connection 目标连接
         * @return Core::Task<> 驱动完成（该连接没有会话时立即返回）
         */
        [[nodiscard]] Core::Task<> pumpHttp3For(QuicConnection &connection);

        /**
         * @brief 按配置建一份加固过、装上证书与服务参数的 SSL_CTX（构造与热轮换共用同一份判据）
         * @param configuration 服务端配置：策略、证书与私钥路径、mTLS 开关、票据密钥路径都从这份取
         * @return std::expected<std::shared_ptr<SSL_CTX>, std::string> 建好的上下文；失败值是中文原因
         * @details 只留一份实现是刻意的：这里的每一项在缺掉时都不报错，而是「这条通道悄悄少了保护」——
         *          少了票据密钥会让跨进程会话恢复率归零，少了 TLS 1.3 下限与 ALPN 回调会让 h3 握手
         *          整片失败，而那些都只在运行期才看得见。
         */
        [[nodiscard]] static std::expected<std::shared_ptr<SSL_CTX>, std::string> buildTlsContext(const Configuration &configuration);

        Core::EventLoop &m_eventLoop;     ///< 所属事件循环
        Configuration    m_configuration; ///< 服务端配置
        /// 连接级限额的生效份：构造时取 `Configuration::serverLimits`（没给就用默认档那份），
        /// `setLimits()` 整体换掉它。会话只读这一份而不是 `m_configuration` 里那份——两处都读就会
        /// 出现「setter 改了其中一处」的分叉，静态目录的映射条数上限与在途预算都从这一份取
        std::shared_ptr<const HttpServerLimits> m_serverLimits;   ///< 交给此后每条连接上新建会话的那份限额
        HttpParserLimits                        m_parserLimits{}; ///< 同上，解析上限的生效份（按值：会话构造时取走副本，之后没有读者）
        /// QUIC 用的 SSL_CTX（含证书与 ALPN）。按 shared_ptr 持有：reloadCertificate() 换掉的就是这一份，
        /// 而在途连接的 SSL 各自持有 OpenSSL 内部的引用，旧上下文活到它们收尾才释放
        std::shared_ptr<SSL_CTX> m_tlsContext;
        /// 上面那份的读写锁。读点是「新连接首包要 SSL_new」那一下，写点是 reloadCertificate()；
        /// 已经建好的连接不再读它，因此这把锁不参与任何在途数据的收发路径
        std::mutex               m_tlsContextMutex;
        Platform::DatagramSocket m_datagramSocket; ///< 绑定的 UDP 套接字
        /// 接手来的平台套接字：有值即「接手模式」，无参的 listen() 认它，带地址的 listen() 拒绝。
        /// 被 listen() 转交给 m_datagramSocket 之后本项转为空
        std::optional<Platform::DatagramSocket> m_adoptedSocket; ///< 尚未装进本端的那份已绑好的套接字
        std::unique_ptr<Core::AsyncUdpSocket>   m_socket;        ///< 套接字的事件循环封装
        Core::Timer                             m_expiryTicker;  ///< 定时驱动的节拍定时器
        /// 实际绑定的端口：绑定成功才写入，故非 0 即「已在监听」。原子量是为了让外部线程能读这个
        /// 启动凭据（写侧在循环线程、读侧只观察它），不是允许跨线程碰本类的其他成员
        std::atomic<std::uint16_t> m_listeningPort{0};
        std::atomic<bool>          m_isStopped{false}; ///< 是否已请求停止：可从别的线程置位，因此必须是原子
        /// 排空期间只挡新连接（收报文与在途请求照常跑）：与 m_isStopped 分开，
        /// 因为后者会让收报文的循环退出，在途请求就永远做不完
        std::atomic<bool>                 m_isRefusingNewConnections{false};
        Platform::SocketAddress           m_localSocketAddress; ///< 本端地址（建连接时要写进回包与日志）
        QuicConnection::StreamDataHandler m_streamDataHandler;  ///< 流数据回调（缺省为空，即收到流数据不回应）

        Router           *m_router{nullptr}; ///< 路由器（不持有；接上之后每条连接才会有 HTTP/3 会话）
        StaticFileService m_staticFiles;     ///< 静态目录配置本体；登记的兜底路由落在 m_router 上

        /// 每条连接上的 HTTP/3 会话：键是连接，会话的开流/写出/归还额度的口子都指向那条连接。
        /// 会话必须在连接被摘除时一起销毁（它内部存的是指向该连接的引用）
        std::map<const QuicConnection *, std::unique_ptr<Http3Session>> m_http3Sessions;

        /// 连接标识 → 连接。键是本端生成的 SCID（对端的 DCID）
        std::map<std::string, std::unique_ptr<QuicConnection>, std::less<>> m_connections;

        /// 上面那张表的在册条数的跨线程镜像：只在表增删的那两处随表刷新（各一条 relaxed 加减），
        /// 供 connectionCount() 在循环外的线程上读——表本体只归循环线程，外部线程读它即为数据竞争。
        /// 每条连接建立与摘除才各动一次，不值得为它做缓存行填充
        std::atomic<std::size_t> m_connectionCountMirror{0};            ///< 在册连接数（与连接表同步维护）
        std::atomic<std::size_t> m_handshakingConnectionCountMirror{0}; ///< 其中未完成握手的条数：按定时器节拍重算（完成点埋在状态机内部，散布加减迟早漏一处）

        /// 撞本监听器并发上限而被拒的新连接累计数：与 h1/h2 侧同一口径进 /metrics
        /// （`over_limit_rejected_connections_total`），原子量因为 stats() 允许从别的线程读
        std::atomic<std::uint64_t> m_overLimitRejectedConnections{0};
        /// 收包侧「一次批次交付」的两笔账：批次数与其中交付的报文条数。两笔要一起看才是
        /// 「一次就绪收了几条」，只报条数看不出批次，只报批次数看不出负载（读口与两侧差异见
        /// `datagramBatchCount()` 的 @details）；同一份读数经 ProcessMetricsRegistry 进 /metrics
        std::atomic<std::uint64_t> m_datagramBatchCount{0}; ///< 批次收包的调用数
        std::atomic<std::uint64_t> m_datagramCount{0};      ///< 其中交付的报文条数
        /// 发包侧的同形两笔账：一轮 flush 攒出的报文按窗交出，这里的批次数是窗口调用数
        std::atomic<std::uint64_t> m_datagramBatchSendCount{0}; ///< 批次发包的调用数
        std::atomic<std::uint64_t> m_datagramSentCount{0};      ///< 其中交给内核的报文条数
        /// 交出去的版本协商包条数：公网端口上「版本不合就礼貌地拒」这件事必须可数，否则它与「静默丢了」
        /// 长得一模一样（G-17 的判据就是这个数在涨而连接表不动）
        std::atomic<std::uint64_t> m_versionNegotiationSentCount{0};
        /// 交出去的 Retry 条数：地址验证是否真在生效就看这一格，它与「名额已满」也是两本账
        std::atomic<std::uint64_t> m_retrySentCount{0};
        /// 验不过的令牌条数：涨了说明端口上有人在搬令牌或自造令牌，也可能是密钥没共享的那几份进程
        std::atomic<std::uint64_t> m_retryTokenRejectedCount{0};
        /// 交出去的无状态重置条数：涨了说明端口上有人在敲已经不在的连接（对端进程重启过、或连接已收口）
        std::atomic<std::uint64_t> m_statelessResetSentCount{0};
        /// 地址验证令牌的签发器；空＝这一项保护没开（`addressValidationTokenLifetime` 为 0）
        std::unique_ptr<QuicAddressValidationTokenCodec> m_addressValidation{};
        /// 无状态重置的静态密钥：构造期定死（留空则随机自造），既喂给每条连接，也用于上面那条回包路径
        std::vector<std::uint8_t> m_statelessResetKey{};
        /// 上面几笔的 /metrics 把手：构造时登记、析构即注销，与 Net 其它非 HTTP 通道同形
        std::array<Core::ProcessMetricHandle, 9> m_metricHandles{};
        /// 本端能不能读到收到数据报的 ECN 字段：读不到就不在 ACK 里报计数（RFC 9000 §13.4.1），
        /// 也不能指望着对端报回来——这一格由 listen 时的一次 enableEcnFieldVisibility 定下来
        bool m_isEcnFieldVisible{false};
        /// 套接字有没有设上「发出的数据报不要在 IP 层分片」：设上了各条连接才允许发路径 MTU 探针
        /// （RFC 9000 §14 的 MUST，理由见 `QuicConnection::Configuration::pathMtuProbeAllowed`）
        bool m_isDoNotFragmentSet{false};
        /// 上限告警是否已经报过（只由循环线程读写）：满载时每条 Initial 都报一条会把日志刷满，
        /// 一条都不报又看不见满载，因此按「空出名额 → 再次撞满」的跳变各报一条
        bool m_overLimitAlerted{false}; ///< 仅由所属循环线程读写

        /// 别名索引：除本端 SCID 之外可以寻址到本连接的目的连接标识 → 连接
        /// （存裸指针，所有权仍在上面那张表里）。目前只有一类来源：客户端最初选的 DCID——
        /// 客户端重传 Initial 时报文里的 DCID 仍是它最初选的那个（RFC 9000 §7.2 首包连接标识
        /// 固定到服务端回话为止），按本端 SCID 建的键对不上，少了这一路会把每条重传都当成新连接
        /// （实测：一个客户端握手却建出 8 条连接）。本端不签发额外连接标识，因此别名只有这一条；
        /// 将来做连接标识轮换时要在这里补登记
        std::map<std::string, QuicConnection *, std::less<>> m_connectionsByAliasConnectionId;

        /// 连接 → 它占住的单来源名额凭据：随连接一起摘除即归还名额（键与上面那张表同源）
        std::map<std::string, PerIpConnectionLimiter::Lease> m_perIpConnectionLeases;
    };
} // namespace AsynGyanis::Net
