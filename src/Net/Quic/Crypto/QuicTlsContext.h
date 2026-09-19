/**
 * @file QuicTlsContext.h
 * @brief QUIC 的 TLS 胶水（RFC 9001 §4.1、OpenSSL 的 QUIC TLS 回调）：纯内存驱动握手并导出密钥
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details OpenSSL 3.5+ 的 `SSL_set_quic_tls_cbs` 是**拉模型**：TLS 通过回调向本类要握手字节、
 *          并把产出的字节交回来，全程不碰 BIO 与 socket。因此本类可以只当作「TLS 记录的多路复用器」
 *          来测——两端各建一个上下文，把记录互相搬运，就能在内存里跑完整个握手。
 *
 * @note 刻意不使用 quictls 的 `SSL_provide_quic_data` / `SSL_set_quic_method` 那一套：主线 OpenSSL
 *       3.6 里没有这些符号（实测头文件零命中），且它们的推送模型与本类的缓冲语义不符。
 */

#pragma once

#include "Net/Quic/Crypto/QuicPacketKeys.h"

#include <openssl/ssl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 加密级别，与 OpenSSL 的 OSSL_RECORD_PROTECTION_LEVEL_* 对应
     *
     * @details 顺序刻意按握手的推进排列：Initial → 0-RTT → Handshake → Application。
     *          OpenSSL 侧的 NONE 就是这里的 Initial（Initial 包用固定盐推导的密钥保护）。
     */
    enum class QuicEncryptionLevel
    {
        Initial,      ///< 首批握手字节
        ZeroRtt,      ///< 早数据（本实现不支持，只作为级别取值占位）
        Handshake,    ///< 握手后续字节
        Application,  ///< 1-RTT 字节，含握手完成后的票据
    };

    /// 密钥的方向，按「谁发出」命名，与 `QuicPacketDirection` 同一套判据
    enum class QuicKeyDirection
    {
        Reading, ///< 解对端发来的报文
        Writing, ///< 给本端发出的报文加密
    };

    /// 一次驱动的结果
    enum class QuicTlsProgress
    {
        NeedData,  ///< 还要对方交来握手字节（或本端产出了待发记录，继续走一轮）
        Completed, ///< 握手完成，1-RTT 密钥已就位
        Failed,    ///< 握手失败：TLS 报了告警或运算出错，本连接该收口
    };

    /**
     * @brief 一条待发的 TLS 记录：属于哪个级别、要作为 CRYPTO 帧的负载交出去
     */
    struct QuicTlsRecord
    {
        QuicEncryptionLevel level{QuicEncryptionLevel::Initial}; ///< 该用哪个级别的密钥保护
        std::vector<std::uint8_t> data{};                        ///< 握手字节，交给 CRYPTO 帧
    };

    /**
     * @brief 一条连接的 TLS 上下文
     *
     * @details 持有每连接的 `SSL`，实现 OpenSSL 的六个 QUIC TLS 回调：产出握手数据、取回入站数据、
     *          交出各方向的 traffic secret（本类立刻用它导出包保护密钥）、收对端 transport parameters、
     *          收告警。密钥导出交给 `deriveQuicPacketKeys`，本类不碰 AEAD 与头部保护。
     *
     * @warning 归事件循环所有的约定由上层承担：本类不是线程安全的，只在所属循环线程上驱动。
     * @note 一个 `SSL` 对应一条连接；构造即建会话并挂好回调，析构只释放会话（释放期间不再有回调）。
     */
    class QuicTlsContext
    {
    public:
        /**
         * @brief 建立每连接的 TLS 上下文
         * @details 服务端走 accept 态、客户端走 connect 态，两侧都挂同一张回调表：客户端只用于测试与
         *          将来的主动连接，服务端才是本项目的生产路径。
         * @param tlsContext 已配好证书与 ALPN 的上下文（生命周期必须覆盖本对象）
         * @param isServerSide true 为服务端
         * @param localTransportParameters 已按 RFC 9000 §18 编码的本端参数；交空即不设参数（握手会因缺
         *        `initial_source_connection_id` 等必填项被对端判错）。**本类会复制一份**：OpenSSL 的
         *        `SSL_set_quic_tls_transport_params` 只记指针不拷内容，所以交临时缓冲进来也不会悬空
         * @throws Base::Exception 运行期故障：建会话失败、挂回调失败或 OpenSSL 拒绝了参数
         */
        QuicTlsContext(SSL_CTX &tlsContext, bool isServerSide, std::span<const std::uint8_t> localTransportParameters);

        /**
         * @brief 释放 TLS 会话
         * @details 释放过程不会回调本对象，因此不需要（也不能）先解绑回调表。
         */
        ~QuicTlsContext();

        QuicTlsContext(const QuicTlsContext &) = delete;
        QuicTlsContext &operator=(const QuicTlsContext &) = delete;

        /**
         * @brief 把对端 CRYPTO 帧的负载交给对应级别的入站缓冲
         * @details 级别参数**必须**收：每个级别是独立的包号空间，报文可以乱序到达，Initial 的尾巴
         *          完全可能落在 Handshake 之后。更要命的是 TLS 状态机要求 ServerHello 结束处正好是一条
         *          记录的末尾（RFC 8446 §4.4.1），把两个级别的字节接成一条流会打断那个判据。
         * @param level 该批字节来自哪个级别的报文
         * @param data 握手字节
         */
        void feedHandshakeData(QuicEncryptionLevel level, std::span<const std::uint8_t> data);

        /**
         * @brief 推进握手一步
         * @return NeedData 还在等对端字节（或本端有待发记录没取走）
         * @return Completed 握手完成
         * @return Failed 已告警或出错，调用方应回 CONNECTION_CLOSE 并丢弃连接
         */
        [[nodiscard]] QuicTlsProgress drive();

        /**
         * @brief 握手是否已完成
         * @return true 已收到对端Finished且1-RTT密钥已导出
         */
        [[nodiscard]] bool isHandshakeCompleted() const noexcept;

        /**
         * @brief 取走一条待发的 TLS 记录
         * @return 有记录时返回它（取走即从队列移除）；没有时返回空
         */
        [[nodiscard]] std::optional<QuicTlsRecord> takeOutboundRecord();

        /**
         * @brief 取某个级别、某个方向的包保护密钥
         * @param level 加密级别
         * @param direction 读或写
         * @return 密钥已导出时返回它，否则为空；Initial 的密钥由连接层从目的连接标识推导，
         *         不经此处（TLS 也会在 NONE 级别交出同样的值，两边必须一致）
         */
        [[nodiscard]] const QuicPacketKeys *keys(QuicEncryptionLevel level, QuicKeyDirection direction) const noexcept;

        /**
         * @brief 对端的 transport parameters 原文
         * @return 已收到时返回编码字节的视图，否则为空视图；解码由连接层的参数编解码器负责
         */
        [[nodiscard]] std::span<const std::uint8_t> peerTransportParameters() const noexcept;

        /**
         * @brief 已协商出的密码套件
         * @return 套件确定前为 nullopt；本类的密钥一律按这个套件导出
         */
        [[nodiscard]] std::optional<QuicCipherSuite> cipherSuite() const noexcept;

        /**
         * @brief 最近一次 TLS 告警码
         * @return 有告警时返回其字节值，否则为空；连接收口的原因文案可以带上它
         */
        [[nodiscard]] std::optional<std::uint8_t> alert() const noexcept;

    private:
        /// 密钥槽位总数：四个级别 × 两个方向
        static constexpr std::size_t kKeySlotCount = 8;

        /// 入站缓冲个数：四个级别各一条
        static constexpr std::size_t kLevelCount = 4;

        /**
         * @brief 槽位下标
         * @param level 加密级别
         * @param direction 读或写
         * @return std::size_t 数组下标
         */
        [[nodiscard]] static std::size_t keySlot(QuicEncryptionLevel level, QuicKeyDirection direction) noexcept;

        // 六个 OpenSSL 回调的静态蹦床：从 arg 取回本对象，再转给下面的成员实现
        /// 下面六个静态蹦床的签名由 OpenSSL 的派发表定死，改名会破坏接线
        static int onSendCryptoData(SSL *session, const unsigned char *data, std::size_t length, std::size_t *consumed, void *argument);
        static int onReadCryptoData(SSL *session, const unsigned char **data, std::size_t *length, void *argument);
        static int onReleaseCryptoData(SSL *session, std::size_t length, void *argument);
        static int onYieldSecret(SSL *session, std::uint32_t protectionLevel, int direction, const unsigned char *secret,
                                 std::size_t length, void *argument);
        static int onGotTransportParameters(SSL *session, const unsigned char *params, std::size_t length, void *argument);
        static int onAlert(SSL *session, unsigned char alertCode, void *argument);

        /// 从 OpenSSL 回调的末参取回本对象：派发表的 arg 就是构造时交进去的 this
        [[nodiscard]] static QuicTlsContext *self(void *argument) noexcept;

        /**
         * @brief OpenSSL 的 QUIC TLS 回调派发表
         * @details 放在类内而不是文件作用域：表里要取六个私有静态蹦床的地址，文件作用域取不到。
         * @return const OSSL_DISPATCH * 以 OSSL_DISPATCH_END 收尾的静态表
         */
        [[nodiscard]] static const OSSL_DISPATCH *dispatchTable() noexcept;

        /// 把一段产出接进待发队列：同级且正好接在尾条之后的合成一条
        void appendOutbound(QuicEncryptionLevel level, std::span<const std::uint8_t> data);

        SSL            *m_session{nullptr}; ///< 每连接的 TLS 会话；构造成功交回对象时恒非空
        /// 本端参数的副本：OpenSSL 只记指针不拷内容，所以这段字节必须活到 `SSL_free` 之后。
        /// 成员要等析构函数体（里面做 `SSL_free`）跑完才销毁，顺序天然满足
        std::vector<std::uint8_t> m_localTransportParameters{};
        std::array<std::vector<std::uint8_t>, kLevelCount> m_inboundData{}; ///< 各级别对端交来、尚未被 TLS 消耗完的握手字节
        QuicEncryptionLevel m_inboundLevel{QuicEncryptionLevel::Initial};    ///< TLS 当前该从哪个级别取字节
        std::deque<QuicTlsRecord> m_outboundRecords{};  ///< 待交出的 TLS 记录，按产出顺序
        std::array<std::optional<QuicPacketKeys>, kKeySlotCount> m_keys{}; ///< 各级别各方向的密钥槽
        std::vector<std::uint8_t> m_peerTransportParameters{}; ///< 对端参数原文
        std::optional<std::uint8_t> m_alert{};            ///< 最近一次告警码
        std::optional<QuicCipherSuite> m_cipherSuite{};   ///< 已协商出的套件
        QuicEncryptionLevel m_transmissionLevel{QuicEncryptionLevel::Initial}; ///< 下一条产出记录属于哪个级别
        bool m_handshakeCompleted{false}; ///< 握手完成标记，最后发布
    };
} // namespace AsynGyanis::Net
