#include "Net/Quic/Crypto/QuicTlsContext.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/QuicOpenSslError.h"

#include <openssl/core_dispatch.h>
#include <openssl/obj_mac.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 把出站连接的三项身份落到这条会话上：SNI、证书里的校验名、ALPN
         * @details 三项都是**硬要求**而不是可调项：少了 SNI，前面坐着 SNI 路由的服务端只会给出默认证书；
         *          少了校验名，OpenSSL 只按名字表里的 CN/SAN 做默认比对，主机名拼错也就没人管。
         *          落不上去时当场抛，不静默跳过——一条「看起来握手成功、其实没验身份」的连接比握手失败更危险。
         * @param session 已建好、尚未挂回调的会话
         * @param settings 客户端身份设置
         * @throws Base::InvalidArgumentException 用法错误：主机名为空，或某个协议标识长度不合法
         * @throws Base::Exception 运行期故障：OpenSSL 拒了这三项里的任何一项
         */
        void applyClientTlsSettings(SSL &session, const QuicClientTlsSettings &settings)
        {
            if (settings.hostName.empty())
            {
                throw Base::InvalidArgumentException("QUIC 出站连接建立失败：客户端身份设置里主机名为空，"
                                                     "SNI 与证书校验名都无处可取");
            }
            if (SSL_set_tlsext_host_name(&session, settings.hostName.c_str()) != 1)
            {
                throw Base::Exception("QUIC 出站连接建立失败：SNI 落不上去（" + quicOpenSslErrorText() + "）");
            }
            if (SSL_set1_host(&session, settings.hostName.c_str()) != 1)
            {
                throw Base::Exception("QUIC 出站连接建立失败：证书校验名落不上去（" + quicOpenSslErrorText() + "）");
            }
            if (settings.applicationProtocolIdentifiers.empty())
            {
                return;
            }
            // ALPN 的线格式是「长度 + 字节」逐条串起来，OpenSSL 不替你算这个前缀
            std::vector<unsigned char> encodedIdentifiers;
            for (const std::string &identifier: settings.applicationProtocolIdentifiers)
            {
                if (identifier.empty() || identifier.size() > 255U)
                {
                    throw Base::InvalidArgumentException("QUIC 出站连接建立失败：ALPN 协议标识 \"" + identifier + "\" 长度不合法（须在 1..255 字节之间）");
                }
                encodedIdentifiers.push_back(static_cast<unsigned char>(identifier.size()));
                encodedIdentifiers.insert(encodedIdentifiers.end(), identifier.begin(), identifier.end());
            }
            // 注意这条的返回值是**反的**：0 才是成功（与 SSL_CTX_set_alpn_protos 同一口径）
            if (SSL_set_alpn_protos(&session, encodedIdentifiers.data(), static_cast<unsigned int>(encodedIdentifiers.size())) != 0)
            {
                throw Base::Exception("QUIC 出站连接建立失败：ALPN 列表落不上去（" + quicOpenSslErrorText() + "）");
            }
        }
        /**
         * @brief 把 OpenSSL 的保护级别换成自家的加密级别
         * @param protectionLevel OSSL_RECORD_PROTECTION_LEVEL_* 之一
         * @return std::optional<QuicEncryptionLevel> 可识别时给出级别；EARLY 之外的未知取值返回空
         */
        std::optional<QuicEncryptionLevel> toEncryptionLevel(const std::uint32_t protectionLevel) noexcept
        {
            switch (protectionLevel)
            {
                case OSSL_RECORD_PROTECTION_LEVEL_NONE:
                    return QuicEncryptionLevel::Initial;
                case OSSL_RECORD_PROTECTION_LEVEL_EARLY:
                    return QuicEncryptionLevel::ZeroRtt;
                case OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE:
                    return QuicEncryptionLevel::Handshake;
                case OSSL_RECORD_PROTECTION_LEVEL_APPLICATION:
                    return QuicEncryptionLevel::Application;
                default:
                    return std::nullopt;
            }
        }

        /**
         * @brief 从协商出的密码推 QUIC 用的套件
         * @param session TLS 会话
         * @return std::optional<QuicCipherSuite> 能识别时给出套件；未协商或未知算法返回空
         */
        std::optional<QuicCipherSuite> cipherSuiteOf(SSL &session) noexcept
        {
            const SSL_CIPHER *const cipher = SSL_get_current_cipher(&session);
            if (cipher == nullptr)
            {
                return std::nullopt;
            }
            switch (SSL_CIPHER_get_cipher_nid(cipher))
            {
                case NID_aes_128_gcm:
                    return QuicCipherSuite::Aes128Gcm;
                case NID_aes_256_gcm:
                    return QuicCipherSuite::Aes256Gcm;
                case NID_chacha20_poly1305:
                    return QuicCipherSuite::ChaCha20Poly1305;
                default:
                    return std::nullopt;
            }
        }

    } // namespace

    const OSSL_DISPATCH *QuicTlsContext::dispatchTable() noexcept
    {
        // 六个回调组成的派发表；末参 arg 交回本对象，因此不占用 SSL 的 app_data 槽
        static const OSSL_DISPATCH table[] = {
                {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onSendCryptoData)},
                {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onReadCryptoData)},
                {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onReleaseCryptoData)},
                {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onYieldSecret)},
                {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onGotTransportParameters)},
                {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)(void)>(&QuicTlsContext::onAlert)},
                OSSL_DISPATCH_END,
        };
        return table;
    }

    std::size_t QuicTlsContext::keySlot(const QuicEncryptionLevel level, const QuicKeyDirection direction) noexcept
    {
        return static_cast<std::size_t>(level) * 2 + static_cast<std::size_t>(direction);
    }

    QuicTlsContext::QuicTlsContext(SSL_CTX &tlsContext, const bool isServerSide, const std::span<const std::uint8_t> localTransportParameters,
                                   const QuicClientTlsSettings *clientSettings)
    {
        // 全程用局部指针，成功到底才交给成员：任何一步抛出去都不存在「构造失败但析构又来放一次」的会话
        SSL *const session = SSL_new(&tlsContext);
        if (session == nullptr)
        {
            throw Base::Exception("QUIC 连接建立失败：无法从 TLS 上下文创建会话（" + quicOpenSslErrorText() + "）");
        }
        if (isServerSide)
        {
            SSL_set_accept_state(session);
        } else
        {
            SSL_set_connect_state(session);
        }

        if (clientSettings != nullptr)
        {
            try
            {
                applyClientTlsSettings(*session, *clientSettings);
            } catch (...)
            {
                SSL_free(session);
                throw;
            }
        }

        if (SSL_set_quic_tls_cbs(session, dispatchTable(), this) != 1)
        {
            SSL_free(session);
            throw Base::Exception("QUIC 连接建立失败：挂 QUIC TLS 回调被拒（" + quicOpenSslErrorText() + "）：请确认依赖里的 OpenSSL 是 3.5 以上的主线版本");
        }

        if (!localTransportParameters.empty())
        {
            // 先落到成员再交指针：OpenSSL 的 `ossl_quic_tls_set_transport_params` 只是
            // `qtls->local_transport_params = transport_params`，不复制内容，交临时量会当场悬空
            m_localTransportParameters.assign(localTransportParameters.begin(), localTransportParameters.end());
            if (SSL_set_quic_tls_transport_params(session, m_localTransportParameters.data(), m_localTransportParameters.size()) != 1)
            {
                SSL_free(session);
                throw Base::Exception("QUIC 连接建立失败：本端 transport parameters 被拒（" + quicOpenSslErrorText() +
                                      "）："
                                      "请核对是否按 RFC 9000 §18 编码、必填项是否齐全");
            }
        }
        m_session = session;
    }

    QuicTlsContext::~QuicTlsContext()
    {
        // 只释放会话，不做「先解绑回调」：`ossl_quic_tls_free` 全程不回调用方，而
        // `SSL_set_quic_tls_cbs` 传空表会在遍历派发表时直接解引用空指针
        SSL_free(m_session);
    }

    void QuicTlsContext::feedHandshakeData(const QuicEncryptionLevel level, const std::span<const std::uint8_t> data)
    {
        if (data.empty())
        {
            return;
        }
        std::vector<std::uint8_t> &buffer = m_inboundData[static_cast<std::size_t>(level)];
        buffer.insert(buffer.end(), data.begin(), data.end());
    }

    QuicTlsProgress QuicTlsContext::drive()
    {
        if (m_alert.has_value())
        {
            return QuicTlsProgress::Failed;
        }

        if (!m_handshakeCompleted)
        {
            const int handshakeResult = SSL_do_handshake(m_session);
            if (handshakeResult == 1)
            {
                // 走到这里六个回调都已跑完，各级别各方向的密钥槽必然就位，标记可以放心地最后置上
                m_handshakeCompleted = true;
            } else
            {
                // 把真实返回值交给 SSL_get_error：0 与负值在它那里分属「对端关线」与「还要数据」两条路
                switch (SSL_get_error(m_session, handshakeResult))
                {
                    case SSL_ERROR_WANT_READ:
                    case SSL_ERROR_WANT_WRITE:
                        return QuicTlsProgress::NeedData;
                    default:
                        return QuicTlsProgress::Failed;
                }
            }
        }

        // 握手完成后的记录（如服务端发来的 NewSessionTicket）仍挂在这条 TLS 流上：不主动抽干，
        // 它们就一直压在入站缓冲里，TLS 也永远不会去解析它们
        if (!m_inboundData[static_cast<std::size_t>(QuicEncryptionLevel::Application)].empty())
        {
            static_cast<void>(SSL_read(m_session, nullptr, 0));
        }
        return QuicTlsProgress::Completed;
    }

    bool QuicTlsContext::isHandshakeCompleted() const noexcept
    {
        return m_handshakeCompleted;
    }

    std::optional<QuicTlsRecord> QuicTlsContext::takeOutboundRecord()
    {
        if (m_outboundRecords.empty())
        {
            return std::nullopt;
        }
        std::optional<QuicTlsRecord> record = std::move(m_outboundRecords.front());
        m_outboundRecords.pop_front();
        return record;
    }

    const QuicPacketKeys *QuicTlsContext::keys(const QuicEncryptionLevel level, const QuicKeyDirection direction) const noexcept
    {
        const std::optional<QuicPacketKeys> &slot = m_keys[keySlot(level, direction)];
        return slot.has_value() ? &*slot : nullptr;
    }

    std::span<const std::uint8_t> QuicTlsContext::peerTransportParameters() const noexcept
    {
        return m_peerTransportParameters;
    }

    std::string_view QuicTlsContext::selectedApplicationProtocol() const noexcept
    {
        const unsigned char *protocol       = nullptr;
        unsigned int         protocolLength = 0;
        SSL_get0_alpn_selected(m_session, &protocol, &protocolLength);
        // 没协商上时 OpenSSL 会把指针置空、长度归零，此时给空视图而不是拿空指针构造 string_view
        return protocol == nullptr ? std::string_view{} : std::string_view(reinterpret_cast<const char *>(protocol), protocolLength);
    }

    std::optional<QuicCipherSuite> QuicTlsContext::cipherSuite() const noexcept
    {
        return m_cipherSuite;
    }

    std::optional<std::uint8_t> QuicTlsContext::alert() const noexcept
    {
        return m_alert;
    }

    void QuicTlsContext::appendOutbound(const QuicEncryptionLevel level, const std::span<const std::uint8_t> data)
    {
        // 同一级别的连续产出合成一条记录：级别一变就必须新起一条，因为 CRYPTO 帧要按级别交给
        // 不同包号空间的报文，合成一条会把后面的字节挂错级别
        if (!m_outboundRecords.empty() && m_outboundRecords.back().level == level)
        {
            auto &tail = m_outboundRecords.back().data;
            tail.insert(tail.end(), data.begin(), data.end());
            return;
        }
        m_outboundRecords.push_back(QuicTlsRecord{level, std::vector<std::uint8_t>(data.begin(), data.end())});
    }

    QuicTlsContext *QuicTlsContext::self(void *const argument) noexcept
    {
        return static_cast<QuicTlsContext *>(argument);
    }

    int QuicTlsContext::onSendCryptoData(SSL *, const unsigned char *data, const std::size_t length, std::size_t *consumed, void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context == nullptr)
        {
            return 0;
        }
        context->appendOutbound(context->m_transmissionLevel, {data, length});
        // 本端总是整条收下：没有「只吃一半」的余地，半条 TLS 记录会让状态机无法继续
        *consumed = length;
        return 1;
    }

    int QuicTlsContext::onReadCryptoData(SSL *, const unsigned char **data, std::size_t *length, void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context == nullptr)
        {
            return 0;
        }
        // 只交当前读级别的那一条缓冲：整条缓冲会被 TLS 当作「一条记录」，跨级别的字节混在一起
        // 会让 ServerHello 结束时不在记录边界上，状态机当场判错
        std::vector<std::uint8_t> &buffer = context->m_inboundData[static_cast<std::size_t>(context->m_inboundLevel)];
        *data                             = buffer.data();
        *length                           = buffer.size();
        return 1;
    }

    int QuicTlsContext::onReleaseCryptoData(SSL *, const std::size_t length, void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context == nullptr)
        {
            return 1;
        }
        std::vector<std::uint8_t> &buffer = context->m_inboundData[static_cast<std::size_t>(context->m_inboundLevel)];
        if (length != buffer.size())
        {
            // TLS 只在整条记录消耗完时才来回调，交回的字节数必然等于当初给出去的那条缓冲；
            // 对不上说明接线被改坏了，继续走会让同一批字节被解析两次
            return 0;
        }
        buffer.clear();
        return 1;
    }

    int QuicTlsContext::onYieldSecret(SSL *session, const std::uint32_t protectionLevel, const int direction, const unsigned char *secret, const std::size_t length,
                                      void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context == nullptr)
        {
            return 1;
        }
        const std::optional<QuicEncryptionLevel> level = toEncryptionLevel(protectionLevel);
        if (!level.has_value() || session == nullptr)
        {
            return 0;
        }
        // Initial 级别的套件按 RFC 9001 §5.2 固定是 AES_128_GCM，此刻还没有协商结果可查
        std::optional<QuicCipherSuite> suite = cipherSuiteOf(*session);
        if (!suite.has_value())
        {
            if (*level != QuicEncryptionLevel::Initial)
            {
                return 0;
            }
            suite = QuicCipherSuite::Aes128Gcm;
        }
        context->m_cipherSuite = suite;

        if (length != quicCipherSuiteSecretByteLength(*suite))
        {
            // 长度不合就导出会静默按短secret算，产出的密钥看着有效却全解不开，宁可当场拒
            return 0;
        }
        const auto             keyMaterial             = deriveQuicPacketKeys(*suite, {secret, length});
        const QuicKeyDirection keyDirection            = direction == 0 ? QuicKeyDirection::Reading : QuicKeyDirection::Writing;
        context->m_keys[keySlot(*level, keyDirection)] = keyMaterial;
        if (direction == 0)
        {
            // 读密钥一换，对端接下来的字节就改用新级别保护了：入站缓冲要跟着换一条
            context->m_inboundLevel = *level;
        } else
        {
            // crypto_send 不带级别参数，本端「正在产出哪个级别」只能跟着写方向的密钥切换
            context->m_transmissionLevel = *level;
        }
        return 1;
    }

    int QuicTlsContext::onGotTransportParameters(SSL *, const unsigned char *params, const std::size_t length, void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context == nullptr)
        {
            return 1;
        }
        context->m_peerTransportParameters.assign(params, params + length);
        return 1;
    }

    int QuicTlsContext::onAlert(SSL *, const unsigned char alertCode, void *const argument)
    {
        QuicTlsContext *const context = self(argument);
        if (context != nullptr)
        {
            context->m_alert = alertCode;
        }
        // 收下告警本身，让 drive() 下一轮把它转成 Failed：返回 0 会让 OpenSSL 在告警还没发出去时就中止
        return 1;
    }
} // namespace AsynGyanis::Net
