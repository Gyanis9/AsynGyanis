/**
 * @file QuicConnectionCore.h
 * @brief QUIC 连接状态机（RFC 9000 §2–§12）：一个数据报进、若干数据报出，不碰 socket 也不碰事件循环
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 把 Codec（报文头/帧/变长整数/传输参数）与 Crypto（密钥、包保护、TLS 胶水）串成一条链：
 *          解报头 → 去头部保护 → 还原包号 → AEAD 解密 → 解帧 → 按级别喂 TLS → 取 TLS 产出编成 CRYPTO
 *          帧 → 组包发出。时间戳一律由调用方注入，因此整条链可以在单测里逐字节复现，不需要真实网络。
 *
 * @note 能力范围：两个角色各自的握手、1-RTT 的流与流量控制、1-RTT 密钥更新，以及恢复层的发包记账、
 *       RTT、判丢、探测超时、按偏移重发、拥塞窗口许可、Initial 空间的退休、ECN 验证与 CE 反应、
 *       路径 MTU 探测（DPLPMTUD）、连接标识的签发/通告/撤回（§5.1）、路径验证与迁移应答
 *       （§8.2、§9.3–§9.4：回应挑战、跟对端换地址、逐路径反放大、验证失败退回），以及无状态重置
 *       令牌的签发与识别（§10.3–§10.3.2：收下对端给的令牌、对上即进 draining；发送那一半在服务端
 *       外壳上，见 `QuicServer::Configuration::statelessResetKey`）。
 *       刻意不做：0-RTT、NEW_TOKEN、版本协商、本端主动发起迁移（本端地址不换，§9.2 那一半）与
 *       §9.6 的首选地址；作客户端时不能接住服务端下发的 Retry（挑一个 8 字节以上的随机目的标识
 *       是当前唯一的缓解，真要互操作到开了 Retry 的服务端，得先补这一路）。
 * @warning 不是线程安全的：一个实例属于一条连接，只能在所属事件循环线程上驱动。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"
#include "Net/Quic/Crypto/QuicTlsContext.h"
#include "Net/Quic/QuicConnectionRole.h"
#include "Net/Quic/QuicOutboundDatagram.h"
#include "Net/Quic/QuicPathMtuDiscovery.h"
#include "Net/Quic/QuicReassemblyBuffer.h"
#include "Net/Quic/QuicReceivedPacketNumbers.h"
#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"
#include "Net/Quic/Recovery/QuicRecovery.h"
#include "Net/Quic/Streams/QuicStreamLayer.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// 无状态重置的静态密钥下限（RFC 9000 §8.1.4 的「难猜」口径）：短于它就不算令牌——全零令牌会把
    /// 「末尾 16 个零字节的报文」变成一条人人可发的掐线指令
    inline constexpr std::size_t kQuicMinimumStatelessResetKeyByteLength = 16;

    /**
     * @brief 按 RFC 9000 §10.3.2 算一条连接标识的无状态重置令牌：HMAC-SHA256(静态密钥, 标识) 截前 16 字节
     * @details 一个进程丢掉了连接状态之后，要靠它从「收到的报文里那条目的标识」现算出一个对端认得的令牌
     *          （§10.3.2：「An endpoint that loses state can use the same method to generate a valid
     *          stateless reset token」），因此这个推导同时属于签发的连接与不认识它的那台服务端——
     *          两处各写一份的话，算错的那一侧发出去的重置没人认，而症状是「客户端干等到自己的超时」
     * @param staticKey 端点静态密钥；低于 16 字节时拒（§8.1.4 的「难猜」口径，两处调用同一条判据）
     * @param connectionId 那条标识本身（可能来自对端报文的明文头部）
     * @return std::array<std::uint8_t, kQuicStatelessResetTokenByteLength> 16 字节令牌；密钥过短或
     *         HMAC 上下文建不起来时给全零——调用方按「令牌算不出来就不发重置」处置
     */
    [[nodiscard]] ASYN_NET_API std::array<std::uint8_t, kQuicStatelessResetTokenByteLength> deriveQuicStatelessResetToken(std::span<const std::uint8_t> staticKey,
                                                                                                                          std::span<const std::uint8_t> connectionId) noexcept;

    /// 状态机的阶段。收口后不另设「drained」阶段：`isFinished()` 就是「已收口且待发队列空」
    enum class QuicConnectionPhase
    {
        Handshaking, ///< 还没拿到对端的完整握手消息
        Established, ///< TLS 报完成且本端 1-RTT 密钥就位
        Closing,     ///< 已经发出或收到 CONNECTION_CLOSE
    };

    /**
     * @brief 建连接状态机需要的全部外部值
     *
     * @details 三条连接标识各自独立：本端签发的、回包要打的、以及客户端第一个 Initial 里那个目的标识
     *          （它决定 Initial 密钥，服务端必须经传输参数把它回传给对端，RFC 9000 §7.3）。
     *          作客户端时「客户端第一个 Initial 的目的标识」就是本端自己挑的那个值，且一开始也是
     *          `peerConnectionId`；收到服务端自报的源标识后，本类会按 §7.2 把 `peerConnectionId` 换过去。
     * @note 各 vector 都拷进本对象，构造完成后调用方即可销毁源容器。
     */
    struct ASYN_NET_API QuicConnectionCoreConfiguration
    {
        QuicConnectionRole        role{QuicConnectionRole::Server};  ///< 本端角色；缺省值与既有服务端调用点一致
        SSL_CTX                  *tlsContext{nullptr};               ///< 已配好证书与 ALPN 的 TLS 上下文，生命周期须覆盖本对象
        std::vector<std::uint8_t> localConnectionId{};               ///< 本端签发的源连接标识：短头按它定长度，路由表也认它
        std::vector<std::uint8_t> peerConnectionId{};                ///< 回包的目的连接标识：对端自报的源标识（§7.2）
        std::vector<std::uint8_t> originalDestinationConnectionId{}; ///< 客户端第一个 Initial 的目的标识，参数里的 ODCID 由它写
        /**
         * @brief 本端刚用这条标识回过 Retry 时填它：Initial 的包密钥改由它推导，参数里也要带上它
         *
         * @details RFC 9000 §17.2.5.2：客户端把 Retry 的源标识抄进后续 Initial 的目的标识，
         *          「改了目的标识也就换了保护这一包的密钥」；§18.2 另要求发过 Retry 的服务端把这一条写进
         *          `retry_source_connection_id`，客户端拿它核对「这台服务端确实看过我的第一个 Initial」。
         *          留空＝这条连接没走过 Retry，密钥仍按上面那个原始目的标识算，参数里也不出现这一项。
         */
        std::vector<std::uint8_t>            retrySourceConnectionId{};
        QuicTransportParameters              transportParameters{}; ///< 本端要声明的传输参数；两个必填的连接标识项由本类按上面三个值补齐
        std::optional<QuicClientTlsSettings> clientTlsSettings{};   ///< 作客户端时按连接生效的身份（SNI、校验名、ALPN）；服务端一侧留空
        /**
         * @brief 本端的平台能不能按单条报文读写 IP 头的 ECN 字段
         * @details 由外壳按 `Platform::DatagramSocket::supportsPerDatagramEcnField()` 填（Windows 为假）。
         *          为假时本端 ECN 直接落在 `QuicEcnState::Failed`：出包不带标、发包凭据里也不记带标——
         *          本端记什么就要线上是什么，否则 §13.4.2.1 的计数校验会拿本端这份错账把一条好路
         *          判成验证失败。缺省给真是因为有单测直接构造本结构模拟一台支持的平台机器；
         *          外壳一律显式填，不留给缺省值去猜平台
         */
        bool supportsPerDatagramEcnField{true};
        /**
         * @brief 本端的套接字有没有设上「发出的数据报不要在 IP 层分片」
         * @details 由外壳按 `Core::AsyncUdpSocket::enableDoNotFragment()` 的结果填。这是 RFC 9000 §14 的
         *          MUST（IPv4 要设 DF 位），也是路径 MTU 探测成立的前提：没设上时超尺寸的报文会被内核
         *          悄悄切开，探针被确认就再也不能说明「这个尺寸整条走得通」。设不上就不探，尺寸停在
         *          BASE——那是 §14.1 本来就允许的最小值，功能不失效，结论也不谎报
         */
        bool pathMtuProbeAllowed{true};
        /**
         * @brief 这条连接用哪一家拥塞控制算法（RFC 9002 §7 的窗口许可，算法可插拔）
         * @details 缺省 NewReno：与这一格落地之前的全部部署逐字一致（换默认值等于把所有既有
         *          连接的线上行为重标一次，不夹带做）。要 CUBIC（RFC 9438：乘性减 0.7、
         *          三次曲线增长、Reno 友好区与快收敛）在此显式选 `QuicCongestionAlgorithmKind::Cubic`；
         *          外壳按各自配置逐层传下来，想按连接试算法的探针也直接造这份配置
         * @see makeQuicCongestionAlgorithm
         */
        QuicCongestionAlgorithmKind congestionAlgorithm{QuicCongestionAlgorithmKind::NewReno};
        /**
         * @brief 本端签发一条新连接标识时的通知：外壳拿它把这条标识登记进路由表（RFC 9000 §5.1.1）
         * @details §5.1.1 明写「签发的标识要么认到连接结束、要么认到对端用 RETIRE_CONNECTION_ID
         *          撤回为止」：标识一旦上线就可能出现在下一包的短头里，路由表必须在那一包到达之前就认它。
         *          本类在造出这条标识的那一刻通知，广告它的 NEW_CONNECTION_ID 要等到下一次 1-RTT
         *          出包，因此登记永远早于上线。空回调＝不登记（单测与不需要按标识路由的用法）
         */
        std::function<void(std::span<const std::uint8_t> connectionId)> onConnectionIdIssued{};
        /**
         * @brief 对端 RETIRE_CONNECTION_ID 撤回一条标识之后的通知：外壳据此把它从路由表摘掉
         * @details 撤回之后本端不再接受落在它上面的报文（§5.1.1 的另一半）。与上一条成对：
         *          登记与摘除各一处，外壳不必去猜本端此刻有哪些标识在册
         */
        std::function<void(std::span<const std::uint8_t> connectionId)> onConnectionIdRetired{};
        /**
         * @brief 对端新地址验证通过的通知（RFC 9000 §8.2.3）：外壳据此记日志
         * @details 验证通过的判据写死在规范里：收到一条 PATH_RESPONSE，其数据与本端发过的某条
         *          PATH_CHALLENGE 逐字节相同——确认不算验证（§8.2.3 末段），包被确认不算，
         *          丢包判重也不算。核心在通知之前已经把拥塞窗口与 RTT 估算按 §9.4 重置过
         */
        std::function<void(const QuicPathKey &)> onPeerPathValidated{};
        /**
         * @brief 对端新地址验证失败的通知（RFC 9000 §8.2.4 / §9.3.2）：外壳据此记日志
         * @details 判据是 §8.2.4 那把计时器：3×max(当前 PTO, 按 kInitialRtt 算的 PTO) 到期
         *          还没收到对应的 PATH_RESPONSE 就放弃。核心已按 §9.3.2 退回上一个已验证地址
         *          （一个都没有时会静默收口），通知里给的正是放弃的那条路径
         */
        std::function<void(const QuicPathKey &)> onPeerPathAbandoned{};
        /**
         * @brief 收到一条对上本端令牌的无状态重置（RFC 9000 §10.3.1）：外壳据此记日志
         * @details 对上令牌即进 draining——不发 CLOSE、也不再往这条连接上发任何东西（对端显然已经不
         *          认识它了）。核心打完这条通知之后就是「已收口且待发队列空」，外层看 `isFinished()`
         *          即可回收。无参数：这条连接的对端是唯一的，地址与标识都归外壳记账
         */
        std::function<void()> onStatelessResetDetected{};
        /**
         * @brief 无状态重置令牌的派生密钥（§10.3.2）；留空＝本连接随机取一把
         * @details 令牌按 RFC 9000 §10.3.2 由「端点静态密钥 + 标识」派生，签发的每条标识在
         *          NEW_CONNECTION_ID 里都要带一个、服务端还给握手那条发一份传输参数（0x02）。
         *          留空只够单连接自己用：服务端要把它喂成全实例一把（`QuicServer` 同名那一格），
         *          否则连接没了之后谁也替它算出对端认得的那份令牌。多进程 worker 同样必须共享
         *          同一把：无状态重置的用处正是「连接没了还能答话」，而那一刻报文可能落到另一个
         *          进程手里（与 `addressValidationKey` 同一条道理）。
         */
        std::vector<std::uint8_t> statelessResetKey{};
    };

    /**
     * @brief 一条连接的传输状态机（两个角色共用，判据见 `QuicConnectionRole`）
     *
     * @details 输入只有 `onDatagramReceived`，输出只有 `takeOutboundDatagram`：本类不持有 socket，也不
     *          自己决定何时发包，由外层（服务端一侧的 `QuicConnection` 外壳、客户端一侧的
     *          `QuicClientConnection`，或单测）喂完包调 `drive`、再把待发队列抽干。
     *
     * @note 包号分三个空间各算各的（Initial / Handshake / Application）：一个数据报里混放不同级别的
     *       报文是合法写法（§12.2 合包），跨空间的包号互相看不见。
     */
    class ASYN_NET_API QuicConnectionCore
    {
    public:
        using Timestamp = std::chrono::microseconds; ///< 注入用的时间戳，单位与 ACK 帧的延迟字段一致

        /**
         * @brief 建状态机并挂上 TLS 上下文
         * @details Initial 的收发密钥在此按 `originalDestinationConnectionId` 推导（RFC 9001 §5.2）：
         *          服务端在收到任何字节之前就知道该用什么密钥解，这正是 Initial 不需要协商的原因。
         * @param configuration 建连接所需的全部外部值
         * @throws Base::InvalidArgumentException 用法错误：TLS 上下文为空、连接标识长度超过 20 字节
         * @throws Base::Exception 运行期故障：TLS 会话建立失败
         */
        explicit QuicConnectionCore(QuicConnectionCoreConfiguration configuration);

        QuicConnectionCore(const QuicConnectionCore &)            = delete;
        QuicConnectionCore &operator=(const QuicConnectionCore &) = delete;
        QuicConnectionCore(QuicConnectionCore &&)                 = delete;
        QuicConnectionCore &operator=(QuicConnectionCore &&)      = delete;

    public:
        /// 待发数据报的类型在 `QuicOutboundDatagram.h`（两侧的公共面都要用它，见那里的 @note）
        using QuicOutboundDatagram = ::AsynGyanis::Net::QuicOutboundDatagram;

        /**
         * @brief 处理一个收到的 UDP 数据报净载荷：只收不发
         * @details 先逐个拆出里面的报文（合包合法），逐条认证；解不开的按 RFC 9001 §4.1.4 静默丢弃，
         *          不算对端违规。产出由调用方随后调 `drive` 拿：与外壳「收到数据报 → flush」的分工同构，
         *          也让 ACK 的延迟字段能反映真实的发送时刻。
         * @param datagram 数据报字节
         * @param arrivalTime 本数据报的到达时刻，用于 ACK 帧的延迟字段
         * @return 成功返回 void，包含「整包被丢弃」这种正常路径
         * @return 失败返回 `QuicDecodeError`：报文违反 v1 的硬性规则（保留位非 0 等）。本类会同时自行
         *         发出 CONNECTION_CLOSE（§10.2 要求连接错误必须通知对端），错误值供调用方记日志
         */
        /**
         * @brief 处理一个收到的 UDP 数据报净载荷
         * @param datagram 数据报字节
         * @param arrivalTime 本数据报的到达时刻，用于 ACK 帧的延迟字段
         * @param ecnCodepoint 这条数据报 IP 头里的 ECN 字段；交空表示本端读不到这个字段（Windows，
         *        或套接字没开那个选项）。读不到与读到 0 是两件事：前者不许在 ACK 里报 ECN 计数
         *        （RFC 9000 §13.4.1 明确允许读不到的端点不报），后者要按 §19.3.2 计入 ECT(0) 那一格
         * @param pathKey 这条数据报打哪条对端路径来（外壳按来源地址折算，RFC 9000 §9.3 的路径就是
         *        本端地址与对端地址的二元组）。§9.3 的「只跟编号最高的非探测包换地址」、§8.2.2 的
         *        「回应必须打在挑战来的那条路径上」以及 §9.3.1 的逐路径反放大上限都按它记账；
         *        单路径的用法（出站客户端）留空即可
         * @return 与不带这一格时同义：失败返回 `QuicDecodeError`，本类自行发出 CONNECTION_CLOSE
         */
        [[nodiscard]] QuicDecodeStatus onDatagramReceived(std::span<const std::uint8_t> datagram, Timestamp arrivalTime, std::optional<std::uint8_t> ecnCodepoint = std::nullopt,
                                                          const QuicPathKey &pathKey = QuicPathKey{});

        /**
         * @brief 取走一条待发数据报
         * @return 有数据报时返回它（取走即出队，缓冲随所有权一起交出去），队列空时返回空
         */
        [[nodiscard]] std::optional<QuicOutboundDatagram> takeOutboundDatagram();

        /**
         * @brief 推进一步握手，并把产出编成待发数据报
         * @details 每次 `onDatagramReceived` 之后都要调它，否则收到的字节既不会交给 TLS 也不会产出回包
         *          ——与外壳「收到数据报 → flush」的分工同构。`now` 用来算 ACK 帧的延迟字段，
         *          所以按真实发送时刻传，别拿到达时刻顶替。
         * @param now 当前时刻
         */
        void drive(Timestamp now);

        /**
         * @brief 下一次该醒的时刻
         * @details 由恢复层给出：可能是按时间阈值判丢，也可能是探测超时。外层拿它定闹钟，到点调
         *          `onTimeout`；返回空表示本连接当前不需要定时器
         * @return 有待确认或待判丢的包时返回那个绝对时刻，否则为空
         */
        [[nodiscard]] std::optional<Timestamp> nextTimeout() const noexcept;

        /**
         * @brief 恢复层定时器到期：判丢并按区间重发握手字节，必要时探一条 PING
         * @param now 当前时刻
         */
        void onTimeout(Timestamp now);

        /**
         * @brief 应用还在做事时替这条连接保活：把「还早」的空闲截止提前用一条 PING 顶住
         * @details 为什么需要：服务端一个处理器的耗时上限是 `writeTimeout`（三条通道同一个口径，h1/h2 在
         *          处理器相位会显式把空闲截止刷成这一段，见 `HttpSession.h` 里 `refreshIdleDeadline` 那处），
         *          而 QUIC 的空闲超时是传输层的闸——它只认「有没有报文进来」。一个 40 秒的处理器既不读
         *          也不写，两端各自的 `max_idle_timeout`（默认 30 秒）先到期，连接被静默收口：同一份业务在
         *          h1/h2 上答得出来，换到 h3 上连响应带别的流一起丢。RFC 9000 §10.1.2 给出的合法解法就是
         *          空闲期间发一条 PING 让对端有东西可确认。
         * @details 为什么不会变成僵尸连接：本方法只欠一条探针，续期仍走 `emitPacket` 里那条既有判据
         *          ——「收到过报文之后的第一包主动发包才续一期」（§10.1 的同一处，见 `m_hasSentAckElicitingSinceReceipt`）。
         *          对端活着就会回 ACK，那一收把标记清掉、下一轮才还能续；对端没了就永远收不到报文，
         *          截止不再被顶开，本端按原定的空闲时限收口。调用方的谓词也必须自限：只对
         *          「还挂着产出时限的流」保活（见 `Http3Session::hasArmedProduceDeadlines`），
         *          时限一响、条目一摘，保活自然停。
         * @param now 当前时刻
         * @return true 这次确实欠下了一条探针（调用方随后要把待发队列刷出去）；false 什么都没做
         * @note 节律：一条 PING 管四分之一期，之后即使仍在「半期之内」也不再重发——承载层的驱动是
         *       每个节拍都调本方法的，不设节律就变成每拍一个包
         */
        bool requestKeepalive(Timestamp now);

        /**
         * @brief 本端主动收口：排一条 CONNECTION_CLOSE 并进入 Closing
         * @details 只在传输层错误码这一档（0x1c）；应用层错误码（0x1d）随 HTTP/3 那层一起接。
         * @param errorCode 连接错误码（RFC 9000 §11.1 / RFC 9001 §4.8）
         * @param reasonPhrase 原因文案，可含任意字节
         * @param now 当前时刻，收口报文也要按发送时刻记账
         */
        void requestClose(std::uint64_t errorCode, std::string_view reasonPhrase, Timestamp now);

        /**
         * @brief 当前阶段
         * @return QuicConnectionPhase 见枚举定义
         */
        [[nodiscard]] QuicConnectionPhase phase() const noexcept;

        /**
         * @brief 拥塞层当前记着的在途字节数
         * @return std::size_t 与 `QuicCongestionControl::bytesInFlight()` 同源的负荷值：
         *         某个包号空间退休后，它的字节应当从这里销账
         */
        [[nodiscard]] std::size_t bytesInFlightByteCount() const noexcept;

        /**
         * @brief 拥塞层的当前窗口字节数
         * @return std::size_t 与 `QuicCongestionControl::congestionWindowByteLength()` 同源的读数
         * @details 与 `bytesInFlightByteCount()` 成对：只有负荷看不出拥塞状态，判成持久拥塞时
         *          窗口该落到最小窗（§7.6.2），运维与用例都靠这一格分辨「窗口本来就小」与「刚被重启」
         */
        [[nodiscard]] std::size_t congestionWindowByteLength() const noexcept;

        /**
         * @brief 这条连接实际在用的拥塞控制算法（构造期按 `congestionAlgorithm` 选定，之后不再换）
         * @return const QuicCongestionAlgorithm& 引用有效期同本对象；观测与用例据它核对选了哪一家
         * @see QuicConnectionCoreConfiguration::congestionAlgorithm
         */
        [[nodiscard]] const QuicCongestionAlgorithm &congestionAlgorithm() const noexcept;

        /**
         * @brief 判成持久拥塞的次数（RFC 9002 §7.6）
         * @details 本层不打日志（纯计算件，日志归外壳），这条连接「曾经把路判死过」就得从读数上问得出来；
         *          外壳与用例都靠它判断窗口那次收缩是重启而不是减半
         */
        [[nodiscard]] std::size_t persistentCongestionEventCount() const noexcept;

        /**
         * @brief 本端对这条路径的 ECN 验证走到哪一格
         * @return QuicEcnState 见恢复层的枚举；外壳与 /metrics 靠它把「还在标」与「标过且验证过」分开报
         */
        [[nodiscard]] QuicEcnState ecnState() const noexcept;

        /**
         * @brief 某个包号空间收到的 ECN 计数（ECT(0)、ECT(1)、ECN-CE 三条累计）
         * @details 就是会被写进该空间 ACK 帧的那一份（§19.3.2 的累计口径）。留成读数是为了让用例与运维
         *          都能直接问「对端标了多少、其中被改成 CE 的有几条」，而不是只能从降窗反推
         * @param level 加密级别（包号空间由它定，本类的外部口径一直是级别）
         * @return std::array<std::uint64_t, 3> 三条累计值
         */
        [[nodiscard]] std::array<std::uint64_t, 3> receivedEcnCount(QuicEncryptionLevel level) const noexcept;

        /**
         * @brief 本端此刻允许的最大数据报净载荷（PLPMTU，RFC 9000 §14.4 / RFC 8899）
         * @return std::size_t 不小于 1200；握手没完成或没探到更大的尺寸时就是 BASE
         * @details 发包预算按它算，所以这一格就是「这条路径上本端敢写多大的数据报」。运维与用例靠它分辨
         *          「对端只肯收 1200」与「还没探过」——两者的数一样，`pathMtuPhase()` 不一样
         */
        [[nodiscard]] std::size_t maximumDatagramPayloadByteLength() const noexcept;

        /**
         * @brief 路径 MTU 探测走到哪一格
         * @return QuicPathMtuPhase 见 `QuicPathMtuDiscovery.h`；Disabled 就是握手还没完成
         */
        [[nodiscard]] QuicPathMtuPhase pathMtuPhase() const noexcept;

        /**
         * @brief 当前待探的数据报尺寸，以及本尺寸已经连丢几条探针
         * @return std::pair<std::size_t, std::size_t> 待探尺寸与失败条数；收口（SearchComplete）时
         *         失败条数归零，等抬升计时器再来一轮
         * @details 探针被丢不是拥塞证据（§14.4），所以这条读数与拥塞那两格各说各的事：窗口减半是路在堵，
         *          这一格涨是尺寸过不去
         */
        [[nodiscard]] std::pair<std::size_t, std::size_t> pathMtuProbeState() const noexcept;

        /**
         * @brief 是否已经没有下文了
         * @return true 已收口且待发队列空，外层可以销毁本连接
         */
        [[nodiscard]] bool isFinished() const noexcept;

        /**
         * @brief 对端的传输参数，已解出并验过
         * @return 参数到达并通过 §7.3 的连接标识绑定校验后返回它，否则为空。不合格的参数不会走到这里：
         *         本类会直接发出 TRANSPORT_PARAMETER_ERROR 收口
         */
        [[nodiscard]] const QuicTransportParameters *peerTransportParameters() const noexcept;

        /**
         * @brief 对端证书身份快照
         * @return 握手完成后读取；对端没出示证书或握手未完成时为空
         */
        [[nodiscard]] Core::TlsPeerIdentityPtr peerIdentity() const noexcept;

        /**
         * @brief 流层本体：读写数据、归还额度、取交付都归它
         * @details 核心只负责把入站帧路由进来、把出站帧编进 1-RTT 包，因此这里刻意不做一层同签名转发。
         *          写完数据要再调一次 `drive(now)` 才会真的上线；本层不自己决定发包时刻。
         * @return QuicStreamLayer& 本连接唯一一份，生命周期随本对象
         */
        [[nodiscard]] QuicStreamLayer &streamLayer() noexcept;

        /**
         * @brief 流层的只读视图
         * @return const QuicStreamLayer& 同上
         */
        [[nodiscard]] const QuicStreamLayer &streamLayer() const noexcept;

        /**
         * @brief 现在能不能由本端发起一次 1-RTT 密钥更新
         * @details §6.1 的两条硬条件：握手已确认，且上一个相位里发出去的包已被确认（保证两侧都拿到新密钥）；
         *          再加 §6.5 建议的 3×PTO 静置期，免得对端还在留旧密钥的阶段被我们打断
         * @param now 当前时刻
         * @return true 调 `initiateKeyUpdate(now)` 会真的换密钥
         */
        [[nodiscard]] bool canInitiateKeyUpdate(Timestamp now) const noexcept;

        /**
         * @brief 本端发起密钥更新：推进 1-RTT 写密钥并翻转 Key Phase 位
         * @details 只换写侧（RFC 9001 §6.1）。读侧的「下一代」密钥在装读密钥时就备好了，等对端回应时提成当前。
         *          下一个 1-RTT 包带着新相位出去，那一包就是更新是否被接受的探针。
         * @param now 当前时刻，作为 3×PTO 静置期的起点
         * @return true 真的换了；条件没满足时什么都不做并返回 false
         */
        bool initiateKeyUpdate(Timestamp now);

        /// @return const QuicPacketKeys* 本端当前 1-RTT 写密钥，供用例与日志核对相位
        [[nodiscard]] const QuicPacketKeys *applicationWriteKeys() const noexcept;

        /**
         * @brief 协商出的 ALPN 协议名，握手完成前为空
         * @return std::string_view 供外壳记那条「握手完成」的日志
         */
        [[nodiscard]] std::string_view selectedApplicationProtocol() const noexcept;

        /**
         * @brief 当前的探测超时周期，含退避倍数
         * @details 外壳要靠它算 §6.5 的「3×PTO 静置期」与 §10.1 的空闲下限，本类不自己定闹钟
         * @return Timestamp 恢复层给出的 PTO
         */
        [[nodiscard]] Timestamp probeTimeoutPeriod() const noexcept;

        /**
         * @brief 本端此刻签发的、仍有效的连接标识条数（含握手期那一条）
         * @details 判据落在这一格上：签发要看得见（`NEW_CONNECTION_ID` 真发出去了）、撤回也要看得见
         *          （对端 RETIRE 之后条数真的减了）。运维与用例都靠它分辨「签了没上线」与「退休了没摘掉」
         * @return std::size_t 有效标识条数；握手期恒为 1
         */
        [[nodiscard]] std::size_t issuedConnectionIdCount() const noexcept;

        /**
         * @brief 本端此刻持有的、对端给的可用连接标识条数（含握手期那一条）
         * @return std::size_t 条数；对端一条 NEW_CONNECTION_ID 都没发过时是 1
         */
        [[nodiscard]] std::size_t peerConnectionIdCount() const noexcept;

        /**
         * @brief 本端此刻用来回包的目的连接标识（§7.2）
         * @details 握手期是「对端自报的源标识」，此后可能因 RETIRE_CONNECTION_ID 换到另一条上；
         *          换用与迁移的关系见 §9.5
         * @return std::span<const std::uint8_t> 视图有效期同本对象
         */
        [[nodiscard]] std::span<const std::uint8_t> activePeerConnectionId() const noexcept;

        /**
         * @brief 当前的 RTT 估算（RFC 9002 §5）
         * @return QuicRoundTripTimeEstimate 各统计量与当前 PTO 周期
         * @details 迁移验证通过时整组值会按 §9.4 重置回初值（kInitialRtt 那一档）：这一格是那条
         *          动作最直接的读数，运维与用例都靠它分辨「估算还是旧路径的」与「已经重新起算了」
         */
        [[nodiscard]] QuicRoundTripTimeEstimate roundTripTimeEstimate() const noexcept;

        /**
         * @brief 某条对端路径上来过解得开的数据报没有（§9.3 的「认证过」判据）
         * @details 外壳按它决定「要不要登记与跟随这个来源地址」：解不开的数据报照样能折出合法
         *          形状的路径标识，无条件登记会让伪造迁移（§21.5.4）灌满地址表、拽走回包地址
         * @param pathKey 路径标识
         * @return true 表示这条路径认证过；从没登记过的路径同样为 false
         */
        [[nodiscard]] bool isPathKeyAuthenticated(const QuicPathKey &pathKey) const noexcept;

    private:
        /// 包号空间：0-RTT 借用 Initial 的空间，所以只有三个
        enum class PacketNumberSpace : std::size_t
        {
            Initial,
            Handshake,
            Application,
        };

        static constexpr std::size_t kPacketNumberSpaceCount = 3;

        /// 同时记账的对端路径条数上界：当前发送路径、候选路径、旧路径复验最多三处同时在册，留一个余量
        static constexpr std::size_t kMaximumTrackedPeerPaths = 4;
        /**
         * @brief 欠发的 PATH_RESPONSE 条数上界（§8.2.2 要求逐条回应，但一个包里塞几十条挑战再来要回应，
         *        就是拿本端当放大器）：aioquic 也为同一个理由设了上限（MAX_REMOTE_CHALLENGES），取同一量级
         */
        static constexpr std::size_t kMaximumPendingPathResponses = 8;
        /// 在途挑战条数上界：候选一条 + 旧路径复验一条（外加 §8.2.3 的 MTU 复验一条）
        static constexpr std::size_t kMaximumPendingPathChallenges = 3;

        /**
         * @brief 一个包号空间的状态
         * @details 已收包号存成区间集合：插入时与相邻区间就地合并，覆盖总数随合并增量维护，
         *          因此几千个包号通常只占一两个节点，出 ACK 时也能直接按区间折。
         */
        struct SpaceState
        {
            std::optional<QuicPacketKeys> readKeys{};          ///< 解对端报文用；未就绪时相关报文只能丢弃
            std::optional<QuicPacketKeys> writeKeys{};         ///< 给本端报文加密用
            std::optional<QuicPacketKeys> nextReadKeys{};      ///< 入站：下一代读密钥，只对 1-RTT 有意义（RFC 9001 §6.3）
            std::optional<QuicPacketKeys> previousReadKeys{};  ///< 入站：上一代读密钥，晚到的旧包还要解，最多留 3×PTO（§6.5）
            std::uint64_t                 nextPacketNumber{0}; ///< 下一个要发出的完整包号

            std::optional<std::uint64_t> largestReceivedPacketNumber{};   ///< 本空间已认证的最大包号，包号还原要靠它
            QuicReceivedPacketNumbers    receivedPacketNumbers{};         ///< 已解密成功的包号区间集合，覆盖总数增量维护；出 ACK 的原料
            std::optional<std::uint64_t> largestAckElicitingReceived{};   ///< 最新的触发确认的包号，即 ACK 帧的最大确认值
            std::optional<Timestamp>     largestAckElicitingArrival{};    ///< 它的到达时刻，算 ack_delay
            bool                         isAcknowledgementPending{false}; ///< 有触发确认的包尚未被确认：下一次出包要带 ACK

            std::vector<std::uint8_t>    cryptoStream{};           ///< 出站：本空间已经交给 TLS 产出、可作为重发依据的全部握手字节
            std::uint64_t                cryptoWriteOffset{0};     ///< 出站：下一个待新发字节的偏移，即 cryptoStream 里已排过队的长度
            std::vector<QuicCryptoRange> pendingRetransmissions{}; ///< 出站：判丢或探针后要重发的区间，按偏移递增

            QuicReassemblyBuffer reassembly{}; ///< 入站：按覆盖区合并的 CRYPTO 分片，交付点就是已喂给 TLS 的字节数
        };

        /// 本端签发的一条连接标识：序列号（§5.1.1）与随它一起发出去的无状态重置令牌（§10.3.2）
        struct IssuedConnectionId
        {
            std::vector<std::uint8_t>    id{};                  ///< 标识本体；长度恒与握手那条一致（短头按定长切，换了长度对端就认不回来）
            std::uint64_t                sequence{0};           ///< 序列号；握手期那条是 0
            std::array<std::uint8_t, 16> statelessResetToken{}; ///< NEW_CONNECTION_ID 必带的 128 位令牌
            bool                         isAdvertised{false};   ///< 己方是否已把 NEW_CONNECTION_ID 发出去过（握手那条随长头隐含，恒真）
        };

        /// 对端给过来的一条连接标识：本端持有，随时可以改用（换用的判据见 §9.5）
        struct PeerConnectionId
        {
            std::vector<std::uint8_t>    id{};                  ///< 标识本体
            std::uint64_t                sequence{0};           ///< 序列号；握手期那条是 0
            std::array<std::uint8_t, 16> statelessResetToken{}; ///< 对端给的无状态重置令牌（§19.15 或服务端参数的 0x02）
            /// 上面那格有没有被真正给过一份：没给过的绝不能拿全零去比对（§10.3.1 只许认
            /// 「对端真发过来的令牌」，否则一条末尾 16 个零字节的报文就能掐断连接）
            bool hasStatelessResetToken{false};
        };

        /**
         * @brief 一条对端路径的记账（RFC 9000 §9.3）
         * @details 两处要用：① 路径还没验证时按 §9.3.1 压 3 倍反放大上限；② §9.4 的「确认新地址」
         *          与 §9.3.2 的「退回上一个已验证地址」看 `isValidated`。
         */
        struct PeerPathState
        {
            QuicPathKey   key{};                ///< 路径标识
            std::uint64_t receivedByteCount{0}; ///< 从这条路径收到的字节（含解不开的数据报：§8.1 的口径是「不管有没有被处理」）
            std::uint64_t sentByteCount{0};     ///< 发往这条路径的字节
            bool          isValidated{false};   ///< 已通过握手级验证，或完成过一次 PATH_CHALLENGE/PATH_RESPONSE 往返（§8.2.3）
            /// 这条路径上来过解得开的数据报（§9.3 的「认证过」判据）：外壳按它决定要不要登记与跟随来源地址
            bool hasReceivedAuthenticatedPacket{false};
        };

        /// 已发出、等着回应的 PATH_CHALLENGE（§8.2.1 的发起、§8.2.4 的计时、§13.3 的换数据重发）
        struct PendingPathChallenge
        {
            QuicPathKey                                                 key{};                ///< 挑战打在哪条路径上
            std::array<std::uint8_t, kQuicPathValidationDataByteLength> data{};               ///< 8 字节不可预测数据（§8.2.1 的 MUST）
            bool                                                        isCandidate{false};   ///< true＝验证候选路径（失败要退回）；false＝旧路径复验或 MTU 复验（成败都不动连接）
            bool                                                        isSendPending{true};  ///< 还没上线：arm 之后的下一次出包把它发出去
            bool                                                        wasExpanded{false};   ///< 上线那条有没有扩到 1200：§8.2.3 的第二轮验证按它决定
            Timestamp                                                   sentTime{};           ///< 最近一次上线的时刻
            Timestamp                                                   abandonDeadline{};    ///< 3×max(当前 PTO, 按 kInitialRtt 的 PTO) 到期就放弃（§8.2.4）
            Timestamp                                                   retransmitDeadline{}; ///< 到期换一份新数据重发（§13.3/§9.4）；只对候选路线有效
        };

        /// 欠着的一条 PATH_RESPONSE（§8.2.2：收到挑战就要原样回显，还必须打在同一条路径上）
        struct PendingPathResponse
        {
            QuicPathKey                                                 key{};  ///< 挑战来的那条路径
            std::array<std::uint8_t, kQuicPathValidationDataByteLength> data{}; ///< 原样回显的 8 字节
        };

        /**
         * @brief 一次出包要把整条数据报补到多大，以及为什么补
         * @details 三种补尺寸的理由不能混：路径 MTU 探针补到待探的那一格（被确认即是「这个尺寸走得通」
         *          的证据，§14.4）、客户端握手期的 Initial 补到 1200（§14.1）、路径验证的挑战与回应
         *          补到 1200（§8.2.1/§8.2.2：让新路先证明能过大包）。只有第一种能进 `isPathMtuProbe`
         */
        struct DatagramPadding
        {
            std::size_t datagramByteLength{0}; ///< 整条数据报要凑到的字节数
            bool        isPathMtuProbe{false}; ///< 是不是路径 MTU 探针
        };

        [[nodiscard]] static PacketNumberSpace spaceOf(QuicEncryptionLevel level) noexcept;
        [[nodiscard]] static std::size_t       spaceIndex(QuicEncryptionLevel level) noexcept;
        [[nodiscard]] static std::size_t       spaceIndex(PacketNumberSpace space) noexcept;

        /// 本端是不是服务端：那几条只约束服务端的规则（反放大、HANDSHAKE_DONE 的方向、参数绑定）都问它
        [[nodiscard]] bool isLocalServer() const noexcept;
        /**
         * @brief 本次出包是否受 §8.1 的反放大上限约束
         * @details 只有「服务端 + 地址还没验证」这一种组合受。作客户端时本端就是主动出声的一方，
         *          收到的字节可能一条都没有，按那条上限算会连第一个 Initial 都发不出去
         * @return true 发包额度要先被 3 倍已收字节压一道
         */
        [[nodiscard]] bool isAmplificationLimited() const noexcept;
        /**
         * @brief 客户端按 §7.2 把回包的目的标识换成服务端自报的源标识
         * @details 只跟一次：服务端第一个长头报文里的源标识就是它此后认的那个值。本端的 `Initial`
         *          密钥不受影响（那由 §5.2 的原始目的标识定，与这里换掉的值无关）
         * @param header 刚认证通过的报文头
         */
        void followPeerConnectionId(const QuicPacketHeader &header);

        /**
         * @brief 把「握手已确认」记下来并武装恢复层（RFC 9001 §4.1.2）
         * @details 两型各有一条成立路径，都收在这个入口里：服务端看对端确认过 Handshake 空间的包，
         *          客户端看自己解开了第一条 1-RTT 报文（那之前拿不到应用密钥，也就解不开）或收到
         *          HANDSHAKE_DONE。重复调用无副作用
         * @param now 确认时刻；路径 MTU 探测以它为起点（RFC 9000 §14.3.1 把 BASE 态放在握手完成之后）
         */
        void                                   confirmHandshake(Timestamp now);
        [[nodiscard]] static QuicRecoverySpace recoverySpaceOf(PacketNumberSpace space) noexcept;
        [[nodiscard]] static PacketNumberSpace spaceOf(QuicRecoverySpace space) noexcept;

        QuicDecodeStatus handlePacket(std::span<const std::uint8_t> packet, const QuicPacketHeader &plainHeader, Timestamp arrivalTime, const QuicPathKey &pathKey);
        /**
         * @brief 把一包已认证、已解出的帧序列处理完：帧门、逐帧分派、触发确认记账与路径跟随
         * @details 从 handlePacket 里剥出来的一段：那一位函数的主线是「解一包」（去保护、解密、按级别
         *          记账），帧层这一段的判据（表 3 的帧门、哪些帧触发确认、什么才算「对端搬了家」）
         *          和它是两件事，分开之后两条主线各自读得完
         * @param frames 已解出的帧序列（指向复用缓冲，本函数返回前用完）
         * @param state 本包所属空间的状态
         * @param space 本包所属的包号空间
         * @param packetNumber 本报文的完整包号
         * @param pathKey 本报文来的路径
         * @param arrivalTime 到达时刻
         * @return 与 handlePacket 同义：失败返回 `QuicDecodeError`，本类自行收口
         */
        QuicDecodeStatus handleFrames(const std::vector<QuicFrame> &frames, SpaceState &state, PacketNumberSpace space, std::uint64_t packetNumber, const QuicPathKey &pathKey,
                                      Timestamp arrivalTime);
        /**
         * @brief 把一条认证过的报文落进本空间的账：包号记账与 ECN 字段计数
         * @details 两件事必须同进同出，顺序也只有一种正确写法：§13.4.1 末句要求重复包不重复计数，
         *          而「是不是重复」正是靠本空间的包号集合判的——先去重再计数。拆成两个调用点就会有人
         *          漏掉那一格，把同一报文收两遍当成路上堵了两遍。计数按空间各一份（§19.3.2），
         *          合包里的每一段各自计一次，因为它们共用同一个 IP 头。
         * @param space 本包所属的包号空间
         * @param packetNumber 还原后的完整包号
         * @return bool 本包是新的（true）还是重复收到的（false）；重复的那些调用方直接交空、不再解帧
         */
        bool noteReceivedPacket(PacketNumberSpace space, std::uint64_t packetNumber) noexcept;
        void handleFrame(const QuicFrame &frame, PacketNumberSpace space, Timestamp arrivalTime, const QuicPathKey &pathKey);
        void handleAcknowledgement(const QuicAcknowledgementFrame &frame, PacketNumberSpace space, Timestamp arrivalTime);
        void handleCryptoBytes(PacketNumberSpace space, std::uint64_t offset, std::span<const std::uint8_t> bytes, Timestamp arrivalTime);
        /**
         * @brief 对端报来一条标识（§19.15）：验形状、按 Retire Prior To 退休旧条目、加入本端持有的集合
         * @details 三条判据各自的去向：Retire Prior To 大于序列号＝FRAME_ENCODING_ERROR（§19.15 的 MUST）；
         *          同一条标识换个令牌或换个序列号报上来＝PROTOCOL_VIOLATION（同节的 MAY，本端从严）；
         *          加完之后本端持有的条数超过本端通告的 `active_connection_id_limit` 门槛＝
         *          CONNECTION_ID_LIMIT_ERROR（§5.1.1 明写这一条按"处理完这一帧之后"的数算）。
         *          重复收到同一条帧（相同令牌、相同序列号）按同节要求不算错，静默忽略
         * @param frame 这一帧
         * @param arrivalTime 收口报文要用它当发送时刻
         */
        void handleNewConnectionIdFrame(const QuicNewConnectionIdFrame &frame, Timestamp arrivalTime);
        /**
         * @brief 对端撤回一条本端签发的标识（§19.16）：从在册表摘掉、通知外壳、并补一条新的
         * @details 撤回一个从未发出去的序列号＝PROTOCOL_VIOLATION（§19.16 的 MUST）。补发是 §5.1.1 的
         *          SHOULD：对端主动退回一条，说明它在为将来留位置，而本端持有的余量低于它的上限时
         *          就该把这个位置填上
         * @param frame 这一帧
         * @param arrivalTime 收口报文要用它当发送时刻
         */
        void handleRetireConnectionIdFrame(const QuicRetireConnectionIdFrame &frame, Timestamp arrivalTime);

        /**
         * @brief 一条认证过的报文带来的路径信息：采纳首条 | 按 §9.3 跟随新地址 | 什么都不做
         * @details 只有认证过的报文才作数：伪造来源的报文解不开，也就不该把本端的发包目标拽走
         *          （§9.3.1 描述的正是这种反射攻击）。客户端的规矩相反——§9 明写「客户端收到来自
         *          陌生服务端地址的报文一律丢弃」，那条门在 `handlePacket` 里已经拦了，这里不再跟。
         *          跟随分两步：以前验证过的地址立即切换；没验证过的只发起候选挑战，发包目标留在旧路，
         *          直到 PATH_RESPONSE 确认（§9.3 的 MUST：验证完成前不发非探测帧到新地址）
         * @param pathKey 本报文来的路径
         * @param space 本报文所属的包号空间（包号只在空间内可比）
         * @param packetNumber 本报文的完整包号
         * @param carriesNonProbingFrame 本报文带没带非探测帧（§9.1：只有非探测帧才算「对端搬了家」）
         * @param now 到达时刻
         */
        void followPeerPath(const QuicPathKey &pathKey, PacketNumberSpace space, std::uint64_t packetNumber, bool carriesNonProbingFrame, Timestamp now);

        /**
         * @brief 武装一条 PATH_CHALLENGE（§8.2.1）
         * @details 数据取不可预测的 8 字节随机数（§8.2.1 的 MUST——可预测的话谁都能抢先答话）。
         *          随机数不可用时干脆不发：用可预测数据发出去比不验证更糟。同一条路径上只留最新的一条
         * @param pathKey 挑战打哪条路径
         * @param isCandidate true＝这次验证的成败决定「留在新地址还是退回旧地址」
         * @param now 武装时刻，两把计时都从它起算
         */
        void armPathChallenge(const QuicPathKey &pathKey, bool isCandidate, Timestamp now);

        /// §8.2.4 到期放弃这次验证：按 §9.3.2 退回上一个已验证地址；一个都没有就静默收口
        void abandonPathValidation(const QuicPathKey &pathKey);

        /**
         * @brief 路径验证的两把计时器到点：先按 §13.3/§9.4 换一份新数据重发，再按 §8.2.4 放弃
         * @param now 当前时刻
         */
        void expirePathChallenges(Timestamp now);

        /**
         * @brief 把欠着的 PATH_RESPONSE 与待发的 PATH_CHALLENGE 编成数据报（§8.2.2 的「不得延迟」靠它）
         * @details 每帧单独一包：§8.2.1 明写一个包里不该放多条 PATH_CHALLENGE，而回应与挑战都必须打在
         *          各自那条路径上、还都要补到 1200——挤进普通出包流水就得给那条流水添三样特例
         * @param now 出包时刻
         */
        void queuePathValidationPackets(Timestamp now);

        /**
         * @brief 候选路径验证通过：置验证状态、按 §9.4 重置拥塞与 RTT、通知外壳，必要时补一轮扩充验证
         * @param pathKey 验证通过的路径
         * @param wasExpanded 通过的那一轮挑战上线时有没有扩到 1200（没扩就按 §8.2.3 末段再来一轮）
         * @param now 验证通过的时刻（下一轮挑战从它起算）
         */
        void confirmPeerPathValidated(const QuicPathKey &pathKey, bool wasExpanded, Timestamp now);

        /// §8.2.2：收到 PATH_CHALLENGE，欠一条原样回显（记下它来的那条路径）
        void queuePathResponse(const QuicPathKey &pathKey, const std::array<std::uint8_t, kQuicPathValidationDataByteLength> &data);

        /**
         * @brief §8.2.3：拿回应的数据去对在途的挑战——对上了才算验证通过
         * @details 判据写死在规范里：数据与本端发过的某条 PATH_CHALLENGE 逐字节相同。对不上任何一条
         *          只是忽略（可能迟到、可能属于别的连接），不构成对端违规。候选挑战对上了先确认
         *          （含 §9.4 的重置与 §8.2.3 的补验），再把发包目标切到新路径：验证完成是唯一允许
         *          开始向新地址发非探测帧的时刻（§9.3 的 MUST）
         * @param frame 这一帧
         * @param now 到达时刻（验证通过时把它交给重置与后续挑战）
         */
        void handlePathResponseFrame(const QuicPathResponseFrame &frame, Timestamp now);

        /**
         * @brief 这条路径上此刻还剩多少反放大额度：§8.1 的握手期总额度（若还在那一段）与 §9.3.1
         *        逐路径额度谁小听谁；两条都不适用时给一个足够大的值
         * @details 只对「服务端 + 该路径还没验证」成立：客户端本来就要先出声（§8.1 那条上限是给
         *          被陌生人打到地址上的一方设的），已验证的路径按 §8.2.3 起可以随便发
         */
        [[nodiscard]] std::size_t amplificationLimitForPath(const QuicPathKey &pathKey) const noexcept;

        /// 路径验证那一包能不能补到 1200：§8.2.1/§8.2.2 的例外分支——反放大或拥塞窗口不许就不补，照常发小包
        [[nodiscard]] bool canExpandPathValidationDatagram(const QuicPathKey &pathKey) const noexcept;

        /// 按路径标识找记账；没有就建一条（表满时淘汰最老的一条没在用的），腾不出位置返回空
        [[nodiscard]] PeerPathState *pathStateFor(const QuicPathKey &pathKey) noexcept;

        /// 只查不建
        [[nodiscard]] const PeerPathState *findPathState(const QuicPathKey &pathKey) const noexcept;
        /**
         * @brief 造一条新标识并登记进在册表：随机 18 字节 + 按 §10.3.2 派生的令牌
         * @details 只造不改线：真正上线要等下一次 1-RTT 出包把它编成 NEW_CONNECTION_ID。
         *          造的同时通知外壳登记路由——那一定早于它出现在线上
         * @param isAdvertised 省去一次广告用：握手期那条随长头隐含，直接标成已上线
         * @return 新条目在表里的位置
         */
        std::size_t issueConnectionId(bool isAdvertised);
        /**
         * @brief 本端还有没有余量可以再发几条标识：在册条数低于对端通告的上限就会补
         * @details 上限取对端参数里的 `active_connection_id_limit`（缺席时按 §18.2 的缺省 2——
         *          就是本端自己通告的那一档）。§5.1.1 的 MUST NOT：给出的条数不许超过对端的上限
         * @return true 还可以再签一条
         */
        [[nodiscard]] bool hasRoomForAnotherConnectionId() const noexcept;
        /// §10.3.2：令牌 = HMAC(静态密钥, 标识) 的前 16 字节
        [[nodiscard]] std::array<std::uint8_t, 16> statelessResetTokenOf(std::span<const std::uint8_t> connectionId) const;

        /**
         * @brief 这条数据报是不是一条对上本端令牌的无状态重置（RFC 9000 §10.3.1）
         * @details 判据写死在规范里：末 16 字节与「本端在用的那条对端标识的令牌」逐字节相同，比对
         *          必须不泄露令牌取值（用 `CRYPTO_memcmp` 做常数时间比对）。只比对在用的那条：
         *          正文的 MUST NOT 不许为「还没用过的标识」的令牌放行，而本端的发送目标只有一条
         *          （对端给的其它标识一直只记账不用，§9.5 的换用还没做）
         * @param datagram 这条数据报（必须已过「长度不小于 16」的判断）
         * @return true 对上令牌
         */
        [[nodiscard]] bool isStatelessResetDatagram(std::span<const std::uint8_t> datagram) const noexcept;

        /// §10.3.1 末段：对上令牌即进 draining——不发 CLOSE、也不再发任何报文，并通知外壳
        void enterDrainingAfterStatelessReset();

        /**
         * @brief 把对端在传输参数里给的握手令牌收进在册表（RFC 9000 §10.3：服务端的 0x02 参数）
         * @details 令牌随它对应的那条标识走，而这一份只配给握手期那条（序列号 0）。本端此后
         *          一直拿那条标识当发送目标（§9.5 的换用没做），收下它，「服务端进程没了」这件事
         *          才有一条不靠计时器的判据
         * @param token 对端给的 16 字节令牌
         */
        void notePeerHandshakeStatelessResetToken(const std::array<std::uint8_t, 16> &token) noexcept;
        /**
         * @brief 按 Retire Prior To 退休本端持有的对端标识（§19.15 的处理顺序：先退再加）
         * @details 每退一条都要发一条 RETIRE_CONNECTION_ID（§5.1.2：不许不打招呼就忘掉一条标识）。
         *          如果退掉的正是本端在用的那条，就换到剩下序列号最小的那条上——手上一条都不剩时
         *          保持原值（此后回包仍打旧标识，对端若真把它退了，那是对端自己制造的僵局）
         * @param retirePriorTo 对端给的阈值：序列号小于它的都要退
         */
        void retirePeerConnectionIdsBelow(std::uint64_t retirePriorTo);
        /**
         * @brief 在用的那条对端标识不再被本端持有时，换到剩下序列号最小的一条上
         * @details 调用点排在「添新的那条」之后（§5.1.2 的「先退再添」顺序），因为被退掉的那条常常
         *          正是同一条帧里刚添进来的替代者。只要列表里还有一条就不会出现「无标识可打」：
         *          真被清空时保持原值，回包仍打旧标识，对端若真把它退了那是对端自己制造的僵局
         */
        void ensureActivePeerConnectionIdIsStillHeld();
        /**
         * @brief 把待发的连接标识帧编进这一包（§19.15 的 NEW_CONNECTION_ID 与 §19.16 的 RETIRE）
         * @details 两类帧都只允许出现在 1-RTT 里（§19 表 3 的 Protection 列），所以只有一个调用点。
         *          NEW_CONNECTION_ID 不做确认跟踪：§19.15 明写同一条帧收两遍不算错，判丢后把在册
         *          标识整批再发一遍即可。RETIRE 不同——它不许每包重带（§13.3 只认判丢重发），
         *          因此按条记账：只在「没压在未确认包里」时编进去，编了哪些随包记档、确认即销账
         * @param frames 帧序列缓冲，编进去的都接在它后面
         * @param retirementSequences 出参：本包编进去的退休通报序列号，由调用方随包记档
         * @return bool 是否至少编进了一帧
         */
        bool appendPendingConnectionIdFrames(std::string &frames, std::vector<std::uint64_t> &retirementSequences);
        /**
         * @brief 1-RTT 出包前的两件准备事：路径 MTU 探针与「给对端补一条没在用的标识」
         * @details 两件事都只在 1-RTT 里做（探针要应用空间的写密钥，标识帧只许出现在 1-RTT 包里），
         *          时机相同、各自独立，合成一处省得每次出包都记两遍
         * @param now 当前时刻
         */
        void prepareOneRttPackets(Timestamp now);
        /**
         * @brief 欠着确认时把 ACK 帧编进这一包（§13.2）
         * @details 取数（最大确认包号、区间、延迟、ECN 计数）都在这里；帧只在这一处产出，
         *          确认的「欠」与「还」因此不会各写一份判据
         * @param state 哪个包号空间的状态
         * @param space 空间枚举（ECN 计数按空间各一份）
         * @param now 出包时刻，ACK 延迟字段由它算
         * @param frames 帧序列缓冲
         */
        void appendAcknowledgementIfPending(SpaceState &state, PacketNumberSpace space, Timestamp now, std::string &frames);
        void adoptTlsKeys();
        /**
         * @brief 清掉一个空间的密钥、握手流与在途账
         * @details 密钥没了就等于这个空间不再存在：后续报文按 §5.1 丢弃，出包按「没有写密钥」跳过，
         *          恢复层也不再为它武装定时器（RFC 9002 §A.11）
         * @param space 要退休的包号空间
         */
        void discardSpace(PacketNumberSpace space);
        void adoptTlsRecords();
        /**
         * @brief 从重发队列或未写出的头部里切出下一段要上线的握手字节
         * @details 重发队列优先（§13.3：判丢的那段最可能正是对端缺的）；预算装不下整段时按预算切开，
         *          剩下的留在账上等下一包。一个字节都切不出来时返回空、账目原样不动
         * @param state 该包号空间的账目
         * @param remainingByteBudget 这一包还装得下的字节数
         * @return std::optional<QuicCryptoRange> 切出的区间（有值即非空）；无可发或预算为零时为空
         */
        [[nodiscard]] std::optional<QuicCryptoRange> takeNextCryptoFragment(SpaceState &state, std::size_t remainingByteBudget);

        void queueSpacePackets(PacketNumberSpace space, Timestamp now);
        /**
         * @brief 到点的话发一条路径 MTU 探针（RFC 9000 §14.4、RFC 8899 §5.2）
         * @details 探针是单独一包：只带一条 PING 再补 PADDING 到待探的尺寸。跟普通数据挤在同一包里就
         *          没有「这个尺寸走过去了」这条证据可言——被确认的是那个尺寸，不是那次巧合的拼车。
         *          它照样要吃拥塞窗口（§14.4 明写 probes consume congestion window），所以窗口腾不出待探
         *          尺寸时状态机不算这次到点，也不会把「窗口小」记成「这个尺寸走不通」
         * @param now 当前时刻
         */
        void queuePathMtuProbe(Timestamp now);
        /**
         * @brief 把这些包带过的握手字节区间并进本空间的重发队列
         * @details 两个来源：判丢的包（已经从在途账里划掉，只能由调用方交进来）与探测超时时仍在途的包。
         *          与队里剩下的区间合并，重叠的不重发两遍。
         * @param space 哪个包号空间
         * @param packets 要按偏移补发的包
         */
        void queueRetransmissions(PacketNumberSpace space, std::span<const QuicSentPacketInfo> packets);
        /**
         * @brief HANDSHAKE_DONE 还欠不欠着一个没被确认的副本
         * @details §19.20 要求它「必须重发直到被确认」，因此发过不等于发完：只要确认还没回来、
         *          在途又没有带着它的包，下一轮出包就得再带一次
         * @return true 还需要（再）发一次
         */
        [[nodiscard]] bool isHandshakeDonePending() const noexcept;
        void               queueConnectionClosePacket(Timestamp now);
        void               beginClose(std::uint64_t errorCode, std::string_view reasonPhrase, Timestamp now);
        /**
         * @brief 流层交回的违规：按它的错误码收口，没违规时什么都不做
         * @param result 入站帧的处理结果
         * @param now 收口报文的发送时刻
         */
        void reportStreamViolation(const QuicStreamViolationStatus &result, Timestamp now);
        /**
         * @brief 组一个包发出去，并把它记进在途账
         * @param space 哪个包号空间
         * @param frames 已编码好的帧序列
         * @param now 发出时刻
         * @param isAckEliciting 本包是否触发确认
         * @param cryptoRange 本包带的握手字节区间，判丢时按它补发
         * @param streamRanges 本包带的流数据区间，确认与判丢都按它回收额度
         * @param carriesHandshakeDone 本包是否带了 HANDSHAKE_DONE：它要在被确认之前一直重发（§19.20）
         * @param streamAnnouncements 本包带出的流收口宣告，确认与判丢同样按它回收（§13.3）
         * @param carriesConnectionIdFrames 本包是否带了 NEW_CONNECTION_ID / RETIRE_CONNECTION_ID（§13.3 要求判丢后重发）
         * @param retirementSequences 本包带出的退休通报序列号：确认按它销账、判丢按它放回待发（§13.3）
         * @param padding 非空就把整包补到这个尺寸（PADDING 帧），要哪一种、为什么补见 `DatagramPadding`
         * @param pathKey 这条数据报发往哪条对端路径：由调用方解析好——普通出包传 `m_sendPathKey`，
         *        路径验证传各自那把键。全零是合法的键值（单路径用法），所以这里没有「留空＝当前路径」
         *        这种默认：§9.3.3 的复验探测要打的正是那条全零键的旧路径，混一起就成了打给新地址
         */
        void emitPacket(PacketNumberSpace space, const std::string &frames, Timestamp now, bool isAckEliciting, std::optional<QuicCryptoRange> cryptoRange,
                        std::vector<QuicStreamRange> streamRanges, bool carriesHandshakeDone, std::vector<QuicStreamAnnouncement> streamAnnouncements,
                        bool carriesConnectionIdFrames, std::vector<std::uint64_t> retirementSequences, std::optional<DatagramPadding> padding, const QuicPathKey &pathKey);
        /**
         * @brief 这一轮还能往网络上压多少净字节
         * @details 三层取最小：数据报上限（§14.1）、拥塞窗口的余量（§7）、以及地址验证之前的
         *          反放大上限（RFC 9000 §8.1，只算到 3 倍已收字节）。
         * @param reservedByteLength 包头 + 包号 + AEAD 标签 + 已有帧的字节数，预算从它上面扣
         * @param ignoresCongestionWindow 探针不受窗口阻塞（§7.5），重发未确认的字节时给 true
         * @return std::size_t 还能装进一个数据报的载荷字节数，算出负数一律收成 0
         */
        [[nodiscard]] std::size_t sendByteBudget(std::size_t reservedByteLength, bool ignoresCongestionWindow) const;
        /**
         * @brief 反放大额度是否连一个固定开销都装不下
         * @details 拥塞窗口豁免探针，这条不豁免：地址没验证之前，只带 ACK 的包也一样是放大
         * @param reservedByteLength 这一包除载荷以外的固定字节数
         * @return true 本空间这一轮什么都不能发
         */
        [[nodiscard]] bool isBlockedByAmplificationLimit(std::size_t reservedByteLength) const noexcept;
        /// @return std::size_t 3 倍已收字节减去已发字节；已验证地址时这条额度不存在
        [[nodiscard]] std::size_t amplificationRemainingByteCount() const noexcept;
        void                      adoptPeerTransportParameters(Timestamp now);
        /**
         * @brief 空闲超时的有效值
         * @details 两端都宣告就是两者取小，只有一端宣告非 0 就用那一个，都是 0 则不启用（§10.1）；
         *          对端参数还没到之前只有本端这一个值可用。最后按 §10.1 的硬要求抬到至少 3 倍 PTO
         * @return std::optional<Timestamp> 不启用时返回空
         */
        [[nodiscard]] std::optional<Timestamp> effectiveIdleTimeout() const noexcept;
        /// @return std::optional<Timestamp> 空闲超时的到期时刻；还没启用时为空
        [[nodiscard]] std::optional<Timestamp> idleDeadlineTime() const noexcept;
        /**
         * @brief 收到并处理成功一个包：重新定下本期空闲额度与截止时刻
         * @details 额度里那个「至少 3 倍当前 PTO」的下限按这一刻的 PTO 取（§10.1）。之后即使
         *          退避把 PTO 翻倍，截止点也不再后移——否则 max_idle_timeout 就成了空头承诺
         * @param now 这个包的到达时刻
         */
        void restartIdleTimer(Timestamp now);

        /// §6.2/§6.3：把下一代读密钥提成当前，并按对端的相位同步推进本端写密钥
        void applyPeerKeyUpdate(SpaceState &state, Timestamp now);
        /// §6.5：过了 3×PTO 就把上一代读密钥丢掉，只留当前与下一代两套
        void                                                    retireStaleKeyPhase(Timestamp now);
        [[nodiscard]] static std::optional<QuicEncryptionLevel> levelOf(const QuicPacketHeader &header) noexcept;
        [[nodiscard]] static QuicEncryptionLevel                levelOf(PacketNumberSpace space) noexcept;
        [[nodiscard]] PacketNumberSpace                         highestSpaceWithWriteKeys() const noexcept;

        QuicConnectionCoreConfiguration m_configuration; ///< 建连接时给的那些值，发包要反复用
        std::unique_ptr<QuicTlsContext> m_tls;           ///< 每连接的 TLS 上下文
        QuicRecovery                    m_recovery{};    ///< 发包记账、RTT、判丢与探测超时
        /// 拥塞窗口：算法在构造期按 `m_configuration.congestionAlgorithm` 选（见 makeQuicCongestionAlgorithm）
        std::unique_ptr<QuicCongestionAlgorithm> m_congestion;
        /// 路径 MTU 探测（RFC 9000 §14.4 / RFC 8899）：发包上限由它给，握手完成时进 BASE、对端参数到手时收天花板
        QuicPathMtuDiscovery m_pathMtu{};
        /// 判成持久拥塞的累计次数：只有「收到确认」那一条出口会加一（§7.6.2 把建立放在收到确认之后），
        /// 只由所属循环线程读写
        std::size_t                                     m_persistentCongestionEventCount{0};
        QuicStreamLayer                                 m_streams;              ///< 流与流量控制；额度取自本端参数，出站要等对端参数
        std::size_t                                     m_receivedByteCount{0}; ///< 已收字节，反放大上限按它算（§8.1）
        std::size_t                                     m_sentByteCount{0};     ///< 已发字节，与上面那项一起决定还剩多少额度
        std::array<SpaceState, kPacketNumberSpaceCount> m_spaces{};             ///< 三个包号空间
        /// 待发数据报队列：元素就是报文本体（std::string 在本仓里当字节缓冲用，与帧序列同一个口径），
        /// 组包器直接往里写，取出时按所有权移交
        /// 各包号空间收到的 ECN 计数（ECT(0)、ECT(1)、CE 三条累计），发 ACK 时按 §19.3.2 原样带上
        std::array<std::array<std::uint64_t, 3>, kPacketNumberSpaceCount> m_receivedEcnCounts{};
        /// 本次 `onDatagramReceived` 那条数据报的 ECN 字段；空表示本端读不到，因此一条都不许计
        std::optional<std::uint8_t> m_incomingEcnCodepoint{};
        /// 本端是否至少真的读到过一次 ECN 字段：没读到过就不在 ACK 里报计数（§13.4.1 的许可）
        bool                                     m_isEcnFieldReadable{false};
        std::deque<QuicOutboundDatagram>         m_outboundDatagrams{};                     ///< 待发数据报队列
        QuicConnectionPhase                      m_phase{QuicConnectionPhase::Handshaking}; ///< 当前阶段
        std::optional<std::uint64_t>             m_localCloseErrorCode{};                   ///< 待发的 CONNECTION_CLOSE 错误码
        std::string                              m_localCloseReasonPhrase{};                ///< 随错误码一起发出的原因文案
        bool                                     m_isHandshakeDoneAcknowledged{false};      ///< 带 HANDSHAKE_DONE 的包被确认过，之后不再重发（§19.20）
        bool                                     m_isHandshakeDoneInFlight{false};          ///< 有一份 HANDSHAKE_DONE 还在途：确认或判丢之前不重复发
        bool                                     m_isHandshakeConfirmed{false};             ///< 对端确认过 Handshake 空间的包，§4.1.2 的「握手已确认」
        bool                                     m_isAddressValidated{false};               ///< 收到过能解开的 Handshake 及以上级别的包，§8.1 的反放大上限到此为止
        bool                                     m_hasAdoptedPeerConnectionId{false};       ///< 客户端已把回包目的标识换成服务端自报的那个（§7.2），此后不再跟
        std::optional<PacketNumberSpace>         m_probeSpace{};                            ///< 探测超时到期后欠一条触发确认的包，出包时补上
        std::optional<Timestamp>                 m_idleDeadline{};                          ///< 空闲超时的截止时刻，只在「活动」发生时重算
        std::optional<Timestamp>                 m_idlePeriod{};                            ///< 本期空闲额度，收包那一刻定下；主动发包的续期沿用它
        std::optional<Timestamp>                 m_lastKeepalive{};                         ///< 上一次为保活排探针的时刻，只管节律不参与续期
        bool                                     m_hasSentAckElicitingSinceReceipt{false};  ///< 上次收包之后是否已发过触发确认的包（§10.1 只让第一包续期）
        bool                                     m_isSendKeyPhaseSet{false};                ///< 本端出包的 Key Phase 位，随写密钥一起翻（RFC 9001 §6.1）
        bool                                     m_isReadKeyPhaseSet{false};                ///< 本端当前读密钥对应的相位位；收发两套各自记账（§6.5）
        std::optional<Timestamp>                 m_keyPhaseChangedAt{};                     ///< 最近一次换相位的时刻，3×PTO 静置期与旧密钥回收都从它起算
        std::optional<std::uint64_t>             m_lowestPacketNumberSentInKeyPhase{};      ///< 本相位发过的最小包号（§6.1 的下一次更新门禁）
        bool                                     m_isKeyPhaseAcknowledged{false};           ///< 对端确认了本相位里的某个包，才允许再来一次更新
        std::optional<QuicTransportParameters>   m_peerParameters{};                        ///< 验过的对端参数
        std::optional<std::vector<std::uint8_t>> m_peerFirstInitialSourceConnectionId{};    ///< 对端第一个 Initial 里的源标识，§7.3 的绑定校验靠它
        /**
         * @brief 本端签发的连接标识在册表（§5.1.1）：握手期那条是序列号 0，其后逐条 +1
         * @details 「在册」＝已签发且未被对端 RETIRE。撤回的条目直接从表里摘掉，序列号不回收
         *          （§5.1.1 要求逐条 +1，复用序列号等于把两条不同的标识说成同一条）
         */
        std::vector<IssuedConnectionId> m_issuedConnectionIds{};
        /// 下一个要用的序列号：握手那条占 0，之后从 1 起递增。只增不减，与在册表分开记
        std::uint64_t m_nextConnectionIdSequence{1};
        /// 对端给过来的连接标识（§5.1.2 的「本端持有、随时可改用」），序列号 0 是握手期那条
        std::vector<PeerConnectionId> m_peerConnectionIds{};
        /// 对端声明过的、最大的 Retire Prior To：只认递增的值（§19.15「较小的值不再有效」）
        std::uint64_t m_peerRetirePriorTo{0};
        /// 本端发出去的 RETIRE_CONNECTION_ID 里最大的序列号；对端撤回一个比这还大的号即越界（§19.16）
        std::uint64_t m_largestRetiredSequenceSent{0};
        /// 一条还没销账的退休通报：§13.3 只认「判丢重发」，因此按条记在途——在途的不重复编进包，
        /// 确认回来即整条销账，判丢则放回待发。永不销账的旧账会让每个后续包都重带全部历史通报
        struct PendingRetirementNotice
        {
            std::uint64_t sequence{0};       ///< 退休的序列号
            bool          isInFlight{false}; ///< 是否已随一个还没被确认的包上路
        };
        /// 待销账的退休通报，按退休先后排
        std::vector<PendingRetirementNotice> m_pendingRetirementNotices{};
        /// 有一包带 NEW_CONNECTION_ID 的包被判丢：下一次出包把在册标识整批再发一遍（§13.3；
        /// 退休通报不走这个粗账，它按条记在 `m_pendingRetirementNotices` 里）
        bool m_isConnectionIdFramesDirty{false};
        /// 无状态重置令牌的派生密钥（§10.3.2）：配置没给就在这里随机取一把
        std::vector<std::uint8_t> m_statelessResetKey{};

        /// 对端路径的记账（§9.3）：当前发送路径、正在验证的候选、外壳报到过的其它来源；上界见 kMaximumTrackedPeerPaths
        std::vector<PeerPathState> m_peerPaths{};
        /// 本端出包默认打哪条路径：第一条认证过的报文定下它（此后按 §9.3 只跟编号更高的非探测包换）
        QuicPathKey m_sendPathKey{};
        /// 路径有没有被定下来过：全零键是合法值（单路径用法留空），单靠它分不出「还没定」与「定了空键」
        bool m_hasAdoptedPeerPath{false};
        /// 最近一次验证通过的对端路径（§9.3.2 的退回目标）；「静默收口」那条判据看 m_hasValidatedPeerPath
        QuicPathKey m_validatedPathKey{};
        /// 有没有一条已验证过的路径：全零键是合法值，光靠 m_validatedPathKey 分不出「没有」与「就是空键」
        bool m_hasValidatedPeerPath{false};
        /// 各包号空间见过的最大非探测包号：§9.3 只让发包目标跟着更高的那些走，防乱序包把它拽回旧路
        std::array<std::optional<std::uint64_t>, kPacketNumberSpaceCount> m_largestNonProbingPacketNumbers{};
        /// 在途的 PATH_CHALLENGE：§8.2.4 的放弃计时与 §13.3 的换数据重发都按它算
        std::vector<PendingPathChallenge> m_pendingPathChallenges{};
        /// 欠发的 PATH_RESPONSE（§8.2.2），出包时按各自路径发走
        std::vector<PendingPathResponse> m_pendingPathResponses{};
    };
} // namespace AsynGyanis::Net
