#include "Net/WebSocket/WebSocketClient.h"
#include "Base/Coding/Base64.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/Crypto/Random.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/ConnectionRace.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Tcp/TcpStream.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/WebSocket/WebSocketUtf8.h"
#include "Net/WebSocket/WebSocketError.h"
#include <openssl/ssl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace AsynGyanis::Net
{
    struct WebSocketClient::PendingRead
    {
        /// 惰性 Task 只在这里启动一次；到期后不销毁，下一次等待继续用它
        std::optional<Core::Task<ssize_t>> task;
    };

    namespace
    {
        /// 明文与 TLS 的默认端口：Configuration.port 留 0 时按 TLS 那一位回落
        constexpr std::uint16_t kDefaultPlainTextPort = 80;
        constexpr std::uint16_t kDefaultSecurePort    = 443;
        /// ALPN 只提的那一个协议名：本类的升级走 101 那条通路（RFC 6455 §4.1）
        constexpr std::string_view kAlpnHttp11Name = "http/1.1";
        /// Sec-WebSocket-Key 的字节数（RFC 6455 §4.1 第 7 条：16 字节随机数按标准 base64 送出）
        constexpr std::size_t kClientKeyByteLength = 16;
        /// 掩码键的字节数（RFC 6455 §5.3），逐帧新取
        constexpr std::size_t kMaskKeyByteLength = 4;
        /// 关闭原因的长度上限：控制帧整体不得过 125 字节，其中状态码占 2（RFC 6455 §5.5、§5.5.1）
        constexpr std::size_t kMaximumCloseReasonLength = kWebSocketMaximumControlPayloadLength - kWebSocketCloseCodeByteLength;
        /// 挂起读的检查节拍：不追求毫秒级精确，只保证到期与本地关闭都能在下一拍被看见
        constexpr std::chrono::milliseconds kReceivePollInterval{5};
        /**
         * @brief 从系统的密码学随机源取 length 字节
         * @details 取不到就抛出：全零或复用的键等于放弃 §5.3 要防的那种中间设施缓存重放，
         *          「降级成不随机」比这次操作失败更糟
         * @param length 需要的字节数
         * @return std::string 按「指针 + 长度」取的随机字节，可以含 NUL
         * @throws Base::Exception 随机源不可用
         */
        std::string cryptographicRandomBytes(const std::size_t length)
        {
            // 取不到就直接抛：Core::Random 的报错文案已经带上「熵源不足 / 随机源失效」与替代做法，
            // 这里不再另造一句——两处各写一份时，将来只有其中一处会跟着返回码的语义走
            const std::vector<std::uint8_t> bytes = Core::Random::bytes(length);
            return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        /// 本帧的掩码键：逐帧新取，绝不复用（§5.3 的抗重放全靠这一条）
        [[nodiscard]] WebSocketMaskKey nextMaskKey()
        {
            const std::string rawBytes = cryptographicRandomBytes(kMaskKeyByteLength);
            WebSocketMaskKey  key;
            std::copy_n(rawBytes.begin(), static_cast<std::ptrdiff_t>(kMaskKeyByteLength), key.bytes.begin());
            return key;
        }
        /// 按 TLS 那一位取实际要连的端口
        [[nodiscard]] std::uint16_t resolvePort(const WebSocketClient::Configuration &configuration)
        {
            if (configuration.port != 0)
            {
                return configuration.port;
            }
            return configuration.clientTls != nullptr ? kDefaultSecurePort : kDefaultPlainTextPort;
        }
        /**
         * @brief Host 头部要写的权威标识
         * @details 默认端口不写端口号（RFC 9110 §4.2.2：Host 取 URI 的权威部分，而默认端口在规范化时
         *          被去掉）；非默认端口必须带上，否则按名字装虚拟主机的服务端选不出该回哪份证书
         */
        [[nodiscard]] std::string hostHeaderFor(const std::string &hostName, const std::uint16_t port, const bool isTls)
        {
            const std::uint16_t defaultPort = isTls ? kDefaultSecurePort : kDefaultPlainTextPort;
            if (port == defaultPort)
            {
                return hostName;
            }
            return hostName + ":" + std::to_string(port);
        }
        /// 主机名是不是 IP 字面量：IP 与 DNS 名的证书校验规则不同，必须分开设（RFC 6125 §6.3）
        [[nodiscard]] bool isIpLiteralAddress(const std::string &hostName)
        {
            return Core::InetAddress::parseLiteral(hostName, 0).has_value();
        }
        /// 一次交换用的读块大小：与 HttpClient 同档，够让 101 的头块在少数几轮里读完
        constexpr std::size_t kExchangeChunkByteLength = 4096;
        /**
         * @brief 升级应答的读取进度
         * @details 五个出口各对应一种线上事实：缺一个出口就意味着某种「对端不说话」的形状没人管
         */
        enum class UpgradeReadOutcome
        {
            NeedsMoreBytes,    ///< 头块还没收齐，继续读
            ResponseComplete,  ///< 一条完整应答到手
            MalformedResponse, ///< 报文不合规
            PeerClosed,        ///< 对端在给出完整应答之前收口
            ReadFailed         ///< 读错误（含本端到时限把套接字关掉那一种）
        };
        /// 升级交换的产出：解析好的应答，以及头块之后一起到的那截帧字节
        struct UpgradeExchange
        {
            HttpResponseInfo response;  ///< 解析完成的应答（状态码与头部）
            std::string      headBytes; ///< 请求原文，写失败时不参与结论
            std::string      leftover;  ///< 头块结束之后的字节：属于第一条帧，一个都不能丢
        };
        /**
         * @brief 把刚读到的一段字节喂给解析器，并给出这一轮的进度
         * @details 两条通路（明文与 TLS）共用这一份判定：进度口径与拒因只在「读」这一步分岔
         * @param parser 本次交换专用的解析器
         * @param leftover 出参：头块之后到的字节累加在这里
         * @param data 刚读到的缓冲区首地址
         * @param length 刚读到的字节数
         * @param isEndOfStream 对端是否已正常收口（读到 0 字节）
         * @return UpgradeReadOutcome 这一轮的结论
         */
        UpgradeReadOutcome feedUpgradeBytes(HttpResponseParser &parser, std::string &leftover, const char *const data, const std::size_t length, const bool isEndOfStream)
        {
            if (isEndOfStream)
            {
                return UpgradeReadOutcome::PeerClosed;
            }
            const std::size_t consumedByte = parser.feed(std::string_view(data, length));
            if (parser.hasFailed())
            {
                return UpgradeReadOutcome::MalformedResponse;
            }
            if (!parser.isComplete())
            {
                return UpgradeReadOutcome::NeedsMoreBytes;
            }
            // 101 之后紧跟着的就是帧字节：解析器只消化到头块结束，剩下的必须留给解码器，否则服务端
            // 「升级应答与第一条消息一起发」这种写法会被截掉半条消息
            if (consumedByte < length)
            {
                leftover.append(data + consumedByte, length - consumedByte);
            }
            return UpgradeReadOutcome::ResponseComplete;
        }
        /**
         * @brief 把一条应答读到底：明文通路
         * @details 刻意不走 HttpOutboundConnection：那条通路的 parser 是按「带正文的响应」设计的，
         *          而升级交换之后这条连接就不再是 HTTP 了。
         * @param loop 所属事件循环（看门狗要挂在它上面定时）
         * @param stream 已连上的明文流
         * @param exchangeBudget 本段（写出请求 + 等到 101）可用的剩余时限；空表示预算已用尽
         * @param requestText 升级请求原文
         * @param target 目标描述，只进拒因文本
         * @return Core::Task<WebSocketUpgradeExpected<UpgradeExchange>> 一条完整应答与它之后的那截字节
         */
        Core::Task<WebSocketUpgradeExpected<UpgradeExchange>> exchangeOverPlain(Core::EventLoop &loop, TcpStream &stream,
                                                                                const std::optional<std::chrono::milliseconds> exchangeBudget, const std::string_view requestText,
                                                                                const std::string target)
        {
            if (!exchangeBudget.has_value())
            {
                co_return std::unexpected(
                        WebSocketUpgradeError{WebSocketUpgradeErrorKind::DeadlineExpired, std::format("WebSocket 出站的握手时限已用尽：还没开始读取 101 应答（目标 {}）", target)});
            }
            // 对端收了请求一言不发时，这条通路上的等待没有别的出口：看门狗到点把套接字关掉，
            // 挂在读上的协程因此被叫醒（关掉描述符本身不会唤醒 epoll 的等待者）
            const Core::DeadlineGuard<TcpStream>       upgradeDeadline(loop, stream, *exchangeBudget, "WebSocketClient");
            std::array<char, kExchangeChunkByteLength> chunk{};
            UpgradeExchange                            exchange;
            HttpResponseParser                         parser;
            try
            {
                co_await stream.writeAll(requestText.data(), requestText.size());
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::TransportFailed, std::format("升级请求没能写进通路（目标 {}）：{}。底层原因：{}", target,
                                                                                                                        "连接在握手期间断开或已到时限", failure.what())});
            }
            while (true)
            {
                // 看门狗掐出来的那条出口是抛出（等待期间套接字被关），而本函数的出口是
                // std::expected：抛出必须在这里落成拒因。让它穿过 connect() 的 expected 契约，
                // 调用方既拿不到结局也等不到协程收尾，现场看着就像「时限到了也不回」
                ssize_t received = 0;
                try
                {
                    received = co_await stream.read(chunk.data(), chunk.size());
                } catch (const Base::Exception &failure)
                {
                    co_return std::unexpected(WebSocketUpgradeError{
                            WebSocketUpgradeErrorKind::TransportFailed,
                            std::format("读取 101 应答时通路出错（目标 {}）：通路被关掉——本次握手的时限到点，或对端复位、本端把会话撤了。底层原因：{}", target, failure.what())});
                }
                if (received < 0)
                {
                    co_return std::unexpected(
                            WebSocketUpgradeError{WebSocketUpgradeErrorKind::TransportFailed, std::format("读取 101 应答时通路出错（目标 {}）：被对端复位或本端被撤", target)});
                }
                const auto outcome = feedUpgradeBytes(parser, exchange.leftover, chunk.data(), received == 0 ? 0 : static_cast<std::size_t>(received), received == 0);
                if (outcome == UpgradeReadOutcome::NeedsMoreBytes)
                {
                    continue;
                }
                if (outcome == UpgradeReadOutcome::PeerClosed)
                {
                    co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::PeerClosedBeforeUpgrade,
                                                                    std::format("对端在给出 101 应答之前就把连接关了（目标 {}）：它可能没把这个路径当作 WebSocket 端点", target)});
                }
                if (outcome == UpgradeReadOutcome::MalformedResponse)
                {
                    co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::MalformedHttpResponse,
                                                                    std::format("对端的升级应答不是合法 HTTP 报文（目标 {}）：状态行或头部读不懂，或越出了声明的上限", target)});
                }
                exchange.response = parser.result();
                co_return exchange;
            }
        }
        /**
         * @brief 把一条应答读到底：TLS 通路
         * @details 与明文那一条同一条判定，只是读写走 TlsSocket，时限也照明文那一条挂。
         * @param loop 所属事件循环（看门狗要挂在它上面定时）
         * @param tlsSocket 已完成握手的客户端 TLS 套接字
         * @param exchangeBudget 本段（写出请求 + 等到 101）可用的剩余时限；空表示预算已用尽
         * @param requestText 升级请求原文
         * @param target 目标描述，只进拒因文本
         * @return Core::Task<WebSocketUpgradeExpected<UpgradeExchange>> 一条完整应答与它之后的那截字节
         */
        Core::Task<WebSocketUpgradeExpected<UpgradeExchange>> exchangeOverTls(Core::EventLoop &loop, Core::TlsSocket &tlsSocket,
                                                                              const std::optional<std::chrono::milliseconds> exchangeBudget, const std::string_view requestText,
                                                                              const std::string target)
        {
            if (!exchangeBudget.has_value())
            {
                co_return std::unexpected(
                        WebSocketUpgradeError{WebSocketUpgradeErrorKind::DeadlineExpired, std::format("WebSocket 出站的握手时限已用尽：还没开始读取 101 应答（目标 {}）", target)});
            }
            const Core::DeadlineGuard<Core::TlsSocket> upgradeDeadline(loop, tlsSocket, *exchangeBudget, "WebSocketClient");
            std::array<char, kExchangeChunkByteLength> chunk{};
            UpgradeExchange                            exchange;
            HttpResponseParser                         parser;
            try
            {
                const ssize_t writtenByte = co_await tlsSocket.asyncSend(requestText.data(), requestText.size());
                if (writtenByte <= 0)
                {
                    co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::TransportFailed,
                                                                    std::format("升级请求没能写进通路（目标 {}）：通路在握手期间被关掉或已到时限", target)});
                }
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::TransportFailed,
                                                                std::format("升级请求没能写进通路（目标 {}）：{}。底层原因：{}", target, "TLS 写出失败", failure.what())});
            }
            while (true)
            {
                ssize_t received = 0;
                try
                {
                    received = co_await tlsSocket.asyncReceive(chunk.data(), chunk.size());
                } catch (const Base::Exception &failure)
                {
                    co_return std::unexpected(WebSocketUpgradeError{
                            WebSocketUpgradeErrorKind::TransportFailed,
                            std::format("读取 101 应答时通路出错（目标 {}）：通路被关掉——本次握手的时限到点，或对端复位、本端把会话撤了。底层原因：{}", target, failure.what())});
                }
                const auto outcome = feedUpgradeBytes(parser, exchange.leftover, chunk.data(), received == 0 ? 0 : static_cast<std::size_t>(received), received == 0);
                if (outcome == UpgradeReadOutcome::NeedsMoreBytes)
                {
                    continue;
                }
                if (outcome == UpgradeReadOutcome::PeerClosed)
                {
                    co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::PeerClosedBeforeUpgrade,
                                                                    std::format("对端在给出 101 应答之前就把连接关了（目标 {}）：它可能没把这个路径当作 WebSocket 端点", target)});
                }
                if (outcome == UpgradeReadOutcome::MalformedResponse)
                {
                    co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::MalformedHttpResponse,
                                                                    std::format("对端的升级应答不是合法 HTTP 报文（目标 {}）：状态行或头部读不懂，或越出了声明的上限", target)});
                }
                exchange.response = parser.result();
                co_return exchange;
            }
        }
        /**
         * @brief 在已经连上的套接字上完成客户端 TLS 握手
         * @details 三件事缺一不可：SNI（让按名字装虚拟主机的服务端挑对证书）、证书主机名校验
         *          （只验链不验名字，任何受信 CA 给他域签的证书都能冒充目标，CWE-297）、ALPN 只提
         *          http/1.1（协商出 h2 就没有 101 而是扩展 CONNECT，帧的形状与本类不是一套）。
         * @param loop 所属事件循环
         * @param context 调用方持有的客户端 TLS 上下文
         * @param socket 已经连上的套接字，所有权交给本函数
         * @param hostName 用于 SNI 与主机名校验的名字
         * @param handshakeBudget TLS 那一段的剩余时限；空表示已经没有预算可用
         * @return Core::Task<WebSocketUpgradeExpected<std::unique_ptr<Core::TlsSocket>>> 成功交出套接字，失败给中文拒因
         */
        Core::Task<WebSocketUpgradeExpected<std::unique_ptr<Core::TlsSocket>>> makeClientTlsSocket(Core::EventLoop &loop, const Core::TlsContext &context, Core::AsyncSocket socket,
                                                                                                   const std::string                              hostName,
                                                                                                   const std::optional<std::chrono::milliseconds> handshakeBudget)
        {
            if (!handshakeBudget.has_value())
            {
                co_return std::unexpected(
                        WebSocketUpgradeError{WebSocketUpgradeErrorKind::DeadlineExpired, std::format("WebSocket 出站的握手时限已用尽：还没开始 TLS 握手（主机 {}）", hostName)});
            }
            // createSSL 把 SSL_new 与 SSL_set_fd 一起做了：少了绑定这一步 SSL_connect 立刻失败，
            // 而错误队列是空的，现场只剩「握手失败」四个字
            SSL *ssl = context.createSSL(socket.fileDescriptor());
            SSL_set_tlsext_host_name(ssl, hostName.c_str());
            if (isIpLiteralAddress(hostName))
            {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), hostName.c_str());
            } else
            {
                SSL_set1_host(ssl, hostName.c_str());
            }
            // ALPN 的线格式是「单字节长度 + 协议名」的串联：填错不会报错，只会对端一条都匹配不上
            std::string encodedProtocolNames;
            encodedProtocolNames.push_back(static_cast<char>(kAlpnHttp11Name.size()));
            encodedProtocolNames.append(kAlpnHttp11Name);
            if (::SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char *>(encodedProtocolNames.data()), static_cast<unsigned int>(encodedProtocolNames.size())) != 0)
            {
                // 还没交给 TlsSocket，这份 SSL 归本函数收尾：漏掉 SSL_free 就是一次带引用的泄漏
                SSL_free(ssl);
                co_return std::unexpected(WebSocketUpgradeError{WebSocketUpgradeErrorKind::TlsSetupRejected, "TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法"});
            }
            auto tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(socket), Core::TlsSocket::Role::Client);
            // 看门狗到点即关套接字：否则「对端不谈了」会让这条协程永久挂着，这一段没有别的出口
            const Core::DeadlineGuard<Core::TlsSocket> tlsDeadline(loop, *tlsSocket, *handshakeBudget, "WebSocketClient");
            try
            {
                co_await tlsSocket->handshake();
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(WebSocketUpgradeError{
                        WebSocketUpgradeErrorKind::TlsHandshakeFailed,
                        std::format("TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线（主机 {}）。底层原因：{}", hostName, failure.what())});
            }
            co_return tlsSocket;
        }
        /**
         * @brief 把一条 101 应答核到底，并核对它选定的子协议
         * @details accept 那一格由 `validateWebSocketUpgradeResponse` 判；这里补上只有客户端才判得了
         *          的一条：对端选定的名字必须在本端提议过的名单里（RFC 6455 §4.1 对客户端的要求）——
         *          不在名单里意味着两边对「这条连接说什么协议」并没有共识，留着比拒掉更糟
         * @param response 交换到的应答（状态码与头部）
         * @param clientKey 本端这次握手发出去的 key
         * @param subprotocols 本端提议过的子协议名单
         * @param offeredExtensions 本端提议过的扩展原文（空表示没提议）；传进校验是为了回显核对
         *        与扩展参数级判据（RFC 7692 §7.1）都按同一次要约对表
         * @return WebSocketUpgradeExpected<WebSocketUpgradeAgreement> 谈成的事实，或中文拒因
         */
        [[nodiscard]] WebSocketUpgradeExpected<WebSocketUpgradeAgreement> checkUpgradeAgreement(const HttpResponseInfo &response, const std::string_view clientKey,
                                                                                                const std::vector<std::string> &subprotocols,
                                                                                                const std::string_view          offeredExtensions)
        {
            auto agreement = validateWebSocketUpgradeResponse(response.statusCode, response.headers, clientKey, offeredExtensions);
            if (!agreement.has_value())
            {
                return std::unexpected(agreement.error());
            }
            if (!agreement->acceptedSubprotocol.empty() && std::find(subprotocols.begin(), subprotocols.end(), agreement->acceptedSubprotocol) == subprotocols.end())
            {
                return std::unexpected(WebSocketUpgradeError{
                        WebSocketUpgradeErrorKind::SubprotocolMismatch,
                        std::format("对端选定的子协议「{}」不在本端提议的名单里（RFC 6455 §4.1）：请核对对面的实现，或把这个名字加进 Configuration.subprotocols",
                                    agreement->acceptedSubprotocol)});
            }
            return *agreement;
        }
    } // namespace
    WebSocketClient::WebSocketClient(std::unique_ptr<HttpOutboundConnection> transport, WebSocketUpgradeAgreement agreement, const Configuration &configuration,
                                     std::string pendingBytes, Core::EventLoop &loop) :
        m_transport(std::move(transport)), m_agreement(std::move(agreement)), m_deflateWindow(m_agreement.deflateWindow), m_readBuffer(kReadChunkByteLength, '\0'),
        m_inboundBytes(std::move(pendingBytes)), m_pendingRead(std::make_unique<PendingRead>()), m_loop(&loop)
    {
        // 本端是客户端：对端（服务端）发来的帧必须不带掩码，带了就当场判错（RFC 6455 §5.1）
        m_decoder.setMaskingRequirement(WebSocketMaskingRequirement::PeerMustNotMask);
        m_decoder.setMaximumMessagePayloadLength(configuration.maximumMessageSize);
        // 协商成功才放行带 RSV1 的帧（RFC 7692 §6）：未协商时这类帧按协议错误收口，是解码器的默认行为
        m_decoder.setPerMessageDeflateEnabled(m_deflateWindow.has_value());
    }
    WebSocketClient::~WebSocketClient() = default;
    Core::Task<WebSocketUpgradeExpected<std::unique_ptr<WebSocketClient>>> WebSocketClient::connect(Core::EventLoop &loop, Configuration configuration)
    {
        // 用法错误在任何资源获取之前判完：解析与连接都是要归还的东西，带着坏参数进去就会留下一条
        // 半途的连接，或者一条永远没人接的协程帧
        if (configuration.hostName.empty())
        {
            co_return std::unexpected(
                    WebSocketUpgradeError{WebSocketUpgradeErrorKind::LocalRequestInvalid,
                                          "WebSocket 出站缺 Configuration.hostName：请给出目标主机名或 IP 字面量（它同时用作 Host、SNI 与证书主机名校验的对象）"});
        }
        if (configuration.handshakeTimeout <= std::chrono::milliseconds::zero())
        {
            co_return std::unexpected(WebSocketUpgradeError{
                    WebSocketUpgradeErrorKind::LocalRequestInvalid,
                    std::format("WebSocket 出站的 handshakeTimeout 是 {} 毫秒：建 TCP 连接、TLS 握手与等 101 这三段必须各有时限，0 或负数会让协程停在这三段里出不来。"
                                "要放宽就写一个更大的正数",
                                configuration.handshakeTimeout.count())});
        }
        const auto startedAt = std::chrono::steady_clock::now();
        // 三段共用一份总时限，各拿「此刻还剩下多少」：连接、TLS 握手、等 101 缺一段都会让总时限形同虚设
        const auto remainingBudget = [&]() -> std::optional<std::chrono::milliseconds>
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
            const auto left    = configuration.handshakeTimeout - elapsed;
            if (left <= std::chrono::milliseconds::zero())
            {
                return std::nullopt;
            }
            return left;
        };
        const std::uint16_t port  = resolvePort(configuration);
        const bool          isTls = configuration.clientTls != nullptr;
        const std::string   target{configuration.hostName + ":" + std::to_string(port)};
        const auto          resolved = co_await Core::AsyncResolver::resolve(loop, configuration.hostName, port);
        if (resolved.empty())
        {
            co_return std::unexpected(WebSocketUpgradeError{
                    WebSocketUpgradeErrorKind::AddressUnresolved,
                    std::format("WebSocket 出站没能把「{}」解析成可用地址：请核对主机名，或直接给 Configuration.hostName 填 IP 字面量", configuration.hostName)});
        }
        const auto connectBudget = remainingBudget();
        if (!connectBudget.has_value())
        {
            co_return std::unexpected(
                    WebSocketUpgradeError{WebSocketUpgradeErrorKind::DeadlineExpired,
                                          std::format("WebSocket 出站的 {} 毫秒握手时限已用尽：还没开始建立 TCP 连接（目标 {}）", configuration.handshakeTimeout.count(), target)});
        }
        auto candidate = co_await Core::connectCandidates(loop, Core::orderForConnectionRace(resolved), *connectBudget);
        if (!candidate.has_value())
        {
            co_return std::unexpected(
                    WebSocketUpgradeError{WebSocketUpgradeErrorKind::TransportFailed, std::format("建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时（目标 {}）", target)});
        }
        // 时限从「连上了」这一刻起算：握手那一段（写出请求 + 等到 101）拿剩余的预算
        // 这把 key 只用于本次握手：accept 由它算出，核对 101 全靠这条对应关系
        const std::string clientKey = Base::base64Encode(cryptographicRandomBytes(kClientKeyByteLength));
        // 扩展要约按开关给：默认提议 permessage-deflate（形态见 kPerMessageDeflateClientOffer），
        // 关掉时升级请求里连那条头部都不发
        const std::string extensionsOffer = configuration.negotiatePerMessageDeflate ? std::string{kPerMessageDeflateClientOffer} : std::string{};
        auto requestText = buildWebSocketUpgradeRequest(hostHeaderFor(configuration.hostName, port, isTls), configuration.requestTarget, clientKey, configuration.subprotocols,
                                                        extensionsOffer);
        if (!requestText.has_value())
        {
            // 用法错误（Host、请求目标或子协议名里带了控制字符之类）：拒因原样交给调用方，
            // 它点得名是哪一段取值不合用
            co_return std::unexpected(requestText.error());
        }
        // 升级交换走本端自己持有的套接字对象，做完才把通路交给 HttpOutboundConnection 承载帧
        std::optional<TcpStream>                                 plainStream;
        std::unique_ptr<Core::TlsSocket>                         tlsSocket;
        std::optional<WebSocketUpgradeExpected<UpgradeExchange>> exchanged;
        if (isTls)
        {
            auto secured = co_await makeClientTlsSocket(loop, *configuration.clientTls, std::move(candidate->socket), configuration.hostName, remainingBudget());
            if (!secured.has_value())
            {
                co_return std::unexpected(secured.error());
            }
            tlsSocket = std::move(*secured);
            exchanged = co_await exchangeOverTls(loop, *tlsSocket, remainingBudget(), *requestText, target);
        } else
        {
            plainStream.emplace(std::move(candidate->socket));
            exchanged = co_await exchangeOverPlain(loop, *plainStream, remainingBudget(), *requestText, target);
        }
        if (!exchanged->has_value())
        {
            co_return std::unexpected(exchanged->error());
        }
        HttpOutboundEndpointKey                 endpointKey{configuration.hostName, port, isTls};
        std::unique_ptr<HttpOutboundConnection> transport;
        if (isTls)
        {
            transport = HttpOutboundConnection::forSecure(endpointKey, std::move(tlsSocket));
        } else
        {
            // TcpStream 内部的读缓冲留着没消费的字节：换到通路之后它们仍在同一条流上，不会丢
            transport = HttpOutboundConnection::forPlain(endpointKey, std::move(*plainStream));
        }
        auto agreement = checkUpgradeAgreement((*exchanged)->response, clientKey, configuration.subprotocols, extensionsOffer);
        if (!agreement.has_value())
        {
            co_return std::unexpected(agreement.error());
        }
        co_return std::unique_ptr<WebSocketClient>(new WebSocketClient(std::move(transport), std::move(*agreement), configuration, std::move((*exchanged)->leftover), loop));
    }
    const WebSocketUpgradeAgreement &WebSocketClient::agreement() const noexcept
    {
        return m_agreement;
    }
    bool WebSocketClient::isOpen() const noexcept
    {
        return m_isOpen;
    }

    std::optional<std::uint16_t> WebSocketClient::remoteCloseCode() const noexcept
    {
        return m_remoteCloseCode;
    }

    std::string_view WebSocketClient::remoteCloseReason() const noexcept
    {
        return m_remoteCloseReason;
    }
    void WebSocketClient::compactInboundBytes()
    {
        // 解码器一次只消化到帧尾为止：帧之后的字节留在 m_inboundBytes 里，下一轮接着喂。
        // 契约与 H3 帧读取器一致：过半才搬移——一次读入多帧时逐帧 erase 开头是 O(n²)
        m_inboundByteOffset += m_decoder.consumedByteCount();
        if (m_inboundByteOffset == m_inboundBytes.size())
        {
            m_inboundBytes.clear();
            m_inboundByteOffset = 0;
        } else if (m_inboundByteOffset >= m_inboundBytes.size() / 2)
        {
            m_inboundBytes.erase(0, m_inboundByteOffset);
            m_inboundByteOffset = 0;
        }
    }
    Core::Task<WebSocketSessionExpected<WebSocketFrame>> WebSocketClient::receive()
    {
        co_return co_await receiveUntil(std::nullopt);
    }
    Core::Task<WebSocketSessionExpected<WebSocketFrame>> WebSocketClient::receiveFor(const std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::milliseconds::zero())
        {
            co_return co_await receiveUntil(std::chrono::steady_clock::now());
        }
        co_return co_await receiveUntil(std::chrono::steady_clock::now() + timeout);
    }
    Core::Task<std::optional<std::ptrdiff_t>> WebSocketClient::waitForRead(const std::optional<std::chrono::steady_clock::time_point> deadline, bool &interrupted)
    {
        interrupted = false;
        if (m_loop == nullptr)
        {
            throw Base::LogicException("WebSocketClient::receiveFor：构造时没有事件循环，无法挂接收超时");
        }
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline && !m_pendingRead->task.has_value())
        {
            // 非正 timeout：只检查已经挂着的读，不为一次「不等」新建读
            co_return std::nullopt;
        }
        if (!m_pendingRead->task.has_value())
        {
            m_pendingRead->task.emplace(m_transport->receive(m_readBuffer.data(), m_readBuffer.size()));
            // Task 是惰性的：这里手动首拍，之后由套接字就绪唤醒它
            m_pendingRead->task->handle().resume();
        }

        const std::stop_token interrupt = m_receiveInterrupt != nullptr ? m_receiveInterrupt->get_token() : std::stop_token{};
        Core::Timer           poll(*m_loop);
        for (;;)
        {
            if (interrupt.stop_requested())
            {
                interrupted = true;
                co_return std::nullopt;
            }
            if (m_pendingRead->task->isReady())
            {
                const ssize_t received = m_pendingRead->task->handle().promise().result();
                m_pendingRead->task.reset();
                co_return std::optional<std::ptrdiff_t>{static_cast<std::ptrdiff_t>(received)};
            }
            if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline)
            {
                // 不销毁 m_pendingRead：下一次 receive 继续等同一个读，已经到达的字节不会丢
                co_return std::nullopt;
            }

            std::chrono::milliseconds wait = kReceivePollInterval;
            if (deadline.has_value())
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
                wait                 = std::min(wait, std::max(remaining, std::chrono::milliseconds::zero()));
            }
            co_await poll.waitFor(wait);
        }
    }
    Core::Task<std::optional<WebSocketSessionExpected<WebSocketFrame>>> WebSocketClient::decodeBufferedFrame()
    {
        for (;;)
        {
            const WebSocketDecodeStatus status = m_decoder.parse(m_inboundBytes.data() + m_inboundByteOffset, m_inboundBytes.size() - m_inboundByteOffset);
            // 解码器一次只消化到帧尾为止：帧之后的字节留到下一轮，过半才搬移（同 H3 帧读取器）
            compactInboundBytes();
            if (status == WebSocketDecodeStatus::Error)
            {
                // 上限越出按 1009、其余按 1002 尽力告知对端（RFC 6455 §7.4.1）；通路已死时这一步作罢，
                // 原因已经在交回的字符串里
                // 关闭码与失败类别同一条判据：超限与帧违例在 RFC 6455 §7.4.1 上本就是两格，调用方要能不看文案就分得开
                const bool          isOverLimit = m_decoder.isLimitExceeded();
                const std::uint16_t closeCode   = isOverLimit ? kWebSocketMessageTooBigCode : kWebSocketProtocolErrorCode;
                const std::string   errorText   = m_decoder.errorMessage();
                co_return co_await failSession(closeCode, isOverLimit ? WebSocketSessionErrorKind::MessageLimitExceeded : WebSocketSessionErrorKind::ProtocolFramingRejected,
                                               errorText);
            }
            if (status != WebSocketDecodeStatus::Frame)
            {
                co_return std::nullopt;
            }

            WebSocketFrame frame = m_decoder.takeFrame();
            // 压缩消息先解压再交付（RFC 7692 §7.2.2）：解不开的连接按协议错误收口——
            // 字节流本端解不了，留在连接上只会越走越偏（与 WebSocketPeer 的收侧同一条口径）
            if (frame.isCompressed)
            {
                // 窗口位数按协商走（服务端声明它按多大压缩）；未协商时带 RSV1 的帧已被解码器判错，
                // 这里按满窗口兜底只是不让一个不可能的组合变成崩溃点
                const int                  decompressBits = m_deflateWindow.has_value() ? m_deflateWindow->decompressBits : kWebSocketDefaultWindowBits;
                const std::size_t          maximumPayload = m_decoder.maximumMessagePayloadLength();
                std::optional<std::string> inflated       = inflateWebSocketMessage(frame.payload, maximumPayload, decompressBits);
                if (!inflated.has_value())
                {
                    const std::string errorText = "对端发来的压缩消息解压失败，或解压结果超过上限（RFC 7692 §7.2.2）：请核对对端的压缩实现与协商参数";
                    co_return co_await failSession(kWebSocketProtocolErrorCode, WebSocketSessionErrorKind::CompressedMessageUnusable, errorText);
                }
                frame.payload = std::move(*inflated);
            }
            // 文本负载必须是合法 UTF-8（RFC 6455 §5.6 的编码要求，§8.1 要求校验不过即失败连接并回 1007）：
            // 校验点是「交给业务之前」的最后一步，解压过的文本同样要在还原之后判
            if (frame.opCode == WebSocketOpCode::Text)
            {
                const std::size_t invalidByteOffset = findInvalidWebSocketUtf8ByteOffset(frame.payload);
                if (invalidByteOffset != std::string_view::npos)
                {
                    // 原始非法字节不进文案：它一旦被写进日志，就是把对端字节注入日志的原料
                    const std::string errorText = std::format("对端发来的文本消息不是合法 UTF-8（RFC 6455 §5.6、§8.1）：第 {} 个字节起违规，"
                                                              "常见原因有过长编码、代理区码点（U+D800~U+DFFF）与截断的多字节序列；本端已按 1007 收口，负载不会交给业务",
                                                              invalidByteOffset);
                    co_return co_await failSession(kWebSocketInvalidPayloadDataCode, WebSocketSessionErrorKind::TextPayloadNotUtf8, errorText);
                }
            }
            if (frame.opCode == WebSocketOpCode::Ping)
            {
                // 心跳必须按原负载回 Pong（RFC 6455 §5.5.3）：不回就是让对端把这条连接判成死的
                if (!co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Pong, frame.payload)))
                {
                    m_isOpen = false;
                    co_return std::unexpected(WebSocketSessionError{WebSocketSessionErrorKind::SessionClosed, "回应 Pong 时通路已不可用：这次心跳没能答出去，连接按已断开处理"});
                }
                continue;
            }
            if (frame.opCode == WebSocketOpCode::Pong)
            {
                // Pong 在这一格消费掉：调用方要的是数据消息。发过 Ping 之后「有没有别的流量」由上层
                // 自己计时，本层不替它攒状态
                continue;
            }
            if (frame.opCode == WebSocketOpCode::Close)
            {
                co_return co_await answerPeerClose(frame);
            }
            co_return WebSocketSessionExpected<WebSocketFrame>{std::move(frame)};
        }
    }
    Core::Task<WebSocketSessionExpected<WebSocketFrame>> WebSocketClient::receiveUntil(const std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        // 收侧只看通路活着没有：本端发过 Close 之后仍然要能读回对端那条 Close（§5.5.1 的握手是双向的），
        // 「还能不能再发」由 m_isOpen 单独管
        if (!m_transport->isOpen())
        {
            co_return std::unexpected(WebSocketSessionError{WebSocketSessionErrorKind::SessionClosed, "通路已经关了：这条会话收不到后续消息"});
        }

        // 每次接收换一份中断源：本端上一次 close() 的停止信号不能带到这一次上
        m_receiveInterrupt = std::make_shared<std::stop_source>();

        for (;;)
        {
            const auto decoded = co_await decodeBufferedFrame();
            if (decoded.has_value())
            {
                co_return std::move(*decoded);
            }

            bool       interrupted = false;
            const auto received    = co_await waitForRead(deadline, interrupted);
            if (interrupted)
            {
                co_return std::unexpected(
                        WebSocketSessionError{WebSocketSessionErrorKind::ReceiveInterruptedByLocalClose, "本端已发起关闭：这次等待被唤醒；对端的回帧仍可用下一次 receive() 读取"});
            }
            if (!received.has_value())
            {
                co_return std::unexpected(WebSocketSessionError{WebSocketSessionErrorKind::ReceiveTimedOut, "等待消息超时：连接仍可用，可以稍后重试 receive() 或加大时限"});
            }
            if (*received == 0)
            {
                m_isOpen = false;
                co_return std::unexpected(WebSocketSessionError{WebSocketSessionErrorKind::PeerClosedWithoutCloseFrame,
                                                                "对端没有留下 Close 帧就收了线（RFC 6455 §7.1.1 的快速断开）：本端按已关闭处理"});
            }
            if (*received < 0)
            {
                m_isOpen = false;
                co_return std::unexpected(WebSocketSessionError{WebSocketSessionErrorKind::ConnectionLostWhileReading, "读取帧时通路出错：连接已断开、被对端复位，或本端把它关了"});
            }
            m_inboundBytes.append(m_readBuffer.data(), static_cast<std::size_t>(*received));
        }
    }
    Core::Task<WebSocketSessionExpected<WebSocketFrame>> WebSocketClient::failSession(const std::uint16_t closeCode, const WebSocketSessionErrorKind kind, std::string errorText)
    {
        // 先把「为什么收口」发出去，再把本端置成不可用：反过来写会让这条出声的帧被自家的闸门拦下，
        // 对端只看到一个没有原因的断开
        static_cast<void>(co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(closeCode, {}))));
        m_isOpen = false;
        m_transport->close();
        co_return std::unexpected(WebSocketSessionError{kind, std::move(errorText)});
    }
    Core::Task<WebSocketSessionExpected<WebSocketFrame>> WebSocketClient::answerPeerClose(const WebSocketFrame &closeFrame)
    {
        // 收侧判定与服务端共用一处（RFC 6455 §5.5.1 / §7.4.1）：非法码回 1002、原因非 UTF-8 回 1007、
        // 合规的原样答回；对端那个码原值记进 remoteCloseCode()，业务据此看得见「对端是违例关闭的」
        const WebSocketCloseVerdict verdict = classifyWebSocketClosePayload(closeFrame.payload);
        m_remoteCloseCode                   = verdict.peerCode;
        m_remoteCloseReason                 = verdict.peerReason;
        // 原因不回送：§5.5.1 只要求把状态码答回去，而对面那条原因是给对面自己看的
        co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(verdict.replyCode, {})));
        m_isOpen = false;
        m_transport->close();
        co_return closeFrame;
    }
    Core::Task<bool> WebSocketClient::sendText(const std::string_view text)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Text, text));
    }
    Core::Task<bool> WebSocketClient::sendBinary(const std::string_view binary)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Binary, binary));
    }
    Core::Task<bool> WebSocketClient::sendPing(const std::string_view payload)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Ping, payload));
    }
    Core::Task<bool> WebSocketClient::close(const std::uint16_t statusCode, const std::string_view reason)
    {
        // 三道用法判据与收侧同解，而且都在这一步跑完：自己不收的东西绝不发出去，否则一次正常收口会在
        // 对端变成协议错误，而业务想看的关闭原因也就丢了。抛出发生在调用点，不建协程帧也不留半条帧在线上
        if (!isAllowedWebSocketCloseCode(statusCode))
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：状态码 {} 不允许出现在线上（RFC 6455 §7.4.1/§7.4.2：1004/1005/1006/1015 为保留值，"
                                                             "1016-2999 段未经注册）：请改用 1000-1003、1007-1014 或 3000-4999 段的值",
                                                             statusCode));
        }
        if (reason.size() > kMaximumCloseReasonLength)
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：关闭原因 {} 字节超过上限 {} 字节（控制帧整体不得超过 {} 字节，其中状态码占 {} 字节，"
                                                             "RFC 6455 §5.5）：请缩短原因，或把长说明改用 sendText() 作为消息发出",
                                                             reason.size(), kMaximumCloseReasonLength, kWebSocketMaximumControlPayloadLength, kWebSocketCloseCodeByteLength));
        }
        if (const std::size_t invalidByteOffset = findInvalidWebSocketUtf8ByteOffset(reason); invalidByteOffset != std::string_view::npos)
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：关闭原因从第 {} 个字节起不是合法 UTF-8（RFC 6455 §7.4 要求状态码之后的正文按 UTF-8 "
                                                             "编码）：这样的帧发出去对端只能按 1007 收口，请把原因改成合法文本或留空",
                                                             invalidByteOffset));
        }
        return sendCloseBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(statusCode, reason)));
    }
    std::string WebSocketClient::prepareFrame(const WebSocketOpCode opCode, const std::string_view payload)
    {
        // 压缩只对数据消息生效（控制帧从不压缩，RFC 7692 §6.1）；压不动或压完更大就原样发——
        // 压缩是带宽优化，而发一条对端解不开的帧比不压严重得多（与 WebSocketPeer::sendFrame 同口径）
        const bool isDataMessage = opCode == WebSocketOpCode::Text || opCode == WebSocketOpCode::Binary;

        // 收成「普通 string + 标志位」而不是让 optional<string> 一直走到 string_view 转换：
        // GCC 13 在 -O3 下读不出 optional 的 engaged 守卫，会沿那条内联路径报 -Wmaybe-uninitialized
        // （CI 的 Release 档 -Werror 当场红；Debug 档不触发，所以本机一直看不见）
        std::string compressedPayload;
        bool        isCompressed = false;
        if (m_deflateWindow.has_value() && isDataMessage)
        {
            const std::optional<std::string> compressed = deflateWebSocketMessage(payload, m_deflateWindow->compressBits);
            if (compressed.has_value())
            {
                compressedPayload = *compressed;
                isCompressed      = compressedPayload.size() < payload.size();
            }
        }
        const std::string_view payloadToSend = isCompressed ? std::string_view{compressedPayload} : payload;

        // 本帧现取一把新键：编码里对控制帧长度与操作码的用法判据也在这一步抛出，调用点当场就能看见
        return encodeWebSocketFrame(opCode, payloadToSend, true, isCompressed, nextMaskKey());
    }
    Core::Task<bool> WebSocketClient::sendCloseBytes(std::string frameBytes)
    {
        // 通路已死或本端已经发过 Close：不再补第二条（§5.5.1 只要求一次关闭握手）
        if (!m_isOpen)
        {
            co_return false;
        }
        // 状态先落定再发帧：写出期间协程会挂起，此时业务若再调 send*() 必须已经看到「不能再发」。
        // 收侧不受这个标记影响——调用方还要靠 receive() 读回对端那条 Close 和它给的原因
        m_isOpen = false;
        // 只中断「正在等待」的那一次接收；连接不关，对端回帧留给下一次 receive()
        if (m_receiveInterrupt != nullptr)
        {
            m_receiveInterrupt->request_stop();
        }
        co_return co_await m_transport->send(frameBytes);
    }
    Core::Task<bool> WebSocketClient::writeFrameBytes(std::string frameBytes)
    {
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await m_transport->send(frameBytes);
    }
} // namespace AsynGyanis::Net
