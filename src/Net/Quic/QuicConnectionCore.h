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
 * @note 本里程碑只做服务端、只做握手：0-RTT、RETRY、版本协商、流与流量控制、密钥更新、
 *       连接迁移都不在当前能力内，收到对应的报文按各自章节丢弃，留待后续里程碑。
 *       丢包恢复已经接进来：发包记账、RTT、判丢、探测超时与按偏移重发握手字节都在本类里跑。
 * @warning 不是线程安全的：一个实例属于一条连接，只能在所属事件循环线程上驱动。
 */

#pragma once

#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"
#include "Net/Quic/Crypto/QuicTlsContext.h"
#include "Net/Quic/Recovery/QuicRecovery.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
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
     *          （它决定 Initial 密钥，也必须经传输参数回传给对端，RFC 9000 §7.3）。
     * @note 各 vector 都拷进本对象，构造完成后调用方即可销毁源容器。
     */
    struct QuicConnectionCoreConfiguration
    {
        SSL_CTX *tlsContext{nullptr};                                      ///< 已配好证书与 ALPN 的 TLS 上下文，生命周期须覆盖本对象
        std::vector<std::uint8_t> localConnectionId{};                     ///< 本端签发的源连接标识：短头按它定长度，路由表也认它
        std::vector<std::uint8_t> peerConnectionId{};                      ///< 回包的目的连接标识：对端自报的源标识（§7.2）
        std::vector<std::uint8_t> originalDestinationConnectionId{};       ///< 客户端第一个 Initial 的目的标识，Initial 密钥由它推导
        QuicTransportParameters transportParameters{};                     ///< 本端要声明的传输参数；两个必填的连接标识项由本类按上面三个值补齐
    };

    /**
     * @brief 一条连接的服务端传输状态机
     *
     * @details 输入只有 `onDatagramReceived`，输出只有 `takeOutboundDatagram`：本类不持有 socket，也不
     *          自己决定何时发包，由外层（现在的 `QuicConnection` 外壳或单测）喂完包调 `drive`、再把待发
     *          队列抽干。
     *
     * @note 包号分三个空间各算各的（Initial / Handshake / Application）：一个数据报里混放不同级别的
     *       报文是合法写法（§12.2 合包），跨空间的包号互相看不见。
     */
    class QuicConnectionCore
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

        QuicConnectionCore(const QuicConnectionCore &) = delete;
        QuicConnectionCore &operator=(const QuicConnectionCore &) = delete;
        QuicConnectionCore(QuicConnectionCore &&) = delete;
        QuicConnectionCore &operator=(QuicConnectionCore &&) = delete;

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
        [[nodiscard]] std::expected<void, QuicDecodeError> onDatagramReceived(std::span<const std::uint8_t> datagram,
                                                                              Timestamp arrivalTime);

        /**
         * @brief 取走一条待发数据报
         * @return 有数据报时返回它（取走即出队），队列空时返回空
         */
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> takeOutboundDatagram();

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

    private:
        /// 包号空间：0-RTT 借用 Initial 的空间，所以只有三个
        enum class PacketNumberSpace : std::size_t
        {
            Initial,
            Handshake,
            Application,
        };

        static constexpr std::size_t kPacketNumberSpaceCount = 3;

        /**
         * @brief 一个包号空间的状态
         * @details 已收包号用有序集合而不是区间结构：握手期每空间只有几十到几百个包，区间合并的
         *          边界条件是纯出错面，换不来什么。
         */
        struct SpaceState
        {
            std::optional<QuicPacketKeys> readKeys{};    ///< 解对端报文用；未就绪时相关报文只能丢弃
            std::optional<QuicPacketKeys> writeKeys{};   ///< 给本端报文加密用
            std::uint64_t nextPacketNumber{0};           ///< 下一个要发出的完整包号

            std::optional<std::uint64_t> largestReceivedPacketNumber{}; ///< 本空间已认证的最大包号，包号还原要靠它
            std::set<std::uint64_t> receivedPacketNumbers{};            ///< 已解密成功的包号，出 ACK 的原料
            std::optional<std::uint64_t> largestAckElicitingReceived{}; ///< 最新的触发确认的包号，即 ACK 帧的最大确认值
            std::optional<Timestamp> largestAckElicitingArrival{};      ///< 它的到达时刻，算 ack_delay
            bool isAcknowledgementPending{false};        ///< 有触发确认的包尚未被确认：下一次出包要带 ACK

            std::vector<std::uint8_t> cryptoStream{};    ///< 出站：本空间已经交给 TLS 产出、可作为重发依据的全部握手字节
            std::uint64_t cryptoWriteOffset{0};          ///< 出站：下一个待新发字节的偏移，即 cryptoStream 里已排过队的长度
            std::vector<QuicCryptoRange> pendingRetransmissions{}; ///< 出站：判丢或探针后要重发的区间，按偏移递增

            std::uint64_t receivedCryptoOffset{0};          ///< 入站：已经交给 TLS 的字节数，也就是下一个期望偏移
            std::map<std::uint64_t, std::vector<std::uint8_t>> laterCryptoFragments{}; ///< 入站：早到的乱序分片，按偏移存
            std::size_t bufferedOutOfOrderByteCount{0};     ///< 上面那个表的总字节数，用来守住缓存上限
        };

        [[nodiscard]] static PacketNumberSpace spaceOf(QuicEncryptionLevel level) noexcept;
        [[nodiscard]] static std::size_t spaceIndex(QuicEncryptionLevel level) noexcept;
        [[nodiscard]] static std::size_t spaceIndex(PacketNumberSpace space) noexcept;
        [[nodiscard]] static QuicRecoverySpace recoverySpaceOf(PacketNumberSpace space) noexcept;
        [[nodiscard]] static PacketNumberSpace spaceOf(QuicRecoverySpace space) noexcept;

        std::expected<void, QuicDecodeError> handlePacket(std::span<const std::uint8_t> packet, const QuicPacketHeader &plainHeader, Timestamp arrivalTime);
        void handleFrame(const QuicFrame &frame, PacketNumberSpace space, Timestamp arrivalTime);
        void handleAcknowledgement(const QuicAcknowledgementFrame &frame, PacketNumberSpace space, Timestamp arrivalTime);
        void handleCryptoBytes(PacketNumberSpace space, std::uint64_t offset, std::span<const std::uint8_t> bytes, Timestamp arrivalTime);
        void adoptTlsKeys();
        void adoptTlsRecords();
        void queueSpacePackets(PacketNumberSpace space, Timestamp now);
        /**
         * @brief 把这些包带过的握手字节区间并进本空间的重发队列
         * @details 两个来源：判丢的包（已经从在途账里划掉，只能由调用方交进来）与探测超时时仍在途的包。
         *          与队里剩下的区间合并，重叠的不重发两遍。
         * @param space 哪个包号空间
         * @param packets 要按偏移补发的包
         */
        void queueRetransmissions(PacketNumberSpace space, std::span<const QuicSentPacketInfo> packets);
        void queueConnectionClosePacket(Timestamp now);
        void beginClose(std::uint64_t errorCode, std::string_view reasonPhrase, Timestamp now);
        void emitPacket(PacketNumberSpace space, const std::string &frames, Timestamp now, bool isAckEliciting,
                        std::optional<QuicCryptoRange> cryptoRange);
        void adoptPeerTransportParameters(Timestamp now);
        [[nodiscard]] static std::optional<QuicEncryptionLevel> levelOf(const QuicPacketHeader &header) noexcept;
        [[nodiscard]] static QuicEncryptionLevel levelOf(PacketNumberSpace space) noexcept;
        [[nodiscard]] PacketNumberSpace highestSpaceWithWriteKeys() const noexcept;

        QuicConnectionCoreConfiguration m_configuration;             ///< 建连接时给的那些值，发包要反复用
        std::unique_ptr<QuicTlsContext> m_tls;                       ///< 每连接的 TLS 上下文
        QuicRecovery m_recovery{};                                   ///< 发包记账、RTT、判丢与探测超时
        std::array<SpaceState, kPacketNumberSpaceCount> m_spaces{};  ///< 三个包号空间
        std::deque<std::vector<std::uint8_t>> m_outboundDatagrams{}; ///< 待发数据报队列
        QuicConnectionPhase m_phase{QuicConnectionPhase::Handshaking}; ///< 当前阶段
        std::optional<std::uint64_t> m_localCloseErrorCode{};        ///< 待发的 CONNECTION_CLOSE 错误码
        std::string m_localCloseReasonPhrase{};                      ///< 随错误码一起发出的原因文案
        bool m_hasSentHandshakeDone{false};                          ///< HANDSHAKE_DONE 一生只发一次（§19.20）
        bool m_isHandshakeConfirmed{false};                          ///< 对端确认过 Handshake 空间的包，§4.1.2 的「握手已确认」
        std::optional<PacketNumberSpace> m_probeSpace{};             ///< 探测超时到期后欠一条触发确认的包，出包时补上
        std::optional<QuicTransportParameters> m_peerParameters{};   ///< 验过的对端参数
        std::optional<std::vector<std::uint8_t>> m_peerFirstInitialSourceConnectionId{}; ///< 对端第一个 Initial 里的源标识，§7.3 的绑定校验靠它
    };
} // namespace AsynGyanis::Net
